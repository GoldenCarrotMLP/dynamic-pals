#pragma once
#include <Unreal/Hooks.hpp>

namespace DynPals {
    class HooksManager {
    public:
        static void RegisterHooks();
        static bool OnUObjectDeleted(RC::Unreal::UObject* Obj);

    private:
        static void OnPalSpawnedReady(RC::Unreal::UnrealScriptFunctionCallableContext& Context, void*);
    };
}