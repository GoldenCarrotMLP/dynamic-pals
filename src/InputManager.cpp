#define NOMINMAX
#include <Windows.h>
#include <Xinput.h>
#include "InputManager.hpp"
#include "Utils.hpp"
#include "DataTypes.hpp"

namespace DynPals {

    int InputManager::KeyNameToVK(const std::wstring& KeyName) const {
        if (KeyName.empty()) return 0;
        if (KeyName.length() == 1) {
            wchar_t c = std::towupper(KeyName[0]);
            if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) return static_cast<int>(c);
        }
        if (KeyName == L"LeftAlt" || KeyName == L"RightAlt" || KeyName == L"Alt") return VK_MENU;
        if (KeyName == L"LeftControl" || KeyName == L"RightControl" || KeyName == L"Control") return VK_CONTROL;
        if (KeyName == L"LeftShift" || KeyName == L"RightShift" || KeyName == L"Shift") return VK_SHIFT;
        if (KeyName == L"SpaceBar") return VK_SPACE;
        if (KeyName == L"Enter") return VK_RETURN;
        if (KeyName == L"Tab") return VK_TAB;
        if (KeyName == L"Escape") return VK_ESCAPE;
        if (KeyName.rfind(L"F", 0) == 0 && KeyName.length() <= 3) {
            try {
                int fNum = std::stoi(KeyName.substr(1));
                if (fNum >= 1 && fNum <= 12) return VK_F1 + (fNum - 1);
            } catch (...) {}
        }
        return 0;
    }

    bool InputManager::PollGamepad(std::wstring& OutKey) const {
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

    bool InputManager::IsHotkeyDown(RC::Unreal::UObject* PlayerController, const std::wstring& Modifier, const std::wstring& Key) const {
        if (Key.empty() || Key == L"None") return false;

        bool bIsGamepad = (Key.rfind(L"Gamepad_", 0) == 0);

        if (!bIsGamepad) {
            int vkKey = KeyNameToVK(Key);
            int vkMod = KeyNameToVK(Modifier);

            bool modDown = (vkMod == 0) || ((GetAsyncKeyState(vkMod) & 0x8000) != 0);
            bool keyDown = (vkKey != 0) && ((GetAsyncKeyState(vkKey) & 0x8000) != 0);

            return modDown && keyDown;
        } else {
            std::wstring pressedGamepad;
            if (PollGamepad(pressedGamepad)) {
                return (pressedGamepad == Key);
            }
            if (PlayerController) {
                return Utils::WasKeyJustPressed(PlayerController, Key);
            }
            return false;
        }
    }

    void InputManager::StartCapture(CaptureCallback OnCaptured, CancelCallback OnCancelled) {
        bIsCapturing = true;
        CaptureDebounceFrames = 10;
        OnKeyCaptured = OnCaptured;
        OnKeyCancelled = OnCancelled;
    }

    void InputManager::CancelCapture() {
        bIsCapturing = false;
        if (OnKeyCancelled) OnKeyCancelled();
        OnKeyCaptured = nullptr;
        OnKeyCancelled = nullptr;
        DP_LOG(Default, "[InputManager] Key capture cancelled.");
    }

    void InputManager::Tick(RC::Unreal::UObject* PlayerController) {
        if (!bIsCapturing) return;

        if (CaptureDebounceFrames > 0) {
            CaptureDebounceFrames--;
            return;
        }

        // 1. ESC to Cancel
        if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
            CancelCapture();
            return;
        }

        // 2. Controller / Steam Deck polling
        std::wstring gamepadKey;
        if (PollGamepad(gamepadKey)) {
            if (gamepadKey == L"Gamepad_Special_Left") { // Back button cancels
                CancelCapture();
                return;
            }

            if (OnKeyCaptured) OnKeyCaptured(gamepadKey, L"");
            bIsCapturing = false;
            OnKeyCaptured = nullptr;
            OnKeyCancelled = nullptr;
            return;
        }

        // 3. Keyboard polling
        std::wstring detectedKey = L"";
        for (int vk = 'A'; vk <= 'Z'; ++vk) {
            if ((GetAsyncKeyState(vk) & 0x8000) != 0) { detectedKey = std::wstring(1, static_cast<wchar_t>(vk)); break; }
        }
        if (detectedKey.empty()) {
            for (int vk = '0'; vk <= '9'; ++vk) {
                if ((GetAsyncKeyState(vk) & 0x8000) != 0) { detectedKey = std::wstring(1, static_cast<wchar_t>(vk)); break; }
            }
        }
        if (detectedKey.empty()) {
            for (int vk = VK_F1; vk <= VK_F12; ++vk) {
                if ((GetAsyncKeyState(vk) & 0x8000) != 0) { detectedKey = L"F" + std::to_wstring(vk - VK_F1 + 1); break; }
            }
        }
        if (detectedKey.empty()) {
            if ((GetAsyncKeyState(VK_SPACE) & 0x8000) != 0) detectedKey = L"SpaceBar";
            else if ((GetAsyncKeyState(VK_RETURN) & 0x8000) != 0) detectedKey = L"Enter";
            else if ((GetAsyncKeyState(VK_TAB) & 0x8000) != 0) detectedKey = L"Tab";
        }

        if (!detectedKey.empty()) {
            std::wstring mod = L"";
            if ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) mod = L"LeftAlt";
            else if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) mod = L"LeftControl";
            else if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) mod = L"LeftShift";

            if (OnKeyCaptured) OnKeyCaptured(detectedKey, mod);
            bIsCapturing = false;
            OnKeyCaptured = nullptr;
            OnKeyCancelled = nullptr;
        }
    }
}