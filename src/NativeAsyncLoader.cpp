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

    struct FBatchRequest {
        std::vector<std::wstring> AssetPaths;
        UObject* Requester;
    };

    static UClass* LoaderClass = nullptr;
    static UObject* GAssetLoaderActor = nullptr;
    
    static std::set<std::wstring> GPendingAssets;
    static std::set<std::wstring> GFailedAssets;
    static std::map<UObject*, int> GPendingCount;
    static std::map<UObject*, std::vector<std::wstring>> GActivePalBatches;
    
    static std::deque<FBatchRequest> GBatchQueue;
    static FBatchRequest GCurrentBatch;

    static bool bIsExecutingBP = false;
    static std::chrono::steady_clock::time_point GCurrentRequestStartTime;

    void NativeAsyncLoader::ClearCache() {
        GPendingAssets.clear();
        GFailedAssets.clear();
        GPendingCount.clear();
        GBatchQueue.clear();
        GCurrentBatch = {};
        bIsExecutingBP = false;
        
        GAssetLoaderActor = nullptr; 
    }

    bool NativeAsyncLoader::IsPending(const std::wstring& AssetPath) { return GPendingAssets.find(AssetPath) != GPendingAssets.end(); }
    bool NativeAsyncLoader::IsFailed(const std::wstring& AssetPath) { return GFailedAssets.find(AssetPath) != GFailedAssets.end(); }
    void NativeAsyncLoader::MarkAsLoaded(const std::wstring& AssetPath) { GPendingAssets.erase(AssetPath); }
    void NativeAsyncLoader::MarkAsFailed(const std::wstring& AssetPath) { GPendingAssets.erase(AssetPath); GFailedAssets.insert(AssetPath); }
    int NativeAsyncLoader::GetPendingCount(UObject* Requester) { return GPendingCount.count(Requester) ? GPendingCount[Requester] : 0; }
    void NativeAsyncLoader::RegisterPendingRequests(UObject* Requester, int Count) { GPendingCount[Requester] += Count; }
    void NativeAsyncLoader::DecrementPendingCount(UObject* Requester) { if (GPendingCount[Requester] > 0) GPendingCount[Requester]--; }

    static void ProcessNextBatch() {
        if (bIsExecutingBP || GBatchQueue.empty()) return;

        bIsExecutingBP = true;
        GCurrentBatch = GBatchQueue.front();
        GBatchQueue.pop_front();
        GCurrentRequestStartTime = std::chrono::steady_clock::now();

        if (!GAssetLoaderActor || !Utils::IsObjectValid(GAssetLoaderActor)) {
            GAssetLoaderActor = UObjectGlobals::FindFirstOf(STR("ModActor_C"));
        }

        if (GAssetLoaderActor && Utils::IsObjectValid(GAssetLoaderActor)) {
            UFunction* Func = GAssetLoaderActor->GetFunctionByNameInChain(STR("RequestBatchAsyncLoad"));
            if (Func) {
                alignas(8) uint8_t BPParams[256] = {0};
                TArray<FString>* AssetPathsPtr = nullptr;
                
                FProperty* AssetPathsProp = Func->GetPropertyByNameInChain(STR("AssetPaths"));
                if (AssetPathsProp) {
                    AssetPathsPtr = AssetPathsProp->ContainerPtrToValuePtr<TArray<FString>>(BPParams);
                    if (AssetPathsPtr) {
                        new (AssetPathsPtr) TArray<FString>();
                        for (const auto& path : GCurrentBatch.AssetPaths) {
                            std::wstring formatted = Utils::FormatAssetPath(path);
                            if (formatted.length() > 2 && formatted.substr(formatted.length() - 2) == L"_C") {
                                formatted = formatted.substr(0, formatted.length() - 2);
                            }
                            AssetPathsPtr->Add(FString(formatted.c_str()));
                        }
                    }
                }

                FProperty* RequesterProp = Func->GetPropertyByNameInChain(STR("Requester"));
                if (RequesterProp) {
                    UObject** Ptr = RequesterProp->ContainerPtrToValuePtr<UObject*>(BPParams);
                    if (Ptr) *Ptr = GCurrentBatch.Requester;
                }

                DP_LOG(Default, "[NativeAsync] Dispatching batch of {} assets to BP for Pal: {}", GCurrentBatch.AssetPaths.size(), (void*)GCurrentBatch.Requester);
                Utils::SafeProcessEvent(GAssetLoaderActor, Func, BPParams);

                if (AssetPathsPtr) {
                    AssetPathsPtr->~TArray<FString>();
                }
                return;
            }
        }
        
        bIsExecutingBP = false;
        for (const auto& path : GCurrentBatch.AssetPaths) {
            GPendingAssets.erase(path);
            GFailedAssets.insert(path);
        }
        GPendingCount[GCurrentBatch.Requester] = 0;
        PalProcessor::Get().ProcessPal(GCurrentBatch.Requester, false, -1);
        ProcessNextBatch();
    }

    void NativeAsyncLoader::OnAsyncLoadComplete(UObject* ModActor, UObject* Requester) {
        if (!ModActor || !Requester) return;

        DP_LOG(Default, "[NativeAsync] Parallel batch completion received for Pal: {}", (void*)Requester);
        
        if (GCurrentBatch.Requester == Requester) {
            for (const auto& path : GCurrentBatch.AssetPaths) {
                GPendingAssets.erase(path);
                if (!Utils::IsAssetLoaded(path)) {
                    DP_LOG(Warning, "[NativeAsync] Batch load completed but asset failed RAM verification: '{}'", path);
                    GFailedAssets.insert(path);
                }
            }

            bIsExecutingBP = false;
            GPendingCount[Requester] = 0;
            
            ProcessNextBatch();
            
            PalProcessor::Get().ProcessPal(Requester, false, -1);
        } else {
            DP_LOG(Warning, "[NativeAsync] Received orphaned/invalid batch completion for Pal: {}", (void*)Requester);
        }
    }

    void NativeAsyncLoader::Tick() {
        if (!bIsExecutingBP) return;
        auto now = std::chrono::steady_clock::now();

        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - GCurrentRequestStartTime).count() > 5000) { 
            DP_LOG(Error, "[NativeAsync] TIMEOUT! Background batch load hung for >5s for Pal: {}", (void*)GCurrentBatch.Requester);
            
            for (const auto& path : GCurrentBatch.AssetPaths) {
                GPendingAssets.erase(path);
                GFailedAssets.insert(path);
            }
            
            UObject* stalledRequester = GCurrentBatch.Requester;
            bIsExecutingBP = false;
            
            ProcessNextBatch(); 

            if (Utils::IsObjectValid(stalledRequester)) {
                GPendingCount[stalledRequester] = 0;
                PalProcessor::Get().ProcessPal(stalledRequester, false, -1);
            }
        }
    }

    void NativeAsyncLoader::Initialize() {
        LoaderClass = static_cast<UClass*>(Utils::LoadAssetSafely(STR("/Game/Mods/DynamicPals/ModActor.ModActor_C")));
        GAssetLoaderActor = nullptr; 
    }

    bool NativeAsyncLoader::RequestBatchAsyncLoad(const std::vector<std::wstring>& AssetPaths, UObject* Requester) {
        if (!LoaderClass || !Utils::IsObjectValid(LoaderClass)) {
            LoaderClass = Utils::GetClassCached(STR("/Game/Mods/DynamicPals/ModActor.ModActor_C"), true);
            if (!LoaderClass || !Utils::IsObjectValid(LoaderClass)) return false;
        }

        if (!GAssetLoaderActor || !Utils::IsObjectValid(GAssetLoaderActor)) {
            GAssetLoaderActor = UObjectGlobals::FindFirstOf(STR("ModActor_C"));
        }

        if (GAssetLoaderActor && Utils::IsObjectValid(GAssetLoaderActor)) {
            
            for (const auto& path : AssetPaths) {
                GPendingAssets.insert(path);
            }

            FBatchRequest req;
            req.AssetPaths = AssetPaths;
            req.Requester = Requester;
            
            GBatchQueue.push_back(req);
            DP_LOG(Default, "[NativeAsync] Queued batch of {} assets. Queue depth: {}", AssetPaths.size(), GBatchQueue.size());
            
            if (!bIsExecutingBP) {
                ProcessNextBatch();
            }
            
            return true;
        }
        return false;
    }
}
// --- END OF FILE src/NativeAsyncLoader.cpp ---