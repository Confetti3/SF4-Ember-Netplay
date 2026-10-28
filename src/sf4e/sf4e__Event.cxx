#include <windows.h>
#include <detours/detours.h>

#include "../Dimps/Dimps__Event.hxx"
#include "sf4e__Event.hxx"
#include "sf4e__GameEvents.hxx"

namespace rEvent = Dimps::Event;
namespace fEvent = sf4e::Event;
using rEventController = rEvent::EventController;
using fEventController = fEvent::EventController;

bool fEventController::bHaltAfterNext = false;
char* fEventController::replacementEvent = nullptr;
bool fEventController::bUpdateAllowed = true;

void fEvent::Install() {
	fEventController::Install();
}

void fEventController::Install() {
	void (fEventController:: * _fQueueEvent)(char*, DWORD, DWORD, DWORD) = &QueueEvent;
	void (fEventController:: * _fRunUpdate)() = &RunUpdate;
	DetourAttach((PVOID*)&rEventController::publicMethods.QueueEvent, *(PVOID*)&_fQueueEvent);
	DetourAttach((PVOID*)&rEventController::publicMethods.RunUpdate, *(PVOID*)&_fRunUpdate);
}

void fEventController::QueueEvent(char* eventName, DWORD arg1, DWORD arg2, DWORD arg3) {
	rEventController* _this = (rEventController*)this;

	if (replacementEvent) {
		eventName = replacementEvent;
		replacementEvent = nullptr;
	}
	// TrainingMode queues its CharaSelect here; this hook already exists, where
	// catching it at TrainingMode itself would need a new one. Nothing is
	// compared unless Ember's Training select is waiting.
	sf4e::GameEvents::TrainingMode::ReplaceCharaSelect(eventName);

	(_this->*rEventController::publicMethods.QueueEvent)(eventName, arg1, arg2, arg3);
}

void fEventController::ReplaceNextEvent(char* replacementEventName) {
	replacementEvent = replacementEventName;
}

void fEventController::RunUpdate() {
	if (bUpdateAllowed) {
		(this->*rEventController::publicMethods.RunUpdate)();

		if (bHaltAfterNext) {
			bHaltAfterNext = false;
			bUpdateAllowed = false;
		}
	}
}