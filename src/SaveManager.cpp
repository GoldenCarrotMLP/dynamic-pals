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

    // Helper to strip the Unreal Engine `/Script/Engine.SkeletalMesh'Path'` wrapper
    static std::wstring CleanAltermaticPath(const std::wstring& path) {
        size_t firstQuote = path.find(L'\'');
        if (firstQuote != std::wstring::npos) {
            size_t lastQuote = path.find(L'\'', firstQuote + 1);
            if (lastQuote != std::wstring::npos) {
                return path.substr(firstQuote + 1, lastQuote - firstQuote - 1);
            }
        }
        return path;
    }

    void SaveManager::Initialize(const std::wstring& BasePath) {
        ConfigPath = BasePath + L"Paks/~mods/"; 
    }

    void SaveManager::Reset() {
        CurrentWorldSaveID = L"";
        PersistedSwaps.clear();
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
        PalProcessor::Get().ClearAllSwappedStatus(); // Clear on world load!

        std::wstring persistPath = ConfigPath + PersistFileName + CurrentWorldSaveID + L".json";
        std::string content = Utils::ReadFileToString(persistPath);
        if (content.empty()) return;

        try {
            nlohmann::json data = nlohmann::json::parse(content);
            
            std::map<std::wstring, std::wstring> altrMeshPaths;
            if (data.contains("SkelMeshPath") && data.at("SkelMeshPath").is_object()) {
                for (auto& [idStr, pathNode] : data.at("SkelMeshPath").items()) {
                    std::wstring rawPath = Utils::StringToWString(pathNode.get<std::string>());
                    altrMeshPaths[Utils::StringToWString(idStr)] = CleanAltermaticPath(rawPath);
                }
            }

            if (data.contains("PalSwap") && data.at("PalSwap").is_object()) {
                for (auto& [instanceIdStr, valueStrNode] : data.at("PalSwap").items()) {
                    std::wstring instanceId = Utils::StringToWString(instanceIdStr);
                    std::wstring valueStr = Utils::StringToWString(valueStrNode.get<std::string>());
                    
                    PalPersistData pd;
                    pd.InstanceID = instanceId;

                    size_t p0 = valueStr.find(L"0/");
                    if (p0 != std::wstring::npos) {
                        size_t pNext = valueStr.find(L'/', p0 + 2);
                        std::wstring altrId = (pNext == std::wstring::npos) ? valueStr.substr(p0 + 2) : valueStr.substr(p0 + 2, pNext - (p0 + 2));
                        
                        pd.SwapIndex = -1;
                        auto ItPath = altrMeshPaths.find(altrId);
                        if (ItPath != altrMeshPaths.end()) {
                            std::wstring MatchPath = ItPath->second;
                            auto& configs = ConfigManager::Get().GetConfigs();
                            for (size_t i = 0; i < configs.size(); ++i) {
                                if (Utils::FormatAssetPath(configs[i].SkelMeshPath) == Utils::FormatAssetPath(MatchPath)) {
                                    pd.SwapIndex = (int)i;
                                    break;
                                }
                            }
                        }

                        size_t p1 = valueStr.find(L"/1/");
                        if (p1 != std::wstring::npos) {
                            std::wstring mBlock = valueStr.substr(p1 + 3);
                            size_t p2 = mBlock.find(L'/');
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

                                        if (pd.SwapIndex >= 0 && pd.SwapIndex < (int)ConfigManager::Get().GetConfigs().size()) {
                                            auto& swapCfg = ConfigManager::Get().GetConfigs()[pd.SwapIndex];
                                            if (mIdx >= 0 && mIdx < (int)swapCfg.MorphTargetList.size()) {
                                                pd.MorphSet[swapCfg.MorphTargetList[mIdx].target] = mVal;
                                            }
                                        }
                                    } catch (...) {}
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
        nlohmann::json systemObj = nlohmann::json::object();
        nlohmann::json skelMeshPathObj = nlohmann::json::object();
        nlohmann::json skelMeshSwapObj = nlohmann::json::object();
        nlohmann::json palSwaps = nlohmann::json::object();
        
        systemObj["ALTR_MODversion"] = "4000";
        systemObj["WorldName"] = "Solo save";
        systemObj["WorldID"] = Utils::WStringToString(CurrentWorldSaveID);

        for (auto& [id, data] : PersistedSwaps) {
            if (data.SwapIndex >= 0 && data.SwapIndex < (int)ConfigManager::Get().GetConfigs().size()) {
                auto& swapCfg = ConfigManager::Get().GetConfigs()[data.SwapIndex];
                std::string idxStr = std::to_string(data.SwapIndex);

                std::wstring formattedPath = L"/Script/Engine.SkeletalMesh'" + Utils::FormatAssetPath(swapCfg.SkelMeshPath) + L"'";
                skelMeshPathObj[idxStr] = Utils::WStringToString(formattedPath);

                skelMeshSwapObj[idxStr] = "0/" + idxStr + "/6/1";

                std::wstring entryStr = L"0/" + Utils::StringToWString(idxStr);
                
                if (!data.MorphSet.empty()) {
                    entryStr += L"/1/";
                    for (auto& [morphName, morphVal] : data.MorphSet) {
                        int mIdx = -1;
                        for (int i = 0; i < (int)swapCfg.MorphTargetList.size(); ++i) {
                            if (swapCfg.MorphTargetList[i].target == morphName) { mIdx = i; break; }
                        }

                        if (mIdx != -1) {
                            std::wstring valStr = std::to_wstring(morphVal);
                            valStr.erase(valStr.find_last_not_of(L'0') + 1, std::wstring::npos);
                            if (valStr.back() == L'.') valStr += L"0"; 
                            
                            entryStr += std::to_wstring(mIdx) + L"_" + valStr + L":";
                        }
                    }
                }
                palSwaps[Utils::WStringToString(data.InstanceID)] = Utils::WStringToString(entryStr);
            }
        }
        
        out["System"] = systemObj;
        out["SkelMeshPath"] = skelMeshPathObj;
        out["MatPath"] = nlohmann::json::object();
        out["SkinName"] = nlohmann::json::object();
        out["Trait"] = nlohmann::json::object();
        out["Morph"] = nlohmann::json::object();
        out["SkelMeshSwap"] = skelMeshSwapObj;
        out["PalSwap"] = palSwaps;

        std::ofstream file(persistPath);
        if (file.is_open()) {
            file << out.dump(4);
            Output::send<LogLevel::Normal>(STR("[DynPals] World persistence synchronized (Altermatic 1:1 Parity).\n"));
        }
    }

    PalPersistData* SaveManager::GetPersistData(const std::wstring& InstanceID) {
        auto it = PersistedSwaps.find(InstanceID);
        return it != PersistedSwaps.end() ? &it->second : nullptr;
    }

    void SaveManager::SetPersistData(const std::wstring& InstanceID, const PalPersistData& Data) {
        PersistedSwaps[InstanceID] = Data;
        
        // Manual change made in UI. Save instantly to prevent data loss!
        SaveWorldData(); 
    }
}