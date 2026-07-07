#include "UI/UIRegistry.hpp"
#include "UI/UIBase.hpp"
#include "Utils.hpp"
#include <algorithm>

using namespace RC::Unreal;

namespace DynPals {

    void UIRegistry::RegisterUI(UIBase* UI) {
        RegisteredUIs.push_back(UI);
        UpdateTickState();
    }

    void UIRegistry::UnregisterUI(UIBase* UI) {
        RegisteredUIs.erase(std::remove(RegisteredUIs.begin(), RegisteredUIs.end(), UI), RegisteredUIs.end());
        UpdateTickState();
    }

    void UIRegistry::TickAll(UObject* PlayerController) {
        for (UIBase* UI : RegisteredUIs) {
            UI->ProcessTick(PlayerController);
        }
    }

    void UIRegistry::UpdateTickState() {
        bRequiresTick = false;
        for (const UIBase* UI : RegisteredUIs) {
            if (UI && (UI->IsOpen() || UI->IsToggleRequested())) {
                bRequiresTick = true;
                break;
            }
        }
    }

    void UIRegistry::UpdateInputState(UObject* PlayerController) {
        if (!PlayerController) return;

        bool bNeedsLock = false;
        for (UIBase* UI : RegisteredUIs) {
            if (UI->IsOpen() && UI->RequiresInputLock()) {
                bNeedsLock = true;
                break;
            }
        }

        if (bNeedsLock && !bIsInputLocked) {
            Utils::SetPropertyValue<bool>(PlayerController, STR("bShowMouseCursor"), true);
            Utils::SetPropertyValue<bool>(PlayerController, STR("bEnableClickEvents"), true);
            Utils::SetPropertyValue<bool>(PlayerController, STR("bEnableMouseOverEvents"), true);
            
            // Ignore camera movement
            struct { bool bNewLookInput; } LookParams{ true };
            Utils::CallFunction(PlayerController, STR("SetIgnoreLookInput"), &LookParams);

            // Ignore player/mount movement 
            struct { bool bNewMoveInput; } MoveParams{ true };
            Utils::CallFunction(PlayerController, STR("SetIgnoreMoveInput"), &MoveParams);

            bIsInputLocked = true;
        } 
        else if (!bNeedsLock && bIsInputLocked) {
            Utils::SetPropertyValue<bool>(PlayerController, STR("bShowMouseCursor"), false);
            Utils::SetPropertyValue<bool>(PlayerController, STR("bEnableClickEvents"), false);
            Utils::SetPropertyValue<bool>(PlayerController, STR("bEnableMouseOverEvents"), false);

            Utils::CallFunction(PlayerController, STR("ResetIgnoreLookInput"));
            Utils::CallFunction(PlayerController, STR("ResetIgnoreMoveInput"));
            
            UObject* WBL = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/UMG.Default__WidgetBlueprintLibrary"));
            if (WBL) {
                Utils::CallFunction(WBL, STR("SetFocusToGameViewport"));
                UFunction* InputModeFunc = WBL->GetFunctionByNameInChain(STR("SetInputMode_GameOnly"));
                if (InputModeFunc) {
                    struct { UObject* PC; bool bConsume; } InputModeParams{ PlayerController, false };
                    WBL->ProcessEvent(InputModeFunc, &InputModeParams);
                }
            }

            bIsInputLocked = false;
        }
    }
}