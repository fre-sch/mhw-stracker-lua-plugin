#include <Windows.h>
#include <hooks.h>
#include <lua.hpp>
#include <sol/sol.hpp>
#include <iostream>
#include <filesystem>
#include "ghidra_export.h"
#include "loader.h"

namespace fs = std::filesystem;

template<typename T>
inline T* offsetPtr(void* ptr, int offset) { return (T*)(((char*)ptr) + offset); }
static void* offsetPtr(void* ptr, int offset) { return offsetPtr<void>(ptr, offset); }


static std::unordered_map<std::string, lua_State*> LUASTATES;

typedef std::unordered_multimap<const char*, lua_State*> LuaHookRegistry;
static LuaHookRegistry LUAHOOKS;

struct HookEntry {
    const char* hookName;
    void* hookFunc;
    void* hookPointer;
    void* originalFunc;
};


namespace lua_bind {
    void LogInfo(const std::string& message) {
        loader::LOG(loader::INFO) << message;
    }

    void GameShowMessage(char* message) {
        MH::Chat::ShowGameMessage(*(undefined**)MH::Chat::MainPtr, message, -1, -1, 0);
    }
}


HOOKFUNC(Monster_CanClawTurn, bool, void* monster)
{
    auto its = LUAHOOKS.equal_range("Monster_CanClawTurn");
    bool result = originalMonster_CanClawTurn(monster);
    for (auto it = its.first; it != its.second; ++it) {
        sol::state_view state(it->second);
        sol::optional<sol::protected_function> hookFunction = state["Hooks"][it->first];
        if (hookFunction != sol::nullopt) {
            result = (*hookFunction)(monster, result);
        }
    }
    return result;
}

HOOKFUNC(Monster_LaunchAction, bool, void* monster, int actionId)
{
    auto its = LUAHOOKS.equal_range("Monster_LaunchAction");
    bool result = originalMonster_LaunchAction(monster, actionId);
    for (auto it = its.first; it != its.second; ++it) {
        sol::state_view state(it->second);
        sol::optional<sol::protected_function> hookFunction = state["Hooks"][it->first];
        if (hookFunction != sol::nullopt) {
            result = (*hookFunction)(monster, actionId, result);
        }
    }
    return result;
}

HOOKFUNC(Quest_Count, int, void)
{
    auto its = LUAHOOKS.equal_range("Quest_Count");
    bool result = originalQuest_Count();
    for (auto it = its.first; it != its.second; ++it) {
        sol::state_view state(it->second);
        sol::optional<sol::protected_function> hookFunction = state["Hooks"][it->first];
        if (hookFunction != sol::nullopt) {
            (*hookFunction)(result);
        }
    }
    return result;
}

#define HOOKENTRY(NAME) HookEntry{#NAME, &NAME, &MH::##NAME, &original##NAME}
static const std::vector<HookEntry> HOOKS = {
    HOOKENTRY(Monster_CanClawTurn),
    HOOKENTRY(Monster_LaunchAction),
    HOOKENTRY(Quest_Count)
};



void initState(sol::state_view& state) {
    //sol::usertype<Monster> monsterType = state.new_usertype<Monster>(
    //    "Monster", sol::no_constructor);
    //monsterType["health"] = sol::property(
    //    sol::resolve<int() const>(&Monster::health),
    //    sol::resolve<void(int)>(&Monster::health)
    //    );
    state["Hooks"] = state.create_table();
    state["Game"] = state.create_table();
    state["Game"]["ShowMessage"] = &lua_bind::GameShowMessage;
    state["Log"] = state.create_table();
    state["Log"]["Info"] = &lua_bind::LogInfo;
}

void initScript(std::string path) {
    loader::LOG(loader::INFO) << "loading script: " << path;
    lua_State* luaState = luaL_newstate();
    LUASTATES[path] = luaState;
    sol::state_view state(luaState);
    state.open_libraries();
    initState(state);
    state.do_file(path);
    for (const HookEntry& it : HOOKS) {
        sol::optional<sol::protected_function> hookFunction = state["Hooks"][it.hookName];
        if (hookFunction != sol::nullopt) {
            loader::LOG(loader::INFO) << "script hook: " << it.hookName << " found";
            LUAHOOKS.insert({ it.hookName, luaState });
        }
        else {
            loader::LOG(loader::INFO) << "script hook: " << it.hookName << " not found";
        }
    }
}

void initLuaScripts(fs::path& path) {
    loader::LOG(loader::INFO) << "loading scripts in path: " << path;
    for (const auto& dirEntry : fs::directory_iterator(path)) {
        if (dirEntry.is_regular_file()
            && dirEntry.path().filename().string().ends_with(".lua")) {
            initScript(dirEntry.path().string());
        }
    }
}

void onLoad() {
    fs::path scriptDir = "nativePC\\plugins\\scripts";
    if (!fs::exists(scriptDir)) {
        loader::LOG(loader::INFO) << "scriptdir " << scriptDir << " does not exist";
        return;
    }

    initLuaScripts(scriptDir);

    MH_Initialize();
    for (const HookEntry& it : HOOKS) {
        if (LUAHOOKS.count(it.hookName) > 0) {
            loader::LOG(loader::INFO) << "enabling hook " << it.hookName;
            MH_CreateHook(it.hookPointer, it.hookFunc, (LPVOID*)(it.originalFunc));
            MH_QueueEnableHook(it.hookPointer);
        }
    }
    MH_ApplyQueued();
}

BOOL APIENTRY DllMain(
    HMODULE hModule, DWORD  ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            onLoad();
            break;
        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH:
        case DLL_PROCESS_DETACH:
            break;
    }
    return TRUE;
}

