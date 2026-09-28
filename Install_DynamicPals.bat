<# :
@echo off
title Dynamic Pals ^& UE4SS Auto-Installer
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -Command "iex ((Get-Content '%~f0') -join [Environment]::NewLine)"
pause
exit /b %errorlevel%
#>
# ====================================================================================
# POWERSHELL SCRIPT CONTEXT BEGINS HERE
# ====================================================================================
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# UE4SS Version Hashes mapped to their display name and color
$UE4SS_Hashes = @{
    "8AC18FBFFC1EF96B0662D4A2D537B3F224C26D65CAABA7989A9404C566102B26" = @{ Text = "Experimental latest"; Color = "Green" }
    "D0107F63E567313CB6A15C505B5DB2BDBA38130964A04E019BDA7611C6178022" = @{ Text = "Palworld outdated"; Color = "Red" }
    "2D76EEE05CD3DE480539F1D96DA7366BF8FE445EF6B85A5C9DDD38BD503E508E" = @{ Text = "Palworld outdated"; Color = "Yellow" }
    "21B691A69A20C0801F465369D4FCBCA7D7444764022FAC2A7E8EDC7709EF92B8" = @{ Text = "Palworld latest"; Color = "Green" }
}

function Find-Palworld {
    $SteamPath = (Get-ItemPropertyValue -Path "HKCU:\Software\Valve\Steam" -Name "SteamPath" -ErrorAction SilentlyContinue)
    if ($SteamPath) {
        $VdfPath = Join-Path $SteamPath "steamapps\libraryfolders.vdf"
        if (Test-Path $VdfPath) {
            $VdfContent = Get-Content $VdfPath -Raw
            $Matches = [regex]::Matches($VdfContent, '"path"\s+"([^"]+)"')
            foreach ($Match in $Matches) {
                $LibPath = $Match.Groups[1].Value.Replace("\\", "\")
                $TestPath = Join-Path $LibPath "steamapps\common\Palworld"
                if (Test-Path $TestPath) {
                    return $TestPath
                }
            }
        }
    }
    
    $Fallback = "C:\Program Files (x86)\Steam\steamapps\common\Palworld"
    if (Test-Path $Fallback) { return $Fallback }
    return $null
}

function Get-ModStatus {
    param ($ModDir, $RemoteVersion)
    $LocalVersionFile = Join-Path $ModDir "dlls\version.txt"
    $LocalVersion = 0
    if (Test-Path $LocalVersionFile) {
        $LocalStr = ([string](Get-Content $LocalVersionFile -Raw)).Trim()
        if ($LocalStr -match '^\d+$') { $LocalVersion = [int]$LocalStr }
    }

    if ($LocalVersion -ge $RemoteVersion -and $RemoteVersion -gt 0) {
        return @{ Text = "Up to Date (v$LocalVersion)"; Color = "Green" }
    } elseif ($LocalVersion -gt 0) {
        return @{ Text = "Outdated (Local: v$LocalVersion, Remote: v$RemoteVersion)"; Color = "Red" }
    } else {
        return @{ Text = "Not Installed"; Color = "Red" }
    }
}

function Get-WorkshopUE4SSFolder($PalworldPath) {
    $PalModSettingsPath = Join-Path $PalworldPath "Mods\PalModSettings.ini"
    if (-not (Test-Path $PalModSettingsPath)) {
        return $null
    }

    $IniContent = Get-Content $PalModSettingsPath -Raw
    if ($IniContent -match "(?im)^\s*WorkshopRootDir\s*=\s*(.*)$") {
        $RawPath = $matches[1].Trim().Trim('"').Trim("'")
        
        # If blank or path does not exist, do not assume workshop
        if ([string]::IsNullOrWhiteSpace($RawPath) -or -not (Test-Path $RawPath)) {
            return $null
        }

        # If folder has no files at all, do not assume workshop
        $HasFiles = Get-ChildItem -Path $RawPath -Recurse -File -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $HasFiles) {
            return $null
        }

        # Look specifically for UE4SS.dll in any of the Workshop ID folders
        $FoundUE4SS = Get-ChildItem -Path $RawPath -Filter "UE4SS.dll" -Recurse -File -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($FoundUE4SS) {
            return $FoundUE4SS.Directory.FullName
        }
    }

    return $null
}

function Uninstall-ManualUE4SS($Win64Dir) {
    $ManualDwmapi = Join-Path $Win64Dir "dwmapi.dll"
    $ManualDll = Join-Path $Win64Dir "ue4ss\UE4SS.dll"
    $ManualRootDll = Join-Path $Win64Dir "UE4SS.dll"

    try {
        $RemovedAny = $false
        if (Test-Path $ManualDwmapi) {
            Remove-Item -Path $ManualDwmapi -Force -ErrorAction Stop
            Write-Host "Deleted: $ManualDwmapi" -ForegroundColor Green
            $RemovedAny = $true
        }
        if (Test-Path $ManualDll) {
            Remove-Item -Path $ManualDll -Force -ErrorAction Stop
            Write-Host "Deleted: $ManualDll" -ForegroundColor Green
            $RemovedAny = $true
        }
        if (Test-Path $ManualRootDll) {
            Remove-Item -Path $ManualRootDll -Force -ErrorAction Stop
            Write-Host "Deleted: $ManualRootDll" -ForegroundColor Green
            $RemovedAny = $true
        }

        if ($RemovedAny) {
            Write-Host "`nDuplicate manual UE4SS successfully uninstalled!" -ForegroundColor Green
        } else {
            Write-Host "`nNo manual UE4SS binaries found to delete." -ForegroundColor Yellow
        }
    } catch {
        Write-Host "`nFailed to remove manual UE4SS files. Ensure Palworld is closed! Error: $_" -ForegroundColor Red
    }
}

function Check-DuplicateUE4SS($PalworldPath, $Win64Dir) {
    $WorkshopUe4ssDir = Get-WorkshopUE4SSFolder $PalworldPath

    $ManualDwmapi = Join-Path $Win64Dir "dwmapi.dll"
    $ManualDll = Join-Path $Win64Dir "ue4ss\UE4SS.dll"
    $ManualRootDll = Join-Path $Win64Dir "UE4SS.dll"

    $HasWorkshop = ($null -ne $WorkshopUe4ssDir)
    $HasManual = (Test-Path $ManualDwmapi) -or (Test-Path $ManualDll) -or (Test-Path $ManualRootDll)

    if ($HasWorkshop -and $HasManual) {
        Write-Host "`n==========================================================" -ForegroundColor Red
        Write-Host "           [WARNING] DUPLICATE UE4SS DETECTED!            " -ForegroundColor Red
        Write-Host "==========================================================" -ForegroundColor Red
        Write-Host "UE4SS was detected in BOTH Steam Workshop and manual Win64 locations:" -ForegroundColor Yellow
        Write-Host " - Workshop folder: $WorkshopUe4ssDir" -ForegroundColor Gray
        Write-Host " - Win64 folder   : $Win64Dir" -ForegroundColor Gray
        Write-Host "`nRunning both versions causes Palworld to load UE4SS twice and crash!" -ForegroundColor Red

        $Choice = Read-Host "`nDo you want to uninstall the manual build by deleting 'dwmapi.dll' and 'ue4ss/UE4SS.dll'? (Y/n)"
        if ([string]::IsNullOrWhiteSpace($Choice) -or $Choice -match '^(y|yes)$') {
            Uninstall-ManualUE4SS $Win64Dir
            Start-Sleep -Seconds 2
        }
    }
}

function Resolve-UE4SSRoot($PalworldPath, $Win64Dir) {
    $WorkshopUe4ssDir = Get-WorkshopUE4SSFolder $PalworldPath

    if ($WorkshopUe4ssDir) {
        return @{
            Root       = $WorkshopUe4ssDir
            IsWorkshop = $true
        }
    }

    return @{
        Root       = (Join-Path $Win64Dir "ue4ss")
        IsWorkshop = $false
    }
}

function Install-DynamicPals($PalworldPath, $Ue4ssRoot, $ModDir, $RemoteVersion) {
    Write-Host "`nSelect Dynamic Pals Build to Install:"
    Write-Host "[1] " -NoNewline
    Write-Host "Auto-Updating" -ForegroundColor Green -NoNewline
    Write-Host " (Recommended - checks for updates on launch)"

    Write-Host "[2] " -NoNewline
    Write-Host "Offline Build" -ForegroundColor Red -NoNewline
    Write-Host " (No external network checks on launch)"

    $BuildChoice = Read-Host "Enter choice (1 or 2, default is 1)"

    $DllUrl = "https://raw.githubusercontent.com/GoldenCarrotMLP/dynamic-pals/main/dlls/main.dll"
    $DllOfflineUrl = "https://raw.githubusercontent.com/GoldenCarrotMLP/dynamic-pals/main/dlls/main_offline.dll"

    if ($BuildChoice -eq "2") {
        $SelectedDllUrl = $DllOfflineUrl
        $BuildName = "Offline Build"
        $BuildColor = "Red"
    } else {
        $SelectedDllUrl = $DllUrl
        $BuildName = "Auto-Updating"
        $BuildColor = "Green"
    }

    Write-Host "`nDownloading Dynamic Pals Base Package (Folders & Assets)..." -ForegroundColor Cyan
    
    $BaseZipUrl = "https://github.com/GoldenCarrotMLP/dynamic-pals/releases/latest/download/DynamicPals_AutoUpdate.zip"
    $ZipTempPath = Join-Path $env:TEMP "DynamicPals_Base.zip"
    $ModsDir = Join-Path $Ue4ssRoot "mods"
    
    New-Item -ItemType Directory -Force -Path $ModsDir | Out-Null

    try {
        Invoke-WebRequest -Uri $BaseZipUrl -OutFile $ZipTempPath -UseBasicParsing
        Write-Host "Extracting Base Package to: $ModsDir" -ForegroundColor Cyan
        Expand-Archive -Path $ZipTempPath -DestinationPath $ModsDir -Force
        Remove-Item $ZipTempPath -Force
    } catch {
        Write-Host "Warning: Could not download the base ZIP from GitHub Releases. Proceeding with bleeding-edge files only..." -ForegroundColor Yellow
    }

    Write-Host "Fetching Latest Bleeding-Edge DLL (" -NoNewline -ForegroundColor Cyan
    Write-Host $BuildName -ForegroundColor $BuildColor -NoNewline
    Write-Host ") and PAK (v$RemoteVersion)..." -ForegroundColor Cyan
    
    $DllDir = Join-Path $ModDir "dlls"
    $LogicModsDir = Join-Path $PalworldPath "Pal\Content\Paks\LogicMods"
    
    New-Item -ItemType Directory -Force -Path $DllDir | Out-Null
    New-Item -ItemType Directory -Force -Path $LogicModsDir | Out-Null

    $PakUrl = "https://raw.githubusercontent.com/GoldenCarrotMLP/dynamic-pals/main/dlls/DynamicPals.pak"

    $DllPath = Join-Path $DllDir "main.dll"
    $PakTemp = Join-Path $env:TEMP "DynamicPals.pak"

    try {
        Invoke-WebRequest -Uri $SelectedDllUrl -OutFile $DllPath -UseBasicParsing
        Invoke-WebRequest -Uri $PakUrl -OutFile $PakTemp -UseBasicParsing

        Set-Content -Path (Join-Path $DllDir "version.txt") -Value $RemoteVersion

        Get-ChildItem -Path $LogicModsDir -Filter "DynamicPals*.pak" | Remove-Item -Force

        $FinalPakPath = Join-Path $LogicModsDir "DynamicPals.pak"
        Move-Item -Path $PakTemp -Destination $FinalPakPath -Force

        $ModsTxt = Join-Path $Ue4ssRoot "mods\mods.txt"
        if (Test-Path $ModsTxt) {
            $Content = Get-Content $ModsTxt -Raw
            if ($Content -notmatch "(?im)^DynamicPals\s*:") {
                Add-Content -Path $ModsTxt -Value "DynamicPals : 1"
            }
        }

        Write-Host "Dynamic Pals (" -NoNewline -ForegroundColor Green
        Write-Host $BuildName -ForegroundColor $BuildColor -NoNewline
        Write-Host ") successfully installed into: $ModDir" -ForegroundColor Green
    } catch {
        Write-Host "Failed to install Dynamic Pals. Ensure the game is closed! Error: $_" -ForegroundColor Red
    }
}

function Install-UE4SS($Win64Dir, $Ue4ssRoot, $IsWorkshopLocation) {
    if ($IsWorkshopLocation) {
        Write-Host "`n[NOTE] Steam Workshop UE4SS detected." -ForegroundColor Cyan
        Write-Host "This will overwrite/update UE4SS directly in the Steam Workshop folder:" -ForegroundColor Gray
        Write-Host "Target: $Ue4ssRoot" -ForegroundColor Gray
        $Proceed = Read-Host "`nDo you want to proceed with updating the Workshop UE4SS? (Y/n)"
        if ($Proceed -match '^(n|no)$') { return }
    }

    Write-Host "`nSelect UE4SS Branch to Install:"
    Write-Host "[1] " -NoNewline
    Write-Host "Palworld-Experimental" -ForegroundColor Green -NoNewline
    Write-Host " (Recommended, stable out-of-the-box)"

    Write-Host "[2] " -NoNewline
    Write-Host "Latest-Experimental" -ForegroundColor Red -NoNewline
    Write-Host " (Upstream experimental release)"

    $Choice = Read-Host "Enter choice (1 or 2, default is 1)"
    if ([string]::IsNullOrWhiteSpace($Choice)) { $Choice = "1" }

    $Headers = @{ "User-Agent" = "PowerShell-UE4SS-Installer" }
    $Url = $null

    if ($Choice -eq "2") {
        Write-Host "Querying GitHub API for latest upstream release..." -ForegroundColor Cyan
        $ApiUrl = "https://api.github.com/repos/UE4SS-RE/RE-UE4SS/releases/latest"
        try {
            $ReleaseData = Invoke-RestMethod -Uri $ApiUrl -Headers $Headers -UseBasicParsing
            foreach ($asset in $ReleaseData.assets) {
                if ($asset.name.EndsWith(".zip") -and $asset.name -notmatch "^zDEV" -and $asset.name -notmatch "^zCustom" -and $asset.name -notmatch "^zMap") {
                    $Url = $asset.browser_download_url
                    break
                }
            }
        } catch {
            Write-Host "Failed to query upstream release API: $_" -ForegroundColor Red
            return
        }
    } else {
        Write-Host "Querying GitHub API for latest Palworld release..." -ForegroundColor Cyan
        $ApiUrl = "https://api.github.com/repos/Okaetsu/RE-UE4SS/releases/latest"
        try {
            $ReleaseData = Invoke-RestMethod -Uri $ApiUrl -Headers $Headers -UseBasicParsing

            $TargetAsset = $ReleaseData.assets |
                Where-Object { $_.name -like "*ue4ss*" -and $_.name.EndsWith(".zip") } |
                Sort-Object -Property size |
                Select-Object -First 1

            if ($TargetAsset) {
                Write-Host "Selected asset: $($TargetAsset.name) ($([math]::Round($TargetAsset.size / 1MB, 2)) MB)" -ForegroundColor Gray
                $Url = $TargetAsset.browser_download_url
            } else {
                Write-Host "Could not find a valid UE4SS zip asset in the release." -ForegroundColor Red
                return
            }
        } catch {
            Write-Host "Failed to query Okaetsu release API: $_" -ForegroundColor Red
            return
        }
    }

    if (-not $Url) {
        Write-Host "Error: No download URL could be resolved." -ForegroundColor Red
        return
    }

    Write-Host "Downloading UE4SS..." -ForegroundColor Cyan
    $ZipPath = Join-Path $env:TEMP "ue4ss.zip"
    $ExtPath = Join-Path $env:TEMP "ue4ss_ext"

    try {
        Invoke-WebRequest -Uri $Url -OutFile $ZipPath -UseBasicParsing

        if (Test-Path $ExtPath) { Remove-Item -Recurse -Force $ExtPath }
        Expand-Archive -Path $ZipPath -DestinationPath $ExtPath -Force

        Write-Host "Deploying UE4SS to: $Ue4ssRoot" -ForegroundColor Cyan
        New-Item -ItemType Directory -Force -Path $Ue4ssRoot | Out-Null

        # Check archive layout
        $OkaetsuDir = Join-Path $ExtPath "ue4ss"
        if (Test-Path $OkaetsuDir) {
            $SourceDir = $OkaetsuDir
            $ProxyDll = Join-Path $ExtPath "dwmapi.dll"
        } else {
            $SourceDir = $ExtPath
            $ProxyDll = Join-Path $ExtPath "dwmapi.dll"
        }

        # If Workshop: remove any leftover Win64 dwmapi.dll to prevent double hooking
        # If Standalone: copy dwmapi.dll to Win64
        if ($IsWorkshopLocation) {
            $OldDwmapi = Join-Path $Win64Dir "dwmapi.dll"
            if (Test-Path $OldDwmapi) {
                Remove-Item -Path $OldDwmapi -Force -ErrorAction SilentlyContinue
                Write-Host "Cleaned up standalone proxy dwmapi.dll to avoid conflicts with Workshop." -ForegroundColor Yellow
            }
        } else {
            if (Test-Path $ProxyDll) {
                Copy-Item $ProxyDll -Destination (Join-Path $Win64Dir "dwmapi.dll") -Force
            }
        }

        # Deploy UE4SS files directly into $Ue4ssRoot
        foreach ($item in Get-ChildItem $SourceDir) {
            if ($item.Name.ToLower() -eq "dwmapi.dll") {
                if (-not $IsWorkshopLocation) {
                    Copy-Item $item.FullName -Destination (Join-Path $Win64Dir "dwmapi.dll") -Force
                }
                continue
            }

            $DestPath = Join-Path $Ue4ssRoot $item.Name
            if ($item.Name.ToLower() -eq "ue4ss-settings.ini" -and (Test-Path $DestPath)) {
                Write-Host "Keeping existing UE4SS-settings.ini..." -ForegroundColor Yellow
                continue
            }
            Copy-Item $item.FullName -Destination $DestPath -Recurse -Force
        }

        # Configure mods.txt
        $ModsTxt = Join-Path $Ue4ssRoot "mods\mods.txt"
        if (Test-Path $ModsTxt) {
            $Lines = Get-Content $ModsTxt
            $Mandatory = @{
                "cheatmanagerenablermod" = "CheatManagerEnablerMod"; "consolecommandsmod" = "ConsoleCommandsMod"
                "consoleenablermod" = "ConsoleEnablerMod"; "bpml_genericfunctions" = "BPML_GenericFunctions"
                "bpmodloadermod" = "BPModLoaderMod"
            }
            $ModifiedLines = @()
            $FoundKeys = @()

            foreach ($line in $Lines) {
                if ($line -match "^\s*([^:]+)\s*:") {
                    $Key = $matches[1].Trim().ToLower()
                    if ($Mandatory.ContainsKey($Key)) {
                        $ModifiedLines += "$($Mandatory[$Key]) : 1"
                        $FoundKeys += $Key
                        continue
                    }
                }
                $ModifiedLines += $line
            }

            foreach ($key in $Mandatory.Keys) {
                if ($key -notin $FoundKeys) {
                    $ModifiedLines = ,"$($Mandatory[$key]) : 1" + $ModifiedLines
                }
            }
            Set-Content -Path $ModsTxt -Value $ModifiedLines -Encoding UTF8
        }

        Remove-Item $ZipPath -Force
        Remove-Item $ExtPath -Recurse -Force
        Write-Host "UE4SS installed successfully!" -ForegroundColor Green
    } catch {
        Write-Host "Failed to install UE4SS. Ensure the game is closed! Error: $_" -ForegroundColor Red
    }
}

# --- MAIN EXECUTION ---
Clear-Host
Write-Host "=========================================================="
Write-Host "         Dynamic Pals & UE4SS Auto-Installer Wizard       "
Write-Host "=========================================================="

$PalworldPath = Find-Palworld
if (-not $PalworldPath) {
    Write-Host "`nCould not auto-detect Palworld." -ForegroundColor Yellow
    $InputPath = Read-Host "Please paste the path to your 'Palworld.exe' folder"
    $PalworldPath = $InputPath.Trim().Trim('"').Trim("'")
}

$Win64Dir = Join-Path $PalworldPath "Pal\Binaries\Win64"
if (-not (Test-Path $Win64Dir)) {
    Write-Host "`nError: Could not resolve 'Win64' binaries folder from the provided path. Aborting." -ForegroundColor Red
    exit
}

# Run duplicate installation detection and offer cleanup
Check-DuplicateUE4SS $PalworldPath $Win64Dir

$RemoteVersionUrl = "https://raw.githubusercontent.com/GoldenCarrotMLP/dynamic-pals/refs/heads/main/dlls/version.txt"
$RemoteResponse = Invoke-RestMethod -Uri $RemoteVersionUrl -UseBasicParsing -ErrorAction SilentlyContinue
$RemoteVersionStr = ([string]$RemoteResponse).Trim()

$RemoteVersion = 0
if ($RemoteVersionStr -match '^\d+$') { $RemoteVersion = [int]$RemoteVersionStr }

while ($true) {
    # Dynamically resolve active root & mode
    $Config = Resolve-UE4SSRoot $PalworldPath $Win64Dir
    $Ue4ssRoot = $Config.Root
    $IsWorkshopLocation = $Config.IsWorkshop
    $ModDir = Join-Path $Ue4ssRoot "mods\DynamicPals"

    # Check if a duplicate exists currently
    $ManualDwmapi = Join-Path $Win64Dir "dwmapi.dll"
    $ManualDll = Join-Path $Win64Dir "ue4ss\UE4SS.dll"
    $ManualRootDll = Join-Path $Win64Dir "UE4SS.dll"

    $HasWorkshop = $IsWorkshopLocation -and (Test-Path (Join-Path $Ue4ssRoot "UE4SS.dll"))
    $HasManual = (Test-Path $ManualDwmapi) -or (Test-Path $ManualDll) -or (Test-Path $ManualRootDll)
    $IsDuplicate = ($HasWorkshop -and $HasManual)

    Write-Host "`n======================= STATUS ==========================="
    if ($IsWorkshopLocation) {
        Write-Host "Mode       : Steam Workshop Managed" -ForegroundColor Cyan
        Write-Host "UE4SS Root : $Ue4ssRoot" -ForegroundColor Gray
        Write-Host "Mod Target : $ModDir" -ForegroundColor Gray
    } else {
        Write-Host "Mode       : Standalone / Manual" -ForegroundColor Gray
        Write-Host "Game Path  : $PalworldPath" -ForegroundColor Gray
        Write-Host "Mod Target : $ModDir" -ForegroundColor Gray
    }
    
    # Check UE4SS Status
    $Ue4ssDll = Join-Path $Ue4ssRoot "UE4SS.dll"
    if (Test-Path $Ue4ssDll) {
        $Hash = (Get-FileHash -Path $Ue4ssDll -Algorithm SHA256).Hash
        if ($UE4SS_Hashes.ContainsKey($Hash)) {
            $HashInfo = $UE4SS_Hashes[$Hash]
            Write-Host "UE4SS      : Installed ($($HashInfo.Text))" -ForegroundColor $HashInfo.Color
        } else {
            Write-Host "UE4SS      : Installed (Custom Build)" -ForegroundColor Yellow
        }
    } else {
        Write-Host "UE4SS      : Not Installed" -ForegroundColor Red
    }

    # Check Mod Status
    $StatusInfo = Get-ModStatus $ModDir $RemoteVersion
    Write-Host "DynamicPals: $($StatusInfo.Text)" -ForegroundColor $StatusInfo.Color

    if ($IsDuplicate) {
        Write-Host "Warning    : DUPLICATE UE4SS DETECTED (Workshop + Win64)!" -ForegroundColor Red
    }
    Write-Host "=========================================================="

    if ($IsWorkshopLocation) {
        Write-Host "`n[NOTICE] Workshop UE4SS detected. DynamicPals will install into the Workshop folder." -ForegroundColor Cyan
    }

    Write-Host "`nSelect an option:"
    Write-Host "[1] Install / Update UE4SS"
    Write-Host "[2] Install / Update Dynamic Pals"
    if ($IsDuplicate) {
        Write-Host "[D] " -NoNewline
        Write-Host "Uninstall Duplicate Manual UE4SS" -ForegroundColor Red -NoNewline
        Write-Host " (Fix duplicate conflict)"
    }
    Write-Host "[3] Exit"

    $Choice = Read-Host "`nEnter option"

    if ($Choice -eq "1") {
        Install-UE4SS $Win64Dir $Ue4ssRoot $IsWorkshopLocation
        Read-Host "`nPress Enter to return to menu..."
        Clear-Host
    } elseif ($Choice -eq "2") {
        if ($RemoteVersion -gt 0) {
            Install-DynamicPals $PalworldPath $Ue4ssRoot $ModDir $RemoteVersion
        } else {
            Write-Host "Could not fetch remote version from GitHub. Check your internet connection." -ForegroundColor Red
        }
        Read-Host "`nPress Enter to return to menu..."
        Clear-Host
    } elseif ($IsDuplicate -and ($Choice -eq "d" -or $Choice -eq "D")) {
        Uninstall-ManualUE4SS $Win64Dir
        Read-Host "`nPress Enter to return to menu..."
        Clear-Host
    } elseif ($Choice -eq "3") {
        exit
    } else {
        Write-Host "Invalid option." -ForegroundColor Yellow
    }
}