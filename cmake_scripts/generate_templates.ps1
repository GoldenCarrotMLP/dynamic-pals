param (
    [Parameter(Mandatory=$true)]
    [string]$SourceDir
)

# Clean JSON Prettifier Helper (Forces standard, uniform 2-space indentation)
function Format-JsonClean {
    param (
        [string]$json,
        [int]$indentSize = 2
    )
    $indent = ""
    $result = New-Object System.Text.StringBuilder
    $inQuote = $false
    $esc = $false

    for ($i = 0; $i -lt $json.Length; $i++) {
        $char = $json[$i]
        if ($esc) {
            [void]$result.Append($char)
            $esc = $false
            continue
        }
        if ($char -eq '\') {
            [void]$result.Append($char)
            $esc = $true
            continue
        }
        if ($char -eq '"') {
            [void]$result.Append($char)
            $inQuote = -not $inQuote
            continue
        }
        if ($inQuote) {
            [void]$result.Append($char)
            continue
        }

        if ($char -eq '{' -or $char -eq '[') {
            [void]$result.Append($char)
            [void]$result.Append([Environment]::NewLine)
            $indent += " " * $indentSize
            [void]$result.Append($indent)
        }
        elseif ($char -eq '}' -or $char -eq ']') {
            [void]$result.Append([Environment]::NewLine)
            if ($indent.Length -ge $indentSize) {
                $indent = $indent.Substring(0, $indent.Length - $indentSize)
            }
            [void]$result.Append($indent)
            [void]$result.Append($char)
        }
        elseif ($char -eq ',') {
            [void]$result.Append($char)
            [void]$result.Append([Environment]::NewLine)
            [void]$result.Append($indent)
        }
        elseif ($char -eq ':') {
            [void]$result.Append(": ")
        }
        elseif ($char -eq ' ' -or $char -eq "`t" -or $char -eq "`n" -or $char -eq "`r") {
            # Skip extra whitespace outside of quotes
        }
        else {
            [void]$result.Append($char)
        }
    }
    $formatted = $result.ToString()
    # Collapse empty objects and arrays back onto a single line (e.g. [] or {} instead of spanning lines)
    $formatted = $formatted -replace '\[\s+\]', '[]' -replace '\{\s+\}', '{}'
    return $formatted
}

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
            
            # Align C++ Struct array names with the JSON array keys ConfigManager actually expects
            if ($name -eq 'MatReplaceList') { $name = 'MatReplace' }
            if ($name -eq 'MorphTargetList') { $name = 'MorphTarget' }
            
            $optionVal = if ($aliases.ContainsKey($type)) { $aliases[$type] = $aliases[$type] } else { $null }
            
            if ($type -like '*std::vector*') {
                if ($type -like '*MatReplace*') {
                    $json[$name] = ,@{'MatPath'='/Game/MaterialPath'; 'RandomHue'=$false; 'Index'='0'}
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
    
    # Ensure Docs directory exists
    if (-not (Test-Path "$SourceDir/Docs")) {
        New-Item -ItemType Directory -Force -Path "$SourceDir/Docs" | Out-Null
    }

    # -------------------------------------------------------------
    # BUILD VERSION 1 TEMPLATE
    # -------------------------------------------------------------
    $rootLayoutV1 = [ordered]@{
        'PackName' = 'Your Replacer Pack Name'
        'SkelMeshSwap' = ,$json
    }
    
    # Compress JSON first to clear out native system whitespace
    $compressedV1 = $rootLayoutV1 | ConvertTo-Json -Depth 8 -Compress
    $finalV1 = Format-JsonClean -json $compressedV1 -indentSize 2
    Set-Content -Path "$SourceDir/Docs/_Template_SwapConfig.json" -Value $finalV1
    
    # -------------------------------------------------------------
    # BUILD VERSION 2 TEMPLATE (Dynamically Extracted)
    # -------------------------------------------------------------
    $v2SkinData = [ordered]@{}
    
    foreach ($key in $json.Keys) {
        # Skip structural keys that dictate V2's JSON hierarchy instead of payload
        if ($key -eq 'CharacterID' -or $key -eq 'SwapLabel') {
            continue
        }
        
        $v2Key = $key
        $v2Val = $json[$key]
        
        # Translate V1 schema names to V2 schema names based on ConfigManager.cpp
        if ($key -eq 'SkelMeshPath') {
            $v2Key = 'SkinPath'
        } elseif ($key -eq 'IsRarePal') {
            $v2Key = 'LuckyStarReq'
        } elseif ($key -eq 'ReqTrait') {
            $v2Key = 'PassiveSkills'
        } elseif ($key -eq 'MatReplace') {
            $v2Key = 'SpecialMaterial'
            $v2Val = ,@{'MaterialAsset'='/Game/MaterialPath'; 'Index'='0'; 'RandomHue'=$false}
        } elseif ($key -eq 'MorphTarget') {
            $v2Key = 'ShapeKeys'
            $morphTypeVal = if ($aliases.ContainsKey('MorphType')) { $aliases['MorphType'] } else { 'Free' }
            $v2Val = ,@{'Min'=0; 'Name'='MorphName'; 'Max'=1; 'Mode'=$morphTypeVal; 'Set'=1}
        }
        
        $v2SkinData[$v2Key] = $v2Val
    }

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

    # Compress JSON first to clear out native system whitespace
    $compressedV2 = $v2RootLayout | ConvertTo-Json -Depth 8 -Compress
    $v2final = Format-JsonClean -json $compressedV2 -indentSize 2
    Set-Content -Path "$SourceDir/Docs/_Template_SwapConfig_V2.json" -Value $v2final
}