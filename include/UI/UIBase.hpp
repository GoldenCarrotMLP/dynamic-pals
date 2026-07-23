#pragma once
#include <Unreal/UObjectGlobals.hpp>

namespace DynPals {

    class UIBase {
    public:
        UIBase();
        virtual ~UIBase();
        

        // Toggles open/close state
        void RequestToggle();
        
        // Silently refreshes the UI without closing it
        void RequestRebuild();

        void InvalidateWidget() { 
            MyWidget = nullptr; 
            bIsOpen = false; 
            OnInvalidate(); 
        }

        
        bool IsOpen() const { return bIsOpen; }
        bool IsToggleRequested() const { return bToggleRequested; }
        bool RequiresInputLock() const { return bRequiresInputLock; }
        RC::Unreal::UObject* GetWidget() const { return MyWidget; }
        virtual RC::Unreal::UObject* GetDesiredFocusTarget() const { return MyWidget; }

        void ProcessTick(RC::Unreal::UObject* PlayerController);

    protected:
        // Lifecycle Hooks
        virtual bool OnSetup() { return true; } // Return false to abort opening
        virtual void OnOpen() {}                // Called when widget is un-collapsed or built
        virtual void OnClose() {}
        virtual void OnInvalidate() {} 
        
        virtual void BuildWidget() = 0;
        virtual void OnTickUI() {}

        void DestroyWidget();

        RC::Unreal::UObject* MyWidget = nullptr;
        RC::Unreal::UObject* CurrentPlayerController = nullptr;
        
        bool bIsOpen = false;
        bool bRequiresInputLock = true;
        bool bCloseOnEscape = true;

    private:
        bool bToggleRequested = false;
        bool bRebuildRequested = false;
        bool bWasEscapeDown = false;
    };
}