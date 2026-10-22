#include "shell_replay_journey.hxx"
#include "shell_additional_journeys.hxx"
#include <iostream>
#include <exception>

int main() {
 try {
  ButtonPromptJourneys(); ControllerJourneys(); ButtonLayoutJourneys(); RoomJourneys(); ReplayJourneys(); TrainingSettingsAndHome(); TrainingFromRoom(); PracticeRoomCreate(); DelayPolicyCreate(); DelayPolicyInRoom(); TrainingInRoom();
  KeyboardJourneys(); RightClickBack(); MemberPopout(); ChatJourneys(); NoticeOverDialogs(); LanguageSaveFailure(); UpdateLaunchJourneys();
  SessionReports(); RecoveryWindow(); ProblemReportJourneys(); SelectorPages(); SelectorFromHome();
  DeveloperSelectors(); TrainingJourneys(); PresentationJourneys(); AppearanceGalleries();
  std::cout << "Shell journeys through the renderer passed.\n";
  return 0;
 } catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
 }
}
