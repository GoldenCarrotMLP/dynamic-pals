#pragma once
#include <Unreal/UObjectGlobals.hpp>
#include <string>

namespace DynPals {
    class UIManager {
    public:
        static UIManager& Get() {
            static UIManager instance;
            return instance;
        }

        void ToggleMenu();
        void DrawUI();

    private:
        UIManager() = default;
        UIManager(const UIManager&) = delete;
        UIManager& operator=(const UIManager&) = delete;

        void UpdateTarget();
        void LockInput(bool bLock);

        bool bIsMenuOpen = false;
        RC::Unreal::UObject* TargetPal = nullptr;
        std::wstring TargetInstanceID = L"";
        std::wstring TargetCharID = L"";
    };
}