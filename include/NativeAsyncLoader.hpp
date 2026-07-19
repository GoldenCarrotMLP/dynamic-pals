// --- START OF FILE include/NativeAsyncLoader.hpp ---
#pragma once
#include <string>
#include <Unreal/UObjectGlobals.hpp>

namespace DynPals {
    class NativeAsyncLoader {
    public:
        static void Initialize();
        static bool RequestAsyncLoad(const std::wstring& AssetPath, RC::Unreal::UObject* Requester);
        
        // Anti-Infinite-Loop trackers
        static bool HasBeenRequested(const std::wstring& AssetPath);
        static void ClearCache();
    };
}
// --- END OF FILE include/NativeAsyncLoader.hpp ---