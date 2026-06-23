param (
    [Parameter(Mandatory=$true)]
    [string]$SourceDir
)

$file = Get-Content -Raw "$SourceDir/include/DataTypes.hpp"

# Phase 1: Parse out TypeScript-style alias type constraints
$aliases = @{}
foreach ($line in ($file -split '\r?\n')) {
    if ($line -match 'using\s+([a-zA-Z0-9_]+)\s*=\s*[^;]+;\s*//\s*OPTIONS:\s*(.*)$') {
        $aliases[$Matches[1].Trim()] = $Matches[2].Trim()
    }
}

# Phase 2: Parse struct variables and map schemas
if ($file -match '(?s)struct\s+SwapConfig\s*\{(.*?)\};') {
    $body = $Matches[1]
    $json = [ordered]@{}
    foreach ($line in ($body -split '\r?\n')) {
        $line = $line -replace '//.*$', ''
        if ($line -match '^\s*([a-zA-Z0-9_:<>]+)\s+([a-zA-Z0-9_]+)(?:\s*=\s*([^;]+))?;') {
            $type = $Matches[1].Trim()
            $name = $Matches[2].Trim()
            $val = if ($Matches[3]) { $Matches[3].Trim() } else { $null }
            
            if ($name -eq 'PackName') { continue }
            
            $optionVal = if ($aliases.ContainsKey($type)) { $aliases[$type] = $aliases[$type] } else { $null }
            
            if ($type -like '*std::vector*') {
                if ($type -like '*MatReplace*') {
                    $json[$name] = ,@{'MatPath'='/Game/MaterialPath'; 'Index'='0'}
                } elseif ($type -like '*MorphTarget*') {
                    $morphTypeVal = if ($aliases.ContainsKey('MorphType')) { $aliases['MorphType'] } else { 'Free' }
                    $json[$name] = ,@{'Max'=1; 'Min'=0; 'Target'='MorphName'; 'Type'=$morphTypeVal; 'Set'=1}
                } else {
                    $json[$name] = ,('TraitExample')
                }
            } elseif ($type -like '*std::optional*') {
                if ($type -like '*bool*') {
                    $json[$name] = $false 
                } else {
                    $json[$name] = $null
                }
            } elseif ($type -eq 'int') {
                $json[$name] = if ($val) { [int]$val } else { 0 }
            } elseif ($type -eq 'double') {
                $json[$name] = if ($val) { [double]$val } else { 0.0 }
            } else {
                if ($optionVal) {
                    $json[$name] = $optionVal
                } else {
                    $json[$name] = if ($val) { $val.Replace('L"', '').Replace('"', '') } else { '' }
                }
            }
        }
    }
    
    # Ensure the DLLs directory exists
    if (-not (Test-Path "$SourceDir/dlls")) {
        New-Item -ItemType Directory -Force -Path "$SourceDir/dlls" | Out-Null
    }

    # -------------------------------------------------------------
    # BUILD VERSION 1 TEMPLATE
    # -------------------------------------------------------------
    $rootLayoutV1 = [ordered]@{
        'PackName' = 'Your Replacer Pack Name'
        'SkelMeshSwap' = ,$json
    }
    $finalV1 = $rootLayoutV1 | ConvertTo-Json -Depth 5
    Set-Content -Path "$SourceDir/Docs/_Template_SwapConfig.json" -Value $finalV1
    
    # -------------------------------------------------------------
    # BUILD VERSION 2 TEMPLATE
    # -------------------------------------------------------------
    $v2SkinData = [ordered]@{}
    $v2SkinData['SkinPath'] = ''
    $v2SkinData['AnimTarget'] = ''
    $v2SkinData['Gender'] = 'None'
    $v2SkinData['MinLevel'] = 1
    $v2SkinData['MaxLevel'] = 999
    $v2SkinData['MinTrust'] = 0
    $v2SkinData['MaxTrust'] = 999999
    $v2SkinData['MinRank'] = 0
    $v2SkinData['MaxRank'] = 5
    $v2SkinData['SpawnWeight'] = 1
    $v2SkinData['LuckyStarReq'] = 'false'
    $v2SkinData['IsWildPal'] = 'false'
    $v2SkinData['PassiveSkills'] = ,('TraitExample')
    $v2SkinData['PrefTrait'] = ,('TraitExample')
    $v2SkinData['SkipTrait'] = ,('TraitExample')
    
    $matObj = [ordered]@{
        'MaterialAsset' = '/Game/MaterialPath'
        'Index' = '0'
    }
    $v2SkinData['SpecialMaterial'] = ,$matObj

    $morphObj = [ordered]@{
        'Min' = 0
        'Name' = 'MorphName'
        'Max' = 1
        'Mode' = 'Restrictive, Free, None'
        'Set' = 1
    }
    $v2SkinData['ShapeKeys'] = ,$morphObj
    $v2SkinData['Extra'] = '{}'

    $v2CharMap = [ordered]@{
        'Your Skin Label' = $v2SkinData
    }

    $v2SkinList = [ordered]@{
        'CharacterID_Example' = $v2CharMap
    }

    $v2RootLayout = [ordered]@{
        'ModelPack' = 'Your Replacer Pack Name'
        'SkinList' = $v2SkinList
    }

    $v2final = $v2RootLayout | ConvertTo-Json -Depth 6
    Set-Content -Path "$SourceDir/Docs/_Template_SwapConfig_V2.json" -Value $v2final
}