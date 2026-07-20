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

$TargetUE4SSHash = "D0107F63E567313CB6A15C505B5DB2BDBA38130964A04E019BDA7611C6178022"

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

function Install-DynamicPals($PalworldPath, $Win64Dir, $ModDir, $RemoteVersion) {
    Write-Host "`nDownloading Dynamic Pals v$RemoteVersion..." -ForegroundColor Cyan
    $DllDir = Join-Path $ModDir "dlls"
    $LogicModsDir = Join-Path $PalworldPath "Pal\Content\Paks\LogicMods"
    
    New-Item -ItemType Directory -Force -Path $DllDir | Out-Null
    New-Item -ItemType Directory -Force -Path $LogicModsDir | Out-Null

    $DllUrl = "https://raw.githubusercontent.com/GoldenCarrotMLP/dynamic-pals/main/dlls/main.dll"
    $PakUrl = "https://raw.githubusercontent.com/GoldenCarrotMLP/dynamic-pals/main/dlls/DynamicPals.pak"

    $DllPath = Join-Path $DllDir "main.dll"
    $PakTemp = Join-Path $env:TEMP "DynamicPals.pak"

    try {
        Invoke-WebRequest -Uri $DllUrl -OutFile $DllPath -UseBasicParsing
        Invoke-WebRequest -Uri $PakUrl -OutFile $PakTemp -UseBasicParsing

        Set-Content -Path (Join-Path $DllDir "version.txt") -Value $RemoteVersion

        # Clean old LogicMod Paks
        Get-ChildItem -Path $LogicModsDir -Filter "DynamicPals*.pak" | Remove-Item -Force

        # Format Pak name to zero-padded number for UE alphabetical priority (e.g. DynamicPals_V0106.pak)
        $FormattedVersion = "{0:D4}" -f $RemoteVersion
        $FinalPakPath = Join-Path $LogicModsDir "DynamicPals_V$FormattedVersion.pak"
        Move-Item -Path $PakTemp -Destination $FinalPakPath -Force

        # Ensure DynamicPals is enabled in mods.txt
        $ModsTxt = Join-Path $Win64Dir "ue4ss\Mods\mods.txt"
        if (Test-Path $ModsTxt) {
            $Content = Get-Content $ModsTxt -Raw
            if ($Content -notmatch "(?im)^DynamicPals\s*:") {
                Add-Content -Path $ModsTxt -Value "DynamicPals : 1"
            }
        }

        Write-Host "Dynamic Pals successfully installed!" -ForegroundColor Green
    } catch {
        Write-Host "Failed to install Dynamic Pals. Ensure the game is closed! Error: $_" -ForegroundColor Red
    }
}

function Install-UE4SS($Win64Dir) {
    Write-Host "`nSelect UE4SS Branch to Install:"
    Write-Host "[1] Palworld-Experimental (Recommended, stable out-of-the-box)"
    Write-Host "[2] Latest-Experimental (Upstream experimental release)"
    $Choice = Read-Host "Enter choice (1 or 2)"

    $Url = "https://github.com/Okaetsu/RE-UE4SS/releases/download/experimental-palworld/UE4SS-Palworld.zip"
    if ($Choice -eq "2") {
        Write-Host "Querying GitHub API for latest upstream release..." -ForegroundColor Cyan
        $ApiUrl = "https://api.github.com/repos/UE4SS-RE/RE-UE4SS/releases/tags/experimental-latest"
        $ReleaseData = Invoke-RestMethod -Uri $ApiUrl
        foreach ($asset in $ReleaseData.assets) {
            if ($asset.name.EndsWith(".zip") -and $asset.name -notmatch "^zDEV" -and $asset.name -notmatch "^zCustom" -and $asset.name -notmatch "^zMap") {
                $Url = $asset.browser_download_url
                break
            }
        }
    }

    Write-Host "Downloading UE4SS..." -ForegroundColor Cyan
    $ZipPath = Join-Path $env:TEMP "ue4ss.zip"
    $ExtPath = Join-Path $env:TEMP "ue4ss_ext"

    try {
        Invoke-WebRequest -Uri $Url -OutFile $ZipPath -UseBasicParsing

        if (Test-Path $ExtPath) { Remove-Item -Recurse -Force $ExtPath }
        Expand-Archive -Path $ZipPath -DestinationPath $ExtPath -Force

        Write-Host "Deploying UE4SS..." -ForegroundColor Cyan
        $TargetUe4ssDir = Join-Path $Win64Dir "ue4ss"
        New-Item -ItemType Directory -Force -Path $TargetUe4ssDir | Out-Null

        $OkaetsuDir = Join-Path $ExtPath "ue4ss"
        if (Test-Path $OkaetsuDir) {
            $DwmApi = Join-Path $ExtPath "dwmapi.dll"
            if (Test-Path $DwmApi) { Copy-Item $DwmApi -Destination (Join-Path $Win64Dir "dwmapi.dll") -Force }
            Copy-Item "$OkaetsuDir\*" -Destination $TargetUe4ssDir -Recurse -Force
        } else {
            foreach ($item in Get-ChildItem $ExtPath) {
                if ($item.Name.ToLower() -eq "dwmapi.dll") {
                    Copy-Item $item.FullName -Destination (Join-Path $Win64Dir "dwmapi.dll") -Force
                } else {
                    Copy-Item $item.FullName -Destination $TargetUe4ssDir -Recurse -Force
                }
            }
        }

        # Configure mods.txt
        $ModsTxt = Join-Path $TargetUe4ssDir "Mods\mods.txt"
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

$ModDir = Join-Path $Win64Dir "ue4ss\Mods\DynamicPals"
$RemoteVersionUrl = "https://raw.githubusercontent.com/GoldenCarrotMLP/dynamic-pals/refs/heads/main/dlls/version.txt"

# Resolve network payload safely by casting to a standard string
$RemoteResponse = Invoke-RestMethod -Uri $RemoteVersionUrl -UseBasicParsing -ErrorAction SilentlyContinue
$RemoteVersionStr = ([string]$RemoteResponse).Trim()

$RemoteVersion = 0
if ($RemoteVersionStr -match '^\d+$') { $RemoteVersion = [int]$RemoteVersionStr }

while ($true) {
    Write-Host "`n======================= STATUS ==========================="
    Write-Host "Game Path  : $PalworldPath" -ForegroundColor Gray
    
    # Check UE4SS Status
    $Ue4ssDll = Join-Path $Win64Dir "ue4ss\UE4SS.dll"
    if (Test-Path $Ue4ssDll) {
        $Hash = (Get-FileHash -Path $Ue4ssDll -Algorithm SHA256).Hash
        if ($Hash -eq $TargetUE4SSHash) {
            Write-Host "UE4SS      : Up to Date" -ForegroundColor Green
        } else {
            Write-Host "UE4SS      : Outdated or Custom Build" -ForegroundColor Red
        }
    } else {
        Write-Host "UE4SS      : Not Installed" -ForegroundColor Red
    }

    # Check Mod Status
    $StatusInfo = Get-ModStatus $ModDir $RemoteVersion
    Write-Host "DynamicPals: $($StatusInfo.Text)" -ForegroundColor $StatusInfo.Color
    Write-Host "=========================================================="

    Write-Host "`nSelect an option:"
    Write-Host "[1] Install / Update UE4SS"
    Write-Host "[2] Install / Update Dynamic Pals"
    Write-Host "[3] Exit"

    $Choice = Read-Host "`nEnter option"

    if ($Choice -eq "1") {
        Install-UE4SS $Win64Dir
        Read-Host "`nPress Enter to return to menu..."
        Clear-Host
    } elseif ($Choice -eq "2") {
        if ($RemoteVersion -gt 0) {
            Install-DynamicPals $PalworldPath $Win64Dir $ModDir $RemoteVersion
        } else {
            Write-Host "Could not fetch remote version from GitHub. Check your internet connection." -ForegroundColor Red
        }
        Read-Host "`nPress Enter to return to menu..."
        Clear-Host
    } elseif ($Choice -eq "3") {
        exit
    } else {
        Write-Host "Invalid option." -ForegroundColor Yellow
    }
}