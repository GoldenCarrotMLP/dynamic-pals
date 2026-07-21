#define NOMINMAX
#include <Windows.h>
#include <wininet.h>
#include <fmt/xchar.h> // Enable wide-character formatting support for {fmt}

#include "Updater.hpp"
#include "Utils.hpp"
#include <DynamicOutput/DynamicOutput.hpp>
#include <vector>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <filesystem>

#pragma comment(lib, "wininet.lib")

using namespace RC;
namespace fs = std::filesystem;

namespace DynPals {

    // Helper: Gets the absolute path of the currently executing main.dll
    static std::wstring GetCurrentDllPath() {
        HMODULE hModule = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)&GetCurrentDllPath, &hModule);
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(hModule, path, MAX_PATH);
        return std::wstring(path);
    }

    // Helper: Trims whitespace and newlines from strings
    static std::string Trim(const std::string& str) {
        size_t first = str.find_first_not_of(" \t\r\n");
        if (std::string::npos == first) return "";
        size_t last = str.find_last_not_of(" \t\r\n");
        return str.substr(first, (last - first + 1));
    }

    // Helper: Safely verifies if a trimmed string contains strictly digits
    static bool IsNumeric(const std::string& str) {
        if (str.empty()) return false;
        for (char c : str) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                return false;
            }
        }
        return true;
    }

    // Helper: Reads a file entirely into a string
    static std::string ReadFile(const std::wstring& path) {
        std::ifstream file(path);
        if (!file.is_open()) return "";
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    // Helper: Writes a string to a file
    static void WriteFileStr(const std::wstring& path, const std::string& content) {
        std::ofstream file(path, std::ios::trunc);
        if (file.is_open()) {
            file << content;
        }
    }

    // Helper: Safely resolves the Palworld/Pal/Content/Paks/LogicMods/ directory
    static fs::path GetLogicModsDirectory() {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        fs::path exeDir(exePath); // Palworld/Pal/Binaries/Win64/Palworld-Win64-Shipping.exe
        fs::path palDir = exeDir.parent_path().parent_path().parent_path(); // Palworld/Pal/
        return palDir / "Content" / "Paks" / "LogicMods";
    }

    // Helper: Performs a fast, block-buffered binary comparison between two files on disk
    static bool AreFilesEqual(const fs::path& p1, const fs::path& p2) {
        if (!fs::exists(p1) || !fs::exists(p2)) return false;
        try {
            if (fs::file_size(p1) != fs::file_size(p2)) return false;
            std::ifstream f1(p1, std::ios::binary);
            std::ifstream f2(p2, std::ios::binary);
            if (!f1.is_open() || !f2.is_open()) return false;
            
            const size_t bufferSize = 64 * 1024; // 64KB Buffer
            std::vector<char> buf1(bufferSize);
            std::vector<char> buf2(bufferSize);
            
            while (f1 && f2) {
                f1.read(buf1.data(), bufferSize);
                f2.read(buf2.data(), bufferSize);
                if (f1.gcount() != f2.gcount()) return false;
                if (f1.gcount() == 0) break;
                if (std::memcmp(buf1.data(), buf2.data(), f1.gcount()) != 0) return false;
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    // Helper: Formats a numeric version string (e.g. "105") to "v0-1-05" for the UI message only
    static std::wstring FormatVersionWithHyphens(const std::string& versionStr) {
        try {
            int versionNum = std::stoi(versionStr);
            int major = versionNum / 1000;
            int minor = (versionNum / 100) % 10;
            int patch = versionNum % 100;
            wchar_t buf[64];
            swprintf(buf, 64, L"v%d.%d.%02d", major, minor, patch);
            return std::wstring(buf);
        } catch (...) {
            std::wstring raw;
            raw.assign(versionStr.begin(), versionStr.end());
            // Safe range loop replaces std::replace to completely avoid narrow/wide compiler warnings
            for (wchar_t& c : raw) {
                if (c == L'.') c = L'-';
            }
            return L"v" + raw;
        }
    }

    // INDEPENDENT STEP: Local pak synchronization.
    static void SyncLocalPakToLogicMods(const std::wstring& dllDir, const std::string& currentVersionStr) {
        std::wstring localStagingPakPath = dllDir + L"DynamicPals.pak";
        if (!fs::exists(localStagingPakPath)) {
            DP_LOG(Default, "Local staged DynamicPals.pak not found. Skipping local LogicMods sync.");
            return;
        }

        fs::path logicModsDir = GetLogicModsDirectory();
        
        if (!fs::exists(logicModsDir)) {
            try { fs::create_directories(logicModsDir); } 
            catch (const std::exception& e) {
                DP_LOG(Error, "Auto-Updater: Failed to create LogicMods directory: {}", Utils::StringToWString(e.what()));
                return;
            }
        }

        fs::path expectedPakPath = logicModsDir / L"DynamicPals.pak";
        fs::path updatePakPath = logicModsDir / L"DynamicPals_update.pak";

        // Clean up any stray update file from a previous failed copy session
        if (fs::exists(updatePakPath)) {
            try { fs::remove(updatePakPath); } catch (...) {}
        }

        // Check if the file is already perfectly synced to prevent unnecessary disk writes
        if (fs::exists(expectedPakPath)) {
            if (AreFilesEqual(localStagingPakPath, expectedPakPath)) {
                DP_LOG(Default, "Auto-Updater: The active asset pack (DynamicPals.pak) matches the staged files. Skip copy.");
                return; 
            }
        }

        try {
            // Attempt to directly overwrite DynamicPals.pak
            fs::copy_file(localStagingPakPath, expectedPakPath, fs::copy_options::overwrite_existing);
            DP_LOG(Normal, "Auto-Updater: Successfully synced asset pack to LogicMods: DynamicPals.pak");
        } catch (const std::exception&) { // Removed unused 'e' variable to fix C4101
            // The copy failed, which means the game has locked the active DynamicPals.pak.
            DP_LOG(Warning, "Auto-Updater: DynamicPals.pak is locked. Saving to DynamicPals_update.pak...");
            
            try {
                fs::copy_file(localStagingPakPath, updatePakPath, fs::copy_options::overwrite_existing);
                DP_LOG(Error, "=========================================================");
                DP_LOG(Error, "UPDATE PENDING: DynamicPals.pak is locked by the game.");
                DP_LOG(Error, "1. Close Palworld.");
                DP_LOG(Error, "2. Navigate to: Pal\\Content\\Paks\\LogicMods\\");
                DP_LOG(Error, "3. Delete 'DynamicPals.pak'.");
                DP_LOG(Error, "4. Rename 'DynamicPals_update.pak' to 'DynamicPals.pak'.");
                DP_LOG(Error, "=========================================================");
            } catch (const std::exception& e2) {
                DP_LOG(Error, "Auto-Updater: Failed to copy staged asset pack to LogicMods entirely: {}", Utils::StringToWString(e2.what()));
            }
        }
    }

    void Updater::CheckForUpdates() {
        std::wstring currentDllPath = GetCurrentDllPath();
        std::wstring oldDllPath = currentDllPath + L".old";
        std::wstring dllDir = currentDllPath.substr(0, currentDllPath.find_last_of(L"\\/") + 1);
        std::wstring versionTxtPath = dllDir + L"version.txt";

        // Clean up legacy artifacts
        DeleteFileW(oldDllPath.c_str());

        // Read and parse current local version file
        std::string localVersion = Trim(ReadFile(versionTxtPath));
        if (!IsNumeric(localVersion)) {
            DP_LOG(Warning, "Auto-Updater: Local version format is invalid. Defaulting to 0 for fallback check.");
            localVersion = "0";
        }

        std::string finalVersion = localVersion;

        // -------------------------------------------------------------
        // INDEPENDENT STEP 1: Network Update Verification
        // -------------------------------------------------------------
        DP_LOG(Default, "Auto-Updater: Checking GitHub for updates...");

        HINTERNET hInternet = InternetOpenA("DynPalsUpdater/1.1", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
        if (hInternet) {
            std::string remoteVersionUrl = "https://raw.githubusercontent.com/GoldenCarrotMLP/dynamic-pals/main/dlls/version.txt";
            HINTERNET hUrl = InternetOpenUrlA(hInternet, remoteVersionUrl.c_str(), NULL, 0, INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE, 0);
            
            if (hUrl) {
                DWORD statusCode = 0;
                DWORD length = sizeof(statusCode);
                if (HttpQueryInfoA(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &statusCode, &length, NULL) && statusCode == 200) {
                    std::string remoteVersion = "";
                    char buffer[1024];
                    DWORD bytesRead;
                    while (InternetReadFile(hUrl, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
                        remoteVersion.append(buffer, bytesRead);
                    }
                    remoteVersion = Trim(remoteVersion);

                    if (IsNumeric(remoteVersion)) {
                        int localVal = std::stoi(localVersion);
                        int remoteVal = std::stoi(remoteVersion);
                        
                        if (remoteVal > localVal) {
                            DP_LOG(Default, "Auto-Updater: Update found! Local: [{}] | Remote: [{}]", Utils::StringToWString(localVersion), Utils::StringToWString(remoteVersion));
                            DP_LOG(Default, "Auto-Updater: Downloading new main.dll...");

                            // Download DLL
                            std::string dllUrl = "https://raw.githubusercontent.com/GoldenCarrotMLP/dynamic-pals/main/dlls/main.dll";
                            HINTERNET hDownload = InternetOpenUrlA(hInternet, dllUrl.c_str(), NULL, 0, INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE, 0);
                            
                            std::vector<char> dllData;
                            if (hDownload) {
                                statusCode = 0; length = sizeof(statusCode);
                                if (HttpQueryInfoA(hDownload, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &statusCode, &length, NULL) && statusCode == 200) {
                                    char dlBuffer[4096];
                                    DWORD dlBytesRead;
                                    while (InternetReadFile(hDownload, dlBuffer, sizeof(dlBuffer), &dlBytesRead) && dlBytesRead > 0) {
                                        dllData.insert(dllData.end(), dlBuffer, dlBuffer + dlBytesRead);
                                    }
                                }
                                InternetCloseHandle(hDownload);
                            }

                            if (!dllData.empty()) {
                                // Download PAK
                                DP_LOG(Default, "Auto-Updater: Downloading DynamicPals.pak asset package...");
                                std::string pakUrl = "https://raw.githubusercontent.com/GoldenCarrotMLP/dynamic-pals/refs/heads/main/dlls/DynamicPals.pak";
                                HINTERNET hPakDownload = InternetOpenUrlA(hInternet, pakUrl.c_str(), NULL, 0, INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE, 0);
                                
                                std::vector<char> pakData;
                                if (hPakDownload) {
                                    statusCode = 0; length = sizeof(statusCode);
                                    if (HttpQueryInfoA(hPakDownload, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &statusCode, &length, NULL) && statusCode == 200) {
                                        char pakBuffer[4096];
                                        DWORD pakBytesRead;
                                        while (InternetReadFile(hPakDownload, pakBuffer, sizeof(pakBuffer), &pakBytesRead) && pakBytesRead > 0) {
                                            pakData.insert(pakData.end(), pakBuffer, pakBuffer + pakBytesRead);
                                        }
                                    }
                                    InternetCloseHandle(hPakDownload);
                                }

                                // Swap file safely on disk
                                if (MoveFileExW(currentDllPath.c_str(), oldDllPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                                    HANDLE hFile = CreateFileW(currentDllPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                                    if (hFile != INVALID_HANDLE_VALUE) {
                                        DWORD bytesWritten;
                                        WriteFile(hFile, dllData.data(), dllData.size(), &bytesWritten, NULL);
                                        CloseHandle(hFile);

                                        WriteFileStr(versionTxtPath, remoteVersion);
                                        finalVersion = remoteVersion; // Track the new version to pass down to LogicMods Sync

                                        if (!pakData.empty()) {
                                            std::wstring localStagingPakPath = dllDir + L"DynamicPals.pak";
                                            HANDLE hPakFile = CreateFileW(localStagingPakPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                                            if (hPakFile != INVALID_HANDLE_VALUE) {
                                                WriteFile(hPakFile, pakData.data(), pakData.size(), &bytesWritten, NULL);
                                                CloseHandle(hPakFile);
                                            }
                                        }

                                        DP_LOG(Default, "=========================================================");
                                        DP_LOG(Normal, "DynPals has been auto-updated to version: {} \nPlease restart Palworld to apply the new update", Utils::StringToWString(remoteVersion));
                                        DP_LOG(Default, "=========================================================");
                                    } else {
                                        MoveFileExW(oldDllPath.c_str(), currentDllPath.c_str(), MOVEFILE_REPLACE_EXISTING);
                                        DP_LOG(Warning, "Auto-Updater: Failed to write new main.dll payload to disk.");
                                    }
                                } else {
                                    DP_LOG(Warning, "Auto-Updater: Failed to rotate old main.dll. File is locked or in use.");
                                }
                            } else {
                                DP_LOG(Warning, "Auto-Updater: DLL download failed or returned empty payload.");
                            }
                        } else {
                            DP_LOG(Default, "Auto-Updater: Mod is up to date (Version: {})", Utils::StringToWString(localVersion));
                        }
                    } else {
                        DP_LOG(Warning, "Auto-Updater: Received non-numeric version index. Update aborted.");
                    }
                } else {
                    DP_LOG(Warning, "Auto-Updater: GitHub returned error status for version.txt.");
                }
                InternetCloseHandle(hUrl);
            } else {
                DP_LOG(Warning, "Auto-Updater: Connection to GitHub version database failed. Offline mode active.");
            }
            InternetCloseHandle(hInternet);
        } else {
            DP_LOG(Warning, "Auto-Updater: Could not initialize internet connection. Offline mode active.");
        }

        // -------------------------------------------------------------
        // INDEPENDENT STEP 2: Unconditional Local Pak Synchronization
        // -------------------------------------------------------------
        // Proceeds to sync local pak to LogicMods regardless of whether we updated over the network or not.
        SyncLocalPakToLogicMods(dllDir, finalVersion);
    }
}