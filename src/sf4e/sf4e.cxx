#include "../platform/FileSha256.hxx"
#include <random>
#include <time.h>
#include <windows.h>
#include <bcrypt.h>
#include <spdlog/spdlog.h>

#include "../Dimps/Dimps__Eva.hxx"
#include "../Dimps/Dimps__Platform.hxx"

#include "sf4e.hxx"
#include "sf4e__BackgroundPlay.hxx"
#include "sf4e__CrashDiagnostics.hxx"
#include "sf4e__Event.hxx"
#include "sf4e__Game.hxx"
#include "sf4e__GameEvents.hxx"
#include "sf4e__InputLag.hxx"
#include "sf4e__Pad.hxx"
#include "sf4e__Gamepads.hxx"
#include "sf4e__Platform.hxx"
#include "sf4e__UserApp.hxx"
#include "sf4e__CustomContent.hxx"
#include "sf4e__NetplayFacade.hxx"
#include "../common/MementoGuards.hxx"

std::mt19937 sf4e::localRand;
std::string sf4e::sidecarHash;
sf4e::Args sf4e::args;
HANDLE sf4e::hSyncEvent;
HANDLE sf4e::hStartupMailbox;

using rIEmSpriteAction = Dimps::Eva::IEmSpriteAction;
using rIEmSpriteNode = Dimps::Eva::IEmSpriteNode;
using rIEmTaskFunctor = Dimps::Eva::IEmTaskFunctor;
using rTask = Dimps::Eva::Task;
using rTaskCore = Dimps::Eva::TaskCore;
using rAllocator = Dimps::Platform::Allocator;
using fIEmSpriteAction = sf4e::Eva::IEmSpriteAction;
using fTask = sf4e::Eva::Task;
using fTaskCore = sf4e::Eva::TaskCore;

// https://learn.microsoft.com/en-us/windows/win32/seccng/creating-a-hash-with-cng
// Sidecar identity and executable compatibility share the handle hasher.
// This caller opens only its own DLL; GameFile retains the executable lock.
HRESULT GetHash(HINSTANCE module, std::string& output) {
    output.clear();
    wchar_t path[32768];
    const DWORD length = GetModuleFileNameW(module, path, 32768);
    if (!length) return HRESULT_FROM_WIN32(GetLastError());
    if (length >= 32768) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    char digest[65];
    const DWORD error = sf4e::platform::FileSha256(file, digest);
    CloseHandle(file);
    if (error) return HRESULT_FROM_WIN32(error);
    output = digest;
    return S_OK;
}

DWORD sf4e::Install(HINSTANCE hinstDll, const sf4e::Payload* const payload) {
	if (payload) {
		sf4e::args = payload->args;
		sf4e::hSyncEvent = payload->hSyncEvent;
		sf4e::hStartupMailbox = payload->hStartupMailbox;
		crash::ConfigureDumpChannel(payload->hDumpRequest, payload->hDumpDone, payload->hDumpMailbox);
		NetplayFacade::InitFromPayload(payload->netplay);
		NetplayFacade::ConfigureHelper(payload->helper, payload->helperError);
        NetplayFacade::ConfigureDiscord(payload->discord);
	}

	HRESULT r = GetHash(hinstDll, sidecarHash);
	if (FAILED(r)) return HRESULT_CODE(r);

	localRand.seed(time(NULL));

	Event::Install();
	Game::Install();
	GameEvents::Install();
    GameEvents::OptionUIController::Install();
	Pad::Install();
	Gamepads::Install();
	BackgroundPlay::Install();
	InputLag::Install();
	Platform::Install();
	UserApp::Install();
	custom::Install();
	return ERROR_SUCCESS;
}

void sf4e::HooksCommitted() {
	BackgroundPlay::Activate();
	InputLag::Activate();
}

void fIEmSpriteAction::RecordToAdditionalMemento(rIEmSpriteAction* a, AdditionalMemento& m) {
	m.action = *a;
}

void fIEmSpriteAction::RestoreFromAdditionalMemento(rIEmSpriteAction* a, const AdditionalMemento& m) {
	*a = m.action;
}

// Copies a functor only when its vtable has a known size that fits the
// buffer. Release builds compile asserts out, so this is the real check.
static bool RecordFunctor(rIEmTaskFunctor* functor, fTask::TaskFunctorBuf& buf, bool& has) {
	has = false;
	const DWORD vtable = *(DWORD*)functor;
	const size_t size = rIEmTaskFunctor::GetDescendantFunctorSizeByVtable(vtable);
	if (size == 0 || size > sizeof(buf)) {
		spdlog::error("Rollback: task functor vtable {:#x} has unsupported size {}", vtable, size);
		return false;
	}
	memcpy_s(&buf, sizeof(buf), functor, size);
	has = true;
	return true;
}

bool fTask::RecordToAdditionalMemento(rTask* t, AdditionalMemento& m) {
	// Copy _almost_ all the raw task state by copying all of it then
	// zeroing any pointers. While we don't strictly need to zero the
	// pointers, it'll turn incorrect accesses into null pointer
	// accesses rather than arbitrary memory accesses, which is at least
	// less dangerous.
	memcpy_s(&m.rawTask, sizeof(rTask), t, sizeof(rTask));
	*rTask::GetCancelFunctor(&m.rawTask) = nullptr;
	*rTask::GetWorkFunctor(&m.rawTask) = nullptr;
	*rTask::GetNext(&m.rawTask) = nullptr;
	*rTask::GetPrevious(&m.rawTask) = nullptr;
	*rTask::GetTaskData(&m.rawTask) = nullptr;

	// There are literally thousands of functors, and only a handful of them
	// have sizes captured. An unknown one is left out and reported.
	bool ok = true;
	rIEmTaskFunctor* cancelFunctor = *rTask::GetCancelFunctor(t);
	m.hasCancelFunctor = false;
	if (cancelFunctor) ok = RecordFunctor(cancelFunctor, m.cancelFunctor, m.hasCancelFunctor) && ok;
	rIEmTaskFunctor* workFunctor = *rTask::GetWorkFunctor(t);
	m.hasWorkFunctor = false;
	if (workFunctor) ok = RecordFunctor(workFunctor, m.workFunctor, m.hasWorkFunctor) && ok;
	return ok;
}

// Copies a recorded functor into a fresh engine allocation, since task
// cancellation frees it. Returns null and marks the restore failed when the
// size is not one RecordFunctor accepts or the engine allocation fails, so
// SaveState::Load reports it instead of copying into a null pointer.
static rIEmTaskFunctor* RestoreFunctor(rAllocator* allocator, const fTask::TaskFunctorBuf& buf) {
	const DWORD vtable = *(const DWORD*)&buf;
	const size_t size = rIEmTaskFunctor::GetDescendantFunctorSizeByVtable(vtable);
	void* functor = allocator && size != 0 && size <= sizeof(buf) ?
		(allocator->*rAllocator::publicMethods.Allocate)(size, 0, -1) : nullptr;
	if (!functor) {
		spdlog::error("Rollback: could not restore task functor vtable {:#x} size {}", vtable, size);
		sf4e::Game::MementoFailure::restore = true;
		return nullptr;
	}
	memcpy_s(functor, size, &buf, size);
	return (rIEmTaskFunctor*)functor;
}

void fTask::RestoreFromAdditionalMemento(rTask* t, const AdditionalMemento& m) {
	rAllocator* allocator = rAllocator::staticMethods.GetSingleton();

	// We shouldn't be attempting to restore into a task that has functors,
	// because we can't soundly delete them. We should only be attempting
	// to restore into newly acquired tasks which don't have functors
	// associated yet. Similarly, the raw task in the memento shouldn't
	// have any functors, or the memcpy below may create leaks.
	assert(*rTask::GetCancelFunctor(t) == nullptr);
	assert(*rTask::GetWorkFunctor(t) == nullptr);
	assert(*rTask::GetCancelFunctor((rTask*)&m.rawTask) == nullptr);
	assert(*rTask::GetWorkFunctor((rTask*)&m.rawTask) == nullptr);

	// The "raw" task shouldn't have a task data pointer- the task
	// data is stored with the TaskCore memento.
	assert(*rTask::GetTaskData((rTask*)&m.rawTask) == nullptr);

	// Set aside any non-game-state pointers, copy the old state
	// directly into place, then re-link the non-game-state pointers.
	//
	// The two key pieces of non-game-state in each task are:
	// * The next and previous pointers of the linked list, managed
	//   (mostly) by the task core itself, and
	// * The task's "private data" pointer, which is allocated by the
	//   task core when the task object pool is created and is constant
	//   for the life of the task core. The data at the pointer is mutable,
	//   but the pointer itself isn't.
	// 
	// Failure to preserve the pointers from the newly-allocated Task when
	// copying the old data into place almost certainly results in segfaults.
	// Having a stronger breakdown of the Task object so that the fields
	// could be directly copied rather than using a memcpy()-and-fix approach
	// might make this more clear, but the full contents of the tasks are
	// unknown.
	rTask* next = *rTask::GetNext(t);
	rTask* prev = *rTask::GetPrevious(t);
	void* taskData = *rTask::GetTaskData(t);
	memcpy_s(t, sizeof(rTask), &m.rawTask, sizeof(rTask));
	*rTask::GetNext(t) = next;
	*rTask::GetPrevious(t) = prev;
	*rTask::GetTaskData(t) = taskData;

	// The functors need to actually be allocated pointers, as the task cancellation
	// will attempt to free the memory that the IEmTaskFunctor* pointers are pointing
	// at. If the tasks just pointed at the memory in the AdditionalMemento, the
	// attempt to free would fail.
	if (m.hasCancelFunctor) *rTask::GetCancelFunctor(t) = RestoreFunctor(allocator, m.cancelFunctor);
	if (m.hasWorkFunctor) *rTask::GetWorkFunctor(t) = RestoreFunctor(allocator, m.workFunctor);
}


void fTaskCore::RecordToAdditionalMemento(rTaskCore* c, AdditionalMemento& m) {
	size_t taskDataSize = *rTaskCore::GetTaskDataSize(c);
	if (taskDataSize > sizeof(fTaskCore::TaskDataBuf)) {
		spdlog::error("Rollback: task data size {} exceeds {}", taskDataSize, sizeof(fTaskCore::TaskDataBuf));
		sf4e::Game::MementoFailure::record = true;
		taskDataSize = sizeof(fTaskCore::TaskDataBuf);
	}

	int i;
	rTask* cursor;
	for (cursor = rTaskCore::GetTaskHead(c), i = 0; cursor != nullptr; cursor = *rTask::GetNext(cursor), i++) {
		if (i >= MAX_TASKS_PER_CORE) {
			spdlog::error("Rollback: task core holds more than {} tasks", MAX_TASKS_PER_CORE);
			sf4e::Game::MementoFailure::record = true;
			break;
		}
		if (!fTask::RecordToAdditionalMemento(cursor, m.tasks[i])) sf4e::Game::MementoFailure::record = true;
		memcpy_s(&m.taskdata[i], sizeof(m.taskdata[i]), *rTask::GetTaskData(cursor), taskDataSize);
	}
	// Restore allocates numUsed tasks from the entries recorded here.
	m.numUsed = (c->*rTaskCore::publicMethods.GetNumUsed)();
	if (m.numUsed > i) {
		spdlog::error("Rollback: task core reports {} used tasks but lists {}", m.numUsed, i);
		sf4e::Game::MementoFailure::record = true;
		m.numUsed = i;
	}
}

void fTaskCore::RestoreFromAdditionalMemento(rTaskCore* c, const AdditionalMemento& m) {
	if (!sf4e::memento::ValidTaskCount(m.numUsed, MAX_TASKS_PER_CORE)) {
		spdlog::error("Rollback: restoring task count {} outside core capacity {}", m.numUsed, MAX_TASKS_PER_CORE);
		sf4e::Game::MementoFailure::restore = true;
		return;
	}
	rTask* cursor;
	for (
		cursor = rTaskCore::GetTaskHead(c);
		cursor != nullptr;
		cursor = *rTask::GetNext(cursor)
	) {
		rTask* victim = cursor;
		(c->*rTaskCore::publicMethods.CancelTask)(&victim);
	}
	(c->*rTaskCore::publicMethods.ReclaimCancelledTasks)();

	size_t taskDataSize = *rTaskCore::GetTaskDataSize(c);
	if (taskDataSize > sizeof(fTaskCore::TaskDataBuf)) {
		// Recording rejects such a core, so this is a different core layout
		// than the one saved. Never read past the recorded buffer.
		spdlog::error("Rollback: restoring task data size {} exceeds {}", taskDataSize, sizeof(fTaskCore::TaskDataBuf));
		sf4e::Game::MementoFailure::restore = true;
		taskDataSize = sizeof(fTaskCore::TaskDataBuf);
	}
	int i;
	for (i = 0; i < m.numUsed; i++) {
		rTask* newTask = nullptr;
		(c->*rTaskCore::publicMethods.AllocateNewTask)(
			&newTask,
			*rTask::GetPriority((rTask*)&m.tasks[i].rawTask),
			0,
			0
		);
		if (!newTask) {
			spdlog::error("Rollback: task core could not allocate task {} of {}", i + 1, m.numUsed);
			sf4e::Game::MementoFailure::restore = true;
			return;
		}
		fTask::RestoreFromAdditionalMemento(newTask, m.tasks[i]);
		memcpy_s(*rTask::GetTaskData(newTask), taskDataSize, &m.taskdata[i], taskDataSize);
	}
}
