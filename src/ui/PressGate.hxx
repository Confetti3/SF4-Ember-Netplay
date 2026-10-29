#pragma once

namespace sf4e { namespace ui {
// The presses of a set of buttons, for a reader that opens and closes: a
// screen, a notice, a toast, the pad adapter. A button down when the reader
// opens was pressed for something else (the press that opened it, a fight's
// held direction) and counts again only once released; the others count at
// once. The opening frame itself counts nothing.
class PressGate {
public:
    explicit PressGate(bool armed=false) : armed_(armed) {}
    void Rearm() { armed_=false; }
    bool Armed() const { return armed_; }
    // The buttons that count, out of those held this frame.
    unsigned Held(unsigned held) {
        if(!armed_) { stale_=held; previous_=0; armed_=true; }
        stale_&=held; return held&~stale_;
    }
    // Of the buttons that count, those down this frame and not the last.
    unsigned Pressed(unsigned counted) { const unsigned pressed=counted&~previous_; previous_=counted; return pressed; }
private:
    bool armed_;
    unsigned stale_=0, previous_=0;
};
} }
