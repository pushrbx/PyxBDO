#include "ScriptDef.h"
#include <Shlwapi.h>
#include "../PyxContext.h"

Pyx::Scripting::ScriptDef::ScriptDef(std::wstring fileName)
{
    wchar_t buffer[MAX_PATH];
    fileName.copy(buffer, MAX_PATH);
    PathRemoveFileSpecW(buffer);
    m_scriptDirectory = std::wstring(buffer) + std::wstring(L"\\");
    auto scriptDefIni = Utility::IniFile(fileName);

    m_scriptSection = scriptDefIni.GetSectionValues(L"script");
    m_filesSection = scriptDefIni.GetSectionValues(L"files");
    m_dependenciestSection = scriptDefIni.GetSectionValues(L"dependencies");
}

const std::wstring Pyx::Scripting::ScriptDef::GetName()
{
    for (auto value : m_scriptSection)
        if (value.Key == L"name")
            return value.Value;
    return L"";
}

const std::wstring Pyx::Scripting::ScriptDef::GetType()
{
    for (auto value : m_scriptSection)
        if (value.Key == L"type")
            return value.Value;
    return L"";
}

std::vector<std::wstring> Pyx::Scripting::ScriptDef::GetFiles()
{
    std::vector<std::wstring> result;
    for (auto value : m_filesSection)
    {
        if (value.Key == L"file")
        {
            std::wstring pathName = m_scriptDirectory + value.Value;
            wchar_t fullPathName[MAX_PATH];
            GetFullPathNameW(pathName.c_str(), MAX_PATH, fullPathName, nullptr);
            result.push_back(fullPathName);
        }
    }
    return result;
}

std::vector<std::wstring> Pyx::Scripting::ScriptDef::GetDependencies()
{
    std::vector<std::wstring> result;
    for (auto value : m_dependenciestSection)
        if (value.Key == L"file")
        {
            std::wstring pathName = m_scriptDirectory + value.Value;
            wchar_t fullPathName[MAX_PATH];
            GetFullPathNameW(pathName.c_str(), MAX_PATH, fullPathName, nullptr);
            result.push_back(fullPathName);
        }
    return result;
}

bool Pyx::Scripting::ScriptDef::Validate(std::wstring& error)
{

    for (auto dep : GetDependencies())
    {
        if (PathFileExistsW(dep.c_str()) == TRUE)
        {
            ScriptDef def(dep);
            if (!def.Validate(error))
                return false;
        }
        else
        {
            auto lastError = GetLastError();
            error = L"[" + GetName() + L"] File not found : " + dep + L" (" + std::to_wstring(lastError) + L")";
            return false;
        }
    }

    for (auto file : GetFiles())
    {
        if (PathFileExistsW(file.c_str()) == FALSE)
        {
            auto lastError = GetLastError();
            error = L"[" + GetName() + L"] File not found : " + file + L" (" + std::to_wstring(lastError) + L")";
            return false;
        }
    }

    return true;

}

bool Pyx::Scripting::ScriptDef::Run(LuaIntf::LuaState& luaState)
{
    for (auto dep : GetDependencies())
    {
        ScriptDef def(dep);
        if (!def.Run(luaState))
        {
            return false;
        }
    }

    for (auto file : GetFiles())
    {

        std::fstream fs;
        fs.open(file, std::ios::in);
        std::string content((std::istreambuf_iterator<char>(fs)), (std::istreambuf_iterator<char>()));
        if (luaState.doString(content.c_str()))
        {
            PyxContext::GetInstance().Log(L"Error in file : \"" + file + L"\" :");
            std::string error = luaState.getString(-1);
            PyxContext::GetInstance().Log(error);
            return false;
        }

    }

    return true;

}
