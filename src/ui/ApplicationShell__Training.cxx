#include "ApplicationShell.hxx"
#include "MenuRows.hxx"
#include "../common/Localization.hxx"

namespace sf4e { namespace ui {
// The game's Play offline > Training opens this instead of its own character
// select: the fighter and stage are the ones rooms use, the dummy its own pick.
std::vector<MenuEntry> ApplicationShell::TrainingSelectRows(const ShellView& v,std::string& title) const {
 title=loc::T("training_select.title");
 const std::string& error=v.selectionError.empty()?v.dummyError:v.selectionError;
 auto start=Row("training-start",loc::T("training_select.start"),error.empty()?loc::T("training_select.start_detail"):error,error.empty());
 start.hint=loc::T("training_select.start");
 return {Row("training-fighter",loc::T("selection.fighter"),v.selectionSummary),Row("training-dummy",loc::T("training.dummy"),v.dummySummary),
  Row("training-stage",loc::T("selection.stage"),v.stageName),std::move(start)};
}
void ApplicationShell::TrainingSelectActivate(const MenuAction& a,const ShellView& v,const Submit& submit) {
 if(a.id=="training-start"){
  ShellAction training;training.startTraining=true;
  if(!submit(std::move(training)))error_=loc::T("error.queue_failed");
  return;
 }
 selectionForDummy_=a.id=="training-dummy";
 selectionFresh_=true;selectionOpenOn_=a.id=="training-stage"?"stage":"";
 menu_.navigation.Push("selection");
}
} }
