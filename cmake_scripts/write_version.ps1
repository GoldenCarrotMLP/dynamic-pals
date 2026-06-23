param (
    [Parameter(Mandatory=$true)]
    [string]$SourceDir
)

$versionFile = "$SourceDir/dlls/version.txt"

if (-not (Test-Path "$SourceDir/dlls")) {
    New-Item -ItemType Directory -Force -Path "$SourceDir/dlls" | Out-Null
}

try {
    # Get commit count using git
    $commitCount = (git -C "$SourceDir" rev-list --count HEAD 2>$null).Trim()
    if ([string]::IsNullOrWhiteSpace($commitCount)) {
        $commitCount = "0"
    }
} catch {
    $commitCount = "0"
}

Set-Content -Path $versionFile -Value $commitCount