// --- START OF FILE src/NativeAsyncLoader.cpp ---
#include "NativeAsyncLoader.hpp"
#include "AsyncHelper.hpp"
#include "Utils.hpp"
#include <vector>

using namespace RC::Unreal;

namespace DynPals {

    // Struct matching Unreal's FOnAssetLoaded dynamic delegate
    struct FOnAssetLoaded {
        UObject* Object = nullptr;
        FName FunctionName;
    };

    // Struct matching Unreal's FLatentActionInfo
    struct FLatentActionInfo {
        int32_t LinkID = -1;
        int32_t UUID = -1;
        FName ExecutionFunction;
        UObject* CallbackTarget = nullptr;
    };

    void NativeAsyncLoader::Initialize() {
        DP_LOG(Default, "[NativeAsync] Initializing Reflected Latent Async Loader...");
        
        UObject* KSL = Utils::GetKSL();
        if (KSL) {
            UFunction* LoadAssetFunc = KSL->GetFunctionByNameInChain(STR("LoadAsset"));
            if (LoadAssetFunc) {
                DP_LOG(Default, "[NativeAsync] Successfully resolved UKismetSystemLibrary::LoadAsset reflection pipeline. Stutter-free loading is active!");
                return;
            }
        }
        DP_LOG(Error, "[NativeAsync] CRITICAL: Failed to resolve UKismetSystemLibrary::LoadAsset function!");
    }

    void NativeAsyncLoader::RequestAsyncLoad(const std::wstring& AssetPath, UObject* WorldContext) {
        UObject* KSL = Utils::GetKSL();
        if (!KSL || !Utils::IsObjectValid(KSL) || !WorldContext) {
            // Failsafe: Standard blocking load
            Utils::LoadAssetSafely(AssetPath);
            return;
        }

        UFunction* LoadAssetFunc = KSL->GetFunctionByNameInChain(STR("LoadAsset"));
        if (!LoadAssetFunc) {
            Utils::LoadAssetSafely(AssetPath);
            return;
        }

        std::wstring formatted = Utils::FormatAssetPath(AssetPath);
        std::wstring package, asset;
        size_t dot = formatted.find(L'.');
        if (dot != std::wstring::npos) {
            package = formatted.substr(0, dot);
            asset = formatted.substr(dot + 1);
        } else {
            return;
        }

        // In Unreal's reflection system, TSoftObjectPtr parameters inside UFunctions
        // are stored as AltrSoftObjectPtr (48 bytes), which wraps FSoftObjectPath.
        AltrSoftObjectPtr TargetPtr;
        TargetPtr.WeakPtr.ObjectIndex = 0;
        TargetPtr.WeakPtr.ObjectSerialNumber = 0;
        TargetPtr.TagAtLastTest = 0;
        TargetPtr.Padding = 0;
        TargetPtr.ObjectID.PackageName = FName(package.c_str(), FNAME_Add);
        TargetPtr.ObjectID.AssetName = FName(asset.c_str(), FNAME_Add);
        TargetPtr.ObjectID.SubPathString = FString(STR(""));

        // Generate a unique UUID for this latent action so the LatentActionManager tracks it uniquely
        static int32_t GAsyncLoadUUID = 10000;
        int32_t CurrentUUID = GAsyncLoadUUID++;

        // Locate the active player controller to act as our ticking driver.
        // Standard NPCs (like PalCharacter) do not natively tick latent actions in C++,
        // but playable PlayerControllers are guaranteed to process them every frame.
        UObject* TickingTarget = UObjectGlobals::FindFirstOf(STR("PalPlayerController"));
        if (!TickingTarget) {
            TickingTarget = WorldContext; // Fallback to character if PC is not yet loaded
        }

        // Setup the parameters matching: 
        // static void LoadAsset(const UObject* WorldContextObject, TSoftObjectPtr<UObject> Asset, FOnAssetLoaded OnLoaded, FLatentActionInfo LatentInfo);
        struct {
            UObject* WorldContextObject;
            AltrSoftObjectPtr Asset; // 48 bytes (Perfectly aligned!)
            FOnAssetLoaded OnLoaded;  // 16 bytes
            FLatentActionInfo LatentInfo; // 24 bytes
        } Params;
        Params.WorldContextObject = TickingTarget;
        Params.Asset = TargetPtr;
        
        // Setup LatentInfo with a valid, ticking CallbackTarget and unique UUID
        Params.LatentInfo.LinkID = 0;
        Params.LatentInfo.UUID = CurrentUUID;
        Params.LatentInfo.ExecutionFunction = FName(); // NAME_None
        Params.LatentInfo.CallbackTarget = TickingTarget; // PlayerController drives the latent execution

        DP_LOG(Default, "[NativeAsync] Requesting async load for: '{}' (UUID: {})", AssetPath, CurrentUUID);

        // Process the latent async load event safely
        KSL->ProcessEvent(LoadAssetFunc, &Params);
    }
}
// --- END OF FILE src/NativeAsyncLoader.cpp ---