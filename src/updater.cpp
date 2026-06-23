#define NOMINMAX
#include <Windows.h>
#include <wininet.h>

#include "Updater.hpp"
#include "Utils.hpp"
#include <DynamicOutput/DynamicOutput.hpp>
#include <vector>
#include <fstream>
#include <algorithm>

#pragma comment(lib, "wininet.lib")

using namespace RC;

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

    // Helper: Trims whitespace and newlines from strings to ensure accurate comparison
    static std::string Trim(const std::string& str) {
        size_t first = str.find_first_not_of(" \t\r\n");
        if (std::string::npos == first) return "";
        size_t last = str.find_last_not_of(" \t\r\n");
        return str.substr(first, (last - first + 1));
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

    void Updater::CheckForUpdates() {
        DP_LOG(Normal, "Auto-Updater: Checking GitHub for updates...");

        std::wstring currentDllPath = GetCurrentDllPath();
        std::wstring oldDllPath = currentDllPath + L".old";
        
        // Define path for local version.txt (assumed to be next to main.dll)
        std::wstring dllDir = currentDllPath.substr(0, currentDllPath.find_last_of(L"\\/") + 1);
        std::wstring versionTxtPath = dllDir + L"version.txt";

        // Clean up any .old DLLs left over from a previous update!
        DeleteFileW(oldDllPath.c_str());

        // 1. Read Local Version
        std::string localVersion = Trim(ReadFile(versionTxtPath));
        if (localVersion.empty()) {
            localVersion = "UNKNOWN";
        }

        // Initialize WinINet
        HINTERNET hInternet = InternetOpenA("DynPalsUpdater/1.0", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
        if (!hInternet) return;

        // 2. Fetch Remote Version
        // Note: Using raw.githubusercontent avoids the 302 redirect of github.com/raw/ URLs
        std::string remoteVersionUrl = "https://raw.githubusercontent.com/GoldenCarrotMLP/dynamic-pals/main/dlls/version.txt";
        HINTERNET hUrl = InternetOpenUrlA(hInternet, remoteVersionUrl.c_str(), NULL, 0, INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE, 0);
        
        std::string remoteVersion = "";
        if (hUrl) {
            char buffer[1024];
            DWORD bytesRead;
            while (InternetReadFile(hUrl, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
                remoteVersion.append(buffer, bytesRead);
            }
            InternetCloseHandle(hUrl);
        }
        
        remoteVersion = Trim(remoteVersion);

        if (remoteVersion.empty()) {
            DP_LOG(Warning, "Auto-Updater: Failed to fetch remote version.txt");
            InternetCloseHandle(hInternet);
            return;
        }

        // 3. Compare and Update
        bool bShouldUpdate = false;
        try {
            int localVal = std::stoi(localVersion);
            int remoteVal = std::stoi(remoteVersion);
            
            if (remoteVal > localVal) {
                bShouldUpdate = true;
            } else {
                DP_LOG(Normal, "Auto-Updater: Local version ({}) is up-to-date or newer than remote ({}). Skipping update.", 
                    Utils::StringToWString(localVersion), Utils::StringToWString(remoteVersion));
            }
        } catch (...) {
            // Fallback to string comparison if either version cannot be parsed as an integer
            if (remoteVersion != localVersion) {
                bShouldUpdate = true;
            }
        }

        if (bShouldUpdate) {
            DP_LOG(Normal, "Auto-Updater: Update found! Local: [{}] | Remote: [{}]", 
                Utils::StringToWString(localVersion), Utils::StringToWString(remoteVersion));
            DP_LOG(Normal, "Auto-Updater: Downloading new main.dll...");

            std::string dllUrl = "https://raw.githubusercontent.com/GoldenCarrotMLP/dynamic-pals/main/dlls/main.dll";
            HINTERNET hDownload = InternetOpenUrlA(hInternet, dllUrl.c_str(), NULL, 0, INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE, 0);
            
            if (hDownload) {
                std::vector<char> dllData;
                char buffer[4096];
                DWORD bytesRead;
                while (InternetReadFile(hDownload, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
                    dllData.insert(dllData.end(), buffer, buffer + bytesRead);
                }
                InternetCloseHandle(hDownload);

                if (!dllData.empty()) {
                    // Rename the currently locked DLL to .old
                    if (MoveFileExW(currentDllPath.c_str(), oldDllPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                        
                        // Write the newly downloaded main.dll
                        HANDLE hFile = CreateFileW(currentDllPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                        if (hFile != INVALID_HANDLE_VALUE) {
                            DWORD bytesWritten;
                            WriteFile(hFile, dllData.data(), dllData.size(), &bytesWritten, NULL);
                            CloseHandle(hFile);

                            // Write the new version.txt so it doesn't loop next startup
                            WriteFileStr(versionTxtPath, remoteVersion);

                            DP_LOG(Warning, "=========================================================");
                            DP_LOG(Warning, "DynPals has been auto-updated to version: {}", Utils::StringToWString(remoteVersion));
                            DP_LOG(Warning, "Please restart Palworld to apply the new update.");
                            DP_LOG(Warning, "=========================================================");
                        } else {
                            // Recover safely if writing the new DLL fails
                            MoveFileExW(oldDllPath.c_str(), currentDllPath.c_str(), MOVEFILE_REPLACE_EXISTING);
                            DP_LOG(Error, "Auto-Updater: Failed to write new main.dll to disk!");
                        }
                    } else {
                        DP_LOG(Error, "Auto-Updater: Failed to rename the locked main.dll. File might be hard-locked.");
                    }
                } else {
                    DP_LOG(Error, "Auto-Updater: Downloaded DLL was empty.");
                }
            } else {
                DP_LOG(Error, "Auto-Updater: Failed to connect to download URL.");
            }
        } else {
            DP_LOG(Normal, "Auto-Updater: Mod is up to date (Version: {})", Utils::StringToWString(localVersion));
        }

        InternetCloseHandle(hInternet);
    }
}