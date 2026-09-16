#pragma once
#include <string>
#include <vector>
#include <functional>
#include <Unreal/UObjectGlobals.hpp>

namespace DynPals {

    class InputManager {
    public:
        static InputManager& Get() {
            static InputManager instance;
            return instance;
        }

        // Key identification helpers
        int KeyNameToVK(const std::wstring& KeyName) const;
        bool IsHotkeyDown(RC::Unreal::UObject* PlayerController, const std::wstring& Modifier, const std::wstring& Key) const;

        // Key capture workflow
        using CaptureCallback = std::function<void(const std::wstring& Key, const std::wstring& Modifier)>;
        using CancelCallback  = std::function<void()>;

        void StartCapture(CaptureCallback OnCaptured, CancelCallback OnCancelled = nullptr);
        void CancelCapture();
        bool IsCapturing() const { return bIsCapturing; }

        // Ticked to poll active input
        void Tick(RC::Unreal::UObject* PlayerController);

    private:
        InputManager() = default;
        InputManager(const InputManager&) = delete;
        InputManager& operator=(const InputManager&) = delete;

        bool PollGamepad(std::wstring& OutKey) const;

        bool bIsCapturing = false;
        int CaptureDebounceFrames = 0;
        CaptureCallback OnKeyCaptured;
        CancelCallback  OnKeyCancelled;
    };
}