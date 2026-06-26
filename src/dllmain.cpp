#define NOMINMAX 
#include <Windows.h>

#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>

#include "ConfigManager.hpp"
#include "SaveManager.hpp"
#include "HooksManager.hpp"
#include "UIManager.hpp"
#include "Utils.hpp"
#include "AsyncHelper.hpp"
#include "VFXManager.hpp"

using namespace RC;
using namespace RC::Unreal;

class DynPalsMod : public CppUserModBase
{
public:
    DynPalsMod() : CppUserModBase()
    {
        ModName = STR("DynamicPals");
        ModVersion = STR("1.0.0");
        ModDescription = STR("Native C++ run-time mesh and material swapper.");
        ModAuthors = STR("Modder");
    }

    ~DynPalsMod() override {}

    auto on_update() -> void override
    {
        static bool bPrevKeyPressed = false;
        static bool bNextKeyPressed = false;

        if (GetAsyncKeyState(VK_MENU) & 0x8000) {
            if (GetAsyncKeyState(VK_LEFT) & 0x8000) { // Alt + Left Arrow [1]
                if (!bPrevKeyPressed) {
                    bPrevKeyPressed = true;
                    DynPals::VFXManager::Get().CyclePrevious();
                }
            } else { bPrevKeyPressed = false; }

            if (GetAsyncKeyState(VK_RIGHT) & 0x8000) { // Alt + Right Arrow [1]
                if (!bNextKeyPressed) {
                    bNextKeyPressed = true;
                    DynPals::VFXManager::Get().CycleNext();
                }
            } else { bNextKeyPressed = false; }
        } else {
            bPrevKeyPressed = false;
            bNextKeyPressed = false;
        }

        static bool bMenuKeyPressed = false;
        if ((GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState(0x4E) & 0x8000)) {
            if (!bMenuKeyPressed) {
                bMenuKeyPressed = true;
                
                // Instantly execute ToggleMenu on the Game Thread
                DynPals::AsyncHelper::AsyncTask(DynPals::ENamedThreads::GameThread, [](){
                    DynPals::UIManager::Get().ToggleMenu();
                });
            }
        } else {
            bMenuKeyPressed = false;
        }
    }
    
    auto on_unreal_init() -> void override
    {
        

        // 1. Initialize the independent scanner
        DynPals::AsyncHelper::Initialize();

        // 2. Initialize the rest of the mod
        UObject* KismetLib = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
        if (KismetLib) {
            FString ContentDir;
            DynPals::Utils::CallFunction(KismetLib, STR("GetProjectContentDirectory"), &ContentDir);
            std::wstring BasePath = DynPals::Utils::FStringToWString(ContentDir);

            DynPals::VFXManager::Get().Initialize();
            DynPals::SaveManager::Get().Initialize(BasePath);
            DynPals::ConfigManager::Get().Initialize(BasePath);
            DynPals::HooksManager::RegisterHooks();
        }
    }
};

#define DYNPALS_MOD_API __declspec(dllexport)
extern "C"
{
    DYNPALS_MOD_API CppUserModBase* start_mod() {
        return new DynPalsMod();
    }

    DYNPALS_MOD_API void uninstall_mod(CppUserModBase* mod) {
        delete mod;
    }
}