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
            RC::Unreal::UObject* OldWidget = MyWidget;
            MyWidget = nullptr; 
            
            BuildWidget(); 
            
            if (OldWidget) {
                Utils::CallFunction(OldWidget, STR("RemoveFromParent")); 
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