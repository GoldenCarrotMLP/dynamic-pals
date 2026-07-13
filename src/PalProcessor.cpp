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

        // NEW: Fast Execution Cache
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


    

    // Fetches Level, Rank, and Friendship/Trust from a Pal with sentinel verification and fallback checks
    PalRuntimeStats RetrievePalStats(UObject* IndivParam, const std::wstring& RawCharID, const std::wstring& InstanceID, bool bLogWarnings) {
        PalRuntimeStats stats;
        stats.Level = -1;
        stats.Rank = -1;
        stats.Friendship = -1;
        if (!IndivParam) return stats;

        // SAFE Lazy-init: Only lock the init flag if we ACTUALLY found the core function!
        if (!GCachedProps.bIsStatsInit) {
            GCachedProps.GetLevelFunc = IndivParam->GetFunctionByNameInChain(STR("GetLevel"));
            GCachedProps.GetRankFunc = IndivParam->GetFunctionByNameInChain(STR("GetRank"));
            GCachedProps.GetFriendshipRankFunc = IndivParam->GetFunctionByNameInChain(STR("GetFriendshipRank"));
            GCachedProps.GetFriendshipPointFunc = IndivParam->GetFunctionByNameInChain(STR("GetFriendshipPoint"));
            
            // Only stop searching if we successfully resolved the pointers
            if (GCachedProps.GetLevelFunc) {
                GCachedProps.bIsStatsInit = true;
            }
        }

        struct { int32_t RetVal = -1; } IntParams;

        if (GCachedProps.GetLevelFunc) {
            IntParams.RetVal = -1;
            IndivParam->ProcessEvent(GCachedProps.GetLevelFunc, &IntParams);
            stats.Level = IntParams.RetVal;
        }

        if (GCachedProps.GetRankFunc) {
            IntParams.RetVal = -1;
            IndivParam->ProcessEvent(GCachedProps.GetRankFunc, &IntParams);
            stats.Rank = IntParams.RetVal;
        }

        if (GCachedProps.GetFriendshipRankFunc) {
            IntParams.RetVal = -1;
            IndivParam->ProcessEvent(GCachedProps.GetFriendshipRankFunc, &IntParams);
            stats.Friendship = IntParams.RetVal;
        } else if (GCachedProps.GetFriendshipPointFunc) {
            IntParams.RetVal = -1;
            IndivParam->ProcessEvent(GCachedProps.GetFriendshipPointFunc, &IntParams);
            stats.Friendship = IntParams.RetVal;
        }

        // Apply defaults silently to avoid massive file I/O bottlenecks in the console
        if (stats.Level == -1) stats.Level = 1;
        if (stats.Rank == -1) stats.Rank = 0;
        if (stats.Friendship == -1) stats.Friendship = 0;

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
        if (!GCachedProps.GetDatabaseCharacterParameterFunc) {
            GCachedProps.GetDatabaseCharacterParameterFunc = PalUtil->GetFunctionByNameInChain(STR("GetDatabaseCharacterParameter"));
        }
        if (!GCachedProps.GetDatabaseCharacterParameterFunc) return false;
        
        PalUtil->ProcessEvent(GCachedProps.GetDatabaseCharacterParameterFunc, &GetDBParams);
        UObject* DB = GetDBParams.DB;
        if (!DB) return false;

        // 2. Fetch the native GetBPClass function
        if (!GCachedProps.GetBPClassFunc) {
            GCachedProps.GetBPClassFunc = DB->GetFunctionByNameInChain(STR("GetBPClass"));
        }
        if (!GCachedProps.GetBPClassFunc) return false;

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
        DB->ProcessEvent(GCachedProps.GetBPClassFunc, &Params);

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
            DP_LOG(Verbose, "Pal '{}' is being destroyed. Skipping.", OutBlueprintName);
        }

        //Disabled to allow pals inside of the box to be swapped

        //bool bIsActive = true;
        //if (Utils::GetPropertyValue<bool>(Pal, STR("bIsPalActiveActor"), bIsActive)) {
        //    if (!bIsActive) return false;
        //}

        UObject* MeshComp = nullptr;
        Utils::CallFunction(Pal, STR("GetMainMesh"), &MeshComp);
        if (!MeshComp) {
            return false;
        }

        // CORRECTED: Validate that the component is alive and tracked by the engine
        if (!Utils::IsObjectValid(MeshComp)) {
            return false;
        }

        //Disabled so pals inside of the palbox can get their skeleton swapped

        //struct { bool ReturnValue; } ColParams{true};
        //Utils::CallFunction(Pal, STR("GetActorEnableCollision"), &ColParams);
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

    // ==========================================
    // ULTIMATE PERFORMANCE CACHE
    // Caches Unreal's reflection pointers statically so we NEVER 
    // do string comparisons inside the ticking loop!
    // ==========================================
    
    
    
    void PalProcessor::ScanActivePals() {
        return;
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - LastScanTime).count() < 3000) return;
        LastScanTime = now;

        UObject* PalUtil = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
        if (!PalUtil) return;

        UObject* WorldContext = UObjectGlobals::FindFirstOf(STR("PalPlayerController"));
        if (!WorldContext) return;

        auto start = std::chrono::high_resolution_clock::now();

        // SAFE Inline Lazy-Init: Only lock if resolved!
        if (!GCachedProps.bIsGlobalsInit) {
            GCachedProps.GetPalCharactersFunc = PalUtil->GetFunctionByNameInChain(STR("GetPalCharacters"));
            GCachedProps.IsOtomoFunc = PalUtil->GetFunctionByNameInChain(STR("IsOtomo"));
            GCachedProps.IsBaseCampPalFunc = PalUtil->GetFunctionByNameInChain(STR("IsBaseCampPal"));
            
            if (GCachedProps.GetPalCharactersFunc) GCachedProps.bIsGlobalsInit = true;
        }

        if (!GCachedProps.GetPalCharactersFunc || !GCachedProps.IsOtomoFunc || !GCachedProps.IsBaseCampPalFunc) return;

        struct { UObject* WorldContextObject; TArray<UObject*> OutCharacters; } Params;
        Params.WorldContextObject = WorldContext;

        PalUtil->ProcessEvent(GCachedProps.GetPalCharactersFunc, &Params);

        for (int32_t i = 0; i < Params.OutCharacters.Num(); ++i) {
            UObject* Pal = Params.OutCharacters[i];
            if (!Pal) continue;

            bool bIsRelevant = false;
            struct { UObject* Actor; bool RetVal; } BoolParams{Pal, false};
            
            PalUtil->ProcessEvent(GCachedProps.IsOtomoFunc, &BoolParams);
            if (BoolParams.RetVal) {
                bIsRelevant = true;
            } else {
                PalUtil->ProcessEvent(GCachedProps.IsBaseCampPalFunc, &BoolParams);
                if (BoolParams.RetVal) bIsRelevant = true;
            }

            if (bIsRelevant) {
                if (ProcessedPals.find(Pal) == ProcessedPals.end()) {
                    ProcessedPals.insert(Pal);
                    ProcessPal(Pal, false);
                } else {
                    // Inline Lazy-Init properties using the live Actor Class
                    if (!GCachedProps.bIsPropsInit) {
                        UClass* PalClass = Pal->GetClassPrivate();
                        if (PalClass) GCachedProps.CharParamCompProp = PalClass->GetPropertyByNameInChain(STR("CharacterParameterComponent"));
                    }

                    UObject* ParamComp = nullptr;
                    if (GCachedProps.CharParamCompProp) {
                        void* ValuePtr = GCachedProps.CharParamCompProp->ContainerPtrToValuePtr<void>(Pal);
                        if (ValuePtr) ParamComp = *reinterpret_cast<UObject**>(ValuePtr);
                    } else {
                        Utils::GetPropertyValue<UObject*>(Pal, STR("CharacterParameterComponent"), ParamComp);
                    }

                    if (ParamComp) {
                        UObject* IndivParam = nullptr;
                        
                        if (!GCachedProps.bIsPropsInit) {
                            UClass* ParamCompClass = ParamComp->GetClassPrivate();
                            if (ParamCompClass) GCachedProps.IndivParamProp = ParamCompClass->GetPropertyByNameInChain(STR("IndividualParameter"));
                        }

                        if (GCachedProps.IndivParamProp) {
                            void* ValuePtr = GCachedProps.IndivParamProp->ContainerPtrToValuePtr<void>(ParamComp);
                            if (ValuePtr) IndivParam = *reinterpret_cast<UObject**>(ValuePtr);
                        } else {
                            Utils::GetPropertyValue<UObject*>(ParamComp, STR("IndividualParameter"), IndivParam);
                        }

                        if (IndivParam) {
                            FPalInstanceID IDStruct;
                            bool bGotId = false;

                            if (!GCachedProps.bIsPropsInit) {
                                UClass* IndivClass = IndivParam->GetClassPrivate();
                                if (IndivClass) GCachedProps.IndivIdProp = IndivClass->GetPropertyByNameInChain(STR("IndividualId"));
                                
                                // Lock the prop cache once we successfully find the final ID property
                                if (GCachedProps.IndivIdProp) GCachedProps.bIsPropsInit = true; 
                            }
                            
                            if (GCachedProps.IndivIdProp) {
                                void* ValuePtr = GCachedProps.IndivIdProp->ContainerPtrToValuePtr<void>(IndivParam);
                                if (ValuePtr) {
                                    IDStruct = *reinterpret_cast<FPalInstanceID*>(ValuePtr);
                                    bGotId = true;
                                }
                            } else {
                                bGotId = Utils::GetPropertyValue<FPalInstanceID>(IndivParam, STR("IndividualId"), IDStruct);
                            }

                            if (bGotId && IDStruct.InstanceId.IsValid()) {
                                std::wstring InstanceID = Utils::GuidToWString(IDStruct.InstanceId);
                                auto it = RuntimeStatsCache.find(InstanceID);
                                
                                if (it != RuntimeStatsCache.end()) {
                                    PalRuntimeStats stats = RetrievePalStats(IndivParam, L"", InstanceID, false); 

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

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        
        RC::Output::send<RC::LogLevel::Default>(
            STR("[DynPals] [Perf - Ultra Optimized] ScanActivePals evaluated {} characters in {} us ({:.3f} ms)\n"), 
            Params.OutCharacters.Num(), duration, duration / 1000.0f
        );
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
        
        float DelaySeconds = VFXManager::Get().PlayComposition(Character, CompName);
        int DelayMs = static_cast<int>(DelaySeconds * 1000.0f);
        
        std::thread([Character, DelayMs]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(DelayMs));
            AsyncHelper::AsyncTask(ENamedThreads::GameThread, [Character]() {
                
                // SAFE CHECK: Consolidated & cached validation
                if (!Utils::IsObjectValid(Character)) return; 

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
            
            ExistingData->MorphSet.clear();
            ExistingData->MatSet.clear();
            ExistingData->MatColorSet.clear();
            
            SaveManager::Get().SetPersistData(InstanceID, *ExistingData, true); 
        }

        std::thread([Character, SwapIndex, DelayMs]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(DelayMs));
            
            AsyncHelper::AsyncTask(ENamedThreads::GameThread, [Character, SwapIndex]() {
                
                // SAFE CHECK: Consolidated & cached validation
                if (!Utils::IsObjectValid(Character)) return; 

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
        
        // --- 1. CORE GLOBALS CACHING ---
        if (PalUtil && !GCachedProps.bIsCoreGlobalsInit) {
            GCachedProps.GetCharacterIDFromCharacterFunc = PalUtil->GetFunctionByNameInChain(STR("GetCharacterIDFromCharacter"));
            GCachedProps.IsWildNPCFunc = PalUtil->GetFunctionByNameInChain(STR("IsWildNPC"));
            
            if (IndivParam) {
                GCachedProps.IsRarePalFunc = IndivParam->GetFunctionByNameInChain(STR("IsRarePal"));
                GCachedProps.GetGenderTypeFunc = IndivParam->GetFunctionByNameInChain(STR("GetGenderType"));
                GCachedProps.GetSkinNameFunc = IndivParam->GetFunctionByNameInChain(STR("GetSkinName"));
                GCachedProps.GetPassiveSkillListFunc = IndivParam->GetFunctionByNameInChain(STR("GetPassiveSkillList"));
            }
            GCachedProps.bIsCoreGlobalsInit = true;
        }

        struct { UObject* Char; FName RetVal; } CharIDParams{Character, FName()};
        if (PalUtil && GCachedProps.GetCharacterIDFromCharacterFunc) {
            PalUtil->ProcessEvent(GCachedProps.GetCharacterIDFromCharacterFunc, &CharIDParams);
        }
        std::wstring RawCharID = CharIDParams.RetVal.ToString();

        if (RawCharID.rfind(L"GYM_", 0) == 0 || RawCharID.find(L"_Gym_") != std::wstring::npos) return;

        // --- 2. OPTIMIZE SAVE MANAGER ACCESS ---
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

        // --- 3. ZERO-REFLECTION PARAMETER FETCHING ---
        struct { UObject* Actor; bool RetVal; } WildParams{Character, false};
        if (PalUtil && GCachedProps.IsWildNPCFunc) PalUtil->ProcessEvent(GCachedProps.IsWildNPCFunc, &WildParams);
        bool IsWild = WildParams.RetVal;

        struct { bool ReturnValue; } RareParams{false};
        if (GCachedProps.IsRarePalFunc) IndivParam->ProcessEvent(GCachedProps.IsRarePalFunc, &RareParams);
        bool IsRare = RareParams.ReturnValue;

        struct { uint8_t RetVal; } GenderParams{0};
        if (GCachedProps.GetGenderTypeFunc) IndivParam->ProcessEvent(GCachedProps.GetGenderTypeFunc, &GenderParams);
        std::wstring GenderStr = (GenderParams.RetVal == 1) ? L"Male" : ((GenderParams.RetVal == 2) ? L"Female" : L"None");

        struct { FName RetVal; } SkinParams{FName()};
        if (GCachedProps.GetSkinNameFunc) IndivParam->ProcessEvent(GCachedProps.GetSkinNameFunc, &SkinParams);
        std::wstring SkinName = SkinParams.RetVal.ToString();
        if (SkinName == L"None") SkinName = L"";

        std::vector<std::wstring> Traits;
        struct { TArray<FName> RetVal; } TraitsParams;
        if (GCachedProps.GetPassiveSkillListFunc) {
            IndivParam->ProcessEvent(GCachedProps.GetPassiveSkillListFunc, &TraitsParams);
            for (int32_t i = 0; i < TraitsParams.RetVal.Num(); ++i) {
                Traits.push_back(TraitsParams.RetVal[i].ToString());
            }
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
                //DP_LOG(Default, "[Debug Swap] Proceeding to Swap Pal '{}' (ID: '{}', Actor: {}). Reason: {}", 
                //    RawCharID, InstanceID, (void*)Character,
                //    (ExplicitSwapIndex != -1) ? L"Explicit Selection" : 
                //    (ForceReroll) ? L"Force Reroll" : 
                //    (finalSwap != currentSwap) ? L"Skin Changed" : L"New Actor Spawned");

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
                        
                        //DP_LOG(Default, "[Nickname] Applied name update for '{}' -> '{}' (Wild: {})", (InstanceID), (finalConfig.SetNickname), IsWild ? L"True" : L"False");
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

        Utils::SetPropertyValue<bool>(MeshComp, STR("bPauseAnims"), true, false);

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
        
        // Helper to perform case-insensitive comparisons
        auto ToLowerW = [](std::wstring str) {
            std::transform(str.begin(), str.end(), str.begin(), ::towlower);
            return str;
        };

        // ONLY perform a heavy blocking disk-load if the user explicitly requested a DIFFERENT animation blueprint!
        bool bNeedsExternalAnimLoad = !AnimPath.empty() && ToLowerW(AnimPath) != ToLowerW(CharID);

        if (bNeedsExternalAnimLoad) {
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

                // Fallback: Guess paths if the ID isn't registered in the native Database
                if (!TargetBPClass) {
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
                        if (!Utils::GetPropertyValue<UObject*>(TargetMesh, STR("SkeletalMesh"), TargetSkelMesh)) {
                            Utils::GetPropertyValue<UObject*>(TargetMesh, STR("SkinnedAsset"), TargetSkelMesh);
                        }

                        if (TargetSkelMesh && !TargetSkeleton) {
                            Utils::GetPropertyValue<UObject*>(TargetSkelMesh, STR("Skeleton"), TargetSkeleton);
                        }
                    }
                    Utils::GetPropertyValue<UObject*>(TargetCDO, STR("StaticCharacterParameterComponent"), TargetStaticParam);
                }
            }
        } else {
            // --- FAST PATH: Extract directly from the live Actor in RAM (Zero-Disk Overhead) ---
            TargetAnimClass = CurrentAnimClass;
            Utils::GetPropertyValue<UObject*>(Character, STR("StaticCharacterParameterComponent"), TargetStaticParam);
            
            UObject* CurrentSkelMesh = nullptr;
            if (!Utils::GetPropertyValue<UObject*>(MeshComp, STR("SkeletalMesh"), CurrentSkelMesh)) {
                Utils::GetPropertyValue<UObject*>(MeshComp, STR("SkinnedAsset"), CurrentSkelMesh);
            }
            if (CurrentSkelMesh) {
                Utils::GetPropertyValue<UObject*>(CurrentSkelMesh, STR("Skeleton"), TargetSkeleton);
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
                DP_LOG(Warning, "Failed to load Skeletal Mesh for Pal '{}' from Pack '{}'!\nPath: {}", CharID, swap.PackName, swap.SkelMeshPath);
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
                     // --- Animations & Movement ---
                    CopyProp(TargetStaticParam, CurrentStaticParam, STR("RandomRestMontageInfos"));
                    CopyProp(TargetStaticParam, CurrentStaticParam, STR("GeneralAnimSequenceMap"));
                    CopyProp(TargetStaticParam, CurrentStaticParam, STR("GeneralMontageMap"));
                    CopyProp(TargetStaticParam, CurrentStaticParam, STR("GeneralBlendSpaceMap"));
                    CopyProp(TargetStaticParam, CurrentStaticParam, STR("ActionMontageMap"));
                    CopyProp(TargetStaticParam, CurrentStaticParam, STR("SleepOnSideAnimMontage"));
                    
                    // --- Fix: Player Petting Posture & Distances ---
                    CopyProp(TargetStaticParam, CurrentStaticParam, STR("PettingSize"));
                    CopyProp(TargetStaticParam, CurrentStaticParam, STR("PettingStartAddDistance"));
                    CopyProp(TargetStaticParam, CurrentStaticParam, STR("PettingEndLeaveDistance"));
                    CopyProp(TargetStaticParam, CurrentStaticParam, STR("PettingDistance"));
                    
                    // --- Fix: UI Positioning (HP Bar Height) ---
                    CopyProp(TargetStaticParam, CurrentStaticParam, STR("HPGaugeUIOffset"));
                    
                    // --- Fix: Bed Alignment & Sleeping Positions ---
                    CopyProp(TargetStaticParam, CurrentStaticParam, STR("SleepOnSideInfoMapForMapObject"));
                    
                    
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

            UObject* NewMat = nullptr;
            if (!ChosenPath.empty()) {
                NewMat = Utils::LoadAssetSafely(ChosenPath);
                if (!NewMat) {
                    DP_LOG(Warning, "[Slot {}] LoadAssetSafely FAILED for path: '{}'", WideIndex, ChosenPath);
                    continue;
                }
            } else {
                struct { int32_t ElementIndex; UObject* ReturnValue; } GetMatParams{idx, nullptr};
                Utils::CallFunction(CurrentMeshComp, STR("GetMaterial"), &GetMatParams);
                NewMat = GetMatParams.ReturnValue;
                
                if (!NewMat) {
                    DP_LOG(Warning, "[Slot {}] Failed to retrieve existing material for dynamic application.", WideIndex);
                    continue;
                }
            }


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
                // Resolve the material library and the target function with zero path string duplication
                UObject* KML = Utils::GetKML();
                UFunction* CreateFunc = Utils::GetKMLFunction(STR("CreateDynamicMaterialInstance"));

                // Explicit local caching for properties
                static FProperty* WCProp = CreateFunc ? CreateFunc->GetPropertyByNameInChain(STR("WorldContextObject")) : nullptr;
                static FProperty* ParentProp = CreateFunc ? CreateFunc->GetPropertyByNameInChain(STR("Parent")) : nullptr;
                static FProperty* NameProp = CreateFunc ? CreateFunc->GetPropertyByNameInChain(STR("OptionalName")) : nullptr;
                static FProperty* RetProp = CreateFunc ? CreateFunc->GetPropertyByNameInChain(STR("ReturnValue")) : nullptr;

                UObject* MID = nullptr;

                if (KML && CreateFunc) {
                    alignas(8) uint8_t MIDParams[128] = {0};

                    if (WCProp) *WCProp->ContainerPtrToValuePtr<UObject*>(MIDParams) = Character;
                    if (ParentProp) *ParentProp->ContainerPtrToValuePtr<UObject*>(MIDParams) = NewMat;
                    if (NameProp) *NameProp->ContainerPtrToValuePtr<FName>(MIDParams) = FName();

                    KML->ProcessEvent(CreateFunc, MIDParams);

                    if (RetProp) MID = *RetProp->ContainerPtrToValuePtr<UObject*>(MIDParams);
                }

                if (MID) {
                    static UFunction* SetVecFunc = MID->GetFunctionByNameInChain(STR("SetVectorParameterValue"));
                    static FProperty* NamePropVec = SetVecFunc ? SetVecFunc->GetPropertyByNameInChain(STR("ParameterName")) : nullptr;
                    static FProperty* ValPropVec = SetVecFunc ? SetVecFunc->GetPropertyByNameInChain(STR("Value")) : nullptr;

                    if (SetVecFunc) {
                        alignas(8) uint8_t VecParams[128] = {0};
                        if (NamePropVec) *NamePropVec->ContainerPtrToValuePtr<FName>(VecParams) = FName(STR("Hue"), FNAME_Add);
                        if (ValPropVec) *ValPropVec->ContainerPtrToValuePtr<FLinearColor_UE5>(VecParams) = appliedColor;
                        
                        MID->ProcessEvent(SetVecFunc, VecParams);
                    }
                    
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

        Utils::SetPropertyValue<bool>(MeshComp, STR("bPauseAnims"), false, false);

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
                } else {
                    DP_LOG(Warning, "[Facial] MainModule on Pal '{}' is missing 'Setup_FacialModule' function. Face textures may stretch.", Character->GetName());
                }
            } else {
                DP_LOG(Warning, "[Facial] Failed to retrieve 'MainModule' from FacialComponent on Pal '{}'.", Character->GetName());
            }
        } else {
            // Replaced with verbose log so it doesn't clutter normal play, but is searchable
            DP_LOG(Verbose, "[Facial] FacialComponent not found on Pal '{}'.", Character->GetName());
        }


        // NATIVE CONSOLE LOG (Kept as 'Normal' level so it stays out of the UI and doesn't clutter player gameplay screen)
        //DP_LOG(Default, "Successfully applied swap '{}' from Pack '{}' to Pal '{}'!\n", swap.SkinName.empty() ? L"Mesh Swap" : swap.SkinName, swap.PackName, CharID);
    }
}