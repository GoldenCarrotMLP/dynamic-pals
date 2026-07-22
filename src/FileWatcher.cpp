#include "FileWatcher.hpp"
#include "ConfigManager.hpp"
#include "PalProcessor.hpp"
#include "AsyncHelper.hpp"
#include "NotificationManager.hpp"
#include "DataTypes.hpp"
#include <Windows.h>
#include <thread>
#include <vector>
#include <string>
#include <chrono>

using namespace RC::Unreal;

namespace DynPals {

    static void WatchDirectoryThread(std::wstring directoryPath) {
        HANDLE hDir = CreateFileW(
            directoryPath.c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS,
            NULL
        );

        if (hDir == INVALID_HANDLE_VALUE) {
            return;
        }

        alignas(DWORD) BYTE buffer[8192];
        DWORD bytesReturned;
        
        // Initialize the cooldown timer
        auto lastReloadTime = std::chrono::steady_clock::now();

        while (true) {
            if (ReadDirectoryChangesW(
                hDir,
                buffer,
                sizeof(buffer),
                TRUE, // Watch recursively
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE,
                &bytesReturned,
                NULL,
                NULL
            )) {
                
                // COOLDOWN GATE:
                // If we triggered a reload less than 1000ms ago, ignore this event.
                // Editors often fire 2-3 events per save; this ensures we only recompile once.
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastReloadTime).count() < 1000) {
                    continue;
                }

                FILE_NOTIFY_INFORMATION* pNotify = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer);
                bool bNeedsReload = false;

                while (pNotify) {
                    std::wstring relPath(pNotify->FileName, pNotify->FileNameLength / sizeof(wchar_t));
                    
                    // Trigger if anything changes inside our two config directories
                    bool bInConfigDir = relPath.find(L"SwapJSON") != std::wstring::npos || 
                                       relPath.find(L"ModelJSON") != std::wstring::npos;

                    if (bInConfigDir) {
                        bNeedsReload = true;
                        break;
                    }

                    if (pNotify->NextEntryOffset == 0) break;
                    pNotify = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(reinterpret_cast<uint8_t*>(pNotify) + pNotify->NextEntryOffset);
                }

                if (bNeedsReload) {
                    // Update the timestamp BEFORE the sleep to catch duplicates buffered during the wait
                    lastReloadTime = std::chrono::steady_clock::now();

                    // Debounce: Give the OS/Editor 500ms to finish the physical file write
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));

                    AsyncHelper::AsyncTask(ENamedThreads::GameThread, []() {
                        // 1. Instantly clear old error notifications from the screen
                        NotificationManager::Get().ClearInGameLogs();

                        DP_LOG(Normal, "[Hot-Reload] Config change detected! Recompiling matchmaking table...");
                        
                        // 2. Reload the JSON database
                        ConfigManager::Get().LoadConfigJSONs();

                        // 3. Refresh all Pals in the world to apply potential new skins
                        std::vector<UObject*> AllPals;
                        UObjectGlobals::FindAllOf(STR("PalCharacter"), AllPals);
                        for (UObject* Pal : AllPals) {
                            if (Pal) PalProcessor::Get().ProcessPal(Pal, false);
                        }
                    });
                }
            } else {
                break;
            }
        }

        CloseHandle(hDir);
    }

    void FileWatcher::Start(const std::wstring& DirectoryPath) {
        std::thread([DirectoryPath]() {
            WatchDirectoryThread(DirectoryPath);
        }).detach();
    }
}