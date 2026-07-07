# Version 1 JSON Format Specification

Version 1 is an array-based format originally created by Michael, but now heavily modified to fit dynamic-pals amplitude of features, where every replacement entry is explicitly defined as an individual object inside the `"SkelMeshSwap"` array.


## Root Schema
* `PackName` *(String, Optional)*: Descriptive label for UI display.
* `SkelMeshSwap` *(Array, Required)*: A list of individual replacement rules.

---

## Property Reference

| Property | Type | Required | Description / Options |
| :--- | :--- | :---: | :--- |
| **`CharacterID`** | String | **Yes** | Internal unique ID of the target Pal (e.g., `"PinkCat"`). |
| **`SkelMeshPath`** | String | **Yes** | Game-internal path to the skeletal mesh asset. |
| **`AnimTarget`** | String | No | Target Pal ID or asset path to copy animations from. |
| **`SetNickname`** | String | No | Changes the nickname on the pal upon reaching this swap. |
| **`Gender`** | String | No | `"Male"`, `"Female"`, `"None"`, `"Futa"`, `"FullFuta"`, `"Andro"`, `"Neutered"`, `"FullNeutered"` |
| **`SkinName`** | String | No | Game-native Skin ID to overwrite. |
| **`SkinLabel`** | String | No | Descriptive name for this specific skin to display in supported UI menus. |
| **`MinLevel`** / **`MaxLevel`** | Integer | No | Minimum (default 1) and Maximum (default 999) level requirements. |
| **`MinTrust`** / **`MaxTrust`** | Integer | No | Minimum (default 0) and Maximum (default 999999) friendship/trust requirements. |
| **`MinRank`** / **`MaxRank`** | Integer | No | Minimum (default 0) and Maximum (default 5) star-rank requirements. |
| **`SpawnWeight`** | Integer | No | Relative probability weight for weighted random selection on ties (default 1). |
| **`IsRarePal`** | Boolean | No | `true` or `false` (Forces match only on Lucky/Rare status). |
| **`IsWildPal`** | Boolean | No | `true` or `false` (Forces match only on wild status). |
| **`ReqSwap`** | Array | No | Array of swap IDs the Pal **must** be in before it can transition to them (useful for evolutions). |
| **`ReqTrait`** | Array | No | Array of passive skill IDs the Pal **must** possess. |
| **`PrefTrait`** | Array | No | Array of passive skill IDs the Pal **should** possess (increases match score). |
| **`SkipTrait`** | Array | No | Array of passive skill IDs that **banned** this swap from occurring. |
| **`MatReplaceList`** | Array | No | Array of material override objects. Contains `"Index"` (String integer or wildcard) and `"MatPath"` (String path). |
| **`MorphTargetList`** | Array | No | Array of morph slider overrides. See Morph Target sub-schema below. |
| **`Extra`** | String/Obj | No | Extended stringified JSON or JSON object for custom metadata. |

### Morph Target Sub-Schema (`MorphTargetList`)
* **`Target`** *(String, Required)*: Name of the blendshape target in the model.
* **`Set`** *(Float, Optional)*: Explicit value to lock the blendshape to.
* **`Min`** / **`Max`** *(Float, Optional)*: Boundaries to roll a random value between if `Set` is omitted.
* **`Type`** *(String, Optional)*: `"Free"` (any value in range) or `"Restrict"` (only Min or Max explicitly).

---

## Fully Complete Example: [`example_v1.json`](./_Template_SwapConfig.json)
```json
{
    "PackName": "V1 Replacer Pack",
    "SkelMeshSwap": [
        {
            "CharacterID": "PinkCat",
            "SkelMeshPath": "/Game/Pal/Model/Character/Monster/PinkCat/SK_PinkCat",
            "AnimTarget": "PinkCat",
            "SetNickname": "CoolCat",
            "Gender": "Female",
            "SkinName": "PinkCat_Skin001",
            "SkinLabel": "Classic Pink Cat Custom",
            "MinLevel": 1,
            "MaxLevel": 50,
            "MinTrust": 0,
            "MaxTrust": 1000,
            "MinRank": 0,
            "MaxRank": 4,
            "SpawnWeight": 10,
            "IsRarePal": false,
            "IsWildPal": true,
            "ReqSwap":  ["Uncool Cat"],
            "ReqTrait": [ "Brave" ],
            "PrefTrait": [ "Swift" ],
            "SkipTrait": [ "Coward" ],
            "MatReplaceList": [
                {
                    "RandomHue": true,
                    "MatPath": "/Game/Pal/Model/Character/Monster/PinkCat/MI_PinkCat_Body",
                    "Index": "0"
                }
            ],
            "MorphTargetList": [
                {
                    "Target": "Breast_Size",
                    "Min": 0.0,
                    "Max": 1.0,
                    "Mode": "Free",
                    "Set": 0.5
                }
            ],
            "Extra": "{}"
        }
    ]
}
