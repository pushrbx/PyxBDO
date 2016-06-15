#pragma once
#include <map>
#include <vector>
#include <mutex>
#include <Pyx/PyxContext.h>
#include <Lua/lua.hpp>
#include <Lua/LuaIntf.h>
#include <Pyx/Utility/String.h>
#include <string>
#include <windows.h>

#define LUAINTF_ADD_ENUM_VALUE(state, constant) \
    state.push((int)constant); \
    state.setGlobal(#constant);

using namespace LuaIntf;

namespace LuaIntf
{
    LUA_USING_LIST_TYPE(std::vector)
    LUA_USING_MAP_TYPE(std::map)
}

namespace Pyx
{
    namespace Scripting
    {
        class Script
        {

        private:
            std::wstring m_name;
            std::wstring m_defFileName;
            std::wstring m_directory;
            bool m_isRunning = false;
            std::map<std::wstring, std::vector<LuaRef>> m_callbacks;
            LuaState m_luaState;
            lua_State* m_pLuaState = nullptr;
            std::recursive_mutex m_Mutex;

        public:
            Script(const std::wstring& name, const std::wstring& defFileName);
            ~Script();
            void Stop(bool fireEvent = true);
            void Start();
            bool IsRunning() const { return m_isRunning; }
            const std::wstring& GetName() const { return m_name; }
            std::map<std::wstring, std::vector<LuaRef>>& GetCallbacks() { return m_callbacks; }
            LuaState& GetLuaState() { return m_luaState; }
            const std::wstring& GetDefFileName() const { return m_defFileName; }
            const std::wstring& GetScriptDirectory() const { return m_directory; }
            void RegisterCallback(const std::wstring& name, LuaRef func);
            void UnregisterCallback(const std::wstring& name, LuaRef func);

        public:
            template <typename P0, typename... P>
            static void pushArg(lua_State* L, P0&& p0, P&&... p)
            {
                Lua::push(L, std::forward<P0>(p0));
                pushArg(L, std::forward<P>(p)...);
            }
            static void pushArg(lua_State*)
            {
                // template terminate function
            }
            template<typename... Args>
            void FireCallback(const std::wstring& name, Args... args)
            {
                if (m_Mutex.try_lock())
                {
                    auto find = m_callbacks.find(name);
                    if (find != m_callbacks.end())
                    {
                        for (auto& f : find->second)
                        {
                            lua_State* L = m_luaState;
                            lua_pushcfunction(L, &LuaException::traceback);
                            f.pushToStack();
                            pushArg(L, std::forward<Args>(args)...);
                            if (lua_pcall(L, sizeof...(Args), 0, -int(sizeof...(Args)+2)) != LUA_OK) {
                                lua_remove(L, -2);
                                std::string luaError;
                                if (lua_gettop(L) > 0) {
                                    luaError = lua_tostring(L, -1);
                                }
                                else {
                                    luaError = "Unknown error";
                                }
                                PyxContext::GetInstance().Log(XorStringW(L"Error in script \"%s\" in callback \"%s\""), m_name.c_str(), name.c_str());
                                PyxContext::GetInstance().Log(luaError);
                            }
                            lua_pop(L, 1);
                        }
                    }
                    m_Mutex.unlock();
                }
            }

        };
    }
}