#include "PalProcessor.hpp"
#include "ConfigManager.hpp"
#include "SaveManager.hpp"
#include "Utils.hpp"
#include <random>

#include <Unreal/UObject.hpp>
#include <Unreal/FString.hpp>
#include <Unreal/Core/Containers/Array.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace DynPals {

    void PalProcessor::ClearAllSwappedStatus() {
        SwappedInstances.clear();
        RuntimeStatsCache.clear();
        ProcessedPals.clear();
        PendingSwaps.clear();
        ProcessingQueue.clear();
    }

    void PalProcessor::ClearSwappedStatus(const std::wstring& InstanceID) {
        SwappedInstances.erase(InstanceID);
        RuntimeStatsCache.erase(InstanceID);
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
        return InputID;
    }

    void PalProcessor::ScanActivePals() {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - LastScanTime).count() < 1000) return;
        LastScanTime = now;

        std::vector<UObject*> AllPals;
        UObjectGlobals::FindAllOf(STR("PalCharacter"), AllPals);

        for (UObject* Pal : AllPals) {
            if (Pal && ProcessedPals.find(Pal) == ProcessedPals.end()) {
                ProcessedPals.insert(Pal);
                ProcessPal(Pal, false);
            }
        }
    }

    void PalProcessor::TickDeferredSwaps() {
        auto now = std::chrono::steady_clock::now();
        
        for (auto it = PendingSwaps.begin(); it != PendingSwaps.end();) {
            if (now >= it->ScheduledTime) {
                if (it->Character) {
                    // FIX: Pass explicit UI selection index instead of destroying it!
                    ExecuteSwap(it->Character, false, it->SwapIndex);
                }
                it = PendingSwaps.erase(it);
            } else {
                ++it;
            }
        }
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

        PalPersistData* ExistingData = SaveManager::Get().GetPersistData(InstanceID);
        if (!ExistingData) {
            PalPersistData newData = { InstanceID, SwapIndex, {} };
            SaveManager::Get().SetPersistData(InstanceID, newData);
        } else {
            ExistingData->SwapIndex = SwapIndex;
            SaveManager::Get().SetPersistData(InstanceID, *ExistingData); 
        }

        PendingSwap ps;
        ps.Character = Character;
        ps.SwapIndex = SwapIndex;
        ps.ScheduledTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(DelayMs);
        PendingSwaps.push_back(ps);
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

        SaveManager::Get().LoadWorldData(World);
        PalPersistData* ExistingData = SaveManager::Get().GetPersistData(InstanceID);

        std::wstring CharID = StripCharacterPrefix(RawCharID);

        struct { UObject* Actor; bool RetVal; } WildParams{Character, false};
        if (PalUtil) Utils::CallFunction(PalUtil, STR("IsWildNPC"), &WildParams);
        bool IsWild = WildParams.RetVal;

        struct { bool ReturnValue; } RareParams{false};
        Utils::CallFunction(IndivParam, STR("IsRarePal"), &RareParams);
        bool IsRare = RareParams.ReturnValue;

        struct { uint8_t RetVal; } GenderParams{0};
        Utils::CallFunction(IndivParam, STR("GetGenderType"), &GenderParams);
        std::wstring GenderStr = (GenderParams.RetVal == 1) ? L"Male" : (GenderParams.RetVal == 2) ? L"Female" : L"None";

        struct { int32_t RetVal; } LevelParams{1};
        Utils::CallFunction(IndivParam, STR("GetLevel"), &LevelParams);
        int LevelNum = LevelParams.RetVal;

        struct { int32_t RetVal; } RankParams{0};
        Utils::CallFunction(IndivParam, STR("GetRank"), &RankParams);
        int RankNum = RankParams.RetVal;

        struct { int32_t RetVal; } FriendshipParams{0};
        Utils::CallFunction(IndivParam, STR("GetFriendshipPoint"), &FriendshipParams);
        int FriendshipNum = FriendshipParams.RetVal;

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
        bool bBucketChanged = false;

        if (CachedStats.Level != -1) {
            auto oldEvaluations = ConfigManager::Get().EvaluateAllSwaps(CharID, IsRare, GenderStr, Traits, CachedStats.Level, SkinName, CachedStats.Rank, CachedStats.Friendship, IsWild);
            auto newEvaluations = ConfigManager::Get().EvaluateAllSwaps(CharID, IsRare, GenderStr, Traits, LevelNum, SkinName, RankNum, FriendshipNum, IsWild);

            std::vector<int> oldValidSkins;
            std::vector<int> newValidSkins;

            for (const auto& ev : oldEvaluations) { if (ev.IsValid) oldValidSkins.push_back(ev.ConfigIndex); }
            for (const auto& ev : newEvaluations) { if (ev.IsValid) newValidSkins.push_back(ev.ConfigIndex); }

            if (oldValidSkins != newValidSkins) {
                bBucketChanged = true;
            }
        } else {
            bBucketChanged = true;
        }

        CachedStats.Level = LevelNum;
        CachedStats.Rank = RankNum;
        CachedStats.Friendship = FriendshipNum;

        int finalSwap = -1;

        // NEW: Strict manual override logic
        if (ExplicitSwapIndex != -1) {
            finalSwap = ExplicitSwapIndex;
        } 
        else {
            if (!ForceReroll && !bBucketChanged && SwappedInstances.find(InstanceID) != SwappedInstances.end()) {
                return; 
            }

            auto evaluations = ConfigManager::Get().EvaluateAllSwaps(CharID, IsRare, GenderStr, Traits, LevelNum, SkinName, RankNum, FriendshipNum, IsWild);
            int newBestSwap = ConfigManager::Get().PickBestSwap(evaluations);
            int currentSwap = ExistingData ? ExistingData->SwapIndex : -1;
            
            finalSwap = currentSwap;

            if (ForceReroll) {
                finalSwap = newBestSwap;
            } else if (currentSwap >= 0 && currentSwap < (int)evaluations.size()) {
                auto& currentEval = evaluations[currentSwap];
                if (bBucketChanged) {
                    int absoluteBestScore = 999999;
                    for (const auto& ev : evaluations) {
                        if (ev.IsValid && ev.Score < absoluteBestScore) {
                            absoluteBestScore = ev.Score;
                        }
                    }

                    if (!currentEval.IsValid || currentEval.Score > absoluteBestScore) {
                        DP_LOG(Normal, "Bucket shifted! Upgrading Pal skin to newly unlocked tier.\n");
                        finalSwap = newBestSwap;
                    }
                }
            } else {
                finalSwap = newBestSwap;
            }
        }

        if (finalSwap != -1) {
            bool bNeedsApply = (ExplicitSwapIndex != -1) || ForceReroll || (finalSwap != (ExistingData ? ExistingData->SwapIndex : -1)) || (SwappedInstances.find(InstanceID) == SwappedInstances.end());
            
            if (bNeedsApply) {
                PalPersistData newData = ExistingData ? *ExistingData : PalPersistData{ InstanceID, finalSwap, {} };
                newData.SwapIndex = finalSwap;
                SaveManager::Get().SetPersistData(InstanceID, newData);

                ApplySwap(Character, ConfigManager::Get().GetConfigs()[finalSwap], newData);
                SwappedInstances.insert(InstanceID);
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

        struct { bool bNewDisablePostProcessBlueprint; } DisablePP{ true };
        Utils::CallFunction(MeshComp, STR("SetDisablePostProcessBlueprint"), &DisablePP);

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

                struct { UObject* InMesh; bool bReinitPose; } MeshParams{NewMesh, true};
                Utils::CallFunction(MeshComp, STR("SetSkinnedAssetAndUpdate"), &MeshParams);
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

        for (auto& mat : swap.MatReplaceList) {
            UObject* NewMat = Utils::LoadAssetSafely(mat.matPath);
            
            if (!IsPalBlueprintValid(Character, BPName)) return;
            Utils::CallFunction(Character, STR("GetMainMesh"), &MeshComp);
            if (!MeshComp) return;

            if (NewMat) {
                int idx = std::stoi(mat.index);
                struct { int32_t ElementIndex; UObject* Material; } MatParams{idx, NewMat};
                Utils::CallFunction(MeshComp, STR("SetMaterial"), &MatParams);
            }
        }

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

        struct { bool bNewDisablePostProcessBlueprint; } EnablePP{ false };
        Utils::CallFunction(MeshComp, STR("SetDisablePostProcessBlueprint"), &EnablePP);

        struct { bool bPause; } UnpauseAnim{ false };
        Utils::CallFunction(MeshComp, STR("SetPauseAnims"), &UnpauseAnim);

        DP_LOG(Normal, "Successfully applied swap and morph targets to Pal!\n");
    }
}