#include "PalProcessor.hpp"
#include "ConfigManager.hpp"
#include "SaveManager.hpp"
#include "Utils.hpp"
#include "AsyncHelper.hpp"
#include "VFXManager.hpp"
#include "../include/NativeAsyncLoader.hpp"
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <random>
#include <thread>
#include <algorithm>
#include <cwctype>
#include <fmt/xchar.h>

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
// DYNAMIC PHYSICS BONE LOGGER (KawaiiPhysics + AnimDynamics)
// =========================================================================
static void LogPhysicsInstanceBones(UObject* AnimInst, const std::wstring& ContextLabel) {
	if (!AnimInst || !Utils::IsObjectValid(AnimInst)) return;

	UClass* Class = AnimInst->GetClassPrivate();
	if (!Class || !Utils::IsObjectValid(Class)) return;

	std::vector<std::wstring> KawaiiBones;
	std::vector<std::wstring> DynamicsBones;

	for (FProperty* Property : TFieldRange<FProperty>(Class, EFieldIterationFlags::IncludeSuper)) {
		if (FStructProperty* StructProp = CastField<FStructProperty>(Property)) {
			if (UStruct* Struct = StructProp->GetStruct()) {
				std::wstring StructName = Struct->GetName();
				void* NodePtr = Property->ContainerPtrToValuePtr<void>(AnimInst);
				if (!NodePtr) continue;

				// 1. Kawaii Physics
				if (StructName == L"AnimNode_KawaiiPhysics") {
					FProperty* RootBoneProp = Struct->GetPropertyByNameInChain(STR("RootBone"));
					if (RootBoneProp) {
						void* RootBonePtr = RootBoneProp->ContainerPtrToValuePtr<void>(NodePtr);
						if (RootBonePtr) {
							if (FStructProperty* BoneRefStructProp = CastField<FStructProperty>(RootBoneProp)) {
								if (UStruct* BoneRefStruct = BoneRefStructProp->GetStruct()) {
									FProperty* BoneNameProp = BoneRefStruct->GetPropertyByNameInChain(STR("BoneName"));
									if (BoneNameProp) {
										FName* pName = BoneNameProp->ContainerPtrToValuePtr<FName>(RootBonePtr);
										if (pName) {
											std::wstring boneName = pName->ToString();
											if (!boneName.empty() && boneName != L"None") {
												KawaiiBones.push_back(boneName);
											}
										}
									}
								}
							}
						}
					}
				}
				// 2. AnimDynamics
				else if (StructName == L"AnimNode_AnimDynamics") {
					FProperty* BoundBoneProp = Struct->GetPropertyByNameInChain(STR("BoundBone"));
					if (BoundBoneProp) {
						void* BoundBonePtr = BoundBoneProp->ContainerPtrToValuePtr<void>(NodePtr);
						if (BoundBonePtr) {
							if (FStructProperty* BoneRefStructProp = CastField<FStructProperty>(BoundBoneProp)) {
								if (UStruct* BoneRefStruct = BoneRefStructProp->GetStruct()) {
									FProperty* BoneNameProp = BoneRefStruct->GetPropertyByNameInChain(STR("BoneName"));
									if (BoneNameProp) {
										FName* pName = BoneNameProp->ContainerPtrToValuePtr<FName>(BoundBonePtr);
										if (pName) {
											std::wstring boneName = pName->ToString();
											if (!boneName.empty() && boneName != L"None") {
												DynamicsBones.push_back(boneName);
											}
										}
									}
								}
							}
						}
					}
				}
			}
		}
	}

	std::wstring summary = L"";
	if (!KawaiiBones.empty()) {
		summary += L"KawaiiRoots: [";
		for (size_t i = 0; i < KawaiiBones.size(); ++i) {
			summary += KawaiiBones[i] + (i + 1 < KawaiiBones.size() ? L", " : L"");
		}
		summary += L"] ";
	}
	if (!DynamicsBones.empty()) {
		summary += L"AnimDynamics: [";
		for (size_t i = 0; i < DynamicsBones.size(); ++i) {
			summary += DynamicsBones[i] + (i + 1 < DynamicsBones.size() ? L", " : L"");
		}
		summary += L"] ";
	}

	if (summary.empty()) {
		DP_LOG(Default, "[PhysicsLog] [{}] (Class: '{}') -> No secondary physics nodes.", ContextLabel, Class->GetName());
	} else {
		DP_LOG(Default, "[PhysicsLog] [{}] (Class: '{}') -> {}", ContextLabel, Class->GetName(), summary);
	}
}

static void LogAllPhysicsLayers(UObject* MeshComp, const std::wstring& StageLabel) {
	if (!MeshComp || !Utils::IsObjectValid(MeshComp)) return;

	// 1. Main AnimInstance
	UObject* AnimInst = nullptr;
	Utils::CallFunction(MeshComp, STR("GetAnimInstance"), &AnimInst);
	LogPhysicsInstanceBones(AnimInst, StageLabel + L" | MainLayer");

	// 2. PostProcess AnimInstance
	UObject* PPInst = nullptr;
	Utils::CallFunction(MeshComp, STR("GetPostProcessInstance"), &PPInst);
	LogPhysicsInstanceBones(PPInst, StageLabel + L" | PostProcessLayer");

	// 3. Linked Instances
	FProperty* LinkedProp = Utils::GetProperty(MeshComp, STR("LinkedInstances"), true);
	if (LinkedProp) {
		TArray<UObject*>* LinkedArray = LinkedProp->ContainerPtrToValuePtr<TArray<UObject*>>(MeshComp);
		if (LinkedArray && LinkedArray->Num() > 0) {
			for (int32_t i = 0; i < LinkedArray->Num(); ++i) {
				UObject* LinkedInst = (*LinkedArray)[i];
				LogPhysicsInstanceBones(LinkedInst, StageLabel + L" | LinkedLayer[" + std::to_wstring(i) + L"]");
			}
		}
	}
}

// =========================================================================
// GLOBAL VALIDATOR
// =========================================================================
static bool IsValidPalActor(UObject* Obj) {
	if (!Utils::IsObjectValid(Obj)) return false;

	UClass* Cls = Obj->GetClassPrivate();
	if (!Cls || !Utils::IsObjectValid(Cls)) return false;

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
	if (!IsValidPalActor(Character)) return id;

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

	struct {
		int32_t RetVal = -1;
	} IntParams;

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
	UClass* PostProcessAnimClass = nullptr;
	UObject* Skeleton = nullptr;
	UObject* SkelMesh = nullptr;
	UObject* StaticParam = nullptr;
	FVector_UE5 MeshScale = { 1.0, 1.0, 1.0 };
	float CapsuleHalfHeight = 0.0f;
	float CapsuleRadius = 0.0f;
};

static FVanillaDefaults ExtractVanillaDefaults(UObject* Character) {
	FVanillaDefaults defs;
	if (!IsValidPalActor(Character)) return defs;

	UClass* CharClass = Character->GetClassPrivate();
	if (!CharClass || !Utils::IsObjectValid(CharClass)) return defs;

	UObject* VanillaCDO = CharClass->GetClassDefaultObject();
	if (!VanillaCDO || !Utils::IsObjectValid(VanillaCDO)) return defs;

	UObject* VanillaMesh = nullptr;
	Utils::GetPropertyValue<UObject*>(VanillaCDO, STR("Mesh"), VanillaMesh);
	if (VanillaMesh && Utils::IsObjectValid(VanillaMesh)) {
		Utils::GetPropertyValue<UClass*>(VanillaMesh, STR("AnimClass"), defs.AnimClass);
		Utils::GetPropertyValue<UClass*>(VanillaMesh, STR("PostProcessAnimBlueprint"), defs.PostProcessAnimClass);

		// Fetch actual SkeletalMesh asset FIRST
		if (!Utils::GetPropertyValue<UObject*>(VanillaMesh, STR("SkeletalMesh"), defs.SkelMesh)) {
			Utils::GetPropertyValue<UObject*>(VanillaMesh, STR("SkinnedAsset"), defs.SkelMesh);
		}

		// Prefer the Skeleton assigned directly to the original SkeletalMesh
		if (defs.SkelMesh && Utils::IsObjectValid(defs.SkelMesh)) {
			Utils::GetPropertyValue<UObject*>(defs.SkelMesh, STR("Skeleton"), defs.Skeleton);
		}

		// Fallback to AnimClass's TargetSkeleton only if mesh had none
		if (!defs.Skeleton && defs.AnimClass && Utils::IsObjectValid(defs.AnimClass)) {
			Utils::GetPropertyValue<UObject*>(defs.AnimClass, STR("TargetSkeleton"), defs.Skeleton);
		}

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
    if (!SrcStaticParam || !DestCharacter || !Utils::IsObjectValid(SrcStaticParam) || !IsValidPalActor(DestCharacter)) return;

    UObject* DestStaticParam = nullptr;
    Utils::GetPropertyValue<UObject*>(DestCharacter, STR("StaticCharacterParameterComponent"), DestStaticParam);
    if (!DestStaticParam || !Utils::IsObjectValid(DestStaticParam)) return;

    // Do NOT copy WazaActionInstancedMap here; CDOs always have an empty instanced map!
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
        FProperty* SrcProp = Utils::GetProperty(SrcStaticParam, propName, true);
        FProperty* DestProp = Utils::GetProperty(DestStaticParam, propName, true);
        if (SrcProp && DestProp) {
            void* SrcPtr = SrcProp->ContainerPtrToValuePtr<void>(SrcStaticParam);
            void* DestPtr = DestProp->ContainerPtrToValuePtr<void>(DestStaticParam);
            if (SrcPtr && DestPtr) {
                DestProp->CopyCompleteValue(DestPtr, SrcPtr);
            }
        }
    }

    // Reset WazaActionInstancedMap using FProperty's container lifecycle methods
    FProperty* InstancedMapProp = Utils::GetProperty(DestStaticParam, STR("WazaActionInstancedMap"), true);
    if (InstancedMapProp) {
        InstancedMapProp->DestroyValue_InContainer(DestStaticParam);
        InstancedMapProp->InitializeValue_InContainer(DestStaticParam);
        DP_LOG(Default, "[WazaSync] Cleared WazaActionInstancedMap to purge stale action instances.");
    }

    // Re-initialize the ActionComponent to natively build and instance fresh attack classes
    UObject* ActionComp = nullptr;
    Utils::GetPropertyValue<UObject*>(DestCharacter, STR("ActionComponent"), ActionComp, true);
    if (ActionComp && Utils::IsObjectValid(ActionComp)) {
        UFunction* OnCompleteCharFunc = ActionComp->GetFunctionByNameInChain(STR("OnCompleteCharacter"));
        if (OnCompleteCharFunc) {
            struct {
                UObject* InCharacter;
            } CompParams{ DestCharacter };
            Utils::SafeProcessEvent(ActionComp, OnCompleteCharFunc, &CompParams);
        }
    }

    Utils::CallFunction(DestCharacter, STR("MasterWazaSetup"));
    DP_LOG(Default, "[WazaSync] Synced Waza maps from CDO and executed MasterWazaSetup for Pal: '{}'", DestCharacter->GetName());
}

static void ClearMaterialOverrides(UObject* MeshComp) {
	if (!MeshComp || !Utils::IsObjectValid(MeshComp)) return;
	struct {
		int32_t RetVal;
	} NumMatParams{ 0 };
	Utils::CallFunction(MeshComp, STR("GetNumMaterials"), &NumMatParams);
	for (int32_t i = 0; i < NumMatParams.RetVal; ++i) {
		struct {
			int32_t ElementIndex;
			UObject* Material;
		} ClearMatParams{ i, nullptr };
		Utils::CallFunction(MeshComp, STR("SetMaterial"), &ClearMatParams);
	}
}

struct FAnimStateCache {
        bool bIsRiding = false;
        bool bShouldBeUseRiderComponent = false;
        bool bShouldBeUseShooterComponent = false;
        bool bIsAiming = false;
        bool bIsShooting = false;
        bool bIsFlying = false;
        bool bIsFloating = false;
        bool bIsJumpPreliminary = false;
        bool bIsSkipJumpStart = false;
    };

    static void RestoreAnimInstanceCaches(UObject* Character, UObject* AnimInst, const FAnimStateCache& StateCache) {
        if (!Character || !AnimInst || !IsValidPalActor(Character) || !Utils::IsObjectValid(AnimInst)) return;

        // 1. RESTORE STATE FLAGS (CRITICAL for preserving Riding/Combat states across swaps)
        Utils::SetPropertyValue<bool>(AnimInst, STR("IsRiding"), StateCache.bIsRiding, true);
        Utils::SetPropertyValue<bool>(AnimInst, STR("ShouldBeUseRiderComponent"), StateCache.bShouldBeUseRiderComponent, true);
        Utils::SetPropertyValue<bool>(AnimInst, STR("ShouldBeUseShooterComponent"), StateCache.bShouldBeUseShooterComponent, true);
        Utils::SetPropertyValue<bool>(AnimInst, STR("IsAiming"), StateCache.bIsAiming, true);
        Utils::SetPropertyValue<bool>(AnimInst, STR("IsShooting"), StateCache.bIsShooting, true);
        Utils::SetPropertyValue<bool>(AnimInst, STR("IsFlying"), StateCache.bIsFlying, true);
        Utils::SetPropertyValue<bool>(AnimInst, STR("IsFloating"), StateCache.bIsFloating, true);
        Utils::SetPropertyValue<bool>(AnimInst, STR("IsJumpPreliminary"), StateCache.bIsJumpPreliminary, true);
        Utils::SetPropertyValue<bool>(AnimInst, STR("IsSkipJumpStart"), StateCache.bIsSkipJumpStart, true);

        // 2. Core Character Context
        Utils::SetPropertyValue<UObject*>(AnimInst, STR("TSCached_OwnerPalCharacter"), Character, true);
        Utils::SetPropertyValue<UObject*>(AnimInst, STR("TSCache_OwnerPalCharacter"), Character, true); 
        Utils::SetPropertyValue<UObject*>(AnimInst, STR("TSCached_OwnerCharacter"), Character, true);

        // 3. LookAt Component (Head Tracking)
        UObject* LookAtComp = nullptr;
        if (Utils::GetPropertyValue<UObject*>(Character, STR("LookAtComponent"), LookAtComp, true) && LookAtComp) {
            Utils::SetPropertyValue<UObject*>(AnimInst, STR("TSCached_LookAtComponent"), LookAtComp, true);
            Utils::SetPropertyValue<UObject*>(AnimInst, STR("LookAtComponent"), LookAtComp, true); 
        }

        // 4. Ride Marker Component (Mounts)
        UObject* RideMarkerComp = nullptr;
        UClass* RideMarkerClass = Utils::GetClassCached(STR("/Script/Pal.PalRideMarkerComponent"));
        if (RideMarkerClass) {
            struct { UClass* ComponentClass; UObject* ReturnValue; } GetCompParams{ RideMarkerClass, nullptr };
            Utils::CallFunction(Character, STR("GetComponentByClass"), &GetCompParams, true);
            RideMarkerComp = GetCompParams.ReturnValue;
        }
        if (RideMarkerComp) {
            Utils::SetPropertyValue<UObject*>(AnimInst, STR("TSCached_RideMarker"), RideMarkerComp, true);
        }

        // 5. Character Movement Component
        UObject* MoveComp = nullptr;
        UClass* MoveClass = Utils::GetClassCached(STR("/Script/Pal.PalCharacterMovementComponent"));
        if (MoveClass) {
            struct { UClass* ComponentClass; UObject* ReturnValue; } GetCompParams{ MoveClass, nullptr };
            Utils::CallFunction(Character, STR("GetComponentByClass"), &GetCompParams, true);
            MoveComp = GetCompParams.ReturnValue;
        }
        if (MoveComp) {
            Utils::SetPropertyValue<UObject*>(AnimInst, STR("TSCached_MovementComponent"), MoveComp, true);
        }

        // 6. FootIK Component
        UObject* FootIKComp = nullptr;
        UClass* FootIKClass = Utils::GetClassCached(STR("/Script/Pal.PalFootIKComponent"));
        if (FootIKClass) {
            struct { UClass* ComponentClass; UObject* ReturnValue; } GetCompParams{ FootIKClass, nullptr };
            Utils::CallFunction(Character, STR("GetComponentByClass"), &GetCompParams, true);
            FootIKComp = GetCompParams.ReturnValue;
        }
        if (FootIKComp) {
            Utils::SetPropertyValue<UObject*>(AnimInst, STR("TSCached_FootIkComponent"), FootIKComp, true);
        }

        // 7. Skeletal Mesh Component
        UObject* MainMesh = nullptr;
        Utils::CallFunction(Character, STR("GetMainMesh"), &MainMesh, true);
        if (MainMesh) {
            Utils::SetPropertyValue<UObject*>(AnimInst, STR("TSCached_SkeletalMeshComponent"), MainMesh, true);
        }

        // 8. Shooter Component (Weapons)
        UObject* ShooterComp = nullptr;
        Utils::GetPropertyValue<UObject*>(Character, STR("PalShooter"), ShooterComp, true);
        if (!ShooterComp || !Utils::IsObjectValid(ShooterComp)) {
            UClass* ShooterClass = Utils::GetClassCached(STR("/Script/Pal.PalShooterComponent"));
            if (ShooterClass) {
                struct { UClass* ComponentClass; UObject* ReturnValue; } GetCompParams{ ShooterClass, nullptr };
                Utils::CallFunction(Character, STR("GetComponentByClass"), &GetCompParams, true);
                ShooterComp = GetCompParams.ReturnValue;
            }
        }

        if (ShooterComp && Utils::IsObjectValid(ShooterComp)) {
            Utils::SetPropertyValue<UObject*>(AnimInst, STR("TSCached_ShooterComponent"), ShooterComp, true);
            Utils::SetPropertyValue<UObject*>(AnimInst, STR("TSCache_ShooterComponent"), ShooterComp, true);

            FProperty* DestWeaponInfoProp = Utils::GetProperty(AnimInst, STR("WeaponInfo"), true);
            FProperty* SrcWeaponInfoProp = Utils::GetProperty(ShooterComp, STR("PrevWeaponAnimationInfo"), true);
            if (DestWeaponInfoProp && SrcWeaponInfoProp) {
                void* DestPtr = DestWeaponInfoProp->ContainerPtrToValuePtr<void>(AnimInst);
                void* SrcPtr = SrcWeaponInfoProp->ContainerPtrToValuePtr<void>(ShooterComp);
                if (DestPtr && SrcPtr) {
                    DestWeaponInfoProp->CopyCompleteValue(DestPtr, SrcPtr);
                }
            }

            UFunction* InitAnimFunc = ShooterComp->GetFunctionByNameInChain(STR("OnOwnerAnimInitialized"));
            if (InitAnimFunc) {
                alignas(8) uint8_t Params[16] = {0};
                Utils::SafeProcessEvent(ShooterComp, InitAnimFunc, Params);
            }

            UFunction* UpdateFunc = AnimInst->GetFunctionByNameInChain(STR("ShooterComponentUpdate"));
            if (UpdateFunc) {
                struct { UObject* InShooter; } UpdateParams{ ShooterComp };
                Utils::SafeProcessEvent(AnimInst, UpdateFunc, &UpdateParams);
            }

            UFunction* CreateWeaponFunc = Character->GetFunctionByNameInChain(STR("CreateWeapon"));
            if (CreateWeaponFunc) {
                alignas(8) uint8_t WeaponParams[64] = {0};
                Utils::SafeProcessEvent(Character, CreateWeaponFunc, WeaponParams);
            }
        }
    }

static void EnsureMontageEndedBound(UObject* AnimInst) {
	if (!AnimInst || !Utils::IsObjectValid(AnimInst)) return;

	FProperty* Prop = Utils::GetProperty(AnimInst, STR("OnMontageEnded"));
	if (!Prop) return;

	FMulticastScriptDelegate* MulticastDelegate = Prop->ContainerPtrToValuePtr<FMulticastScriptDelegate>(AnimInst);
	if (MulticastDelegate) {
		bool bAlreadyBound = false;
		for (int32_t i = 0; i < MulticastDelegate->InvocationList.Num(); ++i) {
			if (MulticastDelegate->InvocationList[i].GetFunctionName() == FName(STR("OnMontageEndedCallback"), FNAME_Find)) {
				bAlreadyBound = true;
				break;
			}
		}

		if (!bAlreadyBound) {
			UFunction* CallbackFunc = AnimInst->GetFunctionByNameInChain(STR("OnMontageEndedCallback"));
			if (CallbackFunc) {
				FScriptDelegate Delegate;
				Delegate.BindUFunction(AnimInst, FName(STR("OnMontageEndedCallback"), FNAME_Add));
				MulticastDelegate->InvocationList.Add(Delegate);
				DP_LOG(Default, "[Montage] Manually bound OnMontageEndedCallback to AnimInstance!");
			}
		}
	}
}

static void ReLinkAnimLayers(UObject* MeshComp, UObject* TargetCDO, UObject* Character = nullptr, UObject* NewSkelMesh = nullptr, UClass* PreExistingImplClass = nullptr) {
    if (!MeshComp || !Utils::IsObjectValid(MeshComp)) return;

    UObject* AnimInst = nullptr;
    Utils::CallFunction(MeshComp, STR("GetAnimInstance"), &AnimInst);
    if (!AnimInst || !Utils::IsObjectValid(AnimInst)) return;

    UFunction* LinkFunc = MeshComp->GetFunctionByNameInChain(STR("LinkAnimClassLayers"));
    if (!LinkFunc) LinkFunc = AnimInst->GetFunctionByNameInChain(STR("LinkAnimClassLayers"));
    if (!LinkFunc) return;

    UClass* MainAnimClass = AnimInst->GetClassPrivate();
    DP_LOG(Default, "[ReLinkAnimLayers] Processing AnimInstance '{}' (Class: '{}') on Character '{}'",
        AnimInst->GetName(), MainAnimClass ? MainAnimClass->GetName() : L"None",
        Character ? Character->GetName() : L"Unknown");

    std::vector<UClass*> TargetImplClasses;

    // 1. Recover Implementation class if explicitly passed
    if (PreExistingImplClass && Utils::IsObjectValid(PreExistingImplClass)) {
        TargetImplClasses.push_back(PreExistingImplClass);
    }

    // 2. Read from AnimInstance's CDO (where default ImplementationClassList is stored)
    if (TargetImplClasses.empty() && MainAnimClass) {
        UObject* AnimCDO = MainAnimClass->GetClassDefaultObject();
        UObject* SourceObj = (AnimCDO && Utils::IsObjectValid(AnimCDO)) ? AnimCDO : AnimInst;

        FProperty* ImplListProp = Utils::GetProperty(SourceObj, STR("ImplementationClassList"), true);
        if (ImplListProp) {
            TArray<UClass*>* ImplArray = ImplListProp->ContainerPtrToValuePtr<TArray<UClass*>>(SourceObj);
            if (ImplArray && ImplArray->Num() > 0) {
                for (int32_t i = 0; i < ImplArray->Num(); ++i) {
                    UClass* ImplCls = (*ImplArray)[i];
                    if (ImplCls && Utils::IsObjectValid(ImplCls)) {
                        TargetImplClasses.push_back(ImplCls);
                    }
                }
            }
        }
    }

    // 3. Fallback: Derive and synchronously load the implementation class path
    if (TargetImplClasses.empty() && MainAnimClass && Utils::IsObjectValid(MainAnimClass)) {
        std::wstring animPath = MainAnimClass->GetPathName();
        size_t dotPos = animPath.find(L'.');
        size_t slashPos = animPath.find_last_of(L'/');
        if (slashPos != std::wstring::npos) {
            std::wstring dir = animPath.substr(0, slashPos + 1);
            std::wstring leaf = (dotPos != std::wstring::npos) ? animPath.substr(slashPos + 1, dotPos - slashPos - 1) : animPath.substr(slashPos + 1);
            if (leaf.length() > 2 && leaf.substr(leaf.length() - 2) == L"_C") {
                leaf = leaf.substr(0, leaf.length() - 2);
            }
            std::wstring candidatePath = dir + leaf + L"_Implementation." + leaf + L"_Implementation_C";
            UClass* LoadedClass = static_cast<UClass*>(Utils::LoadAssetSafely(candidatePath));
            if (LoadedClass && Utils::IsObjectValid(LoadedClass)) {
                TargetImplClasses.push_back(LoadedClass);
            }
        }
    }

    // 4. Link implementation layers
    for (UClass* ImplCls : TargetImplClasses) {
        struct { UClass* InClass; } ImplParams{ ImplCls };
        Utils::SafeProcessEvent(MeshComp, LinkFunc, &ImplParams);
        DP_LOG(Default, "[ReLinkAnimLayers] Linked implementation layer: '{}'", ImplCls->GetName());
    }

    // 5. Verification log
    FProperty* LinkedProp = Utils::GetProperty(MeshComp, STR("LinkedInstances"), true);
    if (LinkedProp) {
        TArray<UObject*>* LinkedArray = LinkedProp->ContainerPtrToValuePtr<TArray<UObject*>>(MeshComp);
        if (LinkedArray) {
            DP_LOG(Default, "[ReLinkAnimLayers] Verification: MeshComp currently has {} active linked instance(s)", LinkedArray->Num());
        }
    }

    UFunction* SetAdditiveFunc = AnimInst->GetFunctionByNameInChain(STR("SetAdditiveAnimationRate"));
    if (SetAdditiveFunc) {
        struct { FName FlagName; float Rate; } AdditiveParams{ FName(STR("UPalAnimInstance::NativeBeginPlay()"), FNAME_Add), 1.0f };
        Utils::SafeProcessEvent(AnimInst, SetAdditiveFunc, &AdditiveParams);
    }
}
static void RefreshFacialModule(UObject* Character, UObject* MeshComp, UObject* TargetCDO = nullptr, const SwapConfig* CurrentSwap = nullptr) {
	if (!IsValidPalActor(Character) || !MeshComp || !Utils::IsObjectValid(MeshComp)) return;

	// 1. Resolve Face Mesh Component
	UObject* FaceMeshComp = nullptr;
	UFunction* GetFaceMeshFunc = Character->GetFunctionByNameInChain(STR("GetOverrideFaceMesh"));
	if (GetFaceMeshFunc) {
		struct {
			UObject* ReturnValue;
		} FaceParams{ nullptr };
		Utils::SafeProcessEvent(Character, GetFaceMeshFunc, &FaceParams);
		FaceMeshComp = FaceParams.ReturnValue;
	}
	if (!FaceMeshComp || !Utils::IsObjectValid(FaceMeshComp)) {
		FaceMeshComp = MeshComp;
	}

	// 2. Resolve PalFacialComponent on Character
	UObject* FacialComp = nullptr;
	Utils::GetPropertyValue<UObject*>(Character, STR("PalFacial"), FacialComp, true);
	if (!FacialComp || !Utils::IsObjectValid(FacialComp)) {
		UClass* FacialClass = Utils::GetClassCached(STR("/Script/Pal.PalFacialComponent"));
		if (FacialClass) {
			struct {
				UClass* ComponentClass;
				UObject* ReturnValue;
			} GetCompParams{ FacialClass, nullptr };
			Utils::CallFunction(Character, STR("GetComponentByClass"), &GetCompParams);
			FacialComp = GetCompParams.ReturnValue;
		}
	}

	if (!FacialComp || !Utils::IsObjectValid(FacialComp)) {
		DP_LOG(Verbose, "[Facial] FacialComponent not found on Pal '{}'.", Character->GetName());
		return;
	}

	// 3. Resolve MainModule on FacialComp
	UObject* MainModule = nullptr;
	Utils::GetPropertyValue<UObject*>(FacialComp, STR("MainModule"), MainModule, true);
	if (!MainModule || !Utils::IsObjectValid(MainModule)) {
		DP_LOG(Warning, "[Facial] MainModule on Pal '{}' is null.", Character->GetName());
		return;
	}

	// 4. Resolve TargetCDO if not provided
	if (!TargetCDO || !Utils::IsObjectValid(TargetCDO)) {
		UClass* CharClass = Character->GetClassPrivate();
		TargetCDO = CharClass ? CharClass->GetClassDefaultObject() : nullptr;
	}

	UObject* TargetFacialComp = nullptr;
	UObject* TargetMainModule = nullptr;
	if (TargetCDO && Utils::IsObjectValid(TargetCDO)) {
		Utils::GetPropertyValue<UObject*>(TargetCDO, STR("PalFacial"), TargetFacialComp, true);
		if (TargetFacialComp && Utils::IsObjectValid(TargetFacialComp)) {
			Utils::GetPropertyValue<UObject*>(TargetFacialComp, STR("MainModule"), TargetMainModule, true);
		}
	}

	// 5. Synchronize morph & blendshape settings from TargetMainModule to MainModule
	if (TargetMainModule && TargetMainModule != MainModule && Utils::IsObjectValid(TargetMainModule)) {
		static const wchar_t* const MainModuleSyncProps[] = {
			STR("MorphSetting_Eye"),
			STR("MorphSetting_Mouth"),
			STR("BlendShape_TypeEyeWeight"),
			STR("BlendShape_TypeMouthWeight"),
			STR("BlendShape_EyeWeight"),
			STR("BlendShape_MouthWeight")
		};
		for (const wchar_t* propName : MainModuleSyncProps) {
			FProperty* SrcProp = Utils::GetProperty(TargetMainModule, propName, true);
			FProperty* DestProp = Utils::GetProperty(MainModule, propName, true);
			if (SrcProp && DestProp) {
				void* SrcPtr = SrcProp->ContainerPtrToValuePtr<void>(TargetMainModule);
				void* DestPtr = DestProp->ContainerPtrToValuePtr<void>(MainModule);
				if (SrcPtr && DestPtr) {
					DestProp->CopyCompleteValue(DestPtr, SrcPtr);
				}
			}
		}
	}

	if (TargetFacialComp && TargetFacialComp != FacialComp && Utils::IsObjectValid(TargetFacialComp)) {
		float TalkSpeed = 1.0f;
		if (Utils::GetPropertyValue<float>(TargetFacialComp, STR("NPCTalkMouthChangeSpeed"), TalkSpeed, true)) {
			Utils::SetPropertyValue<float>(FacialComp, STR("NPCTalkMouthChangeSpeed"), TalkSpeed, true);
		}
		UObject* Curve = nullptr;
		if (Utils::GetPropertyValue<UObject*>(TargetFacialComp, STR("NPCTalkMouthWeightCurve"), Curve, true)) {
			Utils::SetPropertyValue<UObject*>(FacialComp, STR("NPCTalkMouthWeightCurve"), Curve, true);
		}
	}

	// 6. Read base candidate indices
	int32 BaseEyeIndex = -1;
	int32 BaseMouthIndex = -1;
	int32 BaseBrowIndex = -1;
	bool bTargetEnableBlink = true;

	UObject* IndexSourceModule = (TargetMainModule && Utils::IsObjectValid(TargetMainModule)) ? TargetMainModule : MainModule;
	Utils::GetPropertyValue<int32>(IndexSourceModule, STR("EyeMaterialIndex"), BaseEyeIndex, true);
	Utils::GetPropertyValue<int32>(IndexSourceModule, STR("MouthMaterialIndex"), BaseMouthIndex, true);
	Utils::GetPropertyValue<int32>(IndexSourceModule, STR("BrowMaterialIndex"), BaseBrowIndex, true);

	if (TargetFacialComp && Utils::IsObjectValid(TargetFacialComp)) {
		Utils::GetPropertyValue<bool>(TargetFacialComp, STR("bIsEnableEyeBlink"), bTargetEnableBlink, true);
	} else {
		Utils::GetPropertyValue<bool>(FacialComp, STR("bIsEnableEyeBlink"), bTargetEnableBlink, true);
	}

	// 7. Query material slots on FaceMeshComp
	struct {
		int32_t RetVal;
	} NumMatParams{ 0 };
	Utils::CallFunction(FaceMeshComp, STR("GetNumMaterials"), &NumMatParams);
	int32 NumMaterials = NumMatParams.RetVal;

	if (NumMaterials <= 0) {
		Utils::SetPropertyValue<int32>(MainModule, STR("EyeMaterialIndex"), -1, true);
		Utils::SetPropertyValue<int32>(MainModule, STR("MouthMaterialIndex"), -1, true);
		Utils::SetPropertyValue<int32>(MainModule, STR("BrowMaterialIndex"), -1, true);
		Utils::SetPropertyValue<bool>(FacialComp, STR("bIsEnableEyeBlink"), false, true);
		return;
	}

	std::vector<std::wstring> SlotNames(NumMaterials);
	std::vector<std::wstring> MatNames(NumMaterials);

	UFunction* GetSlotNamesFunc = FaceMeshComp->GetFunctionByNameInChain(STR("GetMaterialSlotNames"));
	if (GetSlotNamesFunc) {
		alignas(8) uint8_t SlotParams[128] = {0};
		Utils::SafeProcessEvent(FaceMeshComp, GetSlotNamesFunc, SlotParams);
		FProperty* RetProp = GetSlotNamesFunc->GetPropertyByNameInChain(STR("ReturnValue"));
		if (RetProp) {
			TArray<FName>* Arr = RetProp->ContainerPtrToValuePtr<TArray<FName>>(SlotParams);
			if (Arr) {
				for (int32 i = 0; i < Arr->Num() && i < NumMaterials; ++i) {
					SlotNames[i] = (*Arr)[i].ToString();
				}
			}
		}
	}

	for (int32 i = 0; i < NumMaterials; ++i) {
		struct {
			int32 ElementIndex;
			UObject* ReturnValue;
		} GetMatParams{ i, nullptr };
		Utils::CallFunction(FaceMeshComp, STR("GetMaterial"), &GetMatParams);
		if (GetMatParams.ReturnValue && Utils::IsObjectValid(GetMatParams.ReturnValue)) {
			MatNames[i] = GetMatParams.ReturnValue->GetName();
		}
	}

	auto ToLowerW = [](std::wstring s) {
		std::transform(s.begin(), s.end(), s.begin(), ::towlower);
		return s;
	};

	auto ContainsAny = [](const std::wstring& haystack, const std::vector<std::wstring>& needles) {
		for (const auto& n : needles) {
			if (haystack.find(n) != std::wstring::npos) return true;
		}
		return false;
	};

	auto IsEyeSlot = [&](int32 index) -> bool {
		if (index < 0 || index >= NumMaterials) return false;
		std::wstring sName = ToLowerW(SlotNames[index]);
		std::wstring mName = ToLowerW(MatNames[index]);

		std::vector<std::wstring> negative = { L"body", L"cloth", L"armor", L"skin", L"hair", L"weapon", L"tail", L"wing", L"horn" };
		if (ContainsAny(sName, negative) && !ContainsAny(sName, { L"eye", L"pupil" })) return false;
		if (ContainsAny(mName, negative) && !ContainsAny(mName, { L"eye", L"pupil" })) return false;

		std::vector<std::wstring> eyeKeywords = { L"eye", L"pupil", L"hitomi", L"iris", L"eyeball", L"_me", L"me_" };
		return ContainsAny(sName, eyeKeywords) || ContainsAny(mName, eyeKeywords);
	};

	auto IsMouthSlot = [&](int32 index) -> bool {
		if (index < 0 || index >= NumMaterials) return false;
		std::wstring sName = ToLowerW(SlotNames[index]);
		std::wstring mName = ToLowerW(MatNames[index]);

		std::vector<std::wstring> mouthKeywords = { L"mouth", L"kuchi", L"lip", L"teeth", L"fang", L"tongue" };
		return ContainsAny(sName, mouthKeywords) || ContainsAny(mName, mouthKeywords);
	};

	auto IsBrowSlot = [&](int32 index) -> bool {
		if (index < 0 || index >= NumMaterials) return false;
		std::wstring sName = ToLowerW(SlotNames[index]);
		std::wstring mName = ToLowerW(MatNames[index]);

		std::vector<std::wstring> browKeywords = { L"brow", L"mayu", L"eyebrow" };
		return ContainsAny(sName, browKeywords) || ContainsAny(mName, browKeywords);
	};

	auto IsExplicitNonFacial = [&](int32 index) -> bool {
		if (index < 0 || index >= NumMaterials) return false;
		std::wstring sName = ToLowerW(SlotNames[index]);
		std::wstring mName = ToLowerW(MatNames[index]);
		std::vector<std::wstring> nonFacial = { L"body", L"cloth", L"armor", L"hair", L"weapon", L"tail", L"wing", L"horn", L"fur", L"skin" };
		bool hasNonFacial = ContainsAny(sName, nonFacial) || ContainsAny(mName, nonFacial);
		bool hasFacial = ContainsAny(sName, { L"eye", L"mouth", L"brow" }) || ContainsAny(mName, { L"eye", L"mouth", L"brow" });
		return hasNonFacial && !hasFacial;
	};

	// 8. Align Real Eye Index
	int32 ResolvedEyeIndex = -1;
	if (BaseEyeIndex >= 0 && BaseEyeIndex < NumMaterials) {
		if (IsEyeSlot(BaseEyeIndex)) {
			ResolvedEyeIndex = BaseEyeIndex;
		} else if (IsExplicitNonFacial(BaseEyeIndex)) {
			ResolvedEyeIndex = -1;
		} else {
			ResolvedEyeIndex = BaseEyeIndex;
		}
	}

	if (ResolvedEyeIndex == -1) {
		for (int32 i = 0; i < NumMaterials; ++i) {
			if (IsEyeSlot(i)) {
				ResolvedEyeIndex = i;
				DP_LOG(Default, "[Facial] Realigned eye slot index for Pal '{}': {} ('{}' / '{}')",
				       Character->GetName(), i, SlotNames[i], MatNames[i]);
				break;
			}
		}
	}

	// 9. Align Mouth & Brow Indices
	int32 ResolvedMouthIndex = -1;
	if (BaseMouthIndex >= 0 && BaseMouthIndex < NumMaterials) {
		if (IsMouthSlot(BaseMouthIndex)) {
			ResolvedMouthIndex = BaseMouthIndex;
		} else if (IsExplicitNonFacial(BaseMouthIndex)) {
			ResolvedMouthIndex = -1;
		} else {
			ResolvedMouthIndex = BaseMouthIndex;
		}
	}
	if (ResolvedMouthIndex == -1) {
		for (int32 i = 0; i < NumMaterials; ++i) {
			if (i != ResolvedEyeIndex && IsMouthSlot(i)) {
				ResolvedMouthIndex = i;
				break;
			}
		}
	}

	int32 ResolvedBrowIndex = -1;
	if (BaseBrowIndex >= 0 && BaseBrowIndex < NumMaterials) {
		if (IsBrowSlot(BaseBrowIndex)) {
			ResolvedBrowIndex = BaseBrowIndex;
		} else if (IsExplicitNonFacial(BaseBrowIndex)) {
			ResolvedBrowIndex = -1;
		} else {
			ResolvedBrowIndex = BaseBrowIndex;
		}
	}
	if (ResolvedBrowIndex == -1) {
		for (int32 i = 0; i < NumMaterials; ++i) {
			if (i != ResolvedEyeIndex && i != ResolvedMouthIndex && IsBrowSlot(i)) {
				ResolvedBrowIndex = i;
				break;
			}
		}
	}

	// 10. Check if MatReplace overrides the eye slot with a non-blinking material
	bool bEyeReplacedWithNonBlink = false;
	if (CurrentSwap && ResolvedEyeIndex >= 0) {
		std::string eyeIndexStr = std::to_string(ResolvedEyeIndex);
		for (const auto& matRep : CurrentSwap->MatReplaceList) {
			if (matRep.index == eyeIndexStr) {
				std::wstring repPath = ToLowerW(matRep.matPath);
				bool bRepIsEye = ContainsAny(repPath, { L"eye", L"pupil", L"hitomi", L"iris" });
				if (!bRepIsEye) {
					bEyeReplacedWithNonBlink = true;
					DP_LOG(Default, "[Facial] Eye slot {} overridden by non-eye material '{}'. Disabling blink.", ResolvedEyeIndex, matRep.matPath);
					break;
				}
			}
		}
	}
	if (bEyeReplacedWithNonBlink) {
		ResolvedEyeIndex = -1;
	}

	// 11. Check JSON Extra flags for explicit blink disabling
	bool bDisableBlinkConfig = false;
	if (CurrentSwap && !CurrentSwap->Extra.empty() && CurrentSwap->Extra != L"{}") {
		std::wstring lowerExtra = ToLowerW(CurrentSwap->Extra);
		if (lowerExtra.find(L"disableblink") != std::wstring::npos ||
		        lowerExtra.find(L"disable_blink") != std::wstring::npos ||
		        lowerExtra.find(L"disablefacial") != std::wstring::npos) {
			bDisableBlinkConfig = true;
		}
	}
	if (bDisableBlinkConfig) {
		ResolvedEyeIndex = -1;
		ResolvedMouthIndex = -1;
		ResolvedBrowIndex = -1;
	}

	bool bShouldEnableBlink = (ResolvedEyeIndex >= 0) && bTargetEnableBlink && !bEyeReplacedWithNonBlink && !bDisableBlinkConfig;

	// 12. CLEANUP / DE-SETUP OF DYNAMIC MATERIALS ON NON-FACIAL SLOTS
	for (int32 i = 0; i < NumMaterials; ++i) {
		bool bIsActiveFacialSlot = (bShouldEnableBlink && i == ResolvedEyeIndex) ||
		                           (ResolvedMouthIndex >= 0 && i == ResolvedMouthIndex) ||
		                           (ResolvedBrowIndex >= 0 && i == ResolvedBrowIndex);

		if (!bIsActiveFacialSlot) {
			struct {
				int32 ElementIndex;
				UObject* ReturnValue;
			} MatCheck{ i, nullptr };
			Utils::CallFunction(FaceMeshComp, STR("GetMaterial"), &MatCheck);
			UObject* CurMat = MatCheck.ReturnValue;

			if (CurMat && Utils::IsObjectValid(CurMat)) {
				std::wstring matClassName = CurMat->GetClassPrivate()->GetName();
				if (matClassName.find(L"MaterialInstanceDynamic") != std::wstring::npos) {
					bool bIsRandomHueSlot = false;
					if (CurrentSwap) {
						std::string idxStr = std::to_string(i);
						for (const auto& mr : CurrentSwap->MatReplaceList) {
							if (mr.index == idxStr && mr.bRandomHue) {
								bIsRandomHueSlot = true;
								break;
							}
						}
					}

					if (!bIsRandomHueSlot) {
						bool bRestored = false;
						if (CurrentSwap) {
							std::string idxStr = std::to_string(i);
							for (const auto& mr : CurrentSwap->MatReplaceList) {
								if (mr.index == idxStr && !mr.matPath.empty() && !mr.bRandomHue) {
									UObject* BaseMat = Utils::LoadAssetSafely(mr.matPath);
									if (BaseMat && Utils::IsObjectValid(BaseMat)) {
										struct {
											int32 ElementIndex;
											UObject* Material;
										} SetMatParams{ i, BaseMat };
										Utils::CallFunction(FaceMeshComp, STR("SetMaterial"), &SetMatParams);
										bRestored = true;
									}
									break;
								}
							}
						}

						if (!bRestored) {
							struct {
								int32 ElementIndex;
								UObject* Material;
							} ClearMatParams{ i, nullptr };
							Utils::CallFunction(FaceMeshComp, STR("SetMaterial"), &ClearMatParams);
						}
						DP_LOG(Default, "[Facial] De-setup slot {}: Cleared active dynamic material.", i);
					}
				}
			}
		}
	}

	// 13. Update MainModule slot indices & clear cached dynamic material pointers
	Utils::SetPropertyValue<int32>(MainModule, STR("EyeMaterialIndex"), ResolvedEyeIndex, true);
	Utils::SetPropertyValue<int32>(MainModule, STR("MouthMaterialIndex"), ResolvedMouthIndex, true);
	Utils::SetPropertyValue<int32>(MainModule, STR("BrowMaterialIndex"), ResolvedBrowIndex, true);

	for (FProperty* Prop = (FProperty*)MainModule->GetClassPrivate()->GetChildProperties(); Prop; Prop = (FProperty*)Utils::GetNextField(Prop)) {
		if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop)) {
			std::wstring propName = Prop->GetName();
			if (propName.find(L"Material") != std::wstring::npos || propName.find(L"MID") != std::wstring::npos || propName.find(L"Instance") != std::wstring::npos) {
				void* ContainerPtr = ObjProp->ContainerPtrToValuePtr<void>(MainModule);
				if (ContainerPtr) {
					UObject* ExistingObj = *reinterpret_cast<UObject**>(ContainerPtr);
					if (ExistingObj && Utils::IsObjectValid(ExistingObj)) {
						if (ExistingObj->GetClassPrivate()->GetName().find(L"MaterialInstanceDynamic") != std::wstring::npos) {
							*reinterpret_cast<UObject**>(ContainerPtr) = nullptr;
						}
					}
				}
			}
		}
	}

	// 14. Update FacialComponent blink and notify state
	Utils::SetPropertyValue<bool>(FacialComp, STR("bIsEnableEyeBlink"), bShouldEnableBlink, true);
	struct {
		bool Disable;
	} DisableNotifyParams{ !bShouldEnableBlink };
	Utils::CallFunction(FacialComp, STR("SetDisableNotify"), &DisableNotifyParams);

	if (!bShouldEnableBlink) {
		Utils::CallFunction(FacialComp, STR("StopNPCTalkMouth"));
		Utils::CallFunction(FacialComp, STR("ChangeDefaultFacial"));
		DP_LOG(Default, "[Facial] Pal '{}' eye blink disabled (ResolvedEyeIndex: {}).", Character->GetName(), ResolvedEyeIndex);
	} else {
		UFunction* SetupFunc = MainModule->GetFunctionByNameInChain(STR("Setup_FacialModule"));
		if (SetupFunc) {
			struct {
				UObject* SkeletalMeshComponent;
			} SetupParams{ FaceMeshComp };
			Utils::SafeProcessEvent(MainModule, SetupFunc, &SetupParams);
			DP_LOG(Default, "[Facial] Executed Setup_FacialModule for Pal '{}' on slot {} (FaceMesh: '{}').",
			       Character->GetName(), ResolvedEyeIndex, FaceMeshComp->GetName());
		}

		UFunction* SetUpTestMeshFunc = FacialComp->GetFunctionByNameInChain(STR("SetUpTestMesh"));
		if (SetUpTestMeshFunc) {
			struct {
				UObject* SkeletalMeshComponent;
			} TestParams{ FaceMeshComp };
			Utils::SafeProcessEvent(FacialComp, SetUpTestMeshFunc, &TestParams);
		}

		Utils::CallFunction(FacialComp, STR("ChangeDefaultFacial"));
		struct {
			uint8_t Eye;
		} EyeParam{ 1 }; // EPalFacialEyeType::Default
		Utils::CallFunction(FacialComp, STR("ChangeEyeAndMouthMesh"), &EyeParam);
	}
}

// =========================================================================
// DYNAMICS & KAWAII PHYSICS RESET PIPELINE
// =========================================================================
static void ResetPhysicsAndDynamics(UObject* MeshComp) {
	if (!MeshComp || !Utils::IsObjectValid(MeshComp)) return;

	UObject* AnimInst = nullptr;
	Utils::CallFunction(MeshComp, STR("GetAnimInstance"), &AnimInst);
	if (AnimInst && Utils::IsObjectValid(AnimInst)) {
		struct {
			uint8_t InTeleportType;
		} ResetParams{ 1 };
		UFunction* ResetFunc = AnimInst->GetFunctionByNameInChain(STR("ResetDynamics"));
		if (ResetFunc) Utils::SafeProcessEvent(AnimInst, ResetFunc, &ResetParams);
	}

	UObject* PostProcessInst = nullptr;
	Utils::CallFunction(MeshComp, STR("GetPostProcessInstance"), &PostProcessInst);
	if (PostProcessInst && Utils::IsObjectValid(PostProcessInst)) {
		struct {
			uint8_t InTeleportType;
		} ResetParams{ 1 };
		UFunction* ResetFunc = PostProcessInst->GetFunctionByNameInChain(STR("ResetDynamics"));
		if (ResetFunc) Utils::SafeProcessEvent(PostProcessInst, ResetFunc, &ResetParams);
	}

	FProperty* LinkedProp = Utils::GetProperty(MeshComp, STR("LinkedInstances"), true);
	if (LinkedProp) {
		TArray<UObject*>* LinkedArray = LinkedProp->ContainerPtrToValuePtr<TArray<UObject*>>(MeshComp);
		if (LinkedArray) {
			for (int32_t i = 0; i < LinkedArray->Num(); ++i) {
				UObject* LinkedInst = (*LinkedArray)[i];
				if (LinkedInst && Utils::IsObjectValid(LinkedInst)) {
					struct {
						uint8_t InTeleportType;
					} ResetParams{ 1 };
					UFunction* ResetFunc = LinkedInst->GetFunctionByNameInChain(STR("ResetDynamics"));
					if (ResetFunc) {
						Utils::SafeProcessEvent(LinkedInst, ResetFunc, &ResetParams);
						DP_LOG(Default, "[Physics] Invoked ResetDynamics on LinkedInstance: '{}'", LinkedInst->GetName());
					}
				}
			}
		}
	}

	UFunction* EvalRateFunc = MeshComp->GetFunctionByNameInChain(STR("SetEvaluationRate"));
	if (EvalRateFunc) {
		struct {
			float InRate;
			bool bResetCurrentInterval;
		} RateParams{ 0.0f, true };
		Utils::SafeProcessEvent(MeshComp, EvalRateFunc, &RateParams);
	}
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

	struct {
		UObject* WorldContext;
		UObject* DB;
	} GetDBParams{ WorldContext, nullptr };
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

	struct {
		FName RowName;
		bool bShowError;
		uint8_t Pad[7];
		AltrSoftObjectPtr ReturnValue;
	} Params;
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
	if (!IsValidPalActor(Pal)) return false;

	UClass* PalClass = Pal->GetClassPrivate();
	OutBlueprintName = PalClass->GetName();

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

static bool IEquals(std::wstring_view a, std::wstring_view b) {
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); ++i) {
		if (std::towlower(a[i]) != std::towlower(b[i])) return false;
	}
	return true;
}

std::wstring PalProcessor::StripCharacterPrefix(const std::wstring& InputID) {
	std::wstring result = InputID;

	if      (StartsWithIgnoreCase(result, L"MiddleBoss_")) result = result.substr(11);
	else if (StartsWithIgnoreCase(result, L"BOSS_"))       result = result.substr(5);
	else if (StartsWithIgnoreCase(result, L"RAID_"))       result = result.substr(5);
	else if (StartsWithIgnoreCase(result, L"GYM_"))        result = result.substr(4);
	else if (StartsWithIgnoreCase(result, L"PREDATOR_"))   result = result.substr(9);

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
	if (!IsValidPalActor(Character)) return result;

	result.push_back(Character);

	UClass* FunnelClass = Utils::GetClassCached(STR("/Script/Pal.PalFunnelCharacter"));
	bool bIsFunnel = FunnelClass && Character->GetClassPrivate()->IsChildOf(FunnelClass);

	if (bIsFunnel) {
		UFunction* GetOwnerFunc = Character->GetFunctionByNameInChain(STR("GetOwnerPal"));
		if (GetOwnerFunc) {
			struct {
				UObject* RetVal;
			} Params{ nullptr };
			Utils::SafeProcessEvent(Character, GetOwnerFunc, &Params);
			if (IsValidPalActor(Params.RetVal)) {
				result.push_back(Params.RetVal);
			}
		}
	}
	else {
		UObject* PalUtil = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
		if (PalUtil && Utils::IsObjectValid(PalUtil)) {
			UFunction* IsOtomoFunc = PalUtil->GetFunctionByNameInChain(STR("IsPlayersOtomo"));
			if (IsOtomoFunc) {
				struct {
					UObject* Actor;
					bool RetVal;
				} OtomoParams{ Character, false };
				Utils::SafeProcessEvent(PalUtil, IsOtomoFunc, &OtomoParams);

				if (OtomoParams.RetVal) {
					struct {
						UObject* WorldContext;
						UObject* RetVal;
					} FMgrParams{ Character, nullptr };
					Utils::CallFunction(PalUtil, STR("GetFunnelCharacterManager"), &FMgrParams);

					if (FMgrParams.RetVal && Utils::IsObjectValid(FMgrParams.RetVal)) {
						struct {
							UObject* Owner;
							UObject* RetVal;
						} FunnelParams{ Character, nullptr };
						Utils::SafeProcessEvent(FMgrParams.RetVal, FMgrParams.RetVal->GetFunctionByNameInChain(STR("GetFunnelCharacterByOwner")), &FunnelParams);
						if (IsValidPalActor(FunnelParams.RetVal)) {
							result.push_back(FunnelParams.RetVal);
						}
					}
				}
			}
		}
	}

	return result;
}

// =========================================================================
// DELAYED / ASYNC THREADED EXECUTION HELPERS
// =========================================================================
static void ScheduleProcessPal(UObject* Character, int DelayMs, bool ForceReroll, int ExplicitSwapIndex = -1, bool IsEvolutionEnd = false) {
	std::thread([Character, DelayMs, ForceReroll, ExplicitSwapIndex, IsEvolutionEnd]() {
		if (DelayMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(DelayMs));
		AsyncHelper::AsyncTask(ENamedThreads::GameThread, [Character, ForceReroll, ExplicitSwapIndex, IsEvolutionEnd]() {
			if (!IsValidPalActor(Character)) return;
			PalProcessor::Get().ProcessPal(Character, ForceReroll, ExplicitSwapIndex, false, IsEvolutionEnd);
		});
	}).detach();
}

void PalProcessor::DelayedSwap(UObject* Character, int SwapIndex, const std::wstring& CompName) {
	if (!IsValidPalActor(Character)) return;

	float DelaySeconds = VFXManager::Get().PlayComposition(Character, CompName);
	int DelayMs = static_cast<int>(DelaySeconds * 1000.0f);

	ScheduleProcessPal(Character, DelayMs, false, SwapIndex, true);
}

void PalProcessor::DelayedReroll(UObject* Character, const std::wstring& CompName) {
	if (!IsValidPalActor(Character)) return;

	float DelaySeconds = VFXManager::Get().PlayComposition(Character, CompName);
	int DelayMs = static_cast<int>(DelaySeconds * 1000.0f);

	ScheduleProcessPal(Character, DelayMs, true, -1, false);
}

void PalProcessor::ForceSwap(UObject* Character, int SwapIndex, int DelayMs) {
	if (!IsValidPalActor(Character) || SwapIndex < 0 || SwapIndex >= (int)ConfigManager::Get().GetConfigs().size()) return;

	FPalIdentity id = ResolvePalIdentity(Character);
	if (!id.bIsValid) return;

	ClearSwappedStatus(id.InstanceID, Character);
	ScheduleProcessPal(Character, DelayMs, false, SwapIndex, false);
}

int PalProcessor::EvaluateIdealSwapIndex(UObject* Character, std::wstring& OutInstanceID) {
	return -1;
}

void PalProcessor::ProcessPal(UObject* Character, bool ForceReroll, int ExplicitSwapIndex, bool IsCompanionSync, bool IsEvolutionEnd) {
	if (!IsValidPalActor(Character)) return;

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

	struct {
		UObject* WorldContext;
		UObject* ReturnValue;
	} HolderParams{ WorldContext, nullptr };
	Utils::SafeProcessEvent(PalUtil, PalUtil->GetFunctionByNameInChain(STR("GetOtomoHolderComponent")), &HolderParams);
	UObject* OtomoHolder = HolderParams.ReturnValue;

	struct {
		UObject* WorldContext;
		UObject* ReturnValue;
	} FunnelMgrParams{ WorldContext, nullptr };
	Utils::SafeProcessEvent(PalUtil, PalUtil->GetFunctionByNameInChain(STR("GetFunnelCharacterManager")), &FunnelMgrParams);
	UObject* FunnelManager = FunnelMgrParams.ReturnValue;

	if (OtomoHolder && Utils::IsObjectValid(OtomoHolder)) {
		for (int32_t i = 0; i < 5; ++i) {
			struct {
				int32_t SlotIndex;
				UObject* ReturnValue;
			} OtomoParams{ i, nullptr };
			Utils::SafeProcessEvent(OtomoHolder, OtomoHolder->GetFunctionByNameInChain(STR("TryGetOtomoActorBySlotIndex")), &OtomoParams);
			UObject* OtomoChar = OtomoParams.ReturnValue;

			if (IsValidPalActor(OtomoChar)) {
				ProcessPal(OtomoChar, false);

				if (FunnelManager && Utils::IsObjectValid(FunnelManager)) {
					struct {
						UObject* Owner;
						UObject* ReturnValue;
					} FunnelParams{ OtomoChar, nullptr };
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

bool PalProcessor::OnUObjectDeleted(RC::Unreal::UObject* Obj) {
	std::lock_guard<std::mutex> lock(QueueMutex);
	bool bFound = false;

	if (SwappedInstances.erase(Obj) > 0) bFound = true;
	if (ProcessedPals.erase(Obj) > 0) bFound = true;

	auto itSwap = std::remove_if(SwapQueue.begin(), SwapQueue.end(),
	[Obj](const QueuedSwap& q) {
		return q.Character == Obj;
	});
	if (itSwap != SwapQueue.end()) {
		SwapQueue.erase(itSwap, SwapQueue.end());
		bFound = true;
	}

	auto itProc = std::remove_if(ProcessingQueue.begin(), ProcessingQueue.end(),
	[Obj](const QueuedPal& q) {
		return q.Character == Obj;
	});
	if (itProc != ProcessingQueue.end()) {
		ProcessingQueue.erase(itProc, ProcessingQueue.end());
		bFound = true;
	}

	return bFound;
}

void PalProcessor::Tick() {
	std::vector<QueuedSwap> pendingSwaps;
	{
		std::lock_guard<std::mutex> lock(QueueMutex);
		if (SwapQueue.empty()) return;

		pendingSwaps.assign(SwapQueue.begin(), SwapQueue.end());
		SwapQueue.clear();
	}

	int ProcessedThisFrame = 0;

	for (const auto& req : pendingSwaps) {
		UObject* TargetChar = req.Character;

		if (IsValidPalActor(TargetChar)) {
			NativeAsyncLoader::SetActiveRequester(TargetChar);

			ExecuteSwap(TargetChar, req.ForceReroll, req.ExplicitSwapIndex, req.IsCompanionSync, req.IsEvolutionEnd);

			NativeAsyncLoader::ClearTemporaryPointers(TargetChar);
			NativeAsyncLoader::SetActiveRequester(nullptr);

			ProcessedThisFrame++;
			if (ProcessedThisFrame >= 2) break;
		} else {
			ProcessedThisFrame++;
		}
	}

	if (ProcessedThisFrame < pendingSwaps.size()) {
		std::lock_guard<std::mutex> lock(QueueMutex);
		SwapQueue.insert(SwapQueue.begin(), pendingSwaps.begin() + ProcessedThisFrame, pendingSwaps.end());
	}
}

// =========================================================================
// MODULAR PIPELINE HELPER FUNCTIONS
// =========================================================================
struct FMeshApplyParams {
	UObject* MeshComp = nullptr;
	UObject* NewSkelMesh = nullptr;
	UObject* TargetSkeleton = nullptr;
	UClass* TargetAnimClass = nullptr;
	UObject* TargetCDO = nullptr;
	UObject* Character = nullptr;
	UObject* TargetStaticParam = nullptr;
	bool bReinitPose = false;
};

static void FlushStuckActions(UObject* Character) {
        if (!Character || !Utils::IsObjectValid(Character)) return;

        // 1. Cancel active action on ActionComponent
        UObject* ActionComp = nullptr;
        Utils::GetPropertyValue<UObject*>(Character, STR("ActionComponent"), ActionComp, true);
        if (ActionComp && Utils::IsObjectValid(ActionComp)) {
            Utils::CallFunction(ActionComp, STR("CancelAllAction"));
            DP_LOG(Default, "[Flush] Cancelled all actions on ActionComponent.");
        }

        // 2. Resolve Movement Component (Property name is CharacterMovement, subobject name is CharMoveComp)
        UObject* MoveComp = nullptr;
        if (!Utils::GetPropertyValue<UObject*>(Character, STR("CharacterMovement"), MoveComp, true) || !MoveComp) {
            Utils::GetPropertyValue<UObject*>(Character, STR("CharMoveComp"), MoveComp, true);
        }

        if (MoveComp && Utils::IsObjectValid(MoveComp)) {
            Utils::CallFunction(MoveComp, STR("StopMovementImmediately"));

            // Clear input and movement disable flags left behind by the stalled skill
            UFunction* SetDisableInputFunc = MoveComp->GetFunctionByNameInChain(STR("SetDisableInput"));
            if (SetDisableInputFunc) {
                struct { FName FlagName; bool bDisable; } P1{ FName(STR("Default"), FNAME_Add), false };
                struct { FName FlagName; bool bDisable; } P2{ FName(STR("action"), FNAME_Add), false };
                Utils::SafeProcessEvent(MoveComp, SetDisableInputFunc, &P1);
                Utils::SafeProcessEvent(MoveComp, SetDisableInputFunc, &P2);
            }

            UFunction* SetDisableMoveFunc = MoveComp->GetFunctionByNameInChain(STR("SetDisableMove"));
            if (SetDisableMoveFunc) {
                struct { FName FlagName; bool bDisable; } P1{ FName(STR("action"), FNAME_Add), false };
                Utils::SafeProcessEvent(MoveComp, SetDisableMoveFunc, &P1);
            }

            DP_LOG(Default, "[Flush] Cleared input/movement disable flags and stopped movement on CharacterMovement.");
        }
    }



    static void VerificationLogger(UObject* Character, UObject* MeshComp) {
        if (!Character || !Utils::IsObjectValid(Character)) return;
        DP_LOG(Default, "--- EXHAUSTIVE DUMP FOR {} ---", Character->GetName());

        auto DumpProperties = [&](UObject* Obj, const std::wstring& Prefix) {
            if (!Obj || !Utils::IsObjectValid(Obj)) {
                DP_LOG(Default, "  {} is NULL or Invalid.", Prefix);
                return;
            }
            UClass* Cls = Obj->GetClassPrivate();
            if (!Cls) return;

            
        };

        UObject* AnimInst = nullptr;
        if (MeshComp && Utils::IsObjectValid(MeshComp)) {
            Utils::CallFunction(MeshComp, STR("GetAnimInstance"), &AnimInst);
        }

        UObject* MoveComp = nullptr;
        if (!Utils::GetPropertyValue<UObject*>(Character, STR("CharacterMovement"), MoveComp, true) || !MoveComp) {
            Utils::GetPropertyValue<UObject*>(Character, STR("CharMoveComp"), MoveComp, true);
        }

        UObject* ActionComp = nullptr;
        Utils::GetPropertyValue(Character, STR("ActionComponent"), ActionComp, true);

        UObject* CurrentAction = nullptr;
        if (ActionComp && Utils::IsObjectValid(ActionComp)) {
            Utils::GetPropertyValue(ActionComp, STR("CurrentAction"), CurrentAction, true);
        }

        UObject* AnimNotifyComp = nullptr;
        Utils::GetPropertyValue(Character, STR("AnimNotifyComponent"), AnimNotifyComp, true);

        UObject* RideMarkerComp = nullptr;
        UClass* RideMarkerClass = Utils::GetClassCached(STR("/Script/Pal.PalRideMarkerComponent"));
        if (RideMarkerClass) {
            struct { UClass* ComponentClass; UObject* ReturnValue; } GetCompParams{ RideMarkerClass, nullptr };
            Utils::CallFunction(Character, STR("GetComponentByClass"), &GetCompParams, true);
            RideMarkerComp = GetCompParams.ReturnValue;
        }

        DumpProperties(CurrentAction, L"CurrentAction");
        DumpProperties(AnimNotifyComp, L"AnimNotifyComponent");
        DumpProperties(RideMarkerComp, L"RideMarkerComponent");
        DumpProperties(AnimInst, L"AnimInstance");
        DumpProperties(ActionComp, L"ActionComponent");
        DumpProperties(MoveComp, L"CharacterMovementComponent");
        DumpProperties(MeshComp, L"SkeletalMeshComponent");
        DumpProperties(Character, L"PalCharacter");

        DP_LOG(Default, "--------------------------------");
    }

// =========================================================================
// PERSISTENCE VERIFICATION LOGGER
// =========================================================================
static bool VerifyAssetPersistence(UObject* Character, UObject* MeshComp, UObject* ExpectedMesh, UClass* ExpectedAnimClass) {
	if (!Character || !MeshComp || !Utils::IsObjectValid(MeshComp)) return false;

	bool bAllPersisted = true;
	std::wstring palName = Character->GetName();

	// 1. Verify Skeletal Mesh
	if (ExpectedMesh && Utils::IsObjectValid(ExpectedMesh)) {
		UObject* ActualMesh = nullptr;
		if (!Utils::GetPropertyValue<UObject*>(MeshComp, STR("SkeletalMesh"), ActualMesh)) {
			Utils::GetPropertyValue<UObject*>(MeshComp, STR("SkinnedAsset"), ActualMesh);
		}

		if (ActualMesh != ExpectedMesh) {
			bAllPersisted = false;
			std::wstring expectedName = ExpectedMesh->GetName();
			std::wstring actualName = ActualMesh ? ActualMesh->GetName() : L"None";
			DP_LOG(Error, "[Persistence Check] FAILED: SkeletalMesh did not persist on Pal '{}'! Expected: '{}', Actual: '{}'",
			       palName, expectedName, actualName);
		}
	}

	// 2. Verify AnimClass
	if (ExpectedAnimClass && Utils::IsObjectValid(ExpectedAnimClass)) {
		UClass* ActualAnimClass = nullptr;
		Utils::GetPropertyValue<UClass*>(MeshComp, STR("AnimClass"), ActualAnimClass);

		if (ActualAnimClass != ExpectedAnimClass) {
			bAllPersisted = false;
			std::wstring expectedName = ExpectedAnimClass->GetName();
			std::wstring actualName = ActualAnimClass ? ActualAnimClass->GetName() : L"None";
			DP_LOG(Error, "[Persistence Check] FAILED: AnimClass did not persist on Pal '{}'! Expected: '{}', Actual: '{}'",
			       palName, expectedName, actualName);
		}
	}

	if (bAllPersisted) {
		DP_LOG(Verbose, "[Persistence Check] SUCCESS: All asset modifications verified and persisted on Pal '{}'.", palName);
	}

	return bAllPersisted;
}

static void BindNativeMontageNotifies(UObject* AnimInst) {
    if (!AnimInst || !Utils::IsObjectValid(AnimInst)) return;

    // Direct memory offsets verified from UPalAnimInstance::NativeBeginPlay (FUN_142d41ad0):
    // this + 0x328 : OnPlayMontageNotifyBegin (FMulticastScriptDelegate)
    // this + 0x338 : OnPlayMontageNotifyEnd   (FMulticastScriptDelegate)
    auto* pNotifyBegin = reinterpret_cast<FMulticastScriptDelegate*>(
        reinterpret_cast<uint8_t*>(AnimInst) + 0x328
    );
    auto* pNotifyEnd = reinterpret_cast<FMulticastScriptDelegate*>(
        reinterpret_cast<uint8_t*>(AnimInst) + 0x338
    );

    // 1. Bind OnNotifyBeginReceived to this + 0x328
    UFunction* BeginFunc = AnimInst->GetFunctionByNameInChain(STR("OnNotifyBeginReceived"));
    if (BeginFunc && pNotifyBegin) {
        FName BeginFName(STR("OnNotifyBeginReceived"), FNAME_Find);
        bool bAlreadyBound = false;
        for (int32_t i = 0; i < pNotifyBegin->InvocationList.Num(); ++i) {
            if (pNotifyBegin->InvocationList[i].GetFunctionName() == BeginFName) {
                bAlreadyBound = true;
                break;
            }
        }
        if (!bAlreadyBound) {
            FScriptDelegate Delegate;
            Delegate.BindUFunction(AnimInst, FName(STR("OnNotifyBeginReceived"), FNAME_Add));
            pNotifyBegin->InvocationList.Add(Delegate);
            DP_LOG(Default, "[AnimInit] Successfully bound OnPlayMontageNotifyBegin (offset 0x328) -> OnNotifyBeginReceived!");
        }
    }

    // 2. Bind OnNotifyEndReceived to this + 0x338
    UFunction* EndFunc = AnimInst->GetFunctionByNameInChain(STR("OnNotifyEndReceived"));
    if (EndFunc && pNotifyEnd) {
        FName EndFName(STR("OnNotifyEndReceived"), FNAME_Find);
        bool bAlreadyBound = false;
        for (int32_t i = 0; i < pNotifyEnd->InvocationList.Num(); ++i) {
            if (pNotifyEnd->InvocationList[i].GetFunctionName() == EndFName) {
                bAlreadyBound = true;
                break;
            }
        }
        if (!bAlreadyBound) {
            FScriptDelegate Delegate;
            Delegate.BindUFunction(AnimInst, FName(STR("OnNotifyEndReceived"), FNAME_Add));
            pNotifyEnd->InvocationList.Add(Delegate);
            DP_LOG(Default, "[AnimInit] Successfully bound OnPlayMontageNotifyEnd (offset 0x338) -> OnNotifyEndReceived!");
        }
    }

    // 3. Ensure OnMontageEnded is bound to OnMontageEndedCallback (reflected property)
    FProperty* EndedProp = Utils::GetProperty(AnimInst, STR("OnMontageEnded"), true);
    UFunction* EndedCallbackFunc = AnimInst->GetFunctionByNameInChain(STR("OnMontageEndedCallback"));
    if (EndedProp && EndedCallbackFunc) {
        auto* pEnded = EndedProp->ContainerPtrToValuePtr<FMulticastScriptDelegate>(AnimInst);
        if (pEnded) {
            FName EndedFName(STR("OnMontageEndedCallback"), FNAME_Find);
            bool bAlreadyBound = false;
            for (int32_t i = 0; i < pEnded->InvocationList.Num(); ++i) {
                if (pEnded->InvocationList[i].GetFunctionName() == EndedFName) {
                    bAlreadyBound = true;
                    break;
                }
            }
            if (!bAlreadyBound) {
                FScriptDelegate Delegate;
                Delegate.BindUFunction(AnimInst, FName(STR("OnMontageEndedCallback"), FNAME_Add));
                pEnded->InvocationList.Add(Delegate);
                DP_LOG(Default, "[AnimInit] Successfully bound OnMontageEnded -> OnMontageEndedCallback!");
            }
        }
    }
}

static void ApplyMeshAndAnim(const FMeshApplyParams& Params, bool bNeedsAnimRebuild) {
    if (!Params.MeshComp || !Utils::IsObjectValid(Params.MeshComp)) return;

    // 1. Flush active actions and clear mounted skill slot before teardown
    FlushStuckActions(Params.Character);

    UObject* RideMarkerComp = nullptr;
    UClass* RideMarkerClass = Utils::GetClassCached(STR("/Script/Pal.PalRideMarkerComponent"));
    if (RideMarkerClass && Params.Character) {
        struct { UClass* ComponentClass; UObject* ReturnValue; } GetCompParams{ RideMarkerClass, nullptr };
        Utils::CallFunction(Params.Character, STR("GetComponentByClass"), &GetCompParams, true);
        RideMarkerComp = GetCompParams.ReturnValue;
        if (RideMarkerComp && Utils::IsObjectValid(RideMarkerComp)) {
            Utils::SetPropertyValue<UObject*>(RideMarkerComp, STR("SkillSlot"), nullptr, true);
        }
    }

    // 2. Extract state cache
    FAnimStateCache StateCache;
    UObject* OldAnimInst = nullptr;
    Utils::CallFunction(Params.MeshComp, STR("GetAnimInstance"), &OldAnimInst);
    
    UClass* CapturedImplClass = nullptr;
    if (OldAnimInst && Utils::IsObjectValid(OldAnimInst)) {
        Utils::GetPropertyValue<bool>(OldAnimInst, STR("IsRiding"), StateCache.bIsRiding, true);
        Utils::GetPropertyValue<bool>(OldAnimInst, STR("ShouldBeUseRiderComponent"), StateCache.bShouldBeUseRiderComponent, true);
        Utils::GetPropertyValue<bool>(OldAnimInst, STR("ShouldBeUseShooterComponent"), StateCache.bShouldBeUseShooterComponent, true);
        Utils::GetPropertyValue<bool>(OldAnimInst, STR("IsAiming"), StateCache.bIsAiming, true);
        Utils::GetPropertyValue<bool>(OldAnimInst, STR("IsShooting"), StateCache.bIsShooting, true);
        Utils::GetPropertyValue<bool>(OldAnimInst, STR("IsFlying"), StateCache.bIsFlying, true);
        Utils::GetPropertyValue<bool>(OldAnimInst, STR("IsFloating"), StateCache.bIsFloating, true);
        Utils::GetPropertyValue<bool>(OldAnimInst, STR("IsJumpPreliminary"), StateCache.bIsJumpPreliminary, true);
        Utils::GetPropertyValue<bool>(OldAnimInst, STR("IsSkipJumpStart"), StateCache.bIsSkipJumpStart, true);

        if (!bNeedsAnimRebuild) {
            FProperty* LinkedProp = Utils::GetProperty(Params.MeshComp, STR("LinkedInstances"), true);
            if (LinkedProp) {
                TArray<UObject*>* LinkedArray = LinkedProp->ContainerPtrToValuePtr<TArray<UObject*>>(Params.MeshComp);
                if (LinkedArray) {
                    for (int32_t i = 0; i < LinkedArray->Num(); ++i) {
                        UObject* LayerInst = (*LinkedArray)[i];
                        if (LayerInst && Utils::IsObjectValid(LayerInst)) {
                            UClass* LayerCls = LayerInst->GetClassPrivate();
                            if (LayerCls && LayerCls->GetName().find(L"Implementation") != std::wstring::npos) {
                                CapturedImplClass = LayerCls;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    // 3. Pause & Teardown
    Utils::SetPropertyValue<bool>(Params.MeshComp, STR("bPauseAnims"), true, false);
    struct { bool bNewDisablePostProcessBlueprint; } EnablePP{ true };
    Utils::CallFunction(Params.MeshComp, STR("SetDisablePostProcessBlueprint"), &EnablePP);

    UFunction* SetAnimFunc = Params.MeshComp->GetFunctionByNameInChain(STR("SetAnimInstanceClass"));
    if (!SetAnimFunc) SetAnimFunc = Params.MeshComp->GetFunctionByNameInChain(STR("SetAnimClass"));

    if (bNeedsAnimRebuild && SetAnimFunc) {
        struct { UClass* NewClass; } ClearParams{ nullptr };
        Utils::SafeProcessEvent(Params.MeshComp, SetAnimFunc, &ClearParams);
    }

    // 4. Rebuild Assets
    if (Params.NewSkelMesh && Utils::IsObjectValid(Params.NewSkelMesh)) {
        if (Params.TargetSkeleton && Utils::IsObjectValid(Params.TargetSkeleton)) {
            Utils::SetPropertyValue<UObject*>(Params.NewSkelMesh, STR("Skeleton"), Params.TargetSkeleton, false);
        }
        struct { UObject* InMesh; bool bReinitPose; } MeshParams{ Params.NewSkelMesh, true };
        Utils::CallFunction(Params.MeshComp, STR("SetSkinnedAssetAndUpdate"), &MeshParams);
    }

    if (Params.TargetAnimClass && SetAnimFunc) {
        struct { UClass* NewClass; } AnimParams{ Params.TargetAnimClass };
        Utils::SafeProcessEvent(Params.MeshComp, SetAnimFunc, &AnimParams);
    }

    // 5. Force InitAnim
    struct { bool bForceReinit; } InitParams{ true };
    Utils::CallFunction(Params.MeshComp, STR("InitAnim"), &InitParams);

    // 6. Link Layers
    ReLinkAnimLayers(Params.MeshComp, Params.TargetCDO, Params.Character, Params.NewSkelMesh, CapturedImplClass);

    // 7. Synchronize Actions and Waza
    if (bNeedsAnimRebuild) {
        SyncStaticCharacterParams(Params.TargetStaticParam, Params.Character);
    }

    // 8. Re-attach RideMarker if detached
    if (RideMarkerComp && Utils::IsObjectValid(RideMarkerComp)) {
        FName CurrentSocket;
        if (Utils::GetPropertyValue<FName>(RideMarkerComp, STR("AttachSocketName"), CurrentSocket)) {
            if (CurrentSocket == FName(STR("None"), FNAME_Find) || CurrentSocket.ToString().empty()) {
                UFunction* AttachFunc = RideMarkerComp->GetFunctionByNameInChain(STR("K2_AttachToComponent"));
                if (AttachFunc) {
                    struct {
                        UObject* Parent;
                        FName SocketName;
                        uint8_t LocationRule;
                        uint8_t RotationRule;
                        uint8_t ScaleRule;
                        bool bWeldSimulatedBodies;
                    } AttachParams{ Params.MeshComp, FName(STR("spine_01"), FNAME_Add), 1, 1, 1, false };
                    Utils::SafeProcessEvent(RideMarkerComp, AttachFunc, &AttachParams);
                }
            }
        }
    }

    // 9. Restore State & Bind Native Montage Notifies
    UObject* NewAnimInst = nullptr;
    Utils::CallFunction(Params.MeshComp, STR("GetAnimInstance"), &NewAnimInst);
    if (NewAnimInst && Utils::IsObjectValid(NewAnimInst) && IsValidPalActor(Params.Character)) {
        RestoreAnimInstanceCaches(Params.Character, NewAnimInst, StateCache);
        BindNativeMontageNotifies(NewAnimInst);

        UObject* PostProcessInst = nullptr;
        Utils::CallFunction(Params.MeshComp, STR("GetPostProcessInstance"), &PostProcessInst);
        if (PostProcessInst && Utils::IsObjectValid(PostProcessInst)) {
            RestoreAnimInstanceCaches(Params.Character, PostProcessInst, StateCache);
            BindNativeMontageNotifies(PostProcessInst);
        }
    }

    // 10. Finalize animation state
    Utils::SetPropertyValue<bool>(Params.MeshComp, STR("bAnimTreeInitialised"), true, false);
    Utils::SetPropertyValue<bool>(Params.MeshComp, STR("bPauseAnims"), false, false);
    struct { bool bNewDisablePostProcessBlueprint; } DisablePP_False{ false };
    Utils::CallFunction(Params.MeshComp, STR("SetDisablePostProcessBlueprint"), &DisablePP_False);

    Utils::CallFunction(Params.Character, STR("RefreshSkin"));
}

static void SetPalNickname(UObject* IndivParam, const std::wstring& NewNameStr, const std::wstring& InstanceID, bool IsWild, UObject* Character) {
	if (!IndivParam || !IsValidPalActor(Character)) return;

	FProperty* SaveParamProp = Utils::GetProperty(IndivParam, STR("SaveParameter"));
	if (!SaveParamProp) return;

	void* SaveParamPtr = SaveParamProp->ContainerPtrToValuePtr<void>(IndivParam);
	if (!SaveParamPtr) return;

	FStructProperty* StructProp = CastField<FStructProperty>(SaveParamProp);
	if (!StructProp) return;

	UStruct* SaveParamStruct = StructProp->GetStruct();
	if (!SaveParamStruct) return;

	FString newName(NewNameStr.c_str());

	FProperty* NickNameProp = SaveParamStruct->GetPropertyByNameInChain(STR("NickName"));
	if (NickNameProp) {
		void* pNickName = NickNameProp->ContainerPtrToValuePtr<void>(SaveParamPtr);
		if (pNickName) NickNameProp->CopyCompleteValue(pNickName, &newName);
	}

	FProperty* FilteredNickNameProp = SaveParamStruct->GetPropertyByNameInChain(STR("FilteredNickName"));
	if (FilteredNickNameProp) {
		void* pFilteredNickName = FilteredNickNameProp->ContainerPtrToValuePtr<void>(SaveParamPtr);
		if (pFilteredNickName) FilteredNickNameProp->CopyCompleteValue(pFilteredNickName, &newName);
	}

	if (!IsWild) {
		UObject* GameplayStatics = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
		UObject* PlayerController = nullptr;
		if (GameplayStatics) {
			struct {
				UObject* WorldContextObject;
				int32_t PlayerIndex;
				UObject* ReturnValue;
			} GSParams{Character, 0, nullptr};
			Utils::CallFunction(GameplayStatics, STR("GetPlayerController"), &GSParams);
			PlayerController = GSParams.ReturnValue;
		}
		if (!PlayerController) PlayerController = UObjectGlobals::FindFirstOf(STR("PalPlayerController"));

		if (PlayerController && Utils::IsObjectValid(PlayerController)) {
			UFunction* UpdateNameFunc = PlayerController->GetFunctionByNameInChain(STR("UpdateCharacterNickName_ToServer"));
			if (UpdateNameFunc) {
				FPalInstanceID IDStruct;
				if (Utils::GetPropertyValue<FPalInstanceID>(IndivParam, STR("IndividualId"), IDStruct, true)) {
					struct {
						FPalInstanceID InstanceId;
						FString NewNickName;
					} Params;
					Params.InstanceId = IDStruct;
					Params.NewNickName = newName;

					Utils::SafeProcessEvent(PlayerController, UpdateNameFunc, &Params);
				}
			}
		}
	}

	DP_LOG(Default, "[Nickname] Applied name update for '{}' -> '{}' (Wild: {})", InstanceID, NewNameStr, IsWild ? L"True" : L"False");
}

static void ApplyMaterialOverrides(UObject* MeshComp, const SwapConfig& swap, PalPersistData& persist) {
	ClearMaterialOverrides(MeshComp);

	for (auto& mat : swap.MatReplaceList) {
		std::wstring ChosenPath = mat.matPath;
		std::wstring WideIndex = Utils::StringToWString(mat.index);

		if (mat.matPath.length() >= 2 && mat.matPath.substr(mat.matPath.length() - 2) == L"/*") {
			std::wstring VirtualFolder = mat.matPath.substr(0, mat.matPath.length() - 2);

			auto savedMatIt = persist.MatSet.find(mat.index);
			if (savedMatIt != persist.MatSet.end() && !savedMatIt->second.empty()) {
				ChosenPath = savedMatIt->second;
			} else {
				std::vector<std::wstring> AvailableMats = Utils::GetAssetsInVirtualFolder(VirtualFolder);
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
		try {
			idx = std::stoi(mat.index);
		}
		catch(...) {
			continue;
		}

		UObject* NewMat = nullptr;
		if (!ChosenPath.empty()) {
			NewMat = Utils::LoadAssetSafely(ChosenPath);
			if (!NewMat || !Utils::IsObjectValid(NewMat)) {
				DP_LOG(Warning, "[Slot {}] LoadAssetSafely FAILED for path: '{}'", WideIndex, ChosenPath);
				continue;
			}
		} else {
			struct {
				int32_t ElementIndex;
				UObject* ReturnValue;
			} GetMatParams{idx, nullptr};
			Utils::CallFunction(MeshComp, STR("GetMaterial"), &GetMatParams);
			NewMat = GetMatParams.ReturnValue;
			if (!NewMat || !Utils::IsObjectValid(NewMat)) continue;
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
				float S = 1.0f, V = 1.0f;
				float C = S * V;
				float X = C * (1.0f - std::abs(std::fmod(H / 60.0f, 2.0f) - 1.0f));
				float m = V - C;
				float r = 0, g = 0, b = 0;

				if (H >= 0 && H < 60) {
					r = C, g = X, b = 0;
				}
				else if (H >= 60 && H < 120) {
					r = X, g = C, b = 0;
				}
				else if (H >= 120 && H < 180) {
					r = 0, g = C, b = X;
				}
				else if (H >= 180 && H < 240) {
					r = 0, g = X, b = C;
				}
				else if (H >= 240 && H < 300) {
					r = X, g = 0, b = C;
				}
				else {
					r = C, g = 0, b = X;
				}

				appliedColor = { r + m, g + m, b + m, 1.0f };
				persist.MatColorSet[mat.index] = appliedColor;
			}

			UObject* KML = Utils::GetKML();
			UFunction* CreateFunc = Utils::GetKMLFunction(STR("CreateDynamicMaterialInstance"));

			static FProperty* WCProp = CreateFunc ? CreateFunc->GetPropertyByNameInChain(STR("WorldContextObject")) : nullptr;
			static FProperty* ParentProp = CreateFunc ? CreateFunc->GetPropertyByNameInChain(STR("Parent")) : nullptr;
			static FProperty* RetProp = CreateFunc ? CreateFunc->GetPropertyByNameInChain(STR("ReturnValue")) : nullptr;

			UObject* MID = nullptr;
			if (KML && Utils::IsObjectValid(KML) && CreateFunc) {
				alignas(8) uint8_t MIDParams[128] = {0};
				UObject* CharacterContext = MeshComp->GetOuterPrivate();

				if (WCProp) *WCProp->ContainerPtrToValuePtr<UObject*>(MIDParams) = CharacterContext;
				if (ParentProp) *ParentProp->ContainerPtrToValuePtr<UObject*>(MIDParams) = NewMat;
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

				struct {
					int32_t ElementIndex;
					UObject* Material;
				} MatParams{idx, MID};
				Utils::CallFunction(MeshComp, STR("SetMaterial"), &MatParams);
				continue;
			}
		}

		struct {
			int32_t ElementIndex;
			UObject* Material;
		} MatParams{idx, NewMat};
		Utils::CallFunction(MeshComp, STR("SetMaterial"), &MatParams);
	}
}

static void ApplyMorphTargets(UObject* MeshComp, const SwapConfig& swap, PalPersistData& persist) {
	if (swap.MorphTargetList.empty() || !MeshComp || !Utils::IsObjectValid(MeshComp)) return;

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

		struct {
			FName MorphTargetName;
			float Value;
			bool bRemoveZeroWeight;
		} MorphParams{
			FName(morph.target.c_str(), FNAME_Add), static_cast<float>(val), false
		};
		Utils::CallFunction(MeshComp, STR("SetMorphTarget"), &MorphParams);
	}
}

static void ApplySizeMultiplier(UObject* MeshComp, const SwapConfig& swap, PalPersistData& persist, const FVanillaDefaults& vanillaDefs) {
	double currentSizeMult = persist.SizeMultiplier;
	if (currentSizeMult <= 0.0) {
		if (swap.MinSizeMultiplier < swap.MaxSizeMultiplier) {
			static std::random_device rd;
			static std::mt19937 gen(rd());
			std::uniform_real_distribution<> dis(swap.MinSizeMultiplier, swap.MaxSizeMultiplier);
			currentSizeMult = dis(gen);
		} else {
			currentSizeMult = swap.MinSizeMultiplier;
		}
		persist.SizeMultiplier = currentSizeMult;
	}

	FVector_UE5 FinalMeshScale = {
		vanillaDefs.MeshScale.X * currentSizeMult,
		vanillaDefs.MeshScale.Y * currentSizeMult,
		vanillaDefs.MeshScale.Z * currentSizeMult
	};

	Utils::SetPropertyValue<FVector_UE5>(MeshComp, STR("DefaultScale3D"), FinalMeshScale);
	struct {
		FVector_UE5 NewScale3D;
	} ScaleParams{ FinalMeshScale };
	Utils::CallFunction(MeshComp, STR("SetRelativeScale3D"), &ScaleParams);

	DP_LOG(Default, "[Scale] Applied Mesh Scale: {:.3f} (Multiplier: {:.3f} * CDO Base: {:.3f}).",
	       FinalMeshScale.X, currentSizeMult, vanillaDefs.MeshScale.X);
}

// =========================================================================
// CORE SWAP PIPELINE
// =========================================================================
bool PalProcessor::ExecuteSwap(UObject* Character, bool ForceReroll, int ExplicitSwapIndex, bool IsCompanionSync, bool IsEvolutionEnd) {
	DP_PROFILE("ExecuteSwap", 5.0);
	if (!IsValidPalActor(Character)) return false;

	FPalIdentity id = ResolvePalIdentity(Character);
	if (!id.bIsValid) return false;

	std::wstring BlueprintName = L"";
	if (!IsPalBlueprintValid(Character, BlueprintName)) return false;

	UObject* Level = Character->GetOuterPrivate();
	UObject* World = Level ? Level->GetOuterPrivate() : nullptr;

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

	struct {
		UObject* Char;
		FName RetVal;
	} CharIDParams{Character, FName()};
	if (GCachedProps.GetCharacterIDFromCharacterFunc) {
		Utils::SafeProcessEvent(PalUtil, GCachedProps.GetCharacterIDFromCharacterFunc, &CharIDParams);
	}
	std::wstring RawCharID = CharIDParams.RetVal.ToString();

	static UObject* LastWorldLoaded = nullptr;
	if (World != LastWorldLoaded) {
		SaveManager::Get().LoadWorldData(World);
		LastWorldLoaded = World;
	}
	PalPersistData* ExistingData = SaveManager::Get().GetPersistData(id.InstanceID);

	std::wstring CharID = StripCharacterPrefix(RawCharID);

	if (!ForceReroll && ExplicitSwapIndex == -1) {
		if (ExistingData && ExistingData->IsVanillaLocked()) {
			DP_LOG(Verbose, "[PalProcessor] Pal '{}' (ID: '{}') is locked to Vanilla. Skipping swap.", CharID, id.InstanceID);
			return false;
		}
	}

	PalRuntimeStats stats = RetrievePalStats(id.IndivParam, RawCharID, id.InstanceID, true);
	int LevelNum = stats.Level;
	int RankNum = stats.Rank;
	int FriendshipNum = stats.Friendship;

	struct {
		UObject* Actor;
		bool RetVal;
	} WildParams{Character, false};
	if (GCachedProps.IsWildNPCFunc) Utils::SafeProcessEvent(PalUtil, GCachedProps.IsWildNPCFunc, &WildParams);
	bool IsWild = WildParams.RetVal;

	struct {
		bool ReturnValue;
	} RareParams{false};
	if (GCachedProps.IsRarePalFunc) Utils::SafeProcessEvent(id.IndivParam, GCachedProps.IsRarePalFunc, &RareParams);
	bool IsRare = RareParams.ReturnValue;

	struct {
		uint8_t RetVal;
	} GenderParams{0};
	if (GCachedProps.GetGenderTypeFunc) Utils::SafeProcessEvent(id.IndivParam, GCachedProps.GetGenderTypeFunc, &GenderParams);
	std::wstring GenderStr = (GenderParams.RetVal == 1) ? L"Male" : ((GenderParams.RetVal == 2) ? L"Female" : L"None");

	struct {
		FName RetVal;
	} SkinParams{FName()};
	if (GCachedProps.GetSkinNameFunc) Utils::SafeProcessEvent(id.IndivParam, GCachedProps.GetSkinNameFunc, &SkinParams);
	std::wstring SkinName = SkinParams.RetVal.ToString();
	if (SkinName == L"None") SkinName = L"";

	std::vector<std::wstring> Traits;
	struct {
		TArray<FName> RetVal;
	} TraitsParams;
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
			auto& explicitCfg = ConfigManager::Get().GetConfigs()[finalSwap];

			if (ExistingData && ExistingData->bIsManuallyLocked && ExistingData->SwapLabel == explicitCfg.SwapLabel) {
				bManualLockState = true;
			} else {
				bool bIsSelectedSwapValid = false;
				for (const auto& ev : evaluations) {
					if (ev.ConfigIndex == finalSwap) {
						bIsSelectedSwapValid = ev.IsValid;
						break;
					}
				}

				if (!explicitCfg.ReqSwap.empty()) {
					bool metPriorReq = false;
					std::wstring activeMeshLabel = SwappedInstances.count(Character) ? SwappedInstances[Character] : L"";
					for (const auto& req : explicitCfg.ReqSwap) {
						if (IEquals(req, activeMeshLabel)) {
							metPriorReq = true;
							break;
						}
					}
					if (!metPriorReq) {
						bIsSelectedSwapValid = false;
					}
				}

				bManualLockState = !bIsSelectedSwapValid;
				if (bManualLockState) {
					DP_LOG(Default, "Explicit swap is invalid for this Pal. Engaging Manual Lock.");
				} else {
					DP_LOG(Default, "Explicit swap is valid. Manual Lock disengaged.");
				}
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
							if (newBestSwap == -1) {
								DP_LOG(Normal, "Live Event: Current skin became invalid and no alternatives exist. Reverting to Vanilla.\n");
							} else {
								DP_LOG(Normal, "Live Event: Better skin found or current became invalid. Upgrading skin.\n");
							}
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
			}

			if (bHasFailedDependency) {
				DP_LOG(Error, "[Swap Aborted] '{}'. Asset does not exist on Path: '{}'", RawCharID, failedPath);
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
						if (StructProp && StructProp->GetStruct()) {
							FProperty* NickNameProp = StructProp->GetStruct()->GetPropertyByNameInChain(STR("NickName"));
							if (NickNameProp) {
								FString* pNickName = NickNameProp->ContainerPtrToValuePtr<FString>(SaveParamPtr);
								if (pNickName && (!pNickName->GetCharArray().GetData() || std::wstring_view(pNickName->GetCharArray().GetData()).empty())) {
									bNicknameIsEmpty = true;
								}
							}
						}
					}
				}

				bool bShouldSetNickname = ForceReroll || (ExplicitSwapIndex != -1) || (finalSwap != currentSwap) || bNicknameIsEmpty;
				if (bShouldSetNickname) {
					SetPalNickname(id.IndivParam, finalConfig.SetNickname, id.InstanceID, IsWild, Character);
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

			if (!IsCompanionSync) {
				std::vector<UObject*> linkedCompanions = GetLinkedPals(Character);
				for (UObject* Companion : linkedCompanions) {
					if (IsValidPalActor(Companion) && Companion != Character) {
						DP_LOG(Default, "[PalProcessor] Bidirectionally syncing partner actor: '{}' (Actor: {})", Companion->GetName(), (void*)Companion);
						ProcessPal(Companion, false, finalSwap, true);
					}
				}
			}

			return true;
		}
	} else {
		bool bHasActiveModSwap = SwappedInstances.count(Character) > 0;
		bool bHasSavedModSwap = ExistingData && ExistingData->HasSavedSwap();

		if (bHasSavedModSwap || bHasActiveModSwap) {
			DP_LOG(Default, "[Debug Swap] Pal '{}' (ID: '{}') no longer qualifies for any modded skins. Clearing Mod Data.", CharID, id.InstanceID);

			PalPersistData emptyData;
			emptyData.InstanceID = id.InstanceID;
			emptyData.bIsManuallyLocked = bManualLockState;
			emptyData.SizeMultiplier = 1.0;

			SaveManager::Get().SetPersistData(id.InstanceID, emptyData, true);

			if (bHasActiveModSwap) {
				SwappedInstances.erase(Character);

				UObject* MeshComp = nullptr;
				Utils::CallFunction(Character, STR("GetMainMesh"), &MeshComp);
				if (MeshComp && Utils::IsObjectValid(MeshComp)) {
					FVanillaDefaults defs = ExtractVanillaDefaults(Character);
					UClass* VanillaCharClass = Character->GetClassPrivate();
					UObject* VanillaCDO = VanillaCharClass ? VanillaCharClass->GetClassDefaultObject() : nullptr;

					FMeshApplyParams meshParams;
					meshParams.MeshComp = MeshComp;
					meshParams.NewSkelMesh = defs.SkelMesh;
					meshParams.TargetSkeleton = defs.Skeleton;
					meshParams.TargetAnimClass = defs.AnimClass;
					meshParams.TargetCDO = VanillaCDO;
					meshParams.Character = Character;
					meshParams.TargetStaticParam = defs.StaticParam;
					meshParams.bReinitPose = false;

					ApplyMeshAndAnim(meshParams, true);
					ClearMaterialOverrides(MeshComp);

					if (ExistingData) {
						for (const auto& [morphName, _] : ExistingData->MorphSet) {
							struct {
								FName MorphTargetName;
								float Value;
								bool bRemoveZeroWeight;
							} MorphParams{
								FName(morphName.c_str(), FNAME_Add), 0.0f, true
							};
							Utils::CallFunction(MeshComp, STR("SetMorphTarget"), &MorphParams);
						}
					}

					Utils::SetPropertyValue<bool>(MeshComp, STR("bPauseAnims"), false, false);
					struct {
						bool bNewDisablePostProcessBlueprint;
					} DisablePP_False{ false };
					Utils::CallFunction(MeshComp, STR("SetDisablePostProcessBlueprint"), &DisablePP_False);

					RefreshFacialModule(Character, MeshComp, VanillaCDO, nullptr);
					ResetPhysicsAndDynamics(MeshComp);
				}

				if (!IsCompanionSync) {
					std::vector<UObject*> linkedCompanions = GetLinkedPals(Character);
					for (UObject* Companion : linkedCompanions) {
						if (IsValidPalActor(Companion) && Companion != Character) {
							ProcessPal(Companion, false, -1, true);
						}
					}
				}

				VFXManager::Get().PlaySwapEffect(Character, L"/Game/Pal/Effect/Common/LevelUp/NS_LevelUp_Pal");
			}
			return true;
		}
	}
	return false;
}

void PalProcessor::ApplySwap(UObject* Character, const SwapConfig& swap, PalPersistData& persist) {
    std::wstring BPName;
    if (!IsPalBlueprintValid(Character, BPName)) return;

    UObject* MeshComp = nullptr;
    Utils::CallFunction(Character, STR("GetMainMesh"), &MeshComp);
    if (!MeshComp || !Utils::IsObjectValid(MeshComp)) return;

    UClass* CurrentAnimClass = nullptr;
    Utils::GetPropertyValue<UClass*>(MeshComp, STR("AnimClass"), CurrentAnimClass);

    UClass* TargetAnimClass = nullptr;
    UObject* TargetSkeleton = nullptr;
    UObject* TargetStaticParam = nullptr;

    UObject* PalUtil = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
    struct {
        UObject* Char;
        FName RetVal;
    } CharIDParams{ Character, FName() };
    if (PalUtil && Utils::IsObjectValid(PalUtil)) {
        Utils::SafeProcessEvent(PalUtil, PalUtil->GetFunctionByNameInChain(STR("GetCharacterIDFromCharacter")), &CharIDParams);
    }
    std::wstring CharID = StripCharacterPrefix(CharIDParams.RetVal.ToString());

    std::wstring ResolvedAnimPath = ResolveAnimPath(Character, swap.AnimTarget, CharID);
    bool bNeedsExternalAnimLoad = !ResolvedAnimPath.empty();

    FVanillaDefaults vanillaDefs = ExtractVanillaDefaults(Character);
    UClass* CharClass = Character->GetClassPrivate();
    UObject* VanillaCDO = CharClass ? CharClass->GetClassDefaultObject() : nullptr;
    UObject* TargetCDO = VanillaCDO;

    if (bNeedsExternalAnimLoad) {
        UClass* TargetBPClass = static_cast<UClass*>(Utils::LoadAssetSafely(ResolvedAnimPath));
        if (!IsPalBlueprintValid(Character, BPName)) return;

        Utils::CallFunction(Character, STR("GetMainMesh"), &MeshComp);
        if (!MeshComp || !Utils::IsObjectValid(MeshComp)) return;

        if (TargetBPClass && Utils::IsObjectValid(TargetBPClass)) {
            TargetCDO = TargetBPClass->GetClassDefaultObject();
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

    if (!TargetAnimClass || !Utils::IsObjectValid(TargetAnimClass)) {
        TargetAnimClass = CurrentAnimClass;
    }
    bool bNeedsAnimRebuild = (TargetAnimClass != CurrentAnimClass);

    UObject* NewMesh = nullptr;
    if (!swap.SkelMeshPath.empty()) {
        NewMesh = Utils::LoadSkeletalMeshSafely(swap.SkelMeshPath);
        if (!IsPalBlueprintValid(Character, BPName)) return;

        Utils::CallFunction(Character, STR("GetMainMesh"), &MeshComp);
        if (!MeshComp || !Utils::IsObjectValid(MeshComp)) return;

        if (NewMesh && Utils::IsObjectValid(NewMesh)) {
            std::wstring meshClassName = NewMesh->GetClassPrivate()->GetName();
            if (meshClassName.find(L"SkeletalMesh") == std::wstring::npos && meshClassName.find(L"SkinnedAsset") == std::wstring::npos) {
                DP_LOG(Warning, "[ApplySwap] Aborted: Loaded asset is a '{}', not a SkeletalMesh.", meshClassName);
                NewMesh = nullptr;
            }
        }
    }

    FMeshApplyParams meshParams;
    meshParams.MeshComp = MeshComp;
    meshParams.NewSkelMesh = NewMesh;
    meshParams.TargetSkeleton = TargetSkeleton;
    meshParams.TargetAnimClass = TargetAnimClass;
    meshParams.TargetCDO = TargetCDO;
    meshParams.Character = Character;
    meshParams.TargetStaticParam = TargetStaticParam;
    meshParams.bReinitPose = false;

    ApplyMeshAndAnim(meshParams, bNeedsAnimRebuild);

    ApplyMaterialOverrides(MeshComp, swap, persist);
    Utils::SetPropertyValue<bool>(MeshComp, STR("bPauseAnims"), false, false);
    ApplyMorphTargets(MeshComp, swap, persist);

    struct { bool bNewDisablePostProcessBlueprint; } DisablePP_False{ false };
    Utils::CallFunction(MeshComp, STR("SetDisablePostProcessBlueprint"), &DisablePP_False);

    RefreshFacialModule(Character, MeshComp, TargetCDO, &swap);
    ApplySizeMultiplier(MeshComp, swap, persist, vanillaDefs);
    ResetPhysicsAndDynamics(MeshComp);
}

void PalProcessor::ResetPal(UObject* Character) {
	if (!IsValidPalActor(Character)) {
		DP_LOG(Warning, "[PalProcessor::ResetPal] Invalid Character passed!");
		return;
	}

	FPalIdentity id = ResolvePalIdentity(Character);
	if (!id.bIsValid) return;

	DP_LOG(Default, "[PalProcessor] Resetting Pal '{}' (ID: '{}') to Vanilla Defaults...", Character->GetName(), id.InstanceID);

	PalPersistData* ExistingData = SaveManager::Get().GetPersistData(id.InstanceID);
	std::map<std::wstring, double> MorphsToZero;
	if (ExistingData) {
		MorphsToZero = ExistingData->MorphSet;
	}

	PalPersistData newData;
	newData.InstanceID = id.InstanceID;
	newData.bIsManuallyLocked = true;
	newData.SwapLabel = L"Vanilla";
	newData.PackName = L"";
	newData.SkinName = L"";
	newData.SkelMeshPath = L"";
	newData.SizeMultiplier = 1.0;
	SaveManager::Get().SetPersistData(id.InstanceID, newData, true);

	SwappedInstances.erase(Character);
	std::vector<UObject*> palSet = GetLinkedPals(Character);

	for (UObject* TargetPalObj : palSet) {
		if (!IsValidPalActor(TargetPalObj)) continue;
		SwappedInstances.erase(TargetPalObj);

		FPalIdentity compId = ResolvePalIdentity(TargetPalObj);
		if (compId.bIsValid && compId.InstanceID != id.InstanceID) {
			PalPersistData compData = newData;
			compData.InstanceID = compId.InstanceID;
			SaveManager::Get().SetPersistData(compId.InstanceID, compData, true);
		}

		std::wstring BPName;
		if (!IsPalBlueprintValid(TargetPalObj, BPName)) continue;

		UObject* MeshComp = nullptr;
		Utils::CallFunction(TargetPalObj, STR("GetMainMesh"), &MeshComp);
		if (!MeshComp || !Utils::IsObjectValid(MeshComp)) continue;

		FVanillaDefaults defs = ExtractVanillaDefaults(TargetPalObj);
		UClass* VanillaCharClass = TargetPalObj->GetClassPrivate();
		UObject* VanillaCDO = VanillaCharClass ? VanillaCharClass->GetClassDefaultObject() : nullptr;

		FMeshApplyParams meshParams;
		meshParams.MeshComp = MeshComp;
		meshParams.NewSkelMesh = defs.SkelMesh;
		meshParams.TargetSkeleton = defs.Skeleton;
		meshParams.TargetAnimClass = defs.AnimClass;
		meshParams.TargetCDO = VanillaCDO;
		meshParams.Character = TargetPalObj;
		meshParams.TargetStaticParam = defs.StaticParam;
		meshParams.bReinitPose = false;

		ApplyMeshAndAnim(meshParams, true);
		ClearMaterialOverrides(MeshComp);

		for (const auto& [morphName, _] : MorphsToZero) {
			struct {
				FName MorphTargetName;
				float Value;
				bool bRemoveZeroWeight;
			} MorphParams{
				FName(morphName.c_str(), FNAME_Add), 0.0f, true
			};
			Utils::CallFunction(MeshComp, STR("SetMorphTarget"), &MorphParams);
		}

		Utils::SetPropertyValue<bool>(MeshComp, STR("bPauseAnims"), false, false);
		struct {
			bool bNewDisablePostProcessBlueprint;
		} DisablePP_False{ false };
		Utils::CallFunction(MeshComp, STR("SetDisablePostProcessBlueprint"), &DisablePP_False);

		RefreshFacialModule(TargetPalObj, MeshComp, VanillaCDO, nullptr);
		ResetPhysicsAndDynamics(MeshComp);
	}

	VFXManager::Get().PlaySwapEffect(Character, L"/Game/Pal/Effect/Common/LevelUp/NS_LevelUp_Pal");
	DP_LOG(Default, "[PalProcessor] Successfully reset Pal '{}' (ID: '{}') to Vanilla and locked.", Character->GetName(), id.InstanceID);
}

}