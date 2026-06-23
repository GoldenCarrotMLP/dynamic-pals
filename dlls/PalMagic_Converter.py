import json
import os
from datetime import datetime

def convert_files():
    files = [f for f in os.listdir('.') if f.endswith('.json') and "Converted_" not in f and "Example" not in f]
    
    if not files:
        print(f"[{datetime.now().strftime('%H:%M:%S')}] No JSON files found to process.")
        input("Press Enter to close the window...")
        return

    processed_files = 0
    total_entries_converted = 0
    errors = []

    print(f"[{datetime.now().strftime('%H:%M:%S')}] Starting conversion process...")

    for file_name in files:
        print(f"[{datetime.now().strftime('%H:%M:%S')}] Processing: {file_name}")
        try:
            with open(file_name, 'r', encoding='utf-8') as f:
                data = json.load(f)

            if "SkelMeshSwap" not in data:
                raise ValueError(f"Incompatible structure: 'SkelMeshSwap' key missing.")

            pack_name = data.get("PackName", "PackName")
            lines = ["{", f'    "ModelPack": "{pack_name}",', '    "SkinList": {']
            
            groups = {}
            for entry in data.get("SkelMeshSwap", []):
                char_id = entry.get("CharacterID")
                if char_id not in groups: groups[char_id] = []
                groups[char_id].append(entry)

            used_names = set()
            file_entries = 0
            
            for char_id in groups.keys():
                entries = groups[char_id]
                lines.append(f'\t\t"{char_id}": {{')
                
                for entry in entries:
                    file_entries += 1
                    path = entry.get("SkelMeshPath", "")
                    raw_name = path.split('/')[-1] if path else "Unknown"
                    clean_name = raw_name.replace("SK_", "", 1)
                    
                    # Verbesserte Umbenennungs-Logik
                    base_key = f"{pack_name}_{clean_name}"
                    final_key = base_key
                    counter = 1
                    
                    while final_key in used_names:
                        suffix = f"_Alt{counter:02d}"
                        if final_key.endswith(('_M', '_F')):
                            final_key = final_key[:-2] + suffix + final_key[-2:]
                        else:
                            final_key = base_key + suffix
                        counter += 1
                    
                    used_names.add(final_key)
                    
                    gender = entry.get("Gender", "Any")
                    is_rare = str(entry.get("IsRarePal", "")).lower() == "true"
                    req_trait = entry.get("ReqTrait")
                    has_trait = isinstance(req_trait, list) and len(req_trait) > 0 and req_trait[0] != ""
                    mat_replace = entry.get("MatReplace", [])
                    has_mats = isinstance(mat_replace, list) and len(mat_replace) > 0
                    morph_targets = entry.get("MorphTarget", [])
                    has_morphs = isinstance(morph_targets, list) and len(morph_targets) > 0
                    
                    lines.append(f'\t\t\t"{final_key}":{{')
                    lines.append(f'\t\t\t\t"SkinPath": "{path}",')
                    lines.append(f'\t\t\t\t"Gender": "{gender}"')
                    
                    if is_rare:
                        lines[-1] += ","
                        lines.append(f'\t\t\t\t"LuckyStarReq": "true"')
                    if has_trait:
                        lines[-1] += ","
                        lines.append(f'\t\t\t\t"PassiveSkills": ["{req_trait[0]}"]')
                    if has_mats:
                        lines[-1] += ","
                        lines.append(f'\t\t\t\t"SpecialMaterial": [')
                        mat_lines = []
                        for mat in mat_replace:
                            mat_lines.append(f'\t\t\t\t\t{{"Index": "{mat.get("Index")}","MaterialAsset": "{mat.get("MatPath")}"}}')
                        lines.append(",\n".join(mat_lines))
                        lines.append(f'\t\t\t\t]')
                    if has_morphs:
                        lines[-1] += ","
                        lines.append(f'\t\t\t\t"ShapeKeys": [')
                        for i, morph in enumerate(morph_targets):
                            lines.append(f'\t\t\t\t\t{{')
                            lines.append(f'\t\t\t\t\t"Name":"{morph.get("Target", "Unknown")}",')
                            lines.append(f'\t\t\t\t\t"Min":{morph.get("Min", 0)},')
                            lines.append(f'\t\t\t\t\t"Max":{morph.get("Max", 1)},')
                            lines.append(f'\t\t\t\t\t"Mode":"{"Restrictive" if morph.get("Type") == "Restrict" else morph.get("Type")}"')
                            lines.append(f'\t\t\t\t\t{"" if i == len(morph_targets)-1 else "},"}')
                        lines.append(f'\t\t\t\t\t}}')
                        lines.append(f'\t\t\t\t]')
                    
                    lines.append('\t\t\t},')
                
                lines[-1] = lines[-1].rstrip(',') 
                lines.append('\t\t},')
            
            lines[-1] = lines[-1].rstrip(',')
            lines.append('    }')
            lines.append('}')

            with open(f"Converted_{file_name}", 'w', encoding='utf-8') as f:
                f.write("\n".join(lines))
            
            total_entries_converted += file_entries
            processed_files += 1
            print(f"  -> Successfully converted {file_entries} entries.")

        except Exception as e:
            errors.append(f"Error in {file_name}: {str(e)}")
            print(f"  -> [!] {errors[-1]}")

    input("\nConversion finished. Press Enter to close...")

if __name__ == "__main__":
    convert_files()