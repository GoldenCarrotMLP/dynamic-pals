#include "VFXManager.hpp"
#include "Utils.hpp"
#include "AsyncHelper.hpp"
#include "NotificationManager.hpp"
#include "json.hpp"
#include <sstream>
#include <filesystem> // Required for platform-independent path resolution [1, 2]

using namespace RC::Unreal;
namespace fs = std::filesystem;

namespace DynPals {

    // Unique local representation to bypass UE4SS incomplete header structure definitions
    struct VFXLinearColor {
        float R, G, B, A;
    };

    // Safe stack reflection helper to invoke Unreal's native DrawDebugBox
    static void DrawDebugBoxDirect(UObject* WorldContext, FVector_UE5 Center, FVector_UE5 Extent, VFXLinearColor Color, float Duration, float Thickness) {
        UObject* Lib = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
        if (!Lib) return;

        UFunction* DrawFunc = Lib->GetFunctionByNameInChain(STR("DrawDebugBox"));
        if (!DrawFunc) return;

        alignas(8) uint8_t Params[256] = {0};

        auto FillProp = [&](const wchar_t* Name, auto Value) {
            FProperty* Prop = DrawFunc->GetPropertyByNameInChain(Name);
            if (Prop) *Prop->ContainerPtrToValuePtr<decltype(Value)>(Params) = Value;
        };

        struct FRotator_UE5 { double Pitch, Yaw, Roll; } ZeroRot{0.0, 0.0, 0.0};

        FillProp(STR("WorldContextObject"), WorldContext);
        FillProp(STR("Center"), Center);
        FillProp(STR("Extent"), Extent);
        FillProp(STR("LineColor"), Color);
        FillProp(STR("Rotation"), ZeroRot);
        FillProp(STR("Duration"), Duration);
        FillProp(STR("Thickness"), Thickness);

        Lib->ProcessEvent(DrawFunc, Params);
    }

    // Dynamic Directory Resolver: Traverses upwards from the executing DLL to find the /vfx folder [3]
    static std::wstring GetVfxFolderPath() {
        HMODULE hModule = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)&GetVfxFolderPath, &hModule);
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(hModule, path, MAX_PATH);
        
        fs::path dllPath(path);
        fs::path currentDir = dllPath.parent_path();

        // Traverse upwards up to 3 levels to locate the "vfx" folder [3]
        for (int i = 0; i < 4; ++i) {
            fs::path testPath = currentDir / "vfx";
            if (fs::exists(testPath) && fs::is_directory(testPath)) {
                return testPath.wstring() + L"/";
            }
            if (currentDir.has_parent_path()) {
                currentDir = currentDir.parent_path();
            } else {
                break;
            }
        }

        // Default fallback if folder structure isn't found
        return dllPath.parent_path().wstring() + L"/vfx/";
    }

    void VFXManager::AddComposerTask(UObject* PalActor, const std::vector<VFXTimelineEvent>& Events) {
        if (!PalActor || Events.empty()) return;
        VFXComposerTask task;
        task.TargetPal = PalActor;
        task.Events = Events;
        task.StartTime = std::chrono::steady_clock::now();
        ActiveTasks.push_back(task);
    }

    float VFXManager::PlayComposition(UObject* PalActor, const std::wstring& CompName) {
        if (!PalActor) return 0.0f;

        // Dynamic File Read for rapid iteration!
        if (bHotReloadCompositions) LoadCompositions(true);

        auto it = Compositions.find(CompName);
        if (it == Compositions.end()) {
            DP_LOG(Warning, "VFXManager: Composition '{}' not found in JSON.", CompName);
            return 0.0f;
        }

        std::vector<VFXTimelineEvent> sequence;

        for (const auto& tmpl : it->second.Events) {
            // We use a shared_ptr to store the component pointer across lambdas safely
            auto compState = std::make_shared<UObject*>(nullptr);
            
            // Event to spawn
            sequence.push_back({
                tmpl.Time,
                [this, PalActor, tmpl, compState]() {
                    *compState = AttachVFXToPal(PalActor, tmpl.VfxPath, tmpl.Socket, tmpl.Scale, tmpl.ZOffset);
                }
            });

            // Event to kill (if duration was specified)
            if (tmpl.Duration > 0.0f) {
                sequence.push_back({
                    tmpl.Time + tmpl.Duration,
                    [compState]() {
                        if (*compState) {
                            UFunction* DestroyFunc = (*compState)->GetFunctionByNameInChain(STR("K2_DestroyComponent"));
                            if (!DestroyFunc) DestroyFunc = (*compState)->GetFunctionByNameInChain(STR("DestroyComponent"));
                            if (DestroyFunc) {
                                alignas(8) uint8_t Params[16] = {0};
                                (*compState)->ProcessEvent(DestroyFunc, Params);
                            }
                        }
                    }
                });
            }
        }
        AddComposerTask(PalActor, sequence);
        
        // RETURN the JSON-defined swap delay!
        return it->second.SwapTime; 
    }

    float VFXManager::PlayAnimMontage(UObject* Character, UObject* MontageAsset, float PlayRate) {
        if (!Character || !MontageAsset) return 0.0f;
        UFunction* Func = Character->GetFunctionByNameInChain(STR("PlayAnimMontage"));
        if (!Func) return 0.0f;
        alignas(8) uint8_t Params[64] = {0};
        FProperty* P_Montage = Func->GetPropertyByNameInChain(STR("AnimMontage"));
        if (P_Montage) *P_Montage->ContainerPtrToValuePtr<UObject*>(Params) = MontageAsset;
        FProperty* P_Rate = Func->GetPropertyByNameInChain(STR("InPlayRate"));
        if (P_Rate) *P_Rate->ContainerPtrToValuePtr<float>(Params) = PlayRate;
        Character->ProcessEvent(Func, Params);
        FProperty* RetProp = Func->GetPropertyByNameInChain(STR("ReturnValue"));
        return RetProp ? *RetProp->ContainerPtrToValuePtr<float>(Params) : 0.0f;
    }

    void VFXManager::KillCurrentPreview() {
        if (ActivePreviewComponent) {
            UFunction* DestroyFunc = ActivePreviewComponent->GetFunctionByNameInChain(STR("K2_DestroyComponent"));
            if (!DestroyFunc) DestroyFunc = ActivePreviewComponent->GetFunctionByNameInChain(STR("DestroyComponent"));
            if (DestroyFunc) {
                alignas(8) uint8_t Params[16] = {0}; 
                ActivePreviewComponent->ProcessEvent(DestroyFunc, Params);
            }
            ActivePreviewComponent = nullptr;
        }
    }

    void VFXManager::LoadCompositions(bool bForceReload) {
        std::wstring CompPath = ModsPath + L"vfx_composition.json";
        std::string fileContent = Utils::ReadFileToString(CompPath);
        if (fileContent.empty()) {
            // Add the Error warning here!
            DP_LOG(Error, "Missing 'vfx_composition.json' please download the latest release from GitHub.");
            return;
        }

        try {
            auto j = nlohmann::json::parse(fileContent, nullptr, true, true);

            if (j.contains("hotReload") && j["hotReload"].is_boolean()) {
                bHotReloadCompositions = j["hotReload"].get<bool>();
            }
            if (j.contains("compositions") && j["compositions"].is_object()) {
                Compositions.clear();
                for (auto& [compName, compObj] : j["compositions"].items()) {
                    if (!compObj.is_object()) continue;

                    VFXComposition comp;
                    comp.SwapTime = compObj.value("swapTime", 0.0f);

                    if (compObj.contains("events") && compObj["events"].is_array()) {
                        for (auto& ev : compObj["events"]) {
                            VFXEventTemplate t;
                            t.Time = ev.value("time", 0.0f);
                            t.VfxPath = Utils::StringToWString(ev.value("vfxPath", ""));
                            t.Duration = ev.value("duration", -1.0f);
                            t.Socket = Utils::StringToWString(ev.value("socket", "None"));
                            t.Scale = ev.value("scale", 1.0f);
                            t.ZOffset = ev.value("zOffset", 0.0f);
                            comp.Events.push_back(t);
                        }
                    }
                    Compositions[Utils::StringToWString(compName)] = comp;
                }
            }
            if (!bForceReload) DP_LOG(Default, "VFXManager: Loaded {} dynamic VFX compositions.", Compositions.size());
        } catch (...) {
            DP_LOG(Warning, "VFXManager: Failed to parse vfx_composition.json!");
        }
    }

    void VFXManager::PlaySwapEffect(UObject* PalActor, const std::wstring& VfxPath, float ZOffset) {
        if (!PalActor) return;
        DP_LOG(Default, "VFXManager: Playing manual swap effect on Pal '{}' using path: '{}' (ZOffset: {:.2f})", 
            PalActor->GetName(), VfxPath, ZOffset);

        UObject* Spawned = AttachVFXToPal(PalActor, VfxPath, L"None", 1.0f, ZOffset);
    }

    void VFXManager::Initialize() {
        // Find the absolute folder path of the visual assets dynamically!
        ModsPath = GetVfxFolderPath();
        
        LoadCompositions();

        std::wstring ListPath = ModsPath + L"vfx_list.txt";
        std::string fileContent = Utils::ReadFileToString(ListPath);
        if (!fileContent.empty()) {
            std::istringstream stream(fileContent);
            std::string line;
            while (std::getline(stream, line)) {
                line.erase(0, line.find_first_not_of(" \t\r\n"));
                size_t last = line.find_last_not_of(" \t\r\n");
                if (last != std::string::npos) line.erase(last + 1);
                if (!line.empty()) VFXList.push_back(Utils::StringToWString(line));
            }
            DP_LOG(Default, "VFXManager initialized with {} effects from vfx_list.txt.", VFXList.size());
        } else {
            // Add the Error warning here!
            DP_LOG(Error, "Missing 'vfx_list.txt'! Please download the latest release from GitHub.");
        }
    }

    void VFXManager::SpawnCurrentPreview() {
        if (VFXList.empty()) return;
        KillCurrentPreview();

        UObject* PlayerController = UObjectGlobals::FindFirstOf(STR("PalPlayerController"));
        if (!PlayerController) return;

        struct FRotator_UE5 { double Pitch, Yaw, Roll; };
        struct { FVector_UE5 Location; FRotator_UE5 Rotation; } ViewParams;
        Utils::CallFunction(PlayerController, STR("GetPlayerViewPoint"), &ViewParams);

        double PitchRad = ViewParams.Rotation.Pitch * 0.01745329251;
        double YawRad = ViewParams.Rotation.Yaw * 0.01745329251;
        double CosPitch = std::cos(PitchRad);

        FVector_UE5 Forward;
        Forward.X = std::cos(YawRad) * CosPitch;
        Forward.Y = std::sin(YawRad) * CosPitch;
        Forward.Z = std::sin(PitchRad);

        FVector_UE5 SpawnLoc;
        SpawnLoc.X = ViewParams.Location.X + (Forward.X * 300.0);
        SpawnLoc.Y = ViewParams.Location.Y + (Forward.Y * 300.0);
        SpawnLoc.Z = ViewParams.Location.Z + (Forward.Z * 300.0);

        std::wstring TargetPath = VFXList[CurrentIndex];
        UObject* VFXAsset = Utils::LoadAssetSafely(TargetPath);
        
        if (!VFXAsset) {
            NotificationManager::Get().EnqueueToast(L"Failed to load: " + TargetPath, EPalLogPriority::Important, EPalLogContentToneType::Negative);
            return;
        }

        bool bIsNiagara = (TargetPath.find(L"/NS_") != std::wstring::npos);
        std::wstring LibraryPath = bIsNiagara ? L"/Script/Niagara.Default__NiagaraFunctionLibrary" : L"/Script/Engine.Default__GameplayStatics";
        std::wstring FuncName = bIsNiagara ? L"SpawnSystemAtLocation" : L"SpawnEmitterAtLocation";
        std::wstring TemplateName = bIsNiagara ? L"SystemTemplate" : L"EmitterTemplate";

        UObject* Lib = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, LibraryPath.c_str());
        if (Lib) {
            UFunction* SpawnFunc = Lib->GetFunctionByNameInChain(FuncName.c_str());
            if (SpawnFunc) {
                alignas(8) uint8_t Params[256] = {0};
                auto FillProp = [&](const wchar_t* Name, auto Value) {
                    FProperty* Prop = SpawnFunc->GetPropertyByNameInChain(Name);
                    if (Prop) *Prop->ContainerPtrToValuePtr<decltype(Value)>(Params) = Value;
                };

                FillProp(STR("WorldContextObject"), PlayerController);
                FillProp(TemplateName.c_str(), VFXAsset);
                FillProp(STR("Location"), SpawnLoc);
                
                FProperty* ScaleProp = SpawnFunc->GetPropertyByNameInChain(STR("Scale"));
                if (ScaleProp) {
                    FVector_UE5* ScalePtr = ScaleProp->ContainerPtrToValuePtr<FVector_UE5>(Params);
                    ScalePtr->X = 1.0; ScalePtr->Y = 1.0; ScalePtr->Z = 1.0;
                }

                FProperty* AutoActProp = SpawnFunc->GetPropertyByNameInChain(bIsNiagara ? STR("bAutoActivate") : STR("bAutoActivateSystem"));
                if (AutoActProp && AutoActProp->GetClass().GetName() == L"BoolProperty") {
                    static_cast<FBoolProperty*>(AutoActProp)->SetPropertyValue(AutoActProp->ContainerPtrToValuePtr<void>(Params), true);
                }

                Lib->ProcessEvent(SpawnFunc, Params);

                FProperty* RetProp = SpawnFunc->GetPropertyByNameInChain(STR("ReturnValue"));
                if (RetProp) {
                    ActivePreviewComponent = *RetProp->ContainerPtrToValuePtr<UObject*>(Params);
                    SpawnTime = std::chrono::steady_clock::now();

                    VFXLinearColor GreenColor = {0.0f, 1.0f, 0.0f, 1.0f};
                    FVector_UE5 Extent = {50.0, 50.0, 50.0}; 
                    DrawDebugBoxDirect(PlayerController, SpawnLoc, Extent, GreenColor, 10.0f, 2.0f);
                }
            }
        }

        std::wstring BaseName = TargetPath;
        size_t LastSlash = BaseName.find_last_of(L'/');
        if (LastSlash != std::wstring::npos) BaseName = BaseName.substr(LastSlash + 1);
    }

    void VFXManager::CycleNext() {
        if (VFXList.empty()) return;
        CurrentIndex = (CurrentIndex + 1) % VFXList.size();
        AsyncHelper::AsyncTask(ENamedThreads::GameThread, [this]() { SpawnCurrentPreview(); });
    }

    void VFXManager::CyclePrevious() {
        if (VFXList.empty()) return;
        CurrentIndex = (CurrentIndex - 1 + VFXList.size()) % VFXList.size();
        AsyncHelper::AsyncTask(ENamedThreads::GameThread, [this]() { SpawnCurrentPreview(); });
    }

    void VFXManager::Tick() {
        auto now = std::chrono::steady_clock::now();

        // 1. Tick Composer tasks
        for (auto& task : ActiveTasks) {
            if (task.bIsComplete) continue;

            float elapsed = std::chrono::duration<float>(now - task.StartTime).count();
            bool allTriggered = true;

            for (auto& ev : task.Events) {
                if (!ev.bHasTriggered) {
                    if (elapsed >= ev.TriggerTime) {
                        ev.Action();
                        ev.bHasTriggered = true;
                    } else {
                        allTriggered = false;
                    }
                }
            }
            if (allTriggered) task.bIsComplete = true;
        }

        // Clean up completed tasks
        ActiveTasks.erase(
            std::remove_if(ActiveTasks.begin(), ActiveTasks.end(), [](const VFXComposerTask& t) { return t.bIsComplete; }),
            ActiveTasks.end()
        );

        // 2. Tick standard preview
        if (ActivePreviewComponent) {
            if (std::chrono::duration_cast<std::chrono::seconds>(now - SpawnTime).count() >= 10) {
                KillCurrentPreview();
            }
        }
    }

    UObject* VFXManager::AttachVFXToPal(UObject* PalActor, const std::wstring& VfxPath, const std::wstring& SocketName, float ScaleMult, float ZOffsetMult) {
        if (!PalActor) return nullptr;
        
        UObject* MeshComp = nullptr;
        Utils::CallFunction(PalActor, STR("GetMainMesh"), &MeshComp);

        UObject* RootComp = nullptr;
        Utils::GetPropertyValue(PalActor, STR("RootComponent"), RootComp);

        // Visual only attachments are now natively bound to MeshComp, but positionally resolved via Capsule! [1, 5]
        UObject* AttachTarget = MeshComp;
        if (!AttachTarget) AttachTarget = RootComp;
        if (!AttachTarget) {
            DP_LOG(Warning, "[VFX] Failed to resolve an AttachTarget (MainMesh or RootComponent) for Pal '{}'. Aborting attachment.", PalActor->GetName());
            return nullptr;
        }

        UObject* VFXAsset = Utils::LoadAssetSafely(VfxPath);
        if (!VFXAsset) {
            DP_LOG(Warning, "[VFX] Failed to load VFX Asset at path: '{}'. Check if files are missing.", VfxPath);
            return nullptr;
        }
        if (!VFXAsset) return nullptr;

        // Fetch dynamic 3D bounds in world space
        struct { bool bOnlyCollidingComponents; uint8_t Pad[7]; FVector_UE5 Origin; FVector_UE5 BoxExtent; } BoundsParams{true, {0}, {0.0,0.0,0.0}, {0.0,0.0,0.0}};
        UFunction* BoundsFunc = PalActor->GetFunctionByNameInChain(STR("GetActorBounds"));
        if (BoundsFunc) PalActor->ProcessEvent(BoundsFunc, &BoundsParams);
        
        if (BoundsParams.BoxExtent.Z < 10.0) BoundsParams.BoxExtent = {50.0, 50.0, 50.0}; 

        double BaselineHalfHeight = 50.0;
        double ScaleFactor = (BoundsParams.BoxExtent.Z / BaselineHalfHeight) * ScaleMult;
        if (ScaleFactor < 0.1) ScaleFactor = 0.1;
        if (ScaleFactor > 15.0) ScaleFactor = 15.0;

        FVector_UE5 Scale = { ScaleFactor, ScaleFactor, ScaleFactor };
        FVector_UE5 SpawnLocation = { 0.0, 0.0, 0.0 };

        uint8_t LocationType = 1; // Default to KeepWorldPosition (1) [5]

        if (SocketName == L"None" || SocketName.empty()) {
            FVector_UE5 ActorLoc;
            Utils::CallFunction(PalActor, STR("K2_GetActorLocation"), &ActorLoc);
            
            SpawnLocation.X = ActorLoc.X;
            SpawnLocation.Y = ActorLoc.Y;
            // BoundsParams.Origin.Z is the exact visual center of the Pal.
            // ZOffsetMult of 0.0 = visual center. 1.0 = top of head. -1.0 = feet.
            SpawnLocation.Z = BoundsParams.Origin.Z + (BoundsParams.BoxExtent.Z * ZOffsetMult);
        } else {
            LocationType = 0; // Fallback to KeepRelativeOffset (0) for custom skeletal socket attachments [1]
        }

        bool bIsNiagara = (VfxPath.find(L"/NS_") != std::wstring::npos);
        std::wstring LibraryPath = bIsNiagara ? L"/Script/Niagara.Default__NiagaraFunctionLibrary" : L"/Script/Engine.Default__GameplayStatics";
        std::wstring FuncName = bIsNiagara ? L"SpawnSystemAttached" : L"SpawnEmitterAttached";
        std::wstring TemplateName = bIsNiagara ? L"SystemTemplate" : L"EmitterTemplate";

        UObject* Lib = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, LibraryPath.c_str());
        if (!Lib) return nullptr;

        UFunction* SpawnFunc = Lib->GetFunctionByNameInChain(FuncName.c_str());
        if (!SpawnFunc) return nullptr;

        alignas(8) uint8_t Params[256] = {0};
        
        auto FillProp = [&](const wchar_t* Name, auto Value) {
            FProperty* Prop = SpawnFunc->GetPropertyByNameInChain(Name);
            if (Prop) *Prop->ContainerPtrToValuePtr<decltype(Value)>(Params) = Value;
        };

        FillProp(TemplateName.c_str(), VFXAsset);
        FillProp(STR("AttachToComponent"), AttachTarget);
        FillProp(STR("AttachPointName"), FName(SocketName.c_str(), FNAME_Add));
        FillProp(STR("Location"), SpawnLocation);
        FillProp(STR("Scale"), Scale);

        FProperty* LocationTypeProp = SpawnFunc->GetPropertyByNameInChain(STR("LocationType"));
        if (LocationTypeProp) {
            *LocationTypeProp->ContainerPtrToValuePtr<uint8_t>(Params) = LocationType;
        }

        FProperty* AutoDestroyProp = SpawnFunc->GetPropertyByNameInChain(STR("bAutoDestroy"));
        if (AutoDestroyProp && AutoDestroyProp->GetClass().GetName() == L"BoolProperty") {
            static_cast<FBoolProperty*>(AutoDestroyProp)->SetPropertyValue(AutoDestroyProp->ContainerPtrToValuePtr<void>(Params), true);
        }

        FProperty* AutoActProp = SpawnFunc->GetPropertyByNameInChain(bIsNiagara ? STR("bAutoActivate") : STR("bAutoActivateSystem"));
        if (AutoActProp && AutoActProp->GetClass().GetName() == L"BoolProperty") {
            static_cast<FBoolProperty*>(AutoActProp)->SetPropertyValue(AutoActProp->ContainerPtrToValuePtr<void>(Params), true);
        }

        Lib->ProcessEvent(SpawnFunc, Params);
        
        FProperty* RetProp = SpawnFunc->GetPropertyByNameInChain(STR("ReturnValue"));
        return RetProp ? *RetProp->ContainerPtrToValuePtr<UObject*>(Params) : nullptr;
    }
}