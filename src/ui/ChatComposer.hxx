#pragma once
#include <cstdint>
#include <string>
namespace sf4e { namespace ui {
class GameMenu;
// One editor shared by the room board and Chat history. The shell owns the
// draft and delivery; GameMenu absorbs navigation input while this owns it.
class ChatComposer {
public:
    enum class Result { None, Sent, Released };
    Result Draw(GameMenu& menu,char (&draft)[257],std::uint64_t roomEpoch,float width,const char* focusId,bool inlineBox,bool typable,bool pending,bool activate=false);
    void ObserveScreen(GameMenu& menu);
    void Release(GameMenu& menu);
    bool Typing() const { return typing_; }
private:
    bool typing_=false,focus_=false;
    unsigned id_=0;
    std::string boxText_,screen_;
};
} }
