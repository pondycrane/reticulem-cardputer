#include "KeyboardManager.h"

// Additional key constants (Cardputer lacks dedicated arrow keys)
#define KEY_UP     0x52
#define KEY_DOWN   0x51
#define KEY_LEFT   0x50
#define KEY_RIGHT   0x4F
#define KEY_DEL    0x4C
#define KEY_ESC    0x1B

KeyboardManager::KeyboardManager()
    : _keyPressed(false),
      _lastChar(0),
      _fnDown(false),
      _ctrlDown(false),
      _cursorPos(0)
{
}

void KeyboardManager::update() {
    _keyPressed = false;
    _lastChar = 0;
    _fnDown = false;
    _ctrlDown = false;

    if (!M5Cardputer.Keyboard.isChange()) return;
    if (!M5Cardputer.Keyboard.isPressed()) return;

    Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();
    _fnDown = status.fn;
    _ctrlDown = status.ctrl;

    // Special flags already decoded by library
    if (status.del) { _keyPressed = true; _lastChar = KEY_BACKSPACE; }
    if (status.enter) { _keyPressed = true; _lastChar = KEY_ENTER; }
    if (status.tab) { _keyPressed = true; _lastChar = KEY_TAB; }
    if (status.space) { _keyPressed = true; _lastChar = ' '; }

    // Map raw physical positions to semantic keys (Cardputer ADV layout)
    for (auto keyPos : M5Cardputer.Keyboard.keyList()) {
        // Skip modifier positions
        if ((keyPos.x == 0 && keyPos.y == 2) ||  // FN
            (keyPos.x == 1 && keyPos.y == 2) ||  // SHIFT
            (keyPos.x == 0 && keyPos.y == 3) ||  // CTRL
            (keyPos.x == 1 && keyPos.y == 3) ||  // OPT
            (keyPos.x == 2 && keyPos.y == 3)) {   // ALT
            continue;
        }

        // Dedicated arrow keys (physical positions on ADV keyboard)
        if (keyPos.x == 11 && keyPos.y == 2) { _lastChar = KEY_UP; _keyPressed = true; }
        else if (keyPos.x == 11 && keyPos.y == 3) { _lastChar = KEY_DOWN; _keyPressed = true; }
        else if (keyPos.x == 10 && keyPos.y == 3) { _lastChar = KEY_LEFT; _keyPressed = true; }
        else if (keyPos.x == 12 && keyPos.y == 3) { _lastChar = KEY_RIGHT; _keyPressed = true; }
        // ESC key (top-left physical key, labeled ` or ~)
        else if (keyPos.x == 0 && keyPos.y == 0) { _lastChar = KEY_ESC; _keyPressed = true; }
        // Enter and Backspace are already covered by status flags, but map anyway
        else if (keyPos.x == 13 && keyPos.y == 2) { _lastChar = KEY_ENTER; _keyPressed = true; }
        else if (keyPos.x == 13 && keyPos.y == 0) { _lastChar = KEY_BACKSPACE; _keyPressed = true; }
        else {
            // Regular printable character
            auto kv = M5Cardputer.Keyboard.getKeyValue(keyPos);
            if (status.shift || status.ctrl || M5Cardputer.Keyboard.capslocked()) {
                _lastChar = kv.value_second;
            } else {
                _lastChar = kv.value_first;
            }
            _keyPressed = true;
        }
        break; // only act on first non-modifier key per frame
    }

    if (_keyPressed && _lastChar >= 32 && _lastChar < 127) {
        _cursorPos++;
    } else if ((_lastChar == KEY_BACKSPACE || _lastChar == KEY_DEL) && _cursorPos > 0) {
        _cursorPos--;
    }
}