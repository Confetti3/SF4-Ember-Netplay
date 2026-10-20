#include "ApplicationShell.hxx"
#include "MenuRows.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <string>
#include <utility>

namespace sf4e { namespace ui {
// Settings, Accessibility: speech and how it reads. Everything but Speech
// itself waits for speech to be on, like the ready sound's volume.
namespace {
// The voice pack's size as the rows say it: whole megabytes, at least one.
int Megabytes(long long bytes) { return static_cast<int>((std::max)(1LL,(bytes+500000)/1000000)); }
}
std::vector<MenuEntry> ApplicationShell::AccessibilityRows(const ShellView& v) const {
 const auto& speech=preferences_.speech;const bool speaking=v.canEditPreferences&&speech.enabled;
 const auto onOff=[](bool on){return std::string(on?loc::T("common.on"):loc::T("common.off"));};
 const auto voice=speech::NaturalVoice();
 std::vector<MenuEntry> rows={Value("speech",loc::T("speech.setting"),onOff(speech.enabled),loc::T("speech.setting_detail"),v.canEditPreferences),
  Value("speech-rate",loc::T("speech.rate"),std::to_string(speech.rate),loc::T("speech.rate_detail"),speaking),
  Value("speech-volume",loc::T("speech.volume"),std::to_string(speech.volume)+"%",loc::T("speech.volume_detail"),speaking),
  Value("speech-details",loc::T("speech.details"),onOff(speech.details),loc::T("speech.details_detail"),speaking),
  Value("speech-hints",loc::T("speech.hints"),onOff(speech.hints),loc::T("speech.hints_detail"),speaking),
  Value("speech-position",loc::T("speech.read_position"),onOff(speech.position),loc::T("speech.read_position_detail"),speaking),
  Value("speech-remind",loc::T("speech.remind"),speech.remind==netplay::SpeechPreferences::Auto?loc::T("speech.remind_auto"):onOff(speech.remind==netplay::SpeechPreferences::Always),loc::T("speech.remind_detail"),speaking),
  Value("speech-tick",loc::T("speech.tick"),onOff(speech.tick),loc::T("speech.tick_detail"),speaking),
  Value("speech-chime",loc::T("speech.chime"),onOff(speech.chime),loc::T("speech.chime_detail"),speaking),
  Value("speech-chime-volume",loc::T("speech.chime_volume"),std::to_string(speech.chimeVolume)+"%",loc::T("speech.chime_volume_detail"),speaking&&speech.chime),
  InfoRow("speech-voice",loc::T("speech.voice"),VoiceText(voice),loc::T("speech.voice_detail"))};
 // The language's voice pack, where the relay has one listed (under Proton): its size before the
 // first download, an update, and its removal. Select on the pack row downloads it.
 if(voice.pack!=speech::Pack::None){
  const int mb=Megabytes(voice.packBytes);
  const std::string state=voice.voice==speech::Voice::Downloading?VoiceText(voice):voice.pack==speech::Pack::Current?loc::Tf("speech.pack_current",mb):
   voice.pack==speech::Pack::Outdated?loc::Tf("speech.pack_outdated",mb):voice.pack==speech::Pack::Removed?std::string(loc::T("speech.pack_removed")):loc::Tf("speech.pack_missing",mb);
  rows.push_back(Row("speech-pack",loc::Tf("speech.pack",loc::NativeName(loc::Active())),loc::T("speech.pack_detail"),speaking&&voice.voice!=speech::Voice::Downloading));
  rows.back().value=state;rows.back().hint=loc::T("speech.pack_download");
  auto remove=Row("speech-pack-remove",loc::T("speech.pack_remove"),loc::T("speech.pack_remove_detail"),speaking&&(voice.pack==speech::Pack::Current||voice.pack==speech::Pack::Outdated));
  remove.confirm=true;rows.push_back(remove);
 }
 return rows;
}
bool ApplicationShell::ActivateSpeech(const MenuAction& a) {
 if(a.id=="speech-pack")speech::UpdatePack();
 else if(a.id=="speech-pack-remove")speech::RemovePack();
 else return false;
 return true;
}
bool ApplicationShell::AdjustSpeech(const MenuAction& a) {
 auto& speech=preferences_.speech;
 const auto step=[&](int& value,int least,int most,int by){value=(std::max)(least,(std::min)(most,value+by*a.delta));};
 if(a.id=="speech"){speechIntro_=a.delta>0&&!speech.enabled;speech.introHeard|=speechIntro_;speech.enabled=a.delta>0;}
 else if(a.id=="speech-remind")step(speech.remind,netplay::SpeechPreferences::Auto,netplay::SpeechPreferences::Never,1);
 else if(a.id=="speech-tick")speech.tick=a.delta>0;
 else if(a.id=="speech-rate")step(speech.rate,-10,10,1);
 else if(a.id=="speech-volume")step(speech.volume,10,100,10);
 else if(a.id=="speech-details")speech.details=a.delta>0;
 else if(a.id=="speech-hints")speech.hints=a.delta>0;
 else if(a.id=="speech-position")speech.position=a.delta>0;
 else if(a.id=="speech-chime")speech.chime=a.delta>0;
 else if(a.id=="speech-chime-volume")step(speech.chimeVolume,10,100,10);
 else return false;
 return true;
}
// Every menu speaks with the row on screen, which may not be saved yet.
// Turning speech on says once how it works, naming the accessibility button
// as the player's device prints it; speech that cannot be heard is said on
// screen instead, once; and the natural voice's download is told when it
// starts, ends and fails (VoiceText has its progress).
void ApplicationShell::UpdateSpeech() {
 auto& speech=preferences_.speech;
 // The reminder of the accessibility button ends once the player has used it
 // three times; the first menu with speech on says how it works, once.
 const int described=(std::min)(3-speech.describeUsed,TakeMenuDescribes());
 const bool introDue=speech.enabled&&!speech.introHeard&&!menu_.NoticeOpen();
 if(described>0||introDue){speech.describeUsed+=described;speech.introHeard|=introDue;speechIntro_|=introDue;preferencesDirty_=true;saveAt_=ImGui::GetTime()+.45;}
 SetMenuSpeech(speech);
 if(std::exchange(speechIntro_,false))
  menu_.ShowNotice(NoticeDialog{loc::Tf("speech.intro",AccessibilityKeyName()),loc::T("speech.setting"),{{loc::T("common.ok")}},0,true});
 if(!preferences_.speech.enabled||menu_.NoticeOpen())return;
 const auto missing=speech::Unavailable();
 if(missing!=speech::Missing::Nothing&&!speechNoticeShown_){
  speechNoticeShown_=true;
  menu_.ShowNotice(missing==speech::Missing::WindowsVoice?loc::T("speech.no_windows_voice"):loc::T("speech.no_speech_dispatcher"),loc::T("speech.setting"));
  return;
 }
 const auto status=speech::NaturalVoice();const auto voice=status.voice;
 if(voice==voiceTold_||voice==speech::Voice::System)return;
 // A voice pack's download says its size; it can be tried again from the Accessibility page.
 const bool packed=status.pack!=speech::Pack::None;
 if(voice==speech::Voice::Downloading)menu_.ShowNotice(packed?loc::Tf("speech.pack_downloading",loc::NativeName(loc::Active()),Megabytes(status.packBytes)):std::string(loc::T("speech.voice_detail")),loc::T("speech.voice"));
 else if(voice==speech::Voice::Failed)menu_.ShowNotice(packed?loc::T("speech.pack_failed"):loc::T("speech.voice_failed"),loc::T("speech.voice"));
 else if(voiceTold_==speech::Voice::Downloading)menu_.ShowNotice(loc::T("speech.voice_ready"),loc::T("speech.voice"));
 voiceTold_=voice;
}
} }
