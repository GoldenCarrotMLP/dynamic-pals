// --- START OF FILE src/UI/views/UIManager.cpp ---
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
            SkinDropdown = std::make_unique<UI::Dropdown>(std::vector<std::wstring>{}, 0);
        }
        
        // --- PRELOAD AND CACHE CORE UI BLUEPRINTS TO PREVENT DISK STUTTER ---
        std::vector<std::wstring> AssetsToCache = {
            UI::Assets::Blueprints::CommonWindow,
            UI::Assets::Blueprints::CommonButton,
            UI::Assets::Blueprints::PalTextBlock,
            UI::Assets::Blueprints::PalActionBar,
            UI::Assets::Fonts::PalDefault,
            UI::Assets::Borders::Frame1px,
            UI::Assets::Borders::WhiteSolid
        };
        for (const auto& AssetPath : AssetsToCache) {
            Utils::LoadAssetSafely(AssetPath);
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
            
            // Move offscreen cleanly using Render Translation (Preserves viewport DPI/Layout)
            struct FVector2D_Double { double X; double Y; };
            struct { FVector2D_Double Translation; } RenderParams{ {-99999.0, -99999.0} };
            Utils::CallFunction(PreloadContainer, STR("SetRenderTranslation"), &RenderParams);
            
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

        // Start Profiling Checkpoint
        auto start = std::chrono::high_resolution_clock::now();

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
            SkinDropdown = std::make_unique<UI::Dropdown>(std::vector<std::wstring>{}, 0);
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

        HideInvalidSwitch = std::make_unique<UI::Switch>(MyWidget, bHideInvalidSwaps);
        HideInvalidSwitch->OnChanged([this](bool bState) {
            bHideInvalidSwaps = bState;
            if (MainScrollBoxObj && GetScrollOffsetFunc) {
                struct { float Offset; } Params{ 0.0f };
                MainScrollBoxObj->ProcessEvent(GetScrollOffsetFunc, &Params);
                LastScrollOffset = Params.Offset;
            }
            RefreshUI(); 
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
            RefreshUI(); 
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

        // End Profiling Checkpoint
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        DP_LOG(Default, "[Profile] UIManager::BuildWidget shell layout initialized in {} us ({:.3f} ms)", 
               duration, duration / 1000.0f);

    }

    void UIManager::RefreshUI() {
        if (!TargetPal || !DynamicLogBox || !DynamicMorphBox || !CameraRotationContainer) return;

        // Start Profiling Checkpoint
        auto start = std::chrono::high_resolution_clock::now();

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
            CameraRotationSlider = std::make_unique<UI::Slider>(MyWidget, 0.0, 360.0, SaveManager::Get().Settings.CameraRotation);
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

        // End Profiling Checkpoint
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        DP_LOG(Default, "[Profile] UIManager::RefreshUI completed in {} us ({:.3f} ms)", 
               duration, duration / 1000.0f);
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
