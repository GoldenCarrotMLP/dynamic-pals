#define NOMINMAX 
#include <Windows.h>

#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>

#include "ConfigManager.hpp"
#include "SaveManager.hpp"
#include "HooksManager.hpp"
#include "Utils.hpp"
#include "AsyncHelper.hpp"
#include "VFXManager.hpp"
#include "FileWatcher.hpp"

#include "UI/Views/UIManager.hpp" 
#include "UI/Views/TestUI.hpp"    

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

        // 1. Check hotkey triggers
        if (GetAsyncKeyState(VK_MENU) & 0x8000) {
            if (GetAsyncKeyState(VK_LEFT) & 0x8000) { 
                if (!bPrevKeyPressed) {
                    bPrevKeyPressed = true;
                    DynPals::VFXManager::Get().CyclePrevious();
                }
            } else { bPrevKeyPressed = false; }

            if (GetAsyncKeyState(VK_RIGHT) & 0x8000) { 
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
                DynPals::AsyncHelper::AsyncTask(DynPals::ENamedThreads::GameThread, [](){
                    DynPals::UIManager::Get().RequestToggle(); 
                });
            }
        } else {
            bMenuKeyPressed = false;
        }

        static bool bTestMenuKeyPressed = false;
        if ((GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState(0x47) & 0x8000)) { 
            if (!bTestMenuKeyPressed) {
                bTestMenuKeyPressed = true;
                DynPals::AsyncHelper::AsyncTask(DynPals::ENamedThreads::GameThread, [](){
                    DynPals::TestUI::Get().RequestToggle();
                });
            }
        } else {
            bTestMenuKeyPressed = false;
        }
    }
    
    auto on_unreal_init() -> void override
    {
        DynPals::AsyncHelper::Initialize();

        UObject* KismetLib = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
        if (KismetLib) {
            FString ContentDir;
            DynPals::Utils::CallFunction(KismetLib, STR("GetProjectContentDirectory"), &ContentDir);
            std::wstring BasePath = DynPals::Utils::FStringToWString(ContentDir);

            DynPals::SaveManager::Get().Initialize(BasePath);
            DynPals::ConfigManager::Get().Initialize(BasePath);
            DynPals::FileWatcher::Start(BasePath + L"Paks/~mods/");
            DynPals::VFXManager::Get().Initialize(); 
            DynPals::HooksManager::RegisterHooks();

            // Register singletons natively into UIRegistry immediately
            DynPals::TestUI::Get();
            DynPals::UIManager::Get();
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