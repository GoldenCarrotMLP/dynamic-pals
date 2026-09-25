#pragma once
#define NOMINMAX
#define DP_PROFILE(Name, ThresholdMs) \
    DynPals::Utils::ScopedProfiler profiler_##__LINE__(Name, ThresholdMs)

#include <Windows.h>
#include "DataTypes.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/Core/Containers/Array.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp> 

#include <Unreal/FString.hpp>
#include <Unreal/FText.hpp> 
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <set>
#include <fstream>
#include <shared_mutex>
#include <chrono> 
#include "../include/NativeAsyncLoader.hpp"

namespace DynPals::Utils {

    struct ScopedProfiler {
        std::string Name;
        double ThresholdMs;
        std::chrono::high_resolution_clock::time_point Start;

        ScopedProfiler(std::string InName, double InThresholdMs = 1.0)
            : Name(std::move(InName)), ThresholdMs(InThresholdMs) {
            Start = std::chrono::high_resolution_clock::now();
        }

        ~ScopedProfiler() {
            auto end = std::chrono::high_resolution_clock::now();
            double ms = std::chrono::duration<double, std::milli>(end - Start).count();
            if (ms >= ThresholdMs) {
                std::wstring wName(Name.begin(), Name.end());
                RC::Output::send<RC::LogLevel::Warning>(STR("[Profiler] {} took {:.3f} ms\n"), wName, ms);
            }
        }
    };

    using namespace RC::Unreal;

    inline UObject* GetKismetSystemLibrary();
    inline UFunction* GetKismetFunction(const wchar_t* FunctionName);

    inline void SafeProcessEvent(RC::Unreal::UObject* Obj, RC::Unreal::UFunction* Func, void* Params) {
        if (!Obj || !Func) return;
        __try {
            Obj->ProcessEvent(Func, Params);
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {}
    }

    namespace Caches {
        struct CacheKey {
            UClass* Cls;
            std::wstring Name; // <--- CRITICAL FIX: Retained String Memory
            bool operator<(const CacheKey& o) const {
                if (Cls != o.Cls) return Cls < o.Cls;
                return Name < o.Name;
            }
        };
        struct Key {
            std::wstring Path;
            std::wstring Name;
            bool operator<(const Key& Other) const {
                if (Path != Other.Path) return Path < Other.Path;
                return Name < Other.Name;
            }
        };

        inline std::map<CacheKey, FProperty*> PropCache;
        inline std::shared_mutex PropMutex;
        inline std::map<CacheKey, UFunction*> FuncCache;
        inline std::shared_mutex FuncMutex;
        inline std::map<std::wstring, UObject*> LibraryCache;
        inline std::shared_mutex LibraryMutex;
        inline std::map<Key, UFunction*> LibFuncCache;
        inline std::shared_mutex LibFuncMutex;
        inline std::map<std::wstring, UClass*> ClassCache;
        inline std::shared_mutex ClassMutex;
        inline std::map<std::wstring, UFunction*> KismetFuncCache;
        inline std::shared_mutex KismetFuncMutex;
        inline std::map<std::wstring, std::vector<std::wstring>> FolderCache;
        inline std::shared_mutex FolderMutex;
        
        inline std::set<std::wstring> ScannedFolders;
        inline std::shared_mutex ScannedFoldersMutex;

        inline void ClearAll() {
            std::unique_lock<std::shared_mutex> lock1(PropMutex);
            std::unique_lock<std::shared_mutex> lock2(FuncMutex);
            std::unique_lock<std::shared_mutex> lock4(LibraryMutex);
            std::unique_lock<std::shared_mutex> lock5(LibFuncMutex);
            std::unique_lock<std::shared_mutex> lock6(ClassMutex);
            std::unique_lock<std::shared_mutex> lock7(KismetFuncMutex);
            std::unique_lock<std::shared_mutex> lock8(FolderMutex);
            std::unique_lock<std::shared_mutex> lock9(ScannedFoldersMutex);

            PropCache.clear(); FuncCache.clear(); LibraryCache.clear();
            LibFuncCache.clear(); ClassCache.clear(); KismetFuncCache.clear(); FolderCache.clear();
            ScannedFolders.clear();
        }
    }

    inline UObject* GetLibrary(const wchar_t* LibraryPath) {
        {
            std::shared_lock<std::shared_mutex> read_lock(Caches::LibraryMutex);
            if (Caches::LibraryCache.count(LibraryPath)) return Caches::LibraryCache[LibraryPath];
        }
        UObject* Lib = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, LibraryPath);
        std::unique_lock<std::shared_mutex> write_lock(Caches::LibraryMutex);
        return Caches::LibraryCache[LibraryPath] = Lib;
    }

    inline UFunction* GetLibraryFunction(const wchar_t* LibraryPath, const wchar_t* FunctionName) {
        Caches::Key k{ LibraryPath, FunctionName };
        {
            std::shared_lock<std::shared_mutex> read_lock(Caches::LibFuncMutex);
            if (Caches::LibFuncCache.count(k)) return Caches::LibFuncCache[k];
        }
        UObject* Lib = GetLibrary(LibraryPath);
        UFunction* Func = Lib ? Lib->GetFunctionByNameInChain(FunctionName) : nullptr;
        std::unique_lock<std::shared_mutex> write_lock(Caches::LibFuncMutex);
        return Caches::LibFuncCache[k] = Func;
    }

    inline UClass* GetClassCached(const wchar_t* ClassPath, bool bSilenceLogs = false) {
        {
            std::shared_lock<std::shared_mutex> read_lock(Caches::ClassMutex);
            if (Caches::ClassCache.count(ClassPath)) return Caches::ClassCache[ClassPath];
        }
        UClass* Cls = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, ClassPath);
        std::unique_lock<std::shared_mutex> write_lock(Caches::ClassMutex);
        return Caches::ClassCache[ClassPath] = Cls;
    }

    inline UObject* GetKismetSystemLibrary() { return GetLibrary(STR("/Script/Engine.Default__KismetSystemLibrary")); }

    inline UFunction* GetKismetFunction(const wchar_t* FunctionName) {
        {
            std::shared_lock<std::shared_mutex> read_lock(Caches::KismetFuncMutex);
            if (Caches::KismetFuncCache.count(FunctionName)) return Caches::KismetFuncCache[FunctionName];
        }
        UObject* KSL = GetKismetSystemLibrary();
        UFunction* Func = KSL ? KSL->GetFunctionByNameInChain(FunctionName) : nullptr;
        std::unique_lock<std::shared_mutex> write_lock(Caches::KismetFuncMutex);
        return Caches::KismetFuncCache[FunctionName] = Func;
    }

    inline bool IsObjectValid(UObject* Obj) {
        if (!Obj) return false;
        uint32_t flags = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(Obj) + 0x8);
        if (flags & (0x00008000 | 0x00010000 | 0x08000000)) return false;
        return true;
    }

    inline bool IsObjectTracked(UObject* TargetObj) {
        return IsObjectValid(TargetObj); 
    }

    inline FField* GetNextField(FField* Field) {
        if (!Field) return nullptr;
        return *reinterpret_cast<FField**>(reinterpret_cast<uint8_t*>(Field) + 0x20);
    }

    inline std::wstring GenerateFallbackLabel(const std::wstring& SkelMeshPath, const std::vector<MatReplace>& MatReplaceList, const std::vector<MorphTarget>& MorphTargetList) {
        std::wstring meshName = SkelMeshPath;
        size_t lastSlash = meshName.find_last_of(L'/');
        if (lastSlash != std::wstring::npos) meshName = meshName.substr(lastSlash + 1);
        size_t lastDot = meshName.find_last_of(L'.');
        if (lastDot != std::wstring::npos) meshName = meshName.substr(0, lastDot);

        size_t hashVal = std::hash<std::wstring>{}(SkelMeshPath);
        for (const auto& mat : MatReplaceList) {
            hashVal ^= std::hash<std::string>{}(mat.index) + 0x9e3779b9 + (hashVal << 6) + (hashVal >> 2);
            hashVal ^= std::hash<std::wstring>{}(mat.matPath) + 0x9e3779b9 + (hashVal << 6) + (hashVal >> 2);
        }
        for (const auto& morph : MorphTargetList) {
            hashVal ^= std::hash<std::wstring>{}(morph.target) + 0x9e3779b9 + (hashVal << 6) + (hashVal >> 2);
        }
        wchar_t buf[32];
        swprintf(buf, 32, L" (%08X)", static_cast<unsigned int>(hashVal & 0xFFFFFFFF));
        return meshName + buf;
    }

    inline std::wstring StringToWString(const std::string& str) { return std::wstring(str.begin(), str.end()); }
    inline std::string WStringToString(const std::wstring& wstr) { return std::string(wstr.begin(), wstr.end()); }
    inline std::wstring FStringToWString(const FString& fstr) { const TCHAR* data = fstr.GetCharArray().GetData(); return data ? std::wstring(data) : L""; }
    inline std::wstring GuidToWString(const DynPalsGuid& Guid) { wchar_t buf[64]; swprintf(buf, 64, L"%08X%08X%08X%08X", Guid.A, Guid.B, Guid.C, Guid.D); return std::wstring(buf); }

    inline FProperty* GetProperty(UObject* Object, const wchar_t* PropertyName, bool bSilenceLogs = false) {
        if (!Object || !IsObjectValid(Object)) return nullptr;
        auto* Class = Object->GetClassPrivate();
        if (!Class || !IsObjectValid(Class)) return nullptr;

        Caches::CacheKey key{Class, std::wstring(PropertyName)}; // <--- FIX: std::wstring constructor
        {
            std::shared_lock<std::shared_mutex> read_lock(Caches::PropMutex);
            if (Caches::PropCache.count(key)) return Caches::PropCache[key];
        }
        FProperty* Prop = Class->GetPropertyByNameInChain(PropertyName);
        std::unique_lock<std::shared_mutex> write_lock(Caches::PropMutex);
        return Caches::PropCache[key] = Prop;
    }

    template<typename T>
    inline bool GetPropertyValue(UObject* Object, const wchar_t* PropertyName, T& OutValue, bool bSilenceLogs = false) {
        auto* Property = GetProperty(Object, PropertyName, bSilenceLogs);
        if (Property) {
            T* Ptr = Property->ContainerPtrToValuePtr<T>(Object);
            if (Ptr) { OutValue = *Ptr; return true; }
        }
        return false;
    }

    template<>
    inline bool GetPropertyValue<bool>(UObject* Object, const wchar_t* PropertyName, bool& OutValue, bool bSilenceLogs) {
        auto* Property = GetProperty(Object, PropertyName, bSilenceLogs);
        if (Property && Property->GetClass().GetName() == L"BoolProperty") {
            FBoolProperty* BoolProp = static_cast<FBoolProperty*>(Property);
            void* Ptr = BoolProp->ContainerPtrToValuePtr<void>(Object);
            if (Ptr) { OutValue = BoolProp->GetPropertyValue(Ptr); return true; }
        }
        return false;
    }

    template<typename T>
    inline bool SetPropertyValue(UObject* Object, const wchar_t* PropertyName, const T& Value, bool bSilenceLogs = false) {
        auto* Property = GetProperty(Object, PropertyName, bSilenceLogs);
        if (Property) {
            T* Ptr = Property->ContainerPtrToValuePtr<T>(Object);
            if (Ptr) { *Ptr = Value; return true; }
        }
        return false;
    }

    template<>
    inline bool SetPropertyValue<bool>(UObject* Object, const wchar_t* PropertyName, const bool& Value, bool bSilenceLogs) {
        auto* Property = GetProperty(Object, PropertyName, bSilenceLogs);
        if (Property && Property->GetClass().GetName() == L"BoolProperty") {
            FBoolProperty* BoolProp = static_cast<FBoolProperty*>(Property);
            void* Ptr = BoolProp->ContainerPtrToValuePtr<void>(Object);
            if (Ptr) { BoolProp->SetPropertyValue(Ptr, Value); return true; }
        }
        return false;
    }

    inline void CallFunction(UObject* Object, const wchar_t* FunctionName, void* Params = nullptr, bool bSilenceLogs = false) {
        if (!Object || !IsObjectValid(Object)) return;
        auto* Class = Object->GetClassPrivate();
        if (!Class || !IsObjectValid(Class)) return;

        Caches::CacheKey key{Class, std::wstring(FunctionName)}; // <--- FIX: std::wstring constructor
        UFunction* Function = nullptr;
        {
            std::shared_lock<std::shared_mutex> read_lock(Caches::FuncMutex);
            if (Caches::FuncCache.count(key)) {
                Function = Caches::FuncCache[key];
                if (Function && IsObjectValid(Function)) SafeProcessEvent(Object, Function, Params);
                return;
            }
        }
        Function = Object->GetFunctionByNameInChain(FunctionName);
        {
            std::unique_lock<std::shared_mutex> write_lock(Caches::FuncMutex);
            Caches::FuncCache[key] = Function;
        }
        if (Function && IsObjectValid(Function)) SafeProcessEvent(Object, Function, Params);
    }

    inline std::wstring FormatAssetPath(const std::wstring& Path) {
        if (Path.empty() || Path.find(L'.') != std::wstring::npos) return Path;
        size_t lastSlash = Path.find_last_of(L'/');
        if (lastSlash != std::wstring::npos) return Path + L"." + Path.substr(lastSlash + 1);
        return Path;
    }

    inline std::string ReadFileToString(const std::wstring& path) {
        std::ifstream file(path);
        if (!file.is_open()) return "";
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    inline UClass* GetUserWidgetClass() { return GetClassCached(STR("/Script/UMG.UserWidget")); }
    inline UObject* GetWBL() { return GetLibrary(STR("/Script/UMG.Default__WidgetBlueprintLibrary")); }
    inline UFunction* GetWBLFunction(const wchar_t* FunctionName) { return GetLibraryFunction(STR("/Script/UMG.Default__WidgetBlueprintLibrary"), FunctionName); }
    inline UObject* GetKTL() { return GetLibrary(STR("/Script/Engine.Default__KismetTextLibrary")); }
    inline UFunction* GetKTLFunction(const wchar_t* FunctionName) { return GetLibraryFunction(STR("/Script/Engine.Default__KismetTextLibrary"), FunctionName); }
    inline UObject* GetKML() { return GetLibrary(STR("/Script/Engine.Default__KismetMaterialLibrary")); }
    inline UFunction* GetKMLFunction(const wchar_t* FunctionName) { return GetLibraryFunction(STR("/Script/Engine.Default__KismetMaterialLibrary"), FunctionName); }
    inline UObject* GetKSL() { return GetLibrary(STR("/Script/Engine.Default__KismetSystemLibrary")); }
    inline UFunction* GetKSLFunction(const wchar_t* FunctionName) { return GetLibraryFunction(STR("/Script/Engine.Default__KismetSystemLibrary"), FunctionName); }

    inline void AssignStringToTextProperty(const std::wstring& Str, void* DestContainer, FProperty* DestTextProp) {
        UObject* KTL = GetKTL();
        UFunction* ConvFunc = GetKTLFunction(STR("Conv_StringToText"));
        if (!KTL || !ConvFunc || !DestTextProp || !DestContainer) return;

        alignas(8) uint8_t ConvParams[256] = {0};
        
        for (FProperty* Prop = (FProperty*)ConvFunc->GetChildProperties(); Prop; Prop = (FProperty*)GetNextField(Prop)) {
            Prop->InitializeValue_InContainer(ConvParams);
        }

        FProperty* InStrProp = ConvFunc->GetPropertyByNameInChain(STR("InString"));
        if (!InStrProp) InStrProp = ConvFunc->GetPropertyByNameInChain(STR("InStr"));
        FProperty* OutTextProp = ConvFunc->GetPropertyByNameInChain(STR("ReturnValue"));
        if (!OutTextProp) OutTextProp = ConvFunc->GetPropertyByNameInChain(STR("OutText"));

        if (InStrProp) {
            *InStrProp->ContainerPtrToValuePtr<FString>(ConvParams) = FString(Str.c_str());
        }

        SafeProcessEvent(KTL, ConvFunc, ConvParams);

        if (OutTextProp) {
            DestTextProp->CopyCompleteValue(
                DestTextProp->ContainerPtrToValuePtr<void>(DestContainer),
                OutTextProp->ContainerPtrToValuePtr<void>(ConvParams)
            );
        }

        for (FProperty* Prop = (FProperty*)ConvFunc->GetChildProperties(); Prop; Prop = (FProperty*)GetNextField(Prop)) {
            Prop->DestroyValue_InContainer(ConvParams);
        }
    }

    inline void SetTextSafely(UObject* Target, const wchar_t* FuncName, const std::wstring& Str) {
        if (!Target || !IsObjectValid(Target)) return;

        UObject* TextMainObj = nullptr;
        if (GetPropertyValue<UObject*>(Target, STR("Text_Main"), TextMainObj, true) && TextMainObj && IsObjectValid(TextMainObj)) {
            SetTextSafely(TextMainObj, FuncName, Str);
            return;
        }

        UFunction* Func = Target->GetFunctionByNameInChain(FuncName);
        if (!Func) return;

        alignas(8) uint8_t Params[256] = {0};
        
        for (FProperty* Prop = (FProperty*)Func->GetChildProperties(); Prop; Prop = (FProperty*)GetNextField(Prop)) {
            Prop->InitializeValue_InContainer(Params);
        }

        FProperty* TextProp = Func->GetPropertyByNameInChain(STR("InText"));
        if (!TextProp) TextProp = Func->GetPropertyByNameInChain(STR("Text"));
        if (!TextProp) TextProp = Func->GetPropertyByNameInChain(STR("ReturnValue"));
        
        if (!TextProp) {
            for (FProperty* Prop = (FProperty*)Func->GetChildProperties(); Prop; Prop = (FProperty*)GetNextField(Prop)) {
                if (Prop->GetClass().GetName() == STR("TextProperty")) {
                    TextProp = Prop;
                    break;
                }
            }
        }

        if (TextProp) {
            AssignStringToTextProperty(Str, Params, TextProp);
            SafeProcessEvent(Target, Func, Params);
        }

        for (FProperty* Prop = (FProperty*)Func->GetChildProperties(); Prop; Prop = (FProperty*)GetNextField(Prop)) {
            Prop->DestroyValue_InContainer(Params);
        }
    }

    inline bool IsGameWindowFocused() {
        static uint64_t lastCheckTick = 0;
        static bool cachedResult = true;
        uint64_t currentTick = GetTickCount64();
        
        if (currentTick - lastCheckTick > 100) {
            lastCheckTick = currentTick;
            HWND foregroundWindow = GetForegroundWindow();
            if (!foregroundWindow) {
                cachedResult = false;
            } else {
                DWORD foregroundProcId = 0;
                GetWindowThreadProcessId(foregroundWindow, &foregroundProcId);
                cachedResult = (foregroundProcId == GetCurrentProcessId());
            }
        }
        return cachedResult;
    }

    inline bool WasKeyJustPressed(RC::Unreal::UObject* PlayerController, const std::wstring& KeyName) {
        if (!PlayerController || KeyName.empty() || KeyName == L"None") return false;
        RC::Unreal::UFunction* Func = PlayerController->GetFunctionByNameInChain(STR("WasInputKeyJustPressed"));
        if (!Func) return false;
        
        alignas(8) uint8_t Params[64] = {0};
        RC::Unreal::FProperty* KeyProp = Func->GetPropertyByNameInChain(STR("Key"));
        if (KeyProp) {
            KeyProp->InitializeValue_InContainer(Params);
            RC::Unreal::FName* NamePtr = KeyProp->ContainerPtrToValuePtr<RC::Unreal::FName>(Params);
            if (NamePtr) *NamePtr = RC::Unreal::FName(KeyName.c_str(), RC::Unreal::FNAME_Add);
        }

        SafeProcessEvent(PlayerController, Func, Params);
        
        bool Result = false;
        RC::Unreal::FProperty* RetProp = Func->GetPropertyByNameInChain(STR("ReturnValue"));
        if (RetProp) {
            bool* RetPtr = RetProp->ContainerPtrToValuePtr<bool>(Params);
            if (RetPtr) Result = *RetPtr;
        }

        if (KeyProp) KeyProp->DestroyValue_InContainer(Params);
        return Result;
    }

    inline bool IsKeyDown(RC::Unreal::UObject* PlayerController, const std::wstring& KeyName) {
        if (!PlayerController || KeyName.empty() || KeyName == L"None") return false;
        RC::Unreal::UFunction* Func = PlayerController->GetFunctionByNameInChain(STR("IsInputKeyDown"));
        if (!Func) return false;
        
        alignas(8) uint8_t Params[64] = {0};
        RC::Unreal::FProperty* KeyProp = Func->GetPropertyByNameInChain(STR("Key"));
        if (KeyProp) {
            KeyProp->InitializeValue_InContainer(Params);
            RC::Unreal::FName* NamePtr = KeyProp->ContainerPtrToValuePtr<RC::Unreal::FName>(Params);
            if (NamePtr) *NamePtr = RC::Unreal::FName(KeyName.c_str(), RC::Unreal::FNAME_Add);
        }

        SafeProcessEvent(PlayerController, Func, Params);
        
        bool Result = false;
        RC::Unreal::FProperty* RetProp = Func->GetPropertyByNameInChain(STR("ReturnValue"));
        if (RetProp) {
            bool* RetPtr = RetProp->ContainerPtrToValuePtr<bool>(Params);
            if (RetPtr) Result = *RetPtr;
        }

        if (KeyProp) KeyProp->DestroyValue_InContainer(Params);
        return Result;
    }

    inline void RegisterAssetFolder(const std::wstring& AssetPath) {
        if (AssetPath.empty()) return;
        size_t lastSlash = AssetPath.find_last_of(L'/');
        if (lastSlash == std::wstring::npos) return;
        std::wstring FolderPath = AssetPath.substr(0, lastSlash);

        {
            std::shared_lock<std::shared_mutex> read_lock(Caches::ScannedFoldersMutex);
            if (Caches::ScannedFolders.count(FolderPath)) return;
        }

        UObject* ARH = GetLibrary(STR("/Script/AssetRegistry.Default__AssetRegistryHelpers"));
        if (!ARH) return;

        struct { UObject* ReturnValue; } GetARParams{nullptr};
        CallFunction(ARH, STR("GetAssetRegistry"), &GetARParams);
        UObject* AssetRegistry = GetARParams.ReturnValue;
        if (!AssetRegistry) return;

        UFunction* ScanFunc = AssetRegistry->GetFunctionByNameInChain(STR("ScanPathsSynchronous"));
        if (ScanFunc) {
            alignas(8) uint8_t ScanBuffer[512] = {0}; 
            FString Src(FolderPath.c_str()); 
            TArray<FString> LocalPaths;
            LocalPaths.Add(Src); 
            
            FArrayProperty* InPathsProp = CastField<FArrayProperty>(ScanFunc->GetPropertyByNameInChain(STR("InPaths")));
            if (InPathsProp) {
                void* Dest = InPathsProp->ContainerPtrToValuePtr<void>(ScanBuffer);
                if (Dest) memcpy(Dest, &LocalPaths, sizeof(TArray<FString>));
            }
            
            FBoolProperty* ForceRescanProp = CastField<FBoolProperty>(ScanFunc->GetPropertyByNameInChain(STR("bForceRescan")));
            if (ForceRescanProp) ForceRescanProp->SetPropertyValue(ForceRescanProp->ContainerPtrToValuePtr<void>(ScanBuffer), true);
            
            SafeProcessEvent(AssetRegistry, ScanFunc, ScanBuffer);
        }

        std::unique_lock<std::shared_mutex> write_lock(Caches::ScannedFoldersMutex);
        Caches::ScannedFolders.insert(FolderPath);
    }

inline UObject* LoadAssetInternal(const std::wstring& AssetPath, bool bAllowBlocking = true) {
        if (AssetPath.empty()) return nullptr;

        UObject* DirectPtr = NativeAsyncLoader::GetLoadedPointer(AssetPath);
        if (DirectPtr && IsObjectValid(DirectPtr)) return DirectPtr;

        UObject* GlobalPtr = NativeAsyncLoader::GetGlobalPointer(AssetPath);
        if (GlobalPtr && IsObjectValid(GlobalPtr)) return GlobalPtr;

        std::wstring resolvedPath = NativeAsyncLoader::ResolveCasing(AssetPath);
        std::wstring formatted = FormatAssetPath(resolvedPath); 
        
        UObject* ExistingObj = nullptr;
        if (formatted.rfind(L"/", 0) == 0) {
            ExistingObj = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, formatted.c_str());
        } else {
            UObject* ANY_PACKAGE = reinterpret_cast<UObject*>(-1);
            ExistingObj = UObjectGlobals::StaticFindObject<UObject*>(nullptr, ANY_PACKAGE, formatted.c_str());
        }

        if (ExistingObj && IsObjectValid(ExistingObj)) {
            std::wstring ClassName = ExistingObj->GetClassPrivate()->GetName();
            
            if (ClassName != L"Package") {
                if (ClassName == L"ObjectRedirector") {
                    UObject* Dest = nullptr;
                    if (GetPropertyValue<UObject*>(ExistingObj, STR("DestinationObject"), Dest)) {
                        NativeAsyncLoader::RegisterGlobalPointer(AssetPath, Dest);
                        return Dest;
                    }
                }
                NativeAsyncLoader::RegisterGlobalPointer(AssetPath, ExistingObj);
                return ExistingObj;
            }
        }

        if (!bAllowBlocking) return nullptr;

        std::wstring package, asset;
        size_t dot = formatted.find(L'.');
        if (dot != std::wstring::npos) {
            package = formatted.substr(0, dot);
            asset = formatted.substr(dot + 1);
        } else return nullptr;

        AltrSoftObjectPtr SoftPtr;
        SoftPtr.ObjectID.PackageName = FName(package.c_str(), FNAME_Add);
        SoftPtr.ObjectID.AssetName = FName(asset.c_str(), FNAME_Add);

        UObject* KismetLib = GetKismetSystemLibrary();
        UFunction* LoadFunc = KismetLib ? KismetLib->GetFunctionByNameInChain(STR("LoadAsset_Blocking")) : nullptr;
        if (!LoadFunc) return nullptr;

        alignas(8) uint8_t LoadParams[256] = {0};
        memcpy(LoadParams, &SoftPtr, sizeof(AltrSoftObjectPtr));

        SafeProcessEvent(KismetLib, LoadFunc, LoadParams);
        
        UObject* LoadedObj = nullptr;
        FProperty* RetProp = LoadFunc->GetPropertyByNameInChain(STR("ReturnValue"));
        if (RetProp) {
            UObject** RetPtr = RetProp->ContainerPtrToValuePtr<UObject*>(LoadParams);
            if (RetPtr) LoadedObj = *RetPtr;
        }

        if (LoadedObj && IsObjectValid(LoadedObj)) {
            std::wstring ClassName = LoadedObj->GetClassPrivate()->GetName();
            if (ClassName == L"ObjectRedirector") {
                UObject* Dest = nullptr;
                if (GetPropertyValue<UObject*>(LoadedObj, STR("DestinationObject"), Dest)) {
                    NativeAsyncLoader::RegisterGlobalPointer(AssetPath, Dest);
                    return Dest;
                }
            }
            NativeAsyncLoader::RegisterGlobalPointer(AssetPath, LoadedObj);
            return LoadedObj;
        }
        
        return nullptr;
    }

    inline bool IsAssetLoaded(const std::wstring& AssetPath) {
        if (AssetPath.empty()) return true;
        return LoadAssetInternal(AssetPath, false) != nullptr;
    }

    inline bool IsSkeletalMeshLoaded(const std::wstring& AssetPath) {
        if (IsAssetLoaded(AssetPath)) return true;

        if (AssetPath.find(L"/Mods/") != std::wstring::npos) {
            size_t lastSlash = AssetPath.find_last_of(L'/');
            if (lastSlash != std::wstring::npos) {
                std::wstring directory = AssetPath.substr(0, lastSlash + 1);
                std::wstring filename = AssetPath.substr(lastSlash + 1);

                if (filename.rfind(L"SK_", 0) != 0 && filename.rfind(L"sk_", 0) != 0) {
                    if (IsAssetLoaded(directory + L"SK_" + filename)) return true;
                    if (IsAssetLoaded(directory + L"sk_" + filename)) return true;
                }
            }
        }
        return false;
    }

    inline UObject* LoadAssetSafely(const std::wstring& AssetPath) {
        return LoadAssetInternal(AssetPath, true);
    }

    inline UObject* LoadSkeletalMeshSafely(const std::wstring& AssetPath) {
        UObject* Loaded = LoadAssetInternal(AssetPath, false);
        if (Loaded) return Loaded;

        std::wstring fallbackSK;
        std::wstring fallbacksk;

        if (AssetPath.find(L"/Mods/") != std::wstring::npos) {
            size_t lastSlash = AssetPath.find_last_of(L'/');
            if (lastSlash != std::wstring::npos) {
                std::wstring directory = AssetPath.substr(0, lastSlash + 1);
                std::wstring filename = AssetPath.substr(lastSlash + 1);

                if (filename.rfind(L"SK_", 0) != 0 && filename.rfind(L"sk_", 0) != 0) {
                    fallbackSK = directory + L"SK_" + filename;
                    Loaded = LoadAssetInternal(fallbackSK, false);
                    if (Loaded) return Loaded;

                    fallbacksk = directory + L"sk_" + filename;
                    Loaded = LoadAssetInternal(fallbacksk, false);
                    if (Loaded) return Loaded;
                }
            }
        }

        Loaded = LoadAssetInternal(AssetPath, true);
        if (Loaded) return Loaded;

        if (!fallbackSK.empty()) {
            Loaded = LoadAssetInternal(fallbackSK, true);
            if (Loaded) return Loaded;
            
            Loaded = LoadAssetInternal(fallbacksk, true);
            if (Loaded) return Loaded;
        }

        return nullptr;
    }

    inline std::vector<std::wstring> GetAssetsInVirtualFolder(const std::wstring& FolderPath) {
        {
            std::shared_lock<std::shared_mutex> read_lock(Caches::FolderMutex);
            if (Caches::FolderCache.count(FolderPath)) return Caches::FolderCache[FolderPath];
        }

        std::vector<std::wstring> Results;
        UObject* ARH = GetLibrary(STR("/Script/AssetRegistry.Default__AssetRegistryHelpers"));
        if (!ARH) return Results;

        struct { UObject* ReturnValue; } GetARParams{nullptr};
        CallFunction(ARH, STR("GetAssetRegistry"), &GetARParams);
        UObject* AssetRegistry = GetARParams.ReturnValue;
        if (!AssetRegistry) return Results;

        UFunction* ScanFunc = AssetRegistry->GetFunctionByNameInChain(STR("ScanPathsSynchronous"));
        if (ScanFunc) {
            alignas(8) uint8_t ScanBuffer[512] = {0}; 
            for (FProperty* P = (FProperty*)ScanFunc->GetChildProperties(); P; P = (FProperty*)GetNextField(P)) {
                P->InitializeValue_InContainer(ScanBuffer);
            }

            FArrayProperty* InPathsProp = CastField<FArrayProperty>(ScanFunc->GetPropertyByNameInChain(STR("InPaths")));
            if (InPathsProp) {
                TArray<FString>* Arr = static_cast<TArray<FString>*>(InPathsProp->ContainerPtrToValuePtr<void>(ScanBuffer));
                if (Arr) Arr->Add(FString(FolderPath.c_str()));
            }
            
            FBoolProperty* ForceRescanProp = CastField<FBoolProperty>(ScanFunc->GetPropertyByNameInChain(STR("bForceRescan")));
            if (ForceRescanProp) ForceRescanProp->SetPropertyValue(ForceRescanProp->ContainerPtrToValuePtr<void>(ScanBuffer), true);
            
            SafeProcessEvent(AssetRegistry, ScanFunc, ScanBuffer);

            for (FProperty* P = (FProperty*)ScanFunc->GetChildProperties(); P; P = (FProperty*)GetNextField(P)) {
                P->DestroyValue_InContainer(ScanBuffer);
            }
        }

        UFunction* GetAssetsFunc = AssetRegistry->GetFunctionByNameInChain(STR("GetAssetsByPath"));
        if (!GetAssetsFunc) return Results;

        alignas(8) uint8_t ParamsBuffer[512] = {0}; 
        for (FProperty* P = (FProperty*)GetAssetsFunc->GetChildProperties(); P; P = (FProperty*)GetNextField(P)) {
            P->InitializeValue_InContainer(ParamsBuffer);
        }

        FProperty* PackagePathProp = GetAssetsFunc->GetPropertyByNameInChain(STR("PackagePath"));
        if (PackagePathProp) {
            FName* Dest = static_cast<FName*>(PackagePathProp->ContainerPtrToValuePtr<void>(ParamsBuffer));
            if (Dest) *Dest = FName(FolderPath.c_str(), FNAME_Add);
        }

        FBoolProperty* RecursiveProp = CastField<FBoolProperty>(GetAssetsFunc->GetPropertyByNameInChain(STR("bRecursive")));
        if (RecursiveProp) RecursiveProp->SetPropertyValue(RecursiveProp->ContainerPtrToValuePtr<void>(ParamsBuffer), false);

        FBoolProperty* OnDiskProp = CastField<FBoolProperty>(GetAssetsFunc->GetPropertyByNameInChain(STR("bIncludeOnlyOnDiskAssets")));
        if (OnDiskProp) OnDiskProp->SetPropertyValue(OnDiskProp->ContainerPtrToValuePtr<void>(ParamsBuffer), false);

        SafeProcessEvent(AssetRegistry, GetAssetsFunc, ParamsBuffer);

        FArrayProperty* OutAssetDataProp = CastField<FArrayProperty>(GetAssetsFunc->GetPropertyByNameInChain(STR("OutAssetData")));
        if (OutAssetDataProp) {
            FScriptArray* ScriptArray = static_cast<FScriptArray*>(OutAssetDataProp->ContainerPtrToValuePtr<void>(ParamsBuffer));
            FProperty* InnerProp = OutAssetDataProp->GetInner();

            if (ScriptArray && InnerProp) {
                int32_t NumAssets = ScriptArray->Num();
                int32_t ElementSize = InnerProp->GetSize();
                uint8_t* ArrayData = static_cast<uint8_t*>(ScriptArray->GetData());

                UFunction* GetFullNameFunc = ARH->GetFunctionByNameInChain(STR("GetFullName"));
                
                for (int32_t i = 0; i < NumAssets; ++i) {
                    if (GetFullNameFunc) {
                        alignas(8) uint8_t FNParams[512] = {0}; 
                        for (FProperty* P = (FProperty*)GetFullNameFunc->GetChildProperties(); P; P = (FProperty*)GetNextField(P)) {
                            P->InitializeValue_InContainer(FNParams);
                        }
                        
                        FProperty* InAssetDataProp = GetFullNameFunc->GetPropertyByNameInChain(STR("InAssetData"));
                        if (InAssetDataProp && ArrayData) {
                            void* Dest = InAssetDataProp->ContainerPtrToValuePtr<void>(FNParams);
                            if (Dest) {
                                InAssetDataProp->CopyCompleteValue(Dest, ArrayData + (i * ElementSize));
                            }
                        }

                        SafeProcessEvent(ARH, GetFullNameFunc, FNParams);

                        FProperty* ReturnProp = GetFullNameFunc->GetPropertyByNameInChain(STR("ReturnValue"));
                        if (ReturnProp) {
                            FString* RetStr = static_cast<FString*>(ReturnProp->ContainerPtrToValuePtr<void>(FNParams));
                            if (RetStr && RetStr->GetCharArray().GetData()) {
                                std::wstring FullName = RetStr->GetCharArray().GetData();
                                size_t SpacePos = FullName.find(L' ');
                                if (SpacePos != std::wstring::npos) {
                                    std::wstring ClassName = FullName.substr(0, SpacePos);
                                    std::wstring Path = FullName.substr(SpacePos + 1);

                                    if (ClassName.find(L"Material") != std::wstring::npos) {
                                        Results.push_back(Path);
                                    }
                                } else {
                                    Results.push_back(FullName);
                                }
                            }
                        }

                        for (FProperty* P = (FProperty*)GetFullNameFunc->GetChildProperties(); P; P = (FProperty*)GetNextField(P)) {
                            P->DestroyValue_InContainer(FNParams);
                        }
                    }
                }
            } 
        }

        for (FProperty* P = (FProperty*)GetAssetsFunc->GetChildProperties(); P; P = (FProperty*)GetNextField(P)) {
            P->DestroyValue_InContainer(ParamsBuffer);
        }

        std::unique_lock<std::shared_mutex> write_lock(Caches::FolderMutex);
        {
            std::unique_lock<std::shared_mutex> scan_lock(Caches::ScannedFoldersMutex);
            Caches::ScannedFolders.insert(FolderPath);
        }
        
        return Caches::FolderCache[FolderPath] = Results;
    }
}