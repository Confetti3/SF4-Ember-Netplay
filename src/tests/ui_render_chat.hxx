#pragma once
// The chat shots of ui_render_test.cxx, which includes this after its Require.
// `draw` is the test's own. Each scenario is a room of its own (a new epoch, so the
// shell keeps a fresh transcript for it); the room the caller set up is put back
// at the end. The sweep runs this at every viewport and locale, so it draws as few
// frames as it can: messages are pushed together, a frame at a time between the
// changes of the room that must be seen one by one.
#include "../ui/ApplicationShell.hxx"
#include <imgui.h>
#include <algorithm>
#include <string>
#include <utility>
namespace {
// The Chat screen with a conversation (the player's own messages, others', long
// ones, and the lines for a member joining and leaving, a new host, a game and a
// set won), an empty one, a long draft and one at the limit, and the unread
// count on the room board.
template<class Draw>
void ShootChat(sf4e::ui::ApplicationShell& shell,sf4e::ui::ShellView& view,const Draw& draw){
    using namespace sf4e;using namespace sf4e::ui;
    auto& io=ImGui::GetIO();
    auto& nav=shell.Navigation();
    const auto saved=view.room;
    const auto epoch=saved.roomEpoch;
    std::uint64_t sequence=0;
    const auto say=[&](room::MemberId who,std::string text){view.room.chat.push_back({++sequence,who,std::move(text)});};
    const auto start=[&](std::uint64_t offset){
        view.room=saved;view.room.roomEpoch=epoch+offset;view.room.chat.clear();sequence=0;
        for(auto& table:view.room.tables){table.score[0]=table.score[1]=0;table.lastSet=room::SetRecord{};}
    };
    const auto open=[&](const char* screen){nav.Home();nav.Push(screen);};

    // A conversation, with what happened in the room between the messages.
    start(1);
    const auto last=view.room.members.back();
    view.room.members.pop_back();
    open("room");draw(nullptr,0,2);
    view.room.members.push_back(last);
    say(16,"Hi everyone!");
    say(3,"Anyone up for a set?");
    say(1,"Yes! Table 1 is open, I'm taking P1.");
    say(2,"I will take P2 once I finish this match. It is a long message, to show how the text wraps in the transcript.");
    say(1,"Sounds good.");say(1,"Warming up now.");
    draw(nullptr,0,1);
    view.room.tables[0].score[1]=1;say(5,"GG!");draw(nullptr,0,1);
    // The room drops a member's messages when they leave; the transcript keeps them.
    view.room.tables[0].lastSet.generation=7;view.room.tables[0].lastSet.p1=1;view.room.tables[0].lastSet.p2=2;
    view.room.tables[0].lastSet.score[0]=3;view.room.tables[0].lastSet.score[1]=1;view.room.tables[0].score[1]=0;
    view.room.members.erase(std::find_if(view.room.members.begin(),view.room.members.end(),[](const room::Member& m){return m.id==5;}));
    view.room.chat.erase(std::remove_if(view.room.chat.begin(),view.room.chat.end(),[](const room::ChatMessage& m){return m.sender==5;}),view.room.chat.end());
    draw(nullptr,0,1);
    view.room.host=2;say(3,"Nice set!");draw(nullptr,0,1);
    open("room-chat");draw("chat-conversation",0,3);
    Require(shell.Transcript().Lines().size()>=12,"The chat transcript did not keep the room's events and messages");
    Require(FindWindow("Chat transcript")!=nullptr,"The Chat screen has no transcript");

    // Nothing said yet.
    start(2);
    open("room-chat");draw("chat-empty",0,3);

    // A draft too long for the box to show whole.
    start(3);view.room.chat=saved.chat;
    open("room-chat");draw(nullptr,0,3);
    Require(io.WantTextInput,"The chat box did not take the keyboard");
    std::string words;
    while(words.size()<160)words+="the quick brown fox ";
    io.AddInputCharactersUTF8(words.c_str());draw("chat-draft-long",0,3);

    // Messages that arrive while the player is on the board count on the Chat row and its heading.
    start(4);view.room.chat=saved.chat;
    sequence=view.room.chat.empty()?0:view.room.chat.back().sequence;
    open("room");draw(nullptr,0,2);
    for(int i=0;i<11;++i)say(2+i%5,"New message "+std::to_string(i+1)+", read it in Chat.");
    say(1,"My own message never counts.");
    draw("chat-unread-board",0,3);
    Require(shell.Transcript().Unread({})==11,"The board did not count the messages from others");
    // The Chat row focused, where its explanation says how many are unread.
    nav.Prefer("room-chat");
    draw("chat-unread-row",0,3);
    Require(nav.Focus()=="room-chat","The Chat row did not take the focus");
    open("room-chat");draw(nullptr,0,1);
    Require(shell.Transcript().Unread({})==0,"Opening Chat did not clear the count");

    // The board as the caller left it, with the first battle slot focused again.
    view.room=saved;open("room");nav.Prefer("table-0");
    draw(nullptr,0,3);
    Require(nav.Focus()=="table-0","The first battle slot did not take the focus");
}
}
