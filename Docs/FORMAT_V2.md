# Version 2 JSON Format Specification

Version 2 is a grouped, map-based format created by MonoDragon. Instead of arrays, it maps `CharacterID` and a unique `Skin Label` as nested dictionary keys. This is the preferred format because it provides unique UI labels natively.

Place V2 files in `Palworld/Pal/Content/Paks/~mods/ModelJSON/` (subfolders are scanned too). Files whose name starts with `_` or contains `Template` are ignored.

> **Tip:** Both formats share the same property parser. Property names are **case-insensitive**, and the V1 names (`IsRarePal`, `ReqTrait`, `MatReplace`, `MorphTarget`, `MatPath`) are accepted in V2 files as well. Inside `ShapeKeys`, the V1 `Target` / `Type` names also work.

## Root Schema
* `ModelPack` *(String, Optional)*: Descriptive label for UI display. Alias: `PackName`. Defaults to the JSON file name when omitted.
* `SkinList` *(Object, Required)*: Map of `CharacterID` → map of `Skin Label` → skin properties.
  * The `CharacterID` key is the Pal's internal ID (see [PAL_IDS.json](./PAL_IDS.json)). Boss/alpha prefixes (`BOSS_`, `MiddleBoss_`, `RAID_`, `GYM_`, `PREDATOR_`) and the `_Otomo` suffix are stripped from the Pal before matching, so `"PinkCat"` also covers alpha PinkCats.
  * The `Skin Label` key is the display name shown in the in-game menu. It can be overridden with a `SwapLabel` / `SkinLabel` property inside the entry.

---

## Property Reference

| Property | Type | Required | Description / Options |
| :--- | :--- | :---: | :--- |
| **`SkinPath`** | String | **Yes\*** | Game-internal path to the skeletal mesh asset (V1 `SkelMeshPath`). \*May be omitted for material/morph-only swaps. |
| **`AnimTarget`** | String | No | Pal ID (e.g., `"Anubis"`) whose blueprint animations should be used, or a full animation-blueprint class path. |
| **`SetNickname`** | String | No | Changes the nickname on the pal upon reaching this swap. |
| **`IconAssetPath`** | String | No | Texture asset used as this Pal's icon in-game (party list, Palbox, etc.). |
| **`Gender`** | String | No | `"Male"`, `"Female"`, `"None"` (also `"Any"`, `"All"`, `"Both"`), `"Futa"`, `"FullFuta"`, `"Andro"`, `"Neutered"`, `"FullNeutered"` |
| **`SkinName`** | String | No | Game-native Skin ID required on the Pal. When set, only Pals wearing that official skin match. When omitted, Pals wearing *any* official skin are skipped. |
| **`MinLevel`** / **`MaxLevel`** | Integer | No | Minimum (default 1) and Maximum (default 999) level requirements. |
| **`MinTrust`** / **`MaxTrust`** | Integer | No | Minimum (default 0) and Maximum (default 999999) friendship/trust requirements. |
| **`MinRank`** / **`MaxRank`** | Integer | No | Minimum (default 0) and Maximum (default 999) condenser star-rank requirements. |
| **`MinSizeMultiplier`** | Float | No | Minimum scale the Pal can be multiplied by (default 1). |
| **`MaxSizeMultiplier`** | Float | No | Maximum scale the Pal can be multiplied by (default 1). |
| **`SpawnWeight`** | Float | No | Relative probability weight for weighted random selection on ties (default 1). `0` excludes the swap from automatic selection, but it can still be picked manually in the menu. |
| **`LuckyStarReq`** | String/Bool | No | `true` = only Lucky/Rare Pals, `false` = only non-Lucky Pals. Omit (or `null`) to allow both. Alias: `IsRarePal`. |
| **`IsWildPal`** | String/Bool | No | `true` = only wild Pals, `false` = only tamed Pals. Omit (or `null`) to allow both. |
| **`PassiveSkills`** | Array | No | Array of passive skill IDs the Pal **must** possess. See [TRAIT_IDS.json](./TRAIT_IDS.json). Alias: `ReqTrait`. |
| **`PrefTrait`** | Array | No | Array of passive skill IDs the Pal **should** possess (improves match score). |
| **`ReqSwap`** | Array | No | Array of skin **labels** the Pal **must** currently be wearing before it can transition to this swap (useful for evolutions). |
| **`SkipTrait`** | Array | No | Array of passive skill IDs that **ban** this swap from occurring. |
| **`SpecialMaterial`** | Array | No | Array of material overrides. See Special Material sub-schema below. Alias: `MatReplace`. |
| **`ShapeKeys`** | Array | No | Array of morph (blendshape) overrides. See Shape Keys sub-schema below. Alias: `MorphTarget`. |
| **`Extra`** | String/Obj | No | Free-form JSON object or stringified JSON for your own metadata. Not interpreted by DynamicPals. |

### Special Material Sub-Schema (`SpecialMaterial`)
* **`MaterialAsset`** *(String, Required)*: Path to the Material Instance asset (V1 `MatPath`). End it with `/*` (e.g., `"/Game/Mods/MyPack/Materials/*"`) to pick a random material from that folder per Pal; the choice is remembered.
* **`Index`** *(String or Integer, Required)*: Material slot to overwrite (e.g., `"0"`). Use a comma-separated list (`"0, 2, 3"`) to apply the same material to several slots.
* **`RandomHue`** *(Boolean, Optional)*: Applies a random, persisted color to the material's `"Hue"` vector parameter (ignored by materials without one).

### Shape Keys Sub-Schema (`ShapeKeys`)
* **`Name`** *(String, Required)*: Name of the blendshape target in the model (V1 `Target`).
* **`Set`** *(Float, Optional)*: Explicit value to lock the blendshape to. Overrides `Min`/`Max` when present.
* **`Min`** / **`Max`** *(Float, Optional)*: Boundaries to roll a random value between if `Set` is omitted (defaults 0 and 1).
* **`Mode`** *(String, Optional)*: `"Free"` (any value in range, default) or `"Restrictive"` (rolls exactly `Min` or `Max`) (V1 `Type`; `"Restrict"` is also accepted).

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
                "IconAssetPath": "/Game/Mods/MyPack/Icons/T_PinkCat_Glasses_icon_normal",
                "Gender": "Female",
                "MinLevel": 1,
                "MaxLevel": 50,
                "MinTrust": 0,
                "MaxTrust": 1000,
                "MinRank": 0,
                "MaxRank": 4,
                "MinSizeMultiplier": 0.5,
                "MaxSizeMultiplier": 2,
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
                        "Name": "Breast_Size",
                        "Min": 0.0,
                        "Max": 1.0,
                        "Mode": "Free"
                    }
                ],
                "Extra": "{}"
            }
        }
    }
}
```
