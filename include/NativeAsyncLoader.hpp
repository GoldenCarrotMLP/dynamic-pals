#pragma once
#include <string>
#include <vector>
#include <Unreal/UObjectGlobals.hpp>

namespace DynPals {
    class NativeAsyncLoader {
    public:
        static void Initialize();
        static void Tick(); 

        static bool IsPakInstalled();
        static bool IsPakOnDisk();
        
        static bool RequestBatchAsyncLoad(const std::vector<std::wstring>& AssetPaths, RC::Unreal::UObject* Requester, int ExplicitSwapIndex = -1, bool ForceReroll = false, bool IsCompanionSync = false, bool IsEvolutionEnd = false);
        
        static void OnAsyncLoadComplete(RC::Unreal::UObject* ModActor, RC::Unreal::UObject* Requester);
        static std::wstring ResolveCasing(const std::wstring& Path);

        static void SetActiveRequester(RC::Unreal::UObject* Requester);
        static RC::Unreal::UObject* GetLoadedPointer(const std::wstring& Path);
        static void ClearTemporaryPointers(RC::Unreal::UObject* Requester);

        static RC::Unreal::UObject* GetGlobalPointer(const std::wstring& Path);
        static RC::Unreal::UObject* FetchFromBPMasterArray(const std::wstring& Path);
        static void RegisterGlobalPointer(const std::wstring& Path, RC::Unreal::UObject* Asset);

        static bool IsPending(const std::wstring& AssetPath);
        static bool IsFailed(const std::wstring& AssetPath);
        static void MarkAsLoaded(const std::wstring& AssetPath);
        static void MarkAsFailed(const std::wstring& AssetPath);
        
        static int GetPendingCount(RC::Unreal::UObject* Requester);
        static void RegisterPendingRequests(RC::Unreal::UObject* Requester, int Count);
        static void DecrementPendingCount(RC::Unreal::UObject* Requester);
        
        static void ClearCache();
        static bool OnUObjectDeleted(RC::Unreal::UObject* Obj);
    };
}