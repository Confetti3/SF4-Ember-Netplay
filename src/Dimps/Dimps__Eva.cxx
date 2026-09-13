#include <windows.h>

#include "Dimps__Eva.hxx"

namespace Eva = Dimps::Eva;
using Dimps::Eva::IEmSpriteAction;
using Dimps::Eva::IEmSpriteNode;
using Dimps::Eva::IEmTaskFunctor;
using Dimps::Eva::Task;
using Dimps::Eva::TaskCore;
using Dimps::Eva::TaskCoreRegistry;

IEmSpriteAction::__publicMethods IEmSpriteAction::publicMethods;
IEmSpriteNode::__publicMethods IEmSpriteNode::publicMethods;
TaskCore::__publicMethods TaskCore::publicMethods;
TaskCoreRegistry::__staticMethods TaskCoreRegistry::staticMethods;

void Eva::Locate(HMODULE peRoot) {
	IEmSpriteAction::Locate(peRoot);
	IEmSpriteNode::Locate(peRoot);
	TaskCore::Locate(peRoot);
	TaskCoreRegistry::Locate(peRoot);
}

void IEmSpriteAction::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)(&publicMethods.GetActionState) = (PVOID)(peRootOffset + 0x2ebc80);
}

void IEmSpriteNode::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)(&publicMethods.GetNumChildren) = (PVOID)(peRootOffset + 0x2ea4b0);
	*(PVOID*)(&publicMethods.GetChildByIdx) = (PVOID)(peRootOffset + 0x2ea4f0);
	*(PVOID*)(&publicMethods.GetAction) = (PVOID)(peRootOffset + 0x2ea680);
}

size_t IEmTaskFunctor::GetDescendantFunctorSizeByVtable(DWORD functorVtable) {
	switch (functorVtable) {
	case 0x009501c8:
		return 0x0c; // EmTaskFunctor<class_Dimps::Game::Battle::Chara::Unit>
	case 0x00950894:
		return 0x0c; // EmTaskFunctor<class_Dimps::Game::Battle::Effect::Unit>
	case 0x00950994:
		return 0x0c; // EmTaskFunctor<class_Dimps::Game::Battle::Light::Unit>
	case 0x00950ab4:
		return 0x10; // EmTaskFunctor<class_Dimps::Game::Battle::Camera::Unit>
	case 0x00950bc8:
		return 0x0c; // EmTaskFunctor<class_Dimps::Game::Battle::Hud::Cockpit::Unit>
	case 0x009510d0:
		return 0x0c; // EmTaskFunctor<class_Dimps::Game::Battle::Hud::Announce::Unit>
	case 0x00951268:
		return 0x0c; // EmTaskFunctor<class_Dimps::Game::Battle::Hud::Result::Unit>
	case 0x0095150c:
		return 0x0c; // EmTaskFunctor<class_Dimps::Game::Battle::Hud::Notice::Unit>
	case 0x009516d0:
		return 0x0c; // EmTaskFunctor<class_Dimps::Game::Battle::Hud::Continue::Unit>
	case 0x0095176c:
		return 0x0c; // EmTaskFunctor<class_Dimps::Game::Battle::Hud::Cursor::Unit>
	case 0x00951840:
		return 0x0c; // EmTaskFunctor<class_Dimps::Game::Battle::Hud::Subtitle::Unit>
	case 0x009518dc:
		return 0x0c; // EmTaskFunctor<class_Dimps::Game::Battle::Hud::Training::Unit>
	case 0x00951ab8:
		return 0x0c; // EmTaskFunctor<class_Dimps::Game::Battle::Sound::Unit>
	case 0x00951b88:
		return 0x0c; // EmTaskFunctor<class_Dimps::Game::Battle::Sound::BgmPlayer>
	case 0x0095213c:
		return 0x0c; // EmTaskFunctor<class_Dimps::Game::Battle::Command::Unit>
	case 0x009522d4:
		return 0x0c; // EmTaskFunctor<class_Dimps::Game::Battle::Stage::Unit>
	case 0x00953d24:
		return 0x0c; // EmTaskFunctor<class_Dimps::Game::Battle::Com::Unit>
	case 0x00953e64:
		return 0x0c; // EmTaskFunctor<class_Dimps::Game::Battle::Vfx::Unit>
	case 0x00954848:
		return 0x0c; // EmTaskFunctor<class_Dimps::Game::Battle::Vfx::ColorFadeUnit>
	case 0x009548b8:
		return 0x0c; // EmTaskFunctor<class_Dimps::Game::Battle::Pause::Unit>
	case 0x00954944:
		return 0x0c; // EmTaskFunctor<class_Dimps::Game::Battle::Network::Unit>
	case 0x009550a8:
		return 0x10; // EmTaskFunctor<class_Dimps::Game::Battle::System>
	default:
		return 0;
	}
}

DWORD* Task::Get0x8(Task* t) {
	return (DWORD*)((unsigned int)t + 0x8);
}

DWORD* Task::GetFlags(Task* t) {
	return (DWORD*)((unsigned int)t + 0x4);
}

DWORD* Task::GetPhase(Task* t) {
	return (DWORD*)((unsigned int)t + 0x2c);
}

DWORD* Task::GetState(Task* t) {
	return (DWORD*)((unsigned int)t + 0x28);
}

IEmTaskFunctor** Task::GetCancelFunctor(Task* t) {
	return (IEmTaskFunctor**)((unsigned int)t + 0x18);
}

Task** Task::GetNext(Task* t) {
	return (Task**)((unsigned int)t + 0x20);
}

Task** Task::GetPrevious(Task* t) {
	return (Task**)((unsigned int)t + 0x1c);
}

int* Task::GetPriority(Task* t) {
	return (int*)((unsigned int)t + 0xc);
}

void** Task::GetTaskData(Task* t) {
	return (void**)((unsigned int)t + 0x30);
}

IEmTaskFunctor** Task::GetWorkFunctor(Task* t) {
	return (IEmTaskFunctor**)((unsigned int)t + 0x14);
}

void TaskCore::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)(&publicMethods.AllocateNewTask) = (PVOID)(peRootOffset + 0x2d5380);
	*(PVOID*)(&publicMethods.CancelTask) = (PVOID)(peRootOffset + 0x2d5460);
	*(PVOID*)(&publicMethods.GetName) = (PVOID)(peRootOffset + 0x2d4200);
	*(PVOID*)(&publicMethods.GetNumUsed) = (PVOID)(peRootOffset + 0x28ecf0);
	*(PVOID*)(&publicMethods.GetTaskName) = (PVOID)(peRootOffset + 0x2d55c0);
	*(PVOID*)(&publicMethods.ReclaimCancelledTasks) = (PVOID)(peRootOffset + 0x2d5cf0);
}

Task** TaskCore::GetTaskBuffer(TaskCore* c) {
	return *(Task***)((unsigned int)c + 0x38);
}

size_t* TaskCore::GetTaskDataSize(TaskCore* c) {
	return (size_t*)((unsigned int)c + 0x24);
}

Task* TaskCore::GetTaskHead(TaskCore* c) {
	return *(Task**)((unsigned int)c + 0x48);
}

void TaskCoreRegistry::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	staticMethods.GetCoreByIndex = (TaskCore * (*)(int))(peRootOffset + 0x2d62b0);
	staticMethods.GetNumActiveCores = (int (*)())(peRootOffset + 0x2d69b0);
}
