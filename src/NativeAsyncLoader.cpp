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
        std::chrono::steady_clock::time_point StartTime;
    };

    static UClass* LoaderClass = nullptr;
    static UObject* GAssetLoaderActor = nullptr;
    
    static std::set<std::wstring> GPendingAssets;
    static std::set<std::wstring> GFailedAssets;
    static std::map<UObject*, int> GPendingCount;
    
    static std::map<std::wstring, std::wstring> GCorrectCasingCache;
    static std::map<UObject*, std::map<std::wstring, UObject*>> GResolvedPointers;
    static UObject* GActiveRequester = nullptr;

    // FIFO Queue map keyed by Requester to support rapid rerolls on the same Pal actor
    static std::map<UObject*, std::deque<FBatchRequest>> GActiveBatches;

    // Global C++ pointer cache for instant cross-Pal lookups
    static std::map<std::wstring, UObject*> GGlobalPointerCache;

    void NativeAsyncLoader::ClearCache() {
        GPendingAssets.clear();
        GFailedAssets.clear();
        GPendingCount.clear();
        GCorrectCasingCache.clear(); 
        GResolvedPointers.clear();
        GActiveBatches.clear();
        GGlobalPointerCache.clear();
        GAssetLoaderActor = nullptr; 
        GActiveRequester = nullptr;
    }

    bool NativeAsyncLoader::IsPending(const std::wstring& AssetPath) { 
        return GPendingAssets.find(AssetPath) != GPendingAssets.end(); 
    }

    bool NativeAsyncLoader::IsFailed(const std::wstring& AssetPath) { 
        return GFailedAssets.find(AssetPath) != GFailedAssets.end(); 
    }

    void NativeAsyncLoader::MarkAsLoaded(const std::wstring& AssetPath) { 
        GPendingAssets.erase(AssetPath); 
    }

    void NativeAsyncLoader::MarkAsFailed(const std::wstring& AssetPath) { 
        GPendingAssets.erase(AssetPath); 
        GFailedAssets.insert(AssetPath); 
    }

    int NativeAsyncLoader::GetPendingCount(UObject* Requester) { 
        return GPendingCount.count(Requester) ? GPendingCount[Requester] : 0; 
    }

    void NativeAsyncLoader::RegisterPendingRequests(UObject* Requester, int Count) { 
        GPendingCount[Requester] += Count; 
    }

    void NativeAsyncLoader::DecrementPendingCount(UObject* Requester) { 
        if (GPendingCount[Requester] > 0) GPendingCount[Requester]--; 
    }

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

    // --- GLOBAL POINTER CACHE WITH RECYCLING VERIFICATION ---
    void NativeAsyncLoader::RegisterGlobalPointer(const std::wstring& Path, UObject* Asset) {
        if (Asset && Utils::IsObjectValid(Asset)) {
            GGlobalPointerCache[Path] = Asset;
        }
    }

    UObject* NativeAsyncLoader::GetGlobalPointer(const std::wstring& Path) {
        auto it = GGlobalPointerCache.find(Path);
        if (it != GGlobalPointerCache.end()) {
            UObject* Ptr = it->second;

            // 1. Basic memory validity check
            if (Ptr && Utils::IsObjectValid(Ptr)) {
                
                // 2. RECYCLING CHECK: Verify the object at this address still matches the asset name!
                std::wstring currentName = Ptr->GetName();
                std::wstring lowerName = currentName;
                std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::towlower);

                std::wstring lowerPath = Path;
                std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::towlower);

                // If the object name at this address matches the path, it is valid and NOT recycled
                if (lowerPath.find(lowerName) != std::wstring::npos) {
                    return Ptr;
                } else {
                    DP_LOG(Warning, "[Cache] Pointer recycling detected at {}! Expected '{}', found '{}'. Evicting.", 
                           (void*)Ptr, Path, currentName);
                }
            }

            // Evict stale or recycled pointer
            GGlobalPointerCache.erase(it);
        }
        return nullptr;
    }

    UObject* NativeAsyncLoader::FetchFromBPMasterArray(const std::wstring& Path) {
        if (!GAssetLoaderActor || !Utils::IsObjectValid(GAssetLoaderActor)) return nullptr;
        
        UFunction* GetFunc = GAssetLoaderActor->GetFunctionByNameInChain(STR("GetAllLoadedAssets"));
        if (!GetFunc) return nullptr;

        alignas(8) uint8_t BPParams[128] = {0};
        Utils::SafeProcessEvent(GAssetLoaderActor, GetFunc, BPParams);

        UObject* FoundAsset = nullptr;
        FProperty* OutProp = GetFunc->GetPropertyByNameInChain(STR("OutAssets"));
        if (OutProp) {
            TArray<UObject*>* Ptr = OutProp->ContainerPtrToValuePtr<TArray<UObject*>>(BPParams);
            if (Ptr) {
                std::wstring formattedReq = Utils::FormatAssetPath(Path);
                std::wstring lowerReq = formattedReq;
                std::transform(lowerReq.begin(), lowerReq.end(), lowerReq.begin(), ::towlower);

                for (int32_t i = 0; i < Ptr->Num(); ++i) {
                    UObject* Asset = (*Ptr)[i];
                    if (Asset && Utils::IsObjectValid(Asset)) {
                        std::wstring leafName = Asset->GetName();
                        std::wstring lowerLeafName = leafName;
                        std::transform(lowerLeafName.begin(), lowerLeafName.end(), lowerLeafName.begin(), ::towlower);

                        if (lowerReq.find(lowerLeafName) != std::wstring::npos) {
                            FoundAsset = Asset;
                            RegisterGlobalPointer(Path, Asset); // Cache in C++ for future calls
                            DP_LOG(Default, "[NativeAsync] Retrieved pointer from BP Master Array: '{}'", Path);
                            break;
                        }
                    }
                }
                Ptr->~TArray<UObject*>(); // Clean up parameter allocation
            }
        }
        return FoundAsset;
    }

    void NativeAsyncLoader::Initialize() {
        LoaderClass = static_cast<UClass*>(Utils::LoadAssetSafely(STR("/Game/Mods/DynamicPals/ModActor.ModActor_C")));
        GAssetLoaderActor = nullptr; 
    }

    bool NativeAsyncLoader::RequestBatchAsyncLoad(const std::vector<std::wstring>& AssetPaths, UObject* Requester, int ExplicitSwapIndex, bool ForceReroll, bool IsCompanionSync, bool IsEvolutionEnd) {
        if (!Requester || !Utils::IsObjectValid(Requester)) return false;

        if (!LoaderClass || !Utils::IsObjectValid(LoaderClass)) {
            LoaderClass = Utils::GetClassCached(STR("/Game/Mods/DynamicPals/ModActor.ModActor_C"), true);
            if (!LoaderClass || !Utils::IsObjectValid(LoaderClass)) return false;
        }

        if (!GAssetLoaderActor || !Utils::IsObjectValid(GAssetLoaderActor)) {
            std::vector<UObject*> modActors;
            UObjectGlobals::FindAllOf(STR("ModActor_C"), modActors);
            for (UObject* actor : modActors) {
                if (actor && actor->GetFunctionByNameInChain(STR("RequestBatchAsyncLoad"))) {
                    GAssetLoaderActor = actor;
                    break;
                }
            }
        }

        if (GAssetLoaderActor && Utils::IsObjectValid(GAssetLoaderActor)) {
            UFunction* Func = GAssetLoaderActor->GetFunctionByNameInChain(STR("RequestBatchAsyncLoad"));
            if (Func) {
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
                req.StartTime = std::chrono::steady_clock::now();
                
                GActiveBatches[Requester].push_back(req);

                alignas(8) uint8_t BPParams[256] = {0};
                TArray<FString>* AssetPathsPtr = nullptr;
                
                FProperty* AssetPathsProp = Func->GetPropertyByNameInChain(STR("AssetPaths"));
                if (AssetPathsProp) {
                    AssetPathsPtr = AssetPathsProp->ContainerPtrToValuePtr<TArray<FString>>(BPParams);
                    if (AssetPathsPtr) {
                        new (AssetPathsPtr) TArray<FString>();
                        for (const auto& path : AssetPaths) {
                            std::wstring formatted = Utils::FormatAssetPath(path);
                            AssetPathsPtr->Add(FString(formatted.c_str()));
                        }
                    }
                }

                FProperty* RequesterProp = Func->GetPropertyByNameInChain(STR("Requester"));
                if (RequesterProp) {
                    UObject** Ptr = RequesterProp->ContainerPtrToValuePtr<UObject*>(BPParams);
                    if (Ptr) *Ptr = Requester;
                }

                DP_LOG(Default, "[NativeAsync] Dispatching batch of {} assets to BP for Pal: {}", AssetPaths.size(), (void*)Requester);
                Utils::SafeProcessEvent(GAssetLoaderActor, Func, BPParams);

                if (AssetPathsPtr) {
                    AssetPathsPtr->~TArray<FString>();
                }
                return true;
            }
        }

        for (const auto& path : AssetPaths) {
            GPendingAssets.erase(path);
            GFailedAssets.insert(path);
        }
        GPendingCount[Requester] = 0;
        return false;
    }

    void NativeAsyncLoader::OnAsyncLoadComplete(UObject* ModActor, UObject* Requester) {
        if (!ModActor || !Requester) return;

        DP_LOG(Default, "[NativeAsync] Parallel batch completion received for Pal: {}", (void*)Requester);

        auto it = GActiveBatches.find(Requester);
        if (it == GActiveBatches.end() || it->second.empty()) {
            DP_LOG(Warning, "[NativeAsync] Received orphaned/invalid batch completion for Pal: {}", (void*)Requester);
            return;
        }

        FBatchRequest currentBatch = it->second.front();
        it->second.pop_front();
        if (it->second.empty()) {
            GActiveBatches.erase(it);
        }

        TArray<UObject*> LoadedAssets;
        bool bReadSuccess = false;

        UFunction* GetFunc = ModActor->GetFunctionByNameInChain(STR("GetAndRemoveLoadedAssets"));
        if (GetFunc) {
            alignas(8) uint8_t BPParams[256] = {0};

            FProperty* ReqProp = GetFunc->GetPropertyByNameInChain(STR("Requester"));
            if (ReqProp) {
                UObject** Ptr = ReqProp->ContainerPtrToValuePtr<UObject*>(BPParams);
                if (Ptr) *Ptr = Requester;
            }

            Utils::SafeProcessEvent(ModActor, GetFunc, BPParams);

            FProperty* OutProp = GetFunc->GetPropertyByNameInChain(STR("OutAssets"));
            if (OutProp) {
                TArray<UObject*>* Ptr = OutProp->ContainerPtrToValuePtr<TArray<UObject*>>(BPParams);
                if (Ptr) {
                    LoadedAssets = *Ptr;
                    bReadSuccess = true;
                    Ptr->~TArray<UObject*>();
                }
            }
        }

        if (bReadSuccess) {
            UObject* KSL = Utils::GetKTL();
            UFunction* GetPathFunc = KSL ? KSL->GetFunctionByNameInChain(STR("GetPathName")) : nullptr;

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

                    for (const auto& reqPath : currentBatch.AssetPaths) {
                        std::wstring formattedReq = Utils::FormatAssetPath(reqPath);
                        std::wstring lowerReq = formattedReq;
                        std::transform(lowerReq.begin(), lowerReq.end(), lowerReq.begin(), ::towlower);

                        if (lowerReq.find(lowerLeafName) != std::wstring::npos) {
                            GResolvedPointers[Requester][reqPath] = Asset; 
                            RegisterGlobalPointer(reqPath, Asset); // Register globally!

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
            DP_LOG(Error, "[NativeAsync] CRITICAL: Failed to execute 'GetAndRemoveLoadedAssets' on ModActor_C!");
        }

        for (const auto& path : currentBatch.AssetPaths) {
            if (GPendingAssets.count(path)) {
                GPendingAssets.erase(path);
                GFailedAssets.insert(path);
                DP_LOG(Warning, "[NativeAsync] Batch load completed but asset failed RAM verification: '{}'", path);
            }
        }

        GPendingCount[Requester] = 0;

        PalProcessor::Get().ProcessPal(
            Requester, 
            currentBatch.ForceReroll, 
            currentBatch.ExplicitSwapIndex, 
            currentBatch.IsCompanionSync, 
            currentBatch.IsEvolutionEnd
        );
    }

    void NativeAsyncLoader::Tick() {
        auto now = std::chrono::steady_clock::now();
        
        for (auto reqIt = GActiveBatches.begin(); reqIt != GActiveBatches.end(); ) {
            auto& queue = reqIt->second;
            
            while (!queue.empty()) {
                auto& frontReq = queue.front();
                auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - frontReq.StartTime).count();
                
                if (elapsedMs > 20000) { 
                    DP_LOG(Error, "[NativeAsync] TIMEOUT! Background batch load hung for >20s for Pal: {}", (void*)frontReq.Requester);
                    
                    for (const auto& path : frontReq.AssetPaths) {
                        GPendingAssets.erase(path);
                        GFailedAssets.insert(path);
                    }
                    
                    UObject* stalledRequester = frontReq.Requester;
                    bool force = frontReq.ForceReroll;
                    int expl = frontReq.ExplicitSwapIndex;
                    bool comp = frontReq.IsCompanionSync;
                    bool evo = frontReq.IsEvolutionEnd;
                    
                    queue.pop_front();
                    
                    if (Utils::IsObjectValid(stalledRequester)) {
                        GPendingCount[stalledRequester] = 0;
                        PalProcessor::Get().ProcessPal(stalledRequester, force, expl, comp, evo);
                    }
                } else {
                    break; 
                }
            }

            if (queue.empty()) {
                reqIt = GActiveBatches.erase(reqIt);
            } else {
                ++reqIt;
            }
        }
    }
}