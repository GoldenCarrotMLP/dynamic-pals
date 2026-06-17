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
            
            if (swapJson.contains("MinLevel")) sc.MinLevel = swapJson.at("MinLevel").get<int>();
            if (swapJson.contains("MaxLevel")) sc.MaxLevel = swapJson.at("MaxLevel").get<int>();
            if (swapJson.contains("MinTrust")) sc.MinTrust = swapJson.at("MinTrust").get<int>();
            if (swapJson.contains("MaxTrust")) sc.MaxTrust = swapJson.at("MaxTrust").get<int>();
            if (swapJson.contains("MinRank")) sc.MinRank = swapJson.at("MinRank").get<int>();
            if (swapJson.contains("MaxRank")) sc.MaxRank = swapJson.at("MaxRank").get<int>();
            if (swapJson.contains("Weight")) {
                int w = swapJson.at("Weight").get<int>();
                sc.Weight = w > 0 ? w : 1;
            }
            
            if (swapJson.contains("IsRarePal")) {
                if (swapJson.at("IsRarePal").is_boolean()) {
                    sc.IsRarePal = swapJson.at("IsRarePal").get<bool>();
                } else if (swapJson.at("IsRarePal").is_string()) {
                    std::string s = swapJson.at("IsRarePal").get<std::string>();
                    sc.IsRarePal = (s == "true");
                }
            }

            if (swapJson.contains("IsWildPal")) {
                if (swapJson.at("IsWildPal").is_boolean()) {
                    sc.IsWildPal = swapJson.at("IsWildPal").get<bool>();
                } else if (swapJson.at("IsWildPal").is_string()) {
                    std::string s = swapJson.at("IsWildPal").get<std::string>();
                    sc.IsWildPal = (s == "true");
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

  std::vector<SwapEvaluation> ConfigManager::EvaluateAllSwaps(const std::wstring& CharID, bool IsRare, const std::wstring& GenderStr, const std::vector<std::wstring>& Traits, int Level, const std::wstring& SkinName, int Rank, int Trust, bool IsWild) const {
        std::vector<SwapEvaluation> results;

        for (size_t i = 0; i < Configs.size(); i++) {
            auto& swap = Configs[i];
            if (swap.CharacterID != CharID) continue;

            SwapEvaluation eval;
            eval.ConfigIndex = (int)i;
            eval.Score = 0;
            eval.IsValid = true;

            // 1. Hard Limits
            if (Level < swap.MinLevel || Level > swap.MaxLevel) eval.IsValid = false;
            if (eval.IsValid && (Rank < swap.MinRank || Rank > swap.MaxRank)) eval.IsValid = false;
            if (eval.IsValid && (Trust < swap.MinTrust || Trust > swap.MaxTrust)) eval.IsValid = false;

            // 2. Gender Match with fallbacks
            if (eval.IsValid && swap.Gender != L"None") {
                if (swap.Gender != GenderStr) {
                    bool fallbackMatched = false;
                    if (swap.Gender == L"Male" && (GenderStr == L"Futa" || GenderStr == L"FullFuta")) {
                        eval.Score += 50000;
                        fallbackMatched = true;
                    } else if (swap.Gender == L"Female" && (GenderStr == L"Andro" || GenderStr == L"Neutered" || GenderStr == L"FullNeutered")) {
                        eval.Score += 50000;
                        fallbackMatched = true;
                    }
                    if (!fallbackMatched) eval.IsValid = false;
                }
            } else if (eval.IsValid && swap.Gender == L"None" && GenderStr != L"None") {
                eval.Score += 500000; 
            }

            // 3. Exact String Matches
            if (eval.IsValid && !swap.SkinName.empty() && SkinName != swap.SkinName) {
                eval.IsValid = false;
            }

            // 4. Boolean Flags
            if (eval.IsValid && swap.IsRarePal.has_value()) {
                bool reqRare = swap.IsRarePal.value();
                if (reqRare && !IsRare) eval.IsValid = false; 
                else if (!reqRare && IsRare) eval.Score += 110; 
            }

            if (eval.IsValid && swap.IsWildPal.has_value()) {
                bool reqWild = swap.IsWildPal.value();
                if (reqWild != IsWild) eval.IsValid = false;
            }

            // 5. Traits
            if (eval.IsValid) {
                for (const auto& req : swap.ReqTrait) {
                    bool hasTrait = false;
                    for (const auto& t : Traits) {
                        if (t == req) { hasTrait = true; break; }
                    }
                    if (!hasTrait) {
                        eval.IsValid = false;
                        break;
                    } else {
                        eval.Score -= 5; 
                    }
                }
            }

            if (eval.IsValid) {
                for (const auto& pref : swap.PrefTrait) {
                    bool hasTrait = false;
                    for (const auto& t : Traits) {
                        if (t == pref) { hasTrait = true; break; }
                    }
                    if (hasTrait) eval.Score -= 5; 
                    else eval.Score += 5; 
                }
            }

            results.push_back(eval);
        }
        return results;
    }

    int ConfigManager::PickBestSwap(const std::vector<SwapEvaluation>& evaluations) const {
        int bestScore = 999999;
        std::vector<int> bestMatches;

        for (const auto& eval : evaluations) {
            if (!eval.IsValid) continue;
            
            if (eval.Score < bestScore) {
                bestScore = eval.Score;
                bestMatches = { eval.ConfigIndex };
            } else if (eval.Score == bestScore) {
                bestMatches.push_back(eval.ConfigIndex);
            }
        }

        if (!bestMatches.empty()) {
            // NEW: Calculate the sum of weights among the tied configurations
            int totalWeight = 0;
            for (int idx : bestMatches) {
                totalWeight += Configs[idx].Weight;
            }

            if (totalWeight > 0) {
                std::random_device rd;
                std::mt19937 gen(rd());
                std::uniform_int_distribution<> dis(0, totalWeight - 1);
                int randomValue = dis(gen);

                // Run a cumulative weight select to find the weighted winner
                int cumulativeWeight = 0;
                for (int idx : bestMatches) {
                    cumulativeWeight += Configs[idx].Weight;
                    if (randomValue < cumulativeWeight) {
                        return idx;
                    }
                }
            }
            return bestMatches[0]; // Fallback
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