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
#include <cwctype>

using namespace RC;
using namespace RC::Unreal;

namespace DynPals {

    // =========================================================================
    // PROPERTY & REFLECTION CACHE
    // =========================================================================
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
        
        bool bIsStatsInit = false;
        bool bIsCoreGlobalsInit = false;
    };
    
    static FPalPropertyCache GCachedProps;
    static std::map<std::wstring, std::wstring> GBPClassCache;

    // =========================================================================
    // IDENTITY & STAT RESOLUTION HELPERS
    // =========================================================================
    struct FPalIdentity {
        std::wstring InstanceID;
        FPalInstanceID InstanceIDStruct;
        UObject* IndivParam = nullptr;
        bool bIsValid = false;
    };

    static FPalIdentity ResolvePalIdentity(UObject* Character) {
        FPalIdentity id;
        if (!Character || !Utils::IsObjectValid(Character)) return id;

        UObject* ParamComp = nullptr;
        Utils::GetPropertyValue<UObject*>(Character, STR("CharacterParameterComponent"), ParamComp, true);
        if (!ParamComp || !Utils::IsObjectValid(ParamComp)) return id;

        Utils::GetPropertyValue<UObject*>(ParamComp, STR("IndividualParameter"), id.IndivParam, true);
        if (!id.IndivParam || !Utils::IsObjectValid(id.IndivParam)) return id;

        UClass* FunnelClass = Utils::GetClassCached(STR("/Script/Pal.PalFunnelCharacter"));
        bool bIsFunnel = FunnelClass && Character->GetClassPrivate()->IsChildOf(FunnelClass);

        bool bGotID = false;
        if (bIsFunnel) {
            bGotID = Utils::GetPropertyValue<FPalInstanceID>(Character, STR("OwnerCharacterId"), id.InstanceIDStruct, true);
        }
        if (!bGotID || !id.InstanceIDStruct.InstanceId.IsValid()) {
            bGotID = Utils::GetPropertyValue<FPalInstanceID>(id.IndivParam, STR("IndividualId"), id.InstanceIDStruct, true);
        }

        if (bGotID && id.InstanceIDStruct.InstanceId.IsValid()) {
            id.InstanceID = Utils::GuidToWString(id.InstanceIDStruct.InstanceId);
            id.bIsValid = true;
        }
        return id;
    }

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

    // =========================================================================
    // CDO & ENGINE DEFAULT HELPERS
    // =========================================================================
    struct FVanillaDefaults {
        UClass* AnimClass = nullptr;
        UObject* Skeleton = nullptr;
        UObject* SkelMesh = nullptr;
        UObject* StaticParam = nullptr;
        FVector_UE5 MeshScale = { 1.0, 1.0, 1.0 };
        float CapsuleHalfHeight = 0.0f;
        float CapsuleRadius = 0.0f;
    };

    static FVanillaDefaults ExtractVanillaDefaults(UObject* Character) {
    FVanillaDefaults defs;
    if (!Character || !Utils::IsObjectValid(Character)) return defs;

    UClass* CharClass = Character->GetClassPrivate();
    if (!CharClass || !Utils::IsObjectValid(CharClass)) return defs;

    UObject* VanillaCDO = CharClass->GetClassDefaultObject();
    if (!VanillaCDO || !Utils::IsObjectValid(VanillaCDO)) return defs;

    UObject* VanillaMesh = nullptr;
    Utils::GetPropertyValue<UObject*>(VanillaCDO, STR("Mesh"), VanillaMesh);
    if (VanillaMesh && Utils::IsObjectValid(VanillaMesh)) {
        Utils::GetPropertyValue<UClass*>(VanillaMesh, STR("AnimClass"), defs.AnimClass);
        if (defs.AnimClass && Utils::IsObjectValid(defs.AnimClass)) {
            Utils::GetPropertyValue<UObject*>(defs.AnimClass, STR("TargetSkeleton"), defs.Skeleton);
        }
        if (!Utils::GetPropertyValue<UObject*>(VanillaMesh, STR("SkeletalMesh"), defs.SkelMesh)) {
            Utils::GetPropertyValue<UObject*>(VanillaMesh, STR("SkinnedAsset"), defs.SkelMesh);
        }
        if (defs.SkelMesh && Utils::IsObjectValid(defs.SkelMesh) && !defs.Skeleton) {
            Utils::GetPropertyValue<UObject*>(defs.SkelMesh, STR("Skeleton"), defs.Skeleton);
        }

        // Read RelativeScale3D directly (contains the 1.45 for Bosses)
        FVector_UE5 DefaultMeshScale{ 1.0, 1.0, 1.0 };
        if (Utils::GetPropertyValue<FVector_UE5>(VanillaMesh, STR("RelativeScale3D"), DefaultMeshScale)) {
            if (DefaultMeshScale.X > 0.001 && DefaultMeshScale.Y > 0.001 && DefaultMeshScale.Z > 0.001) {
                defs.MeshScale = DefaultMeshScale;
            }
        }
    }
    
    Utils::GetPropertyValue<UObject*>(VanillaCDO, STR("StaticCharacterParameterComponent"), defs.StaticParam);
    return defs;
}

    static void SyncStaticCharacterParams(UObject* SrcStaticParam, UObject* DestCharacter) {
        if (!SrcStaticParam || !DestCharacter || !Utils::IsObjectValid(SrcStaticParam) || !Utils::IsObjectValid(DestCharacter)) return;

        UObject* DestStaticParam = nullptr;
        Utils::GetPropertyValue<UObject*>(DestCharacter, STR("StaticCharacterParameterComponent"), DestStaticParam);
        if (!DestStaticParam || !Utils::IsObjectValid(DestStaticParam)) return;

        static const wchar_t* const PropNames[] = {
            STR("RandomRestMontageInfos"),
            STR("GeneralAnimSequenceMap"),
            STR("GeneralMontageMap"),
            STR("GeneralBlendSpaceMap"),
            STR("ActionMontageMap"),
            STR("SleepOnSideAnimMontage"),
            STR("PettingSize"),
            STR("PettingStartAddDistance"),
            STR("PettingEndLeaveDistance"),
            STR("PettingDistance"),
            STR("HPGaugeUIOffset"),
            STR("SleepOnSideInfoMapForMapObject"),
            STR("WazaActionDeclarationMap")
        };

        for (const wchar_t* propName : PropNames) {
            FProperty* SrcProp = Utils::GetProperty(SrcStaticParam, propName);
            FProperty* DestProp = Utils::GetProperty(DestStaticParam, propName);
            if (SrcProp && DestProp) {
                void* SrcPtr = SrcProp->ContainerPtrToValuePtr<void>(SrcStaticParam);
                void* DestPtr = DestProp->ContainerPtrToValuePtr<void>(DestStaticParam);
                if (SrcPtr && DestPtr) {
                    DestProp->CopyCompleteValue(DestPtr, SrcPtr);
                }
            }
        }
    }

    static void ClearMaterialOverrides(UObject* MeshComp) {
        if (!MeshComp || !Utils::IsObjectValid(MeshComp)) return;
        struct { int32_t RetVal; } NumMatParams{ 0 };
        Utils::CallFunction(MeshComp, STR("GetNumMaterials"), &NumMatParams);
        for (int32_t i = 0; i < NumMatParams.RetVal; ++i) {
            struct { int32_t ElementIndex; UObject* Material; } ClearMatParams{ i, nullptr };
            Utils::CallFunction(MeshComp, STR("SetMaterial"), &ClearMatParams);
        }
    }

    static void ReLinkAnimLayers(UObject* MeshComp) {
        if (!MeshComp || !Utils::IsObjectValid(MeshComp)) return;
        UObject* AnimInst = nullptr;
        Utils::CallFunction(MeshComp, STR("GetAnimInstance"), &AnimInst);
        if (!AnimInst || !Utils::IsObjectValid(AnimInst)) return;

        UFunction* LinkFunc = AnimInst->GetFunctionByNameInChain(STR("LinkAnimClassLayers"));
        if (!LinkFunc) return;

        static const std::vector<std::wstring> StandardLayers = {
            L"/Game/Pal/Blueprint/Character/Monster/ALI_MonsterBase.ALI_MonsterBase_C",
            L"/Game/Pal/Blueprint/Character/Monster/ALI_MonsterPhysics.ALI_MonsterPhysics_C"
        };

        for (const auto& LayerPath : StandardLayers) {
            UClass* LayerClass = static_cast<UClass*>(Utils::LoadAssetInternal(LayerPath, false));
            if (LayerClass && Utils::IsObjectValid(LayerClass)) {
                struct { UClass* InClass; } LinkParams{ LayerClass };
                Utils::SafeProcessEvent(AnimInst, LinkFunc, &LinkParams);
            }
        }
    }

    static void RefreshFacialModule(UObject* Character, UObject* MeshComp) {
        if (!Character || !MeshComp || !Utils::IsObjectValid(Character) || !Utils::IsObjectValid(MeshComp)) return;

        UObject* FacialComp = nullptr;
        Utils::GetPropertyValue<UObject*>(Character, STR("PalFacial"), FacialComp);
        if (!FacialComp || !Utils::IsObjectValid(FacialComp)) {
            UClass* FacialClass = Utils::GetClassCached(STR("/Script/Pal.PalFacialComponent"));
            if (FacialClass) {
                struct { UClass* ComponentClass; UObject* ReturnValue; } GetCompParams{ FacialClass, nullptr };
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

                    int32_t EyeIdx = -2, MouthIdx = -2, BrowIdx = -2;
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
    }

    // =========================================================================
    // DYNAMICS & KAWAII PHYSICS RESET PIPELINE
    // =========================================================================
    static void ResetPhysicsAndDynamics(UObject* MeshComp) {
        if (!MeshComp || !Utils::IsObjectValid(MeshComp)) return;

        // 1. Reset standard Unreal Engine Dynamics (AnimDynamics, RigidBody, KawaiiPhysics on main AnimInstance)
        UObject* AnimInst = nullptr;
        Utils::CallFunction(MeshComp, STR("GetAnimInstance"), &AnimInst);
        if (AnimInst && Utils::IsObjectValid(AnimInst)) {
            struct { uint8_t InTeleportType; } ResetParams{ 1 }; // ETeleportType: ResetPhysics = 1
            UFunction* ResetFunc = AnimInst->GetFunctionByNameInChain(STR("ResetDynamics"));
            if (ResetFunc) {
                Utils::SafeProcessEvent(AnimInst, ResetFunc, &ResetParams);
            }
        }

        // 2. Reset PostProcess AnimInstance (secondary physics motion)
        UObject* PostProcessInst = nullptr;
        Utils::CallFunction(MeshComp, STR("GetPostProcessInstance"), &PostProcessInst);
        if (PostProcessInst && Utils::IsObjectValid(PostProcessInst)) {
            struct { uint8_t InTeleportType; } ResetParams{ 1 };
            UFunction* ResetFunc = PostProcessInst->GetFunctionByNameInChain(STR("ResetDynamics"));
            if (ResetFunc) {
                Utils::SafeProcessEvent(PostProcessInst, ResetFunc, &ResetParams);
            }
        }

        // 3. KawaiiPhysicsLibrary / Custom Library Fallback
        static UObject* KawaiiLib = nullptr;
        if (!KawaiiLib) {
            KawaiiLib = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/KawaiiPhysics.Default__KawaiiPhysicsLibrary"));
            if (!KawaiiLib) KawaiiLib = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/KawaiiPhysicsRuntime.Default__KawaiiPhysicsLibrary"));
        }

        if (KawaiiLib && Utils::IsObjectValid(KawaiiLib)) {
            UFunction* KResetFunc = KawaiiLib->GetFunctionByNameInChain(STR("ResetDynamics"));
            if (KResetFunc) {
                struct { UObject* InMeshComponent; } KParams{ MeshComp };
                Utils::SafeProcessEvent(KawaiiLib, KResetFunc, &KParams);
            }
        }
    }

    static bool IsValidPalActor(UObject* Obj) {
        if (!Obj) return false;
        if (!Utils::IsMemoryReadable(Obj, sizeof(void*))) return false;
        if (!Utils::IsObjectValid(Obj)) return false;

        UClass* Cls = Obj->GetClassPrivate();
        if (!Cls || !Utils::IsMemoryReadable(Cls, sizeof(void*))) return false;

        UClass* PalCharClass = Utils::GetClassCached(STR("/Script/Pal.PalCharacter"));
        if (!PalCharClass) PalCharClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Pal.PalCharacter"));
        if (!PalCharClass || !Cls->IsChildOf(PalCharClass)) return false;

        bool bBeingDestroyed = false;
        if (Utils::GetPropertyValue<bool>(Obj, STR("bActorIsBeingDestroyed"), bBeingDestroyed, true) && bBeingDestroyed) {
            return false;
        }

        std::wstring name = Obj->GetName();
        if (name.empty() || name == L"None" || name.find(L"Default__") != std::wstring::npos) {
            return false;
        }

        UObject* Level = Obj->GetOuterPrivate();
        if (!Level || !Utils::IsObjectValid(Level)) return false;
        UObject* World = Level->GetOuterPrivate();
        if (!World || !Utils::IsObjectValid(World) || World->GetClassPrivate()->GetName() != L"World") return false;

        return true;
    }

    // =========================================================================
    // CORE ASSET & PATH RESOLUTION
    // =========================================================================
    static bool ResolvePalBlueprintPath(UObject* WorldContext, const std::wstring& CharID, std::wstring& OutPath) {
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

        std::wstring fullPath = AnimTarget;
        size_t dotPos = fullPath.find(L'.');
        if (dotPos == std::wstring::npos) {
            size_t lastSlash = fullPath.find_last_of(L'/');
            if (lastSlash != std::wstring::npos) {
                std::wstring leafName = fullPath.substr(lastSlash + 1);
                fullPath = fullPath + L"." + leafName + L"_C";
            }
        } 
        else if (fullPath.length() < 2 || fullPath.substr(fullPath.length() - 2) != L"_C") {
            fullPath += L"_C";
        }

        return fullPath;
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
            return false;
        }

        bool bHidden = false;
        if (Utils::GetPropertyValue<bool>(Pal, STR("bHidden"), bHidden, true) && bHidden) {
            if (OutBlueprintName.find(L"FunnelCharacter") != std::wstring::npos) return false;
        }

        return true;
    }

    static bool StartsWithIgnoreCase(std::wstring_view str, std::wstring_view prefix) {
        if (str.size() < prefix.size()) return false;
        for (size_t i = 0; i < prefix.size(); ++i) {
            if (std::towlower(str[i]) != std::towlower(prefix[i])) return false;
        }
        return true;
    }

    std::wstring PalProcessor::StripCharacterPrefix(const std::wstring& InputID) {
        std::wstring result = InputID;

        // 1. Strip Boss/Raid Prefixes
        if      (StartsWithIgnoreCase(result, L"MiddleBoss_")) result = result.substr(11);
        else if (StartsWithIgnoreCase(result, L"BOSS_"))       result = result.substr(5);
        else if (StartsWithIgnoreCase(result, L"RAID_"))       result = result.substr(5);
        else if (StartsWithIgnoreCase(result, L"GYM_"))        result = result.substr(4);
        else if (StartsWithIgnoreCase(result, L"PREDATOR_"))   result = result.substr(9); 

        // 2. Strip Player-Party "_otomo" Suffixes but for some reason still not working, gotta check other reasons to figure out whats going on
        if (result.length() > 6) {
            std::wstring suffix = result.substr(result.length() - 6);
            std::wstring lowerSuffix = suffix;
            std::transform(lowerSuffix.begin(), lowerSuffix.end(), lowerSuffix.begin(), ::towlower);
            
            if (lowerSuffix == L"_otomo") {
                result = result.substr(0, result.length() - 6);
            }
        }
        
        return result;
    }

    void PalProcessor::ScanActivePals() {
        return;
    }

    // =========================================================================
    // BIDIRECTIONAL LINKED PAL FINDER (FUNNEL <-> OWNER PAL)
    // =========================================================================
    std::vector<UObject*> PalProcessor::GetLinkedPals(UObject* Character) {
        std::vector<UObject*> result;
        if (!Character || !Utils::IsObjectValid(Character)) return result;

        result.push_back(Character);

        UClass* FunnelClass = Utils::GetClassCached(STR("/Script/Pal.PalFunnelCharacter"));
        bool bIsFunnel = FunnelClass && Character->GetClassPrivate()->IsChildOf(FunnelClass);

        if (bIsFunnel) {
            UFunction* GetOwnerFunc = Character->GetFunctionByNameInChain(STR("GetOwnerPal"));
            if (GetOwnerFunc) {
                struct { UObject* RetVal; } Params{ nullptr };
                Utils::SafeProcessEvent(Character, GetOwnerFunc, &Params);
                if (Params.RetVal && Utils::IsObjectValid(Params.RetVal)) {
                    result.push_back(Params.RetVal);
                }
            }
        } 
        else {
            UObject* PalUtil = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
            if (PalUtil && Utils::IsObjectValid(PalUtil)) {
                UFunction* IsOtomoFunc = PalUtil->GetFunctionByNameInChain(STR("IsPlayersOtomo"));
                if (IsOtomoFunc) {
                    struct { UObject* Actor; bool RetVal; } OtomoParams{ Character, false };
                    Utils::SafeProcessEvent(PalUtil, IsOtomoFunc, &OtomoParams);
                    
                    if (OtomoParams.RetVal) {
                        struct { UObject* WorldContext; UObject* RetVal; } FMgrParams{ Character, nullptr };
                        Utils::CallFunction(PalUtil, STR("GetFunnelCharacterManager"), &FMgrParams);
                        
                        if (FMgrParams.RetVal && Utils::IsObjectValid(FMgrParams.RetVal)) {
                            struct { UObject* Owner; UObject* RetVal; } FunnelParams{ Character, nullptr };
                            Utils::SafeProcessEvent(FMgrParams.RetVal, FMgrParams.RetVal->GetFunctionByNameInChain(STR("GetFunnelCharacterByOwner")), &FunnelParams);
                            if (FunnelParams.RetVal && Utils::IsObjectValid(FunnelParams.RetVal)) {
                                result.push_back(FunnelParams.RetVal);
                            }
                        }
                    }
                }
            }
        }

        return result;
    }
    
    void PalProcessor::DelayedSwap(UObject* Character, int SwapIndex, const std::wstring& CompName) {
        if (!Character || !Utils::IsObjectValid(Character)) return;
        
        // 1. Play the visual composition instantly and get the JSON-defined swap delay
        float DelaySeconds = VFXManager::Get().PlayComposition(Character, CompName);
        int DelayMs = static_cast<int>(DelaySeconds * 1000.0f);
        
        // 2. Schedule the physical swap for later via ForceSwap
        // This ensures the SaveManager state is locked immediately so we don't get duplicate level-up fires
        ForceSwap(Character, SwapIndex, DelayMs);
    }

    void PalProcessor::DelayedReroll(UObject* Character, const std::wstring& CompName) {
        if (!Character || !Utils::IsObjectValid(Character)) return;
        
        float DelaySeconds = VFXManager::Get().PlayComposition(Character, CompName);
        int DelayMs = static_cast<int>(DelaySeconds * 1000.0f);
        
        std::thread([Character, DelayMs]() {
            // FIX: Using milliseconds instead of seconds
            std::this_thread::sleep_for(std::chrono::milliseconds(DelayMs));
            AsyncHelper::AsyncTask(ENamedThreads::GameThread, [Character]() {
                if (!Utils::IsObjectValid(Character)) return; 
                PalProcessor::Get().ProcessPal(Character, true);
            });
        }).detach();
    }

    void PalProcessor::ForceSwap(UObject* Character, int SwapIndex, int DelayMs) {
        if (!Utils::IsObjectValid(Character) || SwapIndex < 0 || SwapIndex >= (int)ConfigManager::Get().GetConfigs().size()) return;

        FPalIdentity id = ResolvePalIdentity(Character);
        if (!id.bIsValid) return;

        ClearSwappedStatus(id.InstanceID, Character);

        auto& config = ConfigManager::Get().GetConfigs()[SwapIndex];

        PalPersistData* ExistingData = SaveManager::Get().GetPersistData(id.InstanceID);
        if (!ExistingData) {
            PalPersistData newData;
            newData.InstanceID = id.InstanceID;
            newData.PackName = config.PackName;
            newData.SkinName = config.SkinName;
            newData.SwapLabel = config.SwapLabel; 
            newData.SkelMeshPath = config.SkelMeshPath;
            newData.SizeMultiplier = -1.0; // Reset size on new explicit swap
            SaveManager::Get().SetPersistData(id.InstanceID, newData, true); 
        } else {
            ExistingData->PackName = config.PackName;
            ExistingData->SkinName = config.SkinName;
            ExistingData->SwapLabel = config.SwapLabel;
            ExistingData->SkelMeshPath = config.SkelMeshPath;
            
            ExistingData->MorphSet.clear();
            ExistingData->MatSet.clear();
            ExistingData->MatColorSet.clear();
            ExistingData->SizeMultiplier = -1.0; // Reset size on new explicit swap
            
            SaveManager::Get().SetPersistData(id.InstanceID, *ExistingData, true); 
        }

        // Link up companions immediately
        GetLinkedPals(Character);

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

    void PalProcessor::ProcessPal(UObject* Character, bool ForceReroll, int ExplicitSwapIndex, bool IsCompanionSync, bool IsEvolutionEnd) {
        if (!Character || !Utils::IsObjectValid(Character)) return;

        std::lock_guard<std::mutex> lock(QueueMutex);
        for (auto& q : SwapQueue) {
            if (q.Character == Character) {
                if (ForceReroll) q.ForceReroll = true;
                if (ExplicitSwapIndex != -1) q.ExplicitSwapIndex = ExplicitSwapIndex;
                if (IsCompanionSync) q.IsCompanionSync = true;
                if (IsEvolutionEnd) q.IsEvolutionEnd = true;
                return;
            }
        }
        SwapQueue.push_back({Character, ForceReroll, ExplicitSwapIndex, IsCompanionSync, IsEvolutionEnd});
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

        struct { UObject* WorldContext; UObject* ReturnValue; } FunnelMgrParams{ WorldContext, nullptr };
        Utils::SafeProcessEvent(PalUtil, PalUtil->GetFunctionByNameInChain(STR("GetFunnelCharacterManager")), &FunnelMgrParams);
        UObject* FunnelManager = FunnelMgrParams.ReturnValue;
        
        if (OtomoHolder && Utils::IsObjectValid(OtomoHolder)) {
            for (int32_t i = 0; i < 5; ++i) {
                struct { int32_t SlotIndex; UObject* ReturnValue; } OtomoParams{ i, nullptr };
                Utils::SafeProcessEvent(OtomoHolder, OtomoHolder->GetFunctionByNameInChain(STR("TryGetOtomoActorBySlotIndex")), &OtomoParams);
                UObject* OtomoChar = OtomoParams.ReturnValue;
                
                if (IsValidPalActor(OtomoChar)) {
                    ProcessPal(OtomoChar, false);

                    if (FunnelManager && Utils::IsObjectValid(FunnelManager)) {
                        struct { UObject* Owner; UObject* ReturnValue; } FunnelParams{ OtomoChar, nullptr };
                        Utils::SafeProcessEvent(FunnelManager, FunnelManager->GetFunctionByNameInChain(STR("GetFunnelCharacterByOwner")), &FunnelParams);
                        if (IsValidPalActor(FunnelParams.ReturnValue)) {
                            ProcessPal(FunnelParams.ReturnValue, false);
                        }
                    }
                }
            }
        }
    }

    void PalProcessor::ClearAllSwappedStatus() {
        std::lock_guard<std::mutex> lock(QueueMutex);
        SwapQueue.clear();
        SwappedInstances.clear();
        RuntimeStatsCache.clear();
        ProcessedPals.clear();
        ProcessingQueue.clear();
    }

    void PalProcessor::ClearSwappedStatus(const std::wstring& InstanceID, RC::Unreal::UObject* Character) {
        if (Character) {
            SwappedInstances.erase(Character);
        }
        RuntimeStatsCache.erase(InstanceID);
    }

    void PalProcessor::Tick() {
        UObject* KSL = Utils::GetKismetSystemLibrary();
        static UFunction* IsValidFunc = Utils::GetKismetFunction(STR("IsValid"));
        if (!KSL || !IsValidFunc) return;

        std::vector<QueuedSwap> pendingSwaps;
        {
            std::lock_guard<std::mutex> lock(QueueMutex);
            if (SwapQueue.empty()) return;
            
            pendingSwaps.assign(SwapQueue.begin(), SwapQueue.end());
            SwapQueue.clear();
        }

        for (const auto& req : pendingSwaps) {
            UObject* TargetChar = req.Character;

            if (Utils::IsObjectValid(TargetChar)) {
                NativeAsyncLoader::SetActiveRequester(TargetChar);

                ExecuteSwap(TargetChar, req.ForceReroll, req.ExplicitSwapIndex, req.IsCompanionSync, req.IsEvolutionEnd);

                NativeAsyncLoader::ClearTemporaryPointers(TargetChar);
                NativeAsyncLoader::SetActiveRequester(nullptr);
            }
        }
    }
    
    bool PalProcessor::ExecuteSwap(UObject* Character, bool ForceReroll, int ExplicitSwapIndex, bool IsCompanionSync, bool IsEvolutionEnd) {
        if (!Character || !Utils::IsObjectValid(Character)) return false;
        
        FPalIdentity id = ResolvePalIdentity(Character);
        if (!id.bIsValid) return false;

        std::vector<UObject*> palSet = GetLinkedPals(Character);

        std::wstring BlueprintName = L"";
        if (!IsPalBlueprintValid(Character, BlueprintName)) return false;

        UObject* Level = Character->GetOuterPrivate();
        if (!Level || !Utils::IsObjectValid(Level)) return false;
        UObject* World = Level->GetOuterPrivate();
        if (!World || !Utils::IsObjectValid(World) || World->GetClassPrivate()->GetName() != L"World") return false;

        static UObject* PalUtil = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
        if (!PalUtil || !Utils::IsObjectValid(PalUtil)) return false;

        if (!GCachedProps.bIsCoreGlobalsInit) {
            GCachedProps.GetCharacterIDFromCharacterFunc = PalUtil->GetFunctionByNameInChain(STR("GetCharacterIDFromCharacter"));
            GCachedProps.IsWildNPCFunc = PalUtil->GetFunctionByNameInChain(STR("IsWildNPC"));
            
            if (id.IndivParam && Utils::IsObjectValid(id.IndivParam)) {
                GCachedProps.IsRarePalFunc = id.IndivParam->GetFunctionByNameInChain(STR("IsRarePal"));
                GCachedProps.GetGenderTypeFunc = id.IndivParam->GetFunctionByNameInChain(STR("GetGenderType"));
                GCachedProps.GetSkinNameFunc = id.IndivParam->GetFunctionByNameInChain(STR("GetSkinName"));
                GCachedProps.GetPassiveSkillListFunc = id.IndivParam->GetFunctionByNameInChain(STR("GetPassiveSkillList"));
            }
            GCachedProps.bIsCoreGlobalsInit = true;
        }

        struct { UObject* Char; FName RetVal; } CharIDParams{Character, FName()};
        if (GCachedProps.GetCharacterIDFromCharacterFunc) {
            Utils::SafeProcessEvent(PalUtil, GCachedProps.GetCharacterIDFromCharacterFunc, &CharIDParams);
        }
        std::wstring RawCharID = CharIDParams.RetVal.ToString();

        if (RawCharID.rfind(L"GYM_", 0) == 0 || RawCharID.find(L"_Gym_") != std::wstring::npos) return false;

        static UObject* LastWorldLoaded = nullptr;
        if (World != LastWorldLoaded) {
            SaveManager::Get().LoadWorldData(World);
            LastWorldLoaded = World;
        }
        PalPersistData* ExistingData = SaveManager::Get().GetPersistData(id.InstanceID);

        std::wstring CharID = StripCharacterPrefix(RawCharID);

        PalRuntimeStats stats = RetrievePalStats(id.IndivParam, RawCharID, id.InstanceID, true);
        int LevelNum = stats.Level;
        int RankNum = stats.Rank;
        int FriendshipNum = stats.Friendship;

        struct { UObject* Actor; bool RetVal; } WildParams{Character, false};
        if (GCachedProps.IsWildNPCFunc) Utils::SafeProcessEvent(PalUtil, GCachedProps.IsWildNPCFunc, &WildParams);
        bool IsWild = WildParams.RetVal;

        struct { bool ReturnValue; } RareParams{false};
        if (GCachedProps.IsRarePalFunc) Utils::SafeProcessEvent(id.IndivParam, GCachedProps.IsRarePalFunc, &RareParams);
        bool IsRare = RareParams.ReturnValue;

        struct { uint8_t RetVal; } GenderParams{0};
        if (GCachedProps.GetGenderTypeFunc) Utils::SafeProcessEvent(id.IndivParam, GCachedProps.GetGenderTypeFunc, &GenderParams);
        std::wstring GenderStr = (GenderParams.RetVal == 1) ? L"Male" : ((GenderParams.RetVal == 2) ? L"Female" : L"None");

        struct { FName RetVal; } SkinParams{FName()};
        if (GCachedProps.GetSkinNameFunc) Utils::SafeProcessEvent(id.IndivParam, GCachedProps.GetSkinNameFunc, &SkinParams);
        std::wstring SkinName = SkinParams.RetVal.ToString();
        if (SkinName == L"None") SkinName = L"";

        std::vector<std::wstring> Traits;
        struct { TArray<FName> RetVal; } TraitsParams;
        if (GCachedProps.GetPassiveSkillListFunc) {
            Utils::SafeProcessEvent(id.IndivParam, GCachedProps.GetPassiveSkillListFunc, &TraitsParams);
            for (int32_t i = 0; i < TraitsParams.RetVal.Num(); ++i) {
                Traits.push_back(TraitsParams.RetVal[i].ToString());
            }
        }

        PalRuntimeStats& CachedStats = RuntimeStatsCache[id.InstanceID];
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
            
            if (!IsEvolutionEnd) {
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
            } else {
                bManualLockState = ExistingData ? ExistingData->bIsManuallyLocked : false;
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
                    RawCharID, id.InstanceID, (void*)Character,
                    (ExplicitSwapIndex != -1) ? L"Explicit Selection" : 
                    (ForceReroll) ? L"Force Reroll" : 
                    (finalSwap != currentSwap) ? L"Skin Changed" : L"New Actor Spawned");

                // FIX: bLiveEventTriggered already guarantees this is a live in-game event and not a fresh world spawn.
                // Removing !bIsNewActor allows vanilla Pals to evolve into custom models with VFX.
                bool bIsLiveEvolution = bLiveEventTriggered && (finalSwap != currentSwap) && (ExplicitSwapIndex == -1) && !ForceReroll;

                std::vector<std::wstring> assetsToLoad;
                bool bHasFailedDependency = false;
                std::wstring failedPath = L"";

                auto CheckDependency = [&](const std::wstring& Path) {
                    if (Path.empty()) return;

                    if (NativeAsyncLoader::IsPending(Path)) {
                        assetsToLoad.push_back(Path);
                        return;
                    }

                    if (NativeAsyncLoader::IsFailed(Path)) {
                        bHasFailedDependency = true;
                        failedPath = Path;
                        return;
                    }

                    if (NativeAsyncLoader::GetGlobalPointer(Path) != nullptr) {
                        return; 
                    }

                    assetsToLoad.push_back(Path);
                };

                CheckDependency(finalConfig.SkelMeshPath);

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

                if (bIsLiveEvolution) {
                    auto compAssets = VFXManager::Get().GetCompositionAssets(L"evolve_1");
                    for (const auto& p : compAssets) CheckDependency(p);
                } else if (ForceReroll || (ExplicitSwapIndex != -1 && !IsEvolutionEnd)) {
                    CheckDependency(L"/Game/Pal/Effect/Common/LevelUp/NS_LevelUp_Pal");
                }

                if (bHasFailedDependency) {
                    DP_LOG(Error, "[Swap Aborted] Pal '{}' swap failed: Material or Mesh asset does not exist! Path: '{}'", RawCharID, failedPath);
                    return false;
                }

                if (!assetsToLoad.empty()) {
                    NativeAsyncLoader::RegisterPendingRequests(Character, static_cast<int>(assetsToLoad.size()));
                    
                    PalPersistData tempPersist = ExistingData ? *ExistingData : PalPersistData{ id.InstanceID, L"", L"", L"", {} };
                    tempPersist.bIsManuallyLocked = bManualLockState; 
                    tempPersist.PackName = finalConfig.PackName;
                    tempPersist.SkinName = finalConfig.SkinName;
                    tempPersist.SwapLabel = finalConfig.SwapLabel;
                    tempPersist.SkelMeshPath = finalConfig.SkelMeshPath;
                    SaveManager::Get().SetPersistData(id.InstanceID, tempPersist, false);

                    if (NativeAsyncLoader::RequestBatchAsyncLoad(assetsToLoad, Character, ExplicitSwapIndex, ForceReroll, IsCompanionSync, IsEvolutionEnd)) {
                        return true; 
                    }
                } else if (NativeAsyncLoader::GetPendingCount(Character) > 0) {
                    return true;
                }

                if (bIsLiveEvolution) {
                    DP_LOG(Normal, "Live Evolution Triggered! Deferring physical swap for visual composition...");
                    DelayedSwap(Character, finalSwap, L"evolve_1");
                    return true; 
                }

                PalPersistData newData = ExistingData ? *ExistingData : PalPersistData{ id.InstanceID, L"", L"", L"", {} };
                newData.bIsManuallyLocked = bManualLockState; 

                newData.PackName = finalConfig.PackName;
                newData.SkinName = finalConfig.SkinName;
                newData.SwapLabel = finalConfig.SwapLabel;
                newData.SkelMeshPath = finalConfig.SkelMeshPath;

                // Reset size and randomizations on skin change, manual pick, or reroll
                if (ForceReroll || ExplicitSwapIndex != -1 || finalSwap != currentSwap) {
                    newData.MorphSet.clear();
                    newData.MatSet.clear();
                    newData.MatColorSet.clear(); 
                    newData.SizeMultiplier = -1.0; 
                }

                if (!finalConfig.SetNickname.empty()) {
                    bool bNicknameIsEmpty = false;
                    FProperty* SaveParamProp = Utils::GetProperty(id.IndivParam, STR("SaveParameter"));
                    if (SaveParamProp) {
                        void* SaveParamPtr = SaveParamProp->ContainerPtrToValuePtr<void>(id.IndivParam);
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
                            void* SaveParamPtr = SaveParamProp->ContainerPtrToValuePtr<void>(id.IndivParam);
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
                                    Params.InstanceId = id.InstanceIDStruct;
                                    Params.NewNickName = FString(finalConfig.SetNickname.c_str());
                                    
                                    Utils::SafeProcessEvent(PlayerController, UpdateNameFunc, &Params);
                                }
                            }
                        }

                        DP_LOG(Default, "[Nickname] Applied name update for '{}' -> '{}' (Wild: {})", id.InstanceID, finalConfig.SetNickname, IsWild ? L"True" : L"False");
                    }
                }

                ApplySwap(Character, finalConfig, newData);

                bool bIsManualAction = (ExplicitSwapIndex != -1) || ForceReroll;
                if (IsEvolutionEnd) bIsManualAction = false; 

                SaveManager::Get().SetPersistData(id.InstanceID, newData, bIsManualAction);

                SwappedInstances[Character] = finalConfig.SwapLabel;

                if (bIsManualAction) {
                    VFXManager::Get().PlaySwapEffect(Character, L"/Game/Pal/Effect/Common/LevelUp/NS_LevelUp_Pal");
                }

                // --- BIDIRECTIONAL COMPANION SYNC DISPATCH ---
                if (!IsCompanionSync) {
                    std::vector<UObject*> linkedCompanions = GetLinkedPals(Character);
                    for (UObject* Companion : linkedCompanions) {
                        if (Utils::IsObjectValid(Companion) && Companion != Character) {
                            DP_LOG(Default, "[PalProcessor] Bidirectionally syncing partner actor: '{}' (Actor: {})", Companion->GetName(), (void*)Companion);
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
        if (!MeshComp || !Utils::IsObjectValid(MeshComp)) return;
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

        FVanillaDefaults vanillaDefs = ExtractVanillaDefaults(Character);

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
            TargetAnimClass = vanillaDefs.AnimClass;
            TargetSkeleton = vanillaDefs.Skeleton;
            TargetStaticParam = vanillaDefs.StaticParam;

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

                        struct { UObject* InMesh; bool bReinitPose; } MeshParams{NewMesh, false};
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

            SyncStaticCharacterParams(TargetStaticParam, Character);
            ReLinkAnimLayers(MeshComp);
            ProfileStep(L"Trace 8: Syncing Static Params & Layers");
        }

        // =========================================================================
        // MATERIAL OVERRIDES
        // =========================================================================
        ClearMaterialOverrides(MeshComp);
        ProfileStep(L"Trace 9.0: Clear Existing Materials");

        for (auto& mat : swap.MatReplaceList) {
            std::wstring ChosenPath = mat.matPath;
            std::wstring WideIndex = Utils::StringToWString(mat.index);

            if (mat.matPath.length() >= 2 && mat.matPath.substr(mat.matPath.length() - 2) == L"/*") {
                std::wstring VirtualFolder = mat.matPath.substr(0, mat.matPath.length() - 2);
                std::vector<std::wstring> AvailableMats = Utils::GetAssetsInVirtualFolder(VirtualFolder);
                ProfileStep(L"Trace 9.1 [Slot " + WideIndex + L"]: Wildcard Folder Scan ('" + VirtualFolder + L"')");

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
                ProfileStep(L"Trace 9.2 [Slot " + WideIndex + L"]: LoadAssetSafely ('" + ChosenPath + L"')");

                if (!NewMat || !Utils::IsObjectValid(NewMat)) {
                    DP_LOG(Warning, "[Slot {}] LoadAssetSafely FAILED for path: '{}'", WideIndex, ChosenPath);
                    continue;
                }
            } else {
                struct { int32_t ElementIndex; UObject* ReturnValue; } GetMatParams{idx, nullptr};
                Utils::CallFunction(MeshComp, STR("GetMaterial"), &GetMatParams);
                NewMat = GetMatParams.ReturnValue;
                ProfileStep(L"Trace 9.2 [Slot " + WideIndex + L"]: GetMaterial Native Fallback");
                
                if (!NewMat || !Utils::IsObjectValid(NewMat)) continue;
            }

            if (!IsPalBlueprintValid(Character, BPName)) return;

            UObject* CurrentMeshComp = nullptr;
            Utils::CallFunction(Character, STR("GetMainMesh"), &CurrentMeshComp);
            if (!CurrentMeshComp || !Utils::IsObjectValid(CurrentMeshComp)) return;

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
                    float S = 1.0f, V = 1.0f;
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
                    ProfileStep(L"Trace 9.3 [Slot " + WideIndex + L"]: Create & Apply Dynamic Material Instance (RandomHue)");
                    continue;
                }
            }

            struct { int32_t ElementIndex; UObject* Material; } MatParams{idx, NewMat};
            Utils::CallFunction(CurrentMeshComp, STR("SetMaterial"), &MatParams);
            ProfileStep(L"Trace 9.4 [Slot " + WideIndex + L"]: SetMaterial Native Call");
        }
        ProfileStep(L"Trace 9.5: Total Applying Materials Finished");

        // UNPAUSE ANIMATIONS
        Utils::SetPropertyValue<bool>(MeshComp, STR("bPauseAnims"), false, false);

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

        RefreshFacialModule(Character, MeshComp);
        ProfileStep(L"Trace 11: PalFacialComponent Setup");

        // =========================================================================
        // SIZE MULTIPLIER (Calculated against baked-in CDO Base Scale)
        // =========================================================================
        double currentSizeMult = persist.SizeMultiplier;
        if (currentSizeMult <= 0.0) {
            if (swap.MinSizeMultiplier < swap.MaxSizeMultiplier) {
                static std::random_device rd;
                static std::mt19937 gen(rd());
                std::uniform_real_distribution<> dis(swap.MinSizeMultiplier, swap.MaxSizeMultiplier);
                currentSizeMult = dis(gen);
            } else {
                currentSizeMult = swap.MinSizeMultiplier; // Defaults cleanly to 1.0 for skins without size specs
            }
            persist.SizeMultiplier = currentSizeMult;
        }

        // Multiplies the Pal's native CDO mesh scale (e.g. 1.45 for Bosses, 1.0 for regular Pals)
        FVector_UE5 FinalMeshScale = {
            vanillaDefs.MeshScale.X * currentSizeMult,
            vanillaDefs.MeshScale.Y * currentSizeMult,
            vanillaDefs.MeshScale.Z * currentSizeMult
        };

        // We explicitly set DefaultScale3D so that when Palworld stows/unstows it, it doesn't overwrite our scale!
        Utils::SetPropertyValue<FVector_UE5>(MeshComp, STR("DefaultScale3D"), FinalMeshScale);
        struct { FVector_UE5 NewScale3D; } ScaleParams{ FinalMeshScale };
        Utils::CallFunction(MeshComp, STR("SetRelativeScale3D"), &ScaleParams);

        // Also scale the capsule collision directly in the StaticCharacterParameterComponent, disabled to prevent multiplayer desync
        /*
        UObject* TargetStaticParamInst = nullptr;
        Utils::GetPropertyValue<UObject*>(Character, STR("StaticCharacterParameterComponent"), TargetStaticParamInst);
        if (TargetStaticParamInst && vanillaDefs.CapsuleHalfHeight > 0.0f) {
            Utils::SetPropertyValue<float>(TargetStaticParamInst, STR("MeshCapsuleHalfHeight"), static_cast<float>(vanillaDefs.CapsuleHalfHeight * currentSizeMult));
            Utils::SetPropertyValue<float>(TargetStaticParamInst, STR("MeshCapsuleRadius"), static_cast<float>(vanillaDefs.CapsuleRadius * currentSizeMult));
        }
            */

        DP_LOG(Default, "[Scale] Applied Mesh Scale: {:.3f} (Multiplier: {:.3f} * CDO Base: {:.3f}). Capsule updated.", 
            FinalMeshScale.X, currentSizeMult, vanillaDefs.MeshScale.X);

        ProfileStep(L"Trace 12: Applying Size Multiplier");

        // Flush dynamics / KawaiiPhysics to prevent stuck/stretched physics simulation
        ResetPhysicsAndDynamics(MeshComp);

        DP_LOG(Default, "Successfully applied swap '{}' from Pack '{}' to Pal '{}' (Scale: {:.2f}x)!\n", 
               swap.SkinName.empty() ? L"Mesh Swap" : swap.SkinName, swap.PackName, CharID, currentSizeMult);

        auto total_duration = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - total_start).count();
        DP_LOG(Default, "[Profile] [ApplySwap] Trace 13: Done! Total ApplySwap execution took {:.3f} ms", total_duration / 1000.0f);
    }

    void PalProcessor::ResetPal(UObject* Character) {
        if (!Character || !Utils::IsObjectValid(Character)) {
            DP_LOG(Warning, "[PalProcessor::ResetPal] Invalid Character passed!");
            return;
        }

        FPalIdentity id = ResolvePalIdentity(Character);
        if (!id.bIsValid) return;

        DP_LOG(Default, "[PalProcessor] Resetting Pal '{}' (ID: '{}') to Vanilla Defaults...", Character->GetName(), id.InstanceID);

        // 1. Fetch current persist data to grab any morphs that need zeroing out
        PalPersistData* ExistingData = SaveManager::Get().GetPersistData(id.InstanceID);
        std::map<std::wstring, double> MorphsToZero;
        if (ExistingData) {
            MorphsToZero = ExistingData->MorphSet;
        }

        // 2. Prepare wiped persist data locked to default
        PalPersistData newData;
        newData.InstanceID = id.InstanceID;
        newData.PackName = L"";
        newData.SkinName = L"";
        newData.SwapLabel = L"";
        newData.SkelMeshPath = L"";
        newData.MorphSet.clear();
        newData.MatSet.clear();
        newData.MatColorSet.clear();
        newData.bIsManuallyLocked = true; // Lock so the matchmaker won't auto-assign skins
        newData.SizeMultiplier = 1.0;

        SaveManager::Get().SetPersistData(id.InstanceID, newData, true);

        // 3. Clear tracked swapped instance
        SwappedInstances.erase(Character);

        // 4. Find all partners (both Funnel and Owner) and reset all of them
        std::vector<UObject*> palSet = GetLinkedPals(Character);

        for (UObject* TargetPalObj : palSet) {
            if (!TargetPalObj || !Utils::IsObjectValid(TargetPalObj)) continue;

            SwappedInstances.erase(TargetPalObj);

            std::wstring BPName;
            if (!IsPalBlueprintValid(TargetPalObj, BPName)) continue;

            UObject* MeshComp = nullptr;
            Utils::CallFunction(TargetPalObj, STR("GetMainMesh"), &MeshComp);
            if (!MeshComp || !Utils::IsObjectValid(MeshComp)) continue;

            Utils::SetPropertyValue<bool>(MeshComp, STR("bPauseAnims"), true, false);

            struct { bool bNewDisablePostProcessBlueprint; } EnablePP{ true };
            Utils::CallFunction(MeshComp, STR("SetDisablePostProcessBlueprint"), &EnablePP);

            FVanillaDefaults defs = ExtractVanillaDefaults(TargetPalObj);

            // A. Restore AnimClass
            UFunction* SetAnimFunc = MeshComp->GetFunctionByNameInChain(STR("SetAnimInstanceClass"));
            if (!SetAnimFunc) SetAnimFunc = MeshComp->GetFunctionByNameInChain(STR("SetAnimClass"));
            if (SetAnimFunc && defs.AnimClass) {
                struct { UClass* NewClass; } Params{ defs.AnimClass };
                Utils::SafeProcessEvent(MeshComp, SetAnimFunc, &Params);
            }

            // B. Restore StaticCharacterParameterComponent & Re-link layers
            SyncStaticCharacterParams(defs.StaticParam, TargetPalObj);
            ReLinkAnimLayers(MeshComp);

            // C. Restore Skeletal Mesh & Restore Exact CDO Base Scale to DefaultScale3D and RelativeScale3D
            if (defs.SkelMesh && Utils::IsObjectValid(defs.SkelMesh)) {
                if (defs.Skeleton && Utils::IsObjectValid(defs.Skeleton)) {
                    Utils::SetPropertyValue<UObject*>(defs.SkelMesh, STR("Skeleton"), defs.Skeleton);
                }
                struct { UObject* InMesh; bool bReinitPose; } MeshParams{defs.SkelMesh, true};
                Utils::CallFunction(MeshComp, STR("SetSkinnedAssetAndUpdate"), &MeshParams);
            }

            // Restore exact CDO mesh scale
            Utils::SetPropertyValue<FVector_UE5>(MeshComp, STR("DefaultScale3D"), defs.MeshScale);
            struct { FVector_UE5 NewScale3D; } ResetMeshScaleParams{ defs.MeshScale };
            Utils::CallFunction(MeshComp, STR("SetRelativeScale3D"), &ResetMeshScaleParams);

            // Restore exact CDO Capsule properties Disabled to prevent multiplayer desync
            /*
            UObject* TargetStaticParamInst = nullptr;
            Utils::GetPropertyValue<UObject*>(TargetPalObj, STR("StaticCharacterParameterComponent"), TargetStaticParamInst);
            if (TargetStaticParamInst && defs.CapsuleHalfHeight > 0.0f) {
                Utils::SetPropertyValue<float>(TargetStaticParamInst, STR("MeshCapsuleHalfHeight"), defs.CapsuleHalfHeight);
                Utils::SetPropertyValue<float>(TargetStaticParamInst, STR("MeshCapsuleRadius"), defs.CapsuleRadius);
                
            }
            */
            // D. Clear Material Overrides
            ClearMaterialOverrides(MeshComp);

            // E. Zero out Morph Targets
            for (const auto& [morphName, _] : MorphsToZero) {
                struct { FName MorphTargetName; float Value; bool bRemoveZeroWeight; } MorphParams{
                    FName(morphName.c_str(), FNAME_Add), 0.0f, true
                };
                Utils::CallFunction(MeshComp, STR("SetMorphTarget"), &MorphParams);
            }

            // F. Unpause Animations & re-enable PostProcess
            Utils::SetPropertyValue<bool>(MeshComp, STR("bPauseAnims"), false, false);
            struct { bool bNewDisablePostProcessBlueprint; } DisablePP_False{ false };
            Utils::CallFunction(MeshComp, STR("SetDisablePostProcessBlueprint"), &DisablePP_False);

            // G. Reset PalFacialComponent
            RefreshFacialModule(TargetPalObj, MeshComp);

            // H. Reset Dynamics / Physics
            ResetPhysicsAndDynamics(MeshComp);
        }

        VFXManager::Get().PlaySwapEffect(Character, L"/Game/Pal/Effect/Common/LevelUp/NS_LevelUp_Pal");
        DP_LOG(Default, "[PalProcessor] Successfully reset Pal '{}' (ID: '{}') to Vanilla and locked.", Character->GetName(), id.InstanceID);
    }
}