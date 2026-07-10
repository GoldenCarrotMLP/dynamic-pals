// --- START OF FILE src/UI/UIBase.cpp ---
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
                    bIsOpen = true;
                    
                    if (!MyWidget) {
                        BuildWidget();
                    } else {
                        // Set visibility to Visible (0)
                        struct { uint8_t InVisibility; } VisParams{ 0 };
                        Utils::CallFunction(MyWidget, STR("SetVisibility"), &VisParams);

                        // Restore rendering position to default (0, 0)
                        struct FVector2D_Double { double X; double Y; };
                        struct { FVector2D_Double Translation; } RenderParams{ {0.0, 0.0} };
                        Utils::CallFunction(MyWidget, STR("SetRenderTranslation"), &RenderParams);
                    }
                    OnOpen();
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
            OnOpen(); 
            
            if (OldWidget) {
                Utils::CallFunction(OldWidget, STR("RemoveFromParent")); 
            }
        }

        if (bStateChanged) {
            UIRegistry::Get().UpdateTickState();
        }

        if (!bIsOpen || !MyWidget) return;

        if (bCloseOnEscape) {
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