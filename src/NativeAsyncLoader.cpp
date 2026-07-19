// --- START OF FILE src/NativeAsyncLoader.cpp ---
#include "../include/NativeAsyncLoader.hpp"
#include "Utils.hpp"
#include "DataTypes.hpp"
#include "PalProcessor.hpp"
#include <new> 
#include <set>
#include <map>
#include <deque>
#include <chrono>

using namespace RC::Unreal;

namespace DynPals {

    static UClass* LoaderClass = nullptr;
    static UObject* GAssetLoaderActor = nullptr;
    
    static std::deque<std::wstring> GLoadQueue;
    static std::set<std::wstring> GPendingAssets;
    static std::set<std::wstring> GFailedAssets;
    static std::map<std::wstring, std::set<UObject*>> GAssetWaiters;
    static std::map<UObject*, int> GPendingCount;

    static bool bIsExecutingBP = false;
    static std::wstring GCurrentExecutingAsset = L"";
    static std::chrono::steady_clock::time_point GCurrentRequestStartTime;

    void NativeAsyncLoader::ClearCache() {
        GLoadQueue.clear(); GPendingAssets.clear(); GFailedAssets.clear(); GAssetWaiters.clear(); GPendingCount.clear();
        bIsExecutingBP = false; GCurrentExecutingAsset = L"";
    }

    bool NativeAsyncLoader::IsPending(const std::wstring& AssetPath) { return GPendingAssets.find(AssetPath) != GPendingAssets.end(); }
    bool NativeAsyncLoader::IsFailed(const std::wstring& AssetPath) { return GFailedAssets.find(AssetPath) != GFailedAssets.end(); }
    void NativeAsyncLoader::MarkAsLoaded(const std::wstring& AssetPath) { GPendingAssets.erase(AssetPath); }
    void NativeAsyncLoader::MarkAsFailed(const std::wstring& AssetPath) { GPendingAssets.erase(AssetPath); GFailedAssets.insert(AssetPath); }
    int NativeAsyncLoader::GetPendingCount(UObject* Requester) { return GPendingCount.count(Requester) ? GPendingCount[Requester] : 0; }
    void NativeAsyncLoader::RegisterPendingRequests(UObject* Requester, int Count) { GPendingCount[Requester] += Count; }
    void NativeAsyncLoader::DecrementPendingCount(UObject* Requester) { if (GPendingCount[Requester] > 0) GPendingCount[Requester]--; }

    static void ProcessNextRequest() {
        if (bIsExecutingBP || GLoadQueue.empty()) return;

        bIsExecutingBP = true;
        GCurrentExecutingAsset = GLoadQueue.front();
        GLoadQueue.pop_front();
        GCurrentRequestStartTime = std::chrono::steady_clock::now();

        DP_LOG(Default, "[Diagnostic] [AsyncLoader] Dispatching BP request: '{}'", GCurrentExecutingAsset);

        if (!GAssetLoaderActor || !Utils::IsObjectValid(GAssetLoaderActor)) GAssetLoaderActor = UObjectGlobals::FindFirstOf(STR("ModActor_C"));

        if (GAssetLoaderActor && Utils::IsObjectValid(GAssetLoaderActor)) {
            UFunction* Func = GAssetLoaderActor->GetFunctionByNameInChain(STR("RequestAsyncLoad"));
            if (Func) {
                std::wstring formattedPath = Utils::FormatAssetPath(GCurrentExecutingAsset);
                if (formattedPath.length() > 2 && formattedPath.substr(formattedPath.length() - 2) == L"_C") {
                    formattedPath = formattedPath.substr(0, formattedPath.length() - 2);
                }

                alignas(8) uint8_t BPParams[256] = {0};
                FString* AssetPathPtr = nullptr;
                FProperty* AssetPathProp = Func->GetPropertyByNameInChain(STR("AssetPath"));
                if (AssetPathProp) {
                    AssetPathPtr = AssetPathProp->ContainerPtrToValuePtr<FString>(BPParams);
                    if (AssetPathPtr) new (AssetPathPtr) FString(formattedPath.c_str()); 
                }

                FProperty* RequesterProp = Func->GetPropertyByNameInChain(STR("Requester"));
                if (RequesterProp) {
                    UObject* FirstValidWaiter = nullptr;
                    for (UObject* W : GAssetWaiters[GCurrentExecutingAsset]) { if (Utils::IsObjectValid(W)) { FirstValidWaiter = W; break; } }
                    UObject** Ptr = RequesterProp->ContainerPtrToValuePtr<UObject*>(BPParams);
                    if (Ptr) *Ptr = FirstValidWaiter;
                }

                Utils::SafeProcessEvent(GAssetLoaderActor, Func, BPParams);
                if (AssetPathPtr) AssetPathPtr->~FString();
                return;
            }
        }
        
        DP_LOG(Warning, "[Diagnostic] [AsyncLoader] Failed to route BP request. Marking as failed.");
        GPendingAssets.erase(GCurrentExecutingAsset);
        GFailedAssets.insert(GCurrentExecutingAsset);
        bIsExecutingBP = false;
        ProcessNextRequest();
    }

    void NativeAsyncLoader::OnAsyncLoadComplete(UObject* ModActor, UObject* Requester) {
        if (!ModActor) return;
        std::wstring FinishedAsset = GCurrentExecutingAsset;
        DP_LOG(Default, "[Diagnostic] [AsyncLoader] SetOwner hook fired! Completing: '{}'", FinishedAsset);

        bIsExecutingBP = false;
        GCurrentExecutingAsset = L"";

        if (!FinishedAsset.empty()) {
            GPendingAssets.erase(FinishedAsset);
            auto Waiters = GAssetWaiters[FinishedAsset];
            GAssetWaiters.erase(FinishedAsset);
            
            ProcessNextRequest();

            for (UObject* Waiter : Waiters) {
                if (Utils::IsObjectValid(Waiter)) {
                    DecrementPendingCount(Waiter);
                    DP_LOG(Default, "[Diagnostic] [AsyncLoader] Decremented wait count for Actor. Remaining: {}", GetPendingCount(Waiter));
                    if (GetPendingCount(Waiter) == 0) {
                        PalProcessor::Get().ProcessPal(Waiter, false, -1);
                    }
                }
            }
        } else {
            ProcessNextRequest();
        }
    }

    void NativeAsyncLoader::Tick() {
        if (!bIsExecutingBP) return;
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - GCurrentRequestStartTime).count() > 2000) {
            DP_LOG(Error, "[Diagnostic] [AsyncLoader] TIMEOUT! Hung for >2s: '{}'", GCurrentExecutingAsset);
            std::wstring FailedAsset = GCurrentExecutingAsset;
            bIsExecutingBP = false;
            GCurrentExecutingAsset = L"";
            GPendingAssets.erase(FailedAsset);
            GFailedAssets.insert(FailedAsset);
            
            auto Waiters = GAssetWaiters[FailedAsset];
            GAssetWaiters.erase(FailedAsset);
            ProcessNextRequest();
            for (UObject* Waiter : Waiters) {
                if (Utils::IsObjectValid(Waiter)) {
                    DecrementPendingCount(Waiter);
                    if (GetPendingCount(Waiter) == 0) PalProcessor::Get().ProcessPal(Waiter, false, -1);
                }
            }
        }
    }

    void NativeAsyncLoader::Initialize() {
        if (LoaderClass && Utils::IsObjectValid(LoaderClass)) return;
        LoaderClass = static_cast<UClass*>(Utils::LoadAssetSafely(STR("/Game/Mods/DynamicPals/ModActor.ModActor_C")));
    }

    bool NativeAsyncLoader::RequestAsyncLoad(const std::wstring& AssetPath, UObject* Requester) {
        if (!GAssetLoaderActor || !Utils::IsObjectValid(GAssetLoaderActor)) GAssetLoaderActor = UObjectGlobals::FindFirstOf(STR("ModActor_C"));
        if (GAssetLoaderActor && Utils::IsObjectValid(GAssetLoaderActor)) {
            GAssetWaiters[AssetPath].insert(Requester);
            if (GPendingAssets.find(AssetPath) == GPendingAssets.end() && GFailedAssets.find(AssetPath) == GFailedAssets.end()) {
                GPendingAssets.insert(AssetPath);
                GLoadQueue.push_back(AssetPath);
                DP_LOG(Default, "[Diagnostic] [AsyncLoader] Queued: '{}'", AssetPath);
                ProcessNextRequest();
            }
            return true;
        }
        return false;
    }
}
// --- END OF FILE src/NativeAsyncLoader.cpp ---