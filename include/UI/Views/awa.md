### 3. `src/UI/Views/UIManager.cpp`
```cpp
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
#include <chrono>

using namespace RC;
using namespace RC::Unreal;

namespace DynPals {

    void UIManager::EnablePalCamera() {
        if (!CurrentPlayerController || !TargetPal || bIsPalCameraActive) return;

        if (OriginalViewTarget && !Utils::IsObjectValid(OriginalViewTarget)) {
            OriginalViewTarget = nullptr;
        }

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
        UFunction* SetViewTargetFunc = CurrentPlayerController->GetFunctionByNameInChain(STR("SetViewTargetWithBlend"));
        if (SetViewTargetFunc) {
            alignas(8) uint8_t SetViewParams[128] = {0};
            FProperty* TargetProp = SetViewTargetFunc->GetPropertyByNameInChain(STR("NewViewTarget"));
            if (TargetProp) *TargetProp->ContainerPtrToValuePtr<UObject*>(SetViewParams) = TargetPal;
            CurrentPlayerController->ProcessEvent(SetViewTargetFunc, SetViewParams);
            bIsPalCameraActive = true;
        }
    }

    void UIManager::DisablePalCamera() {
        if (!bIsPalCameraActive || !CurrentPlayerController) return;

        if (OriginalViewTarget && !Utils::IsObjectValid(OriginalViewTarget)) {
            OriginalViewTarget = nullptr;
        }

        UFunction* SetViewTargetFunc = CurrentPlayerController->GetFunctionByNameInChain(STR("SetViewTargetWithBlend"));
        if (SetViewTargetFunc) {
            alignas(8) uint8_t Params[128] = {0};
            FProperty* TargetProp = SetViewTargetFunc->GetPropertyByNameInChain(STR("NewViewTarget"));
            if (TargetProp) {
                *TargetProp->ContainerPtrToValuePtr<UObject*>(Params) = OriginalViewTarget ? OriginalViewTarget : CurrentPlayerController;
            }
            CurrentPlayerController->ProcessEvent(SetViewTargetFunc, Params);
        }

        bIsPalCameraActive = false;
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

            if (dot >= 0.80) {
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
            FProperty* RetProp = GetViewTargetFunc->GetPropertyByNameInChain(STR("ReturnValue"));
            if (RetProp) OriginalViewTarget = *RetProp->ContainerPtrToValuePtr<UObject*>(Params);
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
    void UIManager::OnInvalidate() {
        TargetPal = nullptr;
        TargetInstanceID = L"";
        TargetCharID = L"";
        SkinDropdown = nullptr;
        
        if (PreloadContainer) {
            Utils::CallFunction(PreloadContainer, STR("RemoveFromParent"));
            PreloadContainer = nullptr;
        }

        HideInvalidSwitch = nullptr;
        RerollButton = nullptr;
        MorphSliderPool.clear(); 
        ActiveMorphSlidersCount = 0;
        FocusPalSwitch = nullptr;
        CameraRotationSlider = nullptr;
        MainScrollBoxObj = nullptr;
        GetScrollOffsetFunc = nullptr;
        
        DynamicMorphBox = nullptr;
        DynamicLogBox = nullptr;
        CameraRotationContainer = nullptr;
        HeaderTextObj = nullptr;
        WidgetTrashBin = nullptr;

        LogTextPool.clear();
        DropdownOptions.clear();
        DropdownConfigIndices.clear();

        OriginalViewTarget = nullptr;
        bIsPalCameraActive = false;
    }
    void UIManager::OnOpen() {
        // Trigger data population and dynamic UI layout immediately upon showing the UI
        RefreshUI();
    }

    void UIManager::OnClose() {
        TargetPal = nullptr;
        TargetInstanceID = L"";
        TargetCharID = L"";

        DisablePalCamera();
        OriginalViewTarget = nullptr;
    }


    void UIManager::PreloadUI(RC::Unreal::UObject* PC) {
        if (!SkinDropdown) {
            SkinDropdown = std::make_unique<class DynPals::UI::Dropdown>(std::vector<std::wstring>{}, 0);
        }
        
        UObject* WBL = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/UMG.Default__WidgetBlueprintLibrary"));
        UClass* WidgetClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.UserWidget"));
        if (!WBL || !WidgetClass) return;

        struct { UObject* WorldContext; UClass* WidgetType; UObject* OwningPlayer; UObject* ReturnValue; } CreateParams{
            PC, WidgetClass, PC, nullptr
        };
        Utils::CallFunction(WBL, STR("Create"), &CreateParams);
        PreloadContainer = CreateParams.ReturnValue;

        if (PreloadContainer) {
            UObject* ScrollBox = UI::ScrollBox(PreloadContainer).Build();
            
            UObject* WidgetTree = nullptr;
            if (Utils::GetPropertyValue(PreloadContainer, STR("WidgetTree"), WidgetTree) && WidgetTree) {
                FProperty* RootProp = Utils::GetProperty(WidgetTree, STR("RootWidget"));
                if (RootProp) *RootProp->ContainerPtrToValuePtr<UObject*>(WidgetTree) = ScrollBox;
            }

            struct { uint8_t InVisibility; } VisParams{ 1 }; // Collapsed
            Utils::CallFunction(PreloadContainer, STR("SetVisibility"), &VisParams);
            
            struct { int32_t ZOrder; } ViewportParams{ -9999 };
            Utils::CallFunction(PreloadContainer, STR("AddToViewport"), &ViewportParams);

            SkinDropdown->SetTrashBin(ScrollBox);
            SkinDropdown->PreloadPool(PreloadContainer, 50, 10);
            
            for (auto& header : SkinDropdown->GetHeaderPool()) {
                if (header.RootWidget && ScrollBox) {
                    struct { UObject* Content; UObject* ReturnValue; } AddParams{header.RootWidget, nullptr};
                    Utils::CallFunction(ScrollBox, STR("AddChild"), &AddParams);
                }
            }
            for (auto& btn : SkinDropdown->GetButtonPool()) {
                if (btn.RootWidget && ScrollBox) {
                    struct { UObject* Content; UObject* ReturnValue; } AddParams{btn.RootWidget, nullptr};
                    Utils::CallFunction(ScrollBox, STR("AddChild"), &AddParams);
                }
            }
        }
    }

    void UIManager::BuildWidget() {
        if (!CurrentPlayerController || !TargetPal) return;

        
        LogTextPool.clear();
        MorphSliderPool.clear();
        ActiveMorphSlidersCount = 0;

        UObject* WBL = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/UMG.Default__WidgetBlueprintLibrary"));
        UClass* WidgetClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.UserWidget"));
        if (!WBL || !WidgetClass) return;

        struct { UObject* WorldContext; UClass* WidgetType; UObject* OwningPlayer; UObject* ReturnValue; } CreateParams{
            CurrentPlayerController, WidgetClass, CurrentPlayerController, nullptr
        };
        Utils::CallFunction(WBL, STR("Create"), &CreateParams);
        MyWidget = CreateParams.ReturnValue;

        PalFontCache = Utils::LoadAssetSafely(UI::Assets::Fonts::PalDefault);
        const FLinearColor_UE5 PalBlue = {0.78f, 0.96f, 1.0f, 1.0f}; 
        const FLinearColor_UE5 White   = {1.0f, 1.0f, 1.0f, 1.0f};
        const FLinearColor_UE5 Emerald = {0.063f, 0.725f, 0.506f, 1.0f};

        // Pre-build persistent Dynamic containers
        DynamicMorphBox = UI::VerticalBox(MyWidget).Build();
        DynamicLogBox = UI::VerticalBox(MyWidget).Build();
        CameraRotationContainer = UI::VerticalBox(MyWidget).Build();

        // Create Trash Bin to hold unused pooled widgets and prevent garbage collection
        WidgetTrashBin = UI::VerticalBox(MyWidget).Build();
        struct { uint8_t InVisibility; } VisParams{ 1 }; // Collapsed
        Utils::CallFunction(WidgetTrashBin, STR("SetVisibility"), &VisParams);

        if (!SkinDropdown) {
            SkinDropdown = std::make_unique<class DynPals::UI::Dropdown>(std::vector<std::wstring>{}, 0);
        }
        SkinDropdown->SetTrashBin(WidgetTrashBin);

        SkinDropdown->OnChanged([this](int Index, std::wstring Choice) {
            if (Index >= 0 && Index < static_cast<int>(DropdownConfigIndices.size())) {
                int TargetConfig = DropdownConfigIndices[Index];
                PalProcessor::Get().ForceSwap(TargetPal, TargetConfig);
                
                if (MainScrollBoxObj && GetScrollOffsetFunc) {
                    struct { float Offset; } Params{ 0.0f };
                    MainScrollBoxObj->ProcessEvent(GetScrollOffsetFunc, &Params);
                    LastScrollOffset = Params.Offset;
                }
                RefreshUI(); 
            }
        });

        HideInvalidSwitch = std::make_unique<class DynPals::UI::Switch>(MyWidget, bHideInvalidSwaps);
        HideInvalidSwitch->OnChanged([this](bool bState) {
            bHideInvalidSwaps = bState;
            if (MainScrollBoxObj && GetScrollOffsetFunc) {
                struct { float Offset; } Params{ 0.0f };
                MainScrollBoxObj->ProcessEvent(GetScrollOffsetFunc, &Params);
                LastScrollOffset = Params.Offset;
            }
            RefreshUI(); 
        });

        FocusPalSwitch = std::make_unique<class DynPals::UI::Switch>(MyWidget, SaveManager::Get().Settings.bFocusPal);
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
            RefreshUI(); 
        });

        auto RerollBtnBuilder = WidgetBuilder(UI::Assets::Blueprints::CommonButton, MyWidget)
            .Text(L"      Reroll Pal      ")
            .BackgroundColor(PalBlue)
            .DesiredSizeOverride(300.0f, 45.0f)
            .UnlockButtonSize(300.0f);

        UObject* RerollBtnObj = RerollBtnBuilder.Build();
        RerollButton = std::make_unique<class DynPals::UI::Button>(RerollBtnObj);
        RerollButton->OnClicked([this]() {
            PalProcessor::Get().ProcessPal(TargetPal, true);
            if (MainScrollBoxObj && GetScrollOffsetFunc) {
                struct { float Offset; } Params{ 0.0f };
                MainScrollBoxObj->ProcessEvent(GetScrollOffsetFunc, &Params);
                LastScrollOffset = Params.Offset;
            }
            RefreshUI(); 
        });

        auto InnerContentBox = UI::VerticalBox(MyWidget);

        InnerContentBox.AddToVerticalBox(
            UI::HorizontalBox(MyWidget).AddToHorizontalBox(
                UI::Text(MyWidget).Text(L"Current Swap:").Font(PalFontCache, L"Medium", 20).TextColor(Emerald),
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
            .AddToHorizontalBox(UI::Text(MyWidget).Text(L"Hide Invalid").Font(PalFontCache, L"Medium", 18).TextColor(White), [](DynPals::BoxSlotBuilder& Slot) { Slot.VerticalAlignment(DynPals::EBuilderVerticalAlignment::VAlign_Center); });
        
        InnerContentBox.AddToVerticalBox(FilterRow, [](DynPals::BoxSlotBuilder& Slot) { Slot.Padding(0,0,0,25); });

        auto CameraSettingsRow = UI::HorizontalBox(MyWidget)
            .AddToHorizontalBox(DynPals::WidgetBuilder(FocusPalSwitch->GetWidget()), [](DynPals::BoxSlotBuilder& Slot) { Slot.Padding(0,0,10,0); })
            .AddToHorizontalBox(UI::Text(MyWidget).Text(L"Focus Pal Camera").Font(PalFontCache, L"Medium", 18).TextColor(White), [](DynPals::BoxSlotBuilder& Slot) { Slot.VerticalAlignment(DynPals::EBuilderVerticalAlignment::VAlign_Center); });
        
        InnerContentBox.AddToVerticalBox(CameraSettingsRow, [](DynPals::BoxSlotBuilder& Slot) { Slot.Padding(0,0,0,15); });

        // Add pre-constructed empty containers
        InnerContentBox.AddToVerticalBox(DynPals::WidgetBuilder(CameraRotationContainer), [](DynPals::BoxSlotBuilder& Slot) { Slot.Padding(0,0,0,10); });
        InnerContentBox.AddToVerticalBox(DynPals::WidgetBuilder(DynamicMorphBox), [](DynPals::BoxSlotBuilder& Slot) { Slot.Padding(0,0,0,10); });
        InnerContentBox.AddToVerticalBox(DynPals::WidgetBuilder(DynamicLogBox), [](DynPals::BoxSlotBuilder& Slot) { Slot.Padding(0,10,0,10); });
        InnerContentBox.AddToVerticalBox(DynPals::WidgetBuilder(WidgetTrashBin)); // Must be added to hierarchy

        auto MainScrollBoxBuilder = UI::ScrollBox(MyWidget).AddChild(InnerContentBox);
        MainScrollBoxObj = MainScrollBoxBuilder.Build();
        if (MainScrollBoxObj) {
            GetScrollOffsetFunc = MainScrollBoxObj->GetFunctionByNameInChain(STR("GetScrollOffset"));
        }

        auto MainContentConstrained = UI::SizeBox(MyWidget).HeightOverride(600.0f).AddChild(MainScrollBoxBuilder);

        auto HeaderTextBuilder = UI::Text(MyWidget).Text(L"DYN PALS: " + TargetCharID).Font(PalFontCache, L"Bold", 24).TextOutline(2, {0.0f, 0.0f, 0.0f, 1.0f}).TextColor(PalBlue);
        HeaderTextObj = HeaderTextBuilder.Build();

        auto HeaderBox = UI::HorizontalBox(MyWidget)
            .AddToHorizontalBox(UI::Image(MyWidget).ImageFromAsset(UI::Assets::Common::NoticeMark).ImageColor(PalBlue).ImageSize(24, 24), [](BoxSlotBuilder& Slot) { Slot.Padding(0, 0, 10, 0).VerticalAlignment(EBuilderVerticalAlignment::VAlign_Center); }) 
            .AddToHorizontalBox(DynPals::WidgetBuilder(HeaderTextObj));


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
    }

    void UIManager::RefreshUI() {
        if (!TargetPal || !DynamicLogBox || !DynamicMorphBox || !CameraRotationContainer) return;

        const FLinearColor_UE5 PalBlue = {0.78f, 0.96f, 1.0f, 1.0f}; 
        const FLinearColor_UE5 White   = {1.0f, 1.0f, 1.0f, 1.0f};
        const FLinearColor_UE5 Emerald = {0.063f, 0.725f, 0.506f, 1.0f};

        // Clear visual containers immediately
        Utils::CallFunction(CameraRotationContainer, STR("ClearChildren"));
        Utils::CallFunction(DynamicMorphBox, STR("ClearChildren"));
        Utils::CallFunction(DynamicLogBox, STR("ClearChildren"));


        // 1. Dynamic Header Text Update
        if (HeaderTextObj) {
            RC::Unreal::UObject* KTL = DynPals::Utils::GetKTL();
            RC::Unreal::UFunction* ConvFunc = DynPals::Utils::GetKTLFunction(STR("Conv_StringToText"));
            if (KTL && ConvFunc) {
                std::wstring headerStr = L"DYN PALS: " + TargetCharID;
                struct { RC::Unreal::FString InString; RC::Unreal::FText ReturnValue; } P1{ RC::Unreal::FString(headerStr.c_str()), RC::Unreal::FText() };
                KTL->ProcessEvent(ConvFunc, &P1);
                struct { RC::Unreal::FText InText; } P2{P1.ReturnValue};
                Utils::CallFunction(HeaderTextObj, STR("SetText"), &P2, true);
            }
        }

        // 2. Fetch Pal Statistics
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

        // 3. Dropdown Reconstruction with Structural Diffing Check
        std::vector<std::wstring> newDropdownOptions;
        std::vector<int> newDropdownConfigIndices;

        for (auto& [packName, evals] : rawPacks) {
            std::map<std::wstring, int> prefixCounts; 
            
            std::wstring headerStr = L"[ " + packName + L" ]";
            newDropdownOptions.push_back(headerStr);
            newDropdownConfigIndices.push_back(-1); 

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
                        for (size_t i = prefixTokens; i < tokens.size(); ++i) {
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
                
                newDropdownOptions.push_back(L"   " + display);
                newDropdownConfigIndices.push_back(eval.ConfigIndex);
            }
        }

        bool bOptionsChanged = false;
        if (newDropdownOptions.size() != DropdownOptions.size()) {
            bOptionsChanged = true;
        } else {
            for (size_t i = 0; i < newDropdownOptions.size(); ++i) {
                if (newDropdownOptions[i] != DropdownOptions[i] || newDropdownConfigIndices[i] != DropdownConfigIndices[i]) {
                    bOptionsChanged = true;
                    break;
                }
            }
        }

        int persistConfigIndex = currentPersist ? ConfigManager::Get().FindConfigIndex(currentPersist->PackName, currentPersist->SkinName, currentPersist->SwapLabel, currentPersist->SkelMeshPath, TargetCharID) : -1;
        int initialIdx = 0;

        for (size_t i = 0; i < newDropdownConfigIndices.size(); ++i) {
            if (newDropdownConfigIndices[i] == persistConfigIndex) {
                initialIdx = static_cast<int>(i);
                break;
            }
        }

        static std::wstring lastTargetInstanceID = L"";
        static int lastPersistConfigIndex = -1;

        if (bOptionsChanged) {
            DropdownOptions = std::move(newDropdownOptions);
            DropdownConfigIndices = std::move(newDropdownConfigIndices);
            lastTargetInstanceID = TargetInstanceID;
            lastPersistConfigIndex = persistConfigIndex;

            if (SkinDropdown) {
                SkinDropdown->SetOptions(DropdownOptions, initialIdx);
            }
        } else if (TargetInstanceID != lastTargetInstanceID || persistConfigIndex != lastPersistConfigIndex) {
            lastTargetInstanceID = TargetInstanceID;
            lastPersistConfigIndex = persistConfigIndex;

            if (SkinDropdown) {
                SkinDropdown->SetOptions(DropdownOptions, initialIdx);
            }
        }

        // 4. Text Widget Pooling Lambda (Mitigates GC Overhead & Avoids Pointer Leakage)
        int logTextUsed = 0;
        auto GetPooledText = [&](const std::wstring& TextStr, const FLinearColor_UE5& Color, int32_t FontSize, const wchar_t* Typeface) -> RC::Unreal::UObject* {
            RC::Unreal::UObject* TextObj = nullptr;
            if (logTextUsed < LogTextPool.size()) {
                TextObj = LogTextPool[logTextUsed];
                if (!Utils::IsObjectValid(TextObj)) {
                    // Failsafe if GC bypassed the trash bin
                    auto Builder = UI::Text(MyWidget);
                    TextObj = Builder.Build();
                    LogTextPool[logTextUsed] = TextObj;
                } else {
                    Utils::CallFunction(TextObj, STR("RemoveFromParent"));
                }
            } else {
                auto Builder = UI::Text(MyWidget);
                TextObj = Builder.Build();
                LogTextPool.push_back(TextObj);
            }
            logTextUsed++;

            RC::Unreal::UObject* KTL = DynPals::Utils::GetKTL();
            RC::Unreal::UFunction* ConvFunc = DynPals::Utils::GetKTLFunction(STR("Conv_StringToText"));
            if (KTL && ConvFunc) {
                struct { RC::Unreal::FString InString; RC::Unreal::FText ReturnValue; } P1{ RC::Unreal::FString(TextStr.c_str()), RC::Unreal::FText() };
                KTL->ProcessEvent(ConvFunc, &P1);
                struct { RC::Unreal::FText InText; } P2{P1.ReturnValue};
                Utils::CallFunction(TextObj, STR("SetText"), &P2, true);
            }

            DynPals::UI::SetTextColor(TextObj, Color);
            
            FProperty* FontProp = Utils::GetProperty(TextObj, STR("Font"));
            if (FontProp) {
                void* FontPtr = FontProp->ContainerPtrToValuePtr<void>(TextObj);
                if (FontPtr) {
                    FStructProperty* StructProp = static_cast<FStructProperty*>(FontProp);
                    if (StructProp && StructProp->GetStruct()) {
                        UStruct* FontStruct = StructProp->GetStruct();
                        
                        FProperty* ObjProp = FontStruct->GetPropertyByNameInChain(STR("FontObject"));
                        if (ObjProp && PalFontCache) {
                            UObject** Ptr = ObjProp->ContainerPtrToValuePtr<UObject*>(FontPtr);
                            if (Ptr) *Ptr = PalFontCache;
                        }

                        FProperty* NameProp = FontStruct->GetPropertyByNameInChain(STR("TypefaceFontName"));
                        if (NameProp) {
                            FName* NamePtr = NameProp->ContainerPtrToValuePtr<FName>(FontPtr);
                            if (NamePtr) *NamePtr = FName(Typeface, FNAME_Add);
                        }

                        FProperty* SizeProp = FontStruct->GetPropertyByNameInChain(STR("Size"));
                        if (SizeProp) {
                            int32_t* SizePtr = SizeProp->ContainerPtrToValuePtr<int32_t>(FontPtr);
                            if (SizePtr) *SizePtr = FontSize;
                        }
                    }
                }
                Utils::CallFunction(TextObj, STR("SetFont"), FontPtr);
            }

            auto* WrapProp = Utils::GetProperty(TextObj, STR("AutoWrapText"));
            if (WrapProp) {
                bool* pWrap = WrapProp->ContainerPtrToValuePtr<bool>(TextObj);
                if (pWrap) *pWrap = true;
            }

            struct { uint8_t InVisibility; } VisParams{ 0 }; 
            Utils::CallFunction(TextObj, STR("SetVisibility"), &VisParams);

            return TextObj;
        };

        // 5. Camera Rotation Logic
        Utils::CallFunction(CameraRotationContainer, STR("ClearChildren"));
        if (SaveManager::Get().Settings.bFocusPal) {
            CameraRotationSlider = std::make_unique<class DynPals::UI::Slider>(MyWidget, 0.0, 360.0, SaveManager::Get().Settings.CameraRotation);
            CameraRotationSlider->OnChanged([this](double NewValue) {
                SaveManager::Get().Settings.CameraRotation = NewValue;
                SaveManager::Get().SaveWorldData();
                UpdatePalCameraRotation(NewValue);
            });

            RC::Unreal::UObject* TitleObj = GetPooledText(L"Camera Rotation", White, 18, L"Medium");
            struct { RC::Unreal::UObject* Content; RC::Unreal::UObject* ReturnValue; } AddL{TitleObj, nullptr};
            Utils::CallFunction(CameraRotationContainer, STR("AddChildToVerticalBox"), &AddL);
            
            struct { RC::Unreal::UObject* Content; RC::Unreal::UObject* ReturnValue; } AddS{CameraRotationSlider->GetWidget(), nullptr};
            Utils::CallFunction(CameraRotationContainer, STR("AddChildToVerticalBox"), &AddS);
            
            if (AddS.ReturnValue) {
                DynPals::BoxSlotBuilder SlotBuilder(AddS.ReturnValue);
                SlotBuilder.Padding(0.0f, 5.0f, 0.0f, 20.0f);
            }
        } else {
            CameraRotationSlider = nullptr;
        }

        // 6. Slider Pooling & Shape Keys
        Utils::CallFunction(DynamicMorphBox, STR("ClearChildren"));
        ActiveMorphSlidersCount = 0;
        
        if (currentPersist && persistConfigIndex != -1) {
            auto& activeCfg = ConfigManager::Get().GetConfigs()[persistConfigIndex];
            
            if (!activeCfg.MorphTargetList.empty()) {
                RC::Unreal::UObject* MTitleObj = GetPooledText(L"Morph Targets", Emerald, 20, L"Bold");
                struct { RC::Unreal::UObject* Content; RC::Unreal::UObject* ReturnValue; } AddMT{MTitleObj, nullptr};
                Utils::CallFunction(DynamicMorphBox, STR("AddChildToVerticalBox"), &AddMT);
                
                if (AddMT.ReturnValue) {
                    DynPals::BoxSlotBuilder SlotBuilder(AddMT.ReturnValue);
                    SlotBuilder.Padding(0.0f, 0.0f, 0.0f, 10.0f);
                }

                int slidersUsed = 0;
                for (auto& morph : activeCfg.MorphTargetList) {
                    if (morph.type != L"Restrict" && morph.minVal < morph.maxVal) {
                        float currentVal = (float)currentPersist->MorphSet[morph.target];
                        
                        RC::Unreal::UObject* LblObj = GetPooledText(morph.target, White, 18, L"Medium");
                        struct { RC::Unreal::UObject* Content; RC::Unreal::UObject* ReturnValue; } AddL{LblObj, nullptr};
                        Utils::CallFunction(DynamicMorphBox, STR("AddChildToVerticalBox"), &AddL);

                        class DynPals::UI::Slider* SliderCtrl = nullptr;
                        if (slidersUsed < static_cast<int>(MorphSliderPool.size())) {
                            SliderCtrl = MorphSliderPool[slidersUsed].get();
                            RC::Unreal::UObject* SliderW = SliderCtrl->GetWidget();
                            if (Utils::IsObjectValid(SliderW)) {
                                Utils::CallFunction(SliderW, STR("RemoveFromParent"));
                            }
                            SliderCtrl->UpdateValue(currentVal, morph.minVal, morph.maxVal);
                        } else {
                            auto NewSlider = std::make_unique<class DynPals::UI::Slider>(MyWidget, morph.minVal, morph.maxVal, currentVal);
                            SliderCtrl = NewSlider.get();
                            MorphSliderPool.push_back(std::move(NewSlider));
                        }
                        slidersUsed++;

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

                        struct { RC::Unreal::UObject* Content; RC::Unreal::UObject* ReturnValue; } AddS{BuiltSlider, nullptr};
                        Utils::CallFunction(DynamicMorphBox, STR("AddChildToVerticalBox"), &AddS);
                        if (AddS.ReturnValue) {
                            DynPals::BoxSlotBuilder SlotBuilder(AddS.ReturnValue);
                            SlotBuilder.Padding(0.0f, 5.0f, 0.0f, 15.0f);
                        }
                    }
                }
                ActiveMorphSlidersCount = slidersUsed;
            }
        }

        // 7. Dynamic Log Output
        Utils::CallFunction(DynamicLogBox, STR("ClearChildren"));
        
        RC::Unreal::UObject* LogTitleObj = GetPooledText(L"Matchmaker Log", Emerald, 20, L"Bold");
        struct { RC::Unreal::UObject* Content; RC::Unreal::UObject* ReturnValue; } AddLogT{LogTitleObj, nullptr};
        Utils::CallFunction(DynamicLogBox, STR("AddChildToVerticalBox"), &AddLogT);
        if (AddLogT.ReturnValue) {
            DynPals::BoxSlotBuilder SlotBuilder(AddLogT.ReturnValue);
            SlotBuilder.Padding(0.0f, 10.0f, 0.0f, 10.0f);
        }

        if (evaluations.empty()) {
            RC::Unreal::UObject* EmptyLogObj = GetPooledText(L"No swaps configured for this Pal.", White, 16, L"Regular");
            struct { RC::Unreal::UObject* Content; RC::Unreal::UObject* ReturnValue; } AddE{EmptyLogObj, nullptr};
            Utils::CallFunction(DynamicLogBox, STR("AddChildToVerticalBox"), &AddE);
        } else {
            for (const auto& eval : evaluations) {
                if (bHideInvalidSwaps && !eval.IsValid) continue;

                auto& cfg = ConfigManager::Get().GetConfigs()[eval.ConfigIndex];
                
                RC::Unreal::UObject* PackObj = GetPooledText(cfg.PackName, White, 16, L"Bold");
                struct { RC::Unreal::UObject* Content; RC::Unreal::UObject* ReturnValue; } AddP{PackObj, nullptr};
                Utils::CallFunction(DynamicLogBox, STR("AddChildToVerticalBox"), &AddP);

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

                RC::Unreal::UObject* LogTxtObj = GetPooledText(logStr, textColor, 16, L"Medium");

                struct { RC::Unreal::UObject* Content; RC::Unreal::UObject* ReturnValue; } AddL{LogTxtObj, nullptr};
                Utils::CallFunction(DynamicLogBox, STR("AddChildToVerticalBox"), &AddL);
                if (AddL.ReturnValue) {
                    DynPals::BoxSlotBuilder SlotBuilder(AddL.ReturnValue);
                    SlotBuilder.Padding(0.0f, 0.0f, 0.0f, 8.0f);
                }
            }
        }

        // 8. Move unused pooled elements to the Trash Bin to retain allocation
        for (size_t i = logTextUsed; i < LogTextPool.size(); ++i) {
            RC::Unreal::UObject* UnusedTxt = LogTextPool[i];
            if (Utils::IsObjectValid(UnusedTxt)) {
                Utils::CallFunction(UnusedTxt, STR("RemoveFromParent"));
                struct { RC::Unreal::UObject* Content; RC::Unreal::UObject* ReturnValue; } AddT{UnusedTxt, nullptr};
                Utils::CallFunction(WidgetTrashBin, STR("AddChildToVerticalBox"), &AddT);
            }
        }

        for (size_t i = ActiveMorphSlidersCount; i < MorphSliderPool.size(); ++i) {
            RC::Unreal::UObject* UnusedSlider = MorphSliderPool[i]->GetWidget();
            if (Utils::IsObjectValid(UnusedSlider)) {
                Utils::CallFunction(UnusedSlider, STR("RemoveFromParent"));
                struct { RC::Unreal::UObject* Content; RC::Unreal::UObject* ReturnValue; } AddS{UnusedSlider, nullptr};
                Utils::CallFunction(WidgetTrashBin, STR("AddChildToVerticalBox"), &AddS);
            }
        }

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

        for (int i = 0; i < ActiveMorphSlidersCount; ++i) {
            if (i < static_cast<int>(MorphSliderPool.size())) {
                MorphSliderPool[i]->Tick();
            }
        }
        
        if (MainScrollBoxObj && GetScrollOffsetFunc) {
            struct { float Offset; } Params{ 0.0f };
            MainScrollBoxObj->ProcessEvent(GetScrollOffsetFunc, &Params);
            LastScrollOffset = Params.Offset;
        }
    }
}
```

### 4. `include/UI/Views/TestUI.hpp`
```cpp
#pragma once
#include "UI/UIBase.hpp"
#include "UI/Components/Button.hpp"
#include "UI/Components/Dropdown.hpp"
#include "UI/Components/Slider.hpp"
#include "UI/Components/Switch.hpp"
#include "UI/Components/Selector.hpp"
#include <vector>
#include <string>
#include <memory>

namespace DynPals {

    class TestUI : public UIBase {
    public:
        static TestUI& Get() {
            static TestUI instance;
            return instance;
        }

    protected:
        virtual void BuildWidget() override;
        virtual void OnTickUI() override;

    private:
        TestUI() { bCloseOnEscape = false; }

        // Tab State Tracking
        int32_t ActiveTab = 0;
        std::unique_ptr<class DynPals::UI::Button> TabBtn1;
        std::unique_ptr<class DynPals::UI::Button> TabBtn2;

        // Tab 0: Vanilla Controls Elements
        std::unique_ptr<class DynPals::UI::Button> HighlightButton;
        std::vector<RC::Unreal::UObject*> TextBlocks;
        std::vector<RC::Unreal::UObject*> RowIcons;
        bool bHighlight = false;

        // Tab 1: Native Options Elements
        std::unique_ptr<class DynPals::UI::Slider> TestSlider;
        std::unique_ptr<class DynPals::UI::Switch> TestSwitch;
        std::unique_ptr<class DynPals::UI::Selector> TestLR;

        // Tab 1: Native Select List Popup
        std::unique_ptr<class DynPals::UI::Dropdown> TestDropdown;
        std::wstring CurrentDropdownChoice = L"Option A";
        std::vector<std::wstring> DropdownOptions;
    };
}
```

### 5. `src/UI/Views/TestUI.cpp`
```cpp
#define NOMINMAX 
#include <Windows.h>

#include "UI/Views/TestUI.hpp"
#include "UI/Components/WindowFrame.hpp"
#include "UI/WidgetBuilder.hpp"
#include "UI/IconLibrary.hpp"
#include "Utils.hpp"

using namespace RC;
using namespace RC::Unreal;

namespace DynPals {

    static bool IsWidgetPressed(RC::Unreal::UObject* Widget) {
        if (!Widget) return false;
        RC::Unreal::UObject* TargetBtn = Widget;
        RC::Unreal::UObject* Temp = nullptr;

        if (Utils::GetPropertyValue(TargetBtn, STR("WBP_PalCommonButton"), Temp) && Temp) TargetBtn = Temp;
        if (Utils::GetPropertyValue(TargetBtn, STR("WBP_PalInvisibleButton"), Temp) && Temp) TargetBtn = Temp;

        struct { bool RetVal; } Params{false};
        Utils::CallFunction(TargetBtn, STR("IsPressed"), &Params);
        return Params.RetVal;
    }

    void TestUI::BuildWidget() {
        if (!CurrentPlayerController) return;

        TabBtn1 = nullptr;
        TabBtn2 = nullptr;
        TestSlider = nullptr;
        TestSwitch = nullptr;
        TestLR = nullptr;
        HighlightButton = nullptr;
        TextBlocks.clear();
        RowIcons.clear();

        if (DropdownOptions.empty()) {
            DropdownOptions = {
                L"Option A", L"Option B", L"Option C", L"Option D", L"Option E",
                L"Option F", L"Option G", L"Option H", L"Option I", L"Option J",
                L"Option K", L"Option L", L"Option M", L"Option N", L"Option O",
                L"Option P", L"Option Q", L"Option R", L"Option S", L"Option T",
                L"Option U", L"Option V", L"Option W", L"Option X", L"Option Y",
                L"Option Z"
            };
        }

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

        auto HeaderBox = UI::HorizontalBox(MyWidget)
            .AddToHorizontalBox(UI::Image(MyWidget).ImageFromAsset(UI::Assets::Common::NoticeMark).ImageColor(PalBlue).ImageSize(24, 24), [](BoxSlotBuilder& Slot) { Slot.Padding(0, 0, 10, 0).VerticalAlignment(EBuilderVerticalAlignment::VAlign_Center); })
            .AddToHorizontalBox(UI::Text(MyWidget).Text(L"DYN PALS SYSTEM TEST").Font(PalFont, L"Bold", 24).TextOutline(2, {0.0f, 0.0f, 0.0f, 1.0f}).TextColor(PalBlue));

        auto TabLayout = UI::HorizontalBox(MyWidget);
        auto Tab1Builder = UI::OptionTab(MyWidget).SetupTab(L"Settings", 0).SetTabActive(ActiveTab == 0);
        auto Tab2Builder = UI::OptionTab(MyWidget).SetupTab(L"Visuals", 1).SetTabActive(ActiveTab == 1);

        UObject* Tab1Widget = Tab1Builder.Build();
        UObject* Tab2Widget = Tab2Builder.Build();

        if (ActiveTab == 0) Utils::CallFunction(Tab1Widget, STR("SetTabActive"), &bHighlight); 
        else Utils::CallFunction(Tab2Widget, STR("SetTabActive"), &bHighlight);

        TabBtn1 = std::make_unique<class DynPals::UI::Button>(Tab1Widget);
        TabBtn1->OnClicked([this]() {
            if (ActiveTab != 0) { ActiveTab = 0; if (TestDropdown) TestDropdown->ClosePopup(); RequestRebuild(); }
        });

        TabBtn2 = std::make_unique<class DynPals::UI::Button>(Tab2Widget);
        TabBtn2->OnClicked([this]() {
            if (ActiveTab != 1) { ActiveTab = 1; RequestRebuild(); }
        });

        TabLayout.AddToHorizontalBox(Tab1Builder, [](BoxSlotBuilder& Slot) { Slot.Padding(0, 0, 10, 0); }).AddToHorizontalBox(Tab2Builder);

        auto ContentContainer = UI::VerticalBox(MyWidget);

        if (ActiveTab == 0) {
            auto ButtonBuilder = WidgetBuilder(UI::Assets::Blueprints::CommonButton, MyWidget)
                .Text(L"Highlight every second element")
                .DesiredSizeOverride(400.0f, 45.0f)
                .UnlockButtonSize(400.0f); // Unlocks internal truncation limits
            
            UObject* ButtonRoot = ButtonBuilder.Build();

            HighlightButton = std::make_unique<class DynPals::UI::Button>(ButtonRoot);
            HighlightButton->OnClicked([this]() {
                bHighlight = !bHighlight; 

                const FLinearColor_UE5 PalBlue = {0.78f, 0.96f, 1.0f, 1.0f};
                const FLinearColor_UE5 White   = {1.0f, 1.0f, 1.0f, 1.0f};
                const FLinearColor_UE5 DarkOut = {0.0f, 0.0f, 0.0f, 0.8f};
                const FLinearColor_UE5 SoftOut = {0.0f, 0.0f, 0.0f, 0.5f};

                for (size_t i = 0; i < TextBlocks.size(); ++i) {
                    UObject* TextBlock = TextBlocks[i];
                    UObject* RowIcon   = RowIcons[i];
                    
                    if (TextBlock && RowIcon) {
                        bool bCondition = bHighlight && ((i + 1) % 2 == 0);
                        UI::SetTextColor(TextBlock, bCondition ? PalBlue : White);
                        UI::SetFontData(TextBlock, bCondition ? 32 : 22, bCondition ? DarkOut : SoftOut);
                        UI::SetImageColor(RowIcon, bCondition ? PalBlue : White);
                    }
                }
            });

            auto ListBoxBuilder = UI::VerticalBox(MyWidget);
            std::vector<std::wstring> labels = { L"A", L"B", L"C", L"D", L"E", L"F" };
            for (size_t i = 0; i < labels.size(); ++i) {
                auto RowIcon = UI::Image(MyWidget).ImageFromAsset(UI::Assets::Common::StatusArrow).ImageColor(White).ImageSize(16, 16);
                auto TextWidget = UI::Text(MyWidget).Text(L"this is text " + labels[i]).Font(PalFont, L"Medium", 20).TextOutline(1, {0.0f, 0.0f, 0.0f, 0.5f}).TextColor(White);
                TextBlocks.push_back(TextWidget.Build());
                RowIcons.push_back(RowIcon.Build());

                auto RowLayout = UI::HorizontalBox(MyWidget)
                    .AddToHorizontalBox(RowIcon, [](BoxSlotBuilder& Slot) { Slot.Padding(0.0f, 0.0f, 12.0f, 0.0f).VerticalAlignment(EBuilderVerticalAlignment::VAlign_Center); })
                    .AddToHorizontalBox(TextWidget, [](BoxSlotBuilder& Slot) { Slot.VerticalAlignment(EBuilderVerticalAlignment::VAlign_Center); });
                ListBoxBuilder.AddToVerticalBox(RowLayout, [](BoxSlotBuilder& Slot) { Slot.Padding(0.0f, 6.0f, 0.0f, 6.0f); });
            }

            ContentContainer
                .AddToVerticalBox(ButtonBuilder, [](BoxSlotBuilder& Slot) { Slot.Padding(0.0f, 0.0f, 0.0f, 20.0f).VerticalAlignment(EBuilderVerticalAlignment::VAlign_Center); })
                .AddToVerticalBox(ListBoxBuilder);

        } else if (ActiveTab == 1) {
            TestSlider = std::make_unique<class DynPals::UI::Slider>(MyWidget, 0.0, 100.0, 50.0);
            TestSwitch = std::make_unique<class DynPals::UI::Switch>(MyWidget, true);
            TestLR = std::make_unique<class DynPals::UI::Selector>(MyWidget, std::vector<std::wstring>{L"Low", L"Medium", L"High", L"Epic"}, 2);

            RC::Unreal::UObject* WidgetTrashBin = UI::VerticalBox(MyWidget).Build();
            struct { uint8_t InVisibility; } VisParams{ 1 };
            Utils::CallFunction(WidgetTrashBin, STR("SetVisibility"), &VisParams);

            if (!TestDropdown) {
                int initialIdx = 0;
                for (size_t i = 0; i < DropdownOptions.size(); ++i) {
                    if (DropdownOptions[i] == CurrentDropdownChoice) {
                        initialIdx = static_cast<int>(i);
                        break;
                    }
                }
                TestDropdown = std::make_unique<class DynPals::UI::Dropdown>(DropdownOptions, initialIdx);
                TestDropdown->OnChanged([this](int Index, std::wstring Choice) {
                    CurrentDropdownChoice = Choice;
                });
            }
            TestDropdown->SetTrashBin(WidgetTrashBin);

            UObject* NativeDropdownWidget = TestDropdown->Build(MyWidget, CurrentPlayerController);

            ContentContainer
                .AddToVerticalBox(UI::Text(MyWidget).Text(L"Option Slider").Font(PalFont, L"Medium", 18))
                .AddToVerticalBox(WidgetBuilder(TestSlider->GetWidget()), [](BoxSlotBuilder& Slot) { Slot.Padding(0, 5, 0, 20); })
                .AddToVerticalBox(UI::Text(MyWidget).Text(L"Option Switch").Font(PalFont, L"Medium", 18))
                .AddToVerticalBox(WidgetBuilder(TestSwitch->GetWidget()), [](BoxSlotBuilder& Slot) { Slot.Padding(0, 5, 0, 20); })
                .AddToVerticalBox(UI::Text(MyWidget).Text(L"Left/Right Selector").Font(PalFont, L"Medium", 18))
                .AddToVerticalBox(WidgetBuilder(TestLR->GetWidget()), [](BoxSlotBuilder& Slot) { Slot.Padding(0, 5, 0, 20); })
                .AddToVerticalBox(UI::Text(MyWidget).Text(L"Native Overlay Dropdown").Font(PalFont, L"Medium", 18))
                .AddToVerticalBox(WidgetBuilder(L"/Script/UMG.SizeBox", MyWidget).AddChild(WidgetBuilder(NativeDropdownWidget)), [](BoxSlotBuilder& Slot) { Slot.Padding(0, 5, 0, 20); })
                .AddToVerticalBox(DynPals::WidgetBuilder(WidgetTrashBin));
        }

        UObject* Canvas = UI::WindowFrame(MyWidget, 600.0f)
            .SetHeader(HeaderBox)
            .AddAutoScaledRow(TabLayout) 
            .AddContent(ContentContainer)
            .SetFooter(UI::ActionBar(MyWidget))
            .Build();

        UObject* WidgetTree = nullptr;
        if (Utils::GetPropertyValue(MyWidget, STR("WidgetTree"), WidgetTree) && WidgetTree) {
            FProperty* RootProp = Utils::GetProperty(WidgetTree, STR("RootWidget"));
            if (RootProp) *RootProp->ContainerPtrToValuePtr<UObject*>(WidgetTree) = Canvas;
        }

        struct { int32_t ZOrder; } ViewportParams{9999};
        Utils::CallFunction(MyWidget, STR("AddToViewport"), &ViewportParams);
    }

    void TestUI::OnTickUI() {
        if (TabBtn1) TabBtn1->Tick();
        if (TabBtn2) TabBtn2->Tick();

        if (ActiveTab == 0) {
            if (HighlightButton) HighlightButton->Tick();
        } else if (ActiveTab == 1) {
            if (TestSlider)   TestSlider->Tick();
            if (TestSwitch)   TestSwitch->Tick();
            if (TestLR)       TestLR->Tick();
            if (TestDropdown) TestDropdown->Tick();
        }
    }
}
```

### 6. `src/HooksManager.cpp`
```cpp
#define NOMINMAX

#include <Zydis/Zydis.h>

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <chrono>
#include <fstream>
#include <safetyhook.hpp>
#include <thread>

#include "AsyncHelper.hpp"
#include "HooksManager.hpp"
#include "NotificationManager.hpp"
#include "PalProcessor.hpp"
#include "SaveManager.hpp"
#include "UI/UIRegistry.hpp"
#include "UI/Views/UIManager.hpp"
#include "Updater.hpp"
#include "Utils.hpp"
#include "VFXManager.hpp"

using namespace RC;

using namespace RC::Unreal;

namespace DynPals {

static bool bCompletedInitReady = false;

static UObject* LastPlayerController = nullptr;

static UObject* LastWorld = nullptr;

static bool bIsAtMenu = false;

// --- NATIVE DETOUR STORAGE ---

static SafetyHookInline Hook_MasterWazaUpdate;

static SafetyHookInline Hook_OnUpdateCharacterRank;

static SafetyHookInline Hook_AddFriendship;

// --- DYNAMIC NATIVE THUNK DISASSEMBLER ---

static void* ResolveNativeFromThunk(void* ThunkAddress) {
    if (!ThunkAddress) return nullptr;

    ZyanStatus status;
    ZydisDecoder decoder;
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64))) {
        return nullptr;
    }

    ZyanUSize offset = 0;
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];

    std::vector<void*> calls;

    const uintptr_t thunkStart = reinterpret_cast<uintptr_t>(ThunkAddress);
    const uintptr_t thunkEnd = thunkStart + 500; // Define the maximum structural boundary of a thunk

    while (offset < 500) {
        status = ZydisDecoderDecodeFull(&decoder, reinterpret_cast<uint8_t*>(ThunkAddress) + offset, 500 - offset, &instruction, operands);
        if (!ZYAN_SUCCESS(status)) break;
        if (instruction.mnemonic == ZYDIS_MNEMONIC_RET) break;

        // 1. Track standard CALL instructions
        if (instruction.mnemonic == ZYDIS_MNEMONIC_CALL) {
            if (operands[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                uintptr_t rip = thunkStart + offset + instruction.length;
                uintptr_t target = rip + operands[0].imm.value.s;
                calls.push_back(reinterpret_cast<void*>(target));
            }
        }
        // 2. Track external JMP instructions (Tail-Calls)
        else if (instruction.mnemonic == ZYDIS_MNEMONIC_JMP) {
            if (operands[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                uintptr_t rip = thunkStart + offset + instruction.length;
                uintptr_t target = rip + operands[0].imm.value.s;

                // CRITICAL CHECK: Only count JMPs that escape the thunk's 500-byte boundary.
                // This ignores all local if/else branches and isolates the true native function target.
                if (target < thunkStart || target > thunkEnd) {
                    calls.push_back(reinterpret_cast<void*>(target));
                    break; // Unconditional JMP out of the thunk marks the end of execution
                }
            }
        }

        offset += instruction.length;
    }

    if (calls.empty()) return nullptr;

    void* lastCallTarget = calls.back();

    // FILTER: Prevent hooking MSVC's '__security_check_cookie'
    status = ZydisDecoderDecodeFull(&decoder, lastCallTarget, 16, &instruction, operands);
    if (ZYAN_SUCCESS(status)) {
        if (instruction.mnemonic == ZYDIS_MNEMONIC_CMP && operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER && operands[0].reg.value == ZYDIS_REGISTER_RCX) {
            if (calls.size() > 1) {
                return calls[calls.size() - 2];
            }
        }
    }

    // SAFETY FILTER: If the target is FFrame::Step, the function was likely fully inlined.
    // We detect this if all captured calls in the thunk point to the same address.
    if (calls.size() > 1 && calls.front() == lastCallTarget) {
        return nullptr; 
    }

    return lastCallTarget;
}


static void* GetNativeAddress(const wchar_t* FunctionPath) {
  UFunction* FuncObj = UObjectGlobals::StaticFindObject<UFunction*>(
      nullptr, nullptr, FunctionPath);

  if (!FuncObj) return nullptr;

  void* ThunkAddr =
      *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(FuncObj) + 0xD8);

  void* NativeAddr = ResolveNativeFromThunk(ThunkAddr);

  if (NativeAddr) {
    return NativeAddr;
  }

  return ThunkAddr;
}

// --- DETOUR CALLBACKS ---

void __fastcall NativeMasterWazaUpdate_Hook(UObject* This, int32_t AddLevel,
                                            int32_t NowLevel) {
  Hook_MasterWazaUpdate.call<void, UObject*, int32_t, int32_t>(This, AddLevel,
                                                               NowLevel);

  if (This) {
    std::wstring actorName = This->GetName();

    DP_LOG(Default,
           "[Native Hook] Pal {} Leveled Up to {}! Checking evolution...",
           actorName, NowLevel);

    PalProcessor::Get().ProcessPal(This, false);
  }
}

void __fastcall NativeOnUpdateCharacterRank_Hook(UObject* This, int32_t NewRank,
                                                 int32_t OldRank) {
  Hook_OnUpdateCharacterRank.call<void, UObject*, int32_t, int32_t>(
      This, NewRank, OldRank);

  if (This) {
    UObject* PalActor = This->GetOuterPrivate();

    if (PalActor) {
      std::wstring actorName = PalActor->GetName();

      DP_LOG(Default,
             "[Native Hook] Pal {} Condensation Rank Up to {}! Checking "
             "evolution...",
             actorName, NewRank);

      PalProcessor::Get().ProcessPal(PalActor, false);
    }
  }
}

void __fastcall NativeAddFriendship_Hook(UObject* This, int32_t Value,
                                         bool bApplyPassiveSkill) {
  Hook_AddFriendship.call<void, UObject*, int32_t, bool>(This, Value,
                                                         bApplyPassiveSkill);

  if (This) {
    UObject* PalActor = nullptr;

    Utils::GetPropertyValue<UObject*>(This, STR("IndividualActor"), PalActor);

    if (PalActor) {
      std::wstring actorName = PalActor->GetName();

      DP_LOG(Default,
             "[Native Hook] Pal {} Friendship updated! Checking evolution...",
             actorName);

      PalProcessor::Get().ProcessPal(PalActor, false);
    }
  }
}

static void OnStartedWorldAutoSave(UnrealScriptFunctionCallableContext&,
                                   void*) {
  DP_LOG(Default, "Auto-Save triggered! Synchronizing world persistence...\n");

  SaveManager::Get().SaveWorldData();
}

static void OnGameThreadTick(UnrealScriptFunctionCallableContext& Context,
                             void*) {
  static bool bIsReentrant = false;

  if (bIsReentrant) return;

  bIsReentrant = true;

  // 1. Tick VFX Manager timeline events (extremely fast check)

  VFXManager::Get().Tick();

  // 2. ULTRA-PERFORMANT EXIT: Skip reflection logic entirely if no menus need
  // to tick!

  if (!UIRegistry::Get().RequiresTick()) {
    bIsReentrant = false;

    return;
  }

  // Only run slow reflection queries if the menu is actually active or
  // transitioning

  static auto LastTickTime = std::chrono::steady_clock::now();

  auto Now = std::chrono::steady_clock::now();

  if (std::chrono::duration_cast<std::chrono::milliseconds>(Now - LastTickTime)
          .count() >= 16) {
    LastTickTime = Now;

    UObject* ActorContext = Context.Context;

    if (ActorContext) {
      UObject* Level = ActorContext->GetOuterPrivate();

      UObject* World = Level ? Level->GetOuterPrivate() : nullptr;

      if (World && World == LastWorld) {
        UObject* PlayerController = nullptr;

        UObject* GameplayStatics = UObjectGlobals::StaticFindObject<UObject*>(
            nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));

        if (GameplayStatics) {
          struct {
            UObject* WorldContextObject;
            int32_t PlayerIndex;
            UObject* ReturnValue;
          } GSParams{ActorContext, 0, nullptr};

          Utils::CallFunction(GameplayStatics, STR("GetPlayerController"),
                              &GSParams);

          PlayerController = GSParams.ReturnValue;
        }

        if (PlayerController) {
          UIRegistry::Get().TickAll(PlayerController);
        }
      }
    }
  }

  bIsReentrant = false;
}

static void OnWidgetAddedToViewport(
    UnrealScriptFunctionCallableContext& Context, void*) {
  if (bIsAtMenu) return;

  UObject* Widget = Context.Context;

  if (!Widget) return;

  UClass* WidgetClass = Widget->GetClassPrivate();

  if (!WidgetClass) return;

  std::wstring WidgetName = WidgetClass->GetName();

  if (WidgetName.find(L"WBP_Title") != std::wstring::npos ||

      WidgetName.find(L"WBP_Login") != std::wstring::npos)

  {
    bIsAtMenu = true;

    bCompletedInitReady = false;

    SaveManager::Get().Reset();

    NotificationManager::Get().SetReady(false);

    PalProcessor::Get().ClearAllSwappedStatus();

    UIRegistry::Get().InvalidateAllUIs();

    DP_LOG(Default,
           "Transitioned to Main Menu (Detected via '{}'). Mod entering "
           "standby mode...\n",
           WidgetName);

    std::thread([]() {
      Updater::CheckForUpdates();
    }).detach();
  }
}

static void OnOpenLevel(UnrealScriptFunctionCallableContext& Context, void*) {
  bIsAtMenu = false;

  bCompletedInitReady = false;

  NotificationManager::Get().SetReady(false);

  UIRegistry::Get().InvalidateAllUIs();
}

static std::wstring GetFormattedVersionString() {
  HMODULE hModule = NULL;

  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,

                     (LPCWSTR)&GetFormattedVersionString, &hModule);

  wchar_t path[MAX_PATH];

  GetModuleFileNameW(hModule, path, MAX_PATH);

  std::wstring currentDllPath(path);

  std::wstring dllDir =
      currentDllPath.substr(0, currentDllPath.find_last_of(L"\\/") + 1);

  std::wstring versionTxtPath = dllDir + L"version.txt";

  std::ifstream file(versionTxtPath);

  if (!file.is_open()) {
    return L"v0.0.56";
  }

  std::string content((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());

  content.erase(0, content.find_first_not_of(" \t\r\n"));

  size_t last = content.find_last_not_of(" \t\r\n");

  if (last != std::string::npos) {
    content.erase(last + 1);
  }

  if (content.empty()) {
    return L"v0.0.56";
  }

  try {
    int versionNum = std::stoi(content);

    int major = versionNum / 1000;

    int minor = (versionNum / 100) % 10;

    int patch = versionNum % 100;

    wchar_t buf[64];

    swprintf(buf, 64, L"v%d.%d.%02d", major, minor, patch);

    return std::wstring(buf);

  } catch (...) {
    std::wstring rawVersion;

    rawVersion.assign(content.begin(), content.end());

    return L"v" + rawVersion;
  }
}

void HooksManager::OnPalSpawnedReady(
    UnrealScriptFunctionCallableContext& Context, void*) {
  UObject* PalNPC = Context.Context;

  std::wstring palName = PalNPC ? PalNPC->GetName() : L"NULL";

  DP_LOG(Default, "[Hook Monitor] OnPalSpawnedReady fired for {}", palName);

  if (!bCompletedInitReady) {
    DP_LOG(Default, "  -> Aborted: Mod is still in startup standby.");

    return;
  }

  if (PalNPC) {
    PalProcessor::Get().ProcessPal(PalNPC, false);
  }
}

static void OnClientRestart(UnrealScriptFunctionCallableContext& Context,
                            void*) {
  UObject* PlayerController = Context.Context;

  if (!PlayerController) return;

  UObject* Level = PlayerController->GetOuterPrivate();

  UObject* CurrentWorld = Level ? Level->GetOuterPrivate() : nullptr;

  if (PlayerController != LastPlayerController || CurrentWorld != LastWorld) {
    LastPlayerController = PlayerController;

    LastWorld = CurrentWorld;

    UObject* GameplayStatics = UObjectGlobals::StaticFindObject<UObject*>(
        nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));

    if (GameplayStatics) {
      struct {
        UObject* WorldContextObject;
        bool bRemovePrefixString;
        FString ReturnValue;
      } Params{PlayerController, true, FString()};

      Utils::CallFunction(GameplayStatics, STR("GetCurrentLevelName"), &Params);

      std::wstring MapName = Utils::FStringToWString(Params.ReturnValue);

      bool bIsMenu = (MapName.find(L"Title") != std::wstring::npos ||

                      MapName.find(L"Login") != std::wstring::npos ||

                      MapName.empty());

      if (bIsMenu) {
        bCompletedInitReady = false;

        NotificationManager::Get().SetReady(false);

        SaveManager::Get().Reset();

        PalProcessor::Get().ClearAllSwappedStatus();

      } else {
        bIsAtMenu = false;

        bCompletedInitReady = false;

        NotificationManager::Get().SetReady(false);

        SaveManager::Get().Reset();

        PalProcessor::Get().ClearAllSwappedStatus();

        DP_LOG(Default,
               "New Session Detected (Map: '{}'). Spawning surge quarantine "
               "active. Pausing swaps for 5 seconds...\n",
               MapName);

        std::thread([]() {
          std::this_thread::sleep_for(std::chrono::seconds(8));

          AsyncHelper::AsyncTask(ENamedThreads::GameThread, []() {
            DP_LOG(Default,
                   "Settle period complete. Running overworld Pal "
                   "reconciliation...\n");

            std::vector<UObject*> AllPals;

            UObjectGlobals::FindAllOf(STR("PalCharacter"), AllPals);

            for (UObject* Pal : AllPals) {
              if (Pal) {
                PalProcessor::Get().ProcessPal(Pal, false);
              }
            }

            // --- PRELOAD UI ---
            UObject* PlayerController = UObjectGlobals::FindFirstOf(STR("PalPlayerController"));
            if (PlayerController) {
                UIManager::Get().PreloadUI(PlayerController);
            }

            bCompletedInitReady = true;

            DP_LOG(Default,
                   "Reconciliation complete. Mod entering zero-overhead "
                   "standby.\n");

            std::wstring verStr = GetFormattedVersionString();

            DP_LOG(Normal, "Welcome to dynamic pals {} and happy palworld 1.0 <3", verStr);

            NotificationManager::Get().FlushQueuedToasts();
          });
        }).detach();
      }
    }
  }
}

void HooksManager::RegisterHooks() {
  UFunction* InitFunc = UObjectGlobals::StaticFindObject<UFunction*>(
      nullptr, nullptr, STR("/Script/Pal.PalNPC:OnCompletedInitParam"));

  if (InitFunc) {
    InitFunc->RegisterPostHook(OnPalSpawnedReady, nullptr);

    DP_LOG(
        Default,
        "Successfully hooked OnCompletedInitParam (Native Pipeline Active!)\n");
  }

  UFunction* RestartFunc = UObjectGlobals::StaticFindObject<UFunction*>(
      nullptr, nullptr, STR("/Script/Engine.PlayerController:ClientRestart"));

  if (RestartFunc) {
    RestartFunc->RegisterPostHook(OnClientRestart, nullptr);

    DP_LOG(Default, "Successfully hooked ClientRestart for map transitions.\n");
  }

  // Restored: Re-hook K2_GetActorRotation to guarantee Game Thread access
  // context!

  UFunction* ActorRotFunc = UObjectGlobals::StaticFindObject<UFunction*>(
      nullptr, nullptr, STR("/Script/Engine.Actor:K2_GetActorRotation"));

  if (ActorRotFunc) {
    ActorRotFunc->RegisterPreHook(OnGameThreadTick, nullptr);

    DP_LOG(Default,
           "Successfully hooked K2_GetActorRotation on the Game Thread.\n");
  }

  UFunction* SaveFunc = UObjectGlobals::StaticFindObject<UFunction*>(
      nullptr, nullptr,
      STR("/Script/Pal.PalSaveGameManager:StartWorldDataAutoSave"));

  if (SaveFunc) {
    SaveFunc->RegisterPostHook(OnStartedWorldAutoSave, nullptr);
  }

  // ==================================================

  // MIXED NATIVE ASSEMBLY DETOURS

  // ==================================================

  uintptr_t BaseAddr = reinterpret_cast<uintptr_t>(GetModuleHandleA(NULL));

  void* MasterWazaUpdateAddr = GetNativeAddress(STR("/Script/Pal.PalNPC:MasterWazaUpdateWhenLevelUp"));
        if (MasterWazaUpdateAddr) {
            Hook_MasterWazaUpdate = safetyhook::create_inline(MasterWazaUpdateAddr, NativeMasterWazaUpdate_Hook);
            DP_LOG(Default, "[Native Hook] Detoured MasterWazaUpdateWhenLevelUp dynamically!");
        } else {
            DP_LOG(Error, "Failed to dynamically resolve Native MasterWazaUpdateWhenLevelUp!");
        }


        // 2. Scan and detour OnUpdateCharacterRank
        void* SetRankAddr = AsyncHelper::FindPattern("40 53 48 83 EC 20 48 8B D9 48 8B 89 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 85 C0 75 ?? 48 8B D0 48 8B CB 48 83 C4 20 5B");
        if (SetRankAddr) {
            Hook_OnUpdateCharacterRank = safetyhook::create_inline(SetRankAddr, NativeOnUpdateCharacterRank_Hook);
            DP_LOG(Default, "[Native Hook] Detoured OnUpdateCharacterRank via AOB!");
        } else {
            DP_LOG(Error, "Failed to resolve AOB for OnUpdateCharacterRank!");
        }

        // 3. Dynamically resolve and detour AddFriendShip
        void* FriendshipAddr = GetNativeAddress(STR("/Script/Pal.PalIndividualCharacterParameter:AddFriendShip"));
        if (FriendshipAddr) {
            //Hook_AddFriendship = safetyhook::create_inline(FriendshipAddr, NativeAddFriendship_Hook);
            DP_LOG(Default, "[Native Hook] Detoured AddFriendShip successfully!");
        } else {
            DP_LOG(Error, "Failed to resolve Native AddFriendShip!");

  }

  UFunction* AddToViewportFunc = UObjectGlobals::StaticFindObject<UFunction*>(
      nullptr, nullptr, STR("/Script/UMG.UserWidget:AddToViewport"));

  if (AddToViewportFunc) {
    AddToViewportFunc->RegisterPostHook(OnWidgetAddedToViewport, nullptr);
  }

  UFunction* AddToPlayerScreenFunc =
      UObjectGlobals::StaticFindObject<UFunction*>(
          nullptr, nullptr, STR("/Script/UMG.UserWidget:AddToPlayerScreen"));

  if (AddToPlayerScreenFunc) {
    AddToPlayerScreenFunc->RegisterPostHook(OnWidgetAddedToViewport, nullptr);
  }

  UFunction* OpenLevelFunc = UObjectGlobals::StaticFindObject<UFunction*>(
      nullptr, nullptr, STR("/Script/Engine.GameplayStatics:OpenLevel"));

  if (OpenLevelFunc) {
    OpenLevelFunc->RegisterPreHook(OnOpenLevel, nullptr);
  }
}

}  // namespace DynPals
```