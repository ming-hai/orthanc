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

#include "../HttpClient.h"

extern "C" 
{
#include <lua.h>
}

#include <EmbeddedResources.h>
#include <boost/noncopyable.hpp>

namespace Orthanc
{
  class LuaContext : public boost::noncopyable
  {
  private:
    friend class LuaFunctionCall;

    lua_State *lua_;
    std::string log_;
    HttpClient httpClient_;

    static int PrintToLog(lua_State *state);
    static int ParseJson(lua_State *state);
    static int DumpJson(lua_State *state);

    static int SetHttpCredentials(lua_State *state);

    static int CallHttpPostOrPut(lua_State *state,
                                 HttpMethod method);
    static int CallHttpGet(lua_State *state);
    static int CallHttpPost(lua_State *state);
    static int CallHttpPut(lua_State *state);
    static int CallHttpDelete(lua_State *state);

    bool AnswerHttpQuery(lua_State* state);

    void ExecuteInternal(std::string* output,
                         const std::string& command);

    void GetJson(Json::Value& result,
                 int top,
                 bool keepStrings);
    
  public:
    LuaContext();

    ~LuaContext();

    void Execute(const std::string& command)
    {
      ExecuteInternal(NULL, command);
    }

    void Execute(std::string& output,
                 const std::string& command)
    {
      ExecuteInternal(&output, command);
    }

    void Execute(Json::Value& output,
                 const std::string& command);

    void Execute(EmbeddedResources::FileResourceId resource);

    bool IsExistingFunction(const char* name);

    void SetHttpCredentials(const char* username,
                            const char* password)
    {
      httpClient_.SetCredentials(username, password);
    }

    void SetHttpProxy(const std::string& proxy)
    {
      httpClient_.SetProxy(proxy);
    }

    void RegisterFunction(const char* name,
                          lua_CFunction func);

    void SetGlobalVariable(const char* name,
                           void* value);

    static LuaContext& GetLuaContext(lua_State *state);

    static const void* GetGlobalVariable(lua_State* state,
                                         const char* name);

    void PushJson(const Json::Value& value);
  };
}
