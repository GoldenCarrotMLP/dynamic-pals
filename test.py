import os

# Search directly for ue4ss.log in H:\SteamLibrary\steamapps\common\Palworld\Pal\Binaries\Win64\
win64_path = "H:\\SteamLibrary\\steamapps\\common\\Palworld\\Pal\\Binaries\\Win64"
log_file = os.path.join(win64_path, "ue4ss.log")

if os.path.exists(log_file):
    print(f"Found ue4ss.log at: {log_file} (size: {os.path.getsize(log_file)} bytes)")
    with open(log_file, 'r', encoding='utf-8', errors='ignore') as f:
        content = f.read().splitlines()
        print("--- Last 40 lines of ue4ss.log ---")
        for line in content[-40:]:
            print(line)
else:
    # Try ue4ss folder
    log_file_alt = os.path.join(win64_path, "ue4ss", "ue4ss.log")
    if os.path.exists(log_file_alt):
        print(f"Found ue4ss.log at: {log_file_alt} (size: {os.path.getsize(log_file_alt)} bytes)")
        with open(log_file_alt, 'r', encoding='utf-8', errors='ignore') as f:
            content = f.read().splitlines()
            print("--- Last 40 lines of ue4ss.log ---")
            for line in content[-40:]:
                print(line)
    else:
        print("ue4ss.log not found in Win64 or Win64/ue4ss!")