#include "VFXManager.hpp"
#include "Utils.hpp"
#include "AsyncHelper.hpp"
#include "NotificationManager.hpp"
#include <sstream>

using namespace RC::Unreal;

namespace DynPals {

    // Unique local representation to bypass UE4SS incomplete header structure definitions
    struct VFXLinearColor {
        float R;
        float G;
        float B;
        float A;
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

    void VFXManager::KillCurrentPreview() {
        if (ActivePreviewComponent) {
            UFunction* DestroyFunc = ActivePreviewComponent->GetFunctionByNameInChain(STR("DestroyComponent"));
            if (DestroyFunc) {
                alignas(8) uint8_t Params[16] = {0}; 
                ActivePreviewComponent->ProcessEvent(DestroyFunc, Params);
            }
            ActivePreviewComponent = nullptr;
        }
    }

    void VFXManager::Initialize() {
        UObject* KismetLib = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
        if (!KismetLib) return;

        FString ContentDir;
        Utils::CallFunction(KismetLib, STR("GetProjectContentDirectory"), &ContentDir);
        std::wstring BasePath = Utils::FStringToWString(ContentDir);
        
        std::wstring ListPath = BasePath + L"Paks/~mods/vfx_list.txt";

        std::string fileContent = Utils::ReadFileToString(ListPath);
        if (fileContent.empty()) {
            DP_LOG(Warning, "VFXManager: vfx_list.txt not found or empty at: '{}'. VFX Viewer standby.", ListPath);
            return;
        }

        std::istringstream stream(fileContent);
        std::string line;
        while (std::getline(stream, line)) {
            line.erase(0, line.find_first_not_of(" \t\r\n"));
            size_t last = line.find_last_not_of(" \t\r\n");
            if (last != std::string::npos) {
                line.erase(last + 1);
            }
            if (!line.empty()) {
                VFXList.push_back(Utils::StringToWString(line));
            }
        }
        DP_LOG(Normal, "VFXManager initialized with {} effects from vfx_list.txt.", VFXList.size());
    }

    void VFXManager::SpawnCurrentPreview() {
        if (VFXList.empty()) {
            DP_LOG(Warning, "VFXManager: Spawn cancelled. List is empty.");
            return;
        }
        
        KillCurrentPreview();

        UObject* PlayerController = UObjectGlobals::FindFirstOf(STR("PalPlayerController"));
        if (!PlayerController) {
            DP_LOG(Warning, "VFXManager: Spawn cancelled. PalPlayerController not found.");
            return;
        }

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
            DP_LOG(Warning, "VFXManager: Failed to load asset path: '{}'", TargetPath);
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
                    DP_LOG(Normal, "Successfully spawned VFX: '{}'", TargetPath);

                    // ==========================================
                    // ZERO LAG FIX: Draw the debug box exactly ONCE with a 10 second duration.
                    // It will stay in the world natively without needing to be ticked every frame!
                    // ==========================================
                    VFXLinearColor GreenColor = {0.0f, 1.0f, 0.0f, 1.0f};
                    FVector_UE5 Extent = {50.0, 50.0, 50.0}; 
                    DrawDebugBoxDirect(PlayerController, SpawnLoc, Extent, GreenColor, 10.0f, 2.0f);

                } else {
                    DP_LOG(Warning, "VFXManager: Spawned function did not return a valid component.");
                }
            } else {
                DP_LOG(Warning, "VFXManager: Function '{}' not found on library.", FuncName);
            }
        } else {
            DP_LOG(Warning, "VFXManager: Library '{}' not found.", LibraryPath);
        }

        std::wstring BaseName = TargetPath;
        size_t LastSlash = BaseName.find_last_of(L'/');
        if (LastSlash != std::wstring::npos) BaseName = BaseName.substr(LastSlash + 1);
        
        //NotificationManager::Get().EnqueueToast(L"Previewing: " + BaseName, EPalLogPriority::Normal, EPalLogContentToneType::Positive);
    }

    void VFXManager::CycleNext() {
        if (VFXList.empty()) {
            DP_LOG(Warning, "VFXManager: CycleNext cancelled. vfx_list.txt is empty.");
            return;
        }
        CurrentIndex = (CurrentIndex + 1) % VFXList.size();
        //DP_LOG(Normal, "VFX Cycle Next! Index: {}/{} - Loading: {}", CurrentIndex + 1, VFXList.size(), VFXList[CurrentIndex]);

        AsyncHelper::AsyncTask(ENamedThreads::GameThread, [this]() {
            SpawnCurrentPreview();
        });
    }

    void VFXManager::CyclePrevious() {
        if (VFXList.empty()) {
            DP_LOG(Warning, "VFXManager: CyclePrevious cancelled. vfx_list.txt is empty.");
            return;
        }
        CurrentIndex = (CurrentIndex - 1 + VFXList.size()) % VFXList.size();
        //DP_LOG(Normal, "VFX Cycle Previous! Index: {}/{} - Loading: {}", CurrentIndex + 1, VFXList.size(), VFXList[CurrentIndex]);

        AsyncHelper::AsyncTask(ENamedThreads::GameThread, [this]() {
            SpawnCurrentPreview();
        });
    }

    void VFXManager::Tick() {
        // Since we removed the Heavy DrawBox logic, Tick is back to being 100% lightweight.
        // It only checks if 10 seconds have passed to kill the particle component.
        if (ActivePreviewComponent) {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - SpawnTime).count() >= 10) {
                KillCurrentPreview();
                return;
            }
        }
    }

    UObject* VFXManager::AttachVFXToPal(UObject* PalActor, const std::wstring& VfxPath, const std::wstring& SocketName) {
        if (!PalActor) return nullptr;
        UObject* MeshComp = nullptr;
        Utils::CallFunction(PalActor, STR("GetMainMesh"), &MeshComp);
        if (!MeshComp) return nullptr;

        UObject* VFXAsset = Utils::LoadAssetSafely(VfxPath);
        if (!VFXAsset) return nullptr;

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
        FillProp(STR("AttachToComponent"), MeshComp);
        FillProp(STR("AttachPointName"), FName(SocketName.c_str(), FNAME_Add));

        FProperty* AutoActProp = SpawnFunc->GetPropertyByNameInChain(bIsNiagara ? STR("bAutoActivate") : STR("bAutoActivateSystem"));
        if (AutoActProp && AutoActProp->GetClass().GetName() == L"BoolProperty") {
            static_cast<FBoolProperty*>(AutoActProp)->SetPropertyValue(AutoActProp->ContainerPtrToValuePtr<void>(Params), true);
        }

        Lib->ProcessEvent(SpawnFunc, Params);
        
        FProperty* RetProp = SpawnFunc->GetPropertyByNameInChain(STR("ReturnValue"));
        return RetProp ? *RetProp->ContainerPtrToValuePtr<UObject*>(Params) : nullptr;
    }
}