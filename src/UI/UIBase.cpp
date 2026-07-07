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

        // Force immediate execution on the Game Thread to prevent hook skipping
        RC::Unreal::UObject* PC = RC::Unreal::UObjectGlobals::FindFirstOf(STR("PalPlayerController"));
        if (PC) {
            ProcessTick(PC);
        }
    }

    void UIBase::RequestRebuild() {
        bRebuildRequested = true;
        UIRegistry::Get().UpdateTickState();

        // Force immediate execution on the Game Thread to prevent visual delay
        RC::Unreal::UObject* PC = RC::Unreal::UObjectGlobals::FindFirstOf(STR("PalPlayerController"));
        if (PC) {
            ProcessTick(PC);
        }
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
                    BuildWidget();
                }
            } else {
                bIsOpen = false;
                DestroyWidget();
                OnClose();
            }
            
            UIRegistry::Get().UpdateInputState(PlayerController);
        }

        if (bRebuildRequested && bIsOpen) {
            bRebuildRequested = false;
            bStateChanged = true;
            
            // --- ZERO-FLICKER DOUBLE BUFFERING ---
            // We keep the old widget on-screen while the new one is built and mounted!
            RC::Unreal::UObject* OldWidget = MyWidget;
            MyWidget = nullptr; 
            
            BuildWidget(); // Instantiates and overlays the fresh UI
            
            if (OldWidget) {
                Utils::CallFunction(OldWidget, STR("RemoveFromParent")); // Safely deletes the old UI
            }
        }

        if (bStateChanged) {
            UIRegistry::Get().UpdateTickState();
        }

        if (!bIsOpen || !MyWidget) return;

        if (bCloseOnEscape) {
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
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
// --- END OF FILE src/UI/UIBase.cpp ---