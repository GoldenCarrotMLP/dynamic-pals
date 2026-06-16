#include "ConfigManager.hpp"
#include "Utils.hpp"
#include <DynamicOutput/DynamicOutput.hpp>
#include "json.hpp" // Updated include
#include <random>

using namespace RC;

namespace DynPals {

    void ConfigManager::Initialize(const std::wstring& BasePath) {
        ConfigPath = BasePath + L"Paks/~mods/SwapJSON/";
        LoadConfigJSONs();
    }

    void ConfigManager::LoadConfigJSONs() {
        std::wstring loadListPath = ConfigPath + L"_LoadList.json";
        std::string content = Utils::ReadFileToString(loadListPath);
        
        if (content.empty()) {
            Output::send<LogLevel::Error>(STR("[DynPals] _LoadList.json missing or unreadable!\n"));
            return;
        }

        try {
            nlohmann::json loadList = nlohmann::json::parse(content, nullptr, true, true);
            if (!loadList.contains("LoadList")) return;

            Configs.clear();
            for (auto& item : loadList.at("LoadList")) {
                std::string filename = item.get<std::string>();
                if (filename.empty()) continue;

                std::wstring filepath = ConfigPath + Utils::StringToWString(filename) + L".json";
                std::string fileContent = Utils::ReadFileToString(filepath);
                if (fileContent.empty()) continue;

                nlohmann::json configData = nlohmann::json::parse(fileContent, nullptr, true, true);
                if (configData.contains("SkelMeshSwap")) {
                    ParseSwaps(configData.at("SkelMeshSwap"));
                }
            }
            Output::send<LogLevel::Normal>(STR("[DynPals] Loaded {} swaps.\n"), Configs.size());
        } catch (const std::exception& e) {
            Output::send<LogLevel::Error>(STR("[DynPals] JSON Error: {}\n"), Utils::StringToWString(e.what()));
        }
    }

    void ConfigManager::ParseSwaps(const nlohmann::json& swapArray) {
        for (auto& swapJson : swapArray) {
            SwapConfig sc;
            sc.CharacterID = Utils::StringToWString(swapJson.at("CharacterID").get<std::string>());
            if (swapJson.contains("SkelMeshPath")) sc.SkelMeshPath = Utils::StringToWString(swapJson.at("SkelMeshPath").get<std::string>());
            if (swapJson.contains("Gender")) sc.Gender = Utils::StringToWString(swapJson.at("Gender").get<std::string>());
            if (swapJson.contains("SkinName")) sc.SkinName = Utils::StringToWString(swapJson.at("SkinName").get<std::string>());
            if (swapJson.contains("MinLevel")) sc.MinLevel = swapJson.at("MinLevel").get<int>();
            if (swapJson.contains("MaxLevel")) sc.MaxLevel = swapJson.at("MaxLevel").get<int>();
            
            if (swapJson.contains("ReqTrait")) {
                for (auto& trait : swapJson.at("ReqTrait")) sc.ReqTrait.push_back(Utils::StringToWString(trait.get<std::string>()));
            }
            
            if (swapJson.contains("MatReplace")) {
                for (auto& mat : swapJson.at("MatReplace")) {
                    MatReplace mr;
                    mr.index = mat.at("Index").is_string() ? mat.at("Index").get<std::string>() : std::to_string(mat.at("Index").get<int>());
                    mr.matPath = Utils::StringToWString(mat.at("MatPath").get<std::string>());
                    sc.MatReplaceList.push_back(mr);
                }
            }
            Configs.push_back(sc);
        }
    }

    int ConfigManager::FindBestSwap(const std::wstring& CharID, bool IsRare, const std::wstring& GenderStr, const std::vector<std::wstring>& Traits, int Level, const std::wstring& SkinName) {
        int bestScore = 999999;
        std::vector<int> bestMatches;

        for (size_t i = 0; i < Configs.size(); i++) {
            auto& swap = Configs[i];
            if (swap.CharacterID != CharID) continue;

            int score = 0;
            bool isValid = true;

            if (Level < swap.MinLevel || Level > swap.MaxLevel) isValid = false;
            
            if (isValid && swap.Gender != L"None" && swap.Gender != GenderStr) isValid = false;

            if (isValid && !swap.SkinName.empty() && SkinName != swap.SkinName) isValid = false;

            if (isValid) {
                for (auto& req : swap.ReqTrait) {
                    bool hasTrait = false;
                    for (auto& t : Traits) if (t == req) { hasTrait = true; break; }
                    if (!hasTrait) { isValid = false; break; }
                }
            }

            if (isValid) {
                if (score < bestScore) {
                    bestScore = score;
                    bestMatches = { (int)i };
                } else if (score == bestScore) {
                    bestMatches.push_back((int)i);
                }
            }
        }

        if (!bestMatches.empty()) {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(0, (int)bestMatches.size() - 1);
            return bestMatches[dis(gen)];
        }
        return -1;
    }
}