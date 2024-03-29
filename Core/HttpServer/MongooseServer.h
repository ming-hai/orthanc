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


#pragma once

#include "IHttpHandler.h"

#include "../OrthancException.h"

#include <list>
#include <map>
#include <set>
#include <stdint.h>
#include <boost/shared_ptr.hpp>

namespace Orthanc
{
  class ChunkStore;

  class IIncomingHttpRequestFilter
  {
  public:
    virtual ~IIncomingHttpRequestFilter()
    {
    }

    virtual bool IsAllowed(HttpMethod method,
                           const char* uri,
                           const char* ip,
                           const char* username) const = 0;
  };


  class IHttpExceptionFormatter
  {
  public:
    virtual ~IHttpExceptionFormatter()
    {
    }

    virtual void Format(HttpOutput& output,
                        const OrthancException& exception,
                        HttpMethod method,
                        const char* uri) = 0;
  };


  class MongooseServer
  {
  private:
    // http://stackoverflow.com/questions/311166/stdauto-ptr-or-boostshared-ptr-for-pimpl-idiom
    struct PImpl;
    boost::shared_ptr<PImpl> pimpl_;

    IHttpHandler *handler_;

    typedef std::set<std::string> RegisteredUsers;
    RegisteredUsers registeredUsers_;

    bool remoteAllowed_;
    bool authentication_;
    bool ssl_;
    std::string certificate_;
    uint16_t port_;
    IIncomingHttpRequestFilter* filter_;
    bool keepAlive_;
    bool httpCompression_;
    IHttpExceptionFormatter* exceptionFormatter_;
  
    bool IsRunning() const;

  public:
    MongooseServer();

    ~MongooseServer();

    void SetPortNumber(uint16_t port);

    uint16_t GetPortNumber() const
    {
      return port_;
    }

    void Start();

    void Stop();

    void ClearUsers();

    void RegisterUser(const char* username,
                      const char* password);

    bool IsAuthenticationEnabled() const
    {
      return authentication_;
    }

    void SetAuthenticationEnabled(bool enabled);

    bool IsSslEnabled() const
    {
      return ssl_;
    }

    void SetSslEnabled(bool enabled);

    bool IsKeepAliveEnabled() const
    {
      return keepAlive_;
    }

    void SetKeepAliveEnabled(bool enabled);

    const std::string& GetSslCertificate() const
    {
      return certificate_;
    }

    void SetSslCertificate(const char* path);

    bool IsRemoteAccessAllowed() const
    {
      return remoteAllowed_;
    }

    void SetRemoteAccessAllowed(bool allowed);

    bool IsHttpCompressionEnabled() const
    {
      return httpCompression_;;
    }

    void SetHttpCompressionEnabled(bool enabled);

    const IIncomingHttpRequestFilter* GetIncomingHttpRequestFilter() const
    {
      return filter_;
    }

    void SetIncomingHttpRequestFilter(IIncomingHttpRequestFilter& filter);

    ChunkStore& GetChunkStore();

    bool IsValidBasicHttpAuthentication(const std::string& basic) const;

    void Register(IHttpHandler& handler);

    bool HasHandler() const
    {
      return handler_ != NULL;
    }

    IHttpHandler& GetHandler() const;

    void SetHttpExceptionFormatter(IHttpExceptionFormatter& formatter);

    IHttpExceptionFormatter* GetExceptionFormatter()
    {
      return exceptionFormatter_;
    }
  };
}
