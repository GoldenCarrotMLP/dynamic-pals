#define NOMINMAX 
#include <Windows.h>

#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>

#include "Updater.hpp" // <-- Add this include at the top
#include <thread>      // <-- Add this include

#include "ConfigManager.hpp"
#include "SaveManager.hpp"
#include "HooksManager.hpp"
#include "UIManager.hpp"
#include "Utils.hpp"

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
        // Process Key Input safely on the worker thread
        static bool bMenuKeyPressed = false;
        if ((GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState(0x4E) & 0x8000)) {
            if (!bMenuKeyPressed) {
                bMenuKeyPressed = true;
                DynPals::UIManager::Get().RequestMenuToggle();
            }
        } else {
            bMenuKeyPressed = false;
        }
    }

    auto on_unreal_init() -> void override
    {
        UObject* KismetLib = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
        if (KismetLib) {
            FString ContentDir;
            DynPals::Utils::CallFunction(KismetLib, STR("GetProjectContentDirectory"), &ContentDir);
            std::wstring BasePath = DynPals::Utils::FStringToWString(ContentDir);

            DynPals::SaveManager::Get().Initialize(BasePath);
            DynPals::ConfigManager::Get().Initialize(BasePath);
            DynPals::HooksManager::RegisterHooks();

            // Run Updater asynchronously
            std::thread([]() {
                DynPals::Updater::CheckForUpdates();
            }).detach();


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