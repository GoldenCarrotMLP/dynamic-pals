#include "SaveManager.hpp"
#include "ConfigManager.hpp"
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
            if (data.contains("PalSwap") && data.at("PalSwap").is_object()) {
                
                // 1:1 ALTERMATIC TRANSLATION (String Unpacking)
                for (auto& [instanceIdStr, valueStrNode] : data.at("PalSwap").items()) {
                    std::wstring instanceId = Utils::StringToWString(instanceIdStr);
                    std::wstring valueStr = Utils::StringToWString(valueStrNode.get<std::string>());
                    
                    PalPersistData pd;
                    pd.InstanceID = instanceId;

                    // Parse "0/<SwapIndex>"
                    size_t p0 = valueStr.find(L"0/");
                    if (p0 != std::wstring::npos) {
                        size_t pNext = valueStr.find(L'/', p0 + 2);
                        std::wstring sIdxStr = (pNext == std::wstring::npos) ? valueStr.substr(p0 + 2) : valueStr.substr(p0 + 2, pNext - (p0 + 2));
                        
                        try { pd.SwapIndex = std::stoi(sIdxStr); } catch (...) { pd.SwapIndex = -1; }

                        // Parse "/1/<MorphIndex>_<MorphVal>:"
                        size_t p1 = valueStr.find(L"/1/");
                        if (p1 != std::wstring::npos) {
                            std::wstring mBlock = valueStr.substr(p1 + 3);
                            size_t p2 = mBlock.find(L'/'); // Cull trailing legacy blocks if they exist
                            if (p2 != std::wstring::npos) mBlock = mBlock.substr(0, p2);

                            size_t pos = 0;
                            while (pos < mBlock.size()) {
                                size_t colon = mBlock.find(L':', pos);
                                if (colon == std::wstring::npos) break;

                                std::wstring pair = mBlock.substr(pos, colon - pos);
                                size_t uscore = pair.find(L'_');
                                if (uscore != std::wstring::npos) {
                                    try {
                                        int mIdx = std::stoi(pair.substr(0, uscore));
                                        double mVal = std::stod(pair.substr(uscore + 1));

                                        // Map the integer MorphIndex back to the actual Morph Name from the Config
                                        if (pd.SwapIndex >= 0 && pd.SwapIndex < (int)ConfigManager::Get().GetConfigs().size()) {
                                            auto& swapCfg = ConfigManager::Get().GetConfigs()[pd.SwapIndex];
                                            if (mIdx >= 0 && mIdx < (int)swapCfg.MorphTargetList.size()) {
                                                pd.MorphSet[swapCfg.MorphTargetList[mIdx].target] = mVal;
                                            }
                                        }
                                    } catch (...) {} // Ignore malformed morph segments
                                }
                                pos = colon + 1;
                            }
                        }
                        PersistedSwaps[pd.InstanceID] = pd;
                    }
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
        nlohmann::json palSwaps = nlohmann::json::object();
        
        // 1:1 ALTERMATIC TRANSLATION (String Packing)
        for (auto& [id, data] : PersistedSwaps) {
            std::wstring entryStr = L"0/" + std::to_wstring(data.SwapIndex);
            
            if (!data.MorphSet.empty() && data.SwapIndex >= 0 && data.SwapIndex < (int)ConfigManager::Get().GetConfigs().size()) {
                entryStr += L"/1/";
                auto& swapCfg = ConfigManager::Get().GetConfigs()[data.SwapIndex];
                
                for (auto& [morphName, morphVal] : data.MorphSet) {
                    int mIdx = -1;
                    // Translate Morph Name back into the integer index Altermatic expects
                    for (int i = 0; i < (int)swapCfg.MorphTargetList.size(); ++i) {
                        if (swapCfg.MorphTargetList[i].target == morphName) { mIdx = i; break; }
                    }

                    if (mIdx != -1) {
                        // Format the double cleanly (e.g. "0.50" instead of "0.500000") to mimic UE Blueprint behavior
                        std::wstring valStr = std::to_wstring(morphVal);
                        valStr.erase(valStr.find_last_not_of(L'0') + 1, std::wstring::npos);
                        if (valStr.back() == L'.') valStr += L"0"; 
                        
                        entryStr += std::to_wstring(mIdx) + L"_" + valStr + L":";
                    }
                }
            }
            
            palSwaps[Utils::WStringToString(data.InstanceID)] = Utils::WStringToString(entryStr);
        }
        
        out["PalSwap"] = palSwaps;

        // Preserve Altermatic's "System" config block so the original mod doesn't throw a fit if it loads this file
        nlohmann::json systemObj = nlohmann::json::object();
        systemObj["InvalidIDAttempts"] = "4";
        systemObj["FallbackFrequency"] = "1.000000";
        out["System"] = systemObj;

        std::ofstream file(persistPath);
        if (file.is_open()) {
            file << out.dump(4);
            Output::send<LogLevel::Normal>(STR("[DynPals] World persistence synchronized (Altermatic 1:1 Parity).\n"));
        }
    }

    void SaveManager::TickSave() {
        if (bSaveRequired) {
            auto now = std::chrono::steady_clock::now();
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