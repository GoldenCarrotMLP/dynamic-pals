#pragma once
#include <string>
#include <fstream>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp> 
#include <Unreal/FString.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/Core/Containers/Array.hpp>
#include "DataTypes.hpp"

namespace DynPals {
    struct DynPalsGuid {
        uint32_t A, B, C, D;
    };
}

namespace DynPals::Utils {
    using namespace RC::Unreal;

    inline std::wstring StringToWString(const std::string& str) {
        return std::wstring(str.begin(), str.end());
    }

    inline std::string WStringToString(const std::wstring& wstr) {
        return std::string(wstr.begin(), wstr.end());
    }

    inline std::wstring FStringToWString(const FString& fstr) {
        const TCHAR* data = fstr.GetCharArray().GetData();
        return data ? std::wstring(data) : L"";
    }

    // Swapped to our custom struct to prevent header conflicts
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

    inline UObject* LoadAssetSafely(const std::wstring& AssetPath) {
        std::wstring formatted = FormatAssetPath(AssetPath);
        std::wstring package, asset;
        
        size_t dot = formatted.find(L'.');
        if (dot != std::wstring::npos) {
            package = formatted.substr(0, dot);
            asset = formatted.substr(dot + 1);
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
        return LoadParams.ReturnValue;
    }
}