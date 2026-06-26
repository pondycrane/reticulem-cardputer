#ifndef RETICULEM_KEYBOARDMANAGER_H
#define RETICULEM_KEYBOARDMANAGER_H

#include <M5Cardputer.h>

class KeyboardManager {
public:
    KeyboardManager();
    void update(); // Call each frame to read keyboard state

    bool keyPressed() const { return _keyPressed; }
    char lastChar() const { return _lastChar; }
    bool fnDown() const { return _fnDown; }
    bool ctrlDown() const { return _ctrlDown; }
    int cursorPos() const { return _cursorPos; }
    void clearKeyPress() { _keyPressed = false; }

private:
    bool _keyPressed;
    char _lastChar;
    bool _fnDown;
    bool _ctrlDown;
    int _cursorPos;
};

#endif // RETICULEM_KEYBOARDMANAGER_H