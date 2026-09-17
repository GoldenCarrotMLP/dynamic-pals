#include "SaveManager.hpp"
#include "ConfigManager.hpp"
#include "PalProcessor.hpp"
#include "InputManager.hpp"
#include "Utils.hpp"
#include <DynamicOutput/DynamicOutput.hpp>
#include <fstream>
#include "json.hpp"

#include <Unreal/UObject.hpp>
#include <Unreal/FString.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace DynPals {

    void DynPalsSettings::CacheKeybinds() {
        bMenuIsGamepad = (MenuKey.rfind(L"Gamepad_", 0) == 0);
        MenuKeyVK = InputManager::Get().KeyNameToVK(MenuKey);
        MenuModVK = InputManager::Get().KeyNameToVK(MenuModifier);

        bTestIsGamepad = (TestMenuKey.rfind(L"Gamepad_", 0) == 0);
        TestKeyVK = InputManager::Get().KeyNameToVK(TestMenuKey);
        TestModVK = InputManager::Get().KeyNameToVK(TestMenuModifier);
    }

    void SaveManager::Initialize(const std::wstring& BasePath) {
        ConfigPath = BasePath + L"Paks/~mods/"; 
        Settings.CacheKeybinds();
    }

    void SaveManager::Reset() {
        CurrentWorldSaveID = L"";
        PersistedSwaps.clear();
        AccessOrder.clear(); 
        Settings = DynPalsSettings{};
        Settings.CacheKeybinds();
    }

    void SaveManager::MarkAccessed(const std::wstring& InstanceID) {
        AccessOrder.remove(InstanceID);
        AccessOrder.push_front(InstanceID);
        
        while (AccessOrder.size() > MaxSaveEntries) {
            std::wstring oldestID = AccessOrder.back();
            AccessOrder.pop_back();
            PersistedSwaps.erase(oldestID);
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

        if (content.empty()) {
            Settings.CacheKeybinds();
            return;
        }

        try {
            auto data = nlohmann::ordered_json::parse(content);
            
            if (data.contains("Settings") && data.at("Settings").is_object()) {
                Settings.bFocusPal = data.at("Settings").value("FocusPal", true);
                Settings.CameraRotation = data.at("Settings").value("CameraRotation", 180.0);
                Settings.bRelativeCamera = data.at("Settings").value("RelativeCamera", true);
                
                Settings.MenuKey = Utils::StringToWString(data.at("Settings").value("MenuKey", "N"));
                Settings.MenuModifier = Utils::StringToWString(data.at("Settings").value("MenuModifier", "LeftAlt"));
                Settings.TestMenuKey = Utils::StringToWString(data.at("Settings").value("TestMenuKey", "G"));
                Settings.TestMenuModifier = Utils::StringToWString(data.at("Settings").value("TestMenuModifier", "LeftAlt"));
            } else {
                Settings.bFocusPal = true;
                Settings.CameraRotation = 180.0;
                Settings.bRelativeCamera = true;
                Settings.MenuKey = L"N";
                Settings.MenuModifier = L"LeftAlt";
                Settings.TestMenuKey = L"G";
                Settings.TestMenuModifier = L"LeftAlt";
            }
            Settings.CacheKeybinds();

            if (data.contains("PersistencePals") && data.at("PersistencePals").is_object()) {
                for (auto& [instanceIdStr, palNode] : data.at("PersistencePals").items()) {
                    PalPersistData pd;
                    pd.InstanceID = Utils::StringToWString(instanceIdStr);
                    pd.PackName = Utils::StringToWString(palNode.value("PackName", ""));
                    pd.SkinName = Utils::StringToWString(palNode.value("SkinName", ""));
                    pd.SwapLabel = Utils::StringToWString(palNode.value("SwapLabel", palNode.value("SkinLabel", ""))); 
                    pd.SkelMeshPath = Utils::StringToWString(palNode.value("SkelMeshPath", ""));
                    pd.bIsManuallyLocked = palNode.value("IsLocked", false);
                    pd.SizeMultiplier = palNode.value("SizeMultiplier", -1.0);

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
            Settings.CacheKeybinds();
        }
    }

    void SaveManager::SaveWorldData() {
        if (CurrentWorldSaveID.empty()) return;
        
        std::wstring persistPath = ConfigPath + PersistFileName + CurrentWorldSaveID + L".json";
        
        // Fast snapshot on Game Thread (<0.05ms)
        auto palsCopy = PersistedSwaps;
        auto accessOrderCopy = AccessOrder;
        auto settingsCopy = Settings;
        std::wstring worldIdCopy = CurrentWorldSaveID;

        // Offload JSON stringification and disk I/O to background thread
        std::thread([persistPath, palsCopy = std::move(palsCopy), accessOrderCopy = std::move(accessOrderCopy), 
                     settingsCopy = std::move(settingsCopy), worldIdCopy = std::move(worldIdCopy)]() {
            static std::mutex fileWriteMutex;
            std::lock_guard<std::mutex> lock(fileWriteMutex);

            nlohmann::ordered_json out;
            
            nlohmann::ordered_json systemObj;
            systemObj["ModVersion"] = "1.1.0";
            systemObj["WorldID"] = Utils::WStringToString(worldIdCopy);
            out["System"] = systemObj;
            
            nlohmann::ordered_json settingsObj;
            settingsObj["FocusPal"] = settingsCopy.bFocusPal;
            settingsObj["CameraRotation"] = settingsCopy.CameraRotation;
            settingsObj["RelativeCamera"] = settingsCopy.bRelativeCamera;
            
            settingsObj["MenuKey"] = Utils::WStringToString(settingsCopy.MenuKey);
            settingsObj["MenuModifier"] = Utils::WStringToString(settingsCopy.MenuModifier);
            settingsObj["TestMenuKey"] = Utils::WStringToString(settingsCopy.TestMenuKey);
            settingsObj["TestMenuModifier"] = Utils::WStringToString(settingsCopy.TestMenuModifier);

            out["Settings"] = settingsObj;
            
            nlohmann::ordered_json palsObj;
            for (const auto& id : accessOrderCopy) {
                auto it = palsCopy.find(id);
                if (it != palsCopy.end()) {
                    auto& data = it->second;
                    if (!data.ShouldSave()) continue;

                    nlohmann::ordered_json palNode;
                    palNode["PackName"] = Utils::WStringToString(data.PackName);
                    palNode["SkinName"] = Utils::WStringToString(data.SkinName);
                    palNode["SwapLabel"] = Utils::WStringToString(data.SwapLabel); 
                    palNode["SkelMeshPath"] = Utils::WStringToString(data.SkelMeshPath);
                    palNode["IsLocked"] = data.bIsManuallyLocked; 
                    if (data.SizeMultiplier > 0.0) palNode["SizeMultiplier"] = data.SizeMultiplier;
                    
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
                RC::Output::send<RC::LogLevel::Default>(STR("[DynPals] Saved world persistence data cleanly to disk (async).\n"));
            }
        }).detach();
    }

    PalPersistData* SaveManager::GetPersistData(const std::wstring& InstanceID) {
        auto it = PersistedSwaps.find(InstanceID);
        if (it != PersistedSwaps.end()) {
            MarkAccessed(InstanceID);
            return &it->second;
        }
        return nullptr;
    }

    void SaveManager::SetPersistData(const std::wstring& InstanceID, const PalPersistData& Data, bool bWriteToDisk) {
        PersistedSwaps[InstanceID] = Data;
        MarkAccessed(InstanceID);
        
        if (bWriteToDisk) {
            SaveWorldData(); 
        }
    }
}