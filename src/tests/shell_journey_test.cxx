#include "shell_replay_journey.hxx"
#include "shell_additional_journeys.hxx"
#include <iostream>
#include <exception>

int main() {
 try {
  ButtonPromptJourneys(); ControllerJourneys(); ButtonLayoutJourneys(); RoomJourneys(); FollowJourneys(); PostMatchJourneys(); PostMatchLiveAnswerJourney(); ArcadeJourneys(); ReplayJourneys(); TrainingSettingsAndHome(); TrainingFromRoom(); PracticeRoomCreate(); DelayPolicyCreate(); DelayPolicyInRoom(); TableCountCreate(); WiredOnlyJourney(); TrainingInRoom();
  KeyboardJourneys(); RightClickBack(); HomeCardRight(); MemberPopout(); RoomPartsHide(); ChatJourneys(); NoticeOverDialogs(); LanguageSaveFailure(); UpdateLaunchJourneys(); NewsAndNotifications();
  SessionReports(); RecoveryWindow(); ProblemReportJourneys(); SelectorPages(); SelectorFromProfile();
  DeveloperSelectors(); SelectorRandomTile(); TrainingJourneys(); PresentationJourneys(); AppearanceGalleries(); SelectorSpeech(); V2AuditJourneys(); SettingsJourneys();
  std::cout << "Shell journeys through the renderer passed.\n";
  return 0;
 } catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
 }
}
