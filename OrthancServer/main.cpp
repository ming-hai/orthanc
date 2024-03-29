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
#include "OrthancRestApi/OrthancRestApi.h"

#include <fstream>
#include <boost/algorithm/string/predicate.hpp>

#include "../Core/Logging.h"
#include "../Core/Uuid.h"
#include "../Core/HttpServer/EmbeddedResourceHttpHandler.h"
#include "../Core/HttpServer/FilesystemHttpHandler.h"
#include "../Core/Lua/LuaFunctionCall.h"
#include "../Core/DicomFormat/DicomArray.h"
#include "DicomProtocol/DicomServer.h"
#include "DicomProtocol/ReusableDicomUserConnection.h"
#include "OrthancInitialization.h"
#include "ServerContext.h"
#include "OrthancFindRequestHandler.h"
#include "OrthancMoveRequestHandler.h"
#include "ServerToolbox.h"
#include "../Plugins/Engine/OrthancPlugins.h"
#include "FromDcmtkBridge.h"

using namespace Orthanc;


class OrthancStoreRequestHandler : public IStoreRequestHandler
{
private:
  ServerContext& server_;

public:
  OrthancStoreRequestHandler(ServerContext& context) :
    server_(context)
  {
  }


  virtual void Handle(const std::string& dicomFile,
                      const DicomMap& dicomSummary,
                      const Json::Value& dicomJson,
                      const std::string& remoteIp,
                      const std::string& remoteAet,
                      const std::string& calledAet) 
  {
    if (dicomFile.size() > 0)
    {
      DicomInstanceToStore toStore;
      toStore.SetDicomProtocolOrigin(remoteIp.c_str(), remoteAet.c_str(), calledAet.c_str());
      toStore.SetBuffer(dicomFile);
      toStore.SetSummary(dicomSummary);
      toStore.SetJson(dicomJson);

      std::string id;
      server_.Store(id, toStore);
    }
  }
};



class MyDicomServerFactory : 
  public IStoreRequestHandlerFactory,
  public IFindRequestHandlerFactory, 
  public IMoveRequestHandlerFactory
{
private:
  ServerContext& context_;

public:
  MyDicomServerFactory(ServerContext& context) : context_(context)
  {
  }

  virtual IStoreRequestHandler* ConstructStoreRequestHandler()
  {
    return new OrthancStoreRequestHandler(context_);
  }

  virtual IFindRequestHandler* ConstructFindRequestHandler()
  {
    std::auto_ptr<OrthancFindRequestHandler> result(new OrthancFindRequestHandler(context_));

    result->SetMaxResults(Configuration::GetGlobalIntegerParameter("LimitFindResults", 0));
    result->SetMaxInstances(Configuration::GetGlobalIntegerParameter("LimitFindInstances", 0));

    if (result->GetMaxResults() == 0)
    {
      LOG(INFO) << "No limit on the number of C-FIND results at the Patient, Study and Series levels";
    }
    else
    {
      LOG(INFO) << "Maximum " << result->GetMaxResults() 
                << " results for C-FIND queries at the Patient, Study and Series levels";
    }

    if (result->GetMaxInstances() == 0)
    {
      LOG(INFO) << "No limit on the number of C-FIND results at the Instance level";
    }
    else
    {
      LOG(INFO) << "Maximum " << result->GetMaxInstances() 
                << " instances will be returned for C-FIND queries at the Instance level";
    }

    return result.release();
  }

  virtual IMoveRequestHandler* ConstructMoveRequestHandler()
  {
    return new OrthancMoveRequestHandler(context_);
  }

  void Done()
  {
  }
};


class OrthancApplicationEntityFilter : public IApplicationEntityFilter
{
private:
  ServerContext& context_;

public:
  OrthancApplicationEntityFilter(ServerContext& context) : context_(context)
  {
  }

  virtual bool IsAllowedConnection(const std::string& /*remoteIp*/,
                                   const std::string& /*remoteAet*/,
                                   const std::string& /*calledAet*/)
  {
    return true;
  }

  virtual bool IsAllowedRequest(const std::string& /*remoteIp*/,
                                const std::string& remoteAet,
                                const std::string& /*calledAet*/,
                                DicomRequestType type)
  {
    if (type == DicomRequestType_Store)
    {
      // Incoming store requests are always accepted, even from unknown AET
      return true;
    }

    if (!Configuration::IsKnownAETitle(remoteAet))
    {
      LOG(ERROR) << "Unknown remote DICOM modality AET: \"" << remoteAet << "\"";
      return false;
    }
    else
    {
      return true;
    }
  }

  virtual bool IsAllowedTransferSyntax(const std::string& remoteIp,
                                       const std::string& remoteAet,
                                       const std::string& calledAet,
                                       TransferSyntax syntax)
  {
    std::string configuration;

    switch (syntax)
    {
      case TransferSyntax_Deflated:
        configuration = "DeflatedTransferSyntaxAccepted";
        break;

      case TransferSyntax_Jpeg:
        configuration = "JpegTransferSyntaxAccepted";
        break;

      case TransferSyntax_Jpeg2000:
        configuration = "Jpeg2000TransferSyntaxAccepted";
        break;

      case TransferSyntax_JpegLossless:
        configuration = "JpegLosslessTransferSyntaxAccepted";
        break;

      case TransferSyntax_Jpip:
        configuration = "JpipTransferSyntaxAccepted";
        break;

      case TransferSyntax_Mpeg2:
        configuration = "Mpeg2TransferSyntaxAccepted";
        break;

      case TransferSyntax_Rle:
        configuration = "RleTransferSyntaxAccepted";
        break;

      default: 
        throw OrthancException(ErrorCode_ParameterOutOfRange);
    }

    {
      std::string lua = "Is" + configuration;

      LuaScripting::Locker locker(context_.GetLua());
      
      if (locker.GetLua().IsExistingFunction(lua.c_str()))
      {
        LuaFunctionCall call(locker.GetLua(), lua.c_str());
        call.PushString(remoteAet);
        call.PushString(remoteIp);
        call.PushString(calledAet);
        return call.ExecutePredicate();
      }
    }

    return Configuration::GetGlobalBoolParameter(configuration, true);
  }


  virtual bool IsUnknownSopClassAccepted(const std::string& remoteIp,
                                         const std::string& remoteAet,
                                         const std::string& calledAet)
  {
    static const char* configuration = "UnknownSopClassAccepted";

    {
      std::string lua = "Is" + std::string(configuration);

      LuaScripting::Locker locker(context_.GetLua());
      
      if (locker.GetLua().IsExistingFunction(lua.c_str()))
      {
        LuaFunctionCall call(locker.GetLua(), lua.c_str());
        call.PushString(remoteAet);
        call.PushString(remoteIp);
        call.PushString(calledAet);
        return call.ExecutePredicate();
      }
    }

    return Configuration::GetGlobalBoolParameter(configuration, false);
  }
};


class MyIncomingHttpRequestFilter : public IIncomingHttpRequestFilter
{
private:
  ServerContext& context_;

public:
  MyIncomingHttpRequestFilter(ServerContext& context) : context_(context)
  {
  }

  virtual bool IsAllowed(HttpMethod method,
                         const char* uri,
                         const char* ip,
                         const char* username) const
  {
    static const char* HTTP_FILTER = "IncomingHttpRequestFilter";

    LuaScripting::Locker locker(context_.GetLua());

    // Test if the instance must be filtered out
    if (locker.GetLua().IsExistingFunction(HTTP_FILTER))
    {
      LuaFunctionCall call(locker.GetLua(), HTTP_FILTER);

      switch (method)
      {
        case HttpMethod_Get:
          call.PushString("GET");
          break;

        case HttpMethod_Put:
          call.PushString("PUT");
          break;

        case HttpMethod_Post:
          call.PushString("POST");
          break;

        case HttpMethod_Delete:
          call.PushString("DELETE");
          break;

        default:
          return true;
      }

      call.PushString(uri);
      call.PushString(ip);
      call.PushString(username);

      if (!call.ExecutePredicate())
      {
        LOG(INFO) << "An incoming HTTP request has been discarded by the filter";
        return false;
      }
    }

    return true;
  }
};



class MyHttpExceptionFormatter : public IHttpExceptionFormatter
{
private:
  bool             describeErrors_;
  OrthancPlugins*  plugins_;

public:
  MyHttpExceptionFormatter(bool describeErrors,
                           OrthancPlugins* plugins) :
    describeErrors_(describeErrors),
    plugins_(plugins)
  {
  }

  virtual void Format(HttpOutput& output,
                      const OrthancException& exception,
                      HttpMethod method,
                      const char* uri)
  {
    {
      bool isPlugin = false;

#if ORTHANC_PLUGINS_ENABLED == 1
      if (plugins_ != NULL)
      {
        plugins_->GetErrorDictionary().LogError(exception.GetErrorCode(), true);
        isPlugin = true;
      }
#endif

      if (!isPlugin)
      {
        LOG(ERROR) << "Exception in the HTTP handler: " << exception.What();
      }
    }      

    Json::Value message = Json::objectValue;
    ErrorCode errorCode = exception.GetErrorCode();
    HttpStatus httpStatus = exception.GetHttpStatus();

    {
      bool isPlugin = false;

#if ORTHANC_PLUGINS_ENABLED == 1
      if (plugins_ != NULL &&
          plugins_->GetErrorDictionary().Format(message, httpStatus, exception))
      {
        errorCode = ErrorCode_Plugin;
        isPlugin = true;
      }
#endif

      if (!isPlugin)
      {
        message["Message"] = exception.What();
      }
    }

    if (!describeErrors_)
    {
      output.SendStatus(httpStatus);
    }
    else
    {
      message["Method"] = EnumerationToString(method);
      message["Uri"] = uri;
      message["HttpError"] = EnumerationToString(httpStatus);
      message["HttpStatus"] = httpStatus;
      message["OrthancError"] = EnumerationToString(errorCode);
      message["OrthancStatus"] = errorCode;

      std::string info = message.toStyledString();
      output.SendStatus(httpStatus, info);
    }
  }
};



static void PrintHelp(const char* path)
{
  std::cout 
    << "Usage: " << path << " [OPTION]... [CONFIGURATION]" << std::endl
    << "Orthanc, lightweight, RESTful DICOM server for healthcare and medical research." << std::endl
    << std::endl
    << "The \"CONFIGURATION\" argument can be a single file or a directory. In the " << std::endl
    << "case of a directory, all the JSON files it contains will be merged. " << std::endl
    << "If no configuration path is given on the command line, a set of default " << std::endl
    << "parameters is used. Please refer to the Orthanc homepage for the full " << std::endl
    << "instructions about how to use Orthanc <http://www.orthanc-server.com/>." << std::endl
    << std::endl
    << "Command-line options:" << std::endl
    << "  --help\t\tdisplay this help and exit" << std::endl
    << "  --logdir=[dir]\tdirectory where to store the log files" << std::endl
    << "\t\t\t(if not used, the logs are dumped to stderr)" << std::endl
    << "  --config=[file]\tcreate a sample configuration file and exit" << std::endl
    << "  --errors\t\tprint the supported error codes and exit" << std::endl
    << "  --verbose\t\tbe verbose in logs" << std::endl
    << "  --trace\t\thighest verbosity in logs (for debug)" << std::endl
    << "  --upgrade\t\tallow Orthanc to upgrade the version of the" << std::endl
    << "\t\t\tdatabase (beware that the database will become" << std::endl
    << "\t\t\tincompatible with former versions of Orthanc)" << std::endl
    << "  --version\t\toutput version information and exit" << std::endl
    << std::endl
    << "Exit status:" << std::endl
    << "   0 if success," << std::endl
#if defined(_WIN32)
    << "!= 0 if error (use the --errors option to get the list of possible errors)." << std::endl
#else
    << "  -1 if error (have a look at the logs)." << std::endl
#endif
    << std::endl;
}


static void PrintVersion(const char* path)
{
  std::cout
    << path << " " << ORTHANC_VERSION << std::endl
    << "Copyright (C) 2012-2016 Sebastien Jodogne, Medical Physics Department, University Hospital of Liege (Belgium)" << std::endl
    << "Licensing GPLv3+: GNU GPL version 3 or later <http://gnu.org/licenses/gpl.html>, with OpenSSL exception." << std::endl
    << "This is free software: you are free to change and redistribute it." << std::endl
    << "There is NO WARRANTY, to the extent permitted by law." << std::endl
    << std::endl
    << "Written by Sebastien Jodogne <s.jodogne@gmail.com>" << std::endl;
}


static void PrintErrorCode(ErrorCode code, const char* description)
{
  std::cout 
    << std::right << std::setw(16) 
    << static_cast<int>(code)
    << "   " << description << std::endl;
}


static void PrintErrors(const char* path)
{
  std::cout
    << path << " " << ORTHANC_VERSION << std::endl
    << "Orthanc, lightweight, RESTful DICOM server for healthcare and medical research." 
    << std::endl << std::endl
    << "List of error codes that could be returned by Orthanc:" 
    << std::endl << std::endl;

  // The content of the following brackets is automatically generated
  // by the "GenerateErrorCodes.py" script
  {
    PrintErrorCode(ErrorCode_InternalError, "Internal error");
    PrintErrorCode(ErrorCode_Success, "Success");
    PrintErrorCode(ErrorCode_Plugin, "Error encountered within the plugin engine");
    PrintErrorCode(ErrorCode_NotImplemented, "Not implemented yet");
    PrintErrorCode(ErrorCode_ParameterOutOfRange, "Parameter out of range");
    PrintErrorCode(ErrorCode_NotEnoughMemory, "Not enough memory");
    PrintErrorCode(ErrorCode_BadParameterType, "Bad type for a parameter");
    PrintErrorCode(ErrorCode_BadSequenceOfCalls, "Bad sequence of calls");
    PrintErrorCode(ErrorCode_InexistentItem, "Accessing an inexistent item");
    PrintErrorCode(ErrorCode_BadRequest, "Bad request");
    PrintErrorCode(ErrorCode_NetworkProtocol, "Error in the network protocol");
    PrintErrorCode(ErrorCode_SystemCommand, "Error while calling a system command");
    PrintErrorCode(ErrorCode_Database, "Error with the database engine");
    PrintErrorCode(ErrorCode_UriSyntax, "Badly formatted URI");
    PrintErrorCode(ErrorCode_InexistentFile, "Inexistent file");
    PrintErrorCode(ErrorCode_CannotWriteFile, "Cannot write to file");
    PrintErrorCode(ErrorCode_BadFileFormat, "Bad file format");
    PrintErrorCode(ErrorCode_Timeout, "Timeout");
    PrintErrorCode(ErrorCode_UnknownResource, "Unknown resource");
    PrintErrorCode(ErrorCode_IncompatibleDatabaseVersion, "Incompatible version of the database");
    PrintErrorCode(ErrorCode_FullStorage, "The file storage is full");
    PrintErrorCode(ErrorCode_CorruptedFile, "Corrupted file (e.g. inconsistent MD5 hash)");
    PrintErrorCode(ErrorCode_InexistentTag, "Inexistent tag");
    PrintErrorCode(ErrorCode_ReadOnly, "Cannot modify a read-only data structure");
    PrintErrorCode(ErrorCode_IncompatibleImageFormat, "Incompatible format of the images");
    PrintErrorCode(ErrorCode_IncompatibleImageSize, "Incompatible size of the images");
    PrintErrorCode(ErrorCode_SharedLibrary, "Error while using a shared library (plugin)");
    PrintErrorCode(ErrorCode_UnknownPluginService, "Plugin invoking an unknown service");
    PrintErrorCode(ErrorCode_UnknownDicomTag, "Unknown DICOM tag");
    PrintErrorCode(ErrorCode_BadJson, "Cannot parse a JSON document");
    PrintErrorCode(ErrorCode_Unauthorized, "Bad credentials were provided to an HTTP request");
    PrintErrorCode(ErrorCode_BadFont, "Badly formatted font file");
    PrintErrorCode(ErrorCode_DatabasePlugin, "The plugin implementing a custom database back-end does not fulfill the proper interface");
    PrintErrorCode(ErrorCode_StorageAreaPlugin, "Error in the plugin implementing a custom storage area");
    PrintErrorCode(ErrorCode_EmptyRequest, "The request is empty");
    PrintErrorCode(ErrorCode_NotAcceptable, "Cannot send a response which is acceptable according to the Accept HTTP header");
    PrintErrorCode(ErrorCode_SQLiteNotOpened, "SQLite: The database is not opened");
    PrintErrorCode(ErrorCode_SQLiteAlreadyOpened, "SQLite: Connection is already open");
    PrintErrorCode(ErrorCode_SQLiteCannotOpen, "SQLite: Unable to open the database");
    PrintErrorCode(ErrorCode_SQLiteStatementAlreadyUsed, "SQLite: This cached statement is already being referred to");
    PrintErrorCode(ErrorCode_SQLiteExecute, "SQLite: Cannot execute a command");
    PrintErrorCode(ErrorCode_SQLiteRollbackWithoutTransaction, "SQLite: Rolling back a nonexistent transaction (have you called Begin()?)");
    PrintErrorCode(ErrorCode_SQLiteCommitWithoutTransaction, "SQLite: Committing a nonexistent transaction");
    PrintErrorCode(ErrorCode_SQLiteRegisterFunction, "SQLite: Unable to register a function");
    PrintErrorCode(ErrorCode_SQLiteFlush, "SQLite: Unable to flush the database");
    PrintErrorCode(ErrorCode_SQLiteCannotRun, "SQLite: Cannot run a cached statement");
    PrintErrorCode(ErrorCode_SQLiteCannotStep, "SQLite: Cannot step over a cached statement");
    PrintErrorCode(ErrorCode_SQLiteBindOutOfRange, "SQLite: Bing a value while out of range (serious error)");
    PrintErrorCode(ErrorCode_SQLitePrepareStatement, "SQLite: Cannot prepare a cached statement");
    PrintErrorCode(ErrorCode_SQLiteTransactionAlreadyStarted, "SQLite: Beginning the same transaction twice");
    PrintErrorCode(ErrorCode_SQLiteTransactionCommit, "SQLite: Failure when committing the transaction");
    PrintErrorCode(ErrorCode_SQLiteTransactionBegin, "SQLite: Cannot start a transaction");
    PrintErrorCode(ErrorCode_DirectoryOverFile, "The directory to be created is already occupied by a regular file");
    PrintErrorCode(ErrorCode_FileStorageCannotWrite, "Unable to create a subdirectory or a file in the file storage");
    PrintErrorCode(ErrorCode_DirectoryExpected, "The specified path does not point to a directory");
    PrintErrorCode(ErrorCode_HttpPortInUse, "The TCP port of the HTTP server is already in use");
    PrintErrorCode(ErrorCode_DicomPortInUse, "The TCP port of the DICOM server is already in use");
    PrintErrorCode(ErrorCode_BadHttpStatusInRest, "This HTTP status is not allowed in a REST API");
    PrintErrorCode(ErrorCode_RegularFileExpected, "The specified path does not point to a regular file");
    PrintErrorCode(ErrorCode_PathToExecutable, "Unable to get the path to the executable");
    PrintErrorCode(ErrorCode_MakeDirectory, "Cannot create a directory");
    PrintErrorCode(ErrorCode_BadApplicationEntityTitle, "An application entity title (AET) cannot be empty or be longer than 16 characters");
    PrintErrorCode(ErrorCode_NoCFindHandler, "No request handler factory for DICOM C-FIND SCP");
    PrintErrorCode(ErrorCode_NoCMoveHandler, "No request handler factory for DICOM C-MOVE SCP");
    PrintErrorCode(ErrorCode_NoCStoreHandler, "No request handler factory for DICOM C-STORE SCP");
    PrintErrorCode(ErrorCode_NoApplicationEntityFilter, "No application entity filter");
    PrintErrorCode(ErrorCode_NoSopClassOrInstance, "DicomUserConnection: Unable to find the SOP class and instance");
    PrintErrorCode(ErrorCode_NoPresentationContext, "DicomUserConnection: No acceptable presentation context for modality");
    PrintErrorCode(ErrorCode_DicomFindUnavailable, "DicomUserConnection: The C-FIND command is not supported by the remote SCP");
    PrintErrorCode(ErrorCode_DicomMoveUnavailable, "DicomUserConnection: The C-MOVE command is not supported by the remote SCP");
    PrintErrorCode(ErrorCode_CannotStoreInstance, "Cannot store an instance");
    PrintErrorCode(ErrorCode_CreateDicomNotString, "Only string values are supported when creating DICOM instances");
    PrintErrorCode(ErrorCode_CreateDicomOverrideTag, "Trying to override a value inherited from a parent module");
    PrintErrorCode(ErrorCode_CreateDicomUseContent, "Use \"Content\" to inject an image into a new DICOM instance");
    PrintErrorCode(ErrorCode_CreateDicomNoPayload, "No payload is present for one instance in the series");
    PrintErrorCode(ErrorCode_CreateDicomUseDataUriScheme, "The payload of the DICOM instance must be specified according to Data URI scheme");
    PrintErrorCode(ErrorCode_CreateDicomBadParent, "Trying to attach a new DICOM instance to an inexistent resource");
    PrintErrorCode(ErrorCode_CreateDicomParentIsInstance, "Trying to attach a new DICOM instance to an instance (must be a series, study or patient)");
    PrintErrorCode(ErrorCode_CreateDicomParentEncoding, "Unable to get the encoding of the parent resource");
    PrintErrorCode(ErrorCode_UnknownModality, "Unknown modality");
    PrintErrorCode(ErrorCode_BadJobOrdering, "Bad ordering of filters in a job");
    PrintErrorCode(ErrorCode_JsonToLuaTable, "Cannot convert the given JSON object to a Lua table");
    PrintErrorCode(ErrorCode_CannotCreateLua, "Cannot create the Lua context");
    PrintErrorCode(ErrorCode_CannotExecuteLua, "Cannot execute a Lua command");
    PrintErrorCode(ErrorCode_LuaAlreadyExecuted, "Arguments cannot be pushed after the Lua function is executed");
    PrintErrorCode(ErrorCode_LuaBadOutput, "The Lua function does not give the expected number of outputs");
    PrintErrorCode(ErrorCode_NotLuaPredicate, "The Lua function is not a predicate (only true/false outputs allowed)");
    PrintErrorCode(ErrorCode_LuaReturnsNoString, "The Lua function does not return a string");
    PrintErrorCode(ErrorCode_StorageAreaAlreadyRegistered, "Another plugin has already registered a custom storage area");
    PrintErrorCode(ErrorCode_DatabaseBackendAlreadyRegistered, "Another plugin has already registered a custom database back-end");
    PrintErrorCode(ErrorCode_DatabaseNotInitialized, "Plugin trying to call the database during its initialization");
    PrintErrorCode(ErrorCode_SslDisabled, "Orthanc has been built without SSL support");
    PrintErrorCode(ErrorCode_CannotOrderSlices, "Unable to order the slices of the series");
    PrintErrorCode(ErrorCode_NoWorklistHandler, "No request handler factory for DICOM C-Find Modality SCP");
  }

  std::cout << std::endl;
}



static void LoadLuaScripts(ServerContext& context)
{
  std::list<std::string> luaScripts;
  Configuration::GetGlobalListOfStringsParameter(luaScripts, "LuaScripts");
  for (std::list<std::string>::const_iterator
         it = luaScripts.begin(); it != luaScripts.end(); ++it)
  {
    std::string path = Configuration::InterpretStringParameterAsPath(*it);
    LOG(WARNING) << "Installing the Lua scripts from: " << path;
    std::string script;
    Toolbox::ReadFile(script, path);

    LuaScripting::Locker locker(context.GetLua());
    locker.GetLua().Execute(script);
  }
}



#if ORTHANC_PLUGINS_ENABLED == 1
static void LoadPlugins(OrthancPlugins& plugins)
{
  std::list<std::string> path;
  Configuration::GetGlobalListOfStringsParameter(path, "Plugins");
  for (std::list<std::string>::const_iterator
         it = path.begin(); it != path.end(); ++it)
  {
    std::string path = Configuration::InterpretStringParameterAsPath(*it);
    LOG(WARNING) << "Loading plugin(s) from: " << path;
    plugins.GetManager().RegisterPlugin(path);
  }  
}
#endif



// Returns "true" if restart is required
static bool WaitForExit(ServerContext& context,
                        OrthancRestApi& restApi)
{
  LOG(WARNING) << "Orthanc has started";

#if ORTHANC_PLUGINS_ENABLED == 1
  if (context.HasPlugins())
  {
    context.GetPlugins().SignalOrthancStarted();
  }
#endif

  context.GetLua().Execute("Initialize");

  Toolbox::ServerBarrier(restApi.LeaveBarrierFlag());
  bool restart = restApi.IsResetRequestReceived();

  context.GetLua().Execute("Finalize");

#if ORTHANC_PLUGINS_ENABLED == 1
  if (context.HasPlugins())
  {
    context.GetPlugins().SignalOrthancStopped();
  }
#endif

  if (restart)
  {
    LOG(WARNING) << "Reset request received, restarting Orthanc";
  }

  // We're done
  LOG(WARNING) << "Orthanc is stopping";

  return restart;
}



static bool StartHttpServer(ServerContext& context,
                            OrthancRestApi& restApi,
                            OrthancPlugins* plugins)
{
  if (!Configuration::GetGlobalBoolParameter("HttpServerEnabled", true))
  {
    LOG(WARNING) << "The HTTP server is disabled";
    return WaitForExit(context, restApi);
  }

  MyHttpExceptionFormatter exceptionFormatter(Configuration::GetGlobalBoolParameter("HttpDescribeErrors", true), plugins);
  

  // HTTP server
  MyIncomingHttpRequestFilter httpFilter(context);
  MongooseServer httpServer;
  httpServer.SetPortNumber(Configuration::GetGlobalIntegerParameter("HttpPort", 8042));
  httpServer.SetRemoteAccessAllowed(Configuration::GetGlobalBoolParameter("RemoteAccessAllowed", false));
  httpServer.SetKeepAliveEnabled(Configuration::GetGlobalBoolParameter("KeepAlive", false));
  httpServer.SetHttpCompressionEnabled(Configuration::GetGlobalBoolParameter("HttpCompressionEnabled", true));
  httpServer.SetIncomingHttpRequestFilter(httpFilter);
  httpServer.SetHttpExceptionFormatter(exceptionFormatter);

  httpServer.SetAuthenticationEnabled(Configuration::GetGlobalBoolParameter("AuthenticationEnabled", false));
  Configuration::SetupRegisteredUsers(httpServer);

  if (Configuration::GetGlobalBoolParameter("SslEnabled", false))
  {
    std::string certificate = Configuration::InterpretStringParameterAsPath(
      Configuration::GetGlobalStringParameter("SslCertificate", "certificate.pem"));
    httpServer.SetSslEnabled(true);
    httpServer.SetSslCertificate(certificate.c_str());
  }
  else
  {
    httpServer.SetSslEnabled(false);
  }

  httpServer.Register(context.GetHttpHandler());

  httpServer.Start();
  LOG(WARNING) << "HTTP server listening on port: " << httpServer.GetPortNumber();
  
  bool restart = WaitForExit(context, restApi);

  httpServer.Stop();
  LOG(WARNING) << "    HTTP server has stopped";

  return restart;
}


static bool StartDicomServer(ServerContext& context,
                             OrthancRestApi& restApi,
                             OrthancPlugins* plugins)
{
  if (!Configuration::GetGlobalBoolParameter("DicomServerEnabled", true))
  {
    LOG(WARNING) << "The DICOM server is disabled";
    return StartHttpServer(context, restApi, plugins);
  }

  MyDicomServerFactory serverFactory(context);

  // DICOM server
  DicomServer dicomServer;
  OrthancApplicationEntityFilter dicomFilter(context);
  dicomServer.SetCalledApplicationEntityTitleCheck(Configuration::GetGlobalBoolParameter("DicomCheckCalledAet", false));
  dicomServer.SetStoreRequestHandlerFactory(serverFactory);
  dicomServer.SetMoveRequestHandlerFactory(serverFactory);
  dicomServer.SetFindRequestHandlerFactory(serverFactory);

#if ORTHANC_PLUGINS_ENABLED == 1
  if (plugins &&
      plugins->HasWorklistHandler())
  {
    dicomServer.SetWorklistRequestHandlerFactory(*plugins);
  }
#endif

  dicomServer.SetPortNumber(Configuration::GetGlobalIntegerParameter("DicomPort", 4242));
  dicomServer.SetApplicationEntityTitle(Configuration::GetGlobalStringParameter("DicomAet", "ORTHANC"));
  dicomServer.SetApplicationEntityFilter(dicomFilter);

  dicomServer.Start();
  LOG(WARNING) << "DICOM server listening with AET " << dicomServer.GetApplicationEntityTitle() 
               << " on port: " << dicomServer.GetPortNumber();

  bool restart;
  ErrorCode error = ErrorCode_Success;

  try
  {
    restart = StartHttpServer(context, restApi, plugins);
  }
  catch (OrthancException& e)
  {
    error = e.GetErrorCode();
  }

  dicomServer.Stop();
  LOG(WARNING) << "    DICOM server has stopped";

  serverFactory.Done();

  if (error != ErrorCode_Success)
  {
    throw OrthancException(error);
  }

  return restart;
}


static bool ConfigureHttpHandler(ServerContext& context,
                                 OrthancPlugins *plugins)
{
#if ORTHANC_PLUGINS_ENABLED == 1
  // By order of priority, first apply the "plugins" layer, so that
  // plugins can overwrite the built-in REST API of Orthanc
  if (plugins)
  {
    assert(context.HasPlugins());
    context.GetHttpHandler().Register(*plugins, false);
  }
#endif

  // Secondly, apply the "static resources" layer
#if ORTHANC_STANDALONE == 1
  EmbeddedResourceHttpHandler staticResources("/app", EmbeddedResources::ORTHANC_EXPLORER);
#else
  FilesystemHttpHandler staticResources("/app", ORTHANC_PATH "/OrthancExplorer");
#endif

  context.GetHttpHandler().Register(staticResources, false);

  // Thirdly, consider the built-in REST API of Orthanc
  OrthancRestApi restApi(context);
  context.GetHttpHandler().Register(restApi, true);

  return StartDicomServer(context, restApi, plugins);
}


static bool UpgradeDatabase(IDatabaseWrapper& database,
                            IStorageArea& storageArea,
                            bool allowDatabaseUpgrade)
{
  // Upgrade the schema of the database, if needed
  unsigned int currentVersion = database.GetDatabaseVersion();
  if (currentVersion == ORTHANC_DATABASE_VERSION)
  {
    return true;
  }

  if (currentVersion > ORTHANC_DATABASE_VERSION)
  {
    LOG(ERROR) << "The version of the database schema (" << currentVersion
               << ") is too recent for this version of Orthanc. Please upgrade Orthanc.";
    return false;
  }

  if (!allowDatabaseUpgrade)
  {
    LOG(ERROR) << "The database schema must be upgraded from version "
               << currentVersion << " to " << ORTHANC_DATABASE_VERSION 
               << ": Please run Orthanc with the \"--upgrade\" command-line option";
    return false;
  }

  LOG(WARNING) << "Upgrading the database from schema version "
               << currentVersion << " to " << ORTHANC_DATABASE_VERSION;

  try
  {
    database.Upgrade(ORTHANC_DATABASE_VERSION, storageArea);
  }
  catch (OrthancException&)
  {
    LOG(ERROR) << "Unable to run the automated upgrade, please use the replication instructions: "
               << "https://orthanc.chu.ulg.ac.be/book/users/replication.html";
    throw;
  }
    
  // Sanity check
  currentVersion = database.GetDatabaseVersion();
  if (ORTHANC_DATABASE_VERSION != currentVersion)
  {
    LOG(ERROR) << "The database schema was not properly upgraded, it is still at version " << currentVersion;
    throw OrthancException(ErrorCode_InternalError);
  }

  return true;
}


static bool ConfigureServerContext(IDatabaseWrapper& database,
                                   IStorageArea& storageArea,
                                   OrthancPlugins *plugins)
{
  ServerContext context(database, storageArea);

  HttpClient::SetDefaultTimeout(Configuration::GetGlobalIntegerParameter("HttpTimeout", 0));
  context.SetCompressionEnabled(Configuration::GetGlobalBoolParameter("StorageCompression", false));
  context.SetStoreMD5ForAttachments(Configuration::GetGlobalBoolParameter("StoreMD5ForAttachments", true));

  try
  {
    context.GetIndex().SetMaximumPatientCount(Configuration::GetGlobalIntegerParameter("MaximumPatientCount", 0));
  }
  catch (...)
  {
    context.GetIndex().SetMaximumPatientCount(0);
  }

  try
  {
    uint64_t size = Configuration::GetGlobalIntegerParameter("MaximumStorageSize", 0);
    context.GetIndex().SetMaximumStorageSize(size * 1024 * 1024);
  }
  catch (...)
  {
    context.GetIndex().SetMaximumStorageSize(0);
  }

  LoadLuaScripts(context);

#if ORTHANC_PLUGINS_ENABLED == 1
  if (plugins)
  {
    plugins->SetServerContext(context);
    context.SetPlugins(*plugins);
  }
#endif

  bool restart;
  ErrorCode error = ErrorCode_Success;

  try
  {
    restart = ConfigureHttpHandler(context, plugins);
  }
  catch (OrthancException& e)
  {
    error = e.GetErrorCode();
  }

  context.Stop();

#if ORTHANC_PLUGINS_ENABLED == 1
  if (plugins)
  {
    context.ResetPlugins();
  }
#endif

  if (error != ErrorCode_Success)
  {
    throw OrthancException(error);
  }

  return restart;
}


static bool ConfigureDatabase(IDatabaseWrapper& database,
                              IStorageArea& storageArea,
                              OrthancPlugins *plugins,
                              bool allowDatabaseUpgrade)
{
  database.Open();
  
  if (!UpgradeDatabase(database, storageArea, allowDatabaseUpgrade))
  {
    return false;
  }

  bool success = ConfigureServerContext(database, storageArea, plugins);

  database.Close();

  return success;
}


static bool ConfigurePlugins(int argc, 
                             char* argv[],
                             bool allowDatabaseUpgrade)
{
  std::auto_ptr<IDatabaseWrapper>  databasePtr;
  std::auto_ptr<IStorageArea>  storage;

#if ORTHANC_PLUGINS_ENABLED == 1
  OrthancPlugins plugins;
  plugins.SetCommandLineArguments(argc, argv);
  LoadPlugins(plugins);

  IDatabaseWrapper* database = NULL;
  if (plugins.HasDatabaseBackend())
  {
    LOG(WARNING) << "Using a custom database from plugins";
    database = &plugins.GetDatabaseBackend();
  }
  else
  {
    databasePtr.reset(Configuration::CreateDatabaseWrapper());
    database = databasePtr.get();
  }

  if (plugins.HasStorageArea())
  {
    LOG(WARNING) << "Using a custom storage area from plugins";
    storage.reset(plugins.CreateStorageArea());
  }
  else
  {
    storage.reset(Configuration::CreateStorageArea());
  }

  assert(database != NULL);
  assert(storage.get() != NULL);

  return ConfigureDatabase(*database, *storage, &plugins, allowDatabaseUpgrade);

#elif ORTHANC_PLUGINS_ENABLED == 0
  // The plugins are disabled
  databasePtr.reset(Configuration::CreateDatabaseWrapper());
  storage.reset(Configuration::CreateStorageArea());

  return ConfigureDatabase(*databasePtr, *storage, NULL, allowDatabaseUpgrade);

#else
#  error The macro ORTHANC_PLUGINS_ENABLED must be set to 0 or 1
#endif
}


static bool StartOrthanc(int argc, 
                         char* argv[],
                         bool allowDatabaseUpgrade)
{
  return ConfigurePlugins(argc, argv, allowDatabaseUpgrade);
}


int main(int argc, char* argv[]) 
{
  Logging::Initialize();

  bool allowDatabaseUpgrade = false;
  const char* configurationFile = NULL;


  /**
   * Parse the command-line options.
   **/ 

  for (int i = 1; i < argc; i++)
  {
    std::string argument(argv[i]); 

    if (argument.empty())
    {
      // Ignore empty arguments
    }
    else if (argument[0] != '-')
    {
      if (configurationFile != NULL)
      {
        LOG(ERROR) << "More than one configuration path were provided on the command line, aborting";
        return -1;
      }
      else
      {
        // Use the first argument that does not start with a "-" as
        // the configuration file
        configurationFile = argv[i];
      }
    }
    else if (argument == "--errors")
    {
      PrintErrors(argv[0]);
      return 0;
    }
    else if (argument == "--help")
    {
      PrintHelp(argv[0]);
      return 0;
    }
    else if (argument == "--version")
    {
      PrintVersion(argv[0]);
      return 0;
    }
    else if (argument == "--verbose")
    {
      Logging::EnableInfoLevel(true);
    }
    else if (argument == "--trace")
    {
      Logging::EnableTraceLevel(true);
    }
    else if (boost::starts_with(argument, "--logdir="))
    {
      std::string directory = argument.substr(9);

      try
      {
        Logging::SetTargetFolder(directory);
      }
      catch (OrthancException&)
      {
        LOG(ERROR) << "The directory where to store the log files (" 
                   << directory << ") is inexistent, aborting.";
        return -1;
      }
    }
    else if (argument == "--upgrade")
    {
      allowDatabaseUpgrade = true;
    }
    else if (boost::starts_with(argument, "--config="))
    {
      std::string configurationSample;
      GetFileResource(configurationSample, EmbeddedResources::CONFIGURATION_SAMPLE);

#if defined(_WIN32)
      // Replace UNIX newlines with DOS newlines 
      boost::replace_all(configurationSample, "\n", "\r\n");
#endif

      std::string target = argument.substr(9);
      Toolbox::WriteFile(configurationSample, target);
      return 0;
    }
    else
    {
      LOG(WARNING) << "Option unsupported by the core of Orthanc: " << argument;
    }
  }


  /**
   * Launch Orthanc.
   **/

  {
    std::string version(ORTHANC_VERSION);

    if (std::string(ORTHANC_VERSION) == "mainline")
    {
      try
      {
        boost::filesystem::path exe(Toolbox::GetPathToExecutable());
        std::time_t creation = boost::filesystem::last_write_time(exe);
        boost::posix_time::ptime converted(boost::posix_time::from_time_t(creation));
        version += " (" + boost::posix_time::to_iso_string(converted) + ")";
      }
      catch (...)
      {
      }
    }

    LOG(WARNING) << "Orthanc version: " << version;
  }

  int status = 0;
  try
  {
    for (;;)
    {
      OrthancInitialize(configurationFile);

      if (0)
      {
        // TODO REMOVE THIS TEST
        DicomUserConnection c;
        c.SetRemoteHost("localhost");
        c.SetRemotePort(4243);
        c.SetRemoteApplicationEntityTitle("ORTHANCTEST");
        c.Open();
        ParsedDicomFile f(false);
        f.Replace(DICOM_TAG_PATIENT_NAME, "M*");
        DicomFindAnswers a;
        c.FindWorklist(a, f);
        Json::Value j;
        a.ToJson(j, true);
        std::cout << j;
      }

      bool restart = StartOrthanc(argc, argv, allowDatabaseUpgrade);
      if (restart)
      {
        OrthancFinalize();
      }
      else
      {
        break;
      }
    }
  }
  catch (const OrthancException& e)
  {
    LOG(ERROR) << "Uncaught exception, stopping now: [" << e.What() << "] (code " << e.GetErrorCode() << ")";
#if defined(_WIN32)
    if (e.GetErrorCode() >= ErrorCode_START_PLUGINS)
    {
      status = static_cast<int>(ErrorCode_Plugin);
    }
    else
    {
      status = static_cast<int>(e.GetErrorCode());
    }

#else
    status = -1;
#endif
  }
  catch (const std::exception& e) 
  {
    LOG(ERROR) << "Uncaught exception, stopping now: [" << e.what() << "]";
    status = -1;
  }
  catch (const std::string& s) 
  {
    LOG(ERROR) << "Uncaught exception, stopping now: [" << s << "]";
    status = -1;
  }
  catch (...)
  {
    LOG(ERROR) << "Native exception, stopping now. Check your plugins, if any.";
    status = -1;
  }

  OrthancFinalize();

  LOG(WARNING) << "Orthanc has stopped";

  Logging::Finalize();

  return status;
}
