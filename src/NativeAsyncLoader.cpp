#include "../include/NativeAsyncLoader.hpp"
#include "Utils.hpp"
#include "DataTypes.hpp"
#include "PalProcessor.hpp"
#include <new> 
#include <set>
#include <map>
#include <deque>
#include <chrono>
#include <algorithm>

using namespace RC::Unreal;

namespace DynPals {

    struct FBatchRequest {
        std::vector<std::wstring> AssetPaths;
        UObject* Requester;
        int ExplicitSwapIndex;
        bool ForceReroll;
        bool IsCompanionSync;
        bool IsEvolutionEnd;
    };

    static UClass* LoaderClass = nullptr;
    static UObject* GAssetLoaderActor = nullptr;
    
    static std::set<std::wstring> GPendingAssets;
    static std::set<std::wstring> GFailedAssets;
    static std::map<UObject*, int> GPendingCount;
    static std::map<UObject*, std::vector<std::wstring>> GActivePalBatches;
    
    static std::map<std::wstring, std::wstring> GCorrectCasingCache;

    static std::map<UObject*, std::map<std::wstring, UObject*>> GResolvedPointers;
    static UObject* GActiveRequester = nullptr;

    static std::deque<FBatchRequest> GBatchQueue;
    static FBatchRequest GCurrentBatch;

    static bool bIsExecutingBP = false;
    static std::chrono::steady_clock::time_point GCurrentRequestStartTime;

    void NativeAsyncLoader::ClearCache() {
        GPendingAssets.clear();
        GFailedAssets.clear();
        GPendingCount.clear();
        GActivePalBatches.clear();
        GBatchQueue.clear();
        GCorrectCasingCache.clear(); 
        GResolvedPointers.clear();
        GCurrentBatch = {};
        bIsExecutingBP = false;
        GAssetLoaderActor = nullptr; 
        GActiveRequester = nullptr;
    }

    bool NativeAsyncLoader::IsPending(const std::wstring& AssetPath) { return GPendingAssets.find(AssetPath) != GPendingAssets.end(); }
    bool NativeAsyncLoader::IsFailed(const std::wstring& AssetPath) { return GFailedAssets.find(AssetPath) != GFailedAssets.end(); }
    void NativeAsyncLoader::MarkAsLoaded(const std::wstring& AssetPath) { GPendingAssets.erase(AssetPath); }
    void NativeAsyncLoader::MarkAsFailed(const std::wstring& AssetPath) { GPendingAssets.erase(AssetPath); GFailedAssets.insert(AssetPath); }
    int NativeAsyncLoader::GetPendingCount(UObject* Requester) { return GPendingCount.count(Requester) ? GPendingCount[Requester] : 0; }
    void NativeAsyncLoader::RegisterPendingRequests(UObject* Requester, int Count) { GPendingCount[Requester] += Count; }
    void NativeAsyncLoader::DecrementPendingCount(UObject* Requester) { if (GPendingCount[Requester] > 0) GPendingCount[Requester]--; }

    std::wstring NativeAsyncLoader::ResolveCasing(const std::wstring& Path) {
        auto it = GCorrectCasingCache.find(Path);
        return it != GCorrectCasingCache.end() ? it->second : Path;
    }

    void NativeAsyncLoader::SetActiveRequester(UObject* Requester) {
        GActiveRequester = Requester;
    }

    UObject* NativeAsyncLoader::GetLoadedPointer(const std::wstring& Path) {
        if (!GActiveRequester || !Utils::IsObjectValid(GActiveRequester)) return nullptr;
        
        auto it = GResolvedPointers.find(GActiveRequester);
        if (it != GResolvedPointers.end()) {
            auto& palMap = it->second;
            auto assetIt = palMap.find(Path);
            if (assetIt != palMap.end() && Utils::IsObjectValid(assetIt->second)) {
                return assetIt->second;
            }
        }
        return nullptr;
    }

    void NativeAsyncLoader::ClearTemporaryPointers(UObject* Requester) {
        if (Requester) {
            GResolvedPointers.erase(Requester); 
        }
    }

    static void ProcessNextBatch() {
        if (bIsExecutingBP || GBatchQueue.empty()) return;

        bIsExecutingBP = true;
        GCurrentBatch = GBatchQueue.front();
        GBatchQueue.pop_front();
        GCurrentRequestStartTime = std::chrono::steady_clock::now();

        if (!GAssetLoaderActor || !Utils::IsObjectValid(GAssetLoaderActor)) {
    std::vector<UObject*> modActors;
    UObjectGlobals::FindAllOf(STR("ModActor_C"), modActors);
    for (UObject* actor : modActors) {
        // Ensure we grab OUR ModActor_C, not another mod's!
        if (LoaderClass && actor->GetClassPrivate() == LoaderClass) {
            GAssetLoaderActor = actor;
            break;
        }
    }
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
                            // CRITICAL FIX: Do NOT strip the _C suffix! 
                            // Packaged Shipping builds delete Blueprint assets and only keep the generated _C classes!
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
        PalProcessor::Get().ProcessPal(GCurrentBatch.Requester, GCurrentBatch.ForceReroll, GCurrentBatch.ExplicitSwapIndex, GCurrentBatch.IsCompanionSync, GCurrentBatch.IsEvolutionEnd);
        ProcessNextBatch();
    }

    void NativeAsyncLoader::OnAsyncLoadComplete(UObject* ModActor, UObject* Requester) {
    if (!ModActor || !Requester) return;

    DP_LOG(Default, "[NativeAsync] Parallel batch completion received for Pal: {}", (void*)Requester);
    
    // 1. Keep your original requester validation gate
    if (GCurrentBatch.Requester == Requester) {

        TArray<UObject*> LoadedAssets;
        bool bReadSuccess = false;

        // 2. Invoke the Blueprint function to retrieve this specific Pal's assets
        UFunction* GetFunc = ModActor->GetFunctionByNameInChain(STR("GetAndRemoveLoadedAssets"));
        if (GetFunc) {
            alignas(8) uint8_t BPParams[256] = {0};

            // Pass the Requester as the input key
            FProperty* ReqProp = GetFunc->GetPropertyByNameInChain(STR("Requester"));
            if (ReqProp) {
                UObject** Ptr = ReqProp->ContainerPtrToValuePtr<UObject*>(BPParams);
                if (Ptr) *Ptr = Requester;
            }

            Utils::SafeProcessEvent(ModActor, GetFunc, BPParams);

            // Read the output array
            FProperty* OutProp = GetFunc->GetPropertyByNameInChain(STR("OutAssets"));
            if (OutProp) {
                TArray<UObject*>* Ptr = OutProp->ContainerPtrToValuePtr<TArray<UObject*>>(BPParams);
                if (Ptr) {
                    LoadedAssets = *Ptr;
                    bReadSuccess = true;
                    
                    // Manually destruct the temporary TArray in raw memory to prevent a leak
                    Ptr->~TArray<UObject*>(); 
                }
            }
        }

        if (bReadSuccess) {
            UObject* KSL = Utils::GetKTL();
            UFunction* GetPathFunc = KSL ? KSL->GetFunctionByNameInChain(STR("GetPathName")) : nullptr;

            // 3. Preserve your original exact verification and capitalization cache loop
            for (int32_t i = 0; i < LoadedAssets.Num(); ++i) {
                UObject* Asset = LoadedAssets[i];
                if (Asset && Utils::IsObjectValid(Asset)) {
                    
                    std::wstring leafName = Asset->GetName();
                    std::wstring lowerLeafName = leafName;
                    std::transform(lowerLeafName.begin(), lowerLeafName.end(), lowerLeafName.begin(), ::towlower);

                    std::wstring correctPath = L"";
                    if (GetPathFunc) {
                        struct { UObject* Obj; FString RetVal; } Params{ Asset, FString() };
                        KSL->ProcessEvent(GetPathFunc, &Params);
                        correctPath = Utils::FStringToWString(Params.RetVal);
                    }

                    for (const auto& reqPath : GCurrentBatch.AssetPaths) {
                        std::wstring formattedReq = Utils::FormatAssetPath(reqPath);
                        std::wstring lowerReq = formattedReq;
                        std::transform(lowerReq.begin(), lowerReq.end(), lowerReq.begin(), ::towlower);

                        if (lowerReq.find(lowerLeafName) != std::wstring::npos) {
                            GResolvedPointers[Requester][reqPath] = Asset; 
                            if (!correctPath.empty()) {
                                GCorrectCasingCache[reqPath] = correctPath;
                            }
                            GPendingAssets.erase(reqPath); 
                            DP_LOG(Default, "[NativeAsync] Verified and registered pointer: '{}' (Matched: '{}')", reqPath, leafName);
                        }
                    }
                }
            }
        } else {
            DP_LOG(Error, "[NativeAsync] CRITICAL: Failed to execute 'GetAndRemoveLoadedAssets' on ModActor_C! Please ensure the Blueprint function is implemented.");
        }

        // 4. Preserve your original fallback loop for failed assets
        for (const auto& path : GCurrentBatch.AssetPaths) {
            if (GPendingAssets.count(path)) {
                GPendingAssets.erase(path);
                GFailedAssets.insert(path);
                DP_LOG(Warning, "[NativeAsync] Batch load completed but asset failed RAM verification: '{}'", path);
            }
        }

        bIsExecutingBP = false;
        GPendingCount[Requester] = 0;
        
        ProcessNextBatch();
        
        PalProcessor::Get().ProcessPal(Requester, GCurrentBatch.ForceReroll, GCurrentBatch.ExplicitSwapIndex, GCurrentBatch.IsCompanionSync, GCurrentBatch.IsEvolutionEnd);
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
            bool force = GCurrentBatch.ForceReroll;
            int expl = GCurrentBatch.ExplicitSwapIndex;
            bool comp = GCurrentBatch.IsCompanionSync;
            bool evo = GCurrentBatch.IsEvolutionEnd;
            bIsExecutingBP = false;
            
            ProcessNextBatch(); 

            if (Utils::IsObjectValid(stalledRequester)) {
                GPendingCount[stalledRequester] = 0;
                PalProcessor::Get().ProcessPal(stalledRequester, force, expl, comp, evo);
            }
        }
    }

    void NativeAsyncLoader::Initialize() {
        LoaderClass = static_cast<UClass*>(Utils::LoadAssetSafely(STR("/Game/Mods/DynamicPals/ModActor.ModActor_C")));
        GAssetLoaderActor = nullptr; 
    }

    bool NativeAsyncLoader::RequestBatchAsyncLoad(const std::vector<std::wstring>& AssetPaths, UObject* Requester, int ExplicitSwapIndex, bool ForceReroll, bool IsCompanionSync, bool IsEvolutionEnd) {
        if (!LoaderClass || !Utils::IsObjectValid(LoaderClass)) {
            LoaderClass = Utils::GetClassCached(STR("/Game/Mods/DynamicPals/ModActor.ModActor_C"), true);
            if (!LoaderClass || !Utils::IsObjectValid(LoaderClass)) return false;
        }

        if (!GAssetLoaderActor || !Utils::IsObjectValid(GAssetLoaderActor)) {
    std::vector<UObject*> modActors;
    UObjectGlobals::FindAllOf(STR("ModActor_C"), modActors);
    for (UObject* actor : modActors) {
        // Ensure we grab OUR ModActor_C, not another mod's!
        if (LoaderClass && actor->GetClassPrivate() == LoaderClass) {
            GAssetLoaderActor = actor;
            break;
        }
    }
}

        if (GAssetLoaderActor && Utils::IsObjectValid(GAssetLoaderActor)) {
            
            for (const auto& path : AssetPaths) {
                GPendingAssets.insert(path);
            }

            FBatchRequest req;
            req.AssetPaths = AssetPaths;
            req.Requester = Requester;
            req.ExplicitSwapIndex = ExplicitSwapIndex;
            req.ForceReroll = ForceReroll;
            req.IsCompanionSync = IsCompanionSync;
            req.IsEvolutionEnd = IsEvolutionEnd;
            
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