#pragma once

namespace sf4e { namespace ui {
// Which device the prompts name for a player who has a pad assigned: the keyboard after a key was pressed, the pad after one of
// its buttons was. Only a press counts, a button or key that was not held the frame before, so a frame that brings no press
// (a key or button still held, nothing at all) leaves the prompts as they were.
class PromptDevice {
public:
    // One frame. keys: the menu's keyboard buttons held, the same set every frame whatever the screen types into; typed: text or
    // a function key arrived this frame; pad: the pad's menu buttons held. A pad press in the same frame wins.
    void Note(unsigned keys, bool typed, unsigned pad) {
        if ((keys & ~keys_) || typed) keyboard_ = true;
        if (pad & ~pad_) keyboard_ = false;
        keys_ = keys;
        pad_ = pad;
    }
    bool Keyboard() const { return keyboard_; }
private:
    bool keyboard_ = false;
    unsigned keys_ = 0, pad_ = 0;
};
} }
