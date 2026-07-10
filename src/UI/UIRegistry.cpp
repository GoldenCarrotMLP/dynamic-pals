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

        UIBase* OpenUI = nullptr;

        for (UIBase* UI : RegisteredUIs) {

            if (UI && UI->IsOpen() && UI->RequiresInputLock()) {

                bNeedsLock = true;

                OpenUI = UI;

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



            // --- Shift Focus and Released Cursor Capture to UI instantly ---

            if (OpenUI && OpenUI->GetWidget()) {

                UObject* WBL = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/UMG.Default__WidgetBlueprintLibrary"));

                if (WBL) {

                    UFunction* InputModeFunc = WBL->GetFunctionByNameInChain(STR("SetInputMode_GameAndUIEx"));

                    if (!InputModeFunc) {

                        InputModeFunc = WBL->GetFunctionByNameInChain(STR("SetInputMode_GameAndUI"));

                    }

                    

                    if (InputModeFunc) {

                        struct {

                            UObject* TargetPlayerController;

                            UObject* InWidgetToFocus;

                            uint8_t InMouseLockMode;

                            bool bHideCursorDuringCapture;

                        } Params{ PlayerController, OpenUI->GetWidget(), 0, false };

                        WBL->ProcessEvent(InputModeFunc, &Params);

                    }

                }

                

                Utils::CallFunction(OpenUI->GetWidget(), STR("SetKeyboardFocus"));

            }



            bIsInputLocked = true;

        } 

        else if (!bNeedsLock && bIsInputLocked) {

            Utils::SetPropertyValue<bool>(PlayerController, STR("bShowMouseCursor"), false);

            Utils::SetPropertyValue<bool>(PlayerController, STR("bEnableClickEvents"), false);

            Utils::SetPropertyValue<bool>(PlayerController, STR("bEnableMouseOverEvents"), false);



            Utils::CallFunction(PlayerController, STR("ResetIgnoreLookInput"));

            Utils::CallFunction(PlayerController, STR("ResetIgnoreMoveInput")); // wait, is it CurrentPlayerController or PlayerController? The function parameter is PlayerController.

            // Let's verify what the original code had:

            // "Utils::CallFunction(PlayerController, STR(\"ResetIgnoreLookInput\"));"

            // "Utils::CallFunction(PlayerController, STR(\"ResetIgnoreMoveInput\"));"

            

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

        void UIRegistry::InvalidateAllUIs() {
        for (UIBase* UI : RegisteredUIs) {
            if (UI) {
                UI->InvalidateWidget();
            }
        }
        bIsInputLocked = false;
        UpdateTickState();
    }

}