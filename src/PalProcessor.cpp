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

    // Validates the Blueprint and checks if the actor is safely spawned
    static bool IsPalBlueprintValid(UObject* Pal, std::wstring& OutBlueprintName) {
        if (!Pal) return false;

        UClass* PalClass = Pal->GetClassPrivate();
        if (!PalClass) {
            DP_LOG(Warning, "WARNING: Spawned Actor has NO VALID BLUEPRINT CLASS! Aborting.");
            return false;
        }
        
        OutBlueprintName = PalClass->GetName();
        if (OutBlueprintName.empty() || OutBlueprintName.find(L"Default__") != std::wstring::npos) return false;

        // DUAL-LAYERED GYM PROTECTION: Ignore composite Gym Leader blueprints
        if (OutBlueprintName.find(L"_Gym") != std::wstring::npos) {
            return false;
        }

        // 1. Guard against actors currently undergoing engine destruction
        bool bBeingDestroyed = false;
        if (Utils::GetPropertyValue<bool>(Pal, STR("bActorIsBeingDestroyed"), bBeingDestroyed) && bBeingDestroyed) {
            DP_LOG(Warning, "WARNING: Blueprint '{}' is pending kill / being destroyed! Aborting.", OutBlueprintName);
            return false;
        }

        // 2. Guard against half-spawned actors that haven't finished BeginPlay
        bool bBegunPlay = false;
        if (Utils::GetPropertyValue<bool>(Pal, STR("bHasBegunPlay"), bBegunPlay)) {
            if (!bBegunPlay) return false;
        }

        // 3. Active overworld Pal check
        bool bIsActive = true;
        if (Utils::GetPropertyValue<bool>(Pal, STR("bIsPalActiveActor"), bIsActive)) {
            // Disabled cuz prevents pals in your inventory from being replaced
            //if (!bIsActive) return false;
        }

        UObject* MeshComp = nullptr;
        Utils::CallFunction(Pal, STR("GetMainMesh"), &MeshComp);
        if (!MeshComp) {
            DP_LOG(Warning, "WARNING: Blueprint '{}' has no valid Mesh Component! Aborting.", OutBlueprintName);
            return false;
        }

        // 4. Ensure the mesh component is registered with the rendering and physics scene
        bool bRegistered = false;
        if (Utils::GetPropertyValue<bool>(MeshComp, STR("bRegistered"), bRegistered)) {
            if (!bRegistered) return false;
        }

        // 5. Ghost/UI/Box Pals have collision disabled. 
        struct { bool ReturnValue; } ColParams{true};
       Utils::CallFunction(Pal, STR("GetActorEnableCollision"), &ColParams);
       // Disabled cuz prevents pals in your inventory from being replaced
        //if (!ColParams.ReturnValue) return false;

        bool bHidden = false;
        if (Utils::GetPropertyValue<bool>(Pal, STR("bHidden"), bHidden)) {
            // Disabled cuz prevents pals in your inventory from being replaced
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

    void PalProcessor::ProcessPal(UObject* Character, bool ForceReroll) {
        std::wstring BlueprintName = L"";
        if (!IsPalBlueprintValid(Character, BlueprintName)) {
            return;
        }

        UObject* ParamComp = nullptr;
        Utils::GetPropertyValue<UObject*>(Character, STR("CharacterParameterComponent"), ParamComp);
        if (!ParamComp) {
            DP_LOG(Warning, "WARNING: Blueprint '{}' is missing a CharacterParameterComponent! Aborting.", BlueprintName);
            return;
        }

        UObject* IndivParam = nullptr;
        Utils::GetPropertyValue<UObject*>(ParamComp, STR("IndividualParameter"), IndivParam);
        if (!IndivParam) return;

        struct FPalInstanceID { DynPalsGuid PlayerUId; DynPalsGuid InstanceId; } InstanceIDStruct;
        if (!Utils::GetPropertyValue<FPalInstanceID>(IndivParam, STR("IndividualId"), InstanceIDStruct)) return;
        if (!InstanceIDStruct.InstanceId.IsValid()) return; 

        std::wstring InstanceID = Utils::GuidToWString(InstanceIDStruct.InstanceId);

        // Prevent repeating expensive swaps if we've already completed them
        if (SwappedInstances.find(InstanceID) != SwappedInstances.end() && !ForceReroll) {
            return;
        }

        UObject* Level = Character->GetOuterPrivate();
        if (!Level) return;
        UObject* World = Level->GetOuterPrivate();
        if (!World || World->GetClassPrivate()->GetName() != L"World") return;

        static UObject* PalUtil = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
        struct { UObject* Char; FName RetVal; } CharIDParams{Character, FName()};
        if (PalUtil) Utils::CallFunction(PalUtil, STR("GetCharacterIDFromCharacter"), &CharIDParams);
        std::wstring RawCharID = CharIDParams.RetVal.ToString();

        // DUAL-LAYERED GYM PROTECTION: Intercept Gym composite bosses safely before evaluation
        if (RawCharID.rfind(L"GYM_", 0) == 0 || RawCharID.find(L"_Gym_") != std::wstring::npos) {
            DP_LOG(Normal, "[DynPals] Skipping swap for Gym Leader Pal (Raw ID: {}). Swapping composite Gym characters is disabled for safety.\n", RawCharID);
            return;
        }

        DP_LOG(Normal, "Processing valid overworld Blueprint: {}\n", BlueprintName);

        // Load world data
        SaveManager::Get().LoadWorldData(World);

        PalPersistData* ExistingData = SaveManager::Get().GetPersistData(InstanceID);
        int SwapIndex = -1;

        if (ExistingData && !ForceReroll) {
            SwapIndex = ExistingData->SwapIndex;
        } else {
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

            auto evaluations = ConfigManager::Get().EvaluateAllSwaps(CharID, IsRare, GenderStr, Traits, LevelNum, SkinName, RankNum, FriendshipNum, IsWild);
            SwapIndex = ConfigManager::Get().PickBestSwap(evaluations);
            if (SwapIndex != -1) {
                PalPersistData newData = { InstanceID, SwapIndex, {} };
                SaveManager::Get().SetPersistData(InstanceID, newData);
            }
        }

        if (SwapIndex >= 0 && SwapIndex < (int)ConfigManager::Get().GetConfigs().size()) {
            ApplySwap(Character, ConfigManager::Get().GetConfigs()[SwapIndex], *SaveManager::Get().GetPersistData(InstanceID));
            SwappedInstances.insert(InstanceID);
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

        // Fetch CharID first so it is fully declared and safe to use for fallbacks!
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
            // First, try loading the path exactly as provided (e.g. GuardianDog_Override)
            std::wstring TryPath1 = L"/Game/Pal/Blueprint/Character/Monster/PalActorBP/" + AnimPath + L"/BP_" + AnimPath + L".BP_" + AnimPath + L"_C";
            TargetBPClass = static_cast<UClass*>(Utils::LoadAssetSafely(TryPath1));

            if (TargetBPClass) {
                TargetPackagePath = L"/Game/Pal/Blueprint/Character/Monster/PalActorBP/" + AnimPath + L"/BP_" + AnimPath;
                TargetClassName = L"BP_" + AnimPath + L"_C";
            } else {
                // Fallback: Truncate at the underscore if the user provided a variant name (e.g. Boss_Override -> Boss)
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
            // A full explicit path was provided
            TargetBPClass = static_cast<UClass*>(Utils::LoadAssetSafely(AnimPath));
            size_t dotPos = AnimPath.find(L'.');
            if (dotPos != std::wstring::npos) {
                TargetPackagePath = AnimPath.substr(0, dotPos);
                TargetClassName = AnimPath.substr(dotPos + 1);
            }
        }

        // Post-Load Safety Gate 1
        if (!IsPalBlueprintValid(Character, BPName)) return;
        Utils::CallFunction(Character, STR("GetMainMesh"), &MeshComp);
        if (!MeshComp) return;

        // SAFELY GET CDO USING THE NATIVE ENGINE LOADER (Prevents UE5 CDO instantiation crashes!)
        if (TargetBPClass && !TargetPackagePath.empty() && !TargetClassName.empty()) {
            DP_LOG(Normal, "[DEBUG] Successfully loaded Target Blueprint: {}\n", TargetBPClass->GetName());
            
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
            } else {
                DP_LOG(Warning, "WARNING: TargetBPClass loaded, but failed to extract CDO via LoadAssetSafely!\n");
            }
        }

        // --- SMART ANIM SWAP LOGIC ---
        // If we don't have a target, fallback to the natural default of the class
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

        // Absolute fallback if everything fails
        if (!TargetAnimClass) { TargetAnimClass = CurrentAnimClass; }

        // Only tear down and rebuild the animation pipeline if the classes are actually different!
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
            
            // Post-Load Safety Gate 3
            if (!IsPalBlueprintValid(Character, BPName)) return;
            Utils::CallFunction(Character, STR("GetMainMesh"), &MeshComp);
            if (!MeshComp) return;

            if (NewMesh) {
                std::wstring MeshClass = NewMesh->GetClassPrivate()->GetName();
                if (MeshClass != L"SkeletalMesh" && MeshClass != L"SkeletalMeshSR" && MeshClass != L"SkinnedAsset") {
                    DP_LOG(Warning, "WARNING: Loaded asset class '{}' is not a valid SkeletalMesh type! Skipping swap.\n", MeshClass);
                    return;
                }

                if (TargetSkeleton) {
                    struct { UObject* NewSkeleton; } SkelParams{ TargetSkeleton };
                    Utils::CallFunction(NewMesh, STR("SetSkeleton"), &SkelParams);
                }

                // ALWAYS set bReinitPose to true, so new modded skeletons map correctly!
                struct { UObject* InMesh; bool bReinitPose; } MeshParams{NewMesh, true};
                DP_LOG(Normal, "\nGonna try to swap skeleton: {}\n", swap.SkelMeshPath);
                Utils::CallFunction(MeshComp, STR("SetSkinnedAssetAndUpdate"), &MeshParams);
            } else {
                DP_LOG(Warning, "WARNING: Failed to load Skeletal Mesh asset at path: '{}'\n", swap.SkelMeshPath);
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
            
            // Post-Load Safety Gate 4
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

        // ALWAYS re-link physics layers regardless of bNeedsAnimRebuild, 
        // to ensure the new custom mod bones properly receive physics calculations!
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
                    
                    // Post-Load Safety Gate 5
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

    void PalProcessor::ForceSwap(UObject* Character, int SwapIndex) {
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

        ProcessPal(Character, false);
    }
}