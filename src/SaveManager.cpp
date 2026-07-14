#include "SaveManager.hpp"
#include "ConfigManager.hpp"
#include "PalProcessor.hpp"
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
        ConfigPath = BasePath + L"Paks/~mods/"; 
    }

    void SaveManager::Reset() {
        CurrentWorldSaveID = L"";
        PersistedSwaps.clear();
        AccessOrder.clear(); 
        Settings = DynPalsSettings{};
    }


    void SaveManager::MarkAccessed(const std::wstring& InstanceID) {
        // 1. Remove from its current position in the queue (if it exists)
        AccessOrder.remove(InstanceID);
        
        // 2. Push to the front (making it the most recently used) [1]
        AccessOrder.push_front(InstanceID);
        
        // 3. If we exceed the 1000-entry limit, evict the oldest forgotten ones! [1, 4]
        while (AccessOrder.size() > MaxSaveEntries) {
            std::wstring oldestID = AccessOrder.back();
            AccessOrder.pop_back(); // Remove from queue [1]
            PersistedSwaps.erase(oldestID); // Erase from database memory [4]
        }
    }

    void SaveManager::LoadWorldData(UObject* World) {
        if (!World) return;

        UObject* GI = nullptr;
        Utils::GetPropertyValue<UObject*>(World, STR("OwningGameInstance"), GI, true);
        if (!GI) return;

        FString SaveDir;
        Utils::GetPropertyValue<FString>(GI, STR("SelectedWorldSaveDirectoryName"), SaveDir, true);
        std::wstring WorldSaveID = Utils::FStringToWString(SaveDir);

        if (WorldSaveID.empty()) {
            WorldSaveID = L"Multiplayer_Shared";
        }

        if (WorldSaveID == CurrentWorldSaveID) return;

        CurrentWorldSaveID = WorldSaveID;
        PersistedSwaps.clear();
        AccessOrder.clear(); 
        Settings = DynPalsSettings{};

        std::wstring persistPath = ConfigPath + PersistFileName + CurrentWorldSaveID + L".json";
        std::string content = Utils::ReadFileToString(persistPath);

        if (content.empty()) return;

        try {
            auto data = nlohmann::ordered_json::parse(content);
            
            // --- NEW: Parse Config Settings ---
            if (data.contains("Settings") && data.at("Settings").is_object()) {
                Settings.bFocusPal = data.at("Settings").value("FocusPal", true);
                Settings.CameraRotation = data.at("Settings").value("CameraRotation", 180.0);
            } else {
                Settings.bFocusPal = true;
                Settings.CameraRotation = 180.0;
            }

            if (data.contains("PersistencePals") && data.at("PersistencePals").is_object()) {
                for (auto& [instanceIdStr, palNode] : data.at("PersistencePals").items()) {
                    PalPersistData pd;
                    pd.InstanceID = Utils::StringToWString(instanceIdStr);
                    pd.PackName = Utils::StringToWString(palNode.value("PackName", ""));
                    pd.SkinName = Utils::StringToWString(palNode.value("SkinName", ""));
                    pd.SwapLabel = Utils::StringToWString(palNode.value("SwapLabel", palNode.value("SkinLabel", ""))); 
                    pd.SkelMeshPath = Utils::StringToWString(palNode.value("SkelMeshPath", ""));
                    pd.bIsManuallyLocked = palNode.value("IsLocked", false);

                    if (palNode.contains("Morphs") && palNode.at("Morphs").is_object()) {
                        for (auto& [morphName, morphVal] : palNode.at("Morphs").items()) {
                            pd.MorphSet[Utils::StringToWString(morphName)] = morphVal.get<double>();
                        }
                    }
                    if (palNode.contains("Mats") && palNode.at("Mats").is_object()) {
                        for (auto& [matIndex, matPath] : palNode.at("Mats").items()) {
                            pd.MatSet[matIndex] = Utils::StringToWString(matPath.get<std::string>());
                        }
                    }
                    if (palNode.contains("MatColors") && palNode.at("MatColors").is_object()) {
                        for (auto& [matIndex, colorArr] : palNode.at("MatColors").items()) {
                            if (colorArr.is_array() && colorArr.size() == 4) {
                                pd.MatColorSet[matIndex] = { colorArr[0].get<float>(), colorArr[1].get<float>(), colorArr[2].get<float>(), colorArr[3].get<float>() };
                            }
                        }
                    }
                    PersistedSwaps[pd.InstanceID] = pd;
                    AccessOrder.push_back(pd.InstanceID); 
                }
            }
        } catch (...) {
            DP_LOG(Error, "Failed to parse world persistence data. File might be corrupted.\n");
        }
    }


    void SaveManager::SaveWorldData() {
        if (CurrentWorldSaveID.empty()) return;
        
        std::wstring persistPath = ConfigPath + PersistFileName + CurrentWorldSaveID + L".json";
        
        nlohmann::ordered_json out;
        
        nlohmann::ordered_json systemObj;
        systemObj["ModVersion"] = "1.1.0";
        systemObj["WorldID"] = Utils::WStringToString(CurrentWorldSaveID);
        out["System"] = systemObj;
        
        // --- NEW: Write Config Settings ---
        nlohmann::ordered_json settingsObj;
        settingsObj["FocusPal"] = Settings.bFocusPal;
        settingsObj["CameraRotation"] = Settings.CameraRotation;
        out["Settings"] = settingsObj;
        
        nlohmann::ordered_json palsObj;
        
        for (const auto& id : AccessOrder) {
            auto it = PersistedSwaps.find(id);
            if (it != PersistedSwaps.end()) {
                auto& data = it->second;
                if (!data.HasSavedSwap()) continue;

                nlohmann::ordered_json palNode;
                palNode["PackName"] = Utils::WStringToString(data.PackName);
                palNode["SkinName"] = Utils::WStringToString(data.SkinName);
                palNode["SwapLabel"] = Utils::WStringToString(data.SwapLabel); 
                palNode["SkelMeshPath"] = Utils::WStringToString(data.SkelMeshPath);
                palNode["IsLocked"] = data.bIsManuallyLocked; 
                
                nlohmann::ordered_json morphsObj;
                for (const auto& [mName, mVal] : data.MorphSet) {
                    morphsObj[Utils::WStringToString(mName)] = mVal;
                }
                palNode["Morphs"] = morphsObj;
                
                nlohmann::ordered_json matsObj;
                for (const auto& [mIndex, mPath] : data.MatSet) {
                    matsObj[mIndex] = Utils::WStringToString(mPath);
                }
                if (!matsObj.empty()) palNode["Mats"] = matsObj;
                
                nlohmann::ordered_json matColorsObj;
                for (const auto& [mIndex, mColor] : data.MatColorSet) {
                    matColorsObj[mIndex] = { mColor.R, mColor.G, mColor.B, mColor.A };
                }
                if (!matColorsObj.empty()) palNode["MatColors"] = matColorsObj;
                
                palsObj[Utils::WStringToString(id)] = palNode;

            }
        }
        
        out["PersistencePals"] = palsObj;

        std::ofstream file(persistPath);
        if (file.is_open()) {
            file << out.dump(4);
            DP_LOG(Default, "Saved world persistence data cleanly to disk.\n");
        }
    }

    
   
    PalPersistData* SaveManager::GetPersistData(const std::wstring& InstanceID) {
        auto it = PersistedSwaps.find(InstanceID);
        if (it != PersistedSwaps.end()) {
            MarkAccessed(InstanceID); // Bump accessed Pal to the front of our LRU queue! [1, 3]
            return &it->second;
        }
        return nullptr;
    }

    void SaveManager::SetPersistData(const std::wstring& InstanceID, const PalPersistData& Data, bool bWriteToDisk) {
        PersistedSwaps[InstanceID] = Data;
        MarkAccessed(InstanceID); // Bump updated Pal and evict the oldest if size > 1000 [1, 3]
        
        if (bWriteToDisk) {
            SaveWorldData(); 
        }
    }
}