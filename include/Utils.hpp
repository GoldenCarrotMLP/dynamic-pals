#pragma once
#define NOMINMAX
#include <Windows.h>
#include "DataTypes.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/Core/Containers/Array.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp> 
#include <Unreal/FString.hpp>
#include <Unreal/FText.hpp> 
#include <string>
#include <vector>
#include <map>
#include <fstream>

namespace DynPals::Utils {
    using namespace RC::Unreal;

    inline FField* GetNextField(FField* Field) {
        if (!Field) return nullptr;
        return *reinterpret_cast<FField**>(reinterpret_cast<uint8_t*>(Field) + 0x20);
    }

    inline std::wstring StringToWString(const std::string& str) {
        std::wstring wstr;
        wstr.reserve(str.size());
        for (char c : str) {
            wstr.push_back(static_cast<wchar_t>(c));
        }
        return wstr;
    }

    inline std::string WStringToString(const std::wstring& wstr) {
        std::string str;
        str.reserve(wstr.size());
        for (wchar_t wc : wstr) {
            str.push_back(static_cast<char>(wc));
        }
        return str;
    }

    inline std::wstring FStringToWString(const FString& fstr) {
        const TCHAR* data = fstr.GetCharArray().GetData();
        return data ? std::wstring(data) : L"";
    }

    inline std::wstring GuidToWString(const DynPalsGuid& Guid) {
        wchar_t buf[64];
        swprintf(buf, 64, L"%08X%08X%08X%08X", Guid.A, Guid.B, Guid.C, Guid.D);
        return std::wstring(buf);
    }

    inline FProperty* GetProperty(UObject* Object, const wchar_t* PropertyName) {
        if (!Object) return nullptr;
        auto* Class = Object->GetClassPrivate();
        return Class ? Class->GetPropertyByNameInChain(PropertyName) : nullptr;
    }

    template<typename T>
    inline bool GetPropertyValue(UObject* Object, const wchar_t* PropertyName, T& OutValue) {
        auto* Property = GetProperty(Object, PropertyName);
        if (Property) {
            T* Ptr = Property->ContainerPtrToValuePtr<T>(Object);
            if (Ptr) {
                OutValue = *Ptr;
                return true;
            }
        }
        return false;
    }

    template<>
    inline bool GetPropertyValue<bool>(UObject* Object, const wchar_t* PropertyName, bool& OutValue) {
        auto* Property = GetProperty(Object, PropertyName);
        if (Property) {
            if (Property->GetClass().GetName() == L"BoolProperty") {
                FBoolProperty* BoolProp = static_cast<FBoolProperty*>(Property);
                void* Ptr = BoolProp->ContainerPtrToValuePtr<void>(Object);
                if (Ptr) {
                    OutValue = BoolProp->GetPropertyValue(Ptr);
                    return true;
                }
            }
        }
        return false;
    }

    template<typename T>
    inline bool SetPropertyValue(UObject* Object, const wchar_t* PropertyName, const T& Value) {
        auto* Property = GetProperty(Object, PropertyName);
        if (Property) {
            T* Ptr = Property->ContainerPtrToValuePtr<T>(Object);
            if (Ptr) {
                *Ptr = Value;
                return true;
            }
        }
        return false;
    }

    template<>
    inline bool SetPropertyValue<bool>(UObject* Object, const wchar_t* PropertyName, const bool& Value) {
        auto* Property = GetProperty(Object, PropertyName);
        if (Property) {
            if (Property->GetClass().GetName() == L"BoolProperty") {
                FBoolProperty* BoolProp = static_cast<FBoolProperty*>(Property);
                void* Ptr = BoolProp->ContainerPtrToValuePtr<void>(Object);
                if (Ptr) {
                    BoolProp->SetPropertyValue(Ptr, Value);
                    return true;
                }
            }
        }
        return false;
    }

    inline void CallFunction(UObject* Object, const wchar_t* FunctionName, void* Params = nullptr) {
        if (!Object) return;
        auto* Function = Object->GetFunctionByNameInChain(FunctionName);
        if (Function) Object->ProcessEvent(Function, Params);
    }

    inline std::wstring FormatAssetPath(const std::wstring& Path) {
        if (Path.empty() || Path.find(L'.') != std::wstring::npos) return Path;
        size_t lastSlash = Path.find_last_of(L'/');
        if (lastSlash != std::wstring::npos) {
            return Path + L"." + Path.substr(lastSlash + 1);
        }
        return Path;
    }

    inline std::string ReadFileToString(const std::wstring& path) {
        std::ifstream file(path);
        if (!file.is_open()) return "";
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    inline UObject* LoadAssetInternal(const std::wstring& AssetPath) {
        std::wstring formatted = FormatAssetPath(AssetPath); 
        std::wstring package, asset;
        
        size_t dot = formatted.find(L'.');
        if (dot != std::wstring::npos) {
            package = formatted.substr(0, dot);
            asset = formatted.substr(dot + 1);
        } else {
            return nullptr;
        }

        AltrSoftObjectPtr SoftPtr;
        SoftPtr.ObjectID.PackageName = FName(package.c_str(), FNAME_Add);
        SoftPtr.ObjectID.AssetName = FName(asset.c_str(), FNAME_Add);
        SoftPtr.ObjectID.SubPathString = FString(STR(""));

        UObject* KismetLib = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
        if (!KismetLib) return nullptr;

        UFunction* LoadFunc = KismetLib->GetFunctionByNameInChain(STR("LoadAsset_Blocking"));
        if (!LoadFunc) return nullptr;

        struct { AltrSoftObjectPtr Asset; UObject* ReturnValue; } LoadParams{SoftPtr, nullptr};
        KismetLib->ProcessEvent(LoadFunc, &LoadParams);
        
        UObject* LoadedObj = LoadParams.ReturnValue;
        if (LoadedObj) {
            std::wstring ClassName = LoadedObj->GetClassPrivate()->GetName();
            if (ClassName == L"ObjectRedirector") {
                UObject* Destination = nullptr;
                if (GetPropertyValue<UObject*>(LoadedObj, STR("DestinationObject"), Destination)) {
                    return Destination;
                }
            }
        }
        return LoadedObj;
    }

    inline UObject* LoadAssetSafely(const std::wstring& AssetPath) {
        return LoadAssetInternal(AssetPath);
    }

    inline UObject* LoadSkeletalMeshSafely(const std::wstring& AssetPath) {
        UObject* Loaded = LoadAssetInternal(AssetPath);
        if (Loaded) return Loaded;

        if (AssetPath.find(L"/Mods/") != std::wstring::npos) {
            size_t lastSlash = AssetPath.find_last_of(L'/');
            if (lastSlash != std::wstring::npos) {
                std::wstring directory = AssetPath.substr(0, lastSlash + 1);
                std::wstring filename = AssetPath.substr(lastSlash + 1);

                if (filename.rfind(L"SK_", 0) != 0 && filename.rfind(L"sk_", 0) != 0) {
                    std::wstring fallback = directory + L"SK_" + filename;
                    Loaded = LoadAssetInternal(fallback);
                    if (Loaded) {
                        return Loaded;
                    }

                    fallback = directory + L"sk_" + filename;
                    Loaded = LoadAssetInternal(fallback);
                    if (Loaded) {
                        return Loaded;
                    }
                }
            }
        }
        return nullptr;
    }

    // Safely reads the virtual folder hierarchy using static padded buffers and filters only Material types
    inline std::vector<std::wstring> GetAssetsInVirtualFolder(const std::wstring& FolderPath) {
        static std::map<std::wstring, std::vector<std::wstring>> FolderCache;
        if (FolderCache.find(FolderPath) != FolderCache.end()) return FolderCache[FolderPath];

        std::vector<std::wstring> Results;
        UObject* ARH = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/AssetRegistry.Default__AssetRegistryHelpers"));
        if (!ARH) {
            DP_LOG(Error, "[Asset Scanner] Failed: AssetRegistryHelpers CDO not found.");
            return Results;
        }

        struct { UObject* ReturnValue; } GetARParams{nullptr};
        CallFunction(ARH, STR("GetAssetRegistry"), &GetARParams);
        UObject* AssetRegistry = GetARParams.ReturnValue;
        if (!AssetRegistry) {
            DP_LOG(Error, "[Asset Scanner] Failed: AssetRegistry instance not found.");
            return Results;
        }

        DP_LOG(Normal, "[Asset Scanner] Initializing search for folder: '{}'", FolderPath);

        // 1. Force the Engine to parse the .pak file for this specific folder before we query it
        UFunction* ScanFunc = AssetRegistry->GetFunctionByNameInChain(STR("ScanPathsSynchronous"));
        if (ScanFunc) {
            alignas(8) uint8_t ScanBuffer[512] = {0}; // Zero-initialized memory replaces InitializeValue
            
            FArrayProperty* InPathsProp = CastField<FArrayProperty>(ScanFunc->GetPropertyByNameInChain(STR("InPaths")));
            if (InPathsProp) {
                TArray<FString>* Arr = static_cast<TArray<FString>*>(InPathsProp->ContainerPtrToValuePtr<void>(ScanBuffer));
                if (Arr) {
                    FString Str(FolderPath.c_str());
                    Arr->Add(Str);
                }
            }
            
            FBoolProperty* ForceRescanProp = CastField<FBoolProperty>(ScanFunc->GetPropertyByNameInChain(STR("bForceRescan")));
            if (ForceRescanProp) ForceRescanProp->SetPropertyValue(ForceRescanProp->ContainerPtrToValuePtr<void>(ScanBuffer), true);
            
            DP_LOG(Normal, "[Asset Scanner] Forcing synchronous rescan of folder to register modded .pak assets...");
            AssetRegistry->ProcessEvent(ScanFunc, ScanBuffer);
        } else {
            DP_LOG(Warning, "[Asset Scanner] ScanPathsSynchronous function missing. Unregistered .pak assets may not be found.");
        }

        // 2. Query the Registry for the contents of the folder
        UFunction* GetAssetsFunc = AssetRegistry->GetFunctionByNameInChain(STR("GetAssetsByPath"));
        if (!GetAssetsFunc) {
            DP_LOG(Error, "[Asset Scanner] Failed: GetAssetsByPath function not found.");
            return Results;
        }

        alignas(8) uint8_t ParamsBuffer[512] = {0}; // Zero-initialized memory replaces InitializeValue

        FProperty* PackagePathProp = GetAssetsFunc->GetPropertyByNameInChain(STR("PackagePath"));
        if (PackagePathProp) {
            FName* Dest = static_cast<FName*>(PackagePathProp->ContainerPtrToValuePtr<void>(ParamsBuffer));
            if (Dest) *Dest = FName(FolderPath.c_str(), FNAME_Add);
        }

        FBoolProperty* RecursiveProp = CastField<FBoolProperty>(GetAssetsFunc->GetPropertyByNameInChain(STR("bRecursive")));
        if (RecursiveProp) RecursiveProp->SetPropertyValue(RecursiveProp->ContainerPtrToValuePtr<void>(ParamsBuffer), false);

        FBoolProperty* OnDiskProp = CastField<FBoolProperty>(GetAssetsFunc->GetPropertyByNameInChain(STR("bIncludeOnlyOnDiskAssets")));
        if (OnDiskProp) OnDiskProp->SetPropertyValue(OnDiskProp->ContainerPtrToValuePtr<void>(ParamsBuffer), false);

        AssetRegistry->ProcessEvent(GetAssetsFunc, ParamsBuffer);

        // 3. Extract, filter, and log the results
        FArrayProperty* OutAssetDataProp = CastField<FArrayProperty>(GetAssetsFunc->GetPropertyByNameInChain(STR("OutAssetData")));
        if (OutAssetDataProp) {
            FScriptArray* ScriptArray = static_cast<FScriptArray*>(OutAssetDataProp->ContainerPtrToValuePtr<void>(ParamsBuffer));
            FProperty* InnerProp = OutAssetDataProp->GetInner();

            if (ScriptArray && InnerProp) {
                int32_t NumAssets = ScriptArray->Num();
                DP_LOG(Normal, "[Asset Scanner] Search complete. Discovered {} total assets in folder.", NumAssets);

                int32_t ElementSize = InnerProp->GetSize();
                uint8_t* ArrayData = static_cast<uint8_t*>(ScriptArray->GetData());

                UFunction* GetFullNameFunc = ARH->GetFunctionByNameInChain(STR("GetFullName"));
                
                for (int32_t i = 0; i < NumAssets; ++i) {
                    if (GetFullNameFunc) {
                        alignas(8) uint8_t FNParams[512] = {0}; // Zero-initialized memory
                        
                        FProperty* InAssetDataProp = GetFullNameFunc->GetPropertyByNameInChain(STR("InAssetData"));
                        if (InAssetDataProp && ArrayData) {
                            void* Dest = InAssetDataProp->ContainerPtrToValuePtr<void>(FNParams);
                            // Direct memcpy replaces CopyCompleteValue
                            if (Dest) memcpy(Dest, ArrayData + (i * ElementSize), InAssetDataProp->GetSize());
                        }

                        ARH->ProcessEvent(GetFullNameFunc, FNParams);

                        FProperty* ReturnProp = GetFullNameFunc->GetPropertyByNameInChain(STR("ReturnValue"));
                        if (ReturnProp) {
                            FString* RetStr = static_cast<FString*>(ReturnProp->ContainerPtrToValuePtr<void>(FNParams));
                            if (RetStr && RetStr->GetCharArray().GetData()) {
                                std::wstring FullName = RetStr->GetCharArray().GetData();
                                size_t SpacePos = FullName.find(L' ');
                                if (SpacePos != std::wstring::npos) {
                                    std::wstring ClassName = FullName.substr(0, SpacePos);
                                    std::wstring Path = FullName.substr(SpacePos + 1);

                                    // Strictly filter for Materials and MaterialInstances
                                    if (ClassName.find(L"Material") != std::wstring::npos) {
                                        Results.push_back(Path);
                                        DP_LOG(Normal, "  -> [Accepted Material] Class: '{}', Path: '{}'", ClassName, Path);
                                    } else {
                                        DP_LOG(Normal, "  -> [Rejected Asset] Class: '{}', Path: '{}'", ClassName, Path);
                                    }
                                } else {
                                    Results.push_back(FullName);
                                }
                            }
                        }
                    }
                }
            } else {
                DP_LOG(Warning, "[Asset Scanner] Warning: Output array was structurally invalid.");
            }
        }

        FolderCache[FolderPath] = Results;
        return Results;
    }
}