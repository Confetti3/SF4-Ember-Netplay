#include "ApplicationShell.hxx"
#include "MenuRows.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <string>
#include <utility>

namespace sf4e { namespace ui {
// Settings, Accessibility: speech and how it reads. Everything but Speech
// itself waits for speech to be on, like the ready sound's volume.
std::vector<MenuEntry> ApplicationShell::AccessibilityRows(const ShellView& v) const {
 const auto& speech=preferences_.speech;const bool speaking=v.canEditPreferences&&speech.enabled;
 const auto onOff=[](bool on){return std::string(on?loc::T("common.on"):loc::T("common.off"));};
 return {Value("speech",loc::T("speech.setting"),onOff(speech.enabled),loc::T("speech.setting_detail"),v.canEditPreferences),
  Value("speech-rate",loc::T("speech.rate"),std::to_string(speech.rate),loc::T("speech.rate_detail"),speaking),
  Value("speech-volume",loc::T("speech.volume"),std::to_string(speech.volume)+"%",loc::T("speech.volume_detail"),speaking),
  Value("speech-details",loc::T("speech.details"),onOff(speech.details),loc::T("speech.details_detail"),speaking),
  Value("speech-hints",loc::T("speech.hints"),onOff(speech.hints),loc::T("speech.hints_detail"),speaking),
  Value("speech-position",loc::T("speech.read_position"),onOff(speech.position),loc::T("speech.read_position_detail"),speaking),
  Value("speech-chime",loc::T("speech.chime"),onOff(speech.chime),loc::T("speech.chime_detail"),speaking),
  Value("speech-chime-volume",loc::T("speech.chime_volume"),std::to_string(speech.chimeVolume)+"%",loc::T("speech.chime_volume_detail"),speaking&&speech.chime),
  InfoRow("speech-voice",loc::T("speech.voice"),VoiceText(speech::NaturalVoice()),loc::T("speech.voice_detail"))};
}
bool ApplicationShell::AdjustSpeech(const MenuAction& a) {
 auto& speech=preferences_.speech;
 const auto step=[&](int& value,int least,int most,int by){value=(std::max)(least,(std::min)(most,value+by*a.delta));};
 if(a.id=="speech"){speechIntro_=a.delta>0&&!speech.enabled;speech.enabled=a.delta>0;}
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
 SetMenuSpeech(preferences_.speech);
 if(std::exchange(speechIntro_,false))
  menu_.ShowNotice(NoticeDialog{loc::Tf("speech.intro",AccessibilityKeyName()),loc::T("speech.setting"),{{loc::T("common.ok")}},0,true});
 if(!preferences_.speech.enabled||menu_.NoticeOpen())return;
 const auto missing=speech::Unavailable();
 if(missing!=speech::Missing::Nothing&&!speechNoticeShown_){
  speechNoticeShown_=true;
  menu_.ShowNotice(missing==speech::Missing::WindowsVoice?loc::T("speech.no_windows_voice"):loc::T("speech.no_speech_dispatcher"),loc::T("speech.setting"));
  return;
 }
 const auto voice=speech::NaturalVoice().voice;
 if(voice==voiceTold_||voice==speech::Voice::System)return;
 if(voice==speech::Voice::Downloading)menu_.ShowNotice(loc::T("speech.voice_detail"),loc::T("speech.voice"));
 else if(voice==speech::Voice::Failed)menu_.ShowNotice(loc::T("speech.voice_failed"),loc::T("speech.voice"));
 else if(voiceTold_==speech::Voice::Downloading)menu_.ShowNotice(loc::T("speech.voice_ready"),loc::T("speech.voice"));
 voiceTold_=voice;
}
} }
