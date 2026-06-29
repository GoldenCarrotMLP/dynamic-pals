#include "PalProcessor.hpp"
#include "ConfigManager.hpp"
#include "SaveManager.hpp"
#include "Utils.hpp"
#include "AsyncHelper.hpp"
#include "VFXManager.hpp"
#include <random>
#include <thread>

#include <Unreal/UObject.hpp>
#include <Unreal/FString.hpp>
#include <Unreal/Core/Containers/Array.hpp>

///New code. Improved getting correct actor blueprint resolution by just using DT_PalBPClass_Common 

using namespace RC;
using namespace RC::Unreal;

namespace DynPals {

     // Fetches Level, Rank, and Friendship/Trust from a Pal with sentinel verification and fallback checks
    PalRuntimeStats RetrievePalStats(UObject* IndivParam, const std::wstring& RawCharID, const std::wstring& InstanceID, bool bLogWarnings) {
            PalRuntimeStats stats;
            if (!IndivParam) return stats;

            struct { int32_t RetVal = -1; } LevelParams;
            Utils::CallFunction(IndivParam, STR("GetLevel"), &LevelParams);
            stats.Level = LevelParams.RetVal;
            if (stats.Level == -1) {
                if (bLogWarnings) {
                    DP_LOG(Warning, "[Stats] Failed to execute 'GetLevel' for Pal '{}' (ID: '{}'). Falling back to level 1.", RawCharID, InstanceID);
                }
                stats.Level = 1;
            }

            struct { int32_t RetVal = -1; } RankParams;
            Utils::CallFunction(IndivParam, STR("GetRank"), &RankParams);
            stats.Rank = RankParams.RetVal;
            if (stats.Rank == -1) {
                if (bLogWarnings) {
                    DP_LOG(Warning, "[Stats] Failed to execute 'GetRank' for Pal '{}' (ID: '{}'). Falling back to rank 0.", RawCharID, InstanceID);
                }
                stats.Rank = 0;
            }

            struct { int32_t RetVal = -1; } FriendshipParams;
            Utils::CallFunction(IndivParam, STR("GetFriendshipRank"), &FriendshipParams);
            stats.Friendship = FriendshipParams.RetVal;
            if (stats.Friendship == -1) {
                // Safeguard legacy fallback check
                struct { int32_t RetVal = -1; } LegacyFriendshipParams;
                Utils::CallFunction(IndivParam, STR("GetFriendshipPoint"), &LegacyFriendshipParams);
                if (LegacyFriendshipParams.RetVal != -1) {
                    stats.Friendship = LegacyFriendshipParams.RetVal;
                } else {
                    if (bLogWarnings) {
                        DP_LOG(Warning, "[Stats] Failed to execute both 'GetFriendshipRank' and 'GetFriendshipPoint' for Pal '{}' (ID: '{}'). Falling back to trust 0.", RawCharID, InstanceID);
                    }
                    stats.Friendship = 0;
                }
            }

            return stats;
        }
    


    void PalProcessor::ClearAllSwappedStatus() {
        SwappedInstances.clear();
        RuntimeStatsCache.clear();
        ProcessedPals.clear();
        ProcessingQueue.clear();
    }

    void PalProcessor::ClearSwappedStatus(const std::wstring& InstanceID) {
        SwappedInstances.erase(InstanceID);
        RuntimeStatsCache.erase(InstanceID);
    }
    // Latch into Palworld's native database subsystem to resolve any CharacterID to its Blueprint Class path
    // Safely queries the game's native DataTable DT_PalBPClass_Common using standard reflection
    static bool ResolvePalBlueprintPath(UObject* WorldContext, const std::wstring& CharID, std::wstring& OutPath) {
        if (!WorldContext) return false;

        UObject* PalUtil = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
        if (!PalUtil) return false;

        // 1. Fetch the active UPalDatabaseCharacterParameter subsystem instance
        struct { UObject* WorldCtx; UObject* DB; } GetDBParams{ WorldContext, nullptr };
        UFunction* GetDBFunc = PalUtil->GetFunctionByNameInChain(STR("GetDatabaseCharacterParameter"));
        if (!GetDBFunc) return false;
        
        PalUtil->ProcessEvent(GetDBFunc, &GetDBParams);
        UObject* DB = GetDBParams.DB;
        if (!DB) return false;

        // 2. Fetch the native GetBPClass function
        UFunction* GetBPClassFunc = DB->GetFunctionByNameInChain(STR("GetBPClass"));
        if (!GetBPClassFunc) return false;

        // 3. Package the parameter block (matches native UE5 alignment & size requirements)
        struct {
            FName RowName;                  // 0x00 (8 bytes)
            bool bShowError;                // 0x08 (1 byte)
            uint8_t Pad[7];                 // 0x09 - 0x0F (Align AltrSoftObjectPtr to 8-byte boundary)
            AltrSoftObjectPtr ReturnValue;  // 0x10 (48 bytes SoftClassPtr / SoftObjectPtr)
        } Params;

        Params.RowName = FName(CharID.c_str(), FNAME_Add);
        Params.bShowError = false;

        // 4. Fire the native function call! (Now 100% stack-safe)
        DB->ProcessEvent(GetBPClassFunc, &Params);

        // 5. Convert the returned SoftClassPtr (AltrSoftObjectPtr) to a std::wstring path
        std::wstring packageName = Params.ReturnValue.ObjectID.PackageName.ToString();
        std::wstring assetName = Params.ReturnValue.ObjectID.AssetName.ToString();

        if (!packageName.empty() && !assetName.empty()) {
            OutPath = packageName + L"." + assetName;
            return true;
        }

        return false;
    }

    static bool IsPalBlueprintValid(UObject* Pal, std::wstring& OutBlueprintName) {
        if (!Pal) return false;

        UClass* PalClass = Pal->GetClassPrivate();
        if (!PalClass) {
            DP_LOG(Warning, "WARNING: Spawned Actor has NO VALID BLUEPRINT CLASS! Aborting.");
            return false;
        }
        
        OutBlueprintName = PalClass->GetName();
        if (OutBlueprintName.empty() || OutBlueprintName.find(L"Default__") != std::wstring::npos) return false;

        if (OutBlueprintName.find(L"_Gym") != std::wstring::npos) {
            return false;
        }

        bool bBeingDestroyed = false;
        if (Utils::GetPropertyValue<bool>(Pal, STR("bActorIsBeingDestroyed"), bBeingDestroyed) && bBeingDestroyed) {
            return false;
        }

        bool bBegunPlay = false;
        if (Utils::GetPropertyValue<bool>(Pal, STR("bHasBegunPlay"), bBegunPlay)) {
            if (!bBegunPlay) return false;
        }

        bool bIsActive = true;
        if (Utils::GetPropertyValue<bool>(Pal, STR("bIsPalActiveActor"), bIsActive)) {
            //if (!bIsActive) return false;
        }

        UObject* MeshComp = nullptr;
        Utils::CallFunction(Pal, STR("GetMainMesh"), &MeshComp);
        if (!MeshComp) {
            return false;
        }

        bool bRegistered = false;
        if (Utils::GetPropertyValue<bool>(MeshComp, STR("bRegistered"), bRegistered)) {
            if (!bRegistered) return false;
        }

        struct { bool ReturnValue; } ColParams{true};
        Utils::CallFunction(Pal, STR("GetActorEnableCollision"), &ColParams);
        //if (!ColParams.ReturnValue) return false;

        bool bHidden = false;
        if (Utils::GetPropertyValue<bool>(Pal, STR("bHidden"), bHidden)) {
            //if (bHidden) return false;
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
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - LastScanTime).count() < 1000) return;
    LastScanTime = now;

    std::vector<UObject*> AllPals;
    UObjectGlobals::FindAllOf(STR("PalCharacter"), AllPals);

    for (UObject* Pal : AllPals) {
        if (Pal) {
            if (ProcessedPals.find(Pal) == ProcessedPals.end()) {
                ProcessedPals.insert(Pal);
                ProcessPal(Pal, false);
            } else {
                // FAST POLL: Lightweight check for level/rank/friendship changes in the background!
                // This creates a safety-net just in case a native stat update bypassed the UE4SS hooks.
                UObject* ParamComp = nullptr;
                Utils::GetPropertyValue<UObject*>(Pal, STR("CharacterParameterComponent"), ParamComp);
                if (ParamComp) {
                    UObject* IndivParam = nullptr;
                    Utils::GetPropertyValue<UObject*>(ParamComp, STR("IndividualParameter"), IndivParam);
                    if (IndivParam) {
                        struct FPalInstanceID { DynPalsGuid PlayerUId; DynPalsGuid InstanceId; } IDStruct;
                        if (Utils::GetPropertyValue<FPalInstanceID>(IndivParam, STR("IndividualId"), IDStruct) && IDStruct.InstanceId.IsValid()) {
                                std::wstring InstanceID = Utils::GuidToWString(IDStruct.InstanceId);
                                auto it = RuntimeStatsCache.find(InstanceID);
                                
                                if (it != RuntimeStatsCache.end()) {
                                    // Silently poll the current engine parameters
                                    PalRuntimeStats stats = RetrievePalStats(IndivParam, L"", InstanceID, false);

                                    // If any cached parameter differs, trigger the update
                                    if (it->second.Level != stats.Level || 
                                        it->second.Rank != stats.Rank || 
                                        it->second.Friendship != stats.Friendship) 
                                    {
                                        ProcessPal(Pal, false);
                                    }
                                }
                            }
                    }
                }
            }
        }
    }
}

    void PalProcessor::DelayedSwap(UObject* Character, int SwapIndex, const std::wstring& CompName) {
        if (!Character) return;
        
        // 1. Play the visual composition instantly and get the JSON-defined swap delay
        float DelaySeconds = VFXManager::Get().PlayComposition(Character, CompName);
        
        // 2. Schedule the physical swap for later
        int DelayMs = static_cast<int>(DelaySeconds * 1000.0f);
        ForceSwap(Character, SwapIndex, DelayMs);
    }

    void PalProcessor::DelayedReroll(UObject* Character, const std::wstring& CompName) {
        if (!Character) return;
        
        // 1. Play the visual composition instantly
        float DelaySeconds = VFXManager::Get().PlayComposition(Character, CompName);

        // 2. Schedule a randomized evaluation for later
        int DelayMs = static_cast<int>(DelaySeconds * 1000.0f);
        std::thread([Character, DelayMs]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(DelayMs));
            AsyncHelper::AsyncTask(ENamedThreads::GameThread, [Character]() {
                PalProcessor::Get().ProcessPal(Character, true);
            });
        }).detach();
    }

    void PalProcessor::ForceSwap(UObject* Character, int SwapIndex, int DelayMs) {
        if (!Character || SwapIndex < 0 || SwapIndex >= (int)ConfigManager::Get().GetConfigs().size()) return;

        UObject* ParamComp = nullptr;
        Utils::GetPropertyValue<UObject*>(Character, STR("CharacterParameterComponent"), ParamComp);
        if (!ParamComp) return;

        UObject* IndivParam = nullptr;
        Utils::GetPropertyValue<UObject*>(ParamComp, STR("IndividualParameter"), IndivParam);
        if (!IndivParam) return;

        struct FPalInstanceID { DynPalsGuid PlayerUId; DynPalsGuid InstanceId; } InstanceIDStruct;
        if (!Utils::GetPropertyValue<FPalInstanceID>(IndivParam, STR("IndividualId"), InstanceIDStruct)) return;
        
        std::wstring InstanceID = Utils::GuidToWString(InstanceIDStruct.InstanceId);

        ClearSwappedStatus(InstanceID);

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
            
            // FIX: Wipe the persistent materials and morphs so the new skin rolls fresh ones!
            ExistingData->MorphSet.clear();
            ExistingData->MatSet.clear();
            
            SaveManager::Get().SetPersistData(InstanceID, *ExistingData, true); 
        }

        // FIX: Launch a fire-and-forget background timer, then safely dispatch the swap to the Game Thread! [1]
        std::thread([Character, SwapIndex, DelayMs]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(DelayMs));
            
            AsyncHelper::AsyncTask(ENamedThreads::GameThread, [Character, SwapIndex]() {
                PalProcessor::Get().ExecuteSwap(Character, false, SwapIndex);
            });
        }).detach();
    }
    
    int PalProcessor::EvaluateIdealSwapIndex(UObject* Character, std::wstring& OutInstanceID) {
        return -1; 
    }

    void PalProcessor::ProcessPal(UObject* Character, bool ForceReroll) {
        ExecuteSwap(Character, ForceReroll, -1);
    }

    void PalProcessor::CheckAndTriggerUpdate(UObject* Character) {
        ExecuteSwap(Character, false, -1);
    }

    void PalProcessor::ExecuteSwap(UObject* Character, bool ForceReroll, int ExplicitSwapIndex) {
        if (!Character) return;
        
        std::wstring BlueprintName = L"";
        if (!IsPalBlueprintValid(Character, BlueprintName)) {
            return;
        }

        UObject* ParamComp = nullptr;
        Utils::GetPropertyValue<UObject*>(Character, STR("CharacterParameterComponent"), ParamComp);
        if (!ParamComp) return;

        UObject* IndivParam = nullptr;
        Utils::GetPropertyValue<UObject*>(ParamComp, STR("IndividualParameter"), IndivParam);
        if (!IndivParam) return;

        struct FPalInstanceID { DynPalsGuid PlayerUId; DynPalsGuid InstanceId; } InstanceIDStruct;
        if (!Utils::GetPropertyValue<FPalInstanceID>(IndivParam, STR("IndividualId"), InstanceIDStruct)) return;
        if (!InstanceIDStruct.InstanceId.IsValid()) return; 

        std::wstring InstanceID = Utils::GuidToWString(InstanceIDStruct.InstanceId);

        UObject* Level = Character->GetOuterPrivate();
        if (!Level) return;
        UObject* World = Level->GetOuterPrivate();
        if (!World || World->GetClassPrivate()->GetName() != L"World") return;

        static UObject* PalUtil = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
        struct { UObject* Char; FName RetVal; } CharIDParams{Character, FName()};
        if (PalUtil) Utils::CallFunction(PalUtil, STR("GetCharacterIDFromCharacter"), &CharIDParams);
        std::wstring RawCharID = CharIDParams.RetVal.ToString();

        if (RawCharID.rfind(L"GYM_", 0) == 0 || RawCharID.find(L"_Gym_") != std::wstring::npos) return;

        // --- DEBUG 1: FUNCTION ENTRANCE ---
        //DP_LOG(Default, "[Debug Swap] ExecuteSwap called for Pal '{}' (ID: {}).", RawCharID, InstanceID);

        SaveManager::Get().LoadWorldData(World);
        PalPersistData* ExistingData = SaveManager::Get().GetPersistData(InstanceID);

        std::wstring CharID = StripCharacterPrefix(RawCharID);

        PalRuntimeStats stats = RetrievePalStats(IndivParam, RawCharID, InstanceID, true);
        int LevelNum = stats.Level;
        int RankNum = stats.Rank;
        int FriendshipNum = stats.Friendship;

        struct { UObject* Actor; bool RetVal; } WildParams{Character, false};
        if (PalUtil) Utils::CallFunction(PalUtil, STR("IsWildNPC"), &WildParams);
        bool IsWild = WildParams.RetVal;

        struct { bool ReturnValue; } RareParams{false};
        Utils::CallFunction(IndivParam, STR("IsRarePal"), &RareParams);
        bool IsRare = RareParams.ReturnValue;

        struct { uint8_t RetVal; } GenderParams{0};
        Utils::CallFunction(IndivParam, STR("GetGenderType"), &GenderParams);
        std::wstring GenderStr = (GenderParams.RetVal == 1) ? L"Male" : ((GenderParams.RetVal == 2) ? L"Female" : L"None");

        struct { FName RetVal; } SkinParams{FName()};
        Utils::CallFunction(IndivParam, STR("GetSkinName"), &SkinParams);
        std::wstring SkinName = SkinParams.RetVal.ToString();
        if (SkinName == L"None") SkinName = L"";

        std::vector<std::wstring> Traits;
        struct { TArray<FName> RetVal; } TraitsParams;
        Utils::CallFunction(IndivParam, STR("GetPassiveSkillList"), &TraitsParams);
        for (int32_t i = 0; i < TraitsParams.RetVal.Num(); ++i) {
            Traits.push_back(TraitsParams.RetVal[i].ToString());
        }

        PalRuntimeStats& CachedStats = RuntimeStatsCache[InstanceID];
        
        bool bLiveEventTriggered = (CachedStats.Level != -1); 
        bool bBucketChanged = false;

        if (bLiveEventTriggered) {
            auto oldEvaluations = ConfigManager::Get().EvaluateAllSwaps(CharID, IsRare, GenderStr, Traits, CachedStats.Level, SkinName, CachedStats.Rank, CachedStats.Friendship, IsWild);
            auto newEvaluations = ConfigManager::Get().EvaluateAllSwaps(CharID, IsRare, GenderStr, Traits, LevelNum, SkinName, RankNum, FriendshipNum, IsWild);

            std::vector<int> oldValidSkins;
            std::vector<int> newValidSkins;

            for (const auto& ev : oldEvaluations) { if (ev.IsValid) oldValidSkins.push_back(ev.ConfigIndex); }
            for (const auto& ev : newEvaluations) { if (ev.IsValid) newValidSkins.push_back(ev.ConfigIndex); }

            if (oldValidSkins != newValidSkins) {
                bBucketChanged = true;
            }
        }

        CachedStats.Level = LevelNum;
        CachedStats.Rank = RankNum;
        CachedStats.Friendship = FriendshipNum;

        int currentSwap = -1;
        if (ExistingData && ExistingData->HasSavedSwap()) {
            // Pass CharID to allow different characters to share the same mesh without collision [1]
            currentSwap = ConfigManager::Get().FindConfigIndex(ExistingData->PackName, ExistingData->SkinName, ExistingData->SwapLabel, ExistingData->SkelMeshPath, CharID);
        }

        int finalSwap = -1;

        if (ExplicitSwapIndex != -1) {
            finalSwap = ExplicitSwapIndex;
        } 
        else {
            // --- DEBUG 2: DUPLICATE GUARD BLOCK ---
            auto it = SwappedInstances.find(InstanceID);
            if (!ForceReroll && !bBucketChanged && it != SwappedInstances.end() && it->second == Character) {
                //DP_LOG(Default, "[Debug Swap] Blocked Duplicate: Pal '{}' (ID: '{}') already processed on Actor {}. Skipping.", RawCharID, InstanceID, (void*)Character);
                return; 
            }

            auto evaluations = ConfigManager::Get().EvaluateAllSwaps(CharID, IsRare, GenderStr, Traits, LevelNum, SkinName, RankNum, FriendshipNum, IsWild);
            int newBestSwap = ConfigManager::Get().PickBestSwap(evaluations);
            
            finalSwap = currentSwap;

            if (ForceReroll) {
                finalSwap = newBestSwap;
            } else if (currentSwap != -1) {
                if (bLiveEventTriggered) {
                    const SwapEvaluation* currentEval = nullptr;
                    for (const auto& ev : evaluations) {
                        if (ev.ConfigIndex == currentSwap) {
                            currentEval = &ev;
                            break;
                        }
                    }

                    if (currentEval) {
                        if (bBucketChanged) {
                            int absoluteBestScore = 999999;
                            for (const auto& ev : evaluations) {
                                if (ev.IsValid && ev.Score < absoluteBestScore) {
                                    absoluteBestScore = ev.Score;
                                }
                            }

                            if (!currentEval->IsValid || currentEval->Score > absoluteBestScore) {
                                DP_LOG(Normal, "Live Event: Skin bucket changed or became invalid. Upgrading skin.\n");
                                finalSwap = newBestSwap;
                            } else {
                                finalSwap = currentSwap;
                            }
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
        }

        if (finalSwap != -1) {
            auto activeIt = SwappedInstances.find(InstanceID);
            bool bIsNewActor = (activeIt == SwappedInstances.end()) || (activeIt->second != Character);

            bool bNeedsApply = (ExplicitSwapIndex != -1) || ForceReroll || (finalSwap != currentSwap) || bIsNewActor;
            
            if (bNeedsApply) {
                // --- DEBUG 3: PROCEEDING WITH SWAP ---
                DP_LOG(Default, "[Debug Swap] Proceeding to Swap Pal '{}' (ID: '{}', Actor: {}). Reason: {}", 
                    RawCharID, InstanceID, (void*)Character,
                    (ExplicitSwapIndex != -1) ? L"Explicit Selection" : 
                    (ForceReroll) ? L"Force Reroll" : 
                    (finalSwap != currentSwap) ? L"Skin Changed" : L"New Actor Spawned");

                // ==========================================
                // LIVE EVOLUTION INTERCEPT
                // If this swap is triggered by an organic, in-game stat change (not a UI click or a fresh spawn), 
                // we abort the instant swap and route it through the Composer for a visual evolution!
                // ==========================================
                bool bIsLiveEvolution = bLiveEventTriggered && !bIsNewActor && (finalSwap != currentSwap) && (ExplicitSwapIndex == -1) && !ForceReroll;

                if (bIsLiveEvolution) {
                    DP_LOG(Default, "Live Evolution Triggered! Deferring physical swap for visual composition...");
                    
                    // Plays evolve_1 and schedules the physical swap to happen after the JSON delay passes!
                    DelayedSwap(Character, finalSwap, L"evolve_1");
                    
                    return; // Abort immediate swap execution!
                }

                PalPersistData newData = ExistingData ? *ExistingData : PalPersistData{ InstanceID, L"", L"", L"", {} };
                
                auto& finalConfig = ConfigManager::Get().GetConfigs()[finalSwap];
                newData.PackName = finalConfig.PackName;
                newData.SkinName = finalConfig.SkinName;
                newData.SwapLabel = finalConfig.SwapLabel;
                newData.SkelMeshPath = finalConfig.SkelMeshPath;

                if (ForceReroll || ExplicitSwapIndex != -1 || finalSwap != currentSwap) {
                    newData.MorphSet.clear();
                    newData.MatSet.clear();
                }

                ApplySwap(Character, finalConfig, newData);

                bool bIsManualAction = (ExplicitSwapIndex != -1) || ForceReroll;
                SaveManager::Get().SetPersistData(InstanceID, newData, bIsManualAction);

                SwappedInstances[InstanceID] = Character;

                // Play the modular fast swap effect on forced/manual UI swaps
                if (bIsManualAction) {
                    VFXManager::Get().PlaySwapEffect(Character, L"/Game/Pal/Effect/Common/LevelUp/NS_LevelUp_Pal");

                }
            }

        }
    }
    
    void PalProcessor::ApplySwap(UObject* Character, const SwapConfig& swap, PalPersistData& persist) {
        std::wstring BPName;
        if (!IsPalBlueprintValid(Character, BPName)) return;

        UObject* MeshComp = nullptr;
        Utils::CallFunction(Character, STR("GetMainMesh"), &MeshComp);
        if (!MeshComp) return;

        struct { bool bPause; } PauseAnim{ true };
        Utils::CallFunction(MeshComp, STR("SetPauseAnims"), &PauseAnim);

        struct { bool bNewDisablePostProcessBlueprint; } EnablePP{ true };
        Utils::CallFunction(MeshComp, STR("SetDisablePostProcessBlueprint"), &EnablePP);

        UClass* CurrentAnimClass = nullptr;
        Utils::GetPropertyValue<UClass*>(MeshComp, STR("AnimClass"), CurrentAnimClass);

        UClass* TargetAnimClass = nullptr;
        UObject* TargetSkeleton = nullptr;
        UObject* TargetStaticParam = nullptr;

        UObject* PalUtil = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
        struct { UObject* Char; FName RetVal; } CharIDParams{Character, FName()};
        if (PalUtil) Utils::CallFunction(PalUtil, STR("GetCharacterIDFromCharacter"), &CharIDParams);
        std::wstring CharID = StripCharacterPrefix(CharIDParams.RetVal.ToString());

        std::wstring AnimPath = swap.AnimTarget;
        if (AnimPath.empty()) {
            AnimPath = CharID;
        }

        UClass* TargetBPClass = nullptr;
        std::wstring TargetPackagePath = L"";
        std::wstring TargetClassName = L"";

         if (AnimPath.find(L'/') == std::wstring::npos) {
            // 1. Primary Attempt: Resolve natively via the game's native Database Subsystem!
            std::wstring ResolvedPath;
            if (ResolvePalBlueprintPath(Character, AnimPath, ResolvedPath)) {
                TargetBPClass = static_cast<UClass*>(Utils::LoadAssetSafely(ResolvedPath));
                if (TargetBPClass) {
                    //DP_LOG(Default, "[DEBUG] Successfully resolved Pal '{}' natively to path: '{}'\n", AnimPath, ResolvedPath);
                    size_t dotPos = ResolvedPath.find(L'.');
                    if (dotPos != std::wstring::npos) {
                        TargetPackagePath = ResolvedPath.substr(0, dotPos);
                        TargetClassName = ResolvedPath.substr(dotPos + 1);
                    }
                }
            }

            // 2. Secondary Fallback: Guess paths if the ID isn't registered in the native Database
            if (!TargetBPClass) {
                DP_LOG(Normal, "Pal '{}' not found in native Database. Falling back to path-guessing...\n", AnimPath);
                
                std::wstring TryPath1 = L"/Game/Pal/Blueprint/Character/Monster/PalActorBP/" + AnimPath + L"/BP_" + AnimPath + L".BP_" + AnimPath + L"_C";
                TargetBPClass = static_cast<UClass*>(Utils::LoadAssetSafely(TryPath1));

                if (TargetBPClass) {
                    TargetPackagePath = L"/Game/Pal/Blueprint/Character/Monster/PalActorBP/" + AnimPath + L"/BP_" + AnimPath;
                    TargetClassName = L"BP_" + AnimPath + L"_C";
                } else {
                    std::wstring FolderName = AnimPath;
                    size_t uscorePos = FolderName.find(L'_');
                    if (uscorePos != std::wstring::npos) {
                        FolderName = FolderName.substr(0, uscorePos);
                        std::wstring TryPath2 = L"/Game/Pal/Blueprint/Character/Monster/PalActorBP/" + FolderName + L"/BP_" + AnimPath + L".BP_" + AnimPath + L"_C";
                        TargetBPClass = static_cast<UClass*>(Utils::LoadAssetSafely(TryPath2));
                        
                        if (TargetBPClass) {
                            TargetPackagePath = L"/Game/Pal/Blueprint/Character/Monster/PalActorBP/" + FolderName + L"/BP_" + AnimPath;
                            TargetClassName = L"BP_" + AnimPath + L"_C";
                        }
                    }
                }
            }
        } else {
            TargetBPClass = static_cast<UClass*>(Utils::LoadAssetSafely(AnimPath));
            if (!TargetBPClass) {
                DP_LOG(Warning, "Failed to load Animation Target Blueprint for Pal '{}' from Pack '{}'!\nPath: {}", CharID, swap.PackName, AnimPath);
            }
            size_t dotPos = AnimPath.find(L'.');
            if (dotPos != std::wstring::npos) {
                TargetPackagePath = AnimPath.substr(0, dotPos);
                TargetClassName = AnimPath.substr(dotPos + 1);


            }
        }

        if (!IsPalBlueprintValid(Character, BPName)) return;
        Utils::CallFunction(Character, STR("GetMainMesh"), &MeshComp);
        if (!MeshComp) return;

        if (TargetBPClass && !TargetPackagePath.empty() && !TargetClassName.empty()) {
            std::wstring CDOPath = TargetPackagePath + L".Default__" + TargetClassName;
            UObject* TargetCDO = Utils::LoadAssetSafely(CDOPath);
            
            if (TargetCDO) {
                UObject* TargetMesh = nullptr;
                Utils::GetPropertyValue<UObject*>(TargetCDO, STR("Mesh"), TargetMesh);
                
                if (TargetMesh) {
                    Utils::GetPropertyValue<UClass*>(TargetMesh, STR("AnimClass"), TargetAnimClass);
                    if (TargetAnimClass) {
                        Utils::GetPropertyValue<UObject*>(TargetAnimClass, STR("TargetSkeleton"), TargetSkeleton);
                    }

                    UObject* TargetSkelMesh = nullptr;
                    Utils::GetPropertyValue<UObject*>(TargetMesh, STR("SkeletalMesh"), TargetSkelMesh);
                    if (!TargetSkelMesh) {
                        Utils::GetPropertyValue<UObject*>(TargetMesh, STR("SkinnedAsset"), TargetSkelMesh);
                    }

                    if (TargetSkelMesh && !TargetSkeleton) {
                        Utils::GetPropertyValue<UObject*>(TargetSkelMesh, STR("Skeleton"), TargetSkeleton);
                    }
                }
                Utils::GetPropertyValue<UObject*>(TargetCDO, STR("StaticCharacterParameterComponent"), TargetStaticParam);
            }
        }

        if (!TargetAnimClass) {
            UClass* PalClass = Character->GetClassPrivate();
            if (PalClass) {
                std::wstring ClassName = PalClass->GetName();
                std::wstring CDOName = L"Default__" + ClassName;
                UObject* PalCDO = UObjectGlobals::StaticFindObject<UObject*>(nullptr, PalClass->GetOuterPrivate(), CDOName.c_str());
                
                if (PalCDO) {
                    UObject* CDOMesh = nullptr;
                    Utils::GetPropertyValue<UObject*>(PalCDO, STR("Mesh"), CDOMesh);
                    if (CDOMesh) {
                        Utils::GetPropertyValue<UClass*>(CDOMesh, STR("AnimClass"), TargetAnimClass);
                        if (TargetAnimClass) {
                            Utils::GetPropertyValue<UObject*>(TargetAnimClass, STR("TargetSkeleton"), TargetSkeleton);
                        }

                        UObject* CDOSkelMesh = nullptr;
                        if (!Utils::GetPropertyValue<UObject*>(CDOMesh, STR("SkeletalMesh"), CDOSkelMesh)) {
                            Utils::GetPropertyValue<UObject*>(CDOMesh, STR("SkinnedAsset"), CDOSkelMesh);
                        }
                        if (CDOSkelMesh && !TargetSkeleton) {
                            Utils::GetPropertyValue<UObject*>(CDOSkelMesh, STR("Skeleton"), TargetSkeleton);
                        }
                    }
                    Utils::GetPropertyValue<UObject*>(PalCDO, STR("StaticCharacterParameterComponent"), TargetStaticParam);
                }
            }
        }

        if (!TargetAnimClass) { TargetAnimClass = CurrentAnimClass; }

        bool bNeedsAnimRebuild = (TargetAnimClass != CurrentAnimClass);

        if (bNeedsAnimRebuild) {
            UFunction* SetAnimFunc = MeshComp->GetFunctionByNameInChain(STR("SetAnimInstanceClass"));
            if (!SetAnimFunc) SetAnimFunc = MeshComp->GetFunctionByNameInChain(STR("SetAnimClass"));

            if (SetAnimFunc) {
                struct { UClass* NewClass; } ClearParams{ nullptr };
                MeshComp->ProcessEvent(SetAnimFunc, &ClearParams);
            }
        }

        UObject* NewMesh = nullptr;
        if (!swap.SkelMeshPath.empty()) {
            NewMesh = Utils::LoadSkeletalMeshSafely(swap.SkelMeshPath);
            
            if (!IsPalBlueprintValid(Character, BPName)) return;
            Utils::CallFunction(Character, STR("GetMainMesh"), &MeshComp);
            if (!MeshComp) return;

            if (NewMesh) {
                if (TargetSkeleton) {
                    struct { UObject* NewSkeleton; } SkelParams{ TargetSkeleton };
                    Utils::CallFunction(NewMesh, STR("SetSkeleton"), &SkelParams);
                }

                struct { UObject* InMesh; bool bReinitPose; } MeshParams{NewMesh, false};
                Utils::CallFunction(MeshComp, STR("SetSkinnedAssetAndUpdate"), &MeshParams);
            } else {
                // UI WARNING: Fails to load custom skeletal mesh asset (e.g. wrong path or missing mod files)
                DP_LOG(Error, "Failed to load Skeletal Mesh for Pal '{}' from Pack '{}'!\nPath: {}", CharID, swap.PackName, swap.SkelMeshPath);
            }
        }

        if (bNeedsAnimRebuild) {
            UFunction* SetAnimFunc = MeshComp->GetFunctionByNameInChain(STR("SetAnimInstanceClass"));
            if (!SetAnimFunc) SetAnimFunc = MeshComp->GetFunctionByNameInChain(STR("SetAnimClass"));

            if (TargetAnimClass && SetAnimFunc) {
                struct { UClass* NewClass; } Params{ TargetAnimClass };
                MeshComp->ProcessEvent(SetAnimFunc, &Params);
            }

            if (TargetStaticParam) {
                UObject* CurrentStaticParam = nullptr;
                Utils::GetPropertyValue<UObject*>(Character, STR("StaticCharacterParameterComponent"), CurrentStaticParam);

                if (CurrentStaticParam) {
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
                }
            }
        }

        struct { int32_t RetVal; } NumMatParams{0};
        Utils::CallFunction(MeshComp, STR("GetNumMaterials"), &NumMatParams);
        for (int32_t i = 0; i < NumMatParams.RetVal; ++i) {
            struct { int32_t ElementIndex; UObject* Material; } ClearMatParams{i, nullptr};
            Utils::CallFunction(MeshComp, STR("SetMaterial"), &ClearMatParams);
        }

        // Apply material overrides safely
        //DP_LOG(Default, "=== MATERIAL OVERRIDE PROCESS START (Count: {}) ===", swap.MatReplaceList.size());

        for (auto& mat : swap.MatReplaceList) {
            std::wstring ChosenPath = mat.matPath;
            std::wstring WideIndex = Utils::StringToWString(mat.index);

            //DP_LOG(Default, "[Slot {}] Target Config Path: '{}'", WideIndex, mat.matPath);

            // 1. Is this a wildcard folder path?
            if (mat.matPath.length() >= 2 && mat.matPath.substr(mat.matPath.length() - 2) == L"/*") {
                std::wstring VirtualFolder = mat.matPath.substr(0, mat.matPath.length() - 2);

                DP_LOG(Default, "[Slot {}] Wildcard folder detected. Querying Asset Registry for folder: '{}'", WideIndex, VirtualFolder);

                // ALWAYS call GetAssetsInVirtualFolder first. This forces the Engine to run 
                // ScanPathsSynchronous on the .pak file so it actually knows the materials exist!
                std::vector<std::wstring> AvailableMats = Utils::GetAssetsInVirtualFolder(VirtualFolder);
                DP_LOG(Default, "[Slot {}] Asset Registry returned {} valid material files.", WideIndex, AvailableMats.size());

                // Verbose: Dump every discovered asset inside the target folder
                for (size_t i = 0; i < AvailableMats.size(); ++i) {
                    DP_LOG(Default, "  -> Discovered [{}]: '{}'", i, AvailableMats[i]);
                }

                // 2. NOW check if we already have a saved material for this specific slot
                auto savedMatIt = persist.MatSet.find(mat.index);
                if (savedMatIt != persist.MatSet.end() && !savedMatIt->second.empty()) {
                    ChosenPath = savedMatIt->second;
                    DP_LOG(Verbose, "[Slot {}] Persistent cache HIT. Using saved material path: '{}'", WideIndex, ChosenPath);
                } 
                else {
                    DP_LOG(Verbose, "[Slot {}] Persistent cache MISS. Picking from available materials.", WideIndex);

                    if (!AvailableMats.empty()) {
                        // 3. Pick randomly and persist the choice
                        static std::random_device rd;
                        static std::mt19937 gen(rd());
                        std::uniform_int_distribution<int> dis(0, (int)(AvailableMats.size() - 1));
                        
                        int RolledIndex = dis(gen);
                        ChosenPath = AvailableMats[RolledIndex];
                        persist.MatSet[mat.index] = ChosenPath;
                        
                        DP_LOG(Verbose, "[Slot {}] Rolled Index {} out of {}. Chosen: '{}'", WideIndex, RolledIndex, AvailableMats.size(), ChosenPath);
                    } else {
                        DP_LOG(Warning, "[Slot {}] Wildcard folder '{}' has ZERO matching material files! Skipping slot.", WideIndex, VirtualFolder);
                        continue;
                    }
                }
            } else {
                //DP_LOG(Default, "[Slot {}] Static path detected. Direct apply.", WideIndex);
                persist.MatSet[mat.index] = ChosenPath;
            }

            // 5. Load and apply the chosen material
            //DP_LOG(Default, "[Slot {}] Invoking LoadAssetSafely for: '{}'", WideIndex, ChosenPath);
            UObject* NewMat = Utils::LoadAssetSafely(ChosenPath);
            
            if (!NewMat) {
                DP_LOG(Warning, "[Slot {}] LoadAssetSafely FAILED for path: '{}'", WideIndex, ChosenPath);
                continue;
            }
            //DP_LOG(Default, "[Slot {}] Material loaded successfully: '{}' (Class: '{}')", WideIndex, NewMat->GetName(), NewMat->GetClassPrivate()->GetName().c_str());

            // 6. Verify mesh/blueprint targets are fully ready
            if (!IsPalBlueprintValid(Character, BPName)) {
                DP_LOG(Warning, "[Slot {}] Target character blueprint is INVALID. Aborting apply.", WideIndex);
                return;
            }

            UObject* MeshComp = nullptr;
            Utils::CallFunction(Character, STR("GetMainMesh"), &MeshComp);
            if (!MeshComp) {
                DP_LOG(Warning, "[Slot {}] GetMainMesh returned NULL. Aborting apply.", WideIndex);
                return;
            }
            //DP_LOG(Default, "[Slot {}] Successfully verified MainMesh component: '{}'", WideIndex, MeshComp->GetName());

            // 7. Execute native SetMaterial
            int idx = std::stoi(mat.index);
            //DP_LOG(Default, "[Slot {}] Preparing to execute SetMaterial on index: {}.", WideIndex, idx);
            
            struct { int32_t ElementIndex; UObject* Material; } MatParams{idx, NewMat};
            Utils::CallFunction(MeshComp, STR("SetMaterial"), &MatParams);
            
            //DP_LOG(Default, "[Slot {}] SetMaterial execution completed successfully.", WideIndex);
        }

        //DP_LOG(Default, "=== MATERIAL OVERRIDE PROCESS END ===");


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

        // === COMPILER OPTIMIZATION: ONLY RE-LINK STANDARD LAYERS IF ANIMATION WAS REBUILT ===
        if (bNeedsAnimRebuild) {
            UObject* NewAnimInst = nullptr;
            Utils::CallFunction(MeshComp, STR("GetAnimInstance"), &NewAnimInst);
            if (NewAnimInst) {
                UFunction* LinkFunc = NewAnimInst->GetFunctionByNameInChain(STR("LinkAnimClassLayers"));
                if (LinkFunc) {
                    std::vector<std::wstring> StandardLayers = {
                        L"/Game/Pal/Blueprint/Character/Monster/ABP_MonsterPhysics.ABP_MonsterPhysics_C",
                        L"/Game/Pal/Blueprint/Character/Monster/ABP_MonsterUpper.ABP_MonsterUpper_C",
                        L"/Game/Pal/Blueprint/Character/Monster/ABP_MonsterLookAt.ABP_MonsterLookAt_C",
                        L"/Game/Pal/Blueprint/Character/Monster/ABP_MonsterLeaning.ABP_MonsterLeaning_C"
                    };

                    for (const auto& LayerPath : StandardLayers) {
                        UClass* LayerClass = static_cast<UClass*>(Utils::LoadAssetSafely(LayerPath));
                        
                        if (!IsPalBlueprintValid(Character, BPName)) return;
                        Utils::CallFunction(Character, STR("GetMainMesh"), &MeshComp);
                        if (!MeshComp) return;

                        if (LayerClass) {
                            struct { UClass* InClass; } LinkParams{ LayerClass };
                            NewAnimInst->ProcessEvent(LinkFunc, &LinkParams);
                        }
                    }
                }
            }
        }

        struct { bool bNewDisablePostProcessBlueprint; } DisablePP_False{ false };
        Utils::CallFunction(MeshComp, STR("SetDisablePostProcessBlueprint"), &DisablePP_False);

        struct { bool bPause; } UnpauseAnim{ false };
        Utils::CallFunction(MeshComp, STR("SetPauseAnims"), &UnpauseAnim);

        // -------------------------------------------------------------
        // FIX: Re-initialize Facial UV Warping to prevent weird body stretching
        // When materials or meshes change, the facial component needs to rescan 
        // the material slots to find the new eye/mouth indices. If it doesn't, 
        // it will warp the UVs of whatever new material happens to be at the old index!
        // -------------------------------------------------------------
        UObject* FacialComp = nullptr;
        Utils::GetPropertyValue<UObject*>(Character, STR("PalFacial"), FacialComp);
        
        // If the PalFacial property wasn't found directly, try finding the component by class
        if (!FacialComp) {
            UClass* FacialClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Pal.PalFacialComponent"));
            if (FacialClass) {
                struct { UClass* ComponentClass; UObject* ReturnValue; } GetCompParams{FacialClass, nullptr};
                Utils::CallFunction(Character, STR("GetComponentByClass"), &GetCompParams);
                FacialComp = GetCompParams.ReturnValue;
            }
        }

        // If we successfully found the Facial Component on this Pal, reset its tracking modules
        if (FacialComp) {
            UObject* MainModule = nullptr;
            if (Utils::GetPropertyValue<UObject*>(FacialComp, STR("MainModule"), MainModule) && MainModule) {
                struct { UObject* SkeletalMeshComponent; } SetupParams{ MeshComp };
                UFunction* SetupFunc = MainModule->GetFunctionByNameInChain(STR("Setup_FacialModule"));
                if (SetupFunc) {
                    MainModule->ProcessEvent(SetupFunc, &SetupParams);

                    
                    // --- VERIFICATION LOGS ---
                    int32_t EyeIdx = -2;
                    int32_t MouthIdx = -2;
                    int32_t BrowIdx = -2;
                    
                    Utils::GetPropertyValue<int32_t>(MainModule, STR("EyeMaterialIndex"), EyeIdx);
                    Utils::GetPropertyValue<int32_t>(MainModule, STR("MouthMaterialIndex"), MouthIdx);
                    Utils::GetPropertyValue<int32_t>(MainModule, STR("BrowMaterialIndex"), BrowIdx);
                    
                    //DP_LOG(Normal, "[Facial Fix] Re-indexed {} -> Eye: {}, Mouth: {}, Brow: {}", Character->GetName(), EyeIdx, MouthIdx, BrowIdx);

                }
            }
            
            // Force the face state to refresh visually to prevent it from getting stuck
            UFunction* ChangeFacialFunc = FacialComp->GetFunctionByNameInChain(STR("ChangeDefaultFacial"));
            if (ChangeFacialFunc) {
                FacialComp->ProcessEvent(ChangeFacialFunc, nullptr);
            }
        }


        // NATIVE CONSOLE LOG (Kept as 'Normal' level so it stays out of the UI and doesn't clutter player gameplay screen)
        //DP_LOG(Default, "Successfully applied swap '{}' from Pack '{}' to Pal '{}'!\n", swap.SkinName.empty() ? L"Mesh Swap" : swap.SkinName, swap.PackName, CharID);
    }
}