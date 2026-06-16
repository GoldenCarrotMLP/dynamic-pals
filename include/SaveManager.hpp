#pragma once
#include <string>
#include <map>
#include <chrono>
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
        void TickSave();

        PalPersistData* GetPersistData(const std::wstring& InstanceID);
        void SetPersistData(const std::wstring& InstanceID, const PalPersistData& Data);

    private:
        SaveManager() = default;
        SaveManager(const SaveManager&) = delete;
        SaveManager& operator=(const SaveManager&) = delete;

        std::wstring ConfigPath;
        std::wstring PersistFileName = L"_Altermatic_Persist_";
        std::wstring CurrentWorldSaveID = L"";
        std::map<std::wstring, PalPersistData> PersistedSwaps;

        bool bSaveRequired = false;
        std::chrono::steady_clock::time_point LastSaveTime = std::chrono::steady_clock::now();
    };
}