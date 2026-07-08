#define NOMINMAX 
#include <Windows.h>

#include "UI/Views/UIManager.hpp"
#include "UI/Components/WindowFrame.hpp"
#include "UI/WidgetBuilder.hpp"
#include "UI/IconLibrary.hpp"
#include "ConfigManager.hpp"
#include "SaveManager.hpp"
#include "PalProcessor.hpp"
#include "Utils.hpp"
#include "DataTypes.hpp"
#include <cmath>
#include <map>
#include <algorithm>

using namespace RC;
using namespace RC::Unreal;

namespace DynPals {

    // --- NEW: Helper function to retrieve the Pal's FollowCamera component ---
    
    // --- END NEW HELPER ---

    void UIManager::UpdateTarget() {
        TargetPal = nullptr;
        TargetInstanceID = L"";
        TargetCharID = L"";

        if (!CurrentPlayerController) return;

        UObject* PlayerPawn = nullptr;
        Utils::CallFunction(CurrentPlayerController, STR("K2_GetPawn"), &PlayerPawn);
        if (!PlayerPawn) return;

        struct FRotator_UE5 { double Pitch, Yaw, Roll; };
        struct { FVector_UE5 Location; FRotator_UE5 Rotation; } ViewPointParams;
        Utils::CallFunction(CurrentPlayerController, STR("GetPlayerViewPoint"), &ViewPointParams);
        
        FVector_UE5 CameraLoc = ViewPointParams.Location;
        FRotator_UE5 CameraRot = ViewPointParams.Rotation;

        double PitchRad = CameraRot.Pitch * 0.01745329251; 
        double YawRad = CameraRot.Yaw * 0.01745329251;
        double CosPitch = std::cos(PitchRad);

        FVector_UE5 CameraForward;
        CameraForward.X = std::cos(YawRad) * CosPitch;
        CameraForward.Y = std::sin(YawRad) * CosPitch;
        CameraForward.Z = std::sin(PitchRad);

        std::vector<UObject*> AllPals;
        UObjectGlobals::FindAllOf(STR("PalCharacter"), AllPals);

        UObject* aimedPal = nullptr;
        double highestDot = -1.0;
        UObject* closestPal = nullptr;
        double closestDistSq = 999999999.0;

        for (UObject* Pal : AllPals) {
            if (Pal == PlayerPawn || !Pal) continue;

            FVector_UE5 PalLoc{ 0.0, 0.0, 0.0 };
            struct { FVector_UE5 RetVal; } PalLocParams;
            Utils::CallFunction(Pal, STR("K2_GetActorLocation"), &PalLocParams);
            PalLoc = PalLocParams.RetVal;

            FVector_UE5 Dir;
            Dir.X = PalLoc.X - CameraLoc.X;
            Dir.Y = PalLoc.Y - CameraLoc.Y;
            Dir.Z = PalLoc.Z - CameraLoc.Z;

            double distSq = (Dir.X * Dir.X) + (Dir.Y * Dir.Y) + (Dir.Z * Dir.Z);
            double dist = std::sqrt(distSq);

            if (dist > 5000.0) continue;

            FVector_UE5 DirNorm = { Dir.X / dist, Dir.Y / dist, Dir.Z / dist };
            double dot = CameraForward.X * DirNorm.X + CameraForward.Y * DirNorm.Y + CameraForward.Z * DirNorm.Z;

            if (dot >= 0.97) {
                if (dot > highestDot) {
                    highestDot = dot;
                    aimedPal = Pal;
                }
            }

            if (distSq < closestDistSq) {
                closestDistSq = distSq;
                closestPal = Pal;
            }
        }

        TargetPal = aimedPal ? aimedPal : closestPal;

        if (TargetPal) {
            UObject* ParamComp = nullptr;
            Utils::GetPropertyValue(TargetPal, STR("CharacterParameterComponent"), ParamComp);
            if (!ParamComp) return;

            UObject* IndivParam = nullptr;
            Utils::GetPropertyValue(ParamComp, STR("IndividualParameter"), IndivParam);
            if (!IndivParam) return;

            FPalInstanceID IDStruct;
            if (Utils::GetPropertyValue(IndivParam, STR("IndividualId"), IDStruct)) {
                TargetInstanceID = Utils::GuidToWString(IDStruct.InstanceId);
            }

            UObject* PalUtil = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
            struct { UObject* Char; FName RetVal; } CharIDParams{TargetPal, FName()};
            if (PalUtil) Utils::CallFunction(PalUtil, STR("GetCharacterIDFromCharacter"), &CharIDParams);
            TargetCharID = PalProcessor::Get().StripCharacterPrefix(CharIDParams.RetVal.ToString());
        }
    }

    void UIManager::EnablePalCamera() {
        if (!CurrentPlayerController || !TargetPal || bIsPalCameraActive) return;

        // 1. Locate and Force-Activate the Pal's FollowCamera Component
        UObject* FollowCameraObj = nullptr;
        bool bCamDirectFound = Utils::GetPropertyValue<UObject*>(TargetPal, STR("FollowCamera"), FollowCameraObj, true);
        
        if (!bCamDirectFound || !FollowCameraObj) {
            UClass* CameraClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Engine.CameraComponent"));
            if (CameraClass) {
                struct { UClass* ComponentClass; UObject* ReturnValue; } GetCompParams{CameraClass, nullptr};
                Utils::CallFunction(TargetPal, STR("GetComponentByClass"), &GetCompParams, true);
                FollowCameraObj = GetCompParams.ReturnValue;
            }
        }

        if (FollowCameraObj) {
            Utils::SetPropertyValue<bool>(FollowCameraObj, STR("bIsActive"), true);
            struct { bool bReset; } ActParams{ false };
            Utils::CallFunction(FollowCameraObj, STR("Activate"), &ActParams, true);
        }

        // 2. Customize the CameraBoom (SpringArm) of the Pal
        UObject* CameraBoomObj = nullptr;
        bool bDirectFound = Utils::GetPropertyValue<UObject*>(TargetPal, STR("CameraBoom"), CameraBoomObj, true);
        
        if (!bDirectFound || !CameraBoomObj) {
            UClass* SpringArmClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Engine.SpringArmComponent"));
            if (SpringArmClass) {
                struct { UClass* ComponentClass; UObject* ReturnValue; } GetCompParams{SpringArmClass, nullptr};
                Utils::CallFunction(TargetPal, STR("GetComponentByClass"), &GetCompParams, true);
                CameraBoomObj = GetCompParams.ReturnValue;
            }
        }

        if (CameraBoomObj) {
            Utils::SetPropertyValue<float>(CameraBoomObj, STR("TargetArmLength"), 2000.0f);
            Utils::SetPropertyValue<bool>(CameraBoomObj, STR("bUsePawnControlRotation"), false);
            Utils::SetPropertyValue<bool>(CameraBoomObj, STR("bDoCollisionTest"), false); 
            
            struct FRotator_UE5 { double Pitch, Yaw, Roll; };
            FRotator_UE5 FrontRot{ 0.0, SaveManager::Get().Settings.CameraRotation, 0.0 };
            Utils::SetPropertyValue<FRotator_UE5>(CameraBoomObj, STR("RelativeRotation"), FrontRot);
        }

        // 3. Switch to Pal's camera via SetViewTargetWithBlend
        RC::Unreal::UFunction* SetViewTargetFunc = CurrentPlayerController->GetFunctionByNameInChain(STR("SetViewTargetWithBlend"));
        if (SetViewTargetFunc) {
            alignas(8) uint8_t SetViewParams[128] = {0};
            RC::Unreal::FProperty* TargetProp = SetViewTargetFunc->GetPropertyByNameInChain(STR("NewViewTarget"));
            if (TargetProp) *TargetProp->ContainerPtrToValuePtr<RC::Unreal::UObject*>(SetViewParams) = TargetPal;
            CurrentPlayerController->ProcessEvent(SetViewTargetFunc, SetViewParams);
            bIsPalCameraActive = true;
        }
    }
    void UIManager::UpdatePalCameraRotation(double Yaw) {
        if (!TargetPal) return;
        
        UObject* CameraBoomObj = nullptr;
        bool bDirectFound = Utils::GetPropertyValue<UObject*>(TargetPal, STR("CameraBoom"), CameraBoomObj, true);
        
        if (!bDirectFound || !CameraBoomObj) {
            UClass* SpringArmClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Engine.SpringArmComponent"));
            if (SpringArmClass) {
                struct { UClass* ComponentClass; UObject* ReturnValue; } GetCompParams{SpringArmClass, nullptr};
                Utils::CallFunction(TargetPal, STR("GetComponentByClass"), &GetCompParams, true);
                CameraBoomObj = GetCompParams.ReturnValue;
            }
        }

        if (CameraBoomObj) {
            struct FRotator_UE5 { double Pitch, Yaw, Roll; };
            FRotator_UE5 NewRot{ 0.0, Yaw, 0.0 };
            Utils::SetPropertyValue<FRotator_UE5>(CameraBoomObj, STR("RelativeRotation"), NewRot);
        }
    }

    void UIManager::DisablePalCamera() {
        if (!bIsPalCameraActive || !CurrentPlayerController) return;

        RC::Unreal::UFunction* SetViewTargetFunc = CurrentPlayerController->GetFunctionByNameInChain(STR("SetViewTargetWithBlend"));
        if (SetViewTargetFunc) {
            alignas(8) uint8_t Params[128] = {0};
            RC::Unreal::FProperty* TargetProp = SetViewTargetFunc->GetPropertyByNameInChain(STR("NewViewTarget"));
            if (TargetProp) {
                *TargetProp->ContainerPtrToValuePtr<RC::Unreal::UObject*>(Params) = OriginalViewTarget ? OriginalViewTarget : CurrentPlayerController;
            }
            CurrentPlayerController->ProcessEvent(SetViewTargetFunc, Params);
        }

        bIsPalCameraActive = false;
    }



    // --- MODIFIED: OnSetup to handle camera switch ---
    bool UIManager::OnSetup() {
        UpdateTarget();
        
        if (!CurrentPlayerController) {
            EnqueueUIToast(L"Player controller not found. Cannot switch camera.", 2, 1);
            return false;
        }

        if (!TargetPal) {
            EnqueueUIToast(L"No valid Pal found in range!", 2, 1);
            return false; 
        }

        // 1. Store original view target safely via Reflection
        RC::Unreal::UFunction* GetViewTargetFunc = CurrentPlayerController->GetFunctionByNameInChain(STR("GetViewTarget")); 
        if (GetViewTargetFunc) {
            alignas(8) uint8_t Params[32] = {0};
            CurrentPlayerController->ProcessEvent(GetViewTargetFunc, Params);
            RC::Unreal::FProperty* RetProp = GetViewTargetFunc->GetPropertyByNameInChain(STR("ReturnValue"));
            if (RetProp) OriginalViewTarget = *RetProp->ContainerPtrToValuePtr<RC::Unreal::UObject*>(Params);
        }

        // Fallback to Pawn if GetViewTarget failed
        if (!OriginalViewTarget) {
            struct { RC::Unreal::UObject* ReturnValue; } GetPawnParams{nullptr};
            Utils::CallFunction(CurrentPlayerController, STR("K2_GetPawn"), &GetPawnParams, true);
            OriginalViewTarget = GetPawnParams.ReturnValue;
        }

        if (SaveManager::Get().Settings.bFocusPal) {
            EnablePalCamera();
        }

        return true;
    }

    void UIManager::OnClose() {
        TargetPal = nullptr;
        TargetInstanceID = L"";
        TargetCharID = L"";
        SkinDropdown = nullptr;
        HideInvalidSwitch = nullptr;
        RerollButton = nullptr;
        MorphSliders.clear();
        FocusPalSwitch = nullptr;
        CameraRotationSlider = nullptr;
        MainScrollBoxObj = nullptr;
        GetScrollOffsetFunc = nullptr;

        DisablePalCamera();
        OriginalViewTarget = nullptr;
    }



    // --- END MODIFIED OnClose ---

    void UIManager::BuildWidget() {
        if (!CurrentPlayerController || !TargetPal) return;

        SkinDropdown = nullptr;
        HideInvalidSwitch = nullptr;
        RerollButton = nullptr;
        MorphSliders.clear();
        MainScrollBoxObj = nullptr;
        GetScrollOffsetFunc = nullptr;
        FocusPalSwitch = nullptr;
        CameraRotationSlider = nullptr;

        UObject* WBL = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/UMG.Default__WidgetBlueprintLibrary"));
        UClass* WidgetClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.UserWidget"));
        if (!WBL || !WidgetClass) return;

        struct { UObject* WorldContext; UClass* WidgetType; UObject* OwningPlayer; UObject* ReturnValue; } CreateParams{
            CurrentPlayerController, WidgetClass, CurrentPlayerController, nullptr
        };
        Utils::CallFunction(WBL, STR("Create"), &CreateParams);
        MyWidget = CreateParams.ReturnValue;

        UObject* PalFont = Utils::LoadAssetSafely(UI::Assets::Fonts::PalDefault);
        const FLinearColor_UE5 PalBlue = {0.78f, 0.96f, 1.0f, 1.0f}; 
        const FLinearColor_UE5 White   = {1.0f, 1.0f, 1.0f, 1.0f};
        const FLinearColor_UE5 Emerald = {0.063f, 0.725f, 0.506f, 1.0f};

        bool IsRare = false, IsWild = false;
        std::wstring GenderStr = L"None", SkinName = L"";
        int LevelNum = 1, RankNum = 0, FriendshipNum = 0;
        std::vector<std::wstring> Traits;

        UObject* ParamComp = nullptr;
        Utils::GetPropertyValue(TargetPal, STR("CharacterParameterComponent"), ParamComp);
        if (ParamComp) {
            UObject* IndivParam = nullptr;
            Utils::GetPropertyValue(ParamComp, STR("IndividualParameter"), IndivParam);
            if (IndivParam) {
                struct { bool RetVal; } RareParams{false};
                Utils::CallFunction(IndivParam, STR("IsRarePal"), &RareParams);
                IsRare = RareParams.RetVal;

                struct { uint8_t RetVal; } GenderParams{0};
                Utils::CallFunction(IndivParam, STR("GetGenderType"), &GenderParams);
                GenderStr = (GenderParams.RetVal == 1) ? L"Male" : ((GenderParams.RetVal == 2) ? L"Female" : L"None");

                struct { int32_t RetVal = -1; } LevelParams;
                Utils::CallFunction(IndivParam, STR("GetLevel"), &LevelParams);
                LevelNum = LevelParams.RetVal == -1 ? 1 : LevelParams.RetVal;

                struct { int32_t RetVal = -1; } RankParams;
                Utils::CallFunction(IndivParam, STR("GetRank"), &RankParams);
                RankNum = RankParams.RetVal == -1 ? 0 : RankParams.RetVal;

                struct { int32_t RetVal = -1; } FriendshipParams;
                Utils::CallFunction(IndivParam, STR("GetFriendshipRank"), &FriendshipParams);
                FriendshipNum = FriendshipParams.RetVal;
                if (FriendshipNum == -1) {
                    struct { int32_t RetVal = -1; } LegacyFriendshipParams;
                    Utils::CallFunction(IndivParam, STR("GetFriendshipPoint"), &LegacyFriendshipParams);
                    FriendshipNum = LegacyFriendshipParams.RetVal == -1 ? 0 : LegacyFriendshipParams.RetVal;
                }

                struct { FName RetVal; } SkinParams{FName()};
                Utils::CallFunction(IndivParam, STR("GetSkinName"), &SkinParams);
                SkinName = SkinParams.RetVal.ToString();
                if (SkinName == L"None") SkinName = L"";

                struct { TArray<FName> RetVal; } TraitsParams;
                Utils::CallFunction(IndivParam, STR("GetPassiveSkillList"), &TraitsParams);
                for (int32_t i = 0; i < TraitsParams.RetVal.Num(); ++i) {
                    Traits.push_back(TraitsParams.RetVal[i].ToString());
                }
            }
        }

        UObject* PalUtil = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
        if (PalUtil) {
            struct { UObject* Actor; bool RetVal; } WildParams{TargetPal, false};
            Utils::CallFunction(PalUtil, STR("IsWildNPC"), &WildParams);
            IsWild = WildParams.RetVal;
        }

        PalPersistData* currentPersist = SaveManager::Get().GetPersistData(TargetInstanceID);
        std::wstring CurrentSwapLabel = currentPersist ? currentPersist->SwapLabel : L"";
        
        auto evaluations = ConfigManager::Get().EvaluateAllSwaps(TargetCharID, IsRare, GenderStr, Traits, LevelNum, SkinName, RankNum, FriendshipNum, IsWild, CurrentSwapLabel);

        int bestScore = 999999;
        for (const auto& eval : evaluations) {
            if (!eval.IsValid) continue;
            if (eval.Score < bestScore) bestScore = eval.Score;
        }

        double totalTiedWeight = 0.0;
        for (const auto& eval : evaluations) {
            if (eval.IsValid && eval.Score == bestScore) {
                totalTiedWeight += ConfigManager::Get().GetConfigs()[eval.ConfigIndex].SpawnWeight;
            }
        }

        std::map<std::wstring, std::vector<SwapEvaluation>> rawPacks;
        for (const auto& eval : evaluations) {
            if (bHideInvalidSwaps && !eval.IsValid) continue;
            rawPacks[ConfigManager::Get().GetConfigs()[eval.ConfigIndex].PackName].push_back(eval);
        }

        DropdownOptions.clear();
        DropdownConfigIndices.clear();

        for (auto& [packName, evals] : rawPacks) {
            std::map<std::wstring, int> prefixCounts; 
            
            std::wstring headerStr = L"[ " + packName + L" ]";
            DropdownOptions.push_back(headerStr);
            DropdownConfigIndices.push_back(-1); 

            for (auto& eval : evals) {
                auto& cfg = ConfigManager::Get().GetConfigs()[eval.ConfigIndex];
                std::wstring labelName = cfg.SwapLabel;
                
                if (labelName.length() > 11) {
                    size_t len = labelName.length();
                    if (labelName[len - 1] == L')' && labelName[len - 10] == L'(' && labelName[len - 11] == L' ') {
                        labelName = labelName.substr(0, len - 11);
                    }
                }

                if (labelName.empty()) labelName = cfg.SkinName;
                if (labelName.empty()) {
                    std::wstring filename = cfg.SkelMeshPath;
                    size_t lastSlash = filename.find_last_of(L'/');
                    if (lastSlash != std::wstring::npos) filename = filename.substr(lastSlash + 1);
                    size_t dotPos = filename.find(L'.');
                    if (dotPos != std::wstring::npos) filename = filename.substr(0, dotPos);
                    labelName = filename;
                }
                
                if (labelName.rfind(L"SK_", 0) == 0 || labelName.rfind(L"sk_", 0) == 0 || labelName.find(L'_') != std::wstring::npos) {
                    std::wstring clean = labelName;
                    if (clean.rfind(L"SK_", 0) == 0 || clean.rfind(L"sk_", 0) == 0) clean = clean.substr(3);
                    
                    std::wstring lowerClean = clean;
                    std::transform(lowerClean.begin(), lowerClean.end(), lowerClean.begin(), ::towlower);
                    std::wstring lowerCharID = TargetCharID;
                    std::transform(lowerCharID.begin(), lowerCharID.end(), lowerCharID.begin(), ::towlower);
                    
                    if (lowerClean.rfind(lowerCharID + L"_", 0) == 0 && clean.length() >= TargetCharID.length() + 1) {
                        clean = clean.substr(TargetCharID.length() + 1);
                    } else if (lowerClean.rfind(lowerCharID, 0) == 0 && clean.length() >= TargetCharID.length()) {
                        clean = clean.substr(TargetCharID.length());
                        if (!clean.empty() && clean[0] == L'_') clean = clean.substr(1);
                    }
                    
                    std::vector<std::wstring> tokens;
                    size_t start = 0, end;
                    while ((end = clean.find(L'_', start)) != std::wstring::npos) {
                        if (end != start) tokens.push_back(clean.substr(start, end - start));
                        start = end + 1;
                    }
                    if (start < clean.length()) tokens.push_back(clean.substr(start));
                    
                    if (tokens.size() >= 3) prefixCounts[tokens[0] + L"_" + tokens[1]]++;
                    if (tokens.size() >= 2) prefixCounts[tokens[0]]++;
                }
            }

            for (auto& eval : evals) {
                auto& cfg = ConfigManager::Get().GetConfigs()[eval.ConfigIndex];
                std::wstring labelName = cfg.SwapLabel;
                
                if (labelName.length() > 11) {
                    size_t len = labelName.length();
                    if (labelName[len - 1] == L')' && labelName[len - 10] == L'(' && labelName[len - 11] == L' ') {
                        labelName = labelName.substr(0, len - 11);
                    }
                }

                if (labelName.empty()) labelName = cfg.SkinName;
                if (labelName.empty()) {
                    std::wstring filename = cfg.SkelMeshPath;
                    size_t lastSlash = filename.find_last_of(L'/');
                    if (lastSlash != std::wstring::npos) filename = filename.substr(lastSlash + 1);
                    size_t dotPos = filename.find(L'.');
                    if (dotPos != std::wstring::npos) filename = filename.substr(0, dotPos);
                    labelName = filename;
                }
                
                std::wstring display = labelName;
                if (labelName.rfind(L"SK_", 0) == 0 || labelName.rfind(L"sk_", 0) == 0 || labelName.find(L'_') != std::wstring::npos) {
                    std::wstring clean = labelName;
                    if (clean.rfind(L"SK_", 0) == 0 || clean.rfind(L"sk_", 0) == 0) clean = clean.substr(3);
                    
                    std::wstring lowerClean = clean;
                    std::transform(lowerClean.begin(), lowerClean.end(), lowerClean.begin(), ::towlower);
                    std::wstring lowerCharID = TargetCharID;
                    std::transform(lowerCharID.begin(), lowerCharID.end(), lowerCharID.begin(), ::towlower);
                    
                    if (lowerClean.rfind(lowerCharID + L"_", 0) == 0 && clean.length() >= TargetCharID.length() + 1) {
                        clean = clean.substr(TargetCharID.length() + 1);
                    } else if (lowerClean.rfind(lowerCharID, 0) == 0 && clean.length() >= TargetCharID.length()) {
                        clean = clean.substr(TargetCharID.length());
                        if (!clean.empty() && clean[0] == L'_') clean = clean.substr(1);
                    }
                    if (clean.empty()) clean = L"(Vanilla Mesh)";
                    
                    std::vector<std::wstring> tokens;
                    size_t start = 0, end;
                    while ((end = clean.find(L'_', start)) != std::wstring::npos) {
                        if (end != start) tokens.push_back(clean.substr(start, end - start));
                        start = end + 1;
                    }
                    if (start < clean.length()) tokens.push_back(clean.substr(start));
                    
                    std::wstring bestPrefix = L"";
                    int prefixTokens = 0;
                    if (tokens.size() >= 3) {
                        std::wstring cand = tokens[0] + L"_" + tokens[1];
                        if (prefixCounts[cand] >= 2) { bestPrefix = cand; prefixTokens = 2; }
                    }
                    if (bestPrefix.empty() && tokens.size() >= 2) {
                        std::wstring cand = tokens[0];
                        if (prefixCounts[cand] >= 2) { bestPrefix = cand; prefixTokens = 1; }
                    }
                    
                    if (!bestPrefix.empty()) {
                        display = L"";
                        for(size_t i = prefixTokens; i < tokens.size(); ++i) {
                            display += tokens[i];
                            if (i < tokens.size() - 1) display += L" ";
                        }
                    } else {
                        std::replace(display.begin(), display.end(), L'_', L' ');
                    }
                    
                    std::wstring splitDisplay = L"";
                    if (!display.empty()) {
                        splitDisplay.push_back(display[0]);
                        for (size_t i = 1; i < display.size(); ++i) {
                            if ((display[i - 1] >= L'a' && display[i - 1] <= L'z') && 
                                (display[i] >= L'A' && display[i] <= L'Z')) {
                                splitDisplay.push_back(L' ');
                            }
                            splitDisplay.push_back(display[i]);
                        }
                        display = splitDisplay;
                    }
                }
                
                DropdownOptions.push_back(L"   " + display);
                DropdownConfigIndices.push_back(eval.ConfigIndex);
            }
        }

        int initialIdx = 0;
        int persistConfigIndex = currentPersist ? ConfigManager::Get().FindConfigIndex(currentPersist->PackName, currentPersist->SkinName, currentPersist->SwapLabel, currentPersist->SkelMeshPath, TargetCharID) : -1;
        
        for (size_t i = 0; i < DropdownConfigIndices.size(); ++i) {
            if (DropdownConfigIndices[i] == persistConfigIndex) {
                initialIdx = static_cast<int>(i);
                break;
            }
        }

        SkinDropdown = std::make_unique<UI::Dropdown>(DropdownOptions, initialIdx);
        SkinDropdown->OnChanged([this](int Index, std::wstring Choice) {
            if (Index >= 0 && Index < static_cast<int>(DropdownConfigIndices.size())) {
                int TargetConfig = DropdownConfigIndices[Index];
                PalProcessor::Get().ForceSwap(TargetPal, TargetConfig);
                
                if (MainScrollBoxObj && GetScrollOffsetFunc) {
                    struct { float Offset; } Params{ 0.0f };
                    MainScrollBoxObj->ProcessEvent(GetScrollOffsetFunc, &Params);
                    LastScrollOffset = Params.Offset;
                }
                RequestRebuild(); 
            }
        });

        HideInvalidSwitch = std::make_unique<UI::Switch>(MyWidget, bHideInvalidSwaps);
        HideInvalidSwitch->OnChanged([this](bool bState) {
            bHideInvalidSwaps = bState;
            if (MainScrollBoxObj && GetScrollOffsetFunc) {
                struct { float Offset; } Params{ 0.0f };
                MainScrollBoxObj->ProcessEvent(GetScrollOffsetFunc, &Params);
                LastScrollOffset = Params.Offset;
            }
            RequestRebuild(); 
        });

        FocusPalSwitch = std::make_unique<UI::Switch>(MyWidget, SaveManager::Get().Settings.bFocusPal);
        FocusPalSwitch->OnChanged([this](bool bState) {
            SaveManager::Get().Settings.bFocusPal = bState;
            SaveManager::Get().SaveWorldData();
            if (bState) {
                EnablePalCamera();
            } else {
                DisablePalCamera();
            }
            
            if (MainScrollBoxObj && GetScrollOffsetFunc) {
                struct { float Offset; } Params{ 0.0f };
                MainScrollBoxObj->ProcessEvent(GetScrollOffsetFunc, &Params);
                LastScrollOffset = Params.Offset;
            }
            RequestRebuild();
        });

        auto RerollBtnBuilder = WidgetBuilder(UI::Assets::Blueprints::CommonButton, MyWidget)
            .Text(L"      Reroll Pal      ")
            .BackgroundColor(PalBlue)
            .DesiredSizeOverride(300.0f, 45.0f)
            .UnlockButtonSize(300.0f);

        UObject* RerollBtnObj = RerollBtnBuilder.Build();
        RerollButton = std::make_unique<UI::Button>(RerollBtnObj);
        RerollButton->OnClicked([this]() {
            PalProcessor::Get().ProcessPal(TargetPal, true);
            if (MainScrollBoxObj && GetScrollOffsetFunc) {
                struct { float Offset; } Params{ 0.0f };
                MainScrollBoxObj->ProcessEvent(GetScrollOffsetFunc, &Params);
                LastScrollOffset = Params.Offset;
            }
            RequestRebuild(); 
        });

        auto InnerContentBox = UI::VerticalBox(MyWidget);

        InnerContentBox.AddToVerticalBox(
            UI::HorizontalBox(MyWidget).AddToHorizontalBox(
                UI::Text(MyWidget).Text(L"Current Swap:").Font(PalFont, L"Medium", 20).TextColor(Emerald),
                [](DynPals::BoxSlotBuilder& Slot) { Slot.Padding(0,0,0,10); } 
            )
        );

        InnerContentBox.AddToVerticalBox(
            DynPals::WidgetBuilder(L"/Script/UMG.SizeBox", MyWidget).AddChild(DynPals::WidgetBuilder(SkinDropdown->Build(MyWidget, CurrentPlayerController))),
            [](DynPals::BoxSlotBuilder& Slot) { Slot.Padding(0, 0, 0, 20); } 
        );

        InnerContentBox.AddToVerticalBox(
            WidgetBuilder(RerollBtnObj),
            [](DynPals::BoxSlotBuilder& Slot) { 
                Slot.Padding(20.0f, 0.0f, 20.0f, 15.0f)
                    .HorizontalAlignment(DynPals::EBuilderHorizontalAlignment::HAlign_Center); 
            } 
        );

        auto FilterRow = UI::HorizontalBox(MyWidget)
            .AddToHorizontalBox(DynPals::WidgetBuilder(HideInvalidSwitch->GetWidget()), [](DynPals::BoxSlotBuilder& Slot) { Slot.Padding(0,0,10,0); })
            .AddToHorizontalBox(UI::Text(MyWidget).Text(L"Hide Invalid").Font(PalFont, L"Medium", 18).TextColor(White), [](DynPals::BoxSlotBuilder& Slot) { Slot.VerticalAlignment(DynPals::EBuilderVerticalAlignment::VAlign_Center); });
        
        InnerContentBox.AddToVerticalBox(FilterRow, [](DynPals::BoxSlotBuilder& Slot) { Slot.Padding(0,0,0,25); });

        auto CameraSettingsRow = UI::HorizontalBox(MyWidget)
            .AddToHorizontalBox(DynPals::WidgetBuilder(FocusPalSwitch->GetWidget()), [](DynPals::BoxSlotBuilder& Slot) { Slot.Padding(0,0,10,0); })
            .AddToHorizontalBox(UI::Text(MyWidget).Text(L"Focus Pal Camera").Font(PalFont, L"Medium", 18).TextColor(White), [](DynPals::BoxSlotBuilder& Slot) { Slot.VerticalAlignment(DynPals::EBuilderVerticalAlignment::VAlign_Center); });
        
        InnerContentBox.AddToVerticalBox(CameraSettingsRow, [this](DynPals::BoxSlotBuilder& Slot) { 
            Slot.Padding(0, 0, 0, SaveManager::Get().Settings.bFocusPal ? 15.0f : 25.0f); 
        });

        if (SaveManager::Get().Settings.bFocusPal) {
            CameraRotationSlider = std::make_unique<UI::Slider>(MyWidget, 0.0, 360.0, SaveManager::Get().Settings.CameraRotation);
            CameraRotationSlider->OnChanged([this](double NewValue) {
                SaveManager::Get().Settings.CameraRotation = NewValue;
                SaveManager::Get().SaveWorldData();
                UpdatePalCameraRotation(NewValue);
            });

            InnerContentBox.AddToVerticalBox(
                UI::Text(MyWidget).Text(L"Camera Rotation").Font(PalFont, L"Medium", 18).TextColor(White)
            );
            InnerContentBox.AddToVerticalBox(DynPals::WidgetBuilder(CameraRotationSlider->GetWidget()), [](DynPals::BoxSlotBuilder& Slot) { Slot.Padding(0,5,0,25); });
        }

        if (currentPersist && persistConfigIndex != -1) {
            auto& activeCfg = ConfigManager::Get().GetConfigs()[persistConfigIndex];
            
            if (!activeCfg.MorphTargetList.empty()) {
                InnerContentBox.AddToVerticalBox(
                    UI::Text(MyWidget).Text(L"Morph Targets").Font(PalFont, L"Bold", 20).TextColor(Emerald),
                    [](DynPals::BoxSlotBuilder& Slot) { Slot.Padding(0,0,0,10); } 
                );

                for (auto& morph : activeCfg.MorphTargetList) {
                    if (morph.type != L"Restrict" && morph.minVal < morph.maxVal) {
                        float currentVal = (float)currentPersist->MorphSet[morph.target];
                        
                        InnerContentBox.AddToVerticalBox(
                            UI::Text(MyWidget).Text(morph.target).Font(PalFont, L"Medium", 18).TextColor(White)
                        );

                        auto SliderCtrl = std::make_unique<UI::Slider>(MyWidget, morph.minVal, morph.maxVal, currentVal);
                        
                        SliderCtrl->OnChanged([this, morphName = morph.target](double NewValue) {
                            PalPersistData* p = SaveManager::Get().GetPersistData(TargetInstanceID);
                            if (p) {
                                p->MorphSet[morphName] = NewValue;
                                SaveManager::Get().SetPersistData(TargetInstanceID, *p, true);
                                
                                UObject* MeshComp = nullptr;
                                Utils::CallFunction(TargetPal, STR("GetMainMesh"), &MeshComp);
                                if (MeshComp) {
                                    struct { FName MorphTargetName; float Value; bool bRemoveZeroWeight; } MorphParams{
                                        FName(morphName.c_str(), FNAME_Add), static_cast<float>(NewValue), false
                                    };
                                    Utils::CallFunction(MeshComp, STR("SetMorphTarget"), &MorphParams);
                                }
                            }
                        });

                        RC::Unreal::UObject* BuiltSlider = SliderCtrl->GetWidget();
                        MorphSliders.push_back({morph.target, std::move(SliderCtrl)});

                        InnerContentBox.AddToVerticalBox(DynPals::WidgetBuilder(BuiltSlider), [](DynPals::BoxSlotBuilder& Slot) { Slot.Padding(0,5,0,15); });
                    }
                }
            }
        }

        InnerContentBox.AddToVerticalBox(
            UI::Text(MyWidget).Text(L"Matchmaker Log").Font(PalFont, L"Bold", 20).TextColor(Emerald),
            [](DynPals::BoxSlotBuilder& Slot) { Slot.Padding(0,10,0,10); } 
        );

        auto LogVBox = UI::VerticalBox(MyWidget);

        if (evaluations.empty()) {
            LogVBox.AddToVerticalBox(UI::Text(MyWidget).Text(L"No swaps configured for this Pal.").TextColor(White));
        } else {
            for (const auto& eval : evaluations) {
                if (bHideInvalidSwaps && !eval.IsValid) continue;

                auto& cfg = ConfigManager::Get().GetConfigs()[eval.ConfigIndex];
                LogVBox.AddToVerticalBox(UI::Text(MyWidget).Text(cfg.PackName).Font(PalFont, L"Bold", 16).TextColor(White));

                std::wstring processedFilename = cfg.SkelMeshPath;
                size_t lastSlash = processedFilename.find_last_of(L'/');
                if (lastSlash != std::wstring::npos) processedFilename = processedFilename.substr(lastSlash + 1);
                size_t dotPos = processedFilename.find(L'.');
                if (dotPos != std::wstring::npos) processedFilename = processedFilename.substr(0, dotPos);

                if (processedFilename.rfind(L"SK_", 0) == 0 || processedFilename.rfind(L"sk_", 0) == 0) processedFilename = processedFilename.substr(3);
                for (wchar_t& c : processedFilename) { if (c == L'_') c = L' '; }

                double pct = 0.0;
                if (eval.IsValid && eval.Score == bestScore && totalTiedWeight > 0.0) {
                    pct = (cfg.SpawnWeight * 100.0) / totalTiedWeight;
                }

                FLinearColor_UE5 textColor = eval.IsValid ? (eval.Score < 0 ? PalBlue : (eval.Score == 0 ? Emerald : FLinearColor_UE5{0.960f, 0.620f, 0.043f, 1.0f})) : FLinearColor_UE5{0.850f, 0.150f, 0.150f, 1.0f};

                wchar_t pctBuf[16];
                swprintf(pctBuf, 16, L"%.1f", pct);
                std::wstring logStr = L"    " + std::wstring(pctBuf) + L"% : " + processedFilename;

                auto LogText = UI::Text(MyWidget).Text(logStr).TextColor(textColor);
                auto* WrapProp = Utils::GetProperty(LogText.Build(), STR("AutoWrapText"));
                if (WrapProp) {
                    bool* pWrap = WrapProp->ContainerPtrToValuePtr<bool>(LogText.Build());
                    if (pWrap) *pWrap = true;
                }

                LogVBox.AddToVerticalBox(LogText, [](DynPals::BoxSlotBuilder& Slot) { Slot.Padding(0,0,0,8); });
            }
        }

        InnerContentBox.AddToVerticalBox(LogVBox);

        auto MainScrollBoxBuilder = UI::ScrollBox(MyWidget).AddChild(InnerContentBox);
        MainScrollBoxObj = MainScrollBoxBuilder.Build();
        if (MainScrollBoxObj) {
            GetScrollOffsetFunc = MainScrollBoxObj->GetFunctionByNameInChain(STR("GetScrollOffset"));
        }

        auto MainContentConstrained = UI::SizeBox(MyWidget).HeightOverride(600.0f).AddChild(MainScrollBoxBuilder);

        auto HeaderBox = UI::HorizontalBox(MyWidget)
            .AddToHorizontalBox(UI::Image(MyWidget).ImageFromAsset(UI::Assets::Common::NoticeMark).ImageColor(PalBlue).ImageSize(24, 24), [](BoxSlotBuilder& Slot) { Slot.Padding(0, 0, 10, 0).VerticalAlignment(EBuilderVerticalAlignment::VAlign_Center); }) 
            .AddToHorizontalBox(UI::Text(MyWidget).Text(L"DYN PALS: " + TargetCharID).Font(PalFont, L"Bold", 24).TextOutline(2, {0.0f, 0.0f, 0.0f, 1.0f}).TextColor(PalBlue));

        UObject* Canvas = UI::WindowFrame(MyWidget, 650.0f)
            .SetHeader(HeaderBox)
            .AddContent(MainContentConstrained) 
            .SetFooter(UI::ActionBar(MyWidget))
            .Build(0.05, 0.5, 0.05, 0.5, 0.0, 0.5); 

        UObject* WidgetTree = nullptr;
        if (Utils::GetPropertyValue(MyWidget, STR("WidgetTree"), WidgetTree) && WidgetTree) {
            FProperty* RootProp = Utils::GetProperty(WidgetTree, STR("RootWidget"));
            if (RootProp) *RootProp->ContainerPtrToValuePtr<UObject*>(WidgetTree) = Canvas;
        }

        struct { int32_t ZOrder; } ViewportParams{9999};
        Utils::CallFunction(MyWidget, STR("AddToViewport"), &ViewportParams);

        if (LastScrollOffset > 0.0f && MainScrollBoxObj) {
            struct { float NewScrollOffset; } ScrollParams{LastScrollOffset};
            Utils::CallFunction(MainScrollBoxObj, STR("SetScrollOffset"), &ScrollParams);
        }
    }
    
    void UIManager::OnTickUI() {
        if (TargetPal && !Utils::IsObjectValid(TargetPal)) {
            TargetPal = nullptr;
            RequestToggle(); 
            return;
        }

        if (SkinDropdown)         SkinDropdown->Tick();
        if (HideInvalidSwitch)    HideInvalidSwitch->Tick();
        if (RerollButton)         RerollButton->Tick();
        if (FocusPalSwitch)       FocusPalSwitch->Tick();
        if (CameraRotationSlider) CameraRotationSlider->Tick();

        for (auto& as : MorphSliders) {
            if (as.SliderCtrl) as.SliderCtrl->Tick();
        }
        
        if (MainScrollBoxObj && GetScrollOffsetFunc) {
            struct { float Offset; } Params{ 0.0f };
            MainScrollBoxObj->ProcessEvent(GetScrollOffsetFunc, &Params);
            LastScrollOffset = Params.Offset;
        }

        
            
        
    }
}