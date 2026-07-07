#define NOMINMAX
#include <Windows.h>
#include <wininet.h>

#include "Updater.hpp"
#include "Utils.hpp"
#include <DynamicOutput/DynamicOutput.hpp>
#include <vector>
#include <fstream>
#include <algorithm>
#include <cctype>

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

    void Updater::CheckForUpdates() {
        DP_LOG(Default, "Auto-Updater: Checking GitHub for updates...");

        std::wstring currentDllPath = GetCurrentDllPath();
        std::wstring oldDllPath = currentDllPath + L".old";
        
        std::wstring dllDir = currentDllPath.substr(0, currentDllPath.find_last_of(L"\\/") + 1);
        std::wstring versionTxtPath = dllDir + L"version.txt";

        // Clean up any .old DLLs left over from previous updates
        DeleteFileW(oldDllPath.c_str());

        // 1. Read and validate local version representation
        std::string localVersion = Trim(ReadFile(versionTxtPath));
        if (!IsNumeric(localVersion)) {
            DP_LOG(Warning, "Auto-Updater: Local version format is invalid. Defaulting to 0 for fallback check.");
            localVersion = "0";
        }

        // Initialize WinINet
        HINTERNET hInternet = InternetOpenA("DynPalsUpdater/1.1", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
        if (!hInternet) return;

        // 2. Fetch Remote Version
        std::string remoteVersionUrl = "https://raw.githubusercontent.com/GoldenCarrotMLP/dynamic-pals/main/dlls/version.txt";
        HINTERNET hUrl = InternetOpenUrlA(hInternet, remoteVersionUrl.c_str(), NULL, 0, INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE, 0);
        
        if (!hUrl) {
            DP_LOG(Warning, "Auto-Updater: Connection to GitHub version database failed.");
            InternetCloseHandle(hInternet);
            return;
        }

        // Validate HTTP Status Code for the version request
        DWORD statusCode = 0;
        DWORD length = sizeof(statusCode);
        if (HttpQueryInfoA(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &statusCode, &length, NULL)) {
            if (statusCode != 200) {
                DP_LOG(Warning, "Auto-Updater: GitHub returned error status {}. Auto updates skipped.", statusCode);
                InternetCloseHandle(hUrl);
                InternetCloseHandle(hInternet);
                return;
            }
        }

        std::string remoteVersion = "";
        char buffer[1024];
        DWORD bytesRead;
        while (InternetReadFile(hUrl, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
            remoteVersion.append(buffer, bytesRead);
        }
        InternetCloseHandle(hUrl);
        
        remoteVersion = Trim(remoteVersion);

        // Verify that the retrieved version is strictly numeric
        if (!IsNumeric(remoteVersion)) {
            DP_LOG(Warning, "Auto-Updater: Received non-numeric version index (likely rate limited or corrupted). Update aborted.");
            InternetCloseHandle(hInternet);
            return;
        }

        // 3. Compare safely using validated numeric integers
        int localVal = std::stoi(localVersion);
        int remoteVal = std::stoi(remoteVersion);
        
        if (remoteVal <= localVal) {
            DP_LOG(Default, "Auto-Updater: Mod is up to date (Version: {})", Utils::StringToWString(localVersion));
            InternetCloseHandle(hInternet);
            return;
        }

        DP_LOG(Default, "Auto-Updater: Update found! Local: [{}] | Remote: [{}]", 
            Utils::StringToWString(localVersion), Utils::StringToWString(remoteVersion));
        DP_LOG(Default, "Auto-Updater: Downloading new main.dll...");

        std::string dllUrl = "https://raw.githubusercontent.com/GoldenCarrotMLP/dynamic-pals/main/dlls/main.dll";
        HINTERNET hDownload = InternetOpenUrlA(hInternet, dllUrl.c_str(), NULL, 0, INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE, 0);
        
        if (!hDownload) {
            DP_LOG(Warning, "Auto-Updater: Connection to GitHub DLL payload failed.");
            InternetCloseHandle(hInternet);
            return;
        }

        // Validate HTTP Status Code for the payload request
        statusCode = 0;
        length = sizeof(statusCode);
        if (HttpQueryInfoA(hDownload, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &statusCode, &length, NULL)) {
            if (statusCode != 200) {
                DP_LOG(Warning, "Auto-Updater: GitHub payload request returned status {}. Download aborted.", statusCode);
                InternetCloseHandle(hDownload);
                InternetCloseHandle(hInternet);
                return;
            }
        }

        std::vector<char> dllData;
        char dlBuffer[4096];
        DWORD dlBytesRead;
        while (InternetReadFile(hDownload, dlBuffer, sizeof(dlBuffer), &dlBytesRead) && dlBytesRead > 0) {
            dllData.insert(dllData.end(), dlBuffer, dlBuffer + dlBytesRead);
        }
        InternetCloseHandle(hDownload);

        if (dllData.empty()) {
            DP_LOG(Warning, "Auto-Updater: Downloaded DLL payload was empty. Aborting write.");
            InternetCloseHandle(hInternet);
            return;
        }

        // Swap file safely on disk
        if (MoveFileExW(currentDllPath.c_str(), oldDllPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            HANDLE hFile = CreateFileW(currentDllPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hFile != INVALID_HANDLE_VALUE) {
                DWORD bytesWritten;
                WriteFile(hFile, dllData.data(), dllData.size(), &bytesWritten, NULL);
                CloseHandle(hFile);

                // Write the verified version file index to disk
                WriteFileStr(versionTxtPath, remoteVersion);

                DP_LOG(Default, "=========================================================");
                DP_LOG(Normal, "DynPals has been auto-updated to version: {} \nPlease restart Palworld to apply the new update", Utils::StringToWString(remoteVersion));
                DP_LOG(Default, "=========================================================");
            } else {
                // Restore old working file
                MoveFileExW(oldDllPath.c_str(), currentDllPath.c_str(), MOVEFILE_REPLACE_EXISTING);
                DP_LOG(Warning, "Auto-Updater: Failed to write new main.dll payload to disk.");
            }
        } else {
            DP_LOG(Warning, "Auto-Updater: Failed to rotate old main.dll. File is locked or in use.");
        }

        InternetCloseHandle(hInternet);
    }
}