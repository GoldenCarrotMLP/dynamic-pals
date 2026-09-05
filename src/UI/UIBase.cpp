#define NOMINMAX 
#include <Windows.h>
#include "UI/UIBase.hpp"
#include "UI/UIRegistry.hpp"
#include "Utils.hpp"

using namespace RC::Unreal;

namespace DynPals {

    UIBase::UIBase() {
        UIRegistry::Get().RegisterUI(this);
    }

    UIBase::~UIBase() {
        UIRegistry::Get().UnregisterUI(this);
        DestroyWidget();
    }

    void UIBase::RequestToggle() {
        bToggleRequested = true;
        UIRegistry::Get().UpdateTickState();
        // Deferring execution to the next frame tick prevents re-entrancy crashes
    }

    void UIBase::RequestRebuild() {
        bRebuildRequested = true;
        UIRegistry::Get().UpdateTickState();
        // Deferring execution to the next frame tick prevents Use-After-Free crashes
    }

    void UIBase::ProcessTick(UObject* PlayerController) {
        CurrentPlayerController = PlayerController;
        bool bStateChanged = false;

        if (bToggleRequested) {
            bToggleRequested = false;
            bStateChanged = true;
            
            if (!bIsOpen) {
                // Try to acquire target/setup view. If false, abort opening.
                if (OnSetup()) {
                    if (!MyWidget) {
                        BuildWidget();
                    }

                    // Verify the widget exists and is valid before marking the UI open
                    if (MyWidget && Utils::IsObjectValid(MyWidget)) {
                        bIsOpen = true;

                        // Set visibility to Visible (0)
                        struct { uint8_t InVisibility; } VisParams{ 0 };
                        Utils::CallFunction(MyWidget, STR("SetVisibility"), &VisParams);

                        // Restore rendering position to default (0, 0)
                        struct FVector2D_Double { double X; double Y; };
                        struct { FVector2D_Double Translation; } RenderParams{ {0.0, 0.0} };
                        Utils::CallFunction(MyWidget, STR("SetRenderTranslation"), &RenderParams);

                        OnOpen();
                    } else {
                        // BuildWidget failed to produce a valid widget -> Abort opening cleanly
                        bIsOpen = false;
                        MyWidget = nullptr;
                    }
                }
            } else {
                bIsOpen = false;
                
                if (MyWidget) {
                    // Set visibility to Collapsed (1) to suspend rendering/layout
                    struct { uint8_t InVisibility; } VisParams{ 1 };
                    Utils::CallFunction(MyWidget, STR("SetVisibility"), &VisParams);

                    // Move rendering offscreen (avoids breaking Viewport stretch/alignment)
                    struct FVector2D_Double { double X; double Y; };
                    struct { FVector2D_Double Translation; } RenderParams{ {-99999.0, -99999.0} };
                    Utils::CallFunction(MyWidget, STR("SetRenderTranslation"), &RenderParams);
                }
                OnClose();
            }
            
            UIRegistry::Get().UpdateInputState(PlayerController);
        }

        if (bRebuildRequested && bIsOpen) {
            bRebuildRequested = false;
            bStateChanged = true;
            
            // --- ZERO-FLICKER DOUBLE BUFFERING ---
            RC::Unreal::UObject* OldWidget = MyWidget;
            MyWidget = nullptr; 
            
            BuildWidget();

            if (MyWidget && Utils::IsObjectValid(MyWidget)) {
                struct { uint8_t InVisibility; } RebuildVisParams{ 0 };
                Utils::CallFunction(MyWidget, STR("SetVisibility"), &RebuildVisParams);

                OnOpen();
                
                if (OldWidget) {
                    Utils::CallFunction(OldWidget, STR("RemoveFromParent")); 
                }
            } else {
                // If rebuild failed, fall back to the old widget or tear down cleanly
                MyWidget = OldWidget;
                if (!MyWidget || !Utils::IsObjectValid(MyWidget)) {
                    bIsOpen = false;
                    OnClose();
                    UIRegistry::Get().UpdateInputState(PlayerController);
                }
            }
        }

        if (bStateChanged) {
            UIRegistry::Get().UpdateTickState();
        }

        if (!bIsOpen || !MyWidget) return;

        // FIX: If the engine garbage collected our widget silently without telling us, reset the state and abort!
        if (!Utils::IsObjectValid(MyWidget)) {
            MyWidget = nullptr;
            bIsOpen = false;
            bToggleRequested = false;
            OnInvalidate();
            UIRegistry::Get().UpdateTickState();
            return;
        }

        if (bCloseOnEscape && Utils::IsGameWindowFocused()) {
            if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) || (GetAsyncKeyState(VK_TAB) & 0x8000)) {
                if (!bWasEscapeDown) {
                    bWasEscapeDown = true;
                    RequestToggle();
                    return;
                }
            } else {
                bWasEscapeDown = false;
            }
        }

        OnTickUI();
    }

    void UIBase::DestroyWidget() {
        if (MyWidget) {
            Utils::CallFunction(MyWidget, STR("RemoveFromParent"));
            MyWidget = nullptr;
        }
    }
}