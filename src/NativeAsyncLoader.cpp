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
    
    // Direct Pointer Cache
    static std::map<std::wstring, UObject*> GResolvedPointers;

    static bool bIsExecutingBP = false;
    static std::wstring GCurrentExecutingAsset = L"";
    static std::chrono::steady_clock::time_point GCurrentRequestStartTime;

    void NativeAsyncLoader::ClearCache() {
        GLoadQueue.clear();
        GPendingAssets.clear();
        GFailedAssets.clear();
        GAssetWaiters.clear();
        GPendingCount.clear();
        GResolvedPointers.clear();
        bIsExecutingBP = false;
        GCurrentExecutingAsset = L"";
    }

    bool NativeAsyncLoader::IsPending(const std::wstring& AssetPath) { return GPendingAssets.find(AssetPath) != GPendingAssets.end(); }
    bool NativeAsyncLoader::IsFailed(const std::wstring& AssetPath) { return GFailedAssets.find(AssetPath) != GFailedAssets.end(); }
    void NativeAsyncLoader::MarkAsLoaded(const std::wstring& AssetPath) { GPendingAssets.erase(AssetPath); }
    void NativeAsyncLoader::MarkAsFailed(const std::wstring& AssetPath) { GPendingAssets.erase(AssetPath); GFailedAssets.insert(AssetPath); }

    int NativeAsyncLoader::GetPendingCount(UObject* Requester) {
        auto it = GPendingCount.find(Requester);
        return it != GPendingCount.end() ? it->second : 0;
    }

    void NativeAsyncLoader::RegisterPendingRequests(UObject* Requester, int Count) {
        GPendingCount[Requester] += Count;
    }

    void NativeAsyncLoader::DecrementPendingCount(UObject* Requester) {
        if (GPendingCount[Requester] > 0) GPendingCount[Requester]--;
    }

    UObject* NativeAsyncLoader::GetLoadedPointer(const std::wstring& Path) {
        auto it = GResolvedPointers.find(Path);
        if (it != GResolvedPointers.end() && Utils::IsObjectValid(it->second)) return it->second;
        return nullptr;
    }

    static void ProcessNextRequest() {
        if (bIsExecutingBP || GLoadQueue.empty()) return;

        bIsExecutingBP = true;
        GCurrentExecutingAsset = GLoadQueue.front();
        GLoadQueue.pop_front();
        GCurrentRequestStartTime = std::chrono::steady_clock::now();

        DP_LOG(Default, "[NativeAsync] Dispatching background request to Blueprint for: '{}'", GCurrentExecutingAsset);

        if (!GAssetLoaderActor || !Utils::IsObjectValid(GAssetLoaderActor)) {
            GAssetLoaderActor = UObjectGlobals::FindFirstOf(STR("ModActor_C"));
        }

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
                    for (UObject* W : GAssetWaiters[GCurrentExecutingAsset]) {
                        if (Utils::IsObjectValid(W)) { FirstValidWaiter = W; break; }
                    }
                    UObject** Ptr = RequesterProp->ContainerPtrToValuePtr<UObject*>(BPParams);
                    if (Ptr) *Ptr = FirstValidWaiter;
                }

                Utils::SafeProcessEvent(GAssetLoaderActor, Func, BPParams);

                if (AssetPathPtr) AssetPathPtr->~FString();
                return;
            }
        }
        
        GPendingAssets.erase(GCurrentExecutingAsset);
        GFailedAssets.insert(GCurrentExecutingAsset);
        bIsExecutingBP = false;
        GCurrentExecutingAsset = L"";
        ProcessNextRequest();
    }

    void NativeAsyncLoader::OnAsyncLoadComplete(UObject* ModActor, UObject* Requester) {
        if (!ModActor) return;

        std::wstring FinishedAsset = GCurrentExecutingAsset;
        bIsExecutingBP = false;
        GCurrentExecutingAsset = L"";

        if (!FinishedAsset.empty()) {
            UObject* AssetPtr = nullptr;
            // Retrieve the direct pointer from your Blueprint's variable
            Utils::GetPropertyValue<UObject*>(ModActor, STR("LoadedAssetPtr"), AssetPtr);

            if (AssetPtr && Utils::IsObjectValid(AssetPtr)) {
                GResolvedPointers[FinishedAsset] = AssetPtr;
                DP_LOG(Default, "[NativeAsync] Captured direct pointer from BP for '{}'", FinishedAsset);
            } else {
                DP_LOG(Warning, "[NativeAsync] BP completed but LoadedAssetPtr was invalid for '{}'. Marking as FAILED.", FinishedAsset);
                GFailedAssets.insert(FinishedAsset); // ---> THE CRITICAL INFINITE LOOP FIX!
            }

            GPendingAssets.erase(FinishedAsset);
            auto Waiters = GAssetWaiters[FinishedAsset];
            GAssetWaiters.erase(FinishedAsset);

            ProcessNextRequest();

            for (UObject* Waiter : Waiters) {
                if (Utils::IsObjectValid(Waiter)) {
                    NativeAsyncLoader::DecrementPendingCount(Waiter);
                    if (NativeAsyncLoader::GetPendingCount(Waiter) == 0) {
                        DP_LOG(Default, "[NativeAsync] All dependencies loaded for Pal. Resuming processing...");
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
            DP_LOG(Error, "[NativeAsync] TIMEOUT! Asset load hung for >2s: '{}'", GCurrentExecutingAsset);
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
                    NativeAsyncLoader::DecrementPendingCount(Waiter);
                    if (NativeAsyncLoader::GetPendingCount(Waiter) == 0) PalProcessor::Get().ProcessPal(Waiter, false, -1);
                }
            }
        }
    }

    void NativeAsyncLoader::Initialize() {
        if (LoaderClass && Utils::IsObjectValid(LoaderClass)) return;
        LoaderClass = static_cast<UClass*>(Utils::LoadAssetSafely(STR("/Game/Mods/DynamicPals/ModActor.ModActor_C")));
    }

    bool NativeAsyncLoader::RequestAsyncLoad(const std::wstring& AssetPath, UObject* Requester) {
        if (!LoaderClass || !Utils::IsObjectValid(LoaderClass)) {
            LoaderClass = Utils::GetClassCached(STR("/Game/Mods/DynamicPals/ModActor.ModActor_C"), true);
            if (!LoaderClass || !Utils::IsObjectValid(LoaderClass)) return false;
        }

        if (!GAssetLoaderActor || !Utils::IsObjectValid(GAssetLoaderActor)) {
            GAssetLoaderActor = UObjectGlobals::FindFirstOf(STR("ModActor_C"));
        }

        if (GAssetLoaderActor && Utils::IsObjectValid(GAssetLoaderActor)) {
            GAssetWaiters[AssetPath].insert(Requester);
            if (GPendingAssets.find(AssetPath) == GPendingAssets.end() && GFailedAssets.find(AssetPath) == GFailedAssets.end()) {
                GPendingAssets.insert(AssetPath);
                GLoadQueue.push_back(AssetPath);
                DP_LOG(Default, "[NativeAsync] Queued asset for loading: '{}'", AssetPath);
                ProcessNextRequest();
            }
            return true;
        }
        return false;
    }
}
// --- END OF FILE src/NativeAsyncLoader.cpp ---