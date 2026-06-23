#pragma once
#include <string>
#include <fstream>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp> 
#include <Unreal/FString.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp> 
#include <Unreal/Core/Containers/Array.hpp>
#include <Unreal/FText.hpp> 
#include "DataTypes.hpp"

namespace DynPals::Utils {
    using namespace RC::Unreal;

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

    // SAFE TEMPLATE SPECIALIZATION: Resolves Unreal bitmasks for correct boolean evaluations
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

    // SAFE TEMPLATE SPECIALIZATION: Prevents setting values from corrupting adjacent bitfields
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

    // Internal raw load routine
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

        // USER LOG REQUEST: Real-time debug logging of FName paths passed to the engine loader
        //DP_LOG(Normal, "[DEBUG] Attempting to load Package: '{}' | Asset: '{}'\n", package, asset);

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
                    //DP_LOG(Normal, "[DEBUG] Resolved Redirector to target: '{}'\n", Destination->GetName());
                    return Destination;
                }
            }
        }
        return LoadedObj;
    }

    // High-level asset loader with standard, clean execution (no fallbacks or logs spam)
    inline UObject* LoadAssetSafely(const std::wstring& AssetPath) {
        return LoadAssetInternal(AssetPath);
    }

    // Specialized mod skeletal mesh loader with safe "SK_" / "sk_" prefix fallbacks [2]
    inline UObject* LoadSkeletalMeshSafely(const std::wstring& AssetPath) {
        UObject* Loaded = LoadAssetInternal(AssetPath);
        if (Loaded) return Loaded;

        // Fallback: Check if a mod-specific swapped mesh is missing its standard prefix [2]
        if (AssetPath.find(L"/Mods/") != std::wstring::npos) {
            size_t lastSlash = AssetPath.find_last_of(L'/');
            if (lastSlash != std::wstring::npos) {
                std::wstring directory = AssetPath.substr(0, lastSlash + 1);
                std::wstring filename = AssetPath.substr(lastSlash + 1);

                if (filename.rfind(L"SK_", 0) != 0 && filename.rfind(L"sk_", 0) != 0) {
                    // Try upper-case SK_ [2]
                    std::wstring fallback = directory + L"SK_" + filename;
                    Loaded = LoadAssetInternal(fallback);
                    if (Loaded) {
                        DP_LOG(Normal, "[DynPals] Found skeletal mesh with 'SK_' fallback path: {}\n", fallback);
                        return Loaded;
                    }

                    // Try lower-case sk_ [2]
                    fallback = directory + L"sk_" + filename;
                    Loaded = LoadAssetInternal(fallback);
                    if (Loaded) {
                        DP_LOG(Normal, "[DynPals] Found skeletal mesh with 'sk_' fallback path: {}\n", fallback);
                        return Loaded;
                    }
                }
            }
        }
        return nullptr;
    }
}