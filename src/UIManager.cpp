#include "UIManager.hpp"
#include "ConfigManager.hpp"
#include "SaveManager.hpp"
#include "PalProcessor.hpp"
#include "Utils.hpp"
#include <imgui.h>
#include <cmath> // Added for sqrt() distance formatting in logs

#include <Unreal/UObject.hpp>
#include <Unreal/FString.hpp>
#include <DynamicOutput/DynamicOutput.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace DynPals {

    void UIManager::ToggleMenu() {
        bIsMenuOpen = !bIsMenuOpen;
        Output::send<LogLevel::Normal>(STR("[DynPals] Menu Toggled. Open State: {}\n"), bIsMenuOpen ? STR("TRUE") : STR("FALSE"));
        
        if (bIsMenuOpen) {
            UpdateTarget();
            LockInput(true);
        } else {
            LockInput(false);
        }
    }

    void UIManager::LockInput(bool bLock) {
        UObject* PlayerController = UObjectGlobals::FindFirstOf(STR("PalPlayerController"));
        if (PlayerController) {
            Output::send<LogLevel::Normal>(STR("[DynPals] Setting PlayerController mouse cursor to {}\n"), bLock ? STR("VISIBLE") : STR("HIDDEN"));
            auto* prop = Utils::GetProperty(PlayerController, STR("bShowMouseCursor"));
            if (prop) {
                bool* pVal = prop->ContainerPtrToValuePtr<bool>(PlayerController);
                if (pVal) *pVal = bLock;
            }
            struct { bool bNewLookInput; } Params{ bLock };
            Utils::CallFunction(PlayerController, STR("SetIgnoreLookInput"), &Params);
        }
    }

    void UIManager::UpdateTarget() {
        TargetPal = nullptr;
        TargetInstanceID = L"";
        TargetCharID = L"";

        UObject* PlayerController = UObjectGlobals::FindFirstOf(STR("PalPlayerController"));
        if (!PlayerController) {
            Output::send<LogLevel::Error>(STR("[DynPals] Target Scanner: PalPlayerController not found!\n"));
            return;
        }

        UObject* PlayerPawn = nullptr;
        Utils::CallFunction(PlayerController, STR("K2_GetPawn"), &PlayerPawn);
        if (!PlayerPawn) {
            Output::send<LogLevel::Error>(STR("[DynPals] Target Scanner: Local PlayerPawn not found!\n"));
            return;
        }

        struct { FVector_UE5 RetVal; } LocParams;
        Utils::CallFunction(PlayerPawn, STR("K2_GetActorLocation"), &LocParams);
        FVector_UE5 PlayerLoc = LocParams.RetVal;

        std::vector<UObject*> AllPals;
        UObjectGlobals::FindAllOf(STR("PalCharacter"), AllPals);
        Output::send<LogLevel::Normal>(STR("[DynPals] Scanning {} loaded actors for closest Pal...\n"), AllPals.size());

        double closestDist = 999999999.0;
        UObject* closestPal = nullptr;

        for (UObject* Pal : AllPals) {
            if (Pal == PlayerPawn || !Pal) continue;

            struct { FVector_UE5 RetVal; } PalLocParams;
            Utils::CallFunction(Pal, STR("K2_GetActorLocation"), &PalLocParams);
            FVector_UE5 PalLoc = PalLocParams.RetVal;

            double dx = PalLoc.X - PlayerLoc.X;
            double dy = PalLoc.Y - PlayerLoc.Y;
            double dz = PalLoc.Z - PlayerLoc.Z;
            double distSq = (dx*dx) + (dy*dy) + (dz*dz);

            if (distSq < closestDist) {
                closestDist = distSq;
                closestPal = Pal;
            }
        }

        // Must be within roughly 30 meters (3000 Unreal Units)
        if (closestPal && closestDist < (3000.0 * 3000.0)) {
            TargetPal = closestPal;

            UObject* ParamComp = nullptr;
            Utils::GetPropertyValue(TargetPal, STR("CharacterParameterComponent"), ParamComp);
            if (!ParamComp) return;

            UObject* IndivParam = nullptr;
            Utils::GetPropertyValue(ParamComp, STR("IndividualParameter"), IndivParam);
            if (!IndivParam) return;

            struct FPalInstanceID { DynPalsGuid PlayerUId; DynPalsGuid InstanceId; } IDStruct;
            if (Utils::GetPropertyValue(IndivParam, STR("IndividualId"), IDStruct)) {
                TargetInstanceID = Utils::GuidToWString(IDStruct.InstanceId);
            }

            UObject* PalUtil = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
            struct { UObject* Char; FName RetVal; } CharIDParams{TargetPal, FName()};
            if (PalUtil) Utils::CallFunction(PalUtil, STR("GetCharacterIDFromCharacter"), &CharIDParams);
            TargetCharID = PalProcessor::Get().StripCharacterPrefix(CharIDParams.RetVal.ToString());

            Output::send<LogLevel::Normal>(STR("[DynPals] Target Scanner: Targeted nearest Pal: '{}' (GUID: '{}', Distance: '{}' units)\n"), 
                TargetCharID, TargetInstanceID, std::sqrt(closestDist));
        } else {
            Output::send<LogLevel::Normal>(STR("[DynPals] Target Scanner: No Pal found within 30 meters.\n"));
        }
    }

    void UIManager::DrawUI() {
        if (!bIsMenuOpen) return;

        // ImGui Modern Styling Setup
        ImGui::SetNextWindowSize(ImVec2(400, 600), ImGuiCond_FirstUseEver);
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 8.0f;
        style.FrameRounding = 6.0f;
        style.GrabRounding = 6.0f;
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.08f, 0.90f);

        if (ImGui::Begin("DynamicPals Target Customization", &bIsMenuOpen, ImGuiWindowFlags_NoCollapse)) {
            
            if (!TargetPal) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "No Pal found nearby.");
                if (ImGui::Button("Scan For Pal", ImVec2(-1, 30))) {
                    UpdateTarget();
                }
            } else {
                ImGui::Text("Target Pal ID: %s", Utils::WStringToString(TargetCharID).c_str());
                if (ImGui::Button("Rescan Target", ImVec2(-1, 25))) UpdateTarget();
                ImGui::Separator();
                ImGui::Spacing();

                // Group skins by PackName
                auto configIndices = ConfigManager::Get().GetConfigsForCharID(TargetCharID);
                std::map<std::wstring, std::vector<int>> groupedPacks;
                for (int idx : configIndices) {
                    groupedPacks[ConfigManager::Get().GetConfigs()[idx].PackName].push_back(idx);
                }

                if (groupedPacks.empty()) {
                    ImGui::Text("No custom skins configured for this Pal.");
                } else {
                    for (auto& [pack, indices] : groupedPacks) {
                        if (ImGui::CollapsingHeader(Utils::WStringToString(pack).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                            for (int idx : indices) {
                                auto& cfg = ConfigManager::Get().GetConfigs()[idx];
                                std::string label = Utils::WStringToString(cfg.SkinName.empty() ? L"Default Base" : cfg.SkinName);
                                label += " (Level " + std::to_string(cfg.MinLevel) + "-" + std::to_string(cfg.MaxLevel) + ")";

                                if (ImGui::Button(label.c_str(), ImVec2(-1, 0))) {
                                    PalProcessor::Get().ForceSwap(TargetPal, idx);
                                }
                            }
                        }
                    }
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Dynamic Morph Sliders
                ImGui::Text("Morph Adjustments:");
                PalPersistData* persist = SaveManager::Get().GetPersistData(TargetInstanceID);
                
                if (persist && persist->SwapIndex != -1) {
                    auto& activeCfg = ConfigManager::Get().GetConfigs()[persist->SwapIndex];
                    
                    if (activeCfg.MorphTargetList.empty()) {
                        ImGui::TextDisabled("No morphs available for selected skin.");
                    } else {
                        for (auto& morph : activeCfg.MorphTargetList) {
                            if (morph.type != L"Restrict" && morph.minVal < morph.maxVal) {
                                
                                float val = persist->MorphSet[morph.target];
                                std::string label = Utils::WStringToString(morph.target);

                                if (ImGui::SliderFloat(label.c_str(), &val, morph.minVal, morph.maxVal)) {
                                    persist->MorphSet[morph.target] = val;
                                    
                                    // Apply the slider update in real-time
                                    UObject* MeshComp = nullptr;
                                    Utils::CallFunction(TargetPal, STR("GetMainMesh"), &MeshComp);
                                    if (MeshComp) {
                                        struct { FName MorphTargetName; float Value; bool bRemoveZeroWeight; } MorphParams{
                                            FName(morph.target.c_str(), FNAME_Add), val, false
                                        };
                                        Utils::CallFunction(MeshComp, STR("SetMorphTarget"), &MorphParams);
                                    }
                                    // Flag for saving
                                    SaveManager::Get().SetPersistData(TargetInstanceID, *persist);
                                }
                            }
                        }
                    }
                } else {
                    ImGui::TextDisabled("Select a skin from the packs above first.");
                }
            }
        }
        ImGui::End();

        // Cleanup if user clicked the 'X' button
        if (!bIsMenuOpen) {
            LockInput(false);
        }
    }
}