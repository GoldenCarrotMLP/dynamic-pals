// --- START OF FILE include/NativeAsyncLoader.hpp ---
#pragma once
#include <string>
#include <vector>
#include <Unreal/UObjectGlobals.hpp>

namespace DynPals {
    class NativeAsyncLoader {
    public:
        static void Initialize();
        static void Tick(); 
        
        // Single Request Fallback
        static bool RequestAsyncLoad(const std::wstring& AssetPath, RC::Unreal::UObject* Requester);
        
        // Parallel Batch Loader
        static bool RequestBatchAsyncLoad(const std::vector<std::wstring>& AssetPaths, RC::Unreal::UObject* Requester);
        
        // Callback routed from HooksManager's SetOwner hook
        static void OnAsyncLoadComplete(RC::Unreal::UObject* ModActor, RC::Unreal::UObject* Requester);
        
        // State Machine Queries
        static bool IsPending(const std::wstring& AssetPath);
        static bool IsFailed(const std::wstring& AssetPath);
        static void MarkAsLoaded(const std::wstring& AssetPath);
        static void MarkAsFailed(const std::wstring& AssetPath);
        
        // Parallel Tracker Methods
        static int GetPendingCount(RC::Unreal::UObject* Requester);
        static void RegisterPendingRequests(RC::Unreal::UObject* Requester, int Count);
        static void DecrementPendingCount(RC::Unreal::UObject* Requester);
        
        static void ClearCache();
    };
}
// --- END OF FILE include/NativeAsyncLoader.hpp ---