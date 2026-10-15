#pragma once
#include <string>
#include <map>
#include <list>
#include <Unreal/UObjectGlobals.hpp>
#include "DataTypes.hpp"

namespace DynPals {

    struct DynPalsSettings {
    bool bFocusPal = true;
    double CameraRotation = 180.0;
    bool bRelativeCamera = true;
    
    std::wstring MenuKey = L"N";
    std::wstring MenuModifier = L"LeftAlt"; 
    std::wstring TestMenuKey = L"G";
    std::wstring TestMenuModifier = L"LeftAlt";

    // --- ADD REROLL KEYBIND (Unset by default) ---
    std::wstring RerollKey = L"None";
    std::wstring RerollModifier = L"None";

    // Pre-cached hotkey data
    int MenuKeyVK = 'N';
    int MenuModVK = 0x12; // VK_MENU
    bool bMenuIsGamepad = false;

    int TestKeyVK = 'G';
    int TestModVK = 0x12;
    bool bTestIsGamepad = false;

    // --- ADD PRE-CACHED VKs ---
    int RerollKeyVK = 0;
    int RerollModVK = 0;
    bool bRerollIsGamepad = false;

    void CacheKeybinds();
};

    class SaveManager {
    public:
        static SaveManager& Get() {
            static SaveManager instance;
            return instance;
        }

        void Initialize(const std::wstring& BasePath);
        void LoadWorldData(RC::Unreal::UObject* World);
        void SaveWorldData();

        PalPersistData* GetPersistData(const std::wstring& InstanceID);
        void SetPersistData(const std::wstring& InstanceID, const PalPersistData& Data, bool bWriteToDisk = false);

        void Reset();

        DynPalsSettings Settings;

    private:
        SaveManager() = default;
        SaveManager(const SaveManager&) = delete;
        SaveManager& operator=(const SaveManager&) = delete;

        void MarkAccessed(const std::wstring& InstanceID);

        std::wstring ConfigPath;
        std::wstring PersistFileName = L"_DynPals_Save_";
        std::wstring CurrentWorldSaveID = L"";
        
        std::map<std::wstring, PalPersistData> PersistedSwaps;
        std::list<std::wstring> AccessOrder;
        const size_t MaxSaveEntries = 1000;
    };
}