# Version 1 JSON Format Specification

Version 1 is an array-based format originally created by Michael, but now heavily modified to fit dynamic-pals amplitude of features, where every replacement entry is explicitly defined as an individual object inside the `"SkelMeshSwap"` array.

Place V1 files in `Palworld/Pal/Content/Paks/~mods/SwapJSON/` (subfolders are scanned too). Files whose name starts with `_` or contains `Template` are ignored.

> **Tip:** Both formats share the same property parser. Property names are **case-insensitive**, and the V2 names (`LuckyStarReq`, `PassiveSkills`, `SpecialMaterial`, `ShapeKeys`, `MaterialAsset`) are accepted in V1 files as well.

## Root Schema
* `PackName` *(String, Optional)*: Descriptive label for UI display. Defaults to the JSON file name when omitted.
* `SkelMeshSwap` *(Array, Required)*: A list of individual replacement rules.

---

## Property Reference

| Property | Type | Required | Description / Options |
| :--- | :--- | :---: | :--- |
| **`CharacterID`** | String | **Yes** | Internal ID of the target Pal (e.g., `"PinkCat"`). See [PAL_IDS.json](./PAL_IDS.json). Boss/alpha prefixes (`BOSS_`, `MiddleBoss_`, `RAID_`, `GYM_`, `PREDATOR_`) and the `_Otomo` suffix are stripped from the Pal before matching, so `"PinkCat"` also covers alpha PinkCats. |
| **`SkelMeshPath`** | String | **Yes\*** | Game-internal path to the skeletal mesh asset. \*May be omitted for material/morph-only swaps, as long as a `SwapLabel` is given. |
| **`SwapLabel`** | String | No | Display name for this swap in the in-game menu. Aliases: `SkinLabel`, `SwapName`. Auto-generated from the mesh/material names when omitted. Duplicate labels for the same Pal within one pack are auto-numbered (`"Label (2)"`). |
| **`AnimTarget`** | String | No | Pal ID (e.g., `"Anubis"`) whose blueprint animations should be used, or a full animation-blueprint class path. |
| **`SetNickname`** | String | No | Changes the nickname on the pal upon reaching this swap. |
| **`Gender`** | String | No | `"Male"`, `"Female"`, `"None"` (also `"Any"`, `"All"`, `"Both"`), `"Futa"`, `"FullFuta"`, `"Andro"`, `"Neutered"`, `"FullNeutered"` |
| **`SkinName`** | String | No | Game-native Skin ID required on the Pal. When set, only Pals wearing that official skin match. When omitted, Pals wearing *any* official skin are skipped. |
| **`IconAssetPath`** | String | No | Texture asset used as this Pal's icon in-game (party list, Palbox, etc.). |
| **`MinLevel`** / **`MaxLevel`** | Integer | No | Minimum (default 1) and Maximum (default 999) level requirements. |
| **`MinTrust`** / **`MaxTrust`** | Integer | No | Minimum (default 0) and Maximum (default 999999) friendship/trust requirements. |
| **`MinRank`** / **`MaxRank`** | Integer | No | Minimum (default 0) and Maximum (default 999) condenser star-rank requirements. |
| **`MinSizeMultiplier`** | Float | No | Minimum scale the Pal can be multiplied by (default 1). |
| **`MaxSizeMultiplier`** | Float | No | Maximum scale the Pal can be multiplied by (default 1). |
| **`SpawnWeight`** | Float | No | Relative probability weight for weighted random selection on ties (default 1). `0` excludes the swap from automatic selection, but it can still be picked manually in the menu. |
| **`IsRarePal`** | Boolean | No | `true` = only Lucky/Rare Pals, `false` = only non-Lucky Pals. Omit (or `null`) to allow both. Alias: `LuckyStarReq`. |
| **`IsWildPal`** | Boolean | No | `true` = only wild Pals, `false` = only tamed Pals. Omit (or `null`) to allow both. |
| **`ReqSwap`** | Array | No | Array of swap **labels** (`SwapLabel`) the Pal **must** currently be wearing before it can transition to this swap (useful for evolutions). |
| **`ReqTrait`** | Array | No | Array of passive skill IDs the Pal **must** possess. See [TRAIT_IDS.json](./TRAIT_IDS.json). Alias: `PassiveSkills`. |
| **`PrefTrait`** | Array | No | Array of passive skill IDs the Pal **should** possess (improves match score). |
| **`SkipTrait`** | Array | No | Array of passive skill IDs that **ban** this swap from occurring. |
| **`MatReplace`** | Array | No | Array of material override objects. See Material sub-schema below. Alias: `SpecialMaterial`. |
| **`MorphTarget`** | Array | No | Array of morph (blendshape) overrides. See Morph Target sub-schema below. Alias: `ShapeKeys`. |
| **`Extra`** | String/Obj | No | Free-form JSON object or stringified JSON for your own metadata. Not interpreted by DynamicPals. |

### Material Sub-Schema (`MatReplace`)
* **`Index`** *(String or Integer, Required)*: Material slot to overwrite (e.g., `"0"`). Use a comma-separated list (`"0, 2, 3"`) to apply the same material to several slots.
* **`MatPath`** *(String, Required)*: Path to the Material Instance asset. Alias: `MaterialAsset`. End it with `/*` (e.g., `"/Game/Mods/MyPack/Materials/*"`) to pick a random material from that folder per Pal; the choice is remembered.
* **`RandomHue`** *(Boolean, Optional)*: Applies a random, persisted color to the material's `"Hue"` vector parameter (ignored by materials without one).

### Morph Target Sub-Schema (`MorphTarget`)
* **`Target`** *(String, Required)*: Name of the blendshape target in the model.
* **`Set`** *(Float, Optional)*: Explicit value to lock the blendshape to. Overrides `Min`/`Max` when present.
* **`Min`** / **`Max`** *(Float, Optional)*: Boundaries to roll a random value between if `Set` is omitted (defaults 0 and 1).
* **`Type`** *(String, Optional)*: `"Free"` (any value in range, default) or `"Restrict"` (rolls exactly `Min` or `Max`).

*If you use the V2 key `ShapeKeys` instead, its entries follow the [V2 Shape Keys sub-schema](./FORMAT_V2.md#shape-keys-sub-schema-shapekeys) (`Name` / `Mode`).*

---

## Fully Complete Example: [`example_v1.json`](./_Template_SwapConfig.json)
```json
{
    "PackName": "V1 Replacer Pack",
    "SkelMeshSwap": [
        {
            "CharacterID": "PinkCat",
            "SkelMeshPath": "/Game/Pal/Model/Character/Monster/PinkCat/SK_PinkCat",
            "SwapLabel": "Classic Pink Cat Custom",
            "AnimTarget": "PinkCat",
            "SetNickname": "CoolCat",
            "Gender": "Female",
            "SkinName": "PinkCat_Skin001",
            "IconAssetPath": "/Game/Mods/MyPack/Icons/T_PinkCat_Custom_icon_normal",
            "MinLevel": 1,
            "MaxLevel": 50,
            "MinTrust": 0,
            "MaxTrust": 1000,
            "MinRank": 0,
            "MaxRank": 4,
            "MinSizeMultiplier": 0.5,
            "MaxSizeMultiplier": 2,
            "SpawnWeight": 10,
            "IsRarePal": false,
            "IsWildPal": true,
            "ReqSwap":  ["Uncool Cat"],
            "ReqTrait": [ "Brave" ],
            "PrefTrait": [ "Swift" ],
            "SkipTrait": [ "Coward" ],
            "MatReplace": [
                {
                    "RandomHue": true,
                    "MatPath": "/Game/Pal/Model/Character/Monster/PinkCat/MI_PinkCat_Body",
                    "Index": "0"
                }
            ],
            "MorphTarget": [
                {
                    "Target": "Breast_Size",
                    "Min": 0.0,
                    "Max": 1.0,
                    "Type": "Free"
                }
            ],
            "Extra": "{}"
        }
    ]
}
```
