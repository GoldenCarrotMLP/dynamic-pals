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

    // Safely validates if a character UObject pointer is still spawned in memory
    static bool IsCharacterValid(UObject* Character) {
        if (!Character) return false;
        std::vector<UObject*> AllPals;
        UObjectGlobals::FindAllOf(STR("PalCharacter"), AllPals);
        for (UObject* Pal : AllPals) {
            if (Pal == Character) return true;
        }
        return false;
    }

    // GATE: Determines if a Pal is actively walking around in the world.
    static bool IsPalVisibleAndActive(UObject* Pal) {
        if (!Pal) return false;

        // 1. CDO CHECK: NEVER process Class Default Objects (the templates)!
        std::wstring PalName = Pal->GetName();
        if (PalName.find(L"Default__") != std::wstring::npos) return false;

        // 2. ORIGIN POOL CHECK: Skip background ghosts stored near exact 0,0,0
        struct { FVector_UE5 ReturnValue; } LocParams;
        Utils::CallFunction(Pal, STR("K2_GetActorLocation"), &LocParams);
        double distSq = (LocParams.ReturnValue.X * LocParams.ReturnValue.X) + 
                        (LocParams.ReturnValue.Y * LocParams.ReturnValue.Y) + 
                        (LocParams.ReturnValue.Z * LocParams.ReturnValue.Z);
        if (distSq < 100.0) return false;

        // 3. PALWORLD ACTIVE FLAG
        bool bIsActive = true;
        if (Utils::GetPropertyValue<bool>(Pal, STR("bIsPalActiveActor"), bIsActive)) {
            if (!bIsActive) return false;
        }

        // 4. COLLISION CHECK (Palbox / Unsummoned pals have collision disabled)
        struct { bool ReturnValue; } ColParams{true};
        Utils::CallFunction(Pal, STR("GetActorEnableCollision"), &ColParams);
        if (!ColParams.ReturnValue) return false;

        // 5. HIDDEN POOL CHECK
        bool bHidden = false;
        if (Utils::GetPropertyValue<bool>(Pal, STR("bHidden"), bHidden)) {
            if (bHidden) return false;
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
        if (!Character) return;
        if (Character->GetName().find(L"Default__") != std::wstring::npos) return;

        // If we force a reroll (e.g. from UI), erase them from memory so they can be processed anew
        if (ForceReroll) {
            ProcessedPals.erase(Character);
        }

        // Don't re-add if already processed or already in the pipeline
        if (ProcessedPals.find(Character) != ProcessedPals.end()) return;
        for (auto& q : ProcessingQueue) {
            if (q.Character == Character) {
                if (ForceReroll) q.ForceReroll = true;
                return;
            }
        }

        // Drop them into State 0: The waiting room.
        QueuedPal qp;
        qp.Character = Character;
        qp.ForceReroll = ForceReroll;
        qp.State = 0; 
        ProcessingQueue.push_back(qp);
    }

    void PalProcessor::ExecuteSwap(UObject* Character, bool ForceReroll) {
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
        
        if (InstanceID == L"00000000000000000000000000000000") return;

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
        }
    }

    void PalProcessor::ApplySwap(UObject* Character, const SwapConfig& swap, PalPersistData& persist) {
        UObject* MeshComp = nullptr;
        Utils::CallFunction(Character, STR("GetMainMesh"), &MeshComp);
        if (!MeshComp) return;

        // 1. SAFELY PAUSE ANIMATIONS TO PREVENT EVALUATION CRASHES DURING SWAP
        struct { bool bPause; } PauseAnim{ true };
        Utils::CallFunction(MeshComp, STR("SetPauseAnims"), &PauseAnim);

        struct { bool bNewDisablePostProcessBlueprint; } DisablePP{ true };
        Utils::CallFunction(MeshComp, STR("SetDisablePostProcessBlueprint"), &DisablePP);

        // 2. RETRIEVE ORIGINAL CHARACTER ID
        UObject* PalUtil = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
        struct { UObject* Char; FName RetVal; } CharIDParams{Character, FName()};
        if (PalUtil) Utils::CallFunction(PalUtil, STR("GetCharacterIDFromCharacter"), &CharIDParams);
        std::wstring CharID = StripCharacterPrefix(CharIDParams.RetVal.ToString());

        // Resolve AnimTargetName: fallback to the original Pal ID if no custom target is defined!
        std::wstring AnimTargetName = swap.AnimTarget;
        if (AnimTargetName.empty()) {
            AnimTargetName = CharID;
        }

        UClass* TargetAnimClass = nullptr;
        UObject* TargetSkeleton = nullptr;
        UObject* TargetStaticParam = nullptr;

        // 3. RESOLVE TARGET BLUEPRINT, SKELETON AND ANIM CLASS UNIFORMLY
        std::wstring AnimPath = AnimTargetName;
        if (AnimPath.find(L'/') == std::wstring::npos) {
            AnimPath = L"/Game/Pal/Blueprint/Character/Monster/PalActorBP/" + AnimPath + L"/BP_" + AnimPath + L".BP_" + AnimPath + L"_C";
        }

        size_t dotPos = AnimPath.find(L'.');
        if (dotPos != std::wstring::npos) {
            std::wstring PackagePath = AnimPath.substr(0, dotPos);
            std::wstring ClassName = AnimPath.substr(dotPos + 1);
            std::wstring CDOPath = PackagePath + L".Default__" + ClassName;

            UClass* TargetBPClass = static_cast<UClass*>(Utils::LoadAssetSafely(AnimPath));
            if (!TargetBPClass) {
                Output::send<LogLevel::Error>(STR("[DynPals] [Loader] FAILED to load BP Class at path: {}\n"), AnimPath);
            }

            UObject* TargetCDO = Utils::LoadAssetSafely(CDOPath);
            if (!TargetCDO) {
                // Fallback search inside the Package
                if (TargetBPClass) {
                    std::wstring CDOName = L"Default__" + ClassName;
                    TargetCDO = UObjectGlobals::StaticFindObject<UObject*>(nullptr, TargetBPClass->GetOuterPrivate(), CDOName.c_str());
                    if (!TargetCDO) {
                        Output::send<LogLevel::Error>(STR("[DynPals] [Loader] FAILED to find TargetCDO inside package.\n"));
                    }
                }
            }

            if (TargetBPClass && TargetCDO) {
                UObject* TargetMesh = nullptr;
                Utils::GetPropertyValue<UObject*>(TargetCDO, STR("Mesh"), TargetMesh);
                
                if (TargetMesh) {
                    // Extract target's AnimClass
                    Utils::GetPropertyValue<UClass*>(TargetMesh, STR("AnimClass"), TargetAnimClass);
                    if (TargetAnimClass) {
                        // Extract target's Skeleton asset from the AnimClass (Primary check)
                        Utils::GetPropertyValue<UObject*>(TargetAnimClass, STR("TargetSkeleton"), TargetSkeleton);
                    }

                    // Fallback: Extract from Skeletal Mesh of CDO if AnimClass didn't have it
                    UObject* TargetSkelMesh = nullptr;
                    Utils::GetPropertyValue<UObject*>(TargetMesh, STR("SkeletalMesh"), TargetSkelMesh);
                    if (!TargetSkelMesh) {
                        Utils::GetPropertyValue<UObject*>(TargetMesh, STR("SkinnedAsset"), TargetSkelMesh);
                    }

                    if (TargetSkelMesh) {
                        if (!TargetSkeleton) {
                            Utils::GetPropertyValue<UObject*>(TargetSkelMesh, STR("Skeleton"), TargetSkeleton);
                        }
                    }
                }
                Utils::GetPropertyValue<UObject*>(TargetCDO, STR("StaticCharacterParameterComponent"), TargetStaticParam);
            }
        }

        // 4. CLEAR ACTIVE ANIM CLASS (Avoids interim Skeleton mismatches!)
        UFunction* SetAnimFunc = MeshComp->GetFunctionByNameInChain(STR("SetAnimInstanceClass"));
        if (!SetAnimFunc) SetAnimFunc = MeshComp->GetFunctionByNameInChain(STR("SetAnimClass"));

        if (SetAnimFunc) {
            struct { UClass* NewClass; } ClearParams{ nullptr };
            MeshComp->ProcessEvent(SetAnimFunc, &ClearParams);
        }

        // 5. LOAD, RE-TARGET AND APPLY NEW SKELETAL MESH
        UObject* NewMesh = nullptr;
        if (!swap.SkelMeshPath.empty()) {
            NewMesh = Utils::LoadAssetSafely(swap.SkelMeshPath);
            if (NewMesh) {
                // Re-target the mesh to our resolved skeleton (custom target or original native skeleton)
                if (TargetSkeleton) {
                    struct { UObject* NewSkeleton; } SkelParams{ TargetSkeleton };
                    Utils::CallFunction(NewMesh, STR("SetSkeleton"), &SkelParams);
                }

                struct { UObject* InMesh; bool bReinitPose; } MeshParams{NewMesh, true};
                Utils::CallFunction(MeshComp, STR("SetSkinnedAssetAndUpdate"), &MeshParams);
            } else {
                Output::send<LogLevel::Error>(STR("[DynPals] [ApplySwap] FAILED to load Skeletal Mesh: {}\n"), swap.SkelMeshPath);
            }
        }

        // 6. APPLY RESOLVED ANIMATION BLUEPRINT
        if (TargetAnimClass && SetAnimFunc) {
            struct { UClass* NewClass; } Params{ TargetAnimClass };
            MeshComp->ProcessEvent(SetAnimFunc, &Params);
        }

        // 7. COPY MONTAGE PARAMETERS
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

        // 8. RE-LINK THE DEFAULT PALWORLD ANIM LAYERS
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
                    if (LayerClass) {
                        struct { UClass* InClass; } LinkParams{ LayerClass };
                        NewAnimInst->ProcessEvent(LinkFunc, &LinkParams);
                    }
                }
            }
        }

        // 9. UNPAUSE ANIMATIONS SO THE NEW GRAPH CAN EVALUATE
        struct { bool bNewDisablePostProcessBlueprint; } EnablePP{ false };
        Utils::CallFunction(MeshComp, STR("SetDisablePostProcessBlueprint"), &EnablePP);

        struct { bool bPause; } UnpauseAnim{ false };
        Utils::CallFunction(MeshComp, STR("SetPauseAnims"), &UnpauseAnim);
    }

    void PalProcessor::ForceSwap(UObject* Character, int SwapIndex, int DelayMs) {
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
        } else {
            ExistingData->SwapIndex = SwapIndex;
            SaveManager::Get().SetPersistData(InstanceID, *ExistingData); 
        }

        PendingSwap ps;
        ps.Character = Character;
        ps.SwapIndex = SwapIndex;
        ps.ScheduledTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(DelayMs);

        for (auto It = PendingSwaps.begin(); It != PendingSwaps.end(); ) {
            if (It->Character == Character) {
                It = PendingSwaps.erase(It);
            } else {
                ++It;
            }
        }

        PendingSwaps.push_back(ps);
    }

    void PalProcessor::TickDeferredSwaps() {
        auto Now = std::chrono::steady_clock::now();
        
        // 1. UI Manual Overrides (Immediate injection)
        if (!PendingSwaps.empty()) {
            std::vector<UObject*> AllPals;
            UObjectGlobals::FindAllOf(STR("PalCharacter"), AllPals);
            std::set<UObject*> ValidPals(AllPals.begin(), AllPals.end());

            for (auto It = PendingSwaps.begin(); It != PendingSwaps.end(); ) {
                if (Now >= It->ScheduledTime) {
                    if (It->Character && ValidPals.find(It->Character) != ValidPals.end()) {
                        UObject* ParamComp = nullptr;
                        Utils::GetPropertyValue<UObject*>(It->Character, STR("CharacterParameterComponent"), ParamComp);
                        if (ParamComp) {
                            UObject* IndivParam = nullptr;
                            Utils::GetPropertyValue<UObject*>(ParamComp, STR("IndividualParameter"), IndivParam);
                            if (IndivParam) {
                                struct FPalInstanceID { DynPalsGuid PlayerUId; DynPalsGuid InstanceId; } InstanceIDStruct;
                                if (Utils::GetPropertyValue<FPalInstanceID>(IndivParam, STR("IndividualId"), InstanceIDStruct)) {
                                    std::wstring InstanceID = Utils::GuidToWString(InstanceIDStruct.InstanceId);
                                    PalPersistData* ExistingData = SaveManager::Get().GetPersistData(InstanceID);
                                    if (ExistingData) {
                                        ApplySwap(It->Character, ConfigManager::Get().GetConfigs()[It->SwapIndex], *ExistingData);
                                    }
                                }
                            }
                        }
                    }
                    It = PendingSwaps.erase(It);
                } else {
                    ++It;
                }
            }
        }

        // 2. Background Pipeline (Throttled to 500ms + Cached single-pass lookup)
        static auto LastPipelineTick = Now;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(Now - LastPipelineTick).count() >= 500) {
            LastPipelineTick = Now;

            if (!ProcessingQueue.empty()) {
                std::vector<UObject*> AllPals;
                UObjectGlobals::FindAllOf(STR("PalCharacter"), AllPals);
                std::set<UObject*> ValidPals(AllPals.begin(), AllPals.end());

                for (auto It = ProcessingQueue.begin(); It != ProcessingQueue.end(); ) {
                    // Safety check: Erase dead pointers
                    if (ValidPals.find(It->Character) == ValidPals.end()) {
                        It = ProcessingQueue.erase(It);
                        continue;
                    }

                    if (It->State == 0) {
                        // STATE 0: Waiting in 0,0,0 Pool
                        if (IsPalVisibleAndActive(It->Character)) {
                            It->State = 1;
                            It->AssemblyEndTime = Now + std::chrono::milliseconds(1000);
                        }
                        ++It;
                    } 
                    else if (It->State == 1) {
                        // STATE 1: Assembling Weapons
                        if (Now >= It->AssemblyEndTime) {
                            if (IsPalVisibleAndActive(It->Character)) {
                                ExecuteSwap(It->Character, It->ForceReroll);
                                It = ProcessingQueue.erase(It); // Done!
                            } else {
                                It->State = 0;
                                ++It;
                            }
                        } else {
                            ++It;
                        }
                    }
                }
            }
        }
    }

    void PalProcessor::ScanActivePals() {
        auto now = std::chrono::steady_clock::now();
        if (now - LastScanTime < std::chrono::seconds(3)) return;
        LastScanTime = now;

        std::vector<UObject*> AllPals;
        UObjectGlobals::FindAllOf(STR("PalCharacter"), AllPals);

        std::set<UObject*> ValidPointers;
        std::set<UObject*> ActivePals;

        for (UObject* Pal : AllPals) {
            if (!Pal || Pal->GetName().find(L"Default__") != std::wstring::npos) continue;
            ValidPointers.insert(Pal);
            
            if (IsPalVisibleAndActive(Pal)) {
                ActivePals.insert(Pal);
            }
        }

        // 1. Clean Processed Memory
        // If a Pal is deleted OR no longer active (e.g., returned to pool), FORGET IT so it can be swapped again later!
        for (auto it = ProcessedPals.begin(); it != ProcessedPals.end(); ) {
            if (ActivePals.find(*it) == ActivePals.end()) {
                it = ProcessedPals.erase(it); 
            } else {
                ++it;
            }
        }

        // 2. Clean Pipeline Memory
        // Only erase from the pipeline if the pointer was completely deleted from memory
        for (auto it = ProcessingQueue.begin(); it != ProcessingQueue.end(); ) {
            if (ValidPointers.find(it->Character) == ValidPointers.end()) {
                it = ProcessingQueue.erase(it); 
            } else {
                ++it;
            }
        }

        // 3. Inject any un-tracked Pals into the start of the Pipeline (State 0)
        for (UObject* Pal : ValidPointers) {
            if (ProcessedPals.find(Pal) != ProcessedPals.end()) continue;
            
            bool bInQueue = false;
            for (const auto& qp : ProcessingQueue) {
                if (qp.Character == Pal) {
                    bInQueue = true; break;
                }
            }
            if (!bInQueue) {
                ProcessPal(Pal, false);
            }
        }
    }
}