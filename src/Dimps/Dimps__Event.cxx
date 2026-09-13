#include <windows.h>

#include "Dimps__Event.hxx"

namespace Event = Dimps::Event;
using Event::EventBase;
using Event::EventBaseWithEC;
using Event::EventController;

EventBase::__publicMethods EventBase::publicMethods;
EventBaseWithEC::__publicMethods EventBaseWithEC::publicMethods;
EventController::__publicMethods EventController::publicMethods;

void Event::Locate(HMODULE peRoot) {
	EventBase::Locate(peRoot);
	EventBaseWithEC::Locate(peRoot);
	EventController::Locate(peRoot);
}

char* EventBase::GetName(EventBase* e) {
	return (char*)((unsigned int)e + 0xc);
}

EventController** EventBase::GetSourceController(EventBase* e) {
	return (EventController**)((unsigned int)e + 0x8);
}

void EventBase::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.IsTerminationComplete = (PVOID)(peRootOffset + 0x0a52e0);
}

EventBase* EventBaseWithEC::FindForegroundEvent(EventBaseWithEC* start, char** eventNames, int numEvents) {
	EventBaseWithEC* cursor = start;
	for (int i = 0; i < numEvents; i++) {
		char* eventName = eventNames[i];

		EventController* controller = (cursor->*EventBaseWithEC::publicMethods.GetChildEventController)();
		EventBase* child = (controller->*EventController::publicMethods.GetForegroundEvent)();
		int childStatus = (controller->*EventController::publicMethods.GetForegroundEventStatus)();
		if (childStatus != EventController::EQS_FOREGROUND || strcmp(EventBase::GetName(child), eventName) != 0) {
			return nullptr;
		}
		cursor = (EventBaseWithEC*)child;
	}
	return cursor;
}

void EventBaseWithEC::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.GetChildEventController = (PVOID)(peRootOffset + 0x0246b0);
}

void EventController::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.EnterTerminalState = (PVOID)(peRootOffset + 0x2aa3f0);
	*(PVOID*)&publicMethods.GetForegroundEvent = (PVOID)(peRootOffset + 0x2aa420);
	*(PVOID*)&publicMethods.GetForegroundEventStatus = (PVOID)(peRootOffset + 0x2aa3c0);
	*(PVOID*)&publicMethods.QueueEvent = (PVOID)(peRootOffset + 0x2ab3b0);
	*(PVOID*)&publicMethods.RunUpdate = (PVOID)(peRootOffset + 0x2aac80);
}
