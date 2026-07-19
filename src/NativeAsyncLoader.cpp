// --- START OF FILE src/NativeAsyncLoader.cpp ---
#include "NativeAsyncLoader.hpp"
#include "Utils.hpp"
#include "DataTypes.hpp"
#include "PalProcessor.hpp"
#include <new> 
#include <set>

using namespace RC::Unreal;

namespace DynPals {

    static UClass* LoaderClass = nullptr;
    static UObject* GAssetLoaderActor = nullptr;
    static std::set<std::wstring> GRequestedAssets;

    void NativeAsyncLoader::ClearCache() {
        GRequestedAssets.clear();
    }

    bool NativeAsyncLoader::HasBeenRequested(const std::wstring& AssetPath) {
        return GRequestedAssets.find(AssetPath) != GRequestedAssets.end();
    }

    void NativeAsyncLoader::Initialize() {
        if (LoaderClass && Utils::IsObjectValid(LoaderClass)) return;

        DP_LOG(Default, "[NativeAsync] Initializing Reflected Latent Async Loader...");

        LoaderClass = static_cast<UClass*>(Utils::LoadAssetSafely(STR("/Game/Mods/DynamicPals/ModActor.ModActor_C")));
        
        if (LoaderClass && Utils::IsObjectValid(LoaderClass)) {
            DP_LOG(Default, "[NativeAsync] ModActor class successfully resolved.");
        } else {
            static bool bWarned = false;
            if (!bWarned) {
                DP_LOG(Warning, "ModActor.pak not found. Async loading disabled; using synchronous fallback.");
                bWarned = true;
            }
        }
    }

    bool NativeAsyncLoader::RequestAsyncLoad(const std::wstring& AssetPath, UObject* Requester) {
        if (!LoaderClass || !Utils::IsObjectValid(LoaderClass)) {
            LoaderClass = Utils::GetClassCached(STR("/Game/Mods/DynamicPals/ModActor.ModActor_C"), true);
            if (!LoaderClass || !Utils::IsObjectValid(LoaderClass)) return false;
        }

        if (!GAssetLoaderActor || !Utils::IsObjectValid(GAssetLoaderActor)) {
            GAssetLoaderActor = UObjectGlobals::FindFirstOf(STR("ModActor_C"));
        }

        if (GAssetLoaderActor && Utils::IsObjectValid(GAssetLoaderActor)) {
            static bool bHooked = false;
            if (!bHooked) {
                UFunction* OnLoadedCallback = GAssetLoaderActor->GetFunctionByNameInChain(STR("OnAssetLoadedDispatcher__DelegateSignature"));
                if (OnLoadedCallback) {
                    OnLoadedCallback->RegisterPreHook([](UnrealScriptFunctionCallableContext& Context, void*) {
                        UFunction* Func = Context.TheStack.Node();
                        if (!Func) return;

                        UObject* RequesterObj = nullptr;
                        FProperty* RequesterProp = Func->GetPropertyByNameInChain(STR("Requester"));
                        
                        if (RequesterProp) {
                            UObject** Ptr = RequesterProp->ContainerPtrToValuePtr<UObject*>(Context.TheStack.Locals());
                            if (Ptr) RequesterObj = *Ptr;
                        }

                        if (RequesterObj && Utils::IsObjectValid(RequesterObj)) {
                            PalProcessor::Get().ProcessPal(RequesterObj, false, -1);
                        }
                    }, nullptr);
                    bHooked = true;
                    DP_LOG(Default, "[NativeAsync] Successfully registered dynamic pre-hook on OnAssetLoadedDispatcher!");
                }
            }

            UFunction* Func = GAssetLoaderActor->GetFunctionByNameInChain(STR("RequestAsyncLoad"));
            if (Func) {
                GRequestedAssets.insert(AssetPath);
                std::wstring formattedPath = Utils::FormatAssetPath(AssetPath);

                if (formattedPath.length() > 2 && formattedPath.substr(formattedPath.length() - 2) == L"_C") {
                    formattedPath = formattedPath.substr(0, formattedPath.length() - 2);
                }

                alignas(8) uint8_t BPParams[256] = {0};
                FString* AssetPathPtr = nullptr;

                FProperty* AssetPathProp = Func->GetPropertyByNameInChain(STR("AssetPath"));
                if (AssetPathProp) {
                    AssetPathPtr = AssetPathProp->ContainerPtrToValuePtr<FString>(BPParams);
                    if (AssetPathPtr) {
                        new (AssetPathPtr) FString(formattedPath.c_str()); 
                    }
                }

                FProperty* RequesterProp = Func->GetPropertyByNameInChain(STR("Requester"));
                if (RequesterProp) {
                    UObject** Ptr = RequesterProp->ContainerPtrToValuePtr<UObject*>(BPParams);
                    if (Ptr) *Ptr = Requester;
                }

                Utils::SafeProcessEvent(GAssetLoaderActor, Func, BPParams);

                if (AssetPathPtr) {
                    AssetPathPtr->~FString();
                }
                
                return true;
            }
        }
        return false;
    }
}
// --- END OF FILE src/NativeAsyncLoader.cpp ---