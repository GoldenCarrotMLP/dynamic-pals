#include "SaveManager.hpp"
#include "Utils.hpp"
#include <DynamicOutput/DynamicOutput.hpp>
#include <fstream>
#include "json.hpp"

#include <Unreal/UObject.hpp>
#include <Unreal/FString.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace DynPals {

    void SaveManager::Initialize(const std::wstring& BasePath) {
        ConfigPath = BasePath + L"Paks/~mods/SwapJSON/";
    }

    void SaveManager::LoadWorldData(UObject* World) {
         if (!World) return;
        UObject* GI = nullptr;
        Utils::GetPropertyValue<UObject*>(World, STR("OwningGameInstance"), GI);
        if (!GI) return;

        FString SaveDir;
        Utils::GetPropertyValue<FString>(GI, STR("SelectedWorldSaveDirectoryName"), SaveDir);
        std::wstring WorldSaveID = Utils::FStringToWString(SaveDir);

        if (WorldSaveID.empty() || WorldSaveID == CurrentWorldSaveID) return;

        CurrentWorldSaveID = WorldSaveID;
        PersistedSwaps.clear();

        std::wstring persistPath = ConfigPath + PersistFileName + CurrentWorldSaveID + L".json";
        std::string content = Utils::ReadFileToString(persistPath);
        if (content.empty()) return;

        try {
            nlohmann::json data = nlohmann::json::parse(content);
            if (data.contains("PalSwap")) {
                for (auto& item : data.at("PalSwap")) {
                    PalPersistData pd;
                    pd.InstanceID = Utils::StringToWString(item.at("InstanceID").get<std::string>());
                    pd.SwapIndex = item.at("SwapIndex").get<int>() - 1;
                    PersistedSwaps[pd.InstanceID] = pd;
                }
            }
        } catch (...) {
            Output::send<LogLevel::Error>(STR("[DynPals] Failed to parse world persistence.\n"));
        }
    }

    void SaveManager::SaveWorldData() {
        if (CurrentWorldSaveID.empty()) return;
        
        std::wstring persistPath = ConfigPath + PersistFileName + CurrentWorldSaveID + L".json";
        nlohmann::json out = nlohmann::json::object();
        nlohmann::json palSwaps = nlohmann::json::array();
        
        for (auto& [id, data] : PersistedSwaps) {
            nlohmann::json item = nlohmann::json::object();
            item["InstanceID"] = Utils::WStringToString(data.InstanceID);
            item["SwapIndex"] = data.SwapIndex + 1; 
            palSwaps.push_back(item);
        }
        
        out["PalSwap"] = palSwaps;

        std::ofstream file(persistPath);
        if (file.is_open()) {
            file << out.dump(4);
            Output::send<LogLevel::Normal>(STR("[DynPals] World persistence synchronized to disk.\n"));
        }
    }

    void SaveManager::TickSave() {
        if (bSaveRequired) {
            auto now = std::chrono::steady_clock::now();
            // Throttles file saving to occur at most once every 10 seconds only when changes were made
            if (now - LastSaveTime > std::chrono::seconds(10)) {
                SaveWorldData();
                bSaveRequired = false;
                LastSaveTime = now;
            }
        }
    }

    PalPersistData* SaveManager::GetPersistData(const std::wstring& InstanceID) {
        auto it = PersistedSwaps.find(InstanceID);
        return it != PersistedSwaps.end() ? &it->second : nullptr;
    }

    void SaveManager::SetPersistData(const std::wstring& InstanceID, const PalPersistData& Data) {
        PersistedSwaps[InstanceID] = Data;
        bSaveRequired = true;
    }
}