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

        UObject* PalUtil = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
        if (!PalUtil) return;

        // Safely fetch an active world context object (Stops instantly upon finding the player)
        UObject* WorldContext = UObjectGlobals::FindFirstOf(STR("PalPlayerController"));
        if (!WorldContext) return;

        // Bypass FindAllOf! Query the game's internal cached array directly for zero-overhead
        struct { UObject* WorldContextObject; TArray<UObject*> OutCharacters; } Params;
        Params.WorldContextObject = WorldContext;

        Utils::CallFunction(PalUtil, STR("GetPalCharacters"), &Params);

        // Safety warning if no Pals are returned by the engine scanner
        static bool bLoggedZeroWarn = false;
        if (Params.OutCharacters.Num() == 0) {
            if (!bLoggedZeroWarn) {
                DP_LOG(Warning, "[ScanActivePals] GetPalCharacters returned 0 active Pals. No characters are currently tracked by the world.");
                bLoggedZeroWarn = true;
            }
        } else {
            bLoggedZeroWarn = false; // Reset state when Pals are successfully discovered
        }

        for (int32_t i = 0; i < Params.OutCharacters.Num(); ++i) {
            UObject* Pal = Params.OutCharacters[i];
            if (!Pal) continue;

            bool bIsRelevant = false;

            // 1. Is this Pal currently summoned as an Otomo (Player's Party)?
            struct { UObject* Actor; bool RetVal; } OtomoParams{Pal, false};
            Utils::CallFunction(PalUtil, STR("IsOtomo"), &OtomoParams);
            if (OtomoParams.RetVal) bIsRelevant = true;

            // 2. Is this Pal actively assigned to a Base Camp?
            if (!bIsRelevant) {
                struct { UObject* Actor; bool RetVal; } CampParams{Pal, false};
                Utils::CallFunction(PalUtil, STR("IsBaseCampPal"), &CampParams);
                if (CampParams.RetVal) bIsRelevant = true;
            }

            // We only process math if they are on our team or in a base camp!
            if (bIsRelevant) {
                if (ProcessedPals.find(Pal) == ProcessedPals.end()) {
                    ProcessedPals.insert(Pal);
                    ProcessPal(Pal, false);
                } else {
                    UObject* ParamComp = nullptr;
                    Utils::GetPropertyValue<UObject*>(Pal, STR("CharacterParameterComponent"), ParamComp);
                    if (ParamComp) {
                        UObject* IndivParam = nullptr;
                        Utils::GetPropertyValue<UObject*>(ParamComp, STR("IndividualParameter"), IndivParam);
                        if (IndivParam) {
                            FPalInstanceID IDStruct;
                            if (Utils::GetPropertyValue<FPalInstanceID>(IndivParam, STR("IndividualId"), IDStruct) && IDStruct.InstanceId.IsValid()) {
                                std::wstring InstanceID = Utils::GuidToWString(IDStruct.InstanceId);
                                auto it = RuntimeStatsCache.find(InstanceID);
                                
                                if (it != RuntimeStatsCache.end()) {
                                    // Silently poll the current engine parameters using our refactored RetrievePalStats helper
                                    PalRuntimeStats stats = RetrievePalStats(IndivParam, L"", InstanceID, false);

                                    // If any dynamic stat changed under the hood, trigger the update
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

        FPalInstanceID InstanceIDStruct;
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
            ExistingData->MatColorSet.clear();
            
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

        FPalInstanceID InstanceIDStruct;
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
        
        // Extract the current skin label so the engine knows the Pal's evolutionary state
        std::wstring CurrentSwapLabel = ExistingData ? ExistingData->SwapLabel : L"";
        
        bool bLiveEventTriggered = (CachedStats.Level != -1); 
        

        CachedStats.Level = LevelNum;
        CachedStats.Rank = RankNum;
        CachedStats.Friendship = FriendshipNum;

        int currentSwap = -1;
        if (ExistingData && ExistingData->HasSavedSwap()) {
            currentSwap = ConfigManager::Get().FindConfigIndex(ExistingData->PackName, ExistingData->SkinName, ExistingData->SwapLabel, ExistingData->SkelMeshPath, CharID);
        }

        int finalSwap = -1;

        if (ExplicitSwapIndex != -1) {
            finalSwap = ExplicitSwapIndex;
        } 
        else {
            auto it = SwappedInstances.find(InstanceID);
            

            auto evaluations = ConfigManager::Get().EvaluateAllSwaps(CharID, IsRare, GenderStr, Traits, LevelNum, SkinName, RankNum, FriendshipNum, IsWild, CurrentSwapLabel);
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
                        int absoluteBestScore = 999999;
                        for (const auto& ev : evaluations) {
                            if (ev.IsValid && ev.Score < absoluteBestScore) {
                                absoluteBestScore = ev.Score;
                            }
                        }

                        // Always evolve if the current skin became invalid OR a strictly better skin is available
                        if (!currentEval->IsValid || currentEval->Score > absoluteBestScore) {
                            DP_LOG(Verbose, "Live Event: Better skin found or current became invalid. Upgrading skin.\n");
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
                    newData.MatColorSet.clear(); 
                }

                // --- NEW: INDEPENDENT AUTOMATIC NICKNAME ENGINE ---
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
                                            std::wstring curName = pNickName->GetCharArray().GetData() ? pNickName->GetCharArray().GetData() : L"";
                                            if (curName.empty()) {
                                                bNicknameIsEmpty = true;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // Set the name if we are forcing it, selecting it explicitly, changing skins, or if the Pal has no custom name yet!
                    bool bShouldSetNickname = ForceReroll || (ExplicitSwapIndex != -1) || (finalSwap != currentSwap) || bNicknameIsEmpty;

                    if (bShouldSetNickname) {
                        // 1. Force the name change client-side safely using Reflection (Works 100% on wild Pals!)
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

                        // 2. If it is NOT a wild Pal, tell the server so it permanently saves in the database
                        if (!IsWild) {
                            UObject* PlayerController = UObjectGlobals::FindFirstOf(STR("PalPlayerController"));
                            if (PlayerController) {
                                UFunction* UpdateNameFunc = PlayerController->GetFunctionByNameInChain(STR("UpdateCharacterNickName_ToServer"));
                                if (UpdateNameFunc) {
                                    struct { 
                                        FPalInstanceID InstanceId; 
                                        FString NewNickName; 
                                    } Params;
                                    Params.InstanceId = InstanceIDStruct;
                                    Params.NewNickName = FString(finalConfig.SetNickname.c_str());
                                    
                                    PlayerController->ProcessEvent(UpdateNameFunc, &Params);
                                }
                            }
                        }
                        
                        DP_LOG(Default, "[Nickname] Applied name update for '{}' -> '{}' (Wild: {})", (InstanceID), (finalConfig.SetNickname), IsWild ? L"True" : L"False");
                    }
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
            std::wstring ResolvedPath;
            if (ResolvePalBlueprintPath(Character, AnimPath, ResolvedPath)) {
                TargetBPClass = static_cast<UClass*>(Utils::LoadAssetSafely(ResolvedPath));
                if (TargetBPClass) {
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

        // --- MATERIAL APPLICATION LOOP ---
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

            UObject* NewMat = Utils::LoadAssetSafely(ChosenPath);
            if (!NewMat) {
                DP_LOG(Warning, "[Slot {}] LoadAssetSafely FAILED for path: '{}'", WideIndex, ChosenPath);
                continue;
            }

            if (!IsPalBlueprintValid(Character, BPName)) {
                return;
            }

            UObject* CurrentMeshComp = nullptr;
            Utils::CallFunction(Character, STR("GetMainMesh"), &CurrentMeshComp);
            if (!CurrentMeshComp) {
                return;
            }

            int idx = 0;
            try { idx = std::stoi(mat.index); } catch(...) { continue; }

            // --- DYNAMIC MATERIAL INSTANCE & HUE GENERATOR ---
            if (mat.bRandomHue) {
                FLinearColor_UE5 appliedColor;
                auto colorIt = persist.MatColorSet.find(mat.index);
                
                if (colorIt != persist.MatColorSet.end()) {
                    appliedColor = colorIt->second; // Use persisted color
                } else {
                    // Generate vibrant random color using HSV to RGB
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

                // Create the Dynamic Material Instance
                UObject* KML = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetMaterialLibrary"));
                UFunction* CreateMIDFunc = KML ? KML->GetFunctionByNameInChain(STR("CreateDynamicMaterialInstance")) : nullptr;
                UObject* MID = nullptr;

                if (CreateMIDFunc) {
                    alignas(8) uint8_t MIDParams[128] = {0};

                    FProperty* WCProp = CreateMIDFunc->GetPropertyByNameInChain(STR("WorldContextObject"));
                    if (WCProp) *WCProp->ContainerPtrToValuePtr<UObject*>(MIDParams) = Character;

                    FProperty* ParentProp = CreateMIDFunc->GetPropertyByNameInChain(STR("Parent"));
                    if (ParentProp) *ParentProp->ContainerPtrToValuePtr<UObject*>(MIDParams) = NewMat;

                    FProperty* NameProp = CreateMIDFunc->GetPropertyByNameInChain(STR("OptionalName"));
                    if (NameProp) *NameProp->ContainerPtrToValuePtr<FName>(MIDParams) = FName();

                    KML->ProcessEvent(CreateMIDFunc, MIDParams);

                    FProperty* RetProp = CreateMIDFunc->GetPropertyByNameInChain(STR("ReturnValue"));
                    if (RetProp) MID = *RetProp->ContainerPtrToValuePtr<UObject*>(MIDParams);
                }

                if (MID) {
                    UFunction* SetVecFunc = MID->GetFunctionByNameInChain(STR("SetVectorParameterValue"));
                    if (SetVecFunc) {
                        alignas(8) uint8_t VecParams[128] = {0};
                        
                        FProperty* NameProp = SetVecFunc->GetPropertyByNameInChain(STR("ParameterName"));
                        if (NameProp) *NameProp->ContainerPtrToValuePtr<FName>(VecParams) = FName(STR("Hue"), FNAME_Add);
                        
                        FProperty* ValProp = SetVecFunc->GetPropertyByNameInChain(STR("Value"));
                        if (ValProp) *ValProp->ContainerPtrToValuePtr<FLinearColor_UE5>(VecParams) = appliedColor;
                        
                        MID->ProcessEvent(SetVecFunc, VecParams);
                    }
                    
                    // Verify that the color applied correctly using engine readbacks
                    UFunction* GetVecFunc = MID->GetFunctionByNameInChain(STR("K2_GetVectorParameterValue"));
                    if (GetVecFunc) {
                        alignas(8) uint8_t GetParams[128] = {0};
                        FProperty* NamePropGet = GetVecFunc->GetPropertyByNameInChain(STR("ParameterName"));
                        if (NamePropGet) *NamePropGet->ContainerPtrToValuePtr<FName>(GetParams) = FName(STR("Hue"), FNAME_Add);
                        
                        MID->ProcessEvent(GetVecFunc, GetParams);
                        
                        FProperty* RetPropGet = GetVecFunc->GetPropertyByNameInChain(STR("ReturnValue"));
                        if (RetPropGet) {
                            FLinearColor_UE5* ReadColor = RetPropGet->ContainerPtrToValuePtr<FLinearColor_UE5>(GetParams);
                            DP_LOG(Default, "[RandomHue] Successfully pushed Hue Shift to MID on slot {}. Readback Color -> R:{:.2f} G:{:.2f} B:{:.2f}", idx, ReadColor->R, ReadColor->G, ReadColor->B);
                        }
                    }
                    // --------------------------
                    
                    // Apply the tinted Dynamic Instance directly to the mesh
                    struct { int32_t ElementIndex; UObject* Material; } MatParams{idx, MID};
                    Utils::CallFunction(CurrentMeshComp, STR("SetMaterial"), &MatParams);
                    continue;
                }
            }

            // Standard fallback applicationbut it should work. im using that exact same function
            struct { int32_t ElementIndex; UObject* Material; } MatParams{idx, NewMat};
            Utils::CallFunction(CurrentMeshComp, STR("SetMaterial"), &MatParams);
        }

        // --- MORPH TARGET APPLICATION ---
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