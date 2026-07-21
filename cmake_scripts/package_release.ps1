param (
    [Parameter(Mandatory=$true)]
    [string]$SourceDir
)

$AutoUpdateZip = "$SourceDir/DynamicPals_AutoUpdate.zip"
$OfflineZip = "$SourceDir/DynamicPals.zip"

# 1. Clean up any existing releases in the root directory
foreach ($zip in @($AutoUpdateZip, $OfflineZip)) {
    if (Test-Path $zip) { 
        Remove-Item $zip -Force 
    }
}

# 2. Setup isolated staging environments in the temp directory
$TempRoot = Join-Path $env:TEMP "DynamicPals_PackStage"
if (Test-Path $TempRoot) { 
    Remove-Item $TempRoot -Recurse -Force 
}

$StageAuto = Join-Path $TempRoot "AutoUpdate/DynamicPals"
$StageOffline = Join-Path $TempRoot "Offline/DynamicPals"

New-Item -ItemType Directory -Force -Path "$StageAuto/dlls" | Out-Null
New-Item -ItemType Directory -Force -Path "$StageOffline/dlls" | Out-Null

# 3. Synchronize config files and metadata
$EnabledFile = "$SourceDir/enabled.txt"
if (-not (Test-Path $EnabledFile)) {
    Set-Content -Path $EnabledFile -Value "1"
}

Copy-Item $EnabledFile -Destination "$StageAuto/enabled.txt" -Force
Copy-Item $EnabledFile -Destination "$StageOffline/enabled.txt" -Force

# Locate Docs safely (handling potential casing differences)
$DocsDir = ""
if (Test-Path "$SourceDir/Docs") { $DocsDir = "$SourceDir/Docs" }
elseif (Test-Path "$SourceDir/docs") { $DocsDir = "$SourceDir/docs" }

if (-not [string]::IsNullOrEmpty($DocsDir)) {
    Copy-Item -Path $DocsDir -Destination "$StageAuto/Docs" -Recurse -Force
    Copy-Item -Path $DocsDir -Destination "$StageOffline/Docs" -Recurse -Force
}

# Locate vfx directory safely (handling potential casing differences)
$VFXDir = ""
if (Test-Path "$SourceDir/VFX") { $VFXDir = "$SourceDir/VFX" }
elseif (Test-Path "$SourceDir/vfx") { $VFXDir = "$SourceDir/vfx" }

if (-not [string]::IsNullOrEmpty($VFXDir)) {
    Copy-Item -Path $VFXDir -Destination "$StageAuto/vfx" -Recurse -Force
    Copy-Item -Path $VFXDir -Destination "$StageOffline/vfx" -Recurse -Force
}

# Distribute generated configs, paks, and version tracking
foreach ($stage in @($StageAuto, $StageOffline)) {
    Copy-Item "$SourceDir/dlls/version.txt" -Destination "$stage/dlls/version.txt" -Force
    
    # Copy DynamicPals.pak asset package into the staged dlls folder
    if (Test-Path "$SourceDir/dlls/DynamicPals.pak") {
        Copy-Item "$SourceDir/dlls/DynamicPals.pak" -Destination "$stage/dlls/DynamicPals.pak" -Force
    }
    
    if (Test-Path "$SourceDir/dlls/_Template_SwapConfig.json") {
        Copy-Item "$SourceDir/dlls/_Template_SwapConfig.json" -Destination "$stage/dlls/_Template_SwapConfig.json" -Force
    }
    if (Test-Path "$SourceDir/dlls/_Template_SwapConfig_V2.json") {
        Copy-Item "$SourceDir/dlls/_Template_SwapConfig_V2.json" -Destination "$stage/dlls/_Template_SwapConfig_V2.json" -Force
    }
}

# 4. Handle Dual-DLL renaming rules
# AutoUpdate gets standard main.dll
Copy-Item "$SourceDir/dlls/main.dll" -Destination "$StageAuto/dlls/main.dll" -Force

# Offline gets main_offline.dll renamed to main.dll
Copy-Item "$SourceDir/dlls/main_offline.dll" -Destination "$StageOffline/dlls/main.dll" -Force

# 5. Compress into production-ready ZIPs
# Compressing the folder itself ensures correct directory nesting during extraction
Compress-Archive -Path (Join-Path $TempRoot "AutoUpdate/DynamicPals") -DestinationPath $AutoUpdateZip -Force
Compress-Archive -Path (Join-Path $TempRoot "Offline/DynamicPals") -DestinationPath $OfflineZip -Force

# 6. Purge temporary files
if (Test-Path $TempRoot) { 
    Remove-Item $TempRoot -Recurse -Force 
}