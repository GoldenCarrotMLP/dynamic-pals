#include "ConfigManager.hpp"
#include "Utils.hpp"
#include <DynamicOutput/DynamicOutput.hpp>
#include "json.hpp"
#include <random>

using namespace RC;
using namespace RC::Unreal;

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
                std::wstring packName = L"Default Pack";
                if (configData.contains("PackName")) {
                    packName = Utils::StringToWString(configData.at("PackName").get<std::string>());
                }

                if (configData.contains("SkelMeshSwap")) {
                    ParseSwaps(packName, configData.at("SkelMeshSwap"));
                }

            }
            Output::send<LogLevel::Normal>(STR("[DynPals] Complete matchmaking table compiled with {} swaps.\n"), Configs.size());
        } catch (const std::exception& e) {
            Output::send<LogLevel::Error>(STR("[DynPals] JSON Error: {}\n"), Utils::StringToWString(e.what()));
        }
    }

    void ConfigManager::ParseSwaps(const std::wstring& PackName, const nlohmann::json& swapArray) {
        for (auto& swapJson : swapArray) {
            SwapConfig sc;
            sc.PackName = PackName; 
            sc.CharacterID = Utils::StringToWString(swapJson.at("CharacterID").get<std::string>());
            if (swapJson.contains("SkelMeshPath")) sc.SkelMeshPath = Utils::StringToWString(swapJson.at("SkelMeshPath").get<std::string>());
            if (swapJson.contains("Gender")) sc.Gender = Utils::StringToWString(swapJson.at("Gender").get<std::string>());
            if (swapJson.contains("SkinName")) sc.SkinName = Utils::StringToWString(swapJson.at("SkinName").get<std::string>());
            
            // New Matchmaking Properties
            if (swapJson.contains("MinLevel")) sc.MinLevel = swapJson.at("MinLevel").get<int>();
            if (swapJson.contains("MaxLevel")) sc.MaxLevel = swapJson.at("MaxLevel").get<int>();
            if (swapJson.contains("MinTrust")) sc.MinTrust = swapJson.at("MinTrust").get<int>();
            if (swapJson.contains("MaxTrust")) sc.MaxTrust = swapJson.at("MaxTrust").get<int>();
            if (swapJson.contains("MinRank")) sc.MinRank = swapJson.at("MinRank").get<int>();
            if (swapJson.contains("MaxRank")) sc.MaxRank = swapJson.at("MaxRank").get<int>();
            
            if (swapJson.contains("IsRarePal")) {
                if (swapJson.at("IsRarePal").is_boolean()) {
                    sc.IsRarePal = swapJson.at("IsRarePal").get<bool>() ? L"true" : L"false";
                } else {
                    sc.IsRarePal = Utils::StringToWString(swapJson.at("IsRarePal").get<std::string>());
                }
            }

            if (swapJson.contains("ReqTrait")) {
                for (auto& trait : swapJson.at("ReqTrait")) sc.ReqTrait.push_back(Utils::StringToWString(trait.get<std::string>()));
            }
            if (swapJson.contains("PrefTrait")) {
                for (auto& trait : swapJson.at("PrefTrait")) sc.PrefTrait.push_back(Utils::StringToWString(trait.get<std::string>()));
            }
            
            if (swapJson.contains("MatReplace")) {
                for (auto& mat : swapJson.at("MatReplace")) {
                    MatReplace mr;
                    mr.index = mat.at("Index").is_string() ? mat.at("Index").get<std::string>() : std::to_string(mat.at("Index").get<int>());
                    mr.matPath = Utils::StringToWString(mat.at("MatPath").get<std::string>());
                    sc.MatReplaceList.push_back(mr);
                }
            }

            if (swapJson.contains("MorphTarget")) {
                for (auto& morph : swapJson.at("MorphTarget")) {
                    MorphTarget mt;
                    mt.target = Utils::StringToWString(morph.at("Target").get<std::string>());
                    if (morph.contains("Set")) mt.setVal = morph.at("Set").get<double>();
                    if (morph.contains("Min")) mt.minVal = morph.at("Min").get<double>();
                    if (morph.contains("Max")) mt.maxVal = morph.at("Max").get<double>();
                    if (morph.contains("Type")) mt.type = Utils::StringToWString(morph.at("Type").get<std::string>());
                    sc.MorphTargetList.push_back(mt);
                }
            }
            Configs.push_back(sc);
        }
    }

    int ConfigManager::FindBestSwap(const std::wstring& CharID, bool IsRare, const std::wstring& GenderStr, const std::vector<std::wstring>& Traits, int Level, const std::wstring& SkinName, int Rank, int Trust) {
        int bestScore = 999999;
        std::vector<int> bestMatches;

        for (size_t i = 0; i < Configs.size(); i++) {
            auto& swap = Configs[i];
            if (swap.CharacterID != CharID) continue;

            int score = 0;
            bool isValid = true;

            // 1. Level Check
            if (Level < swap.MinLevel || Level > swap.MaxLevel) isValid = false;

            // 2. Rank Check (Stars 0-4)
            if (isValid && (Rank < swap.MinRank || Rank > swap.MaxRank)) isValid = false;

            // 3. Trust Check (Friendship)
            if (isValid && (Trust < swap.MinTrust || Trust > swap.MaxTrust)) isValid = false;

            // 4. Gender Match with fallbacks
            if (isValid && swap.Gender != L"None") {
                if (swap.Gender != GenderStr) {
                    bool fallbackMatched = false;
                    
                    if (swap.Gender == L"Male" && (GenderStr == L"Futa" || GenderStr == L"FullFuta")) {
                        score += 50000;
                        fallbackMatched = true;
                    } else if (swap.Gender == L"Female" && (GenderStr == L"Andro" || GenderStr == L"Neutered" || GenderStr == L"FullNeutered")) {
                        score += 50000;
                        fallbackMatched = true;
                    }
                    
                    if (!fallbackMatched) isValid = false;
                }
            } else if (isValid && swap.Gender == L"None" && GenderStr != L"None") {
                score += 500000; // Genderless fallback degrade
            }

            // 5. Skin Name Check
            if (isValid && !swap.SkinName.empty() && SkinName != swap.SkinName) {
                isValid = false;
            }

            // 6. Rare Status Match
            if (isValid && !swap.IsRarePal.empty()) {
                bool reqRare = (swap.IsRarePal == L"true");
                if (reqRare && !IsRare) isValid = false; 
                else if (!reqRare && IsRare) score += 110; 
            }

            // 7. Required Traits
            if (isValid) {
                for (const auto& req : swap.ReqTrait) {
                    bool hasTrait = false;
                    for (const auto& t : Traits) {
                        if (t == req) { hasTrait = true; break; }
                    }
                    if (!hasTrait) {
                        isValid = false;
                        break;
                    } else {
                        score -= 5; 
                    }
                }
            }

            // 8. Preferred Traits
            if (isValid) {
                for (const auto& pref : swap.PrefTrait) {
                    bool hasTrait = false;
                    for (const auto& t : Traits) {
                        if (t == pref) { hasTrait = true; break; }
                    }
                    if (hasTrait) score -= 5; 
                    else score += 5; 
                }
            }

            // Evaluation
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

    std::vector<int> ConfigManager::GetConfigsForCharID(const std::wstring& CharID) const {
        std::vector<int> results;
        for (size_t i = 0; i < Configs.size(); ++i) {
            if (Configs[i].CharacterID == CharID) {
                results.push_back((int)i);
            }
        }
        return results;
    }
}