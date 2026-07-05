#include "FileWatcher.hpp"
#include "ConfigManager.hpp"
#include "PalProcessor.hpp"
#include "AsyncHelper.hpp"
#include "DataTypes.hpp"
#include <Windows.h>
#include <thread>
#include <vector>

using namespace RC::Unreal;

namespace DynPals {

    // Background thread loop
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

        alignas(DWORD) BYTE buffer[4096];
        DWORD bytesReturned;

        while (true) {
            // Block and wait for any file creations, deletions, modifications, or folder renames
            if (ReadDirectoryChangesW(
                hDir,
                buffer,
                sizeof(buffer),
                TRUE, // Watch recursively (both SwapJSON/ and ModelJSON/ subdirectories)
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE,
                &bytesReturned,
                NULL,
                NULL
            )) {
                FILE_NOTIFY_INFORMATION* pNotify = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer);
                bool bNeedsReload = false;

                while (pNotify) {
                    std::wstring filename(pNotify->FileName, pNotify->FileNameLength / sizeof(wchar_t));
                    
                    // We only care about modifications or additions to JSON files
                    if (filename.find(L".json") != std::wstring::npos) {
                        bNeedsReload = true;
                        break;
                    }

                    if (pNotify->NextEntryOffset == 0) break;
                    pNotify = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(reinterpret_cast<uint8_t*>(pNotify) + pNotify->NextEntryOffset);
                }

                if (bNeedsReload) {
                    // Debounce: Wait 250ms for text editors to finish writing to disk
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));

                    AsyncHelper::AsyncTask(ENamedThreads::GameThread, []() {
                        DP_LOG(Normal, "[Hot-Reload] Config change detected! Recompiling skin matchmaking table...");
                        
                        // 1. Reload the JSON database cleanly
                        ConfigManager::Get().LoadConfigJSONs();

                        // 2. Instantly reconcile all overworld Pals so their models update in real-time! [1]
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