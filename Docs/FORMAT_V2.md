# Version 2 JSON Format Specification

Version 2 is a grouped, map-based format created by MonoDrago. Instead of arrays, it maps `CharacterID` and a unique `Skin Label` as nested dictionary keys. This is the preferred format because it provides unique UI labels natively.

## Root Schema
* `ModelPack` *(String, Optional)*: Descriptive label for UI display.
* `SkinList` *(Object, Required)*: Map of `CharacterID` to `Skin Label` maps.

---

## Property Reference

| Property | Type | Required | Description / Options |
| :--- | :--- | :---: | :--- |
| **`SkinPath`** | String | **Yes** | Game-internal path to the skeletal mesh asset (Maps to V1 `SkelMeshPath`). |
| **`AnimTarget`** | String | No | Target Pal ID or asset path to copy animations from. |
| **`SetNickname`** | String | No | Changes the nickname on the pal upon reaching this swap. |
| **`Gender`** | String | No | `"Male"`, `"Female"`, `"None"`, `"Futa"`, `"FullFuta"`, `"Andro"`, `"Neutered"`, `"FullNeutered"` |
| **`MinLevel`** / **`MaxLevel`** | Integer | No | Minimum and Maximum level requirements. |
| **`MinTrust`** / **`MaxTrust`** | Integer | No | Minimum and Maximum friendship/trust requirements. |
| **`MinRank`** / **`MaxRank`** | Integer | No | Minimum and Maximum star-rank requirements. |
| **`SpawnWeight`** | Integer | No | Relative probability weight for weighted random selection on ties (default 1). |
| **`LuckyStarReq`** | String/Bool | No | `"true"` or `"false"` (replaces V1 `IsRarePal`). |
| **`IsWildPal`** | String/Bool | No | `"true"` or `"false"`. |
| **`PassiveSkills`** | Array | No | Array of passive skill IDs the Pal **must** possess (replaces V1 `ReqTrait`). |
| **`PrefTrait`** | Array | No | Array of passive skill IDs the Pal **should** possess. |
| **`ReqSwap`** | Array | No | Array of swap IDs the Pal **must** be in before it can transition to them (useful for evolutions). |
| **`SkipTrait`** | Array | No | Array of passive skill IDs that **banned** this swap from occurring. |
| **`SpecialMaterial`** | Array | No | Array of material overrides. See Special Material sub-schema below. |
| **`ShapeKeys`** | Array | No | Array of morph slider overrides. See Shape Keys sub-schema below. |
| **`Extra`** | String/Obj | No | Extended stringified JSON or JSON object for custom metadata. |

### Special Material Sub-Schema (`SpecialMaterial`)
* **`MaterialAsset`** *(String, Required)*: Path to the Material Instance asset (replaces V1 `MatPath`).
* **`Index`** *(String, Required)*: Material slot to overwrite (e.g., `"0"`).

### Shape Keys Sub-Schema (`ShapeKeys`)
* **`Name`** *(String, Required)*: Name of the blendshape target in the model (replaces V1 `Target`).
* **`Set`** *(Float, Optional)*: Explicit value to lock the blendshape to.
* **`Min`** / **`Max`** *(Float, Optional)*: Boundaries to roll a random value between if `Set` is omitted.
* **`Mode`** *(String, Optional)*: `"Free"` (any value in range) or `"Restrictive"` (only Min or Max explicitly) (replaces V1 `Type`).

---

## Fully Complete Example: [`example_v2.json`](./_Template_SwapConfig_V2.json)
```json
{
    "ModelPack": "V2 Replacer Pack",
    "SkinList": {
        "PinkCat": {
            "Glasses Cat Alternate": {
                "SkinPath": "/Game/Pal/Model/Character/Monster/PinkCat/SK_PinkCat",
                "AnimTarget": "PinkCat",
                "SetNickname": "CoolCat",
                "Gender": "Female",
                "MinLevel": 1,
                "MaxLevel": 50,
                "MinTrust": 0,
                "MaxTrust": 1000,
                "MinRank": 0,
                "MaxRank": 4,
                "SpawnWeight": 10,
                "LuckyStarReq": "false",
                "IsWildPal": "true",
                "PassiveSkills": [ "Brave" ],
                "PrefTrait": [ "Swift" ],
                "ReqSwap": [ "Uncool cat" ],
                "SkipTrait": [ "Coward" ],
                "SpecialMaterial": [
                    {
                        "RandomHue": true,
                        "MaterialAsset": "/Game/Pal/Model/Character/Monster/PinkCat/MI_PinkCat_Body",
                        "Index": "0"
                    }
                ],
                "ShapeKeys": [
                    {
                        "Min": 0.0,
                        "Name": "Breast_Size",
                        "Max": 1.0,
                        "Mode": "Free",
                        "Set": 0.5
                    }
                ],
                "Extra": "{}"
            }
        }
    }
}