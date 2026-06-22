@echo off
setlocal

:: Resolve the absolute directory path of this script to bypass PowerShell's relative-path bugs
for %%I in ("%~f0") do set "SCRIPT_DIR=%%~dpI"
cd /d "%SCRIPT_DIR%"

title DynamicPals Standalone Updater

echo =========================================
echo        DynamicPals Standalone Updater
echo =========================================
echo.

:: Safety check: Ensure the user ran this in the correct folder
if exist main.dll goto SkipSafetyCheck

echo [WARNING] main.dll was not found in this folder.
echo Please ensure this update.bat is placed inside your mod's DLL directory:
echo e.g., ...\Pal\Binaries\Win64\Mods\DynamicPals\dlls\
echo.
set "CHOICE="
set /p CHOICE="Do you want to download the mod files into this folder anyway? (Y/N): "
if not defined CHOICE goto CancelUpdate
if /i "%CHOICE%" neq "Y" goto CancelUpdate
goto SkipSafetyCheck

:CancelUpdate
echo Update cancelled.
pause
exit /b

:SkipSafetyCheck

:: Check if curl is available
where curl >nul 2>nul
if errorlevel 1 goto CurlMissing
goto CurlAvailable

:CurlMissing
echo [ERROR] 'curl' is not installed or not in your PATH.
echo This updater requires curl (standard on Windows 10/11).
pause
exit /b

:CurlAvailable

:: Read local version if it exists
set LOCAL_VERSION=0
if not exist version.txt goto FetchRemote
set /p LOCAL_VERSION=<version.txt
:: Clean up any trailing spaces/newlines
for /f "tokens=*" %%a in ("%LOCAL_VERSION%") do set LOCAL_VERSION=%%a

:FetchRemote
echo Local Version:  [%LOCAL_VERSION%]
echo Fetching latest version from GitHub...

:: Fetch remote version to a temp file
if exist temp_version.txt del temp_version.txt
curl -s -f -o temp_version.txt "https://raw.githubusercontent.com/GoldenCarrotMLP/dynamic-pals/main/dlls/version.txt"

if not exist temp_version.txt goto FetchFailed
goto FetchSuccess

:FetchFailed
echo [ERROR] Failed to fetch the remote version file.
echo Please check your internet connection or repository URL.
pause
exit /b

:FetchSuccess
set /p REMOTE_VERSION=<temp_version.txt
for /f "tokens=*" %%a in ("%REMOTE_VERSION%") do set REMOTE_VERSION=%%a
del temp_version.txt

echo Remote Version: [%REMOTE_VERSION%]
echo.

if "%LOCAL_VERSION%"=="%REMOTE_VERSION%" goto AlreadyUpToDate
goto RunUpdateCheck

:AlreadyUpToDate
echo [INFO] You are already up to date!
pause
exit /b

:RunUpdateCheck
:: Verify the game is not running (prevents file locking issues)
tasklist /FI "IMAGENAME eq Palworld-Win64-Shipping.exe" 2>NUL | find /I /N "Palworld-Win64-Shipping.exe" >NUL
if errorlevel 1 goto GameNotRunning

echo.
echo [CRITICAL ERROR] Palworld is currently running!
echo Please close the game before running this standalone updater to prevent file lock errors.
echo.
pause
exit /b

:GameNotRunning
echo [UPDATE] A new update is available.
echo Downloading main.dll...

:: Download the new DLL to a temp file
curl -s -f -o main.dll.tmp "https://raw.githubusercontent.com/GoldenCarrotMLP/dynamic-pals/main/dlls/main.dll"

if not exist main.dll.tmp goto DownloadFailed
goto DownloadSuccess

:DownloadFailed
echo [ERROR] Failed to download the updated main.dll.
pause
exit /b

:DownloadSuccess
:: Perform clean replace
if exist main.dll del main.dll
rename main.dll.tmp main.dll

:: Write version.txt without a trailing newline (this matches standard C++ std::ofstream formatting)
<nul set /p="%REMOTE_VERSION%">version.txt

echo.
echo =========================================
echo [SUCCESS] DynamicPals successfully updated to version [%REMOTE_VERSION%]!
echo =========================================
pause