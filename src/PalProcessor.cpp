#include "PalProcessor.hpp"
#include "ConfigManager.hpp"
#include "SaveManager.hpp"
#include "Utils.hpp"
#include <DynamicOutput/DynamicOutput.hpp>
#include <random>

#include <Unreal/UObject.hpp>
#include <Unreal/FString.hpp>
#include <Unreal/Core/Containers/Array.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace DynPals {

    std::wstring PalProcessor::StripCharacterPrefix(const std::wstring& InputID) {
        if (InputID.rfind(L"BOSS_", 0) == 0) return InputID.substr(5);
        if (InputID.rfind(L"RAID_", 0) == 0) return InputID.substr(5);
        if (InputID.rfind(L"GYM_", 0) == 0) return InputID.substr(4);
        return InputID;
    }

    void PalProcessor::ProcessPal(UObject* Character, bool ForceReroll) {
        if (!Character) return;

        UObject* ParamComp = nullptr;
        Utils::GetPropertyValue<UObject*>(Character, STR("CharacterParameterComponent"), ParamComp);
        if (!ParamComp) return;

        UObject* IndivParam = nullptr;
        Utils::GetPropertyValue<UObject*>(ParamComp, STR("IndividualParameter"), IndivParam);
        if (!IndivParam) return;

        UObject* Level = Character->GetOuterPrivate();
        if (Level) SaveManager::Get().LoadWorldData(Level->GetOuterPrivate());

        struct FPalInstanceID { DynPalsGuid PlayerUId; DynPalsGuid InstanceId; } InstanceIDStruct;
        if (!Utils::GetPropertyValue<FPalInstanceID>(IndivParam, STR("IndividualId"), InstanceIDStruct)) return;
        
        std::wstring InstanceID = Utils::GuidToWString(InstanceIDStruct.InstanceId);
        
        // Wait until Pal is fully initialized with a valid InstanceID before tracking/processing it.
        if (InstanceID == L"00000000000000000000000000000000") return;

        // Register the Pal as initialized so the scanner leaves it alone
        ProcessedPals.insert(Character);

        PalPersistData* ExistingData = SaveManager::Get().GetPersistData(InstanceID);
        int SwapIndex = -1;

        if (ExistingData && !ForceReroll) {
            SwapIndex = ExistingData->SwapIndex;
        } else {
            UObject* PalUtil = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
            
            struct { UObject* Char; FName RetVal; } CharIDParams{Character, FName()};
            if (PalUtil) Utils::CallFunction(PalUtil, STR("GetCharacterIDFromCharacter"), &CharIDParams);
            std::wstring CharID = StripCharacterPrefix(CharIDParams.RetVal.ToString());

            // NEW: Fetch Wild status via UPalUtility
            struct { UObject* Actor; bool RetVal; } WildParams{Character, false};
            if (PalUtil) Utils::CallFunction(PalUtil, STR("IsWildNPC"), &WildParams);
            bool IsWild = WildParams.RetVal;

            struct { bool RetVal; } RareParams{false};
            Utils::CallFunction(IndivParam, STR("IsRarePal"), &RareParams);
            bool IsRare = RareParams.RetVal;

            struct { uint8_t RetVal; } GenderParams{0};
            Utils::CallFunction(IndivParam, STR("GetGenderType"), &GenderParams);
            std::wstring GenderStr = (GenderParams.RetVal == 1) ? L"Male" : (GenderParams.RetVal == 2) ? L"Female" : L"None";

            struct { int32_t RetVal; } LevelParams{1};
            Utils::CallFunction(IndivParam, STR("GetLevel"), &LevelParams);
            int LevelNum = LevelParams.RetVal;

            // Fetch condensed star rank (0 to 4)
            struct { int32_t RetVal; } RankParams{0};
            Utils::CallFunction(IndivParam, STR("GetRank"), &RankParams);
            int RankNum = RankParams.RetVal;

            // Fetch trust/friendship points
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

            // Execute Matchmaker Pipeline
            auto evaluations = ConfigManager::Get().EvaluateAllSwaps(CharID, IsRare, GenderStr, Traits, LevelNum, SkinName, RankNum, FriendshipNum, IsWild);
            SwapIndex = ConfigManager::Get().PickBestSwap(evaluations);
            if (SwapIndex != -1) {
                PalPersistData newData = { InstanceID, SwapIndex, {} };
                SaveManager::Get().SetPersistData(InstanceID, newData);
            }
        }

        if (SwapIndex >= 0 && SwapIndex < (int)ConfigManager::Get().GetConfigs().size()) {
            ApplySwap(Character, ConfigManager::Get().GetConfigs()[SwapIndex], *SaveManager::Get().GetPersistData(InstanceID));
        }
    }

    void PalProcessor::ApplySwap(UObject* Character, const SwapConfig& swap, PalPersistData& persist) {
        UObject* MeshComp = nullptr;
        Utils::CallFunction(Character, STR("GetMainMesh"), &MeshComp);
        if (!MeshComp) return;

        UObject* NewMesh = nullptr;
        if (!swap.SkelMeshPath.empty()) {
            NewMesh = Utils::LoadAssetSafely(swap.SkelMeshPath);
            if (NewMesh) {
                struct { UObject* InMesh; bool bReinitPose; } MeshParams{NewMesh, true};
                Utils::CallFunction(MeshComp, STR("SetSkinnedAssetAndUpdate"), &MeshParams);
            }
        }
// NEW: Animation Bridging System with Deep Diagnostic Logging
if (!swap.AnimTarget.empty()) {
    std::wstring AnimPath = swap.AnimTarget;
    
    // Quality of Life: Auto-format simple IDs (like "WeaselDragon") into full Blueprint paths
    if (AnimPath.find(L'/') == std::wstring::npos) {
        AnimPath = L"/Game/Pal/Blueprint/Character/Monster/PalActorBP/" + AnimPath + L"/BP_" + AnimPath + L".BP_" + AnimPath + L"_C";
    }

    Output::send<LogLevel::Normal>(STR("[DynPals] Animation Bridge: Initiating swap to target: {}\n"), AnimPath.c_str());

    // Split the package path and class name to construct the Class Default Object (CDO) path
    size_t dotPos = AnimPath.find(L'.');
    if (dotPos != std::wstring::npos) {
        std::wstring PackagePath = AnimPath.substr(0, dotPos);
        std::wstring ClassName = AnimPath.substr(dotPos + 1);
        
        // Construct the CDO path: e.g. "/Game/.../BP_WeaselDragon.Default__BP_WeaselDragon_C"
        std::wstring CDOPath = PackagePath + L".Default__" + ClassName;
        Output::send<LogLevel::Normal>(STR("[DynPals] Animation Bridge: Calculated CDO Path: {}\n"), CDOPath.c_str());

        // Load both the Class and the CDO safely as standard asset paths
        UClass* TargetBPClass = static_cast<UClass*>(Utils::LoadAssetSafely(AnimPath));
        UObject* TargetCDO = Utils::LoadAssetSafely(CDOPath);
        
        if (!TargetBPClass) {
            Output::send<LogLevel::Error>(STR("[DynPals] Animation Bridge Error: Failed to load target Blueprint Class at: {}\n"), AnimPath.c_str());
        }
        if (!TargetCDO) {
            Output::send<LogLevel::Error>(STR("[DynPals] Animation Bridge Error: Failed to load target CDO at: {}\n"), CDOPath.c_str());
        }

        if (TargetBPClass && TargetCDO) {
            Output::send<LogLevel::Normal>(STR("[DynPals] Animation Bridge: Successfully loaded Class and CDO. Processing subcomponents...\n"));
            
            // 1. Swap the Base AnimBlueprint on the Skeletal Mesh
            UObject* TargetMesh = nullptr;
Utils::GetPropertyValue<UObject*>(TargetCDO, STR("Mesh"), TargetMesh);
if (TargetMesh) {
    UClass* TargetAnimClass = nullptr;
    Utils::GetPropertyValue<UClass*>(TargetMesh, STR("AnimClass"), TargetAnimClass);
    
    if (TargetAnimClass) {
        std::wstring AnimClassName = TargetAnimClass->GetName();
        Output::send<LogLevel::Normal>(STR("[DynPals] Animation Bridge: Found Target AnimClass: {}\n"), AnimClassName.c_str());
        
        // FIX: Changed "SetAnimInstanceClass" to "SetAnimClass" for UE5 compatibility
        UFunction* SetAnimFunc = MeshComp->GetFunctionByNameInChain(STR("SetAnimClass"));
        if (SetAnimFunc) {
            struct { UClass* NewClass; } Params{ TargetAnimClass };
            MeshComp->ProcessEvent(SetAnimFunc, &Params);
            Output::send<LogLevel::Normal>(STR("[DynPals] Animation Bridge: Successfully called SetAnimClass on MeshComp.\n"));
        } else {
            Output::send<LogLevel::Error>(STR("[DynPals] Animation Bridge Error: MeshComp does not have 'SetAnimClass' function!\n"));
        }
    } else {
        Output::send<LogLevel::Warning>(STR("[DynPals] Animation Bridge Warning: Target mesh does not have an AnimClass defined.\n"));
    }
} else {
    Output::send<LogLevel::Error>(STR("[DynPals] Animation Bridge Error: Failed to find 'Mesh' property on Target CDO.\n"));

            }

            // 2. Clone the Animation Montages from the Target's Static Parameter Component
            UObject* TargetStaticParam = nullptr;
            Utils::GetPropertyValue<UObject*>(TargetCDO, STR("StaticCharacterParameterComponent"), TargetStaticParam);

            UObject* CurrentStaticParam = nullptr;
            Utils::GetPropertyValue<UObject*>(Character, STR("StaticCharacterParameterComponent"), CurrentStaticParam);

            if (!TargetStaticParam) {
                Output::send<LogLevel::Error>(STR("[DynPals] Animation Bridge Error: Failed to find StaticCharacterParameterComponent on Target CDO.\n"));
            }
            if (!CurrentStaticParam) {
                Output::send<LogLevel::Error>(STR("[DynPals] Animation Bridge Error: Failed to find StaticCharacterParameterComponent on Current Character.\n"));
            }

            if (TargetStaticParam && CurrentStaticParam) {
                Output::send<LogLevel::Normal>(STR("[DynPals] Animation Bridge: Found both parameter components. Copying properties...\n"));
                
                // Helper lambda to safely copy memory values between FProperties with strict logging
                auto CopyProp = [](UObject* Src, UObject* Dest, const wchar_t* PropName) {
                    FProperty* SrcProp = Utils::GetProperty(Src, PropName);
                    FProperty* DestProp = Utils::GetProperty(Dest, PropName);
                    if (!SrcProp) {
                        Output::send<LogLevel::Warning>(STR("[DynPals] Animation Bridge: Property '{}' missing on SOURCE.\n"), PropName);
                    }
                    if (!DestProp) {
                        Output::send<LogLevel::Warning>(STR("[DynPals] Animation Bridge: Property '{}' missing on DESTINATION.\n"), PropName);
                    }
                    
                    if (SrcProp && DestProp) {
                        void* SrcPtr = SrcProp->ContainerPtrToValuePtr<void>(Src);
                        void* DestPtr = DestProp->ContainerPtrToValuePtr<void>(Dest);
                        if (SrcPtr && DestPtr) {
                            DestProp->CopyCompleteValue(DestPtr, SrcPtr);
                            Output::send<LogLevel::Normal>(STR("[DynPals] Animation Bridge: Copied '{}' successfully.\n"), PropName);
                            return;
                        }
                    }
                };

                // Copy all fundamental Animation Maps/Arrays
                CopyProp(TargetStaticParam, CurrentStaticParam, STR("RandomRestMontageInfos"));
                CopyProp(TargetStaticParam, CurrentStaticParam, STR("GeneralAnimSequenceMap"));
                CopyProp(TargetStaticParam, CurrentStaticParam, STR("GeneralMontageMap"));
                CopyProp(TargetStaticParam, CurrentStaticParam, STR("GeneralBlendSpaceMap"));
                CopyProp(TargetStaticParam, CurrentStaticParam, STR("ActionMontageMap"));
                CopyProp(TargetStaticParam, CurrentStaticParam, STR("SleepOnSideAnimMontage"));
            }
        }
    } else {
        Output::send<LogLevel::Error>(STR("[DynPals] Animation Bridge Error: Malformed AnimTarget path (No dot found): {}\n"), AnimPath.c_str());
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
            if (NewMat) {
                int idx = std::stoi(mat.index);
                struct { int32_t ElementIndex; UObject* Material; } MatParams{idx, NewMat};
                Utils::CallFunction(MeshComp, STR("SetMaterial"), &MatParams);
            }
        }

        if (!swap.MorphTargetList.empty()) {
            std::random_device rd;
            std::mt19937 gen(rd());

            for (auto& morph : swap.MorphTargetList) {
                double val = 0.0;
                auto it = persist.MorphSet.find(morph.target);
                
                bool hasValidSavedVal = false;
                double savedVal = -1000.0;
                
                if (it != persist.MorphSet.end()) {
                    savedVal = it->second;
                    if (savedVal >= -900.0) {
                        hasValidSavedVal = true;
                    }
                }

                if (morph.setVal != -1000.0) {
                    val = morph.setVal;
                } 
                else if (hasValidSavedVal) {
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
                } 
                else {
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
    }

    void PalProcessor::ForceSwap(UObject* Character, int SwapIndex) {
        if (!Character || SwapIndex < 0 || SwapIndex >= (int)ConfigManager::Get().GetConfigs().size()) return;

        ProcessedPals.insert(Character);

        UObject* ParamComp = nullptr;
        Utils::GetPropertyValue<UObject*>(Character, STR("CharacterParameterComponent"), ParamComp);
        if (!ParamComp) return;

        UObject* IndivParam = nullptr;
        Utils::GetPropertyValue<UObject*>(ParamComp, STR("IndividualParameter"), IndivParam);
        if (!IndivParam) return;

        struct FPalInstanceID { DynPalsGuid PlayerUId; DynPalsGuid InstanceId; } InstanceIDStruct;
        if (!Utils::GetPropertyValue<FPalInstanceID>(IndivParam, STR("IndividualId"), InstanceIDStruct)) return;
        
        std::wstring InstanceID = Utils::GuidToWString(InstanceIDStruct.InstanceId);

        PalPersistData* ExistingData = SaveManager::Get().GetPersistData(InstanceID);
        if (!ExistingData) {
            PalPersistData newData = { InstanceID, SwapIndex, {} };
            SaveManager::Get().SetPersistData(InstanceID, newData);
            ExistingData = SaveManager::Get().GetPersistData(InstanceID);
        } else {
            ExistingData->SwapIndex = SwapIndex;
            SaveManager::Get().SetPersistData(InstanceID, *ExistingData); 
        }

        ApplySwap(Character, ConfigManager::Get().GetConfigs()[SwapIndex], *ExistingData);
    }

    void PalProcessor::ScanActivePals() {
        auto now = std::chrono::steady_clock::now();
        if (now - LastScanTime < std::chrono::seconds(3)) return;
        LastScanTime = now;

        std::vector<UObject*> AllPals;
        UObjectGlobals::FindAllOf(STR("PalCharacter"), AllPals);

        std::set<UObject*> CurrentActivePals(AllPals.begin(), AllPals.end());

        for (UObject* Pal : AllPals) {
            if (!Pal) continue; 
            
            if (ProcessedPals.find(Pal) == ProcessedPals.end()) {
                ProcessPal(Pal, false);
            }
        }

        for (auto it = ProcessedPals.begin(); it != ProcessedPals.end(); ) {
            if (CurrentActivePals.find(*it) == CurrentActivePals.end()) {
                it = ProcessedPals.erase(it); 
            } else {
                ++it;
            }
        }
    }
}