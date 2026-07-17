// --- START OF FILE include/NativeAsyncLoader.hpp ---
#pragma once
#include <string>
#include <Unreal/FString.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include "DataTypes.hpp"

namespace DynPals {
    class NativeAsyncLoader {
    public:
        static void Initialize();
        
        // Pass WorldContext dynamically from any active character 
        static void RequestAsyncLoad(const std::wstring& AssetPath, RC::Unreal::UObject* WorldContext);
    };
}
// --- END OF FILE include/NativeAsyncLoader.hpp ---