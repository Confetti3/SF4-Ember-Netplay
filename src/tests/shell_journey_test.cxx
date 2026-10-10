#include "shell_replay_journey.hxx"
#include "shell_additional_journeys.hxx"
#include <iostream>
#include <exception>

int main() {
 try {
  RoomJourneys(); ReplayJourneys(); TrainingFromHome(); TrainingFromRoom(); TrainingInRoom();
  KeyboardJourneys(); ChatJourneys(); NoticeOverDialogs(); LanguageSaveFailure();
  SessionReports(); RecoveryWindow(); ProblemReportJourneys(); SelectorPages(); SelectorFromHome();
  DeveloperSelectors(); TrainingJourneys(); PresentationJourneys(); AppearanceGalleries();
  std::cout << "Shell journeys through the renderer passed.\n";
  return 0;
 } catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
 }
}
