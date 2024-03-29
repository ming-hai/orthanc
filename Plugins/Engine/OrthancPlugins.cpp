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


#include "../../OrthancServer/PrecompiledHeadersServer.h"
#include "OrthancPlugins.h"

#if ORTHANC_PLUGINS_ENABLED != 1
#error The plugin support is disabled
#endif


#include "../../Core/ChunkedBuffer.h"
#include "../../Core/HttpServer/HttpToolbox.h"
#include "../../Core/Logging.h"
#include "../../Core/OrthancException.h"
#include "../../Core/Toolbox.h"
#include "../../OrthancServer/FromDcmtkBridge.h"
#include "../../OrthancServer/ToDcmtkBridge.h"
#include "../../OrthancServer/OrthancInitialization.h"
#include "../../OrthancServer/ServerContext.h"
#include "../../OrthancServer/ServerToolbox.h"
#include "../../OrthancServer/Search/HierarchicalMatcher.h"
#include "../../OrthancServer/Internals/DicomImageDecoder.h"
#include "../../Core/Compression/ZlibCompressor.h"
#include "../../Core/Compression/GzipCompressor.h"
#include "../../Core/Images/Image.h"
#include "../../Core/Images/PngReader.h"
#include "../../Core/Images/PngWriter.h"
#include "../../Core/Images/JpegReader.h"
#include "../../Core/Images/JpegWriter.h"
#include "../../Core/Images/ImageProcessing.h"
#include "../../OrthancServer/DefaultDicomImageDecoder.h"
#include "PluginsEnumerations.h"

#include <boost/regex.hpp> 
#include <dcmtk/dcmdata/dcdict.h>
#include <dcmtk/dcmdata/dcdicent.h>

namespace Orthanc
{
  static void CopyToMemoryBuffer(OrthancPluginMemoryBuffer& target,
                                 const void* data,
                                 size_t size)
  {
    target.size = size;

    if (size == 0)
    {
      target.data = NULL;
    }
    else
    {
      target.data = malloc(size);
      if (target.data != NULL)
      {
        memcpy(target.data, data, size);
      }
      else
      {
        throw OrthancException(ErrorCode_NotEnoughMemory);
      }
    }
  }


  static void CopyToMemoryBuffer(OrthancPluginMemoryBuffer& target,
                                 const std::string& str)
  {
    if (str.size() == 0)
    {
      target.size = 0;
      target.data = NULL;
    }
    else
    {
      CopyToMemoryBuffer(target, str.c_str(), str.size());
    }
  }


  namespace
  {
    class PluginStorageArea : public IStorageArea
    {
    private:
      _OrthancPluginRegisterStorageArea callbacks_;
      PluginsErrorDictionary&  errorDictionary_;

      void Free(void* buffer) const
      {
        if (buffer != NULL)
        {
          callbacks_.free(buffer);
        }
      }

    public:
      PluginStorageArea(const _OrthancPluginRegisterStorageArea& callbacks,
                        PluginsErrorDictionary&  errorDictionary) : 
        callbacks_(callbacks),
        errorDictionary_(errorDictionary)
      {
      }


      virtual void Create(const std::string& uuid,
                          const void* content, 
                          size_t size,
                          FileContentType type)
      {
        OrthancPluginErrorCode error = callbacks_.create
          (uuid.c_str(), content, size, Plugins::Convert(type));

        if (error != OrthancPluginErrorCode_Success)
        {
          errorDictionary_.LogError(error, true);
          throw OrthancException(static_cast<ErrorCode>(error));
        }
      }


      virtual void Read(std::string& content,
                        const std::string& uuid,
                        FileContentType type)
      {
        void* buffer = NULL;
        int64_t size = 0;

        OrthancPluginErrorCode error = callbacks_.read
          (&buffer, &size, uuid.c_str(), Plugins::Convert(type));

        if (error != OrthancPluginErrorCode_Success)
        {
          errorDictionary_.LogError(error, true);
          throw OrthancException(static_cast<ErrorCode>(error));
        }

        try
        {
          content.resize(static_cast<size_t>(size));
        }
        catch (...)
        {
          Free(buffer);
          throw OrthancException(ErrorCode_NotEnoughMemory);
        }

        if (size > 0)
        {
          memcpy(&content[0], buffer, static_cast<size_t>(size));
        }

        Free(buffer);
      }


      virtual void Remove(const std::string& uuid,
                          FileContentType type) 
      {
        OrthancPluginErrorCode error = callbacks_.remove
          (uuid.c_str(), Plugins::Convert(type));

        if (error != OrthancPluginErrorCode_Success)
        {
          errorDictionary_.LogError(error, true);
          throw OrthancException(static_cast<ErrorCode>(error));
        }
      }
    };


    class StorageAreaFactory : public boost::noncopyable
    {
    private:
      SharedLibrary&   sharedLibrary_;
      _OrthancPluginRegisterStorageArea  callbacks_;
      PluginsErrorDictionary&  errorDictionary_;

    public:
      StorageAreaFactory(SharedLibrary& sharedLibrary,
                         const _OrthancPluginRegisterStorageArea& callbacks,
                         PluginsErrorDictionary&  errorDictionary) :
        sharedLibrary_(sharedLibrary),
        callbacks_(callbacks),
        errorDictionary_(errorDictionary)
      {
      }

      SharedLibrary&  GetSharedLibrary()
      {
        return sharedLibrary_;
      }

      IStorageArea* Create() const
      {
        return new PluginStorageArea(callbacks_, errorDictionary_);
      }
    };
  }


  struct OrthancPlugins::PImpl
  {
    class RestCallback : public boost::noncopyable
    {
    private:
      boost::regex              regex_;
      OrthancPluginRestCallback callback_;
      bool                      lock_;

      OrthancPluginErrorCode InvokeInternal(HttpOutput& output,
                                            const std::string& flatUri,
                                            const OrthancPluginHttpRequest& request)
      {
        return callback_(reinterpret_cast<OrthancPluginRestOutput*>(&output), 
                         flatUri.c_str(), 
                         &request);
      }

    public:
      RestCallback(const char* regex,
                   OrthancPluginRestCallback callback,
                   bool lockRestCallbacks) :
        regex_(regex),
        callback_(callback),
        lock_(lockRestCallbacks)
      {
      }

      const boost::regex& GetRegularExpression() const
      {
        return regex_;
      }

      OrthancPluginErrorCode Invoke(boost::recursive_mutex& restCallbackMutex,
                                    HttpOutput& output,
                                    const std::string& flatUri,
                                    const OrthancPluginHttpRequest& request)
      {
        if (lock_)
        {
          boost::recursive_mutex::scoped_lock lock(restCallbackMutex);
          return InvokeInternal(output, flatUri, request);
        }
        else
        {
          return InvokeInternal(output, flatUri, request);
        }
      }
    };


    typedef std::pair<std::string, _OrthancPluginProperty>  Property;
    typedef std::list<RestCallback*>  RestCallbacks;
    typedef std::list<OrthancPluginOnStoredInstanceCallback>  OnStoredCallbacks;
    typedef std::list<OrthancPluginOnChangeCallback>  OnChangeCallbacks;
    typedef std::map<Property, std::string>  Properties;

    PluginsManager manager_;
    ServerContext* context_;
    RestCallbacks restCallbacks_;
    OnStoredCallbacks  onStoredCallbacks_;
    OnChangeCallbacks  onChangeCallbacks_;
    OrthancPluginWorklistCallback  worklistCallback_;
    OrthancPluginDecodeImageCallback  decodeImageCallback_;
    std::auto_ptr<StorageAreaFactory>  storageArea_;
    boost::recursive_mutex restCallbackMutex_;
    boost::recursive_mutex storedCallbackMutex_;
    boost::recursive_mutex changeCallbackMutex_;
    boost::mutex worklistCallbackMutex_;
    boost::mutex decodeImageCallbackMutex_;
    boost::recursive_mutex invokeServiceMutex_;
    Properties properties_;
    int argc_;
    char** argv_;
    std::auto_ptr<OrthancPluginDatabase>  database_;
    PluginsErrorDictionary  dictionary_;

    PImpl() : 
      context_(NULL), 
      worklistCallback_(NULL),
      decodeImageCallback_(NULL),
      argc_(1),
      argv_(NULL)
    {
    }
  };


  
  class OrthancPlugins::WorklistHandler : public IWorklistRequestHandler
  {
  private:
    OrthancPlugins&  that_;
    std::auto_ptr<HierarchicalMatcher> matcher_;
    ParsedDicomFile* currentQuery_;

    void Reset()
    {
      matcher_.reset(NULL);
      currentQuery_ = NULL;
    }

  public:
    WorklistHandler(OrthancPlugins& that) : that_(that)
    {
      Reset();
    }

    virtual void Handle(DicomFindAnswers& answers,
                        ParsedDicomFile& query,
                        const std::string& remoteIp,
                        const std::string& remoteAet,
                        const std::string& calledAet)
    {
      bool caseSensitivePN = Configuration::GetGlobalBoolParameter("CaseSensitivePN", false);
      matcher_.reset(new HierarchicalMatcher(query, caseSensitivePN));
      currentQuery_ = &query;

      {
        boost::mutex::scoped_lock lock(that_.pimpl_->worklistCallbackMutex_);

        if (that_.pimpl_->worklistCallback_)
        {
          OrthancPluginErrorCode error = that_.pimpl_->worklistCallback_
            (reinterpret_cast<OrthancPluginWorklistAnswers*>(&answers),
             reinterpret_cast<const OrthancPluginWorklistQuery*>(this),
             remoteAet.c_str(),
             calledAet.c_str());

          if (error != OrthancPluginErrorCode_Success)
          {
            Reset();
            that_.GetErrorDictionary().LogError(error, true);
            throw OrthancException(static_cast<ErrorCode>(error));
          }
        }
      }

      Reset();
    }

    void GetDicomQuery(OrthancPluginMemoryBuffer& target) const
    {
      assert(currentQuery_ != NULL);
      std::string dicom;
      currentQuery_->SaveToMemoryBuffer(dicom);
      CopyToMemoryBuffer(target, dicom.c_str(), dicom.size());
    }

    bool IsMatch(const void* dicom,
                 size_t size) const
    {
      assert(matcher_.get() != NULL);
      ParsedDicomFile f(dicom, size);
      return matcher_->Match(f);
    }

    void AddAnswer(OrthancPluginWorklistAnswers* answers,
                   const void* dicom,
                   size_t size) const
    {
      assert(matcher_.get() != NULL);
      ParsedDicomFile f(dicom, size);
      std::auto_ptr<ParsedDicomFile> summary(matcher_->Extract(f));
      reinterpret_cast<DicomFindAnswers*>(answers)->Add(*summary);
    }
  };

  
  static char* CopyString(const std::string& str)
  {
    char *result = reinterpret_cast<char*>(malloc(str.size() + 1));
    if (result == NULL)
    {
      throw OrthancException(ErrorCode_NotEnoughMemory);
    }

    if (str.size() == 0)
    {
      result[0] = '\0';
    }
    else
    {
      memcpy(result, &str[0], str.size() + 1);
    }

    return result;
  }


  OrthancPlugins::OrthancPlugins()
  {
    /* Sanity check of the compiler */
    if (sizeof(int32_t) != sizeof(OrthancPluginErrorCode) ||
        sizeof(int32_t) != sizeof(OrthancPluginHttpMethod) ||
        sizeof(int32_t) != sizeof(_OrthancPluginService) ||
        sizeof(int32_t) != sizeof(_OrthancPluginProperty) ||
        sizeof(int32_t) != sizeof(OrthancPluginPixelFormat) ||
        sizeof(int32_t) != sizeof(OrthancPluginContentType) ||
        sizeof(int32_t) != sizeof(OrthancPluginResourceType) ||
        sizeof(int32_t) != sizeof(OrthancPluginChangeType) ||
        sizeof(int32_t) != sizeof(OrthancPluginImageFormat) ||
        sizeof(int32_t) != sizeof(OrthancPluginCompressionType) ||
        sizeof(int32_t) != sizeof(OrthancPluginValueRepresentation) ||
        sizeof(int32_t) != sizeof(OrthancPluginDicomToJsonFlags) ||
        sizeof(int32_t) != sizeof(OrthancPluginDicomToJsonFormat) ||
        sizeof(int32_t) != sizeof(OrthancPluginCreateDicomFlags) ||
        sizeof(int32_t) != sizeof(_OrthancPluginDatabaseAnswerType) ||
        sizeof(int32_t) != sizeof(OrthancPluginIdentifierConstraint) ||
        sizeof(int32_t) != sizeof(OrthancPluginInstanceOrigin) ||
        static_cast<int>(OrthancPluginDicomToJsonFlags_IncludeBinary) != static_cast<int>(DicomToJsonFlags_IncludeBinary) ||
        static_cast<int>(OrthancPluginDicomToJsonFlags_IncludePrivateTags) != static_cast<int>(DicomToJsonFlags_IncludePrivateTags) ||
        static_cast<int>(OrthancPluginDicomToJsonFlags_IncludeUnknownTags) != static_cast<int>(DicomToJsonFlags_IncludeUnknownTags) ||
        static_cast<int>(OrthancPluginDicomToJsonFlags_IncludePixelData) != static_cast<int>(DicomToJsonFlags_IncludePixelData) ||
        static_cast<int>(OrthancPluginDicomToJsonFlags_ConvertBinaryToNull) != static_cast<int>(DicomToJsonFlags_ConvertBinaryToNull) ||
        static_cast<int>(OrthancPluginDicomToJsonFlags_ConvertBinaryToAscii) != static_cast<int>(DicomToJsonFlags_ConvertBinaryToAscii) ||
        static_cast<int>(OrthancPluginCreateDicomFlags_DecodeDataUriScheme) != static_cast<int>(DicomFromJsonFlags_DecodeDataUriScheme) ||
        static_cast<int>(OrthancPluginCreateDicomFlags_GenerateIdentifiers) != static_cast<int>(DicomFromJsonFlags_GenerateIdentifiers))

    {
      throw OrthancException(ErrorCode_Plugin);
    }

    pimpl_.reset(new PImpl());
    pimpl_->manager_.RegisterServiceProvider(*this);
  }

  
  void OrthancPlugins::SetServerContext(ServerContext& context)
  {
    pimpl_->context_ = &context;
  }


  
  OrthancPlugins::~OrthancPlugins()
  {
    for (PImpl::RestCallbacks::iterator it = pimpl_->restCallbacks_.begin(); 
         it != pimpl_->restCallbacks_.end(); ++it)
    {
      delete *it;
    }
  }


  static void ArgumentsToPlugin(std::vector<const char*>& keys,
                                std::vector<const char*>& values,
                                const IHttpHandler::Arguments& arguments)
  {
    keys.resize(arguments.size());
    values.resize(arguments.size());

    size_t pos = 0;
    for (IHttpHandler::Arguments::const_iterator 
           it = arguments.begin(); it != arguments.end(); ++it)
    {
      keys[pos] = it->first.c_str();
      values[pos] = it->second.c_str();
      pos++;
    }
  }


  static void ArgumentsToPlugin(std::vector<const char*>& keys,
                                std::vector<const char*>& values,
                                const IHttpHandler::GetArguments& arguments)
  {
    keys.resize(arguments.size());
    values.resize(arguments.size());

    for (size_t i = 0; i < arguments.size(); i++)
    {
      keys[i] = arguments[i].first.c_str();
      values[i] = arguments[i].second.c_str();
    }
  }


  bool OrthancPlugins::Handle(HttpOutput& output,
                              RequestOrigin /*origin*/,
                              const char* /*remoteIp*/,
                              const char* /*username*/,
                              HttpMethod method,
                              const UriComponents& uri,
                              const Arguments& headers,
                              const GetArguments& getArguments,
                              const char* bodyData,
                              size_t bodySize)
  {
    std::string flatUri = Toolbox::FlattenUri(uri);
    PImpl::RestCallback* callback = NULL;

    std::vector<std::string> groups;
    std::vector<const char*> cgroups;

    // Loop over the callbacks registered by the plugins
    bool found = false;
    for (PImpl::RestCallbacks::const_iterator it = pimpl_->restCallbacks_.begin(); 
         it != pimpl_->restCallbacks_.end() && !found; ++it)
    {
      // Check whether the regular expression associated to this
      // callback matches the URI
      boost::cmatch what;
      if (boost::regex_match(flatUri.c_str(), what, (*it)->GetRegularExpression()))
      {
        callback = *it;

        // Extract the value of the free parameters of the regular expression
        if (what.size() > 1)
        {
          groups.resize(what.size() - 1);
          cgroups.resize(what.size() - 1);
          for (size_t i = 1; i < what.size(); i++)
          {
            groups[i - 1] = what[i];
            cgroups[i - 1] = groups[i - 1].c_str();
          }
        }
      }
    }

    if (callback == NULL)
    {
      // Callback not found
      return false;
    }

    LOG(INFO) << "Delegating HTTP request to plugin for URI: " << flatUri;

    std::vector<const char*> getKeys, getValues, headersKeys, headersValues;

    OrthancPluginHttpRequest request;
    memset(&request, 0, sizeof(OrthancPluginHttpRequest));

    ArgumentsToPlugin(headersKeys, headersValues, headers);

    switch (method)
    {
      case HttpMethod_Get:
        request.method = OrthancPluginHttpMethod_Get;
        ArgumentsToPlugin(getKeys, getValues, getArguments);
        break;

      case HttpMethod_Post:
        request.method = OrthancPluginHttpMethod_Post;
        break;

      case HttpMethod_Delete:
        request.method = OrthancPluginHttpMethod_Delete;
        break;

      case HttpMethod_Put:
        request.method = OrthancPluginHttpMethod_Put;
        break;

      default:
        throw OrthancException(ErrorCode_InternalError);
    }


    request.groups = (cgroups.size() ? &cgroups[0] : NULL);
    request.groupsCount = cgroups.size();
    request.getCount = getArguments.size();
    request.body = bodyData;
    request.bodySize = bodySize;
    request.headersCount = headers.size();
    
    if (getArguments.size() > 0)
    {
      request.getKeys = &getKeys[0];
      request.getValues = &getValues[0];
    }
    
    if (headers.size() > 0)
    {
      request.headersKeys = &headersKeys[0];
      request.headersValues = &headersValues[0];
    }

    assert(callback != NULL);
    OrthancPluginErrorCode error = callback->Invoke(pimpl_->restCallbackMutex_, output, flatUri, request);

    if (error == OrthancPluginErrorCode_Success && 
        output.IsWritingMultipart())
    {
      output.CloseMultipart();
    }

    if (error == OrthancPluginErrorCode_Success)
    {
      return true;
    }
    else
    {
      GetErrorDictionary().LogError(error, true);
      throw OrthancException(static_cast<ErrorCode>(error));
    }
  }


  void OrthancPlugins::SignalStoredInstance(const std::string& instanceId,
                                            DicomInstanceToStore& instance,
                                            const Json::Value& simplifiedTags)
  {
    boost::recursive_mutex::scoped_lock lock(pimpl_->storedCallbackMutex_);

    for (PImpl::OnStoredCallbacks::const_iterator
           callback = pimpl_->onStoredCallbacks_.begin(); 
         callback != pimpl_->onStoredCallbacks_.end(); ++callback)
    {
      OrthancPluginErrorCode error = (*callback) 
        (reinterpret_cast<OrthancPluginDicomInstance*>(&instance),
         instanceId.c_str());

      if (error != OrthancPluginErrorCode_Success)
      {
        GetErrorDictionary().LogError(error, true);
        throw OrthancException(static_cast<ErrorCode>(error));
      }
    }
  }



  void OrthancPlugins::SignalChangeInternal(OrthancPluginChangeType changeType,
                                            OrthancPluginResourceType resourceType,
                                            const char* resource)
  {
    boost::recursive_mutex::scoped_lock lock(pimpl_->changeCallbackMutex_);

    for (std::list<OrthancPluginOnChangeCallback>::const_iterator 
           callback = pimpl_->onChangeCallbacks_.begin(); 
         callback != pimpl_->onChangeCallbacks_.end(); ++callback)
    {
      OrthancPluginErrorCode error = (*callback) (changeType, resourceType, resource);

      if (error != OrthancPluginErrorCode_Success)
      {
        GetErrorDictionary().LogError(error, true);
        throw OrthancException(static_cast<ErrorCode>(error));
      }
    }
  }



  void OrthancPlugins::SignalChange(const ServerIndexChange& change)
  {
    SignalChangeInternal(Plugins::Convert(change.GetChangeType()),
                         Plugins::Convert(change.GetResourceType()),
                         change.GetPublicId().c_str());
  }



  void OrthancPlugins::RegisterRestCallback(const void* parameters,
                                            bool lock)
  {
    const _OrthancPluginRestCallback& p = 
      *reinterpret_cast<const _OrthancPluginRestCallback*>(parameters);

    LOG(INFO) << "Plugin has registered a REST callback "
              << (lock ? "with" : "witout")
              << " mutual exclusion on: " 
              << p.pathRegularExpression;

    pimpl_->restCallbacks_.push_back(new PImpl::RestCallback(p.pathRegularExpression, p.callback, lock));
  }



  void OrthancPlugins::RegisterOnStoredInstanceCallback(const void* parameters)
  {
    const _OrthancPluginOnStoredInstanceCallback& p = 
      *reinterpret_cast<const _OrthancPluginOnStoredInstanceCallback*>(parameters);

    LOG(INFO) << "Plugin has registered an OnStoredInstance callback";
    pimpl_->onStoredCallbacks_.push_back(p.callback);
  }


  void OrthancPlugins::RegisterOnChangeCallback(const void* parameters)
  {
    const _OrthancPluginOnChangeCallback& p = 
      *reinterpret_cast<const _OrthancPluginOnChangeCallback*>(parameters);

    LOG(INFO) << "Plugin has registered an OnChange callback";
    pimpl_->onChangeCallbacks_.push_back(p.callback);
  }


  void OrthancPlugins::RegisterWorklistCallback(const void* parameters)
  {
    const _OrthancPluginWorklistCallback& p = 
      *reinterpret_cast<const _OrthancPluginWorklistCallback*>(parameters);

    boost::mutex::scoped_lock lock(pimpl_->worklistCallbackMutex_);

    if (pimpl_->worklistCallback_ != NULL)
    {
      LOG(ERROR) << "Can only register one plugin to handle modality worklists";
      throw OrthancException(ErrorCode_Plugin);
    }
    else
    {
      LOG(INFO) << "Plugin has registered a callback to handle modality worklists";
      pimpl_->worklistCallback_ = p.callback;
    }
  }


  void OrthancPlugins::RegisterDecodeImageCallback(const void* parameters)
  {
    const _OrthancPluginDecodeImageCallback& p = 
      *reinterpret_cast<const _OrthancPluginDecodeImageCallback*>(parameters);

    boost::mutex::scoped_lock lock(pimpl_->decodeImageCallbackMutex_);

    if (pimpl_->decodeImageCallback_ != NULL)
    {
      LOG(ERROR) << "Can only register one plugin to handle the decompression of DICOM images";
      throw OrthancException(ErrorCode_Plugin);
    }
    else
    {
      LOG(INFO) << "Plugin has registered a callback to decode DICOM images";
      pimpl_->decodeImageCallback_ = p.callback;
    }
  }




  void OrthancPlugins::AnswerBuffer(const void* parameters)
  {
    const _OrthancPluginAnswerBuffer& p = 
      *reinterpret_cast<const _OrthancPluginAnswerBuffer*>(parameters);

    HttpOutput* translatedOutput = reinterpret_cast<HttpOutput*>(p.output);
    translatedOutput->SetContentType(p.mimeType);
    translatedOutput->Answer(p.answer, p.answerSize);
  }


  void OrthancPlugins::Redirect(const void* parameters)
  {
    const _OrthancPluginOutputPlusArgument& p = 
      *reinterpret_cast<const _OrthancPluginOutputPlusArgument*>(parameters);

    HttpOutput* translatedOutput = reinterpret_cast<HttpOutput*>(p.output);
    translatedOutput->Redirect(p.argument);
  }


  void OrthancPlugins::SendHttpStatusCode(const void* parameters)
  {
    const _OrthancPluginSendHttpStatusCode& p = 
      *reinterpret_cast<const _OrthancPluginSendHttpStatusCode*>(parameters);

    HttpOutput* translatedOutput = reinterpret_cast<HttpOutput*>(p.output);
    translatedOutput->SendStatus(static_cast<HttpStatus>(p.status));
  }


  void OrthancPlugins::SendHttpStatus(const void* parameters)
  {
    const _OrthancPluginSendHttpStatus& p = 
      *reinterpret_cast<const _OrthancPluginSendHttpStatus*>(parameters);

    HttpOutput* translatedOutput = reinterpret_cast<HttpOutput*>(p.output);
    HttpStatus status = static_cast<HttpStatus>(p.status);

    if (p.bodySize > 0 && p.body != NULL)
    {
      translatedOutput->SendStatus(status, p.body, p.bodySize);
    }
    else
    {
      translatedOutput->SendStatus(status);
    }
  }


  void OrthancPlugins::SendUnauthorized(const void* parameters)
  {
    const _OrthancPluginOutputPlusArgument& p = 
      *reinterpret_cast<const _OrthancPluginOutputPlusArgument*>(parameters);

    HttpOutput* translatedOutput = reinterpret_cast<HttpOutput*>(p.output);
    translatedOutput->SendUnauthorized(p.argument);
  }


  void OrthancPlugins::SendMethodNotAllowed(const void* parameters)
  {
    const _OrthancPluginOutputPlusArgument& p = 
      *reinterpret_cast<const _OrthancPluginOutputPlusArgument*>(parameters);

    HttpOutput* translatedOutput = reinterpret_cast<HttpOutput*>(p.output);
    translatedOutput->SendMethodNotAllowed(p.argument);
  }


  void OrthancPlugins::SetCookie(const void* parameters)
  {
    const _OrthancPluginSetHttpHeader& p = 
      *reinterpret_cast<const _OrthancPluginSetHttpHeader*>(parameters);

    HttpOutput* translatedOutput = reinterpret_cast<HttpOutput*>(p.output);
    translatedOutput->SetCookie(p.key, p.value);
  }


  void OrthancPlugins::SetHttpHeader(const void* parameters)
  {
    const _OrthancPluginSetHttpHeader& p = 
      *reinterpret_cast<const _OrthancPluginSetHttpHeader*>(parameters);

    HttpOutput* translatedOutput = reinterpret_cast<HttpOutput*>(p.output);
    translatedOutput->AddHeader(p.key, p.value);
  }


  void OrthancPlugins::CompressAndAnswerPngImage(const void* parameters)
  {
    // Bridge for backward compatibility with Orthanc <= 0.9.3
    const _OrthancPluginCompressAndAnswerPngImage& p = 
      *reinterpret_cast<const _OrthancPluginCompressAndAnswerPngImage*>(parameters);

    _OrthancPluginCompressAndAnswerImage p2;
    p2.output = p.output;
    p2.imageFormat = OrthancPluginImageFormat_Png;
    p2.pixelFormat = p.format;
    p2.width = p.width;
    p2.height = p.height;
    p2.pitch = p.height;
    p2.buffer = p.buffer;
    p2.quality = 0;

    CompressAndAnswerImage(&p2);
  }


  void OrthancPlugins::CompressAndAnswerImage(const void* parameters)
  {
    const _OrthancPluginCompressAndAnswerImage& p = 
      *reinterpret_cast<const _OrthancPluginCompressAndAnswerImage*>(parameters);

    HttpOutput* translatedOutput = reinterpret_cast<HttpOutput*>(p.output);

    ImageAccessor accessor;
    accessor.AssignReadOnly(Plugins::Convert(p.pixelFormat), p.width, p.height, p.pitch, p.buffer);

    std::string compressed;

    switch (p.imageFormat)
    {
      case OrthancPluginImageFormat_Png:
      {
        PngWriter writer;
        writer.WriteToMemory(compressed, accessor);
        translatedOutput->SetContentType("image/png");
        break;
      }

      case OrthancPluginImageFormat_Jpeg:
      {
        JpegWriter writer;
        writer.SetQuality(p.quality);
        writer.WriteToMemory(compressed, accessor);
        translatedOutput->SetContentType("image/jpeg");
        break;
      }

      default:
        throw OrthancException(ErrorCode_ParameterOutOfRange);
    }

    translatedOutput->Answer(compressed);
  }


  void OrthancPlugins::CheckContextAvailable()
  {
    if (!pimpl_->context_)
    {
      throw OrthancException(ErrorCode_DatabaseNotInitialized);
    }
  }


  void OrthancPlugins::GetDicomForInstance(const void* parameters)
  {
    const _OrthancPluginGetDicomForInstance& p = 
      *reinterpret_cast<const _OrthancPluginGetDicomForInstance*>(parameters);

    std::string dicom;

    CheckContextAvailable();
    pimpl_->context_->ReadFile(dicom, p.instanceId, FileContentType_Dicom);

    CopyToMemoryBuffer(*p.target, dicom);
  }


  void OrthancPlugins::RestApiGet(const void* parameters,
                                  bool afterPlugins)
  {
    const _OrthancPluginRestApiGet& p = 
      *reinterpret_cast<const _OrthancPluginRestApiGet*>(parameters);
        
    LOG(INFO) << "Plugin making REST GET call on URI " << p.uri
              << (afterPlugins ? " (after plugins)" : " (built-in API)");

    CheckContextAvailable();
    IHttpHandler& handler = pimpl_->context_->GetHttpHandler().RestrictToOrthancRestApi(!afterPlugins);

    std::string result;
    if (HttpToolbox::SimpleGet(result, handler, RequestOrigin_Plugins, p.uri))
    {
      CopyToMemoryBuffer(*p.target, result);
    }
    else
    {
      throw OrthancException(ErrorCode_BadRequest);
    }
  }


  void OrthancPlugins::RestApiGet2(const void* parameters)
  {
    const _OrthancPluginRestApiGet2& p = 
      *reinterpret_cast<const _OrthancPluginRestApiGet2*>(parameters);
        
    LOG(INFO) << "Plugin making REST GET call on URI " << p.uri
              << (p.afterPlugins ? " (after plugins)" : " (built-in API)");

    IHttpHandler::Arguments headers;

    for (uint32_t i = 0; i < p.headersCount; i++)
    {
      headers[p.headersKeys[i]] = p.headersValues[i];
    }

    CheckContextAvailable();
    IHttpHandler& handler = pimpl_->context_->GetHttpHandler().RestrictToOrthancRestApi(!p.afterPlugins);

    std::string result;
    if (HttpToolbox::SimpleGet(result, handler, RequestOrigin_Plugins, p.uri, headers))
    {
      CopyToMemoryBuffer(*p.target, result);
    }
    else
    {
      throw OrthancException(ErrorCode_BadRequest);
    }
  }


  void OrthancPlugins::RestApiPostPut(bool isPost, 
                                      const void* parameters,
                                      bool afterPlugins)
  {
    const _OrthancPluginRestApiPostPut& p = 
      *reinterpret_cast<const _OrthancPluginRestApiPostPut*>(parameters);

    LOG(INFO) << "Plugin making REST " << EnumerationToString(isPost ? HttpMethod_Post : HttpMethod_Put)
              << " call on URI " << p.uri << (afterPlugins ? " (after plugins)" : " (built-in API)");

    CheckContextAvailable();
    IHttpHandler& handler = pimpl_->context_->GetHttpHandler().RestrictToOrthancRestApi(!afterPlugins);

    std::string result;
    if (isPost ? 
        HttpToolbox::SimplePost(result, handler, RequestOrigin_Plugins, p.uri, p.body, p.bodySize) :
        HttpToolbox::SimplePut (result, handler, RequestOrigin_Plugins, p.uri, p.body, p.bodySize))
    {
      CopyToMemoryBuffer(*p.target, result);
    }
    else
    {
      throw OrthancException(ErrorCode_BadRequest);
    }
  }


  void OrthancPlugins::RestApiDelete(const void* parameters,
                                     bool afterPlugins)
  {
    const char* uri = reinterpret_cast<const char*>(parameters);
    LOG(INFO) << "Plugin making REST DELETE call on URI " << uri
              << (afterPlugins ? " (after plugins)" : " (built-in API)");

    CheckContextAvailable();
    IHttpHandler& handler = pimpl_->context_->GetHttpHandler().RestrictToOrthancRestApi(!afterPlugins);

    if (!HttpToolbox::SimpleDelete(handler, RequestOrigin_Plugins, uri))
    {
      throw OrthancException(ErrorCode_BadRequest);
    }
  }


  void OrthancPlugins::LookupResource(_OrthancPluginService service,
                                      const void* parameters)
  {
    const _OrthancPluginRetrieveDynamicString& p = 
      *reinterpret_cast<const _OrthancPluginRetrieveDynamicString*>(parameters);

    /**
     * The enumeration below only uses the tags that are indexed in
     * the Orthanc database. It reflects the
     * "CandidateResources::ApplyFilter()" method of the
     * "OrthancFindRequestHandler" class.
     **/

    DicomTag tag(0, 0);
    ResourceType level;
    switch (service)
    {
      case _OrthancPluginService_LookupPatient:
        tag = DICOM_TAG_PATIENT_ID;
        level = ResourceType_Patient;
        break;

      case _OrthancPluginService_LookupStudy:
        tag = DICOM_TAG_STUDY_INSTANCE_UID;
        level = ResourceType_Study;
        break;

      case _OrthancPluginService_LookupStudyWithAccessionNumber:
        tag = DICOM_TAG_ACCESSION_NUMBER;
        level = ResourceType_Study;
        break;

      case _OrthancPluginService_LookupSeries:
        tag = DICOM_TAG_SERIES_INSTANCE_UID;
        level = ResourceType_Series;
        break;

      case _OrthancPluginService_LookupInstance:
        tag = DICOM_TAG_SOP_INSTANCE_UID;
        level = ResourceType_Instance;
        break;

      default:
        throw OrthancException(ErrorCode_InternalError);
    }

    CheckContextAvailable();

    std::list<std::string> result;
    pimpl_->context_->GetIndex().LookupIdentifierExact(result, level, tag, p.argument);

    if (result.size() == 1)
    {
      *p.result = CopyString(result.front());
    }
    else
    {
      throw OrthancException(ErrorCode_UnknownResource);
    }
  }


  static void AccessInstanceMetadataInternal(bool checkExistence,
                                             const _OrthancPluginAccessDicomInstance& params,
                                             const DicomInstanceToStore& instance)
  {
    MetadataType metadata;

    try
    {
      metadata = StringToMetadata(params.key);
    }
    catch (OrthancException&)
    {
      // Unknown metadata
      if (checkExistence)
      {
        *params.resultInt64 = -1;
      }
      else
      {
        *params.resultString = NULL;
      }

      return;
    }

    ServerIndex::MetadataMap::const_iterator it = 
      instance.GetMetadata().find(std::make_pair(ResourceType_Instance, metadata));

    if (checkExistence)
    {
      if (it != instance.GetMetadata().end())
      {
        *params.resultInt64 = 1;
      }
      else
      {
        *params.resultInt64 = 0;
      }
    }
    else
    {
      if (it != instance.GetMetadata().end())
      {      
        *params.resultString = it->second.c_str();
      }
      else
      {
        // Error: Missing metadata
        *params.resultString = NULL;
      }
    }
  }


  static void AccessDicomInstance(_OrthancPluginService service,
                                  const void* parameters)
  {
    const _OrthancPluginAccessDicomInstance& p = 
      *reinterpret_cast<const _OrthancPluginAccessDicomInstance*>(parameters);

    DicomInstanceToStore& instance =
      *reinterpret_cast<DicomInstanceToStore*>(p.instance);

    switch (service)
    {
      case _OrthancPluginService_GetInstanceRemoteAet:
        *p.resultString = instance.GetRemoteAet();
        return;

      case _OrthancPluginService_GetInstanceSize:
        *p.resultInt64 = instance.GetBufferSize();
        return;

      case _OrthancPluginService_GetInstanceData:
        *p.resultString = instance.GetBufferData();
        return;

      case _OrthancPluginService_HasInstanceMetadata:
        AccessInstanceMetadataInternal(true, p, instance);
        return;

      case _OrthancPluginService_GetInstanceMetadata:
        AccessInstanceMetadataInternal(false, p, instance);
        return;

      case _OrthancPluginService_GetInstanceJson:
      case _OrthancPluginService_GetInstanceSimplifiedJson:
      {
        Json::StyledWriter writer;
        std::string s;

        if (service == _OrthancPluginService_GetInstanceJson)
        {
          s = writer.write(instance.GetJson());
        }
        else
        {
          Json::Value simplified;
          Toolbox::SimplifyTags(simplified, instance.GetJson(), DicomToJsonFormat_Human);
          s = writer.write(simplified);
        }

        *p.resultStringToFree = CopyString(s);
        return;
      }

      case _OrthancPluginService_GetInstanceOrigin:   // New in Orthanc 0.9.5
        *p.resultOrigin = Plugins::Convert(instance.GetRequestOrigin());
        return;

      default:
        throw OrthancException(ErrorCode_InternalError);
    }
  }


  void OrthancPlugins::BufferCompression(const void* parameters)
  {
    const _OrthancPluginBufferCompression& p = 
      *reinterpret_cast<const _OrthancPluginBufferCompression*>(parameters);

    std::string result;

    {
      std::auto_ptr<DeflateBaseCompressor> compressor;

      switch (p.compression)
      {
        case OrthancPluginCompressionType_Zlib:
        {
          compressor.reset(new ZlibCompressor);
          compressor->SetPrefixWithUncompressedSize(false);
          break;
        }

        case OrthancPluginCompressionType_ZlibWithSize:
        {
          compressor.reset(new ZlibCompressor);
          compressor->SetPrefixWithUncompressedSize(true);
          break;
        }

        case OrthancPluginCompressionType_Gzip:
        {
          compressor.reset(new GzipCompressor);
          compressor->SetPrefixWithUncompressedSize(false);
          break;
        }

        case OrthancPluginCompressionType_GzipWithSize:
        {
          compressor.reset(new GzipCompressor);
          compressor->SetPrefixWithUncompressedSize(true);
          break;
        }

        default:
          throw OrthancException(ErrorCode_ParameterOutOfRange);
      }

      if (p.uncompress)
      {
        compressor->Uncompress(result, p.source, p.size);
      }
      else
      {
        compressor->Compress(result, p.source, p.size);
      }
    }

    CopyToMemoryBuffer(*p.target, result);
  }


  static OrthancPluginImage* ReturnImage(std::auto_ptr<ImageAccessor>& image)
  {
    // Images returned to plugins are assumed to be writeable. If the
    // input image is read-only, we return a copy so that it can be modified.

    if (image->IsReadOnly())
    {
      std::auto_ptr<Image> copy(new Image(image->GetFormat(), image->GetWidth(), image->GetHeight()));
      ImageProcessing::Copy(*copy, *image);
      image.reset(NULL);
      return reinterpret_cast<OrthancPluginImage*>(copy.release());
    }
    else
    {
      return reinterpret_cast<OrthancPluginImage*>(image.release());
    }
  }


  void OrthancPlugins::UncompressImage(const void* parameters)
  {
    const _OrthancPluginUncompressImage& p = *reinterpret_cast<const _OrthancPluginUncompressImage*>(parameters);

    std::auto_ptr<ImageAccessor> image;

    switch (p.format)
    {
      case OrthancPluginImageFormat_Png:
      {
        image.reset(new PngReader);
        reinterpret_cast<PngReader&>(*image).ReadFromMemory(p.data, p.size);
        break;
      }

      case OrthancPluginImageFormat_Jpeg:
      {
        image.reset(new JpegReader);
        reinterpret_cast<JpegReader&>(*image).ReadFromMemory(p.data, p.size);
        break;
      }

      case OrthancPluginImageFormat_Dicom:
      {
        image.reset(Decode(p.data, p.size, 0));
        break;
      }

      default:
        throw OrthancException(ErrorCode_ParameterOutOfRange);
    }

    *(p.target) = ReturnImage(image);
  }


  void OrthancPlugins::CompressImage(const void* parameters)
  {
    const _OrthancPluginCompressImage& p = *reinterpret_cast<const _OrthancPluginCompressImage*>(parameters);

    std::string compressed;

    switch (p.imageFormat)
    {
      case OrthancPluginImageFormat_Png:
      {
        PngWriter writer;
        writer.WriteToMemory(compressed, p.width, p.height, p.pitch, Plugins::Convert(p.pixelFormat), p.buffer);
        break;
      }

      case OrthancPluginImageFormat_Jpeg:
      {
        JpegWriter writer;
        writer.SetQuality(p.quality);
        writer.WriteToMemory(compressed, p.width, p.height, p.pitch, Plugins::Convert(p.pixelFormat), p.buffer);
        break;
      }

      default:
        throw OrthancException(ErrorCode_ParameterOutOfRange);
    }

    CopyToMemoryBuffer(*p.target, compressed.size() > 0 ? compressed.c_str() : NULL, compressed.size());
  }


  void OrthancPlugins::CallHttpClient(const void* parameters)
  {
    const _OrthancPluginCallHttpClient& p = *reinterpret_cast<const _OrthancPluginCallHttpClient*>(parameters);

    HttpClient client;
    client.SetUrl(p.url);

    if (p.username != NULL && 
        p.password != NULL)
    {
      client.SetCredentials(p.username, p.password);
    }

    switch (p.method)
    {
      case OrthancPluginHttpMethod_Get:
        client.SetMethod(HttpMethod_Get);
        break;

      case OrthancPluginHttpMethod_Post:
        client.SetMethod(HttpMethod_Post);
        client.GetBody().assign(p.body, p.bodySize);
        break;

      case OrthancPluginHttpMethod_Put:
        client.SetMethod(HttpMethod_Put);
        client.GetBody().assign(p.body, p.bodySize);
        break;

      case OrthancPluginHttpMethod_Delete:
        client.SetMethod(HttpMethod_Delete);
        break;

      default:
        throw OrthancException(ErrorCode_ParameterOutOfRange);
    }

    std::string s;
    client.ApplyAndThrowException(s);

    if (p.method != OrthancPluginHttpMethod_Delete)
    {
      CopyToMemoryBuffer(*p.target, s);
    }
  }


  void OrthancPlugins::ConvertPixelFormat(const void* parameters)
  {
    const _OrthancPluginConvertPixelFormat& p = *reinterpret_cast<const _OrthancPluginConvertPixelFormat*>(parameters);
    const ImageAccessor& source = *reinterpret_cast<const ImageAccessor*>(p.source);

    std::auto_ptr<ImageAccessor> target(new Image(Plugins::Convert(p.targetFormat), source.GetWidth(), source.GetHeight()));
    ImageProcessing::Convert(*target, source);

    *(p.target) = ReturnImage(target);
  }



  void OrthancPlugins::GetFontInfo(const void* parameters)
  {
    const _OrthancPluginGetFontInfo& p = *reinterpret_cast<const _OrthancPluginGetFontInfo*>(parameters);

    const Font& font = Configuration::GetFontRegistry().GetFont(p.fontIndex);

    if (p.name != NULL)
    {
      *(p.name) = font.GetName().c_str();
    }
    else if (p.size != NULL)
    {
      *(p.size) = font.GetSize();
    }
    else
    {
      throw OrthancException(ErrorCode_InternalError);
    }
  }


  void OrthancPlugins::DrawText(const void* parameters)
  {
    const _OrthancPluginDrawText& p = *reinterpret_cast<const _OrthancPluginDrawText*>(parameters);

    ImageAccessor& target = *reinterpret_cast<ImageAccessor*>(p.image);
    const Font& font = Configuration::GetFontRegistry().GetFont(p.fontIndex);

    font.Draw(target, p.utf8Text, p.x, p.y, p.r, p.g, p.b);
  }


  void OrthancPlugins::ApplyDicomToJson(_OrthancPluginService service,
                                        const void* parameters)
  {
    const _OrthancPluginDicomToJson& p =
      *reinterpret_cast<const _OrthancPluginDicomToJson*>(parameters);

    std::auto_ptr<ParsedDicomFile> dicom;

    if (service == _OrthancPluginService_DicomBufferToJson)
    {
      dicom.reset(new ParsedDicomFile(p.buffer, p.size));
    }
    else
    {
      if (p.instanceId == NULL)
      {
        throw OrthancException(ErrorCode_ParameterOutOfRange);
      }

      std::string content;
      pimpl_->context_->ReadFile(content, p.instanceId, FileContentType_Dicom);
      dicom.reset(new ParsedDicomFile(content));
    }

    Json::Value json;
    dicom->ToJson(json, Plugins::Convert(p.format), 
                  static_cast<DicomToJsonFlags>(p.flags), p.maxStringLength);

    Json::FastWriter writer;
    *p.result = CopyString(writer.write(json));
  }
        

  void OrthancPlugins::ApplyCreateDicom(_OrthancPluginService service,
                                        const void* parameters)
  {
    const _OrthancPluginCreateDicom& p =
      *reinterpret_cast<const _OrthancPluginCreateDicom*>(parameters);

    Json::Value json;

    if (p.json == NULL)
    {
      json = Json::objectValue;
    }
    else
    {
      Json::Reader reader;
      if (!reader.parse(p.json, json))
      {
        throw OrthancException(ErrorCode_BadJson);
      }
    }

    std::string dicom;

    {
      std::auto_ptr<ParsedDicomFile> file
        (ParsedDicomFile::CreateFromJson(json, static_cast<DicomFromJsonFlags>(p.flags)));

      if (p.pixelData)
      {
        file->EmbedImage(*reinterpret_cast<const ImageAccessor*>(p.pixelData));
      }

      file->SaveToMemoryBuffer(dicom);
    }

    CopyToMemoryBuffer(*p.target, dicom);
  }


  void OrthancPlugins::ComputeHash(_OrthancPluginService service,
                                   const void* parameters)
  {
    const _OrthancPluginComputeHash& p =
      *reinterpret_cast<const _OrthancPluginComputeHash*>(parameters);
 
    std::string hash;
    switch (service)
    {
      case _OrthancPluginService_ComputeMd5:
        Toolbox::ComputeMD5(hash, p.buffer, p.size);
        break;

      case _OrthancPluginService_ComputeSha1:
        Toolbox::ComputeSHA1(hash, p.buffer, p.size);
        break;

      default:
        throw OrthancException(ErrorCode_ParameterOutOfRange);
    }
   
    *p.result = CopyString(hash);
  }


  void OrthancPlugins::ApplyCreateImage(_OrthancPluginService service,
                                        const void* parameters)
  {
    const _OrthancPluginCreateImage& p =
      *reinterpret_cast<const _OrthancPluginCreateImage*>(parameters);

    std::auto_ptr<ImageAccessor> result;

    switch (service)
    {
      case _OrthancPluginService_CreateImage:
        result.reset(new Image(Plugins::Convert(p.format), p.width, p.height));
        break;

      case _OrthancPluginService_CreateImageAccessor:
        result.reset(new ImageAccessor);
        result->AssignWritable(Plugins::Convert(p.format), p.width, p.height, p.pitch, p.buffer);
        break;

      case _OrthancPluginService_DecodeDicomImage:
      {
        result.reset(Decode(p.constBuffer, p.bufferSize, p.frameIndex));
        break;
      }

      default:
        throw OrthancException(ErrorCode_InternalError);
    }

    *(p.target) = ReturnImage(result);
  }


  void OrthancPlugins::ApplySendMultipartItem(const void* parameters)
  {
    // An exception might be raised in this function if the
    // connection was closed by the HTTP client.
    const _OrthancPluginAnswerBuffer& p =
      *reinterpret_cast<const _OrthancPluginAnswerBuffer*>(parameters);

    HttpOutput* output = reinterpret_cast<HttpOutput*>(p.output);

    std::map<std::string, std::string> headers;  // No custom headers
    output->SendMultipartItem(p.answer, p.answerSize, headers);
  }


  void OrthancPlugins::ApplySendMultipartItem2(const void* parameters)
  {
    // An exception might be raised in this function if the
    // connection was closed by the HTTP client.
    const _OrthancPluginSendMultipartItem2& p =
      *reinterpret_cast<const _OrthancPluginSendMultipartItem2*>(parameters);
    HttpOutput* output = reinterpret_cast<HttpOutput*>(p.output);
    
    std::map<std::string, std::string> headers;
    for (uint32_t i = 0; i < p.headersCount; i++)
    {
      headers[p.headersKeys[i]] = p.headersValues[i];
    }
    
    output->SendMultipartItem(p.answer, p.answerSize, headers);
  }
      

  void OrthancPlugins::DatabaseAnswer(const void* parameters)
  {
    const _OrthancPluginDatabaseAnswer& p =
      *reinterpret_cast<const _OrthancPluginDatabaseAnswer*>(parameters);

    if (pimpl_->database_.get() != NULL)
    {
      pimpl_->database_->AnswerReceived(p);
    }
    else
    {
      LOG(ERROR) << "Cannot invoke this service without a custom database back-end";
      throw OrthancException(ErrorCode_BadRequest);
    }
  }



  namespace
  {
    class DictionaryReadLocker
    {
    private:
      const DcmDataDictionary& dictionary_;

    public:
      DictionaryReadLocker() : dictionary_(dcmDataDict.rdlock())
      {
      }

      ~DictionaryReadLocker()
      {
        dcmDataDict.unlock();
      }

      const DcmDataDictionary* operator->()
      {
        return &dictionary_;
      }
    };
  }


  void OrthancPlugins::ApplyLookupDictionary(const void* parameters)
  {
    const _OrthancPluginLookupDictionary& p =
      *reinterpret_cast<const _OrthancPluginLookupDictionary*>(parameters);

    DicomTag tag(FromDcmtkBridge::ParseTag(p.name));
    DcmTagKey tag2(tag.GetGroup(), tag.GetElement());

    DictionaryReadLocker locker;
    const DcmDictEntry* entry = locker->findEntry(tag2, NULL);

    if (entry == NULL)
    {
      throw OrthancException(ErrorCode_UnknownDicomTag);
    }
    else
    {
      p.target->group = entry->getKey().getGroup();
      p.target->element = entry->getKey().getElement();
      p.target->vr = Plugins::Convert(entry->getEVR());
      p.target->minMultiplicity = static_cast<uint32_t>(entry->getVMMin());
      p.target->maxMultiplicity = (entry->getVMMax() == DcmVariableVM ? 0 : static_cast<uint32_t>(entry->getVMMax()));
    }
  }



  bool OrthancPlugins::InvokeService(SharedLibrary& plugin,
                                     _OrthancPluginService service,
                                     const void* parameters)
  {
    VLOG(1) << "Calling service " << service << " from plugin " << plugin.GetPath();

    if (service == _OrthancPluginService_DatabaseAnswer)
    {
      // This case solves a deadlock at (*) reported by James Webster
      // on 2015-10-27 that was present in versions of Orthanc <=
      // 0.9.4 and related to database plugins implementing a custom
      // index. The problem was that locking the database is already
      // ensured by the "ServerIndex" class if the invoked service is
      // "DatabaseAnswer".
      DatabaseAnswer(parameters);
      return true;
    }

    boost::recursive_mutex::scoped_lock lock(pimpl_->invokeServiceMutex_);   // (*)

    switch (service)
    {
      case _OrthancPluginService_GetOrthancPath:
      {
        std::string s = Toolbox::GetPathToExecutable();
        *reinterpret_cast<const _OrthancPluginRetrieveDynamicString*>(parameters)->result = CopyString(s);
        return true;
      }

      case _OrthancPluginService_GetOrthancDirectory:
      {
        std::string s = Toolbox::GetDirectoryOfExecutable();
        *reinterpret_cast<const _OrthancPluginRetrieveDynamicString*>(parameters)->result = CopyString(s);
        return true;
      }

      case _OrthancPluginService_GetConfigurationPath:
      {
        *reinterpret_cast<const _OrthancPluginRetrieveDynamicString*>(parameters)->result = 
          CopyString(Configuration::GetConfigurationAbsolutePath());
        return true;
      }

      case _OrthancPluginService_GetConfiguration:
      {
        std::string s;
        Configuration::FormatConfiguration(s);

        *reinterpret_cast<const _OrthancPluginRetrieveDynamicString*>(parameters)->result = CopyString(s);
        return true;
      }

      case _OrthancPluginService_BufferCompression:
        BufferCompression(parameters);
        return true;

      case _OrthancPluginService_RegisterRestCallback:
        RegisterRestCallback(parameters, true);
        return true;

      case _OrthancPluginService_RegisterRestCallbackNoLock:
        RegisterRestCallback(parameters, false);
        return true;

      case _OrthancPluginService_RegisterOnStoredInstanceCallback:
        RegisterOnStoredInstanceCallback(parameters);
        return true;

      case _OrthancPluginService_RegisterOnChangeCallback:
        RegisterOnChangeCallback(parameters);
        return true;

      case _OrthancPluginService_RegisterWorklistCallback:
        RegisterWorklistCallback(parameters);
        return true;

      case _OrthancPluginService_RegisterDecodeImageCallback:
        RegisterDecodeImageCallback(parameters);
        return true;

      case _OrthancPluginService_AnswerBuffer:
        AnswerBuffer(parameters);
        return true;

      case _OrthancPluginService_CompressAndAnswerPngImage:
        CompressAndAnswerPngImage(parameters);
        return true;

      case _OrthancPluginService_CompressAndAnswerImage:
        CompressAndAnswerImage(parameters);
        return true;

      case _OrthancPluginService_GetDicomForInstance:
        GetDicomForInstance(parameters);
        return true;

      case _OrthancPluginService_RestApiGet:
        RestApiGet(parameters, false);
        return true;

      case _OrthancPluginService_RestApiGetAfterPlugins:
        RestApiGet(parameters, true);
        return true;

      case _OrthancPluginService_RestApiGet2:
        RestApiGet2(parameters);
        return true;

      case _OrthancPluginService_RestApiPost:
        RestApiPostPut(true, parameters, false);
        return true;

      case _OrthancPluginService_RestApiPostAfterPlugins:
        RestApiPostPut(true, parameters, true);
        return true;

      case _OrthancPluginService_RestApiDelete:
        RestApiDelete(parameters, false);
        return true;

      case _OrthancPluginService_RestApiDeleteAfterPlugins:
        RestApiDelete(parameters, true);
        return true;

      case _OrthancPluginService_RestApiPut:
        RestApiPostPut(false, parameters, false);
        return true;

      case _OrthancPluginService_RestApiPutAfterPlugins:
        RestApiPostPut(false, parameters, true);
        return true;

      case _OrthancPluginService_Redirect:
        Redirect(parameters);
        return true;

      case _OrthancPluginService_SendUnauthorized:
        SendUnauthorized(parameters);
        return true;

      case _OrthancPluginService_SendMethodNotAllowed:
        SendMethodNotAllowed(parameters);
        return true;

      case _OrthancPluginService_SendHttpStatus:
        SendHttpStatus(parameters);
        return true;

      case _OrthancPluginService_SendHttpStatusCode:
        SendHttpStatusCode(parameters);
        return true;

      case _OrthancPluginService_SetCookie:
        SetCookie(parameters);
        return true;

      case _OrthancPluginService_SetHttpHeader:
        SetHttpHeader(parameters);
        return true;

      case _OrthancPluginService_LookupPatient:
      case _OrthancPluginService_LookupStudy:
      case _OrthancPluginService_LookupStudyWithAccessionNumber:
      case _OrthancPluginService_LookupSeries:
      case _OrthancPluginService_LookupInstance:
        LookupResource(service, parameters);
        return true;

      case _OrthancPluginService_GetInstanceRemoteAet:
      case _OrthancPluginService_GetInstanceSize:
      case _OrthancPluginService_GetInstanceData:
      case _OrthancPluginService_GetInstanceJson:
      case _OrthancPluginService_GetInstanceSimplifiedJson:
      case _OrthancPluginService_HasInstanceMetadata:
      case _OrthancPluginService_GetInstanceMetadata:
      case _OrthancPluginService_GetInstanceOrigin:
        AccessDicomInstance(service, parameters);
        return true;

      case _OrthancPluginService_RegisterStorageArea:
      {
        LOG(INFO) << "Plugin has registered a custom storage area";
        const _OrthancPluginRegisterStorageArea& p = 
          *reinterpret_cast<const _OrthancPluginRegisterStorageArea*>(parameters);
        
        if (pimpl_->storageArea_.get() == NULL)
        {
          pimpl_->storageArea_.reset(new StorageAreaFactory(plugin, p, GetErrorDictionary()));
        }
        else
        {
          throw OrthancException(ErrorCode_StorageAreaAlreadyRegistered);
        }

        return true;
      }

      case _OrthancPluginService_SetPluginProperty:
      {
        const _OrthancPluginSetPluginProperty& p = 
          *reinterpret_cast<const _OrthancPluginSetPluginProperty*>(parameters);
        pimpl_->properties_[std::make_pair(p.plugin, p.property)] = p.value;
        return true;
      }

      case _OrthancPluginService_SetGlobalProperty:
      {
        const _OrthancPluginGlobalProperty& p = 
          *reinterpret_cast<const _OrthancPluginGlobalProperty*>(parameters);
        if (p.property < 1024)
        {
          return false;
        }
        else
        {
          CheckContextAvailable();
          pimpl_->context_->GetIndex().SetGlobalProperty(static_cast<GlobalProperty>(p.property), p.value);
          return true;
        }
      }

      case _OrthancPluginService_GetGlobalProperty:
      {
        CheckContextAvailable();

        const _OrthancPluginGlobalProperty& p = 
          *reinterpret_cast<const _OrthancPluginGlobalProperty*>(parameters);
        std::string result = pimpl_->context_->GetIndex().GetGlobalProperty(static_cast<GlobalProperty>(p.property), p.value);
        *(p.result) = CopyString(result);
        return true;
      }

      case _OrthancPluginService_GetCommandLineArgumentsCount:
      {
        const _OrthancPluginReturnSingleValue& p =
          *reinterpret_cast<const _OrthancPluginReturnSingleValue*>(parameters);
        *(p.resultUint32) = pimpl_->argc_ - 1;
        return true;
      }

      case _OrthancPluginService_GetCommandLineArgument:
      {
        const _OrthancPluginGlobalProperty& p =
          *reinterpret_cast<const _OrthancPluginGlobalProperty*>(parameters);
        
        if (p.property + 1 > pimpl_->argc_)
        {
          return false;
        }
        else
        {
          std::string arg = std::string(pimpl_->argv_[p.property + 1]);
          *(p.result) = CopyString(arg);
          return true;
        }
      }

      case _OrthancPluginService_RegisterDatabaseBackend:
      {
        LOG(INFO) << "Plugin has registered a custom database back-end";

        const _OrthancPluginRegisterDatabaseBackend& p =
          *reinterpret_cast<const _OrthancPluginRegisterDatabaseBackend*>(parameters);

        if (pimpl_->database_.get() == NULL)
        {
          pimpl_->database_.reset(new OrthancPluginDatabase(plugin, GetErrorDictionary(), 
                                                            *p.backend, NULL, 0, p.payload));
        }
        else
        {
          throw OrthancException(ErrorCode_DatabaseBackendAlreadyRegistered);
        }

        *(p.result) = reinterpret_cast<OrthancPluginDatabaseContext*>(pimpl_->database_.get());

        return true;
      }

      case _OrthancPluginService_RegisterDatabaseBackendV2:
      {
        LOG(INFO) << "Plugin has registered a custom database back-end";

        const _OrthancPluginRegisterDatabaseBackendV2& p =
          *reinterpret_cast<const _OrthancPluginRegisterDatabaseBackendV2*>(parameters);

        if (pimpl_->database_.get() == NULL)
        {
          pimpl_->database_.reset(new OrthancPluginDatabase(plugin, GetErrorDictionary(),
                                                            *p.backend, p.extensions,
                                                            p.extensionsSize, p.payload));
        }
        else
        {
          throw OrthancException(ErrorCode_DatabaseBackendAlreadyRegistered);
        }

        *(p.result) = reinterpret_cast<OrthancPluginDatabaseContext*>(pimpl_->database_.get());

        return true;
      }

      case _OrthancPluginService_DatabaseAnswer:
        throw OrthancException(ErrorCode_InternalError);   // Implemented before locking (*)

      case _OrthancPluginService_GetExpectedDatabaseVersion:
      {
        const _OrthancPluginReturnSingleValue& p =
          *reinterpret_cast<const _OrthancPluginReturnSingleValue*>(parameters);
        *(p.resultUint32) = ORTHANC_DATABASE_VERSION;
        return true;
      }

      case _OrthancPluginService_StartMultipartAnswer:
      {
        const _OrthancPluginStartMultipartAnswer& p =
          *reinterpret_cast<const _OrthancPluginStartMultipartAnswer*>(parameters);
        HttpOutput* output = reinterpret_cast<HttpOutput*>(p.output);
        output->StartMultipart(p.subType, p.contentType);
        return true;
      }

      case _OrthancPluginService_SendMultipartItem:
        ApplySendMultipartItem(parameters);
        return true;

      case _OrthancPluginService_SendMultipartItem2:
        ApplySendMultipartItem2(parameters);
        return true;

      case _OrthancPluginService_ReadFile:
      {
        const _OrthancPluginReadFile& p =
          *reinterpret_cast<const _OrthancPluginReadFile*>(parameters);

        std::string content;
        Toolbox::ReadFile(content, p.path);
        CopyToMemoryBuffer(*p.target, content.size() > 0 ? content.c_str() : NULL, content.size());

        return true;
      }

      case _OrthancPluginService_WriteFile:
      {
        const _OrthancPluginWriteFile& p =
          *reinterpret_cast<const _OrthancPluginWriteFile*>(parameters);
        Toolbox::WriteFile(p.data, p.size, p.path);
        return true;
      }

      case _OrthancPluginService_GetErrorDescription:
      {
        const _OrthancPluginGetErrorDescription& p =
          *reinterpret_cast<const _OrthancPluginGetErrorDescription*>(parameters);
        *(p.target) = EnumerationToString(static_cast<ErrorCode>(p.error));
        return true;
      }

      case _OrthancPluginService_GetImagePixelFormat:
      {
        const _OrthancPluginGetImageInfo& p = *reinterpret_cast<const _OrthancPluginGetImageInfo*>(parameters);
        *(p.resultPixelFormat) = Plugins::Convert(reinterpret_cast<const ImageAccessor*>(p.image)->GetFormat());
        return true;
      }

      case _OrthancPluginService_GetImageWidth:
      {
        const _OrthancPluginGetImageInfo& p = *reinterpret_cast<const _OrthancPluginGetImageInfo*>(parameters);
        *(p.resultUint32) = reinterpret_cast<const ImageAccessor*>(p.image)->GetWidth();
        return true;
      }

      case _OrthancPluginService_GetImageHeight:
      {
        const _OrthancPluginGetImageInfo& p = *reinterpret_cast<const _OrthancPluginGetImageInfo*>(parameters);
        *(p.resultUint32) = reinterpret_cast<const ImageAccessor*>(p.image)->GetHeight();
        return true;
      }

      case _OrthancPluginService_GetImagePitch:
      {
        const _OrthancPluginGetImageInfo& p = *reinterpret_cast<const _OrthancPluginGetImageInfo*>(parameters);
        *(p.resultUint32) = reinterpret_cast<const ImageAccessor*>(p.image)->GetPitch();
        return true;
      }

      case _OrthancPluginService_GetImageBuffer:
      {
        const _OrthancPluginGetImageInfo& p = *reinterpret_cast<const _OrthancPluginGetImageInfo*>(parameters);
        *(p.resultBuffer) = reinterpret_cast<const ImageAccessor*>(p.image)->GetBuffer();
        return true;
      }

      case _OrthancPluginService_FreeImage:
      {
        const _OrthancPluginFreeImage& p = *reinterpret_cast<const _OrthancPluginFreeImage*>(parameters);
        if (p.image == NULL)
        {
          throw OrthancException(ErrorCode_ParameterOutOfRange);
        }
        else
        {
          delete reinterpret_cast<ImageAccessor*>(p.image);
          return true;
        }
      }

      case _OrthancPluginService_UncompressImage:
        UncompressImage(parameters);
        return true;

      case _OrthancPluginService_CompressImage:
        CompressImage(parameters);
        return true;

      case _OrthancPluginService_CallHttpClient:
        CallHttpClient(parameters);
        return true;

      case _OrthancPluginService_ConvertPixelFormat:
        ConvertPixelFormat(parameters);
        return true;

      case _OrthancPluginService_GetFontsCount:
      {
        const _OrthancPluginReturnSingleValue& p =
          *reinterpret_cast<const _OrthancPluginReturnSingleValue*>(parameters);
        *(p.resultUint32) = Configuration::GetFontRegistry().GetSize();
        return true;
      }

      case _OrthancPluginService_GetFontInfo:
        GetFontInfo(parameters);
        return true;

      case _OrthancPluginService_DrawText:
        DrawText(parameters);
        return true;

      case _OrthancPluginService_StorageAreaCreate:
      {
        const _OrthancPluginStorageAreaCreate& p =
          *reinterpret_cast<const _OrthancPluginStorageAreaCreate*>(parameters);
        IStorageArea& storage = *reinterpret_cast<IStorageArea*>(p.storageArea);
        storage.Create(p.uuid, p.content, p.size, Plugins::Convert(p.type));
        return true;
      }

      case _OrthancPluginService_StorageAreaRead:
      {
        const _OrthancPluginStorageAreaRead& p =
          *reinterpret_cast<const _OrthancPluginStorageAreaRead*>(parameters);
        IStorageArea& storage = *reinterpret_cast<IStorageArea*>(p.storageArea);
        std::string content;
        storage.Read(content, p.uuid, Plugins::Convert(p.type));
        CopyToMemoryBuffer(*p.target, content);
        return true;
      }

      case _OrthancPluginService_StorageAreaRemove:
      {
        const _OrthancPluginStorageAreaRemove& p =
          *reinterpret_cast<const _OrthancPluginStorageAreaRemove*>(parameters);
        IStorageArea& storage = *reinterpret_cast<IStorageArea*>(p.storageArea);
        storage.Remove(p.uuid, Plugins::Convert(p.type));
        return true;
      }

      case _OrthancPluginService_RegisterErrorCode:
      {
        const _OrthancPluginRegisterErrorCode& p =
          *reinterpret_cast<const _OrthancPluginRegisterErrorCode*>(parameters);
        *(p.target) = pimpl_->dictionary_.Register(plugin, p.code, p.httpStatus, p.message);
        return true;
      }

      case _OrthancPluginService_RegisterDictionaryTag:
      {
        const _OrthancPluginRegisterDictionaryTag& p =
          *reinterpret_cast<const _OrthancPluginRegisterDictionaryTag*>(parameters);
        FromDcmtkBridge::RegisterDictionaryTag(DicomTag(p.group, p.element),
                                               Plugins::Convert(p.vr), p.name,
                                               p.minMultiplicity, p.maxMultiplicity);
        return true;
      }

      case _OrthancPluginService_ReconstructMainDicomTags:
      {
        const _OrthancPluginReconstructMainDicomTags& p =
          *reinterpret_cast<const _OrthancPluginReconstructMainDicomTags*>(parameters);

        if (pimpl_->database_.get() == NULL)
        {
          LOG(ERROR) << "The service ReconstructMainDicomTags can only be invoked by custom database plugins";
          throw OrthancException(ErrorCode_DatabasePlugin);
        }

        IStorageArea& storage = *reinterpret_cast<IStorageArea*>(p.storageArea);
        Toolbox::ReconstructMainDicomTags(*pimpl_->database_, storage, Plugins::Convert(p.level));

        return true;
      }

      case _OrthancPluginService_DicomBufferToJson:
      case _OrthancPluginService_DicomInstanceToJson:
        ApplyDicomToJson(service, parameters);
        return true;

      case _OrthancPluginService_CreateDicom:
        ApplyCreateDicom(service, parameters);
        return true;

      case _OrthancPluginService_WorklistAddAnswer:
      {
        const _OrthancPluginWorklistAnswersOperation& p =
          *reinterpret_cast<const _OrthancPluginWorklistAnswersOperation*>(parameters);
        reinterpret_cast<const WorklistHandler*>(p.query)->AddAnswer(p.answers, p.dicom, p.size);
        return true;
      }

      case _OrthancPluginService_WorklistMarkIncomplete:
      {
        const _OrthancPluginWorklistAnswersOperation& p =
          *reinterpret_cast<const _OrthancPluginWorklistAnswersOperation*>(parameters);
        reinterpret_cast<DicomFindAnswers*>(p.answers)->SetComplete(false);
        return true;
      }

      case _OrthancPluginService_WorklistIsMatch:
      {
        const _OrthancPluginWorklistQueryOperation& p =
          *reinterpret_cast<const _OrthancPluginWorklistQueryOperation*>(parameters);
        *p.isMatch = reinterpret_cast<const WorklistHandler*>(p.query)->IsMatch(p.dicom, p.size);
        return true;
      }

      case _OrthancPluginService_WorklistGetDicomQuery:
      {
        const _OrthancPluginWorklistQueryOperation& p =
          *reinterpret_cast<const _OrthancPluginWorklistQueryOperation*>(parameters);
        reinterpret_cast<const WorklistHandler*>(p.query)->GetDicomQuery(*p.target);
        return true;
      }

      case _OrthancPluginService_CreateImage:
      case _OrthancPluginService_CreateImageAccessor:
      case _OrthancPluginService_DecodeDicomImage:
        ApplyCreateImage(service, parameters);
        return true;

      case _OrthancPluginService_ComputeMd5:
      case _OrthancPluginService_ComputeSha1:
        ComputeHash(service, parameters);
        return true;

      case _OrthancPluginService_LookupDictionary:
        ApplyLookupDictionary(parameters);
        return true;

      default:
      {
        // This service is unknown to the Orthanc plugin engine
        return false;
      }
    }
  }


  bool OrthancPlugins::HasStorageArea() const
  {
    return pimpl_->storageArea_.get() != NULL;
  }
  
  bool OrthancPlugins::HasDatabaseBackend() const
  {
    return pimpl_->database_.get() != NULL;
  }


  IStorageArea* OrthancPlugins::CreateStorageArea()
  {
    if (!HasStorageArea())
    {
      throw OrthancException(ErrorCode_BadSequenceOfCalls);
    }
    else
    {
      return pimpl_->storageArea_->Create();
    }
  }


  const SharedLibrary& OrthancPlugins::GetStorageAreaLibrary() const
  {
    if (!HasStorageArea())
    {
      throw OrthancException(ErrorCode_BadSequenceOfCalls);
    }
    else
    {
      return pimpl_->storageArea_->GetSharedLibrary();
    }
  }


  IDatabaseWrapper& OrthancPlugins::GetDatabaseBackend()
  {
    if (!HasDatabaseBackend())
    {
      throw OrthancException(ErrorCode_BadSequenceOfCalls);
    }
    else
    {
      return *pimpl_->database_;
    }
  }


  const SharedLibrary& OrthancPlugins::GetDatabaseBackendLibrary() const
  {
    if (!HasDatabaseBackend())
    {
      throw OrthancException(ErrorCode_BadSequenceOfCalls);
    }
    else
    {
      return pimpl_->database_->GetSharedLibrary();
    }
  }


  const char* OrthancPlugins::GetProperty(const char* plugin,
                                          _OrthancPluginProperty property) const
  {
    PImpl::Property p = std::make_pair(plugin, property);
    PImpl::Properties::const_iterator it = pimpl_->properties_.find(p);

    if (it == pimpl_->properties_.end())
    {
      return NULL;
    }
    else
    {
      return it->second.c_str();
    }
  }


  void OrthancPlugins::SetCommandLineArguments(int argc, char* argv[])
  {
    if (argc < 1 || argv == NULL)
    {
      throw OrthancException(ErrorCode_ParameterOutOfRange);
    }

    pimpl_->argc_ = argc;
    pimpl_->argv_ = argv;
  }


  PluginsManager& OrthancPlugins::GetManager()
  {
    return pimpl_->manager_;
  }


  const PluginsManager& OrthancPlugins::GetManager() const
  {
    return pimpl_->manager_;
  }


  PluginsErrorDictionary&  OrthancPlugins::GetErrorDictionary()
  {
    return pimpl_->dictionary_;
  }


  IWorklistRequestHandler* OrthancPlugins::ConstructWorklistRequestHandler()
  {
    if (HasWorklistHandler())
    {
      return new WorklistHandler(*this);
    }
    else
    {
      return NULL;
    }
  }


  bool OrthancPlugins::HasWorklistHandler()
  {
    boost::mutex::scoped_lock lock(pimpl_->worklistCallbackMutex_);
    return pimpl_->worklistCallback_ != NULL;
  }


  bool OrthancPlugins::HasCustomImageDecoder()
  {
    boost::mutex::scoped_lock lock(pimpl_->decodeImageCallbackMutex_);
    return (pimpl_->decodeImageCallback_ != NULL);
  }


  ImageAccessor*  OrthancPlugins::Decode(const void* dicom,
                                         size_t size,
                                         unsigned int frame)
  {
    {
      boost::mutex::scoped_lock lock(pimpl_->decodeImageCallbackMutex_);
      if (pimpl_->decodeImageCallback_ != NULL)
      {
        OrthancPluginImage* pluginImage = NULL;
        if (pimpl_->decodeImageCallback_(&pluginImage, dicom, size, frame) == OrthancPluginErrorCode_Success &&
            pluginImage != NULL)
        {
          return reinterpret_cast<ImageAccessor*>(pluginImage);
        }

        LOG(WARNING) << "The custom image decoder cannot handle an image, fallback to the built-in decoder";
      }
    }

    DefaultDicomImageDecoder defaultDecoder;
    return defaultDecoder.Decode(dicom, size, frame);  // TODO RETURN NULL ???
  }
}
