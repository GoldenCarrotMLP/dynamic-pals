// --- START OF FILE src/PalProcessor.cpp ---
#include "PalProcessor.hpp"
#include "ConfigManager.hpp"
#include "SaveManager.hpp"
#include "Utils.hpp"
#include "AsyncHelper.hpp"
#include "VFXManager.hpp"
#include "../include/NativeAsyncLoader.hpp"
#include <random>
#include <thread>
#include <algorithm>

using namespace RC;
using namespace RC::Unreal;

namespace DynPals {
    struct FPalPropertyCache {
        UFunction* GetPalCharactersFunc = nullptr;
        UFunction* IsOtomoFunc = nullptr;
        UFunction* IsBaseCampPalFunc = nullptr;
        
        FProperty* CharParamCompProp = nullptr;
        FProperty* IndivParamProp = nullptr;
        FProperty* IndivIdProp = nullptr;
        
        UFunction* GetLevelFunc = nullptr;
        UFunction* GetRankFunc = nullptr;
        UFunction* GetFriendshipRankFunc = nullptr;
        UFunction* GetFriendshipPointFunc = nullptr;

        UFunction* GetCharacterIDFromCharacterFunc = nullptr;
        UFunction* IsWildNPCFunc = nullptr;
        UFunction* IsRarePalFunc = nullptr;
        UFunction* GetGenderTypeFunc = nullptr;
        UFunction* GetSkinNameFunc = nullptr;
        UFunction* GetPassiveSkillListFunc = nullptr;
        UFunction* GetDatabaseCharacterParameterFunc = nullptr;
        UFunction* GetBPClassFunc = nullptr;
        
        bool bIsGlobalsInit = false;
        bool bIsStatsInit = false;
        bool bIsPropsInit = false;
        bool bIsCoreGlobalsInit = false;
    };
    
    static FPalPropertyCache GCachedProps;

    PalRuntimeStats RetrievePalStats(UObject* IndivParam, const std::wstring& RawCharID, const std::wstring& InstanceID, bool bLogWarnings) {
        PalRuntimeStats stats;
        stats.Level = -1;
        stats.Rank = -1;
        stats.Friendship = -1;
        if (!IndivParam || !Utils::IsObjectValid(IndivParam)) return stats;

        if (!GCachedProps.bIsStatsInit) {
            GCachedProps.GetLevelFunc = IndivParam->GetFunctionByNameInChain(STR("GetLevel"));
            GCachedProps.GetRankFunc = IndivParam->GetFunctionByNameInChain(STR("GetRank"));
            GCachedProps.GetFriendshipRankFunc = IndivParam->GetFunctionByNameInChain(STR("GetFriendshipRank"));
            GCachedProps.GetFriendshipPointFunc = IndivParam->GetFunctionByNameInChain(STR("GetFriendshipPoint"));
            
            if (GCachedProps.GetLevelFunc) {
                GCachedProps.bIsStatsInit = true;
            }
        }

        struct { int32_t RetVal = -1; } IntParams;

        if (GCachedProps.GetLevelFunc) {
            IntParams.RetVal = -1;
            Utils::SafeProcessEvent(IndivParam, GCachedProps.GetLevelFunc, &IntParams);
            stats.Level = IntParams.RetVal;
        }

        if (GCachedProps.GetRankFunc) {
            IntParams.RetVal = -1;
            Utils::SafeProcessEvent(IndivParam, GCachedProps.GetRankFunc, &IntParams);
            stats.Rank = IntParams.RetVal;
        }

        if (GCachedProps.GetFriendshipRankFunc) {
            IntParams.RetVal = -1;
            Utils::SafeProcessEvent(IndivParam, GCachedProps.GetFriendshipRankFunc, &IntParams);
            stats.Friendship = IntParams.RetVal;
        } else if (GCachedProps.GetFriendshipPointFunc) {
            IntParams.RetVal = -1;
            Utils::SafeProcessEvent(IndivParam, GCachedProps.GetFriendshipPointFunc, &IntParams);
            stats.Friendship = IntParams.RetVal;
        }

        if (stats.Level == -1) stats.Level = 1;
        if (stats.Rank == -1) stats.Rank = 0;
        if (stats.Friendship == -1) stats.Friendship = 0;

        return stats;
    }

    void PalProcessor::ClearAllSwappedStatus() {
        std::lock_guard<std::mutex> lock(QueueMutex);
        SwapQueue.clear();
        SwappedInstances.clear();
        ActivePalsByInstanceID.clear();
        RuntimeStatsCache.clear();
        ProcessedPals.clear();
        ProcessingQueue.clear();
    }

    void PalProcessor::ClearSwappedStatus(const std::wstring& InstanceID, RC::Unreal::UObject* Character) {
        if (Character) {
            SwappedInstances.erase(Character);
        }
        ActivePalsByInstanceID.erase(InstanceID);
        RuntimeStatsCache.erase(InstanceID);
    }

    static std::map<std::wstring, std::wstring> GBPClassCache;

    static bool ResolvePalBlueprintPath(UObject* WorldContext, const std::wstring& CharID, std::wstring& OutPath) {
        // --- 0.0ms Database Cache ---
        if (GBPClassCache.count(CharID)) {
            OutPath = GBPClassCache[CharID];
            return !OutPath.empty();
        }

        if (!WorldContext || !Utils::IsObjectValid(WorldContext)) return false;

        UObject* PalUtil = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
        if (!PalUtil || !Utils::IsObjectValid(PalUtil)) return false;

        struct { UObject* WorldContext; UObject* DB; } GetDBParams{ WorldContext, nullptr };
        if (!GCachedProps.GetDatabaseCharacterParameterFunc) {
            GCachedProps.GetDatabaseCharacterParameterFunc = PalUtil->GetFunctionByNameInChain(STR("GetDatabaseCharacterParameter"));
        }
        if (!GCachedProps.GetDatabaseCharacterParameterFunc) return false;
        
        Utils::SafeProcessEvent(PalUtil, GCachedProps.GetDatabaseCharacterParameterFunc, &GetDBParams);
        UObject* DB = GetDBParams.DB;
        if (!DB || !Utils::IsObjectValid(DB)) return false;

        if (!GCachedProps.GetBPClassFunc) {
            GCachedProps.GetBPClassFunc = DB->GetFunctionByNameInChain(STR("GetBPClass"));
        }
        if (!GCachedProps.GetBPClassFunc) return false;

        struct { FName RowName; bool bShowError; uint8_t Pad[7]; AltrSoftObjectPtr ReturnValue; } Params;
        Params.RowName = FName(CharID.c_str(), FNAME_Add);
        Params.bShowError = false;

        Utils::SafeProcessEvent(DB, GCachedProps.GetBPClassFunc, &Params);

        std::wstring packageName = Params.ReturnValue.ObjectID.PackageName.ToString();
        std::wstring assetName = Params.ReturnValue.ObjectID.AssetName.ToString();

        if (!packageName.empty() && !assetName.empty()) {
            OutPath = packageName + L"." + assetName;
            GBPClassCache[CharID] = OutPath;
            return true;
        }
        GBPClassCache[CharID] = L"";
        return false;
    }

    static std::wstring ResolveAnimPath(UObject* Character, const std::wstring& AnimTarget, const std::wstring& CharID) {
        if (AnimTarget.empty()) return L"";
        
        auto ToLowerW = [](std::wstring str) {
            std::transform(str.begin(), str.end(), str.begin(), ::towlower);
            return str;
        };

        if (ToLowerW(AnimTarget) == ToLowerW(CharID)) return L""; 

        if (AnimTarget.find(L'/') == std::wstring::npos) {
            std::wstring ResolvedPath;
            if (ResolvePalBlueprintPath(Character, AnimTarget, ResolvedPath)) {
                return ResolvedPath;
            }
            std::wstring TryPath1 = L"/Game/Pal/Blueprint/Character/Monster/PalActorBP/" + AnimTarget + L"/BP_" + AnimTarget + L".BP_" + AnimTarget + L"_C";
            return TryPath1;
        }
        return AnimTarget;
    }

    static bool IsPalBlueprintValid(UObject* Pal, std::wstring& OutBlueprintName) {
        if (!Utils::IsObjectValid(Pal)) return false;

        UClass* PalClass = Pal->GetClassPrivate();
        if (!Utils::IsObjectValid(PalClass)) {
            DP_LOG(Warning, "WARNING: Spawned Actor has NO VALID BLUEPRINT CLASS! Aborting.");
            return false;
        }
        
        OutBlueprintName = PalClass->GetName();
        if (OutBlueprintName.empty() || OutBlueprintName.find(L"Default__") != std::wstring::npos) return false;

        bool bBeingDestroyed = false;
        if (Utils::GetPropertyValue<bool>(Pal, STR("bActorIsBeingDestroyed"), bBeingDestroyed, true) && bBeingDestroyed) {
            DP_LOG(Verbose, "Pal '{}' is being destroyed. Skipping.", OutBlueprintName);
            return false;
        }

        bool bHidden = false;
        if (Utils::GetPropertyValue<bool>(Pal, STR("bHidden"), bHidden, true) && bHidden) {
            if (OutBlueprintName.find(L"FunnelCharacter") != std::wstring::npos) return false;
        }

        return true;
    }

    std::wstring PalProcessor::StripCharacterPrefix(const std::wstring& InputID) {
        if (InputID.rfind(L"BOSS_", 0) == 0) return InputID.substr(5);
        if (InputID.rfind(L"RAID_", 0) == 0) return InputID.substr(5);
        if (InputID.rfind(L"GYM_", 0) == 0) return InputID.substr(4);
        if (InputID.rfind(L"PREDATOR_", 0) == 0) return InputID.substr(9); 
        return InputID;
    }

    void PalProcessor::ScanActivePals() {
        return;
    }

    void PalProcessor::DelayedSwap(UObject* Character, int SwapIndex, const std::wstring& CompName) {
        if (!Character || !Utils::IsObjectValid(Character)) return;
        
        float DelaySeconds = VFXManager::Get().PlayComposition(Character, CompName);
        int DelayMs = static_cast<int>(DelaySeconds * 1000.0f);
        ForceSwap(Character, SwapIndex, DelayMs);
    }

    void PalProcessor::DelayedReroll(UObject* Character, const std::wstring& CompName) {
        if (!Character || !Utils::IsObjectValid(Character)) return;
        
        float DelaySeconds = VFXManager::Get().PlayComposition(Character, CompName);
        int DelayMs = static_cast<int>(DelaySeconds * 1000.0f);
        
        std::thread([Character, DelayMs]() {
            std::this_thread::sleep_for(std::chrono::seconds(DelayMs));
            AsyncHelper::AsyncTask(ENamedThreads::GameThread, [Character]() {
                if (!Utils::IsObjectValid(Character)) return; 
                PalProcessor::Get().ProcessPal(Character, true);
            });
        }).detach();
    }

    void PalProcessor::ForceSwap(UObject* Character, int SwapIndex, int DelayMs) {
        if (!Utils::IsObjectValid(Character) || SwapIndex < 0 || SwapIndex >= (int)ConfigManager::Get().GetConfigs().size()) return;

        UObject* ParamComp = nullptr;
        Utils::GetPropertyValue<UObject*>(Character, STR("CharacterParameterComponent"), ParamComp);
        if (!ParamComp || !Utils::IsObjectValid(ParamComp)) return;

        UObject* IndivParam = nullptr;
        Utils::GetPropertyValue<UObject*>(ParamComp, STR("IndividualParameter"), IndivParam);
        if (!IndivParam || !Utils::IsObjectValid(IndivParam)) return;

        FPalInstanceID InstanceIDStruct;
        if (!Utils::GetPropertyValue<FPalInstanceID>(IndivParam, STR("IndividualId"), InstanceIDStruct)) return;
        
        std::wstring InstanceID = Utils::GuidToWString(InstanceIDStruct.InstanceId);

        ClearSwappedStatus(InstanceID, Character);

        auto& config = ConfigManager::Get().GetConfigs()[SwapIndex];

        PalPersistData* ExistingData = SaveManager::Get().GetPersistData(InstanceID);
        if (!ExistingData) {
            PalPersistData newData;
            newData.InstanceID = InstanceID;
            newData.PackName = config.PackName;
            newData.SkinName = config.SkinName;
            newData.SwapLabel = config.SwapLabel; 
            newData.SkelMeshPath = config.SkelMeshPath;
            SaveManager::Get().SetPersistData(InstanceID, newData, true); 
        } else {
            ExistingData->PackName = config.PackName;
            ExistingData->SkinName = config.SkinName;
            ExistingData->SwapLabel = config.SwapLabel;
            ExistingData->SkelMeshPath = config.SkelMeshPath;
            
            ExistingData->MorphSet.clear();
            ExistingData->MatSet.clear();
            ExistingData->MatColorSet.clear();
            
            SaveManager::Get().SetPersistData(InstanceID, *ExistingData, true); 
        }

        std::thread([Character, SwapIndex, DelayMs]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(DelayMs));
            
            AsyncHelper::AsyncTask(ENamedThreads::GameThread, [Character, SwapIndex]() {
                if (!Utils::IsObjectValid(Character)) return; 
                PalProcessor::Get().ProcessPal(Character, false, SwapIndex);
            });
        }).detach();
    }

    int PalProcessor::EvaluateIdealSwapIndex(UObject* Character, std::wstring& OutInstanceID) {
        return -1; 
    }

    void PalProcessor::ProcessPal(UObject* Character, bool ForceReroll, int ExplicitSwapIndex, bool IsCompanionSync) {
        if (!Character || !Utils::IsObjectValid(Character)) return;

        std::lock_guard<std::mutex> lock(QueueMutex);
        for (auto& q : SwapQueue) {
            if (q.Character == Character) {
                if (ForceReroll) q.ForceReroll = true;
                if (ExplicitSwapIndex != -1) q.ExplicitSwapIndex = ExplicitSwapIndex;
                if (IsCompanionSync) q.IsCompanionSync = true;
                return;
            }
        }
        SwapQueue.push_back({Character, ForceReroll, ExplicitSwapIndex, IsCompanionSync});
    }

    void PalProcessor::CheckAndTriggerUpdate(UObject* Character) {
        ProcessPal(Character, false);
    }

    void PalProcessor::ProcessPlayerParty(UObject* WorldContext) {
        if (!WorldContext || !Utils::IsObjectValid(WorldContext)) return;

        UObject* PalUtil = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
        if (!PalUtil || !Utils::IsObjectValid(PalUtil)) return;

        struct { UObject* WorldContext; UObject* ReturnValue; } HolderParams{ WorldContext, nullptr };
        Utils::SafeProcessEvent(PalUtil, PalUtil->GetFunctionByNameInChain(STR("GetOtomoHolderComponent")), &HolderParams);
        UObject* OtomoHolder = HolderParams.ReturnValue;
        if (!OtomoHolder || !Utils::IsObjectValid(OtomoHolder)) return;

        struct { UObject* WorldContext; UObject* ReturnValue; } FunnelMgrParams{ WorldContext, nullptr };
        Utils::SafeProcessEvent(PalUtil, PalUtil->GetFunctionByNameInChain(STR("GetFunnelCharacterManager")), &FunnelMgrParams);
        UObject* FunnelManager = FunnelMgrParams.ReturnValue;

        for (int32_t i = 0; i < 5; ++i) {
            struct { int32_t SlotIndex; UObject* ReturnValue; } OtomoParams{ i, nullptr };
            Utils::SafeProcessEvent(OtomoHolder, OtomoHolder->GetFunctionByNameInChain(STR("TryGetOtomoActorBySlotIndex")), &OtomoParams);
            UObject* OtomoChar = OtomoParams.ReturnValue;
            
            if (OtomoChar && Utils::IsObjectValid(OtomoChar)) {
                ProcessPal(OtomoChar, false);

                if (FunnelManager && Utils::IsObjectValid(FunnelManager)) {
                    struct { UObject* Owner; UObject* ReturnValue; } FunnelParams{ OtomoChar, nullptr };
                    Utils::SafeProcessEvent(FunnelManager, FunnelManager->GetFunctionByNameInChain(STR("GetFunnelCharacterByOwner")), &FunnelParams);
                    UObject* FunnelChar = FunnelParams.ReturnValue;
                    
                    if (FunnelChar && Utils::IsObjectValid(FunnelChar)) {
                        ProcessPal(FunnelChar, false);
                    }
                }
            }
        }
    }

    void PalProcessor::Tick() {
        UObject* KSL = Utils::GetKismetSystemLibrary();
        static UFunction* IsValidFunc = Utils::GetKismetFunction(STR("IsValid"));
        if (!KSL || !IsValidFunc) return;

        QueuedSwap req;
        {
            std::lock_guard<std::mutex> lock(QueueMutex);
            if (SwapQueue.empty()) return;
            
            req = SwapQueue.front();
            SwapQueue.pop_front();
        }

        UObject* TargetChar = req.Character;

        if (Utils::IsObjectTracked(TargetChar) && Utils::IsObjectValid(TargetChar)) {
            ExecuteSwap(TargetChar, req.ForceReroll, req.ExplicitSwapIndex, req.IsCompanionSync);
        }

        static int pruneCounter = 0;
        pruneCounter++;
        if (pruneCounter > 500) {
            pruneCounter = 0;
            for (auto it = SwappedInstances.begin(); it != SwappedInstances.end(); ) {
                if (!Utils::IsObjectValid(it->first)) {
                    it = SwappedInstances.erase(it);
                } else {
                    ++it;
                }
            }
            for (auto it = ActivePalsByInstanceID.begin(); it != ActivePalsByInstanceID.end(); ) {
                auto& palSet = it->second;
                for (auto setIt = palSet.begin(); setIt != palSet.end(); ) {
                    if (!Utils::IsObjectValid(*setIt)) {
                        setIt = palSet.erase(setIt);
                    } else {
                        ++setIt;
                    }
                }
                if (palSet.empty()) {
                    it = ActivePalsByInstanceID.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    bool PalProcessor::ExecuteSwap(UObject* Character, bool ForceReroll, int ExplicitSwapIndex, bool IsCompanionSync) {
        if (!Character) return false;
        
        if (!Utils::IsObjectValid(Character)) {
            return false;
        }
        
        UObject* ParamComp = nullptr;
        Utils::GetPropertyValue<UObject*>(Character, STR("CharacterParameterComponent"), ParamComp, true);
        if (!ParamComp || !Utils::IsObjectValid(ParamComp)) {
            return false;
        }

        UObject* IndivParam = nullptr;
        Utils::GetPropertyValue<UObject*>(ParamComp, STR("IndividualParameter"), IndivParam, true);
        if (!IndivParam || !Utils::IsObjectValid(IndivParam)) {
            return false;
        }

        FPalInstanceID InstanceIDStruct;
        if (!Utils::GetPropertyValue<FPalInstanceID>(IndivParam, STR("IndividualId"), InstanceIDStruct, true)) {
            return false;
        }
        if (!InstanceIDStruct.InstanceId.IsValid()) {
            return false;
        } 

        std::wstring InstanceID = Utils::GuidToWString(InstanceIDStruct.InstanceId);

        {
            auto& palSet = ActivePalsByInstanceID[InstanceID];
            for (auto it = palSet.begin(); it != palSet.end(); ) {
                if (!Utils::IsObjectValid(*it)) {
                    it = palSet.erase(it);
                } else {
                    ++it;
                }
            }
            palSet.insert(Character);
        }

        std::wstring BlueprintName = L"";
        if (!IsPalBlueprintValid(Character, BlueprintName)) {
            return false;
        }

        UObject* Level = Character->GetOuterPrivate();
        if (!Level || !Utils::IsObjectValid(Level)) {
            return false;
        }
        UObject* World = Level->GetOuterPrivate();
        if (!World || !Utils::IsObjectValid(World) || World->GetClassPrivate()->GetName() != L"World") {
            return false;
        }

        static UObject* PalUtil = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
        if (!PalUtil || !Utils::IsObjectValid(PalUtil)) {
            return false;
        }

        if (!GCachedProps.bIsCoreGlobalsInit) {
            GCachedProps.GetCharacterIDFromCharacterFunc = PalUtil->GetFunctionByNameInChain(STR("GetCharacterIDFromCharacter"));
            GCachedProps.IsWildNPCFunc = PalUtil->GetFunctionByNameInChain(STR("IsWildNPC"));
            
            if (IndivParam && Utils::IsObjectValid(IndivParam)) {
                GCachedProps.IsRarePalFunc = IndivParam->GetFunctionByNameInChain(STR("IsRarePal"));
                GCachedProps.GetGenderTypeFunc = IndivParam->GetFunctionByNameInChain(STR("GetGenderType"));
                GCachedProps.GetSkinNameFunc = IndivParam->GetFunctionByNameInChain(STR("GetSkinName"));
                GCachedProps.GetPassiveSkillListFunc = IndivParam->GetFunctionByNameInChain(STR("GetPassiveSkillList"));
            }
            GCachedProps.bIsCoreGlobalsInit = true;
        }

        struct { UObject* Char; FName RetVal; } CharIDParams{Character, FName()};
        if (GCachedProps.GetCharacterIDFromCharacterFunc) {
            Utils::SafeProcessEvent(PalUtil, GCachedProps.GetCharacterIDFromCharacterFunc, &CharIDParams);
        }
        std::wstring RawCharID = CharIDParams.RetVal.ToString();

        if (RawCharID.rfind(L"GYM_", 0) == 0 || RawCharID.find(L"_Gym_") != std::wstring::npos) {
            return false;
        }

        static UObject* LastWorldLoaded = nullptr;
        if (World != LastWorldLoaded) {
            SaveManager::Get().LoadWorldData(World);
            LastWorldLoaded = World;
        }
        PalPersistData* ExistingData = SaveManager::Get().GetPersistData(InstanceID);

        std::wstring CharID = StripCharacterPrefix(RawCharID);

        PalRuntimeStats stats = RetrievePalStats(IndivParam, RawCharID, InstanceID, true);
        int LevelNum = stats.Level;
        int RankNum = stats.Rank;
        int FriendshipNum = stats.Friendship;

        struct { UObject* Actor; bool RetVal; } WildParams{Character, false};
        if (GCachedProps.IsWildNPCFunc) Utils::SafeProcessEvent(PalUtil, GCachedProps.IsWildNPCFunc, &WildParams);
        bool IsWild = WildParams.RetVal;

        struct { bool ReturnValue; } RareParams{false};
        if (GCachedProps.IsRarePalFunc) Utils::SafeProcessEvent(IndivParam, GCachedProps.IsRarePalFunc, &RareParams);
        bool IsRare = RareParams.ReturnValue;

        struct { uint8_t RetVal; } GenderParams{0};
        if (GCachedProps.GetGenderTypeFunc) Utils::SafeProcessEvent(IndivParam, GCachedProps.GetGenderTypeFunc, &GenderParams);
        std::wstring GenderStr = (GenderParams.RetVal == 1) ? L"Male" : ((GenderParams.RetVal == 2) ? L"Female" : L"None");

        struct { FName RetVal; } SkinParams{FName()};
        if (GCachedProps.GetSkinNameFunc) Utils::SafeProcessEvent(IndivParam, GCachedProps.GetSkinNameFunc, &SkinParams);
        std::wstring SkinName = SkinParams.RetVal.ToString();
        if (SkinName == L"None") SkinName = L"";

        std::vector<std::wstring> Traits;
        struct { TArray<FName> RetVal; } TraitsParams;
        if (GCachedProps.GetPassiveSkillListFunc) {
            Utils::SafeProcessEvent(IndivParam, GCachedProps.GetPassiveSkillListFunc, &TraitsParams);
            for (int32_t i = 0; i < TraitsParams.RetVal.Num(); ++i) {
                Traits.push_back(TraitsParams.RetVal[i].ToString());
            }
        }

        PalRuntimeStats& CachedStats = RuntimeStatsCache[InstanceID];

        std::wstring CurrentSwapLabel = ExistingData ? ExistingData->SwapLabel : L"";
        
        bool bLiveEventTriggered = (CachedStats.Level != -1); 

        CachedStats.Level = LevelNum;
        CachedStats.Rank = RankNum;
        CachedStats.Friendship = FriendshipNum;

        int currentSwap = -1;
        if (ExistingData && ExistingData->HasSavedSwap()) {
            currentSwap = ConfigManager::Get().FindConfigIndex(ExistingData->PackName, ExistingData->SkinName, ExistingData->SwapLabel, ExistingData->SkelMeshPath, CharID);
        }

        auto evaluations = ConfigManager::Get().EvaluateAllSwaps(CharID, IsRare, GenderStr, Traits, LevelNum, SkinName, RankNum, FriendshipNum, IsWild, CurrentSwapLabel);
        int newBestSwap = ConfigManager::Get().PickBestSwap(evaluations);

        bool bManualLockState = ExistingData ? ExistingData->bIsManuallyLocked : false;
        int finalSwap = -1;

        if (ExplicitSwapIndex != -1) {
            finalSwap = ExplicitSwapIndex;
            
            bool bIsSelectedSwapValid = false;
            for (const auto& ev : evaluations) {
                if (ev.ConfigIndex == finalSwap) {
                    bIsSelectedSwapValid = ev.IsValid;
                    break;
                }
            }
            
            bManualLockState = !bIsSelectedSwapValid;
            if (bManualLockState) {
                DP_LOG(Default, "Explicit swap is invalid for this Pal. Engaging Manual Lock.");
            } else {
                DP_LOG(Default, "Explicit swap is valid. Manual Lock disengaged.");
            }
        } 
        else if (ForceReroll) {
            finalSwap = newBestSwap;
            bManualLockState = false; 
            DP_LOG(Default, "Pal rerolled. Manual Lock disengaged.");
        }
        else {
            finalSwap = currentSwap;
            
            if (!bManualLockState) {
                if (currentSwap != -1) {
                    if (bLiveEventTriggered) {
                        const SwapEvaluation* currentEval = nullptr;
                        for (const auto& ev : evaluations) {
                            if (ev.ConfigIndex == currentSwap) {
                                currentEval = &ev;
                                break;
                            }
                        }

                        if (currentEval) {
                            int absoluteBestScore = 999999;
                            for (const auto& ev : evaluations) {
                                if (ev.IsValid && ev.Score < absoluteBestScore) {
                                    absoluteBestScore = ev.Score;
                                }
                            }

                            if (!currentEval->IsValid || currentEval->Score > absoluteBestScore) {
                                DP_LOG(Normal, "Live Event: Better skin found or current became invalid. Upgrading skin.\n");
                                finalSwap = newBestSwap;
                            } else {
                                finalSwap = currentSwap;
                            }
                        } else {
                            finalSwap = newBestSwap;
                        }
                    } else {
                        finalSwap = currentSwap;
                    }
                } else {
                    finalSwap = newBestSwap;
                }
            } else {
                if (bLiveEventTriggered) {
                    DP_LOG(Verbose, "Live Event ignored: Pal is Manually Locked to its current skin.");
                }
            }
        }

        if (finalSwap != -1) {
            auto activeIt = SwappedInstances.find(Character);
            bool bIsNewActor = (activeIt == SwappedInstances.end());

            auto& finalConfig = ConfigManager::Get().GetConfigs()[finalSwap];

            bool bNeedsApply = (ExplicitSwapIndex != -1) || ForceReroll || (finalSwap != currentSwap) || bIsNewActor;
            if (!bNeedsApply && activeIt != SwappedInstances.end() && activeIt->second != finalConfig.SwapLabel) {
                bNeedsApply = true;
            }
            
            if (bNeedsApply) {
                
                DP_LOG(Default, "[Debug Swap] Proceeding to Swap Pal '{}' (ID: '{}', Actor: {}). Reason: {}", 
                    RawCharID, InstanceID, (void*)Character,
                    (ExplicitSwapIndex != -1) ? L"Explicit Selection" : 
                    (ForceReroll) ? L"Force Reroll" : 
                    (finalSwap != currentSwap) ? L"Skin Changed" : L"New Actor Spawned");

                bool bIsLiveEvolution = bLiveEventTriggered && !bIsNewActor && (finalSwap != currentSwap) && (ExplicitSwapIndex == -1) && !ForceReroll;

                if (bIsLiveEvolution) {
                    DP_LOG(Normal, "Live Evolution Triggered! Deferring physical swap for visual composition...");
                    DelayedSwap(Character, finalSwap, L"evolve_1");
                    return true; 
                }

                PalPersistData newData = ExistingData ? *ExistingData : PalPersistData{ InstanceID, L"", L"", L"", {} };
                newData.bIsManuallyLocked = bManualLockState; 

                newData.PackName = finalConfig.PackName;
                newData.SkinName = finalConfig.SkinName;
                newData.SwapLabel = finalConfig.SwapLabel;
                newData.SkelMeshPath = finalConfig.SkelMeshPath;

                if (ForceReroll || ExplicitSwapIndex != -1 || finalSwap != currentSwap) {
                    newData.MorphSet.clear();
                    newData.MatSet.clear();
                    newData.MatColorSet.clear(); 
                }

                // --- ASYNC MULTI-ASSET PRE-CACHING INTERCEPT ---
                std::vector<std::wstring> assetsToLoad;

                auto CheckDependency = [&](const std::wstring& Path) {
                    if (Path.empty()) return;
                    
                    bool bIsMeshLoaded = (Path == finalConfig.SkelMeshPath) ? Utils::IsSkeletalMeshLoaded(Path) : Utils::IsAssetLoaded(Path);
                    if (bIsMeshLoaded) {
                        if (NativeAsyncLoader::IsPending(Path)) NativeAsyncLoader::MarkAsLoaded(Path);
                        return; 
                    }
                    
                    if (NativeAsyncLoader::IsFailed(Path)) return; 

                    assetsToLoad.push_back(Path);
                };

                // 1. Check Skeletal Mesh
                CheckDependency(finalConfig.SkelMeshPath);

                // 2. Check Animation Blueprint
                std::wstring ResolvedAnimPath = ResolveAnimPath(Character, finalConfig.AnimTarget, CharID);
                if (!ResolvedAnimPath.empty()) {
                    CheckDependency(ResolvedAnimPath);
                    
                    std::vector<std::wstring> StandardLayers = {
                        L"/Game/Pal/Blueprint/Character/Monster/ALI_MonsterBase.ALI_MonsterBase_C",
                        L"/Game/Pal/Blueprint/Character/Monster/ALI_MonsterPhysics.ALI_MonsterPhysics_C"
                    };
                    for (const auto& layer : StandardLayers) {
                        CheckDependency(layer);
                    }
                } 
                // We REMOVED the VanillaClassPath check entirely. The spawned Actor IS the valid BP.
                // This natively handles BOSS_, RAID_, and GYM_ BP overrides without string guessing!

                // 3. Check Material Replacements
                for (const auto& mat : finalConfig.MatReplaceList) {
                    std::wstring chosenPath = mat.matPath;
                    if (mat.matPath.length() >= 2 && mat.matPath.substr(mat.matPath.length() - 2) == L"/*") {
                        std::wstring VirtualFolder = mat.matPath.substr(0, mat.matPath.length() - 2);
                        std::wstring savedPath = L"";
                        if (ExistingData) {
                            auto savedMatIt = ExistingData->MatSet.find(mat.index);
                            if (savedMatIt != ExistingData->MatSet.end()) savedPath = savedMatIt->second;
                        }

                        if (!savedPath.empty()) {
                            chosenPath = savedPath;
                        } else {
                            std::vector<std::wstring> AvailableMats = Utils::GetAssetsInVirtualFolder(VirtualFolder);
                            if (!AvailableMats.empty()) {
                                for (const auto& path : AvailableMats) CheckDependency(path);
                                chosenPath = L""; 
                            }
                        }
                    }
                    CheckDependency(chosenPath);
                }

                // Trigger Parallel Async Load for all missing assets
                if (!assetsToLoad.empty()) {
                    SaveManager::Get().SetPersistData(InstanceID, newData, false);
                    NativeAsyncLoader::RegisterPendingRequests(Character, static_cast<int>(assetsToLoad.size()));
                    
                    // Call the new C++ Batch Loader in a single, un-throttled, stutter-free call!
                    if (NativeAsyncLoader::RequestBatchAsyncLoad(assetsToLoad, Character)) {
                        return true; // Halt swap. Wait for the single completed batch callback
                    } else {
                        // Fallback
                        static bool bWarned = false;
                        if (!bWarned) {
                            DP_LOG(Warning, "ModActor .pak not found! Falling back to blocking load.");
                            bWarned = true;
                        }
                    }
                } else if (NativeAsyncLoader::GetPendingCount(Character) > 0) {
                    return true;
                }

                if (!finalConfig.SetNickname.empty()) {
                    bool bNicknameIsEmpty = false;
                    FProperty* SaveParamProp = Utils::GetProperty(IndivParam, STR("SaveParameter"));
                    if (SaveParamProp) {
                        void* SaveParamPtr = SaveParamProp->ContainerPtrToValuePtr<void>(IndivParam);
                        if (SaveParamPtr) {
                            FStructProperty* StructProp = CastField<FStructProperty>(SaveParamProp);
                            if (StructProp) {
                                UStruct* SaveParamStruct = StructProp->GetStruct();
                                if (SaveParamStruct) {
                                    FProperty* NickNameProp = SaveParamStruct->GetPropertyByNameInChain(STR("NickName"));
                                    if (NickNameProp) {
                                        FString* pNickName = NickNameProp->ContainerPtrToValuePtr<FString>(SaveParamPtr);
                                        if (pNickName) {
                                            std::wstring_view curName = pNickName->GetCharArray().GetData() ? pNickName->GetCharArray().GetData() : L"";
                                            if (curName.empty()) {
                                                bNicknameIsEmpty = true;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    bool bShouldSetNickname = ForceReroll || (ExplicitSwapIndex != -1) || (finalSwap != currentSwap) || bNicknameIsEmpty;

                    if (bShouldSetNickname) {
                        if (SaveParamProp) {
                            void* SaveParamPtr = SaveParamProp->ContainerPtrToValuePtr<void>(IndivParam);
                            if (SaveParamPtr) {
                                FStructProperty* StructProp = CastField<FStructProperty>(SaveParamProp);
                                if (StructProp) {
                                    UStruct* SaveParamStruct = StructProp->GetStruct();
                                    if (SaveParamStruct) {
                                        FString newNameStr(finalConfig.SetNickname.c_str());

                                        FProperty* NickNameProp = SaveParamStruct->GetPropertyByNameInChain(STR("NickName"));
                                        if (NickNameProp) {
                                            void* pNickName = NickNameProp->ContainerPtrToValuePtr<void>(SaveParamPtr);
                                            if (pNickName) NickNameProp->CopyCompleteValue(pNickName, &newNameStr);
                                        }

                                        FProperty* FilteredNickNameProp = SaveParamStruct->GetPropertyByNameInChain(STR("FilteredNickName"));
                                        if (FilteredNickNameProp) {
                                            void* pFilteredNickName = FilteredNickNameProp->ContainerPtrToValuePtr<void>(SaveParamPtr);
                                            if (pFilteredNickName) FilteredNickNameProp->CopyCompleteValue(pFilteredNickName, &newNameStr);
                                        }
                                    }
                                }
                            }
                        }

                        if (!IsWild) {
                            UObject* PlayerController = UObjectGlobals::FindFirstOf(STR("PalPlayerController"));
                            if (PlayerController && Utils::IsObjectValid(PlayerController)) {
                                UFunction* UpdateNameFunc = PlayerController->GetFunctionByNameInChain(STR("UpdateCharacterNickName_ToServer"));
                                if (UpdateNameFunc) {
                                    struct { 
                                        FPalInstanceID InstanceId; 
                                        FString NewNickName; 
                                    } Params;
                                    Params.InstanceId = InstanceIDStruct;
                                    Params.NewNickName = FString(finalConfig.SetNickname.c_str());
                                    
                                    Utils::SafeProcessEvent(PlayerController, UpdateNameFunc, &Params);
                                }
                            }
                        }

                        DP_LOG(Default, "[Nickname] Applied name update for '{}' -> '{}' (Wild: {})", InstanceID, finalConfig.SetNickname, IsWild ? L"True" : L"False");
                    }
                }

                ApplySwap(Character, finalConfig, newData);

                bool bIsManualAction = (ExplicitSwapIndex != -1) || ForceReroll;
                SaveManager::Get().SetPersistData(InstanceID, newData, bIsManualAction);

                SwappedInstances[Character] = finalConfig.SwapLabel;

                if (bIsManualAction) {
                    VFXManager::Get().PlaySwapEffect(Character, L"/Game/Pal/Effect/Common/LevelUp/NS_LevelUp_Pal");
                }

                if (!IsCompanionSync) {
                    auto& palSet = ActivePalsByInstanceID[InstanceID];
                    for (UObject* Companion : palSet) {
                        if (Utils::IsObjectValid(Companion) && Companion != Character) {
                            ProcessPal(Companion, false, finalSwap, true);
                        }
                    }
                }

                return true; 
            } else {
                return false;
            }
        }

        return false; 
    }

    void PalProcessor::ApplySwap(UObject* Character, const SwapConfig& swap, PalPersistData& persist) {
        auto total_start = std::chrono::high_resolution_clock::now();
        auto step_start = total_start;
        auto ProfileStep = [&](const std::wstring& stepName) {
            auto now = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(now - step_start).count();
            DP_LOG(Default, "[Profile] [ApplySwap] {} took {:.3f} ms", stepName, duration / 1000.0f);
            step_start = now;
        };

        std::wstring BPName;
        if (!IsPalBlueprintValid(Character, BPName)) return;
        ProfileStep(L"Trace 1: Initial BP Validation");

        UObject* MeshComp = nullptr;
        Utils::CallFunction(Character, STR("GetMainMesh"), &MeshComp);
        if (!MeshComp || !Utils::IsObjectValid(MeshComp)) {
            return;
        }
        ProfileStep(L"Trace 2: GetMainMesh");

        Utils::SetPropertyValue<bool>(MeshComp, STR("bPauseAnims"), true, false);

        struct { bool bNewDisablePostProcessBlueprint; } EnablePP{ true };
        Utils::CallFunction(MeshComp, STR("SetDisablePostProcessBlueprint"), &EnablePP);

        UClass* CurrentAnimClass = nullptr;
        Utils::GetPropertyValue<UClass*>(MeshComp, STR("AnimClass"), CurrentAnimClass);

        UClass* TargetAnimClass = nullptr;
        UObject* TargetSkeleton = nullptr;
        UObject* TargetStaticParam = nullptr;
        ProfileStep(L"Trace 3: Pausing Anims & Fetching CurrentAnimClass");

        UObject* PalUtil = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
        struct { UObject* Char; FName RetVal; } CharIDParams{Character, FName()};
        if (PalUtil && Utils::IsObjectValid(PalUtil)) Utils::SafeProcessEvent(PalUtil, PalUtil->GetFunctionByNameInChain(STR("GetCharacterIDFromCharacter")), &CharIDParams);
        std::wstring CharID = StripCharacterPrefix(CharIDParams.RetVal.ToString());

        std::wstring ResolvedAnimPath = ResolveAnimPath(Character, swap.AnimTarget, CharID);
        bool bNeedsExternalAnimLoad = !ResolvedAnimPath.empty();
        ProfileStep(L"Trace 4: Resolving Anim Path (CharID & Utils)");

        if (bNeedsExternalAnimLoad) {
            UClass* TargetBPClass = static_cast<UClass*>(Utils::LoadAssetSafely(ResolvedAnimPath));
            
            if (!IsPalBlueprintValid(Character, BPName)) return;
            Utils::CallFunction(Character, STR("GetMainMesh"), &MeshComp);
            if (!MeshComp || !Utils::IsObjectValid(MeshComp)) return;

            if (TargetBPClass && Utils::IsObjectValid(TargetBPClass)) {
                UObject* TargetCDO = TargetBPClass->GetClassDefaultObject();
                
                if (TargetCDO && Utils::IsObjectValid(TargetCDO)) {
                    UObject* TargetMesh = nullptr;
                    Utils::GetPropertyValue<UObject*>(TargetCDO, STR("Mesh"), TargetMesh);
                    
                    if (TargetMesh && Utils::IsObjectValid(TargetMesh)) {
                        Utils::GetPropertyValue<UClass*>(TargetMesh, STR("AnimClass"), TargetAnimClass);
                        if (TargetAnimClass && Utils::IsObjectValid(TargetAnimClass)) {
                            Utils::GetPropertyValue<UObject*>(TargetAnimClass, STR("TargetSkeleton"), TargetSkeleton);
                        }

                        UObject* TargetSkelMesh = nullptr;
                        if (!Utils::GetPropertyValue<UObject*>(TargetMesh, STR("SkeletalMesh"), TargetSkelMesh)) {
                            Utils::GetPropertyValue<UObject*>(TargetMesh, STR("SkinnedAsset"), TargetSkelMesh);
                        }

                        if (TargetSkelMesh && Utils::IsObjectValid(TargetSkelMesh) && !TargetSkeleton) {
                            Utils::GetPropertyValue<UObject*>(TargetSkelMesh, STR("Skeleton"), TargetSkeleton);
                        }
                    }
                    Utils::GetPropertyValue<UObject*>(TargetCDO, STR("StaticCharacterParameterComponent"), TargetStaticParam);
                }
            }
        } else {
            UClass* CharClass = Character->GetClassPrivate();

            if (CharClass && Utils::IsObjectValid(CharClass)) {
                UObject* VanillaCDO = CharClass->GetClassDefaultObject();
                
                if (VanillaCDO && Utils::IsObjectValid(VanillaCDO)) {
                    UObject* VanillaMesh = nullptr;
                    Utils::GetPropertyValue<UObject*>(VanillaCDO, STR("Mesh"), VanillaMesh);
                    
                    if (VanillaMesh && Utils::IsObjectValid(VanillaMesh)) {
                        Utils::GetPropertyValue<UClass*>(VanillaMesh, STR("AnimClass"), TargetAnimClass);
                        if (TargetAnimClass && Utils::IsObjectValid(TargetAnimClass)) {
                            Utils::GetPropertyValue<UObject*>(TargetAnimClass, STR("TargetSkeleton"), TargetSkeleton);
                        }
                        
                        UObject* VanillaSkelMesh = nullptr;
                        if (!Utils::GetPropertyValue<UObject*>(VanillaMesh, STR("SkeletalMesh"), VanillaSkelMesh)) {
                            Utils::GetPropertyValue<UObject*>(VanillaMesh, STR("SkinnedAsset"), VanillaSkelMesh);
                        }
                        if (VanillaSkelMesh && Utils::IsObjectValid(VanillaSkelMesh) && !TargetSkeleton) {
                            Utils::GetPropertyValue<UObject*>(VanillaSkelMesh, STR("Skeleton"), TargetSkeleton);
                        }
                    }
                    Utils::GetPropertyValue<UObject*>(VanillaCDO, STR("StaticCharacterParameterComponent"), TargetStaticParam);
                }
            }

            if (!TargetAnimClass || !Utils::IsObjectValid(TargetAnimClass)) {
                TargetAnimClass = CurrentAnimClass;
                Utils::GetPropertyValue<UObject*>(Character, STR("StaticCharacterParameterComponent"), TargetStaticParam);
                
                UObject* CurrentSkelMesh = nullptr;
                if (!Utils::GetPropertyValue<UObject*>(MeshComp, STR("SkeletalMesh"), CurrentSkelMesh)) {
                    Utils::GetPropertyValue<UObject*>(MeshComp, STR("SkinnedAsset"), CurrentSkelMesh);
                }
                if (CurrentSkelMesh && Utils::IsObjectValid(CurrentSkelMesh)) {
                    Utils::GetPropertyValue<UObject*>(CurrentSkelMesh, STR("Skeleton"), TargetSkeleton);
                }
            }
        }

        if (!TargetAnimClass || !Utils::IsObjectValid(TargetAnimClass)) { TargetAnimClass = CurrentAnimClass; }

        bool bNeedsAnimRebuild = (TargetAnimClass != CurrentAnimClass);
        ProfileStep(L"Trace 5: Loading Targets & External BP classes");

        if (bNeedsAnimRebuild) {
            UFunction* SetAnimFunc = MeshComp->GetFunctionByNameInChain(STR("SetAnimInstanceClass"));
            if (!SetAnimFunc) SetAnimFunc = MeshComp->GetFunctionByNameInChain(STR("SetAnimClass"));

            if (SetAnimFunc) {
                struct { UClass* NewClass; } ClearParams{ nullptr };
                Utils::SafeProcessEvent(MeshComp, SetAnimFunc, &ClearParams);
            }
        }
        ProfileStep(L"Trace 6: Clear Current AnimClass");

        UObject* NewMesh = nullptr;
        if (!swap.SkelMeshPath.empty()) {
            NewMesh = Utils::LoadSkeletalMeshSafely(swap.SkelMeshPath);
            ProfileStep(L"Trace 7.1: Loading New SkelMesh Asset");
            
            if (!IsPalBlueprintValid(Character, BPName)) return;
            Utils::CallFunction(Character, STR("GetMainMesh"), &MeshComp);
            if (!MeshComp || !Utils::IsObjectValid(MeshComp)) return;
            ProfileStep(L"Trace 7.2: Re-validating BP after load");

            if (NewMesh && Utils::IsObjectValid(NewMesh)) {
                UClass* MeshClass = NewMesh->GetClassPrivate();
                
                if (MeshClass && Utils::IsObjectValid(MeshClass)) {
                    std::wstring meshClassName = MeshClass->GetName();
                    
                    if (meshClassName.find(L"SkeletalMesh") != std::wstring::npos || meshClassName.find(L"SkinnedAsset") != std::wstring::npos) {
                        
                        if (TargetSkeleton && Utils::IsObjectValid(TargetSkeleton)) {
                            Utils::SetPropertyValue<UObject*>(NewMesh, STR("Skeleton"), TargetSkeleton);
                        }

                        struct { UObject* InMesh; bool bReinitPose; } MeshParams{NewMesh, true};
                        Utils::CallFunction(MeshComp, STR("SetSkinnedAssetAndUpdate"), &MeshParams);
                        ProfileStep(L"Trace 7.3: SetSkinnedAssetAndUpdate Native Call");
                    } else {
                        DP_LOG(Warning, "[ApplySwap] Aborted: Loaded asset is a '{}', not a SkeletalMesh.", meshClassName);
                    }
                } else {
                    DP_LOG(Warning, "[ApplySwap] Aborted: Loaded mesh asset has a corrupt or missing Class definition.");
                }
            } else {
                DP_LOG(Warning, "Failed to load Skeletal Mesh for Pal '{}' from Pack '{}'!\nPath: {}", CharID, swap.PackName, swap.SkelMeshPath);
            }
        }
        ProfileStep(L"Trace 7: Complete Loading New Mesh Flow");

        if (bNeedsAnimRebuild) {
            UFunction* SetAnimFunc = MeshComp->GetFunctionByNameInChain(STR("SetAnimInstanceClass"));
            if (!SetAnimFunc) SetAnimFunc = MeshComp->GetFunctionByNameInChain(STR("SetAnimClass"));

            if (TargetAnimClass && SetAnimFunc) {
                struct { UClass* NewClass; } Params{ TargetAnimClass };
                Utils::SafeProcessEvent(MeshComp, SetAnimFunc, &Params);
            }

            if (TargetStaticParam && Utils::IsObjectValid(TargetStaticParam)) {
                UObject* CurrentStaticParam = nullptr;
                Utils::GetPropertyValue<UObject*>(Character, STR("StaticCharacterParameterComponent"), CurrentStaticParam);

                if (CurrentStaticParam && Utils::IsObjectValid(CurrentStaticParam)) {
                    auto CopyProp = [](UObject* Src, UObject* Dest, const wchar_t* PropName) {
                        FProperty* SrcProp = Utils::GetProperty(Src, PropName);
                        FProperty* DestProp = Utils::GetProperty(Dest, PropName);
                        if (SrcProp && DestProp) {
                            void* SrcPtr = SrcProp->ContainerPtrToValuePtr<void>(Src);
                            void* DestPtr = DestProp->ContainerPtrToValuePtr<void>(Dest);
                            if (SrcPtr && DestPtr) {
                                DestProp->CopyCompleteValue(DestPtr, SrcPtr);
                            }
                        }
                    };
                    CopyProp(TargetStaticParam, CurrentStaticParam, STR("RandomRestMontageInfos"));
                    CopyProp(TargetStaticParam, CurrentStaticParam, STR("GeneralAnimSequenceMap"));
                    CopyProp(TargetStaticParam, CurrentStaticParam, STR("GeneralMontageMap"));
                    CopyProp(TargetStaticParam, CurrentStaticParam, STR("GeneralBlendSpaceMap"));
                    CopyProp(TargetStaticParam, CurrentStaticParam, STR("ActionMontageMap"));
                    CopyProp(TargetStaticParam, CurrentStaticParam, STR("SleepOnSideAnimMontage"));
                    CopyProp(TargetStaticParam, CurrentStaticParam, STR("PettingSize"));
                    CopyProp(TargetStaticParam, CurrentStaticParam, STR("PettingStartAddDistance"));
                    CopyProp(TargetStaticParam, CurrentStaticParam, STR("PettingEndLeaveDistance"));
                    CopyProp(TargetStaticParam, CurrentStaticParam, STR("PettingDistance"));
                    CopyProp(TargetStaticParam, CurrentStaticParam, STR("HPGaugeUIOffset"));
                    CopyProp(TargetStaticParam, CurrentStaticParam, STR("SleepOnSideInfoMapForMapObject"));
                }
            }

            UObject* NewAnimInst = nullptr;
            Utils::CallFunction(MeshComp, STR("GetAnimInstance"), &NewAnimInst);
            if (NewAnimInst && Utils::IsObjectValid(NewAnimInst)) {
                UFunction* LinkFunc = NewAnimInst->GetFunctionByNameInChain(STR("LinkAnimClassLayers"));
                if (LinkFunc) {
                    // ---> EXPLICIT FIX: Only Base and Physics exist as classes on disk/RAM in Palworld
                    std::vector<std::wstring> StandardLayers = {
                        L"/Game/Pal/Blueprint/Character/Monster/ALI_MonsterBase.ALI_MonsterBase_C",
                        L"/Game/Pal/Blueprint/Character/Monster/ALI_MonsterPhysics.ALI_MonsterPhysics_C"
                    };

                    for (const auto& LayerPath : StandardLayers) {
                        // Use non-blocking mode (false) to prevent the 70ms disk search penalty
                        UClass* LayerClass = static_cast<UClass*>(Utils::LoadAssetInternal(LayerPath, false));
                        
                        if (!IsPalBlueprintValid(Character, BPName)) return;
                        Utils::CallFunction(Character, STR("GetMainMesh"), &MeshComp);
                        if (!MeshComp || !Utils::IsObjectValid(MeshComp)) return;

                        if (LayerClass && Utils::IsObjectValid(LayerClass)) {
                            struct { UClass* InClass; } LinkParams{ LayerClass };
                            Utils::SafeProcessEvent(NewAnimInst, LinkFunc, &LinkParams);
                        }
                    }
                }
            }
            ProfileStep(L"Trace 8: Syncing Static Params");
        }

        struct { int32_t RetVal; } NumMatParams{0};
        Utils::CallFunction(MeshComp, STR("GetNumMaterials"), &NumMatParams);
        for (int32_t i = 0; i < NumMatParams.RetVal; ++i) {
            struct { int32_t ElementIndex; UObject* Material; } ClearMatParams{i, nullptr};
            Utils::CallFunction(MeshComp, STR("SetMaterial"), &ClearMatParams);
        }

        for (auto& mat : swap.MatReplaceList) {
            std::wstring ChosenPath = mat.matPath;
            std::wstring WideIndex = Utils::StringToWString(mat.index);

            if (mat.matPath.length() >= 2 && mat.matPath.substr(mat.matPath.length() - 2) == L"/*") {
                std::wstring VirtualFolder = mat.matPath.substr(0, mat.matPath.length() - 2);

                std::vector<std::wstring> AvailableMats = Utils::GetAssetsInVirtualFolder(VirtualFolder);

                auto savedMatIt = persist.MatSet.find(mat.index);
                if (savedMatIt != persist.MatSet.end() && !savedMatIt->second.empty()) {
                    ChosenPath = savedMatIt->second;
                } 
                else {
                    if (!AvailableMats.empty()) {
                        static std::random_device rd;
                        static std::mt19937 gen(rd());
                        std::uniform_int_distribution<int> dis(0, (int)(AvailableMats.size() - 1));
                        
                        ChosenPath = AvailableMats[dis(gen)];
                        persist.MatSet[mat.index] = ChosenPath;
                    } else {
                        DP_LOG(Warning, "[Slot {}] Wildcard folder '{}' has ZERO matching material files! Skipping slot.", WideIndex, VirtualFolder);
                        continue;
                    }
                }
            } else {
                persist.MatSet[mat.index] = ChosenPath;
            }

            int idx = 0;
            try { idx = std::stoi(mat.index); } catch(...) { continue; }

            UObject* NewMat = nullptr;
            if (!ChosenPath.empty()) {
                NewMat = Utils::LoadAssetSafely(ChosenPath);
                if (!NewMat || !Utils::IsObjectValid(NewMat)) {
                    DP_LOG(Warning, "[Slot {}] LoadAssetSafely FAILED for path: '{}'", WideIndex, ChosenPath);
                    continue;
                }
            } else {
                struct { int32_t ElementIndex; UObject* ReturnValue; } GetMatParams{idx, nullptr};
                Utils::CallFunction(MeshComp, STR("GetMaterial"), &GetMatParams);
                NewMat = GetMatParams.ReturnValue;
                
                if (!NewMat || !Utils::IsObjectValid(NewMat)) {
                    continue;
                }
            }

            if (!IsPalBlueprintValid(Character, BPName)) {
                return;
            }

            UObject* CurrentMeshComp = nullptr;
            Utils::CallFunction(Character, STR("GetMainMesh"), &CurrentMeshComp);
            if (!CurrentMeshComp || !Utils::IsObjectValid(CurrentMeshComp)) {
                return;
            }

            if (mat.bRandomHue) {
                FLinearColor_UE5 appliedColor;
                auto colorIt = persist.MatColorSet.find(mat.index);
                
                if (colorIt != persist.MatColorSet.end()) {
                    appliedColor = colorIt->second; 
                } else {
                    static std::random_device rdColor;
                    static std::mt19937 genColor(rdColor());
                    std::uniform_real_distribution<float> disHue(0.0f, 360.0f);
                    
                    float H = disHue(genColor);
                    float S = 1.0f;
                    float V = 1.0f;
                    float C = S * V;
                    float X = C * (1.0f - std::abs(std::fmod(H / 60.0f, 2.0f) - 1.0f));
                    float m = V - C;
                    float r = 0, g = 0, b = 0;
                    
                    if (H >= 0 && H < 60) { r = C, g = X, b = 0; }
                    else if (H >= 60 && H < 120) { r = X, g = C, b = 0; }
                    else if (H >= 120 && H < 180) { r = 0, g = C, b = X; }
                    else if (H >= 180 && H < 240) { r = 0, g = X, b = C; }
                    else if (H >= 240 && H < 300) { r = X, g = 0, b = C; }
                    else { r = C, g = 0, b = X; }
                    
                    appliedColor = { r + m, g + m, b + m, 1.0f };
                    persist.MatColorSet[mat.index] = appliedColor;
                }

                UObject* KML = Utils::GetKML();
                UFunction* CreateFunc = Utils::GetKMLFunction(STR("CreateDynamicMaterialInstance"));

                static FProperty* WCProp = CreateFunc ? CreateFunc->GetPropertyByNameInChain(STR("WorldContextObject")) : nullptr;
                static FProperty* ParentProp = CreateFunc ? CreateFunc->GetPropertyByNameInChain(STR("Parent")) : nullptr;
                static FProperty* NameProp = CreateFunc ? CreateFunc->GetPropertyByNameInChain(STR("OptionalName")) : nullptr;
                static FProperty* RetProp = CreateFunc ? CreateFunc->GetPropertyByNameInChain(STR("ReturnValue")) : nullptr;

                UObject* MID = nullptr;

                if (KML && Utils::IsObjectValid(KML) && CreateFunc) {
                    alignas(8) uint8_t MIDParams[128] = {0};

                    if (WCProp) *WCProp->ContainerPtrToValuePtr<UObject*>(MIDParams) = Character;
                    if (ParentProp) *ParentProp->ContainerPtrToValuePtr<UObject*>(MIDParams) = NewMat;
                    if (NameProp) *NameProp->ContainerPtrToValuePtr<FName>(MIDParams) = FName();

                    Utils::SafeProcessEvent(KML, CreateFunc, MIDParams);

                    if (RetProp) MID = *RetProp->ContainerPtrToValuePtr<UObject*>(MIDParams);
                }

                if (MID && Utils::IsObjectValid(MID)) {
                    static UFunction* SetVecFunc = MID->GetFunctionByNameInChain(STR("SetVectorParameterValue"));
                    static FProperty* NamePropVec = SetVecFunc ? SetVecFunc->GetPropertyByNameInChain(STR("ParameterName")) : nullptr;
                    static FProperty* ValPropVec = SetVecFunc ? SetVecFunc->GetPropertyByNameInChain(STR("Value")) : nullptr;

                    if (SetVecFunc) {
                        alignas(8) uint8_t VecParams[128] = {0};
                        if (NamePropVec) *NamePropVec->ContainerPtrToValuePtr<FName>(VecParams) = FName(STR("Hue"), FNAME_Add);
                        if (ValPropVec) *ValPropVec->ContainerPtrToValuePtr<FLinearColor_UE5>(VecParams) = appliedColor;
                        
                        Utils::SafeProcessEvent(MID, SetVecFunc, VecParams);
                    }
                    
                    struct { int32_t ElementIndex; UObject* Material; } MatParams{idx, MID};
                    Utils::CallFunction(CurrentMeshComp, STR("SetMaterial"), &MatParams);
                    continue;
                }
            }

            struct { int32_t ElementIndex; UObject* Material; } MatParams{idx, NewMat};
            Utils::CallFunction(CurrentMeshComp, STR("SetMaterial"), &MatParams);
        }
        ProfileStep(L"Trace 9: Applying Materials");

        if (!swap.MorphTargetList.empty()) {
            static std::random_device rd;
            static std::mt19937 gen(rd());

            for (auto& morph : swap.MorphTargetList) {
                double val = 0.0;
                auto iVal = persist.MorphSet.find(morph.target);
                bool hasValidSavedVal = false;
                double savedVal = -1000.0;
                if (iVal != persist.MorphSet.end()) {
                    savedVal = iVal->second;
                    if (savedVal >= -900.0) hasValidSavedVal = true;
                }

                if (morph.setVal != -1000.0) {
                    val = morph.setVal;
                } else if (hasValidSavedVal) {
                    if (morph.type == L"Restrict") {
                        double midpoint = ((morph.maxVal - morph.minVal) / 2.0) + morph.minVal;
                        val = (savedVal >= midpoint) ? morph.maxVal : morph.minVal;
                    } else {
                        if (savedVal >= morph.minVal && savedVal <= morph.maxVal) {
                            val = savedVal;
                        } else {
                            std::uniform_real_distribution<> dis(morph.minVal, morph.maxVal);
                            val = dis(gen);
                        }
                    }
                } else {
                    if (morph.type == L"Restrict") {
                        std::uniform_int_distribution<> dis(0, 1);
                        val = dis(gen) ? morph.maxVal : morph.minVal;
                    } else {
                        std::uniform_real_distribution<> dis(morph.minVal, morph.maxVal);
                        val = dis(gen);
                    }
                }

                persist.MorphSet[morph.target] = val;

                struct { FName MorphTargetName; float Value; bool bRemoveZeroWeight; } MorphParams{
                    FName(morph.target.c_str(), FNAME_Add), (float)val, false
                };
                Utils::CallFunction(MeshComp, STR("SetMorphTarget"), &MorphParams);
            }
        }
        ProfileStep(L"Trace 10: Applying Morph Targets");

        struct { bool bNewDisablePostProcessBlueprint; } DisablePP_False{ false };
        Utils::CallFunction(MeshComp, STR("SetDisablePostProcessBlueprint"), &DisablePP_False);

        Utils::SetPropertyValue<bool>(MeshComp, STR("bPauseAnims"), false, false);

        UObject* FacialComp = nullptr;
        Utils::GetPropertyValue<UObject*>(Character, STR("PalFacial"), FacialComp);
        
        if (!FacialComp || !Utils::IsObjectValid(FacialComp)) {
            UClass* FacialClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Pal.PalFacialComponent"));
            if (FacialClass && Utils::IsObjectValid(FacialClass)) {
                struct { UClass* ComponentClass; UObject* ReturnValue; } GetCompParams{FacialClass, nullptr};
                Utils::CallFunction(Character, STR("GetComponentByClass"), &GetCompParams);
                FacialComp = GetCompParams.ReturnValue;
            }
        }

        if (FacialComp && Utils::IsObjectValid(FacialComp)) {
            UObject* MainModule = nullptr;
            if (Utils::GetPropertyValue<UObject*>(FacialComp, STR("MainModule"), MainModule) && MainModule && Utils::IsObjectValid(MainModule)) {
                struct { UObject* SkeletalMeshComponent; } SetupParams{ MeshComp };
                UFunction* SetupFunc = MainModule->GetFunctionByNameInChain(STR("Setup_FacialModule"));
                if (SetupFunc) {
                    Utils::SafeProcessEvent(MainModule, SetupFunc, &SetupParams);

                    int32_t EyeIdx = -2;
                    int32_t MouthIdx = -2;
                    int32_t BrowIdx = -2;
                    
                    Utils::GetPropertyValue<int32_t>(MainModule, STR("EyeMaterialIndex"), EyeIdx);
                    Utils::GetPropertyValue<int32_t>(MainModule, STR("MouthMaterialIndex"), MouthIdx);
                    Utils::GetPropertyValue<int32_t>(MainModule, STR("BrowMaterialIndex"), BrowIdx);
                } else {
                    DP_LOG(Warning, "[Facial] MainModule on Pal '{}' is missing 'Setup_FacialModule' function. Face textures may stretch.", Character->GetName());
                } 
            } else {
                DP_LOG(Warning, "[Facial] Failed to retrieve 'MainModule' from FacialComponent on Pal '{}'.", Character->GetName());
            }
        } else {
            DP_LOG(Verbose, "[Facial] FacialComponent not found on Pal '{}'.", Character->GetName());
        }
        ProfileStep(L"Trace 11: PalFacialComponent Setup");

        DP_LOG(Default, "Successfully applied swap '{}' from Pack '{}' to Pal '{}'!\n", swap.SkinName.empty() ? L"Mesh Swap" : swap.SkinName, swap.PackName, CharID);

        auto total_duration = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - total_start).count();
        DP_LOG(Default, "[Profile] [ApplySwap] Trace 12: Done! Total ApplySwap execution took {:.3f} ms", total_duration / 1000.0f);
    }

}
// --- END OF FILE src/PalProcessor.cpp ---