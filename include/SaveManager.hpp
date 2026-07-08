// --- START OF FILE include/SaveManager.hpp ---
#pragma once
#include <string>
#include <map>
#include <list> // Required for LRU Queue
#include <Unreal/UObjectGlobals.hpp>
#include "DataTypes.hpp"

namespace DynPals {

    struct DynPalsSettings {
        bool bFocusPal = true;
        double CameraRotation = 180.0;
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

        // Clears cached save IDs and states on world transition
        void Reset();

        DynPalsSettings Settings;

    private:
        SaveManager() = default;
        SaveManager(const SaveManager&) = delete;
        SaveManager& operator=(const SaveManager&) = delete;

        // Bumps accessed Pals to the front of our LRU queue
        void MarkAccessed(const std::wstring& InstanceID);

        std::wstring ConfigPath;
        std::wstring PersistFileName = L"_DynPals_Save_";
        std::wstring CurrentWorldSaveID = L"";
        
        std::map<std::wstring, PalPersistData> PersistedSwaps;
        std::list<std::wstring> AccessOrder; // Tracks recency (front = newest, back = oldest)
        const size_t MaxSaveEntries = 1000;  // Hard-capped limit
    };
}