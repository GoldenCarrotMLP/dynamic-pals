#define NOMINMAX
#include <Windows.h>

#include <Zydis/Zydis.h>

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <chrono>
#include <fstream>
#include <safetyhook.hpp>
#include <thread>

#include "AsyncHelper.hpp"
#include "HooksManager.hpp"
#include "NotificationManager.hpp"
#include "PalProcessor.hpp"
#include "SaveManager.hpp"
#include "UI/UIRegistry.hpp"
#include "UI/Views/TestUI.hpp" 
#include "UI/Views/UIManager.hpp"
#include "Updater.hpp"
#include "Utils.hpp"
#include "VFXManager.hpp"
#include "InputManager.hpp"
#include "../include/NativeAsyncLoader.hpp" 

using namespace RC;
using namespace RC::Unreal;

namespace DynPals {

static bool bCompletedInitReady = false;
static UObject* LastPlayerController = nullptr;
static UObject* LastWorld = nullptr;
static bool bIsAtMenu = false;

// The global cached class pointer we use for 1-cycle comparisons
static UClass* GCachedModActorClass = nullptr; 

static SafetyHookInline Hook_MasterWazaUpdate;
static SafetyHookInline Hook_OnUpdateCharacterRank;
static SafetyHookInline Hook_AddFriendshipRankupLog;

bool HooksManager::OnUObjectDeleted(RC::Unreal::UObject* Obj) {
    bool bFound = false;
    if (LastPlayerController == Obj) { LastPlayerController = nullptr; bFound = true; }
    if (LastWorld == Obj) { LastWorld = nullptr; bFound = true; }
    
    // FIX: If the engine deletes the ModActor class, wipe our cache!
    if (GCachedModActorClass == Obj) { GCachedModActorClass = nullptr; bFound = true; } 
    return bFound;
}

static void CheckAndWarnConflictingMods() {
    try {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        std::filesystem::path palDir = std::filesystem::path(exePath).parent_path().parent_path().parent_path();
        std::filesystem::path logicModsDir = palDir / "Content" / "Paks" / "LogicMods";

        if (!std::filesystem::exists(logicModsDir)) return;

        std::vector<std::wstring> detectedConflicts;

        for (const auto& entry : std::filesystem::directory_iterator(logicModsDir)) {
            if (entry.is_regular_file()) {
                std::wstring filename = entry.path().filename().wstring();
                std::wstring lower = filename;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);

                if (lower.find(L"palmagic") != std::wstring::npos || lower.find(L"altermatic") != std::wstring::npos) {
                    detectedConflicts.push_back(filename);
                }
            }
        }

        if (!detectedConflicts.empty()) {
            std::wstring conflictListStr = L"";
            for (size_t i = 0; i < detectedConflicts.size(); ++i) {
                conflictListStr += L"• " + detectedConflicts[i];
                if (i + 1 < detectedConflicts.size()) conflictListStr += L"\n";
            }

            std::wstring dialogMsg = 
                L"DUPLICATE / CONFLICTING MOD DETECTED:\n\n"
                + conflictListStr + L"\n\n"
                L"Please disable or remove either DynamicPals or the conflicting mod(s) to avoid errors and crashes.";

            DP_LOG(Error, "[Conflict Checker] Duplicate mod(s) found in LogicMods:\n{}", conflictListStr);

            NotificationManager::Get().ShowTwoButtonModal(
                dialogMsg,
                L"Open Folder", []() {
                    wchar_t exePath[MAX_PATH];
                    GetModuleFileNameW(NULL, exePath, MAX_PATH);
                    std::filesystem::path palDir = std::filesystem::path(exePath).parent_path().parent_path().parent_path();
                    std::wstring modsPath = (palDir / L"Content" / L"Paks" / L"LogicMods").wstring();
                    ShellExecuteW(NULL, L"open", modsPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
                },
                L"Dismiss", []() {}
            );
        }
    } catch (...) {}
}

static void* ResolveNativeFromThunk(void* ThunkAddress) {
    if (!ThunkAddress) return nullptr;

    ZyanStatus status;
    ZydisDecoder decoder;
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64))) {
        return nullptr;
    }

    ZyanUSize offset = 0;
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];

    std::vector<void*> calls;

    const uintptr_t thunkStart = reinterpret_cast<uintptr_t>(ThunkAddress);
    const uintptr_t thunkEnd = thunkStart + 500; 

    while (offset < 500) {
        status = ZydisDecoderDecodeFull(&decoder, reinterpret_cast<uint8_t*>(ThunkAddress) + offset, 500 - offset, &instruction, operands);
        if (!ZYAN_SUCCESS(status)) break;
        if (instruction.mnemonic == ZYDIS_MNEMONIC_RET) break;

        if (instruction.mnemonic == ZYDIS_MNEMONIC_CALL) {
            if (operands[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                uintptr_t rip = thunkStart + offset + instruction.length;
                uintptr_t target = rip + operands[0].imm.value.s;
                calls.push_back(reinterpret_cast<void*>(target));
            }
        }
        else if (instruction.mnemonic == ZYDIS_MNEMONIC_JMP) {
            if (operands[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                uintptr_t rip = thunkStart + offset + instruction.length;
                uintptr_t target = rip + operands[0].imm.value.s;

                if (target < thunkStart || target > thunkEnd) {
                    calls.push_back(reinterpret_cast<void*>(target));
                    break; 
                }
            }
        }

        offset += instruction.length;
    }

    if (calls.empty()) return nullptr;
    void* lastCallTarget = calls.back();

    status = ZydisDecoderDecodeFull(&decoder, lastCallTarget, 16, &instruction, operands);
    if (ZYAN_SUCCESS(status)) {
        if (instruction.mnemonic == ZYDIS_MNEMONIC_CMP && operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER && operands[0].reg.value == ZYDIS_REGISTER_RCX) {
            if (calls.size() > 1) {
                return calls[calls.size() - 2];
            }
        }
    }

    if (calls.size() > 1 && calls.front() == lastCallTarget) {
        return nullptr; 
    }

    return lastCallTarget;
}

static void* GetNativeAddress(const wchar_t* FunctionPath) {
    UFunction* FuncObj = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, FunctionPath);
    if (!FuncObj) return nullptr;

    void* ThunkAddr = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(FuncObj) + 0xD8);
    void* NativeAddr = ResolveNativeFromThunk(ThunkAddr);

    if (NativeAddr) {
        return NativeAddr;
    }
    return ThunkAddr;
}

void __fastcall NativeMasterWazaUpdate_Hook(UObject* This, int32_t AddLevel, int32_t NowLevel) {
    Hook_MasterWazaUpdate.call<void, UObject*, int32_t, int32_t>(This, AddLevel, NowLevel);
    if (This) {
        PalProcessor::Get().ProcessPal(This, false);
    }
}

void __fastcall NativeOnUpdateCharacterRank_Hook(UObject* This, int32_t NewRank, int32_t OldRank) {
    Hook_OnUpdateCharacterRank.call<void, UObject*, int32_t, int32_t>(This, NewRank, OldRank);
    if (This) {
        UObject* PalActor = This->GetOuterPrivate();
        if (PalActor) {
            PalProcessor::Get().ProcessPal(PalActor, false);
        }
    }
}

void __fastcall NativeAddFriendshipRankupLog_Hook(UObject* WorldContextObject, UObject* IndividualParameter, int32_t NewRank, bool bFirstRankup) {
    Hook_AddFriendshipRankupLog.call<void, UObject*, UObject*, int32_t, bool>(WorldContextObject, IndividualParameter, NewRank, bFirstRankup);
    if (IndividualParameter && Utils::IsObjectValid(IndividualParameter)) {
        UObject* PalActor = nullptr;
        Utils::GetPropertyValue<UObject*>(IndividualParameter, STR("IndividualActor"), PalActor);
        if (PalActor && Utils::IsObjectValid(PalActor)) {
            PalProcessor::Get().ProcessPal(PalActor, false);
        }
    }
}

static void OnStartedWorldAutoSave(UnrealScriptFunctionCallableContext&, void*) {
    DP_LOG(Default, "Auto-Save triggered! Synchronizing world persistence...\n");
    SaveManager::Get().SaveWorldData();
}

struct FReentrantGuard {
    bool& bFlag;
    FReentrantGuard(bool& InFlag) : bFlag(InFlag) { bFlag = true; }
    ~FReentrantGuard() { bFlag = false; }
};

static void OnEngineTick(Unreal::Hook::TCallbackIterationData<void>&, Unreal::UEngine*, float DeltaSeconds, bool bIdle) {
    if (bIdle) return;

    static bool bIsReentrant = false;
    if (bIsReentrant) return;
    FReentrantGuard Guard(bIsReentrant);

    DP_PROFILE("OnEngineTick_Total", 3.0); 

    VFXManager::Get().Tick();
    NativeAsyncLoader::Tick();

    static int VirtualFrameCount = 0;
    VirtualFrameCount++;
    if (VirtualFrameCount >= 6) {
        VirtualFrameCount = 0;
        DP_PROFILE("PalProcessor_Tick", 2.0);
        PalProcessor::Get().Tick();
    }

    UObject* PlayerController = LastPlayerController;
    if (!PlayerController || !Utils::IsObjectValid(PlayerController)) {
        static auto lastSearchTime = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastSearchTime).count() >= 2) {
            lastSearchTime = now;
            DP_PROFILE("PlayerController_GlobalSearch", 1.0);
            PlayerController = UObjectGlobals::FindFirstOf(STR("PalPlayerController"));
            if (PlayerController) {
                LastPlayerController = PlayerController;
            }
        }
    }

    if (Utils::IsGameWindowFocused()) {
        auto& Settings = SaveManager::Get().Settings;

        static bool bVfxPrevPressed = false;
        static bool bVfxNextPressed = false;
        if (GetAsyncKeyState(VK_MENU) & 0x8000) {
            if (GetAsyncKeyState(VK_LEFT) & 0x8000) {
                if (!bVfxPrevPressed) {
                    bVfxPrevPressed = true;
                    VFXManager::Get().CyclePrevious();
                }
            } else { bVfxPrevPressed = false; }

            if (GetAsyncKeyState(VK_RIGHT) & 0x8000) {
                if (!bVfxNextPressed) {
                    bVfxNextPressed = true;
                    VFXManager::Get().CycleNext();
                }
            } else { bVfxNextPressed = false; }
        } else {
            bVfxPrevPressed = false;
            bVfxNextPressed = false;
        }

        if (PlayerController) {
            static bool bMenuKeyPressed = false;
            if (InputManager::Get().IsHotkeyDownFast(PlayerController, Settings.MenuModVK, Settings.MenuKeyVK, Settings.bMenuIsGamepad, Settings.MenuModifier, Settings.MenuKey)) {
                if (!bMenuKeyPressed) {
                    bMenuKeyPressed = true;
                    DP_LOG(Default, "[Hotkey] Main Menu hotkey triggered!");
                    UIManager::Get().RequestToggle();
                }
            } else {
                bMenuKeyPressed = false;
            }

            static bool bTestMenuKeyPressed = false;
            if (InputManager::Get().IsHotkeyDownFast(PlayerController, Settings.TestModVK, Settings.TestKeyVK, Settings.bTestIsGamepad, Settings.TestMenuModifier, Settings.TestMenuKey)) {
                if (!bTestMenuKeyPressed) {
                    bTestMenuKeyPressed = true;
                    DP_LOG(Default, "[Hotkey] Test Menu hotkey triggered!");
                    TestUI::Get().RequestToggle();
                }
            } else {
                bTestMenuKeyPressed = false;
            }
        }
    }

    if (UIRegistry::Get().RequiresTick() && PlayerController) {
        DP_PROFILE("UIRegistry_TickAll", 2.0); 
        UIRegistry::Get().TickAll(PlayerController);
    }
}

static void OnWidgetAddedToViewport(UnrealScriptFunctionCallableContext& Context, void*) {
    if (bIsAtMenu) return;

    UObject* Widget = Context.Context;
    if (!Widget) return;

    UClass* WidgetClass = Widget->GetClassPrivate();
    if (!WidgetClass) return;

    std::wstring WidgetName = WidgetClass->GetName();

    if (WidgetName.find(L"WBP_Title") != std::wstring::npos ||
        WidgetName.find(L"WBP_Login") != std::wstring::npos) {
        bIsAtMenu = true;
        bCompletedInitReady = false;
        
        // FIX: Wipe session statics!
        LastPlayerController = nullptr;
        LastWorld = nullptr;
        GCachedModActorClass = nullptr;

        SaveManager::Get().Reset();
        NotificationManager::Get().SetReady(false);
        PalProcessor::Get().ClearAllSwappedStatus();
        UIRegistry::Get().InvalidateAllUIs();
        Utils::Caches::ClearAll(); 
        NativeAsyncLoader::ClearCache();

        DP_LOG(Default, "Transitioned to Main Menu. Mod entering standby mode...");
        std::thread([]() { Updater::CheckForUpdates(); }).detach();
    }
}

static void OnOpenLevel(UnrealScriptFunctionCallableContext& Context, void*) {
    bIsAtMenu = false;
    bCompletedInitReady = false;
    
    // FIX: Wipe session statics!
    LastPlayerController = nullptr;
    LastWorld = nullptr;
    GCachedModActorClass = nullptr;

    NotificationManager::Get().SetReady(false);
    UIRegistry::Get().InvalidateAllUIs();
    Utils::Caches::ClearAll(); 
    NativeAsyncLoader::ClearCache(); 
}

static std::wstring GetFormattedVersionString() {
    HMODULE hModule = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&GetFormattedVersionString, &hModule);
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(hModule, path, MAX_PATH);

    std::wstring currentDllPath(path);
    std::wstring dllDir = currentDllPath.substr(0, currentDllPath.find_last_of(L"\\/") + 1);
    std::wstring versionTxtPath = dllDir + L"version.txt";

    std::ifstream file(versionTxtPath);
    if (!file.is_open()) return L"v0.0.56";

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    content.erase(0, content.find_first_not_of(" \t\r\n"));
    size_t last = content.find_last_not_of(" \t\r\n");
    if (last != std::string::npos) content.erase(last + 1);

    if (content.empty()) return L"v0.0.56";

    try {
        int versionNum = std::stoi(content);
        int major = versionNum / 1000;
        int minor = (versionNum / 100) % 10;
        int patch = versionNum % 100;
        wchar_t buf[64];
        swprintf(buf, 64, L"v%d.%d.%02d", major, minor, patch);
        return std::wstring(buf);
    } catch (...) {
        std::wstring rawVersion(content.begin(), content.end());
        return L"v" + rawVersion;
    }
}

void HooksManager::OnPalSpawnedReady(UnrealScriptFunctionCallableContext& Context, void*) {
    if (!bCompletedInitReady) return;

    UObject* PalNPC = Context.Context;
    if (PalNPC) {
        PalProcessor::Get().ProcessPal(PalNPC, false);
    }
}

static void OnClientRestart(UnrealScriptFunctionCallableContext& Context, void*) {
    UObject* PlayerController = Context.Context;
    if (!PlayerController) return;

    UObject* Level = PlayerController->GetOuterPrivate();
    UObject* CurrentWorld = Level ? Level->GetOuterPrivate() : nullptr;

    if (PlayerController != LastPlayerController || CurrentWorld != LastWorld) {
        LastPlayerController = PlayerController;
        LastWorld = CurrentWorld;

        UObject* GameplayStatics = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
        if (GameplayStatics) {
            struct {
                UObject* WorldContextObject;
                bool bRemovePrefixString;
                FString ReturnValue;
            } Params{PlayerController, true, FString()};

            Utils::CallFunction(GameplayStatics, STR("GetCurrentLevelName"), &Params);
            std::wstring MapName = Utils::FStringToWString(Params.ReturnValue);

            bool bIsMenu = (MapName.find(L"Title") != std::wstring::npos ||
                            MapName.find(L"Login") != std::wstring::npos ||
                            MapName.empty());

            if (bIsMenu) {
                bCompletedInitReady = false;
                
                // FIX: Wipe session statics!
                GCachedModActorClass = nullptr;

                NotificationManager::Get().SetReady(false);
                SaveManager::Get().Reset();
                PalProcessor::Get().ClearAllSwappedStatus();
                Utils::Caches::ClearAll(); 
                NativeAsyncLoader::ClearCache();
            } else {
                bIsAtMenu = false;
                bCompletedInitReady = false;

                // FIX: Wipe session statics!
                GCachedModActorClass = nullptr;

                NotificationManager::Get().SetReady(false);
                SaveManager::Get().Reset();
                PalProcessor::Get().ClearAllSwappedStatus();
                Utils::Caches::ClearAll(); 
                NativeAsyncLoader::ClearCache();

                DP_LOG(Default, "New Session Detected (Map: '{}'). Standby active. Waiting 8 seconds for level load...\n", MapName);

                std::thread([CapturedWorld = LastWorld]() {
                    std::this_thread::sleep_for(std::chrono::seconds(8)); 

                    AsyncHelper::AsyncTask(ENamedThreads::GameThread, [CapturedWorld]() {
                        DP_LOG(Default, "Settle period complete. Safely resolving player active party...\n");

                        NativeAsyncLoader::Initialize();

                        UObject* PlayerControllerObj = nullptr;
                        if (CapturedWorld && Utils::IsObjectValid(CapturedWorld)) {
                            UObject* GameplayStatics = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
                            if (GameplayStatics) {
                                struct { UObject* WorldContextObject; int32_t PlayerIndex; UObject* ReturnValue; } GSParams{CapturedWorld, 0, nullptr};
                                Utils::CallFunction(GameplayStatics, STR("GetPlayerController"), &GSParams);
                                PlayerControllerObj = GSParams.ReturnValue;
                            }
                        }
                        if (!PlayerControllerObj && LastPlayerController) {
                            PlayerControllerObj = LastPlayerController;
                        }
                        if (!PlayerControllerObj) {
                            PlayerControllerObj = UObjectGlobals::FindFirstOf(STR("PalPlayerController"));
                        }

                        if (PlayerControllerObj) {
                            LastPlayerController = PlayerControllerObj;
                            PalProcessor::Get().ProcessPlayerParty(PlayerControllerObj);
                            UIManager::Get().PreloadUI(PlayerControllerObj);
                        }

                        bCompletedInitReady = true;

                        std::wstring verStr = GetFormattedVersionString();
                        DP_LOG(Normal, "Welcome to dynamic pals {} - Experimental", verStr);

                        NotificationManager::Get().SetReady(true);
                        NotificationManager::Get().FlushQueuedToasts();

                        CheckAndWarnConflictingMods();
                    });
                }).detach();
            }
        }
    }
}

void HooksManager::RegisterHooks() {
    UFunction* InitFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Pal.PalNPC:OnCompletedInitParam"));
    if (InitFunc) {
        InitFunc->RegisterPostHook(OnPalSpawnedReady, nullptr);
        DP_LOG(Default, "Successfully hooked OnCompletedInitParam (Native Pipeline Active!)\n");
    }

    UFunction* FunnelSpawnFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Pal.PalFunnelCharacter:OnSpawned"));
    if (FunnelSpawnFunc) {
        FunnelSpawnFunc->RegisterPostHook(OnPalSpawnedReady, nullptr);
    }

    UFunction* FunnelOnActiveFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Pal.PalFunnelCharacter:OnActive"));
    if (FunnelOnActiveFunc) {
        FunnelOnActiveFunc->RegisterPostHook(OnPalSpawnedReady, nullptr);
    }

    UFunction* RestartFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.PlayerController:ClientRestart"));
    if (RestartFunc) {
        RestartFunc->RegisterPostHook(OnClientRestart, nullptr);
        DP_LOG(Default, "Successfully hooked ClientRestart for map transitions.\n");
    }

    Unreal::Hook::RegisterEngineTickPreCallback(OnEngineTick, {false, false, STR("DynamicPals"), STR("OnEngineTick")});
    DP_LOG(Default, "Successfully hooked UEngine::Tick (Native EngineTick Active!)\n");

    UFunction* SaveFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Pal.PalSaveGameManager:StartWorldDataAutoSave"));
    if (SaveFunc) {
        SaveFunc->RegisterPostHook(OnStartedWorldAutoSave, nullptr);
    }

    void* MasterWazaUpdateAddr = GetNativeAddress(STR("/Script/Pal.PalNPC:MasterWazaUpdateWhenLevelUp"));
    if (MasterWazaUpdateAddr) {
        Hook_MasterWazaUpdate = safetyhook::create_inline(MasterWazaUpdateAddr, NativeMasterWazaUpdate_Hook);
    }

    void* SetRankAddr = AsyncHelper::FindPattern("40 53 48 83 EC 20 48 8B D9 48 8B 89 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 85 C0 75 ?? 48 8B D0 48 8B CB 48 83 C4 20 5B");
    if (SetRankAddr) {
        Hook_OnUpdateCharacterRank = safetyhook::create_inline(SetRankAddr, NativeOnUpdateCharacterRank_Hook);
    }

    void* FriendshipRankupAddr = GetNativeAddress(STR("/Script/Pal.PalLogUtility:AddFriendshipRankupLog"));
    if (FriendshipRankupAddr) {
        Hook_AddFriendshipRankupLog = safetyhook::create_inline(FriendshipRankupAddr, NativeAddFriendshipRankupLog_Hook);
    }

    UFunction* AddToViewportFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/UMG.UserWidget:AddToViewport"));
    if (AddToViewportFunc) {
        AddToViewportFunc->RegisterPostHook(OnWidgetAddedToViewport, nullptr);
    }

    UFunction* AddToPlayerScreenFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/UMG.UserWidget:AddToPlayerScreen"));
    if (AddToPlayerScreenFunc) {
        AddToPlayerScreenFunc->RegisterPostHook(OnWidgetAddedToViewport, nullptr);
    }

    UFunction* OpenLevelFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.GameplayStatics:OpenLevel"));
    if (OpenLevelFunc) {
        OpenLevelFunc->RegisterPreHook(OnOpenLevel, nullptr);
    }

    UFunction* SetOwnerFunc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.Actor:SetOwner"));
    if (SetOwnerFunc) {
        SetOwnerFunc->RegisterPreHook([](UnrealScriptFunctionCallableContext& Context, void*) {
            
            // FIX: Ensure we resolve the correct class pointer for the current game session
            if (!GCachedModActorClass || !Utils::IsObjectValid(GCachedModActorClass)) {
                GCachedModActorClass = Utils::GetClassCached(STR("/Game/Mods/DynamicPals/ModActor.ModActor_C"));
            }

            if (Context.Context && Context.Context->GetClassPrivate() == GCachedModActorClass) {
                if (!GCachedModActorClass->GetPropertyByNameInChain(STR("LoadedAssetsTemp"))) return;
                UFunction* Func = Context.TheStack.Node();
                if (!Func) return;

                UObject* Requester = nullptr;
                FProperty* OwnerProp = Func->GetPropertyByNameInChain(STR("NewOwner"));
                if (OwnerProp) {
                    UObject** Ptr = OwnerProp->ContainerPtrToValuePtr<UObject*>(Context.TheStack.Locals());
                    if (Ptr) Requester = *Ptr;
                }

                if (Requester) {
                    NativeAsyncLoader::OnAsyncLoadComplete(Context.Context, Requester);
                }
            }
        }, nullptr);
        DP_LOG(Default, "Successfully registered native callback hook on Actor:SetOwner.");
    }
}

}