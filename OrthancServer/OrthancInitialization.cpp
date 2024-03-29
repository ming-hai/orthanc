/**
 * Orthanc - A Lightweight, RESTful DICOM Store
 * Copyright (C) 2012-2016 Sebastien Jodogne, Medical Physics
 * Department, University Hospital of Liege, Belgium
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * In addition, as a special exception, the copyright holders of this
 * program give permission to link the code of its release with the
 * OpenSSL project's "OpenSSL" library (or with modified versions of it
 * that use the same license as the "OpenSSL" library), and distribute
 * the linked executables. You must obey the GNU General Public License
 * in all respects for all of the code used other than "OpenSSL". If you
 * modify file(s) with this exception, you may extend this exception to
 * your version of the file(s), but you are not obligated to do so. If
 * you do not wish to do so, delete this exception statement from your
 * version. If you delete this exception statement from all source files
 * in the program, then also delete it here.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 **/


#include "PrecompiledHeadersServer.h"
#include "OrthancInitialization.h"

#include "../Core/HttpClient.h"
#include "../Core/Logging.h"
#include "../Core/OrthancException.h"
#include "../Core/Toolbox.h"
#include "../Core/FileStorage/FilesystemStorage.h"

#include "ServerEnumerations.h"
#include "DatabaseWrapper.h"
#include "FromDcmtkBridge.h"

#include <boost/lexical_cast.hpp>
#include <boost/filesystem.hpp>
#include <curl/curl.h>
#include <boost/thread.hpp>


#if ORTHANC_SSL_ENABLED == 1
// For OpenSSL initialization and finalization
#include <openssl/conf.h>
#include <openssl/engine.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#endif


#if ORTHANC_JPEG_ENABLED == 1
#include <dcmtk/dcmjpeg/djdecode.h>
#endif


#if ORTHANC_JPEG_LOSSLESS_ENABLED == 1
#include <dcmtk/dcmjpls/djdecode.h>
#endif


namespace Orthanc
{
  static boost::recursive_mutex globalMutex_;
  static Json::Value configuration_;
  static boost::filesystem::path defaultDirectory_;
  static std::string configurationAbsolutePath_;
  static FontRegistry fontRegistry_;


  static std::string GetGlobalStringParameterInternal(const std::string& parameter,
                                                      const std::string& defaultValue)
  {
    if (configuration_.isMember(parameter))
    {
      if (configuration_[parameter].type() != Json::stringValue)
      {
        LOG(ERROR) << "The configuration option \"" << parameter << "\" must be a string";
        throw OrthancException(ErrorCode_BadParameterType);
      }
      else
      {
        return configuration_[parameter].asString();
      }
    }
    else
    {
      return defaultValue;
    }
  }


  static bool GetGlobalBoolParameterInternal(const std::string& parameter,
                                             bool defaultValue)
  {
    if (configuration_.isMember(parameter))
    {
      if (configuration_[parameter].type() != Json::booleanValue)
      {
        LOG(ERROR) << "The configuration option \"" << parameter << "\" must be a Boolean (true or false)";
        throw OrthancException(ErrorCode_BadParameterType);
      }
      else
      {
        return configuration_[parameter].asBool();
      }
    }
    else
    {
      return defaultValue;
    }
  }



  static void AddFileToConfiguration(const boost::filesystem::path& path)
  {
    LOG(WARNING) << "Reading the configuration from: " << path;

    Json::Value config;

    {
      std::string content;
      Toolbox::ReadFile(content, path.string());

      Json::Value tmp;
      Json::Reader reader;
      if (!reader.parse(content, tmp) ||
          tmp.type() != Json::objectValue)
      {
        LOG(ERROR) << "The configuration file does not follow the JSON syntax: " << path;
        throw OrthancException(ErrorCode_BadJson);
      }

      Toolbox::CopyJsonWithoutComments(config, tmp);
    }

    if (configuration_.size() == 0)
    {
      configuration_ = config;
    }
    else
    {
      Json::Value::Members members = config.getMemberNames();
      for (Json::Value::ArrayIndex i = 0; i < members.size(); i++)
      {
        if (configuration_.isMember(members[i]))
        {
          LOG(ERROR) << "The configuration section \"" << members[i] << "\" is defined in 2 different configuration files";
          throw OrthancException(ErrorCode_BadFileFormat);          
        }
        else
        {
          configuration_[members[i]] = config[members[i]];
        }
      }
    }
  }


  static void ScanFolderForConfiguration(const char* folder)
  {
    using namespace boost::filesystem;

    LOG(WARNING) << "Scanning folder \"" << folder << "\" for configuration files";

    directory_iterator end_it; // default construction yields past-the-end
    for (directory_iterator it(folder);
         it != end_it;
         ++it)
    {
      if (!is_directory(it->status()))
      {
        std::string extension = boost::filesystem::extension(it->path());
        Toolbox::ToLowerCase(extension);

        if (extension == ".json")
        {
          AddFileToConfiguration(it->path().string());
        }
      }
    }
  }


  static void ReadGlobalConfiguration(const char* configurationFile)
  {
    // Prepare the default configuration
    defaultDirectory_ = boost::filesystem::current_path();
    configuration_ = Json::objectValue;
    configurationAbsolutePath_ = "";

    if (configurationFile)
    {
      if (!boost::filesystem::exists(configurationFile))
      {
        LOG(ERROR) << "Inexistent path to configuration: " << configurationFile;
        throw OrthancException(ErrorCode_InexistentFile);
      }
      
      if (boost::filesystem::is_directory(configurationFile))
      {
        defaultDirectory_ = boost::filesystem::path(configurationFile);
        configurationAbsolutePath_ = boost::filesystem::absolute(configurationFile).parent_path().string();
        ScanFolderForConfiguration(configurationFile);
      }
      else
      {
        defaultDirectory_ = boost::filesystem::path(configurationFile).parent_path();
        configurationAbsolutePath_ = boost::filesystem::absolute(configurationFile).string();
        AddFileToConfiguration(configurationFile);
      }
    }
    else
    {
#if ORTHANC_STANDALONE == 1
      // No default path for the standalone configuration
      LOG(WARNING) << "Using the default Orthanc configuration";
      return;

#else
      // In a non-standalone build, we use the
      // "Resources/Configuration.json" from the Orthanc source code

      boost::filesystem::path p = ORTHANC_PATH;
      p /= "Resources";
      p /= "Configuration.json";
      configurationAbsolutePath_ = boost::filesystem::absolute(p).string();

      AddFileToConfiguration(p);      
#endif
    }
  }


  static void ValidateGlobalConfiguration()
  {
    std::set<std::string> ids;

    Configuration::GetListOfOrthancPeers(ids);
    for (std::set<std::string>::const_iterator it = ids.begin(); it != ids.end(); ++it)
    {
      OrthancPeerParameters peer;
      Configuration::GetOrthancPeer(peer, *it);
    }

    Configuration::GetListOfDicomModalities(ids);
    for (std::set<std::string>::const_iterator it = ids.begin(); it != ids.end(); ++it)
    {
      RemoteModalityParameters modality;
      Configuration::GetDicomModalityUsingSymbolicName(modality, *it);
    }
  }


  static void RegisterUserMetadata()
  {
    if (configuration_.isMember("UserMetadata"))
    {
      const Json::Value& parameter = configuration_["UserMetadata"];

      Json::Value::Members members = parameter.getMemberNames();
      for (size_t i = 0; i < members.size(); i++)
      {
        const std::string& name = members[i];

        if (!parameter[name].isInt())
        {
          LOG(ERROR) << "Not a number in this user-defined metadata: " << name;
          throw OrthancException(ErrorCode_BadParameterType);
        }

        int metadata = parameter[name].asInt();        

        LOG(INFO) << "Registering user-defined metadata: " << name << " (index " 
                  << metadata << ")";

        try
        {
          RegisterUserMetadata(metadata, name);
        }
        catch (OrthancException&)
        {
          LOG(ERROR) << "Cannot register this user-defined metadata: " << name;
          throw;
        }
      }
    }
  }


  static void RegisterUserContentType()
  {
    if (configuration_.isMember("UserContentType"))
    {
      const Json::Value& parameter = configuration_["UserContentType"];

      Json::Value::Members members = parameter.getMemberNames();
      for (size_t i = 0; i < members.size(); i++)
      {
        const std::string& name = members[i];
        std::string mime = "application/octet-stream";

        const Json::Value& value = parameter[name];
        int contentType;

        if (value.isArray() &&
            value.size() == 2 &&
            value[0].isInt() &&
            value[1].isString())
        {
          contentType = value[0].asInt();
          mime = value[1].asString();
        }
        else if (value.isInt())
        {
          contentType = value.asInt();
        }
        else
        {
          LOG(ERROR) << "Not a number in this user-defined attachment type: " << name;
          throw OrthancException(ErrorCode_BadParameterType);
        }

        LOG(INFO) << "Registering user-defined attachment type: " << name << " (index " 
                  << contentType << ") with MIME type \"" << mime << "\"";

        try
        {
          RegisterUserContentType(contentType, name, mime);
        }
        catch (OrthancException&)
        {
          throw;
        }
      }
    }
  }


  static void LoadCustomDictionary(const Json::Value& configuration)
  {
    if (configuration.type() != Json::objectValue ||
        !configuration.isMember("Dictionary") ||
        configuration["Dictionary"].type() != Json::objectValue)
    {
      return;
    }

    Json::Value::Members tags(configuration["Dictionary"].getMemberNames());

    for (Json::Value::ArrayIndex i = 0; i < tags.size(); i++)
    {
      const Json::Value& content = configuration["Dictionary"][tags[i]];
      if (content.type() != Json::arrayValue ||
          content.size() < 2 ||
          content.size() > 4 ||
          content[0].type() != Json::stringValue ||
          content[1].type() != Json::stringValue ||
          (content.size() >= 3 && content[2].type() != Json::intValue) ||
          (content.size() >= 4 && content[3].type() != Json::intValue))
      {
        throw OrthancException(ErrorCode_BadFileFormat);
      }

      DicomTag tag(FromDcmtkBridge::ParseTag(tags[i]));
      DcmEVR vr = FromDcmtkBridge::ParseValueRepresentation(content[0].asString());
      std::string name = content[1].asString();
      unsigned int minMultiplicity = (content.size() >= 2) ? content[2].asUInt() : 1;
      unsigned int maxMultiplicity = (content.size() >= 3) ? content[3].asUInt() : 1;

      FromDcmtkBridge::RegisterDictionaryTag(tag, vr, name, minMultiplicity, maxMultiplicity);
    }
  }



  void OrthancInitialize(const char* configurationFile)
  {
    boost::recursive_mutex::scoped_lock lock(globalMutex_);

#if ORTHANC_SSL_ENABLED == 1
    // https://wiki.openssl.org/index.php/Library_Initialization
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
    ERR_load_crypto_strings();

    curl_global_init(CURL_GLOBAL_ALL);
#else
    curl_global_init(CURL_GLOBAL_ALL & ~CURL_GLOBAL_SSL);
#endif

    InitializeServerEnumerations();

    // Read the user-provided configuration
    ReadGlobalConfiguration(configurationFile);
    ValidateGlobalConfiguration();

    HttpClient::GlobalInitialize(GetGlobalBoolParameterInternal("HttpsVerifyPeers", true),
                                 GetGlobalStringParameterInternal("HttpsCACertificates", ""));

    RegisterUserMetadata();
    RegisterUserContentType();

    FromDcmtkBridge::InitializeDictionary();
    LoadCustomDictionary(configuration_);

#if ORTHANC_JPEG_LOSSLESS_ENABLED == 1
    LOG(WARNING) << "Registering JPEG Lossless codecs";
    DJLSDecoderRegistration::registerCodecs();    
#endif

#if ORTHANC_JPEG_ENABLED == 1
    LOG(WARNING) << "Registering JPEG codecs";
    DJDecoderRegistration::registerCodecs(); 
#endif

    fontRegistry_.AddFromResource(EmbeddedResources::FONT_UBUNTU_MONO_BOLD_16);
  }



  void OrthancFinalize()
  {
    boost::recursive_mutex::scoped_lock lock(globalMutex_);
    HttpClient::GlobalFinalize();

#if ORTHANC_JPEG_LOSSLESS_ENABLED == 1
    // Unregister JPEG-LS codecs
    DJLSDecoderRegistration::cleanup();
#endif

#if ORTHANC_JPEG_ENABLED == 1
    // Unregister JPEG codecs
    DJDecoderRegistration::cleanup();
#endif

    curl_global_cleanup();

#if ORTHANC_SSL_ENABLED == 1
    // Finalize OpenSSL
    // https://wiki.openssl.org/index.php/Library_Initialization#Cleanup
    FIPS_mode_set(0);
    ENGINE_cleanup();
    CONF_modules_unload(1);
    EVP_cleanup();
    CRYPTO_cleanup_all_ex_data();
    ERR_remove_state(0);
    ERR_free_strings();
#endif
  }


  std::string Configuration::GetGlobalStringParameter(const std::string& parameter,
                                                      const std::string& defaultValue)
  {
    boost::recursive_mutex::scoped_lock lock(globalMutex_);
    return GetGlobalStringParameterInternal(parameter, defaultValue);
  }


  int Configuration::GetGlobalIntegerParameter(const std::string& parameter,
                                               int defaultValue)
  {
    boost::recursive_mutex::scoped_lock lock(globalMutex_);

    if (configuration_.isMember(parameter))
    {
      if (configuration_[parameter].type() != Json::intValue)
      {
        LOG(ERROR) << "The configuration option \"" << parameter << "\" must be an integer";
        throw OrthancException(ErrorCode_BadParameterType);
      }
      else
      {
        return configuration_[parameter].asInt();
      }
    }
    else
    {
      return defaultValue;
    }
  }


  bool Configuration::GetGlobalBoolParameter(const std::string& parameter,
                                             bool defaultValue)
  {
    boost::recursive_mutex::scoped_lock lock(globalMutex_);
    return GetGlobalBoolParameterInternal(parameter, defaultValue);
  }


  void Configuration::GetDicomModalityUsingSymbolicName(RemoteModalityParameters& modality,
                                                        const std::string& name)
  {
    boost::recursive_mutex::scoped_lock lock(globalMutex_);

    if (!configuration_.isMember("DicomModalities"))
    {
      LOG(ERROR) << "No modality with symbolic name: " << name;
      throw OrthancException(ErrorCode_InexistentItem);
    }

    const Json::Value& modalities = configuration_["DicomModalities"];
    if (modalities.type() != Json::objectValue ||
        !modalities.isMember(name))
    {
      LOG(ERROR) << "No modality with symbolic name: " << name;
      throw OrthancException(ErrorCode_InexistentItem);
    }

    try
    {
      modality.FromJson(modalities[name]);
    }
    catch (OrthancException&)
    {
      LOG(ERROR) << "Syntax error in the definition of DICOM modality \"" << name 
                 << "\". Please check your configuration file.";
      throw;
    }
  }



  void Configuration::GetOrthancPeer(OrthancPeerParameters& peer,
                                     const std::string& name)
  {
    boost::recursive_mutex::scoped_lock lock(globalMutex_);

    if (!configuration_.isMember("OrthancPeers"))
    {
      LOG(ERROR) << "No peer with symbolic name: " << name;
      throw OrthancException(ErrorCode_InexistentItem);
    }

    try
    {
      const Json::Value& modalities = configuration_["OrthancPeers"];
      if (modalities.type() != Json::objectValue ||
          !modalities.isMember(name))
      {
        LOG(ERROR) << "No peer with symbolic name: " << name;
        throw OrthancException(ErrorCode_InexistentItem);
      }

      peer.FromJson(modalities[name]);
    }
    catch (OrthancException&)
    {
      LOG(ERROR) << "Syntax error in the definition of peer \"" << name 
                 << "\". Please check your configuration file.";
      throw;
    }
  }


  static bool ReadKeys(std::set<std::string>& target,
                       const char* parameter,
                       bool onlyAlphanumeric)
  {
    boost::recursive_mutex::scoped_lock lock(globalMutex_);

    target.clear();
  
    if (!configuration_.isMember(parameter))
    {
      return true;
    }

    const Json::Value& modalities = configuration_[parameter];
    if (modalities.type() != Json::objectValue)
    {
      LOG(ERROR) << "Bad format of the \"DicomModalities\" configuration section";
      throw OrthancException(ErrorCode_BadFileFormat);
    }

    Json::Value::Members members = modalities.getMemberNames();
    for (size_t i = 0; i < members.size(); i++)
    {
      if (onlyAlphanumeric)
      {
        for (size_t j = 0; j < members[i].size(); j++)
        {
          if (!isalnum(members[i][j]) && members[i][j] != '-')
          {
            return false;
          }
        }
      }

      target.insert(members[i]);
    }

    return true;
  }


  void Configuration::GetListOfDicomModalities(std::set<std::string>& target)
  {
    target.clear();

    if (!ReadKeys(target, "DicomModalities", true))
    {
      LOG(ERROR) << "Only alphanumeric and dash characters are allowed in the names of the modalities";
      throw OrthancException(ErrorCode_BadFileFormat);
    }
  }


  void Configuration::GetListOfOrthancPeers(std::set<std::string>& target)
  {
    target.clear();

    if (!ReadKeys(target, "OrthancPeers", true))
    {
      LOG(ERROR) << "Only alphanumeric and dash characters are allowed in the names of Orthanc peers";
      throw OrthancException(ErrorCode_BadFileFormat);
    }
  }



  void Configuration::SetupRegisteredUsers(MongooseServer& httpServer)
  {
    boost::recursive_mutex::scoped_lock lock(globalMutex_);

    httpServer.ClearUsers();

    if (!configuration_.isMember("RegisteredUsers"))
    {
      return;
    }

    const Json::Value& users = configuration_["RegisteredUsers"];
    if (users.type() != Json::objectValue)
    {
      LOG(ERROR) << "Badly formatted list of users";
      throw OrthancException(ErrorCode_BadFileFormat);
    }

    Json::Value::Members usernames = users.getMemberNames();
    for (size_t i = 0; i < usernames.size(); i++)
    {
      const std::string& username = usernames[i];
      std::string password = users[username].asString();
      httpServer.RegisterUser(username.c_str(), password.c_str());
    }
  }


  std::string Configuration::InterpretRelativePath(const std::string& baseDirectory,
                                                   const std::string& relativePath)
  {
    boost::filesystem::path base(baseDirectory);
    boost::filesystem::path relative(relativePath);

    /**
       The following lines should be equivalent to this one: 

       return (base / relative).string();

       However, for some unknown reason, some versions of Boost do not
       make the proper path resolution when "baseDirectory" is an
       absolute path. So, a hack is used below.
    **/

    if (relative.is_absolute())
    {
      return relative.string();
    }
    else
    {
      return (base / relative).string();
    }
  }

  std::string Configuration::InterpretStringParameterAsPath(const std::string& parameter)
  {
    boost::recursive_mutex::scoped_lock lock(globalMutex_);
    return InterpretRelativePath(defaultDirectory_.string(), parameter);
  }


  void Configuration::GetGlobalListOfStringsParameter(std::list<std::string>& target,
                                                      const std::string& key)
  {
    boost::recursive_mutex::scoped_lock lock(globalMutex_);

    target.clear();
  
    if (!configuration_.isMember(key))
    {
      return;
    }

    const Json::Value& lst = configuration_[key];

    if (lst.type() != Json::arrayValue)
    {
      LOG(ERROR) << "Badly formatted list of strings";
      throw OrthancException(ErrorCode_BadFileFormat);
    }

    for (Json::Value::ArrayIndex i = 0; i < lst.size(); i++)
    {
      target.push_back(lst[i].asString());
    }    
  }


  bool Configuration::IsSameAETitle(const std::string& aet1,
                                    const std::string& aet2)
  {
    if (GetGlobalBoolParameter("StrictAetComparison", false))
    {
      // Case-sensitive matching
      return aet1 == aet2;
    }
    else
    {
      // Case-insensitive matching (default)
      std::string tmp1, tmp2;
      Toolbox::ToLowerCase(tmp1, aet1);
      Toolbox::ToLowerCase(tmp2, aet2);
      return tmp1 == tmp2;
    }
  }


  bool Configuration::LookupDicomModalityUsingAETitle(RemoteModalityParameters& modality,
                                                      const std::string& aet)
  {
    std::set<std::string> modalities;
    GetListOfDicomModalities(modalities);

    for (std::set<std::string>::const_iterator 
           it = modalities.begin(); it != modalities.end(); ++it)
    {
      try
      {
        GetDicomModalityUsingSymbolicName(modality, *it);

        if (IsSameAETitle(aet, modality.GetApplicationEntityTitle()))
        {
          return true;
        }
      }
      catch (OrthancException&)
      {
      }
    }

    return false;
  }


  bool Configuration::IsKnownAETitle(const std::string& aet)
  {
    RemoteModalityParameters modality;
    return LookupDicomModalityUsingAETitle(modality, aet);
  }


  RemoteModalityParameters Configuration::GetModalityUsingSymbolicName(const std::string& name)
  {
    RemoteModalityParameters modality;
    GetDicomModalityUsingSymbolicName(modality, name);

    return modality;
  }


  RemoteModalityParameters Configuration::GetModalityUsingAet(const std::string& aet)
  {
    RemoteModalityParameters modality;

    if (LookupDicomModalityUsingAETitle(modality, aet))
    {
      return modality;
    }
    else
    {
      LOG(ERROR) << "Unknown modality for AET: " << aet;
      throw OrthancException(ErrorCode_InexistentItem);
    }
  }


  void Configuration::UpdateModality(const std::string& symbolicName,
                                     const RemoteModalityParameters& modality)
  {
    boost::recursive_mutex::scoped_lock lock(globalMutex_);

    if (!configuration_.isMember("DicomModalities"))
    {
      configuration_["DicomModalities"] = Json::objectValue;
    }

    Json::Value& modalities = configuration_["DicomModalities"];
    if (modalities.type() != Json::objectValue)
    {
      LOG(ERROR) << "Bad file format for modality: " << symbolicName;
      throw OrthancException(ErrorCode_BadFileFormat);
    }

    modalities.removeMember(symbolicName);

    Json::Value v;
    modality.ToJson(v);
    modalities[symbolicName] = v;
  }
  

  void Configuration::RemoveModality(const std::string& symbolicName)
  {
    boost::recursive_mutex::scoped_lock lock(globalMutex_);

    if (!configuration_.isMember("DicomModalities"))
    {
      LOG(ERROR) << "No modality with symbolic name: " << symbolicName;
      throw OrthancException(ErrorCode_BadFileFormat);
    }

    Json::Value& modalities = configuration_["DicomModalities"];
    if (modalities.type() != Json::objectValue)
    {
      LOG(ERROR) << "Bad file format for the \"DicomModalities\" configuration section";
      throw OrthancException(ErrorCode_BadFileFormat);
    }

    modalities.removeMember(symbolicName.c_str());
  }


  void Configuration::UpdatePeer(const std::string& symbolicName,
                                 const OrthancPeerParameters& peer)
  {
    boost::recursive_mutex::scoped_lock lock(globalMutex_);

    if (!configuration_.isMember("OrthancPeers"))
    {
      LOG(ERROR) << "No peer with symbolic name: " << symbolicName;
      configuration_["OrthancPeers"] = Json::objectValue;
    }

    Json::Value& peers = configuration_["OrthancPeers"];
    if (peers.type() != Json::objectValue)
    {
      LOG(ERROR) << "Bad file format for the \"OrthancPeers\" configuration section";
      throw OrthancException(ErrorCode_BadFileFormat);
    }

    peers.removeMember(symbolicName);

    Json::Value v;
    peer.ToJson(v);
    peers[symbolicName] = v;
  }
  

  void Configuration::RemovePeer(const std::string& symbolicName)
  {
    boost::recursive_mutex::scoped_lock lock(globalMutex_);

    if (!configuration_.isMember("OrthancPeers"))
    {
      LOG(ERROR) << "No peer with symbolic name: " << symbolicName;
      throw OrthancException(ErrorCode_BadFileFormat);
    }

    Json::Value& peers = configuration_["OrthancPeers"];
    if (peers.type() != Json::objectValue)
    {
      LOG(ERROR) << "Bad file format for the \"OrthancPeers\" configuration section";
      throw OrthancException(ErrorCode_BadFileFormat);
    }

    peers.removeMember(symbolicName.c_str());
  }


  
  const std::string& Configuration::GetConfigurationAbsolutePath()
  {
    return configurationAbsolutePath_;
  }


  static IDatabaseWrapper* CreateSQLiteWrapper()
  {
    std::string storageDirectoryStr = Configuration::GetGlobalStringParameter("StorageDirectory", "OrthancStorage");

    // Open the database
    boost::filesystem::path indexDirectory = Configuration::InterpretStringParameterAsPath(
      Configuration::GetGlobalStringParameter("IndexDirectory", storageDirectoryStr));

    LOG(WARNING) << "SQLite index directory: " << indexDirectory;

    try
    {
      boost::filesystem::create_directories(indexDirectory);
    }
    catch (boost::filesystem::filesystem_error)
    {
    }

    return new DatabaseWrapper(indexDirectory.string() + "/index");
  }


  namespace
  {
    // Anonymous namespace to avoid clashes between compilation modules

    class FilesystemStorageWithoutDicom : public IStorageArea
    {
    private:
      FilesystemStorage storage_;

    public:
      FilesystemStorageWithoutDicom(const std::string& path) : storage_(path)
      {
      }

      virtual void Create(const std::string& uuid,
                          const void* content, 
                          size_t size,
                          FileContentType type)
      {
        if (type != FileContentType_Dicom)
        {
          storage_.Create(uuid, content, size, type);
        }
      }

      virtual void Read(std::string& content,
                        const std::string& uuid,
                        FileContentType type)
      {
        if (type != FileContentType_Dicom)
        {
          storage_.Read(content, uuid, type);
        }
        else
        {
          throw OrthancException(ErrorCode_UnknownResource);
        }
      }

      virtual void Remove(const std::string& uuid,
                          FileContentType type) 
      {
        if (type != FileContentType_Dicom)
        {
          storage_.Remove(uuid, type);
        }
      }
    };
  }


  static IStorageArea* CreateFilesystemStorage()
  {
    std::string storageDirectoryStr = Configuration::GetGlobalStringParameter("StorageDirectory", "OrthancStorage");

    boost::filesystem::path storageDirectory = Configuration::InterpretStringParameterAsPath(storageDirectoryStr);
    LOG(WARNING) << "Storage directory: " << storageDirectory;

    if (Configuration::GetGlobalBoolParameter("StoreDicom", true))
    {
      return new FilesystemStorage(storageDirectory.string());
    }
    else
    {
      LOG(WARNING) << "The DICOM files will not be stored, Orthanc running in index-only mode";
      return new FilesystemStorageWithoutDicom(storageDirectory.string());
    }
  }


  IDatabaseWrapper* Configuration::CreateDatabaseWrapper()
  {
    return CreateSQLiteWrapper();
  }


  IStorageArea* Configuration::CreateStorageArea()
  {
    return CreateFilesystemStorage();
  }  


  void Configuration::GetConfiguration(Json::Value& result)
  {
    boost::recursive_mutex::scoped_lock lock(globalMutex_);
    result = configuration_;
  }


  void Configuration::FormatConfiguration(std::string& result)
  {
    Json::Value config;
    GetConfiguration(config);

    Json::StyledWriter w;
    result = w.write(config);
  }


  const FontRegistry& Configuration::GetFontRegistry()
  {
    return fontRegistry_;
  }
}
