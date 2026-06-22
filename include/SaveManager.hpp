#pragma once
#include <string>
#include <map>
#include <Unreal/UObjectGlobals.hpp>
#include "DataTypes.hpp"

namespace DynPals {
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

    private:
        SaveManager() = default;
        SaveManager(const SaveManager&) = delete;
        SaveManager& operator=(const SaveManager&) = delete;

        std::wstring ConfigPath;
        std::wstring PersistFileName = L"_Altermatic_Persist_";
        std::wstring CurrentWorldSaveID = L"";
        std::map<std::wstring, PalPersistData> PersistedSwaps;
    };
}