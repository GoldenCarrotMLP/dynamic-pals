# Correct the CurrentPlayerController typo in UIRegistry.cpp
with open('./src/UI/UIRegistry.cpp', 'r', encoding='utf-8') as f:
    content = f.read()

# Replace the incorrect variable
incorrect_line = 'Utils::CallFunction(CurrentPlayerController, STR("ResetIgnoreMoveInput"));'
correct_line = 'Utils::CallFunction(PlayerController, STR("ResetIgnoreMoveInput"));'

if incorrect_line in content:
    content = content.replace(incorrect_line, correct_line)
    with open('./src/UI/UIRegistry.cpp', 'w', encoding='utf-8', newline='\r\n') as f:
        f.write(content)
    print("Typo corrected in UIRegistry.cpp! Replaced CurrentPlayerController with PlayerController.")
else:
    print("Typos line not found. Let's do a broader check.")
    # Checking for any "CurrentPlayerController" inside UIRegistry.cpp
    if "CurrentPlayerController" in content:
        content = content.replace("CurrentPlayerController", "PlayerController")
        with open('./src/UI/UIRegistry.cpp', 'w', encoding='utf-8', newline='\r\n') as f:
            f.write(content)
        print("Bypassed via broader string replace!")
    else:
        print("No CurrentPlayerController exists inside UIRegistry.cpp. Already clean!")