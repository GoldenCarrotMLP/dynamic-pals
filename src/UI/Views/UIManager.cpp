#define NOMINMAX 
#include <Windows.h>
#include <Xinput.h>

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


    static const std::vector<std::wstring> PollableKeys = {
        L"A", L"B", L"C", L"D", L"E", L"F", L"G", L"H", L"I", L"J", L"K", L"L", L"M", 
        L"N", L"O", L"P", L"Q", L"R", L"S", L"T", L"U", L"V", L"W", L"X", L"Y", L"Z",
        L"Zero", L"One", L"Two", L"Three", L"Four", L"Five", L"Six", L"Seven", L"Eight", L"Nine",
        L"F1", L"F2", L"F3", L"F4", L"F5", L"F6", L"F7", L"F8", L"F9", L"F10", L"F11", L"F12",
        L"SpaceBar", L"Enter", L"Tab", L"Escape",
        L"Gamepad_FaceButton_Bottom", L"Gamepad_FaceButton_Right", L"Gamepad_FaceButton_Left", L"Gamepad_FaceButton_Top", 
        L"Gamepad_LeftShoulder", L"Gamepad_RightShoulder", L"Gamepad_LeftTrigger", L"Gamepad_RightTrigger", 
        L"Gamepad_DPad_Up", L"Gamepad_DPad_Down", L"Gamepad_DPad_Left", L"Gamepad_DPad_Right",
        L"Gamepad_Special_Left", L"Gamepad_Special_Right", L"Gamepad_LeftThumbstick", L"Gamepad_RightThumbstick",
        L"LeftAlt", L"LeftControl", L"LeftShift"
    };

    // Set to true for smooth camera interpolation; set to false for instant snapping
    static constexpr bool bEnableCameraSmoothing = false;

    // Helper: Strips any 8-character hexadecimal fallback hash like " (41BCA68A)" from labels
    static std::wstring StripFallbackHash(std::wstring str) {
        size_t openPos = 0;
        while ((openPos = str.find(L" (", openPos)) != std::wstring::npos) {
            if (openPos + 10 < str.length() && str[openPos + 10] == L')') {
                bool isHex = true;
                for (size_t k = openPos + 2; k < openPos + 10; ++k) {
                    if (!std::iswxdigit(str[k])) {
                        isHex = false;
                        break;
                    }
                }
                if (isHex) {
                    str.erase(openPos, 11);
                    continue;
                }
            }
            openPos += 2;
        }
        return str;
    }

    // Helper: Safely updates USceneComponent rotation using native reflection functions
    static void SetComponentRotationDirect(RC::Unreal::UObject* Comp, const FRotator_UE5& Rot, bool bWorldSpace, bool bTeleport = false) {
        if (!Comp || !Utils::IsObjectValid(Comp)) return;

        const wchar_t* FuncName = bWorldSpace ? STR("K2_SetWorldRotation") : STR("K2_SetRelativeRotation");
        RC::Unreal::UFunction* Func = Comp->GetFunctionByNameInChain(FuncName);
        if (!Func) return;

        alignas(8) uint8_t Buffer[512] = {0};
        for (RC::Unreal::FProperty* Prop = (RC::Unreal::FProperty*)Func->GetChildProperties(); Prop; Prop = (RC::Unreal::FProperty*)Utils::GetNextField(Prop)) {
            Prop->InitializeValue_InContainer(Buffer);
        }

        RC::Unreal::FProperty* RotProp = Func->GetPropertyByNameInChain(STR("NewRotation"));
        if (RotProp) {
            *RotProp->ContainerPtrToValuePtr<FRotator_UE5>(Buffer) = Rot;
        }

        RC::Unreal::FProperty* TeleportProp = Func->GetPropertyByNameInChain(STR("bTeleport"));
        if (TeleportProp && TeleportProp->GetClass().GetName() == STR("BoolProperty")) {
            static_cast<RC::Unreal::FBoolProperty*>(TeleportProp)->SetPropertyValue(TeleportProp->ContainerPtrToValuePtr<void>(Buffer), bTeleport);
        }

        Utils::SafeProcessEvent(Comp, Func, Buffer);

        for (RC::Unreal::FProperty* Prop = (RC::Unreal::FProperty*)Func->GetChildProperties(); Prop; Prop = (RC::Unreal::FProperty*)Utils::GetNextField(Prop)) {
            Prop->DestroyValue_InContainer(Buffer);
        }
    }

    // --- CONSOLIDATED CAMERA BOOM RESOLVER ---
    RC::Unreal::UObject* UIManager::GetCameraBoom(RC::Unreal::UObject* Pal) {
        if (!Pal || !Utils::IsObjectValid(Pal)) return nullptr;

        UObject* CameraBoomObj = nullptr;
        if (Utils::GetPropertyValue<UObject*>(Pal, STR("CameraBoom"), CameraBoomObj, true) && CameraBoomObj) {
            return CameraBoomObj;
        }

        UClass* SpringArmClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Engine.SpringArmComponent"));
        if (SpringArmClass) {
            struct { UClass* ComponentClass; UObject* ReturnValue; } GetCompParams{SpringArmClass, nullptr};
            Utils::CallFunction(Pal, STR("GetComponentByClass"), &GetCompParams, true);
            return GetCompParams.ReturnValue;
        }
        return nullptr;
    }

    // --- CONSOLIDATED SCROLL OFFSET CACHER ---
    void UIManager::CacheScrollOffset() {
        if (MainScrollBoxObj && GetScrollOffsetFunc) {
            struct { float Offset; } Params{ 0.0f };
            MainScrollBoxObj->ProcessEvent(GetScrollOffsetFunc, &Params);
            LastScrollOffset = Params.Offset;
        }
    }

    // --- DEBOUNCED DISK SAVE METHODS ---
    void UIManager::TriggerSaveDebounced() {
        bPendingDiskSave = true;
        LastSliderChangeTime = std::chrono::steady_clock::now();
    }

    void UIManager::FlushPendingSave() {
        if (bPendingDiskSave) {
            bPendingDiskSave = false;
            SaveManager::Get().SaveWorldData();
        }
    }

    void UIManager::EnablePalCamera() {
        if (!CurrentPlayerController || !TargetPal || bIsPalCameraActive) return;

        if (OriginalViewTarget && !Utils::IsObjectValid(OriginalViewTarget)) {
            OriginalViewTarget = nullptr;
        }

        // 1. Locate and Force-Activate FollowCamera
        UObject* FollowCameraObj = nullptr;
        if (!Utils::GetPropertyValue<UObject*>(TargetPal, STR("FollowCamera"), FollowCameraObj, true) || !FollowCameraObj) {
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

        // 2. Configure CameraBoom
        UObject* CameraBoomObj = GetCameraBoom(TargetPal);
        if (CameraBoomObj) {
            Utils::SetPropertyValue<float>(CameraBoomObj, STR("TargetArmLength"), 2000.0f);
            Utils::SetPropertyValue<bool>(CameraBoomObj, STR("bUsePawnControlRotation"), false);
            Utils::SetPropertyValue<bool>(CameraBoomObj, STR("bDoCollisionTest"), false); 

            Utils::SetPropertyValue<bool>(CameraBoomObj, STR("bEnableCameraLag"), bEnableCameraSmoothing);
            Utils::SetPropertyValue<bool>(CameraBoomObj, STR("bEnableCameraRotationLag"), bEnableCameraSmoothing);
            if constexpr (bEnableCameraSmoothing) {
                Utils::SetPropertyValue<float>(CameraBoomObj, STR("CameraLagSpeed"), 10.0f);
                Utils::SetPropertyValue<float>(CameraBoomObj, STR("CameraRotationLagSpeed"), 15.0f);
            }

            UpdatePalCameraRotation(SaveManager::Get().Settings.CameraRotation, true);
        }

        // 3. Blend view target
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

        // Restore CameraBoom settings
        UObject* CameraBoomObj = GetCameraBoom(TargetPal);
        if (CameraBoomObj) {
            UFunction* SetAbsFunc = CameraBoomObj->GetFunctionByNameInChain(STR("SetAbsolute"));
            if (SetAbsFunc) {
                struct { bool bNewAbsoluteLocation; bool bNewAbsoluteRotation; bool bNewAbsoluteScale; } AbsParams{ false, false, false };
                Utils::SafeProcessEvent(CameraBoomObj, SetAbsFunc, &AbsParams);
            }
            Utils::SetPropertyValue<bool>(CameraBoomObj, STR("bInheritYaw"), true);
            Utils::SetPropertyValue<bool>(CameraBoomObj, STR("bDoCollisionTest"), true);
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

    void UIManager::UpdatePalCameraRotation(double Yaw, bool bTeleport) {
        UObject* CameraBoomObj = GetCameraBoom(TargetPal);
        if (CameraBoomObj && Utils::IsObjectValid(CameraBoomObj)) {
            bool bRelative = SaveManager::Get().Settings.bRelativeCamera;

            Utils::SetPropertyValue<bool>(CameraBoomObj, STR("bInheritYaw"), bRelative);

            UFunction* SetAbsFunc = CameraBoomObj->GetFunctionByNameInChain(STR("SetAbsolute"));
            if (SetAbsFunc) {
                struct { bool bNewAbsoluteLocation; bool bNewAbsoluteRotation; bool bNewAbsoluteScale; } AbsParams{ false, !bRelative, false };
                Utils::SafeProcessEvent(CameraBoomObj, SetAbsFunc, &AbsParams);
            }

            bool bEffectiveTeleport = bTeleport || !bEnableCameraSmoothing;
            FRotator_UE5 NewRot{ 0.0, Yaw, 0.0 };
            SetComponentRotationDirect(CameraBoomObj, NewRot, !bRelative, bEffectiveTeleport);
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

        struct { FVector_UE5 Location; FRotator_UE5 Rotation; } ViewPointParams;
        Utils::CallFunction(CurrentPlayerController, STR("GetPlayerViewPoint"), &ViewPointParams);
        
        FVector_UE5 CameraLoc = ViewPointParams.Location;
        FRotator_UE5 CameraRot = ViewPointParams.Rotation;

        double PitchRad = CameraRot.Pitch * 0.01745329251; 
        double YawRad = CameraRot.Yaw * 0.01745329251;
        double CosPitch = std::cos(PitchRad);

        FVector_UE5 CameraForward{
            std::cos(YawRad) * CosPitch,
            std::sin(YawRad) * CosPitch,
            std::sin(PitchRad)
        };

        std::vector<UObject*> AllPals;
        UObjectGlobals::FindAllOf(STR("PalCharacter"), AllPals);

        UObject* aimedPal = nullptr;
        double highestDot = -1.0;
        UObject* closestPal = nullptr;
        double closestDistSq = 999999999.0;

        for (UObject* Pal : AllPals) {
            if (Pal == PlayerPawn || !Utils::IsObjectValid(Pal)) continue;

            bool bHidden = false;
            Utils::GetPropertyValue<bool>(Pal, STR("bHidden"), bHidden, true);
            bool bBeingDestroyed = false;
            Utils::GetPropertyValue<bool>(Pal, STR("bActorIsBeingDestroyed"), bBeingDestroyed, true);

            if (bHidden || bBeingDestroyed) continue;

            struct { FVector_UE5 RetVal; } PalLocParams;
            Utils::CallFunction(Pal, STR("K2_GetActorLocation"), &PalLocParams);
            FVector_UE5 PalLoc = PalLocParams.RetVal;

            FVector_UE5 Dir{ PalLoc.X - CameraLoc.X, PalLoc.Y - CameraLoc.Y, PalLoc.Z - CameraLoc.Z };
            double distSq = (Dir.X * Dir.X) + (Dir.Y * Dir.Y) + (Dir.Z * Dir.Z);
            double dist = std::sqrt(distSq);

            if (dist > 5000.0) continue;

            FVector_UE5 DirNorm{ Dir.X / dist, Dir.Y / dist, Dir.Z / dist };
            double dot = CameraForward.X * DirNorm.X + CameraForward.Y * DirNorm.Y + CameraForward.Z * DirNorm.Z;

            if (dot >= 0.80 && dot > highestDot) {
                highestDot = dot;
                aimedPal = Pal;
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

        RC::Unreal::UFunction* GetViewTargetFunc = CurrentPlayerController->GetFunctionByNameInChain(STR("GetViewTarget")); 
        if (GetViewTargetFunc) {
            alignas(8) uint8_t Params[32] = {0};
            CurrentPlayerController->ProcessEvent(GetViewTargetFunc, Params);
            FProperty* RetProp = GetViewTargetFunc->GetPropertyByNameInChain(STR("ReturnValue"));
            if (RetProp) OriginalViewTarget = *RetProp->ContainerPtrToValuePtr<UObject*>(Params);
        }

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
        if (bIsCapturingKey) {
            CancelKeyCapture();
        }
        FlushPendingSave();
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
        ResetButton = nullptr;
        MorphSliderPool.clear(); 
        ActiveMorphSlidersCount = 0;
        FocusPalSwitch = nullptr;
        RelativeCameraSwitch = nullptr;
        CameraRotationSlider = nullptr;
        SizeSlider = nullptr;
        MainScrollBoxObj = nullptr;
        GetScrollOffsetFunc = nullptr;
        
        DynamicMorphBox = nullptr;
        DynamicLogBox = nullptr;
        CameraRotationContainer = nullptr;
        SizeSliderContainer = nullptr;
        HeaderTextObj = nullptr;
        WidgetTrashBin = nullptr;

        LastObservedSize = -999.0;
        LastObservedLabel = L"";

        LogTextPool.clear();
        DropdownOptions.clear();
        DropdownConfigIndices.clear();

        OriginalViewTarget = nullptr;
        bIsPalCameraActive = false;
    }

    void UIManager::OnOpen() {
        RefreshUI();
    }

    void UIManager::OnClose() {
        if (bIsCapturingKey) {
            CancelKeyCapture();
        }
        FlushPendingSave();

        if (SkinDropdown) SkinDropdown->ClosePopup();

        TargetPal = nullptr;
        TargetInstanceID = L"";
        TargetCharID = L"";

        LastScrollOffset = 0.0f;
        bNeedsRefresh = false;

        DisablePalCamera();
        OriginalViewTarget = nullptr;
    }

    std::wstring UIManager::FormatKeybindText(const std::wstring& Modifier, const std::wstring& Key) {
        std::wstring result = L"[ ";
        if (!Modifier.empty()) result += Modifier + L" + ";
        result += Key + L" ]";
        return result;
    }


    // Helper: Polls Gamepad/Steam Deck inputs directly via XInput (immune to UI mode locks)
    static bool PollGamepadInput(std::wstring& OutKey) {
        typedef DWORD(WINAPI* PFN_XInputGetState)(DWORD, XINPUT_STATE*);
        static PFN_XInputGetState pfnXInputGetState = nullptr;
        static bool bTriedLoad = false;
        if (!bTriedLoad) {
            bTriedLoad = true;
            HMODULE hXInput = LoadLibraryA("xinput1_4.dll");
            if (!hXInput) hXInput = LoadLibraryA("xinput9_1_0.dll");
            if (!hXInput) hXInput = LoadLibraryA("xinput1_3.dll");
            if (hXInput) {
                pfnXInputGetState = (PFN_XInputGetState)GetProcAddress(hXInput, "XInputGetState");
            }
        }
        if (!pfnXInputGetState) return false;

        XINPUT_STATE state;
        ZeroMemory(&state, sizeof(XINPUT_STATE));
        for (DWORD i = 0; i < 4; ++i) {
            if (pfnXInputGetState(i, &state) == ERROR_SUCCESS) {
                WORD b = state.Gamepad.wButtons;
                if (b & XINPUT_GAMEPAD_A) { OutKey = L"Gamepad_FaceButton_Bottom"; return true; }
                if (b & XINPUT_GAMEPAD_B) { OutKey = L"Gamepad_FaceButton_Right"; return true; }
                if (b & XINPUT_GAMEPAD_X) { OutKey = L"Gamepad_FaceButton_Left"; return true; }
                if (b & XINPUT_GAMEPAD_Y) { OutKey = L"Gamepad_FaceButton_Top"; return true; }
                if (b & XINPUT_GAMEPAD_START) { OutKey = L"Gamepad_Special_Right"; return true; }
                if (b & XINPUT_GAMEPAD_BACK) { OutKey = L"Gamepad_Special_Left"; return true; }
                if (b & XINPUT_GAMEPAD_LEFT_SHOULDER) { OutKey = L"Gamepad_LeftShoulder"; return true; }
                if (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) { OutKey = L"Gamepad_RightShoulder"; return true; }
                if (b & XINPUT_GAMEPAD_LEFT_THUMB) { OutKey = L"Gamepad_LeftThumbstick"; return true; }
                if (b & XINPUT_GAMEPAD_RIGHT_THUMB) { OutKey = L"Gamepad_RightThumbstick"; return true; }
                if (b & XINPUT_GAMEPAD_DPAD_UP) { OutKey = L"Gamepad_DPad_Up"; return true; }
                if (b & XINPUT_GAMEPAD_DPAD_DOWN) { OutKey = L"Gamepad_DPad_Down"; return true; }
                if (b & XINPUT_GAMEPAD_DPAD_LEFT) { OutKey = L"Gamepad_DPad_Left"; return true; }
                if (b & XINPUT_GAMEPAD_DPAD_RIGHT) { OutKey = L"Gamepad_DPad_Right"; return true; }
                if (state.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) { OutKey = L"Gamepad_LeftTrigger"; return true; }
                if (state.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) { OutKey = L"Gamepad_RightTrigger"; return true; }
            }
        }
        return false;
    }

    void UIManager::CancelKeyCapture() {
        bIsCapturingKey = false;
        bCloseOnEscape = true; // Restore normal ESC-to-close menu behavior
        if (CaptureOverlay && Utils::IsObjectValid(CaptureOverlay)) {
            Utils::CallFunction(CaptureOverlay, STR("RemoveFromParent"));
        }
        CaptureOverlay = nullptr;
        DP_LOG(Default, "[Keybind] Key capture cancelled.");
    }

    void UIManager::StartKeyCapture(int Target) {
        bIsCapturingKey = true;
        bCloseOnEscape = false; // Prevent UIBase from closing the menu on ESC!
        CapturingTarget = Target;
        CaptureDebounceFrames = 10; // 10-frame buffer so the mouse-up click isn't captured

        RC::Unreal::UObject* PalFontCache = Utils::LoadAssetSafely(UI::Assets::Fonts::PalDefault);
        
        auto OverlayBuilder = UI::Canvas(MyWidget)
            .AddToCanvas(
                UI::Border(MyWidget).BrushColor({0.0f, 0.0f, 0.0f, 0.90f}),
                [](CanvasSlotBuilder& Slot) { Slot.Anchors(0.0, 0.0, 1.0, 1.0).Offsets(0, 0, 0, 0); }
            )
            .AddToCanvas(
                UI::Text(MyWidget)
                .Text(L"Press Any Key or Button...\n(Press ESC to cancel)")
                .Font(PalFontCache, L"Bold", 32)
                .TextColor({1.0f, 1.0f, 1.0f, 1.0f}),
                [](CanvasSlotBuilder& Slot) { Slot.Anchors(0.5, 0.5, 0.5, 0.5).Alignment(0.5, 0.5).AutoSize(true); }
            );

        CaptureOverlay = OverlayBuilder.Build();
        
        RC::Unreal::UObject* WidgetTree = nullptr;
        RC::Unreal::UObject* RootCanvas = nullptr;
        if (Utils::GetPropertyValue(MyWidget, STR("WidgetTree"), WidgetTree, true) && WidgetTree) {
            Utils::GetPropertyValue(WidgetTree, STR("RootWidget"), RootCanvas, true);
        }

        if (RootCanvas && CaptureOverlay) {
            struct { RC::Unreal::UObject* Content; RC::Unreal::UObject* ReturnValue; } AddParams{CaptureOverlay, nullptr};
            Utils::CallFunction(RootCanvas, STR("AddChild"), &AddParams);
            if (AddParams.ReturnValue) {
                struct { int32_t ZOrder; } ZParams{99999};
                Utils::CallFunction(AddParams.ReturnValue, STR("SetZOrder"), &ZParams);
            }
        }
    }


    void UIManager::PreloadUI(RC::Unreal::UObject* PC) {
        if (!SkinDropdown) {
            SkinDropdown = std::make_unique<UI::Dropdown>(std::vector<std::wstring>{}, 0);
        }
        
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

            struct { uint8_t InVisibility; } VisParams{ 1 };
            Utils::CallFunction(PreloadContainer, STR("SetVisibility"), &VisParams);
            
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
        TabBtn1 = nullptr;
        TabBtn2 = nullptr;
        BindMenuBtn = nullptr;
        BindTestMenuBtn = nullptr;

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

        auto HeaderTextBuilder = UI::Text(MyWidget).Text(L"DYN PALS: " + TargetCharID).Font(PalFontCache, L"Bold", 24).TextOutline(2, {0.0f, 0.0f, 0.0f, 1.0f}).TextColor(PalBlue);
        HeaderTextObj = HeaderTextBuilder.Build();

        auto HeaderBox = UI::HorizontalBox(MyWidget)
            .AddToHorizontalBox(UI::Image(MyWidget).ImageFromAsset(UI::Assets::Common::NoticeMark).ImageColor(PalBlue).ImageSize(24, 24), [](BoxSlotBuilder& Slot) { Slot.Padding(0, 0, 10, 0).VerticalAlignment(EBuilderVerticalAlignment::VAlign_Center); }) 
            .AddToHorizontalBox(DynPals::WidgetBuilder(HeaderTextObj));

        // --- TABS ---
        auto TabLayout = UI::HorizontalBox(MyWidget);
        auto Tab1Builder = UI::OptionTab(MyWidget).SetupTab(L"Settings", 0);
        auto Tab2Builder = UI::OptionTab(MyWidget).SetupTab(L"Preferences", 1);
        UObject* Tab1Widget = Tab1Builder.Build();
        UObject* Tab2Widget = Tab2Builder.Build();

        TabBtn1 = std::make_unique<UI::Button>(Tab1Widget);
        TabBtn1->OnClicked([this]() { if (ActiveTab != 0) { ActiveTab = 0; if (SkinDropdown) SkinDropdown->ClosePopup(); RequestRebuild(); } });
        TabBtn2 = std::make_unique<UI::Button>(Tab2Widget);
        TabBtn2->OnClicked([this]() { if (ActiveTab != 1) { ActiveTab = 1; if (SkinDropdown) SkinDropdown->ClosePopup(); RequestRebuild(); } });

        TabLayout.AddToHorizontalBox(Tab1Builder, [](BoxSlotBuilder& Slot) { Slot.Padding(0, 0, 10, 0); }).AddToHorizontalBox(Tab2Builder);

        // --- MAIN CONTENT ---
        auto InnerContentBox = UI::VerticalBox(MyWidget);
        
        if (ActiveTab == 0) {
            DynamicMorphBox = UI::VerticalBox(MyWidget).Build();
            DynamicLogBox = UI::VerticalBox(MyWidget).Build();
            CameraRotationContainer = UI::VerticalBox(MyWidget).Build();
            SizeSliderContainer = UI::VerticalBox(MyWidget).Build();

            WidgetTrashBin = UI::VerticalBox(MyWidget).Build();
            struct { uint8_t InVisibility; } VisParams{ 1 };
            Utils::CallFunction(WidgetTrashBin, STR("SetVisibility"), &VisParams);

            if (!SkinDropdown) { SkinDropdown = std::make_unique<UI::Dropdown>(std::vector<std::wstring>{}, 0); }
            SkinDropdown->SetTrashBin(WidgetTrashBin);

            SkinDropdown->OnChanged([this](int Index, std::wstring Choice) {
                if (Index >= 0 && Index < static_cast<int>(DropdownConfigIndices.size())) {
                    int TargetConfig = DropdownConfigIndices[Index];
                    if (TargetConfig != -1) PalProcessor::Get().ForceSwap(TargetPal, TargetConfig);
                    CacheScrollOffset();
                }
            });

            HideInvalidSwitch = std::make_unique<UI::Switch>(MyWidget, bHideInvalidSwaps);
            HideInvalidSwitch->OnChanged([this](bool bState) { bHideInvalidSwaps = bState; CacheScrollOffset(); bNeedsRefresh = true; });

            FocusPalSwitch = std::make_unique<UI::Switch>(MyWidget, SaveManager::Get().Settings.bFocusPal);
            FocusPalSwitch->OnChanged([this](bool bState) {
                SaveManager::Get().Settings.bFocusPal = bState; SaveManager::Get().SaveWorldData();
                if (bState) EnablePalCamera(); else DisablePalCamera();
                CacheScrollOffset(); bNeedsRefresh = true; 
            });

            UObject* RerollBtnObj = WidgetBuilder(UI::Assets::Blueprints::CommonButton, MyWidget).Text(L"      Reroll Pal      ").BackgroundColor(PalBlue).DesiredSizeOverride(300.0f, 45.0f).UnlockButtonSize(300.0f).Build();
            RerollButton = std::make_unique<UI::Button>(RerollBtnObj);
            RerollButton->OnClicked([this]() { PalProcessor::Get().ProcessPal(TargetPal, true); CacheScrollOffset(); bNeedsRefresh = true; });

            UObject* ResetBtnObj = WidgetBuilder(UI::Assets::Blueprints::CommonButton, MyWidget).Text(L"      Reset Pal      ").BackgroundColor(FLinearColor_UE5{0.85f, 0.25f, 0.25f, 1.0f}).DesiredSizeOverride(300.0f, 45.0f).UnlockButtonSize(300.0f).Build();
            ResetButton = std::make_unique<UI::Button>(ResetBtnObj);
            ResetButton->OnClicked([this]() { if (TargetPal && Utils::IsObjectValid(TargetPal)) { PalProcessor::Get().ResetPal(TargetPal); CacheScrollOffset(); bNeedsRefresh = true; } });

            InnerContentBox.AddToVerticalBox(UI::HorizontalBox(MyWidget).AddToHorizontalBox(UI::Text(MyWidget).Text(L"Current Swap:").Font(PalFontCache, L"Medium", 20).TextColor(Emerald), [](DynPals::BoxSlotBuilder& Slot) { Slot.Padding(0, 0, 0, 10); }));
            InnerContentBox.AddToVerticalBox(DynPals::WidgetBuilder(L"/Script/UMG.SizeBox", MyWidget).AddChild(DynPals::WidgetBuilder(SkinDropdown->Build(MyWidget, CurrentPlayerController))), [](DynPals::BoxSlotBuilder& Slot) { Slot.Padding(0, 0, 0, 20); });
            InnerContentBox.AddToVerticalBox(WidgetBuilder(RerollBtnObj), [](DynPals::BoxSlotBuilder& Slot) { Slot.Padding(20.0f, 0.0f, 20.0f, 10.0f).HorizontalAlignment(DynPals::EBuilderHorizontalAlignment::HAlign_Center); });
            InnerContentBox.AddToVerticalBox(WidgetBuilder(ResetBtnObj), [](DynPals::BoxSlotBuilder& Slot) { Slot.Padding(20.0f, 0.0f, 20.0f, 15.0f).HorizontalAlignment(DynPals::EBuilderHorizontalAlignment::HAlign_Center); });

            auto FilterRow = UI::HorizontalBox(MyWidget)
                .AddToHorizontalBox(DynPals::WidgetBuilder(HideInvalidSwitch->GetWidget()), [](DynPals::BoxSlotBuilder& Slot) { Slot.Padding(0, 0, 10, 0); })
                .AddToHorizontalBox(UI::Text(MyWidget).Text(L"Hide Invalid").Font(PalFontCache, L"Medium", 18).TextColor(White), [](DynPals::BoxSlotBuilder& Slot) { Slot.VerticalAlignment(DynPals::EBuilderVerticalAlignment::VAlign_Center); });
            InnerContentBox.AddToVerticalBox(FilterRow, [](DynPals::BoxSlotBuilder& Slot) { Slot.Padding(0, 0, 0, 25); });

            auto CameraSettingsRow = UI::HorizontalBox(MyWidget)
                .AddToHorizontalBox(DynPals::WidgetBuilder(FocusPalSwitch->GetWidget()), [](DynPals::BoxSlotBuilder& Slot) { Slot.Padding(0, 0, 10, 0); })
                .AddToHorizontalBox(UI::Text(MyWidget).Text(L"Focus Pal Camera").Font(PalFontCache, L"Medium", 18).TextColor(White), [](DynPals::BoxSlotBuilder& Slot) { Slot.VerticalAlignment(DynPals::EBuilderVerticalAlignment::VAlign_Center); });
            InnerContentBox.AddToVerticalBox(CameraSettingsRow, [](DynPals::BoxSlotBuilder& Slot) { Slot.Padding(0, 0, 0, 15); });

            InnerContentBox.AddToVerticalBox(DynPals::WidgetBuilder(CameraRotationContainer), [](DynPals::BoxSlotBuilder& Slot) { Slot.Padding(0, 0, 0, 10); });
            InnerContentBox.AddToVerticalBox(DynPals::WidgetBuilder(SizeSliderContainer), [](DynPals::BoxSlotBuilder& Slot) { Slot.Padding(0, 0, 0, 10); });
            InnerContentBox.AddToVerticalBox(DynPals::WidgetBuilder(DynamicMorphBox), [](DynPals::BoxSlotBuilder& Slot) { Slot.Padding(0, 0, 0, 10); });
            InnerContentBox.AddToVerticalBox(DynPals::WidgetBuilder(DynamicLogBox), [](DynPals::BoxSlotBuilder& Slot) { Slot.Padding(0, 10, 0, 10); });
            InnerContentBox.AddToVerticalBox(DynPals::WidgetBuilder(WidgetTrashBin));
        } 
        else if (ActiveTab == 1) { // --- PREFERENCES TAB ---
            auto& Settings = SaveManager::Get().Settings;
            
            UObject* MenuBtnObj = WidgetBuilder(UI::Assets::Blueprints::CommonButton, MyWidget).Text(FormatKeybindText(Settings.MenuModifier, Settings.MenuKey)).DesiredSizeOverride(200.0f, 45.0f).UnlockButtonSize(200.0f).Build();
            BindMenuBtn = std::make_unique<UI::Button>(MenuBtnObj);
            BindMenuBtn->OnClicked([this]() { StartKeyCapture(0); });

            UObject* TestMenuBtnObj = WidgetBuilder(UI::Assets::Blueprints::CommonButton, MyWidget).Text(FormatKeybindText(Settings.TestMenuModifier, Settings.TestMenuKey)).DesiredSizeOverride(200.0f, 45.0f).UnlockButtonSize(200.0f).Build();
            BindTestMenuBtn = std::make_unique<UI::Button>(TestMenuBtnObj);
            BindTestMenuBtn->OnClicked([this]() { StartKeyCapture(1); });

            auto BindRow1 = UI::HorizontalBox(MyWidget)
                .AddToHorizontalBox(UI::Text(MyWidget).Text(L"Main Menu Toggle").Font(PalFontCache, L"Medium", 18).TextColor(White), [](BoxSlotBuilder& Slot) { Slot.Padding(0, 0, 20, 0).VerticalAlignment(EBuilderVerticalAlignment::VAlign_Center); })
                .AddToHorizontalBox(DynPals::WidgetBuilder(MenuBtnObj));

            auto BindRow2 = UI::HorizontalBox(MyWidget)
                .AddToHorizontalBox(UI::Text(MyWidget).Text(L"Test Menu Toggle").Font(PalFontCache, L"Medium", 18).TextColor(White), [](BoxSlotBuilder& Slot) { Slot.Padding(0, 0, 20, 0).VerticalAlignment(EBuilderVerticalAlignment::VAlign_Center); })
                .AddToHorizontalBox(DynPals::WidgetBuilder(TestMenuBtnObj));

            InnerContentBox.AddToVerticalBox(UI::Text(MyWidget).Text(L"Global Keybinds").Font(PalFontCache, L"Bold", 20).TextColor(Emerald), [](BoxSlotBuilder& Slot) { Slot.Padding(0, 0, 0, 15); });
            InnerContentBox.AddToVerticalBox(BindRow1, [](BoxSlotBuilder& Slot) { Slot.Padding(0, 0, 0, 15); });
            InnerContentBox.AddToVerticalBox(BindRow2, [](BoxSlotBuilder& Slot) { Slot.Padding(0, 0, 0, 15); });
        }

        auto MainScrollBoxBuilder = UI::ScrollBox(MyWidget).AddChild(InnerContentBox);
        MainScrollBoxObj = MainScrollBoxBuilder.Build();
        if (MainScrollBoxObj) GetScrollOffsetFunc = MainScrollBoxObj->GetFunctionByNameInChain(STR("GetScrollOffset"));

        auto MainContentConstrained = UI::SizeBox(MyWidget).HeightOverride(560.0f).AddChild(MainScrollBoxBuilder);

        UObject* Canvas = UI::WindowFrame(MyWidget, 650.0f)
            .SetHeader(HeaderBox)
            .AddAutoScaledRow(TabLayout, 20.0f)
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

        struct { bool bActive; } Tab1ActiveParams{ ActiveTab == 0 };
        struct { bool bActive; } Tab2ActiveParams{ ActiveTab == 1 };
        Utils::CallFunction(Tab1Widget, STR("SetTabActive"), &Tab1ActiveParams);
        Utils::CallFunction(Tab2Widget, STR("SetTabActive"), &Tab2ActiveParams);
    }

    RC::Unreal::UObject* UIManager::GetDesiredFocusTarget() const {
        if (RerollButton && RerollButton->GetWidget()) {
            return RerollButton->GetWidget();
        }
        return MyWidget;
    }

    void UIManager::RefreshUI() {
        if (!TargetPal || !DynamicLogBox || !DynamicMorphBox || !CameraRotationContainer) return;

        const FLinearColor_UE5 PalBlue = {0.78f, 0.96f, 1.0f, 1.0f}; 
        const FLinearColor_UE5 White   = {1.0f, 1.0f, 1.0f, 1.0f};
        const FLinearColor_UE5 Emerald = {0.063f, 0.725f, 0.506f, 1.0f};

        // Clear visual containers
        Utils::CallFunction(CameraRotationContainer, STR("ClearChildren"));
        Utils::CallFunction(DynamicMorphBox, STR("ClearChildren"));
        Utils::CallFunction(DynamicLogBox, STR("ClearChildren"));

        // Helper lambda: Consolidated VBox child addition with padding
        auto AddToVBox = [](UObject* Container, UObject* Widget, float Bottom = 10.0f, float Left = 0.0f, float Top = 0.0f, float Right = 0.0f) {
            if (!Container || !Widget) return;
            struct { UObject* Content; UObject* ReturnValue; } Params{Widget, nullptr};
            Utils::CallFunction(Container, STR("AddChildToVerticalBox"), &Params);
            if (Params.ReturnValue) {
                DynPals::BoxSlotBuilder Slot(Params.ReturnValue);
                Slot.Padding(Left, Top, Right, Bottom);
            }
        };

        PalPersistData* currentPersist = SaveManager::Get().GetPersistData(TargetInstanceID);

        // 1. Dynamic Header Text Update
        if (HeaderTextObj) {
            std::wstring headerStr = L"DYN PALS: " + TargetCharID;
            if (currentPersist && currentPersist->bIsManuallyLocked) {
                headerStr += L" [LOCKED]";
            }
            DynPals::Utils::SetTextSafely(HeaderTextObj, STR("SetText"), headerStr);
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

        std::wstring CurrentSwapLabel = currentPersist ? currentPersist->SwapLabel : L"";
        auto evaluations = ConfigManager::Get().EvaluateAllSwaps(TargetCharID, IsRare, GenderStr, Traits, LevelNum, SkinName, RankNum, FriendshipNum, IsWild, CurrentSwapLabel);

        int bestScore = 999999;
        for (const auto& eval : evaluations) {
            if (eval.IsValid && eval.Score < bestScore) bestScore = eval.Score;
        }

        double totalTiedWeight = 0.0;
        for (const auto& eval : evaluations) {
            if (eval.IsValid && eval.Score == bestScore) {
                double w = ConfigManager::Get().GetConfigs()[eval.ConfigIndex].SpawnWeight;
                if (w > 0.0) totalTiedWeight += w;
            }
        }

        int persistConfigIndex = currentPersist ? ConfigManager::Get().FindConfigIndex(currentPersist->PackName, currentPersist->SkinName, currentPersist->SwapLabel, currentPersist->SkelMeshPath, TargetCharID) : -1;

        std::map<std::wstring, std::vector<SwapEvaluation>> rawPacks;
        for (const auto& eval : evaluations) {
            if (bHideInvalidSwaps && !eval.IsValid && eval.ConfigIndex != persistConfigIndex) {
                continue;
            }
            rawPacks[ConfigManager::Get().GetConfigs()[eval.ConfigIndex].PackName].push_back(eval);
        }

        // 3. Consolidated Dropdown Reconstruction (Single Pre-Parse Pass)
        std::vector<std::wstring> newDropdownOptions;
        std::vector<int> newDropdownConfigIndices;

        struct FParsedDropdownItem {
            int ConfigIndex;
            std::wstring CleanLabel;
            std::vector<std::wstring> Tokens;
        };

        for (auto& [packName, evals] : rawPacks) {
            newDropdownOptions.push_back(L"[ " + packName + L" ]");
            newDropdownConfigIndices.push_back(-1); 

            std::map<std::wstring, int> prefixCounts;
            std::vector<FParsedDropdownItem> parsedItems;
            parsedItems.reserve(evals.size());

            // Single Pre-parsing Pass: Extracts label, strips prefixes, tokenizes once
            for (auto& eval : evals) {
                auto& cfg = ConfigManager::Get().GetConfigs()[eval.ConfigIndex];
                std::wstring label = StripFallbackHash(cfg.SwapLabel);
                if (label.empty()) label = cfg.SkinName;
                if (label.empty()) {
                    label = cfg.SkelMeshPath;
                    size_t slash = label.find_last_of(L'/');
                    if (slash != std::wstring::npos) label = label.substr(slash + 1);
                    size_t dot = label.find(L'.');
                    if (dot != std::wstring::npos) label = label.substr(0, dot);
                }

                FParsedDropdownItem item{ eval.ConfigIndex, label, {} };

                if (label.rfind(L"SK_", 0) == 0 || label.rfind(L"sk_", 0) == 0 || label.find(L'_') != std::wstring::npos) {
                    std::wstring clean = label;
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

                    size_t start = 0, end = 0;
                    while ((end = clean.find(L'_', start)) != std::wstring::npos) {
                        if (end != start) item.Tokens.push_back(clean.substr(start, end - start));
                        start = end + 1;
                    }
                    if (start < clean.length()) item.Tokens.push_back(clean.substr(start));

                    item.CleanLabel = clean;
                    if (item.Tokens.size() >= 3) prefixCounts[item.Tokens[0] + L"_" + item.Tokens[1]]++;
                    if (item.Tokens.size() >= 2) prefixCounts[item.Tokens[0]]++;
                }

                parsedItems.push_back(std::move(item));
            }

            // Assembly Pass: Generates clean display strings from cached tokens
            for (const auto& item : parsedItems) {
                std::wstring display = item.CleanLabel;

                if (!item.Tokens.empty()) {
                    std::wstring bestPrefix = L"";
                    int prefixTokens = 0;
                    if (item.Tokens.size() >= 3 && prefixCounts[item.Tokens[0] + L"_" + item.Tokens[1]] >= 2) {
                        bestPrefix = item.Tokens[0] + L"_" + item.Tokens[1];
                        prefixTokens = 2;
                    } else if (item.Tokens.size() >= 2 && prefixCounts[item.Tokens[0]] >= 2) {
                        bestPrefix = item.Tokens[0];
                        prefixTokens = 1;
                    }

                    if (!bestPrefix.empty()) {
                        display = L"";
                        for (size_t i = prefixTokens; i < item.Tokens.size(); ++i) {
                            display += item.Tokens[i] + (i < item.Tokens.size() - 1 ? L" " : L"");
                        }
                    } else {
                        std::replace(display.begin(), display.end(), L'_', L' ');
                    }

                    // CamelCase spacing
                    std::wstring splitDisplay = L"";
                    if (!display.empty()) {
                        splitDisplay.push_back(display[0]);
                        for (size_t i = 1; i < display.size(); ++i) {
                            if ((display[i - 1] >= L'a' && display[i - 1] <= L'z') && (display[i] >= L'A' && display[i] <= L'Z')) {
                                splitDisplay.push_back(L' ');
                            }
                            splitDisplay.push_back(display[i]);
                        }
                        display = splitDisplay;
                    }
                }

                newDropdownOptions.push_back(L"   " + display);
                newDropdownConfigIndices.push_back(item.ConfigIndex);
            }
        }

        if (newDropdownOptions.empty()) {
            newDropdownOptions.push_back(L"   (No Skins Available)");
            newDropdownConfigIndices.push_back(-1);
        }

        bool bOptionsChanged = (newDropdownOptions.size() != DropdownOptions.size());
        if (!bOptionsChanged) {
            for (size_t i = 0; i < newDropdownOptions.size(); ++i) {
                if (newDropdownOptions[i] != DropdownOptions[i] || newDropdownConfigIndices[i] != DropdownConfigIndices[i]) {
                    bOptionsChanged = true;
                    break;
                }
            }
        }

        int initialIdx = -1;
        if (persistConfigIndex != -1) {
            for (size_t i = 0; i < newDropdownConfigIndices.size(); ++i) {
                if (newDropdownConfigIndices[i] == persistConfigIndex) {
                    initialIdx = static_cast<int>(i);
                    break;
                }
            }
        }

        static std::wstring lastTargetInstanceID = L"";
        static int lastPersistConfigIndex = -999;

        if (bOptionsChanged || TargetInstanceID != lastTargetInstanceID || persistConfigIndex != lastPersistConfigIndex) {
            DropdownOptions = std::move(newDropdownOptions);
            DropdownConfigIndices = std::move(newDropdownConfigIndices);
            lastTargetInstanceID = TargetInstanceID;
            lastPersistConfigIndex = persistConfigIndex;

            if (SkinDropdown) {
                SkinDropdown->SetOptions(DropdownOptions, initialIdx);
            }
        }

        // 4. Text Widget Pooling Lambda
        int logTextUsed = 0;
        auto GetPooledText = [&](const std::wstring& TextStr, const FLinearColor_UE5& Color, int32_t FontSize, const wchar_t* Typeface) -> RC::Unreal::UObject* {
            RC::Unreal::UObject* TextObj = nullptr;
            if (logTextUsed < LogTextPool.size()) {
                TextObj = LogTextPool[logTextUsed];
                if (!Utils::IsObjectValid(TextObj)) {
                    TextObj = UI::Text(MyWidget).Build();
                    LogTextPool[logTextUsed] = TextObj;
                } else {
                    Utils::CallFunction(TextObj, STR("RemoveFromParent"));
                }
            } else {
                TextObj = UI::Text(MyWidget).Build();
                LogTextPool.push_back(TextObj);
            }
            logTextUsed++;

            DynPals::Utils::SetTextSafely(TextObj, STR("SetText"), TextStr);
            DynPals::UI::SetTextColor(TextObj, Color);
            
            FProperty* FontProp = Utils::GetProperty(TextObj, STR("Font"));
            if (FontProp) {
                void* FontPtr = FontProp->ContainerPtrToValuePtr<void>(TextObj);
                if (FontPtr) {
                    FStructProperty* StructProp = static_cast<FStructProperty*>(FontProp);
                    if (StructProp && StructProp->GetStruct()) {
                        UStruct* FontStruct = StructProp->GetStruct();
                        if (PalFontCache) {
                            FProperty* ObjProp = FontStruct->GetPropertyByNameInChain(STR("FontObject"));
                            if (ObjProp) *ObjProp->ContainerPtrToValuePtr<UObject*>(FontPtr) = PalFontCache;
                        }
                        FProperty* NameProp = FontStruct->GetPropertyByNameInChain(STR("TypefaceFontName"));
                        if (NameProp) *NameProp->ContainerPtrToValuePtr<FName>(FontPtr) = FName(Typeface, FNAME_Add);
                        FProperty* SizeProp = FontStruct->GetPropertyByNameInChain(STR("Size"));
                        if (SizeProp) *SizeProp->ContainerPtrToValuePtr<int32_t>(FontPtr) = FontSize;
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

        // 5. Camera Controls (Relative Switch + Rotation Slider)
        if (SaveManager::Get().Settings.bFocusPal) {
            RelativeCameraSwitch = std::make_unique<UI::Switch>(MyWidget, SaveManager::Get().Settings.bRelativeCamera);
            RelativeCameraSwitch->OnChanged([this](bool bState) {
                SaveManager::Get().Settings.bRelativeCamera = bState;
                SaveManager::Get().SaveWorldData();
                UpdatePalCameraRotation(SaveManager::Get().Settings.CameraRotation);
            });

            auto ShrunkSwitch = UI::Scaled(MyWidget, DynPals::WidgetBuilder(RelativeCameraSwitch->GetWidget()), 0.6f);

            auto RelativeCamRow = UI::HorizontalBox(MyWidget)
                .AddToHorizontalBox(ShrunkSwitch, [](DynPals::BoxSlotBuilder& Slot) { 
                    Slot.Padding(0, 0, 8, 0).VerticalAlignment(DynPals::EBuilderVerticalAlignment::VAlign_Center); 
                })
                .AddToHorizontalBox(UI::Text(MyWidget).Text(L"Relative Camera").Font(PalFontCache, L"Medium", 15).TextColor(FLinearColor_UE5{0.85f, 0.85f, 0.85f, 1.0f}), [](DynPals::BoxSlotBuilder& Slot) { 
                    Slot.VerticalAlignment(DynPals::EBuilderVerticalAlignment::VAlign_Center); 
                });

            AddToVBox(CameraRotationContainer, RelativeCamRow.Build(), 12.0f, 25.0f);

            CameraRotationSlider = std::make_unique<UI::Slider>(MyWidget, 0.0, 360.0, SaveManager::Get().Settings.CameraRotation);
            CameraRotationSlider->OnChanged([this](double NewValue) {
                SaveManager::Get().Settings.CameraRotation = NewValue;
                UpdatePalCameraRotation(NewValue);
                TriggerSaveDebounced(); // <--- Debounce disk write!
            });


            AddToVBox(CameraRotationContainer, GetPooledText(L"Camera Rotation", White, 18, L"Medium"), 0.0f);
            AddToVBox(CameraRotationContainer, CameraRotationSlider->GetWidget(), 20.0f, 0.0f, 5.0f);
        } else {
            RelativeCameraSwitch = nullptr;
            CameraRotationSlider = nullptr;
        }

        // 6. Dynamic Size Slider
        Utils::CallFunction(SizeSliderContainer, STR("ClearChildren"));
        SizeSlider = nullptr;

        if (currentPersist && persistConfigIndex != -1) {
            auto& activeCfg = ConfigManager::Get().GetConfigs()[persistConfigIndex];
            if (activeCfg.MinSizeMultiplier < activeCfg.MaxSizeMultiplier) {
                double currentVal = currentPersist->SizeMultiplier > 0.0 ? currentPersist->SizeMultiplier : 1.0;
                currentVal = std::clamp(currentVal, activeCfg.MinSizeMultiplier, activeCfg.MaxSizeMultiplier);

                SizeSlider = std::make_unique<UI::Slider>(MyWidget, activeCfg.MinSizeMultiplier, activeCfg.MaxSizeMultiplier, currentVal);
                SizeSlider->OnChanged([this](double NewValue) {
                    LastObservedSize = NewValue;

                    PalPersistData* p = SaveManager::Get().GetPersistData(TargetInstanceID);
                    if (p) {
                        p->SizeMultiplier = NewValue;
                        SaveManager::Get().SetPersistData(TargetInstanceID, *p, false); // <--- Pass false (memory only)

                        if (TargetPal && Utils::IsObjectValid(TargetPal)) {
                            UObject* MeshComp = nullptr;
                            Utils::CallFunction(TargetPal, STR("GetMainMesh"), &MeshComp);
                            if (MeshComp && Utils::IsObjectValid(MeshComp)) {
                                UClass* CharClass = TargetPal->GetClassPrivate();
                                if (CharClass) {
                                    UObject* CDO = CharClass->GetClassDefaultObject();
                                    UObject* VanillaMesh = nullptr;
                                    if (CDO) Utils::GetPropertyValue<UObject*>(CDO, STR("Mesh"), VanillaMesh);
                                    
                                    FVector_UE5 BaseScale{ 1.0, 1.0, 1.0 };
                                    if (VanillaMesh) {
                                        FVector_UE5 CDOScale{ 1.0, 1.0, 1.0 };
                                        if (Utils::GetPropertyValue<FVector_UE5>(VanillaMesh, STR("RelativeScale3D"), CDOScale)) {
                                            if (CDOScale.X > 0.001 && CDOScale.Y > 0.001 && CDOScale.Z > 0.001) BaseScale = CDOScale;
                                        }
                                    }

                                    if (NewValue <= 0.001) NewValue = 1.0;
                                    FVector_UE5 FinalMeshScale{ BaseScale.X * NewValue, BaseScale.Y * NewValue, BaseScale.Z * NewValue };

                                    Utils::SetPropertyValue<FVector_UE5>(MeshComp, STR("DefaultScale3D"), FinalMeshScale);
                                    struct { FVector_UE5 NewScale3D; } ScaleParams{ FinalMeshScale };
                                    Utils::CallFunction(MeshComp, STR("SetRelativeScale3D"), &ScaleParams);
                                }
                            }
                        }
                    }
                    TriggerSaveDebounced(); // <--- Debounce disk write!
                });


                AddToVBox(SizeSliderContainer, GetPooledText(L"Size Adjustment", Emerald, 18, L"Bold"), 0.0f);
                AddToVBox(SizeSliderContainer, SizeSlider->GetWidget(), 15.0f, 0.0f, 5.0f);
            }
        }

        // 7. Shape Keys / Morph Sliders
        ActiveMorphSlidersCount = 0;
        if (currentPersist && persistConfigIndex != -1) {
            auto& activeCfg = ConfigManager::Get().GetConfigs()[persistConfigIndex];
            if (!activeCfg.MorphTargetList.empty()) {
                AddToVBox(DynamicMorphBox, GetPooledText(L"Morph Targets", Emerald, 20, L"Bold"), 10.0f);

                int slidersUsed = 0;
                for (auto& morph : activeCfg.MorphTargetList) {
                    if (morph.type != L"Restrict" && morph.minVal < morph.maxVal) {
                        float currentVal = (float)currentPersist->MorphSet[morph.target];
                        AddToVBox(DynamicMorphBox, GetPooledText(morph.target, White, 18, L"Medium"), 0.0f);

                        class DynPals::UI::Slider* SliderCtrl = nullptr;
                        if (slidersUsed < static_cast<int>(MorphSliderPool.size())) {
                            SliderCtrl = MorphSliderPool[slidersUsed].get();
                            RC::Unreal::UObject* SliderW = SliderCtrl->GetWidget();
                            if (Utils::IsObjectValid(SliderW)) Utils::CallFunction(SliderW, STR("RemoveFromParent"));
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
                                SaveManager::Get().SetPersistData(TargetInstanceID, *p, false); // <--- Pass false (memory only)
                                
                                UObject* MeshComp = nullptr;
                                Utils::CallFunction(TargetPal, STR("GetMainMesh"), &MeshComp);
                                if (MeshComp) {
                                    struct { FName MorphTargetName; float Value; bool bRemoveZeroWeight; } MorphParams{
                                        FName(morphName.c_str(), FNAME_Add), static_cast<float>(NewValue), false
                                    };
                                    Utils::CallFunction(MeshComp, STR("SetMorphTarget"), &MorphParams);
                                }
                            }
                            TriggerSaveDebounced(); // <--- Debounce disk write!
                        });
                        AddToVBox(DynamicMorphBox, SliderCtrl->GetWidget(), 15.0f, 0.0f, 5.0f);
                    }
                }
                ActiveMorphSlidersCount = slidersUsed;
            }
        }

        // 8. Dynamic Log Output
        if (currentPersist && currentPersist->SizeMultiplier > 0.0 && std::abs(currentPersist->SizeMultiplier - 1.0) > 0.005) {
            wchar_t sizeBuf[32];
            swprintf(sizeBuf, 32, L"Size Modifier: %.2fx", currentPersist->SizeMultiplier);
            AddToVBox(DynamicLogBox, GetPooledText(sizeBuf, PalBlue, 18, L"Bold"), 10.0f);
        }
        
        AddToVBox(DynamicLogBox, GetPooledText(L"Matchmaker Log", Emerald, 20, L"Bold"), 10.0f);

        if (evaluations.empty()) {
            AddToVBox(DynamicLogBox, GetPooledText(L"No swaps configured for this Pal.", White, 16, L"Regular"), 0.0f);
        } else {
            for (const auto& eval : evaluations) {
                if (bHideInvalidSwaps && !eval.IsValid) continue;

                auto& cfg = ConfigManager::Get().GetConfigs()[eval.ConfigIndex];
                AddToVBox(DynamicLogBox, GetPooledText(cfg.PackName, White, 16, L"Bold"), 0.0f);

                // --- 1. Prioritize SwapLabel ---
                std::wstring displayLabel = StripFallbackHash(cfg.SwapLabel);
                
                // --- 2. Fallback to SkinName ---
                if (displayLabel.empty()) {
                    displayLabel = cfg.SkinName;
                }
                
                // --- 3. Fallback to Skeletal Mesh Filename ---
                if (displayLabel.empty() && !cfg.SkelMeshPath.empty()) {
                    std::wstring meshName = cfg.SkelMeshPath;
                    size_t slash = meshName.find_last_of(L'/');
                    if (slash != std::wstring::npos) meshName = meshName.substr(slash + 1);
                    size_t dot = meshName.find(L'.');
                    if (dot != std::wstring::npos) meshName = meshName.substr(0, dot);
                    
                    if (meshName.rfind(L"SK_", 0) == 0 || meshName.rfind(L"sk_", 0) == 0) meshName = meshName.substr(3);
                    for (wchar_t& c : meshName) { if (c == L'_') c = L' '; }
                    displayLabel = meshName;
                }
                
                // --- 4. Final Absolute Fallback ---
                if (displayLabel.empty()) {
                    displayLabel = L"(Unlabeled)";
                }

                double pct = 0.0;
                if (eval.IsValid && eval.Score == bestScore && totalTiedWeight > 0.0 && cfg.SpawnWeight > 0.0) {
                    pct = (cfg.SpawnWeight * 100.0) / totalTiedWeight;
                }

                FLinearColor_UE5 textColor = eval.IsValid ? (eval.Score < 0 ? PalBlue : (eval.Score == 0 ? Emerald : FLinearColor_UE5{0.960f, 0.620f, 0.043f, 1.0f})) : FLinearColor_UE5{0.850f, 0.150f, 0.150f, 1.0f};

                std::wstring logStr;
                if (eval.IsValid) {
                    wchar_t pctBuf[16];
                    swprintf(pctBuf, 16, L"%.1f", pct);
                    logStr = L"    " + std::wstring(pctBuf) + L"% : " + displayLabel;
                } else {
                    logStr = L"    [X] " + displayLabel;
                    if (!eval.RejectionReason.empty()) {
                        logStr += L" (" + eval.RejectionReason + L")";
                    }
                }
                AddToVBox(DynamicLogBox, GetPooledText(logStr, textColor, 16, L"Medium"), 8.0f);
            }
        }

        // 9. Move unused pooled elements to Trash Bin
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

        // --- NEW: Keybind Capturing ---
        if (bIsCapturingKey) {
            if (CaptureDebounceFrames > 0) {
                CaptureDebounceFrames--;
                return;
            }

            // 1. ESC cancels the capture overlay without closing the menu
            if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
                CancelKeyCapture();
                RequestRebuild();
                return;
            }

            // 2. Controller / Steam Deck support via direct XInput
            std::wstring gamepadKey;
            if (PollGamepadInput(gamepadKey)) {
                // Gamepad Back (Select) button cancels capture
                if (gamepadKey == L"Gamepad_Special_Left") {
                    CancelKeyCapture();
                    RequestRebuild();
                    return;
                }

                auto& Settings = SaveManager::Get().Settings;
                if (CapturingTarget == 0) {
                    Settings.MenuKey = gamepadKey;
                    Settings.MenuModifier = L""; // Gamepads do not use Alt/Ctrl modifiers
                } else if (CapturingTarget == 1) {
                    Settings.TestMenuKey = gamepadKey;
                    Settings.TestMenuModifier = L"";
                }
                SaveManager::Get().SaveWorldData();
                DP_LOG(Default, "[Keybind] Rebound Target {} to Gamepad: {}", CapturingTarget, gamepadKey);

                CancelKeyCapture();
                RequestRebuild();
                return;
            }

            // 3. Keyboard support via Win32 (bypasses Slate UI-only input lock)
            std::wstring detectedKey = L"";
            
            // Check Letters A-Z
            for (int vk = 'A'; vk <= 'Z'; ++vk) {
                if ((GetAsyncKeyState(vk) & 0x8000) != 0) {
                    detectedKey = std::wstring(1, static_cast<wchar_t>(vk));
                    break;
                }
            }
            // Check Numbers 0-9
            if (detectedKey.empty()) {
                for (int vk = '0'; vk <= '9'; ++vk) {
                    if ((GetAsyncKeyState(vk) & 0x8000) != 0) {
                        detectedKey = std::wstring(1, static_cast<wchar_t>(vk));
                        break;
                    }
                }
            }
            // Check Function Keys F1-F12
            if (detectedKey.empty()) {
                for (int vk = VK_F1; vk <= VK_F12; ++vk) {
                    if ((GetAsyncKeyState(vk) & 0x8000) != 0) {
                        detectedKey = L"F" + std::to_wstring(vk - VK_F1 + 1);
                        break;
                    }
                }
            }
            // Check Special / Navigation Keys
            if (detectedKey.empty()) {
                if ((GetAsyncKeyState(VK_SPACE) & 0x8000) != 0) detectedKey = L"SpaceBar";
                else if ((GetAsyncKeyState(VK_RETURN) & 0x8000) != 0) detectedKey = L"Enter";
                else if ((GetAsyncKeyState(VK_TAB) & 0x8000) != 0) detectedKey = L"Tab";
                else if ((GetAsyncKeyState(VK_OEM_3) & 0x8000) != 0) detectedKey = L"Tilde";
            }

            if (!detectedKey.empty()) {
                // Detect any held modifiers
                std::wstring mod = L"";
                if ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) mod = L"LeftAlt";
                else if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) mod = L"LeftControl";
                else if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) mod = L"LeftShift";

                auto& Settings = SaveManager::Get().Settings;
                if (CapturingTarget == 0) {
                    Settings.MenuKey = detectedKey;
                    Settings.MenuModifier = mod;
                } else if (CapturingTarget == 1) {
                    Settings.TestMenuKey = detectedKey;
                    Settings.TestMenuModifier = mod;
                }
                SaveManager::Get().SaveWorldData();
                DP_LOG(Default, "[Keybind] Rebound Target {} to Keyboard: {} + {}", CapturingTarget, mod, detectedKey);

                CancelKeyCapture();
                RequestRebuild();
                return;
            }

            return; // Block background interaction while overlay is active
        }
        // ------------------------------

        // --- DEBOUNCED DISK SAVE CHECK ---
        if (bPendingDiskSave) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - LastSliderChangeTime).count();
            if (elapsed >= 500) { 
                bPendingDiskSave = false;
                SaveManager::Get().SaveWorldData();
            }
        }

        // --- AUTO-DETECT BACKGROUND SWAP & SIZE UPDATES ---
        PalPersistData* p = SaveManager::Get().GetPersistData(TargetInstanceID);
        if (p) {
            if (p->SizeMultiplier != LastObservedSize || p->SwapLabel != LastObservedLabel) {
                LastObservedSize = p->SizeMultiplier;
                LastObservedLabel = p->SwapLabel;
                bNeedsRefresh = true;
            }
        }
        
        if (bNeedsRefresh) { bNeedsRefresh = false; RefreshUI(); }

        if (TabBtn1) TabBtn1->Tick();
        if (TabBtn2) TabBtn2->Tick();
        if (BindMenuBtn) BindMenuBtn->Tick();
        if (BindTestMenuBtn) BindTestMenuBtn->Tick();

        if (ActiveTab == 0) {
            if (SkinDropdown)         SkinDropdown->Tick();
            if (HideInvalidSwitch)    HideInvalidSwitch->Tick();
            if (RerollButton)         RerollButton->Tick();
            if (ResetButton)          ResetButton->Tick();
            if (FocusPalSwitch)       FocusPalSwitch->Tick();
            if (RelativeCameraSwitch) RelativeCameraSwitch->Tick();
            if (CameraRotationSlider) CameraRotationSlider->Tick();
            if (SizeSlider)           SizeSlider->Tick();

            for (int i = 0; i < ActiveMorphSlidersCount; ++i) {
                if (i < static_cast<int>(MorphSliderPool.size())) MorphSliderPool[i]->Tick();
            }
        }
        
        CacheScrollOffset();

    }
}