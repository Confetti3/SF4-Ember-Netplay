#include <random>
#include <time.h>
#include <windows.h>
#include <bcrypt.h>

#include "../Dimps/Dimps__Eva.hxx"
#include "../Dimps/Dimps__Platform.hxx"

#include "sf4e.hxx"
#include "sf4e__Event.hxx"
#include "sf4e__Game.hxx"
#include "sf4e__GameEvents.hxx"
#include "sf4e__Pad.hxx"
#include "sf4e__Platform.hxx"
#include "sf4e__UserApp.hxx"
#include "sf4e__NetplayFacade.hxx"

std::mt19937 sf4e::localRand;
std::string sf4e::sidecarHash;
sf4e::Args sf4e::args;
HANDLE sf4e::hSyncEvent;

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
#define NT_SUCCESS(Status)          (((NTSTATUS)(Status)) >= 0)
#define STATUS_UNSUCCESSFUL         ((NTSTATUS)0xC0000001L)

#define SF4E_HASH_BLOCK_SIZE 1024

HRESULT GetHash(HINSTANCE hinstDll, std::string& output) {
	int i;
	CHAR hexPair[3];
	BCRYPT_ALG_HANDLE hAlg = NULL;
	BCRYPT_HASH_HANDLE hHash = NULL;
	NTSTATUS status;
	DWORD cbData = 0;
	DWORD cbHash = 0;
	DWORD cbHashObject = 0;
	DWORD cbRead = 0;
	PBYTE pbHash = NULL;
	PBYTE pbHashObject = NULL;
	HRESULT ret = S_OK;
	HANDLE hFile = INVALID_HANDLE_VALUE;
	TCHAR dllPath[MAX_PATH];
	BYTE rgbFile[SF4E_HASH_BLOCK_SIZE];
	BOOL bResult = FALSE;

	if (!GetModuleFileName(
		hinstDll,
		dllPath,
		MAX_PATH
	)) {
		ret = E_FAIL;
		goto Cleanup;
	}

	//open an algorithm handle
	if (!NT_SUCCESS(status = BCryptOpenAlgorithmProvider(
		&hAlg,
		BCRYPT_SHA256_ALGORITHM,
		NULL,
		0)))
	{
		wprintf(L"**** Error 0x%x returned by BCryptOpenAlgorithmProvider\n", status);
		ret = E_FAIL;
		goto Cleanup;
	}

	//calculate the size of the buffer to hold the hash object
	if (!NT_SUCCESS(status = BCryptGetProperty(
		hAlg,
		BCRYPT_OBJECT_LENGTH,
		(PBYTE)&cbHashObject,
		sizeof(DWORD),
		&cbData,
		0)))
	{
		wprintf(L"**** Error 0x%x returned by BCryptGetProperty\n", status);
		ret = E_FAIL;
		goto Cleanup;
	}

	//allocate the hash object on the heap
	pbHashObject = (PBYTE)HeapAlloc(GetProcessHeap(), 0, cbHashObject);
	if (NULL == pbHashObject)
	{
		wprintf(L"**** memory allocation failed\n");
		ret = E_FAIL;
		goto Cleanup;
	}

	//calculate the length of the hash
	if (!NT_SUCCESS(status = BCryptGetProperty(
		hAlg,
		BCRYPT_HASH_LENGTH,
		(PBYTE)&cbHash,
		sizeof(DWORD),
		&cbData,
		0)))
	{
		wprintf(L"**** Error 0x%x returned by BCryptGetProperty\n", status);
		ret = E_FAIL;
		goto Cleanup;
	}

	//allocate the hash buffer on the heap
	pbHash = (PBYTE)HeapAlloc(GetProcessHeap(), 0, cbHash);
	if (NULL == pbHash)
	{
		wprintf(L"**** memory allocation failed\n");
		ret = E_FAIL;
		goto Cleanup;
	}

	//create a hash
	if (!NT_SUCCESS(status = BCryptCreateHash(
		hAlg,
		&hHash,
		pbHashObject,
		cbHashObject,
		NULL,
		0,
		0)))
	{
		wprintf(L"**** Error 0x%x returned by BCryptCreateHash\n", status);
		ret = E_FAIL;
		goto Cleanup;
	}

	hFile = CreateFile(dllPath,
		GENERIC_READ,
		FILE_SHARE_READ,
		NULL,
		OPEN_EXISTING,
		FILE_FLAG_SEQUENTIAL_SCAN,
		NULL);

	if (INVALID_HANDLE_VALUE == hFile)
	{
		ret = E_FAIL;
		goto Cleanup;
	}

	while (bResult = ReadFile(hFile, rgbFile, SF4E_HASH_BLOCK_SIZE, &cbRead, NULL)) {
		if (0 == cbRead)
		{
			break;
		}

		//hash some data
		if (!NT_SUCCESS(status = BCryptHashData(
			hHash,
			rgbFile,
			cbRead,
			0)))
		{
			wprintf(L"**** Error 0x%x returned by BCryptHashData\n", status);
			ret = E_FAIL;
			goto Cleanup;
		}
	}

	//close the hash
	if (!NT_SUCCESS(status = BCryptFinishHash(
		hHash,
		pbHash,
		cbHash,
		0)))
	{
		wprintf(L"**** Error 0x%x returned by BCryptFinishHash\n", status);
		ret = E_FAIL;
		goto Cleanup;
	}

	for (i = 0; i < cbHash; i++) {
		snprintf(hexPair, 3, "%02hhx", pbHash[i]);
		output.append(hexPair);
	}

	ret = S_OK;
Cleanup:

	if (hAlg) {
		BCryptCloseAlgorithmProvider(hAlg, 0);
	}

	if (hHash) {
		BCryptDestroyHash(hHash);
	}

	if (pbHash) {
		HeapFree(GetProcessHeap(), 0, pbHash);
	}

	if (pbHashObject) {
		HeapFree(GetProcessHeap(), 0, pbHashObject);
	}

	if (hFile != INVALID_HANDLE_VALUE) {
		CloseHandle(hFile);
	}

	return ret;
}

void sf4e::Install(HINSTANCE hinstDll, const sf4e::Payload* const payload) {
	if (payload) {
		sf4e::args = payload->args;
		sf4e::hSyncEvent = payload->hSyncEvent;
		NetplayFacade::InitFromPayload(payload->netplay);
		NetplayFacade::ConfigureHelper(payload->helper, payload->helperError);
        NetplayFacade::ConfigureDiscord(payload->discord);
	}

	HRESULT r = GetHash(hinstDll, sidecarHash);
	if (r != S_OK) {
		MessageBox(NULL, TEXT("Could not hash sidecar!"), NULL, MB_OK);
	}

	localRand.seed(time(NULL));

	Event::Install();
	Game::Install();
	GameEvents::Install();
	Pad::Install();
	Platform::Install();
	UserApp::Install();
}

void fIEmSpriteAction::RecordToAdditionalMemento(rIEmSpriteAction* a, AdditionalMemento& m) {
	m.action = *a;
}

void fIEmSpriteAction::RestoreFromAdditionalMemento(rIEmSpriteAction* a, const AdditionalMemento& m) {
	*a = m.action;
}

void fTask::RecordToAdditionalMemento(rTask* t, AdditionalMemento& m) {
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

	rIEmTaskFunctor* cancelFunctor = *rTask::GetCancelFunctor(t);
	m.hasCancelFunctor = cancelFunctor != nullptr;
	if (m.hasCancelFunctor) {
		size_t cancelFunctorSize = rIEmTaskFunctor::GetDescendantFunctorSizeByVtable(*(DWORD*)cancelFunctor);
		// There are literally thousands of functors, and only a handful of them
		// have sizes captured. TaskFunctorBuf should be large enough to capture
		// all of them, but asserting that with static analysis is borderline
		// impossible.
		assert(cancelFunctorSize > 0);
		assert(cancelFunctorSize <= sizeof(TaskFunctorBuf));
		memcpy_s(&m.cancelFunctor, cancelFunctorSize, cancelFunctor, cancelFunctorSize);
	}

	rIEmTaskFunctor* workFunctor = *rTask::GetWorkFunctor(t);
	m.hasWorkFunctor = workFunctor != nullptr;
	if (m.hasWorkFunctor) {
		size_t workFunctorSize = rIEmTaskFunctor::GetDescendantFunctorSizeByVtable(*(DWORD*)workFunctor);
		// There are literally thousands of functors, and only a handful of them
		// have sizes captured. TaskFunctorBuf should be large enough to capture
		// all of them, but asserting that with static analysis is borderline
		// impossible.
		assert(workFunctorSize > 0);
		assert(workFunctorSize <= sizeof(TaskFunctorBuf));
		memcpy_s(&m.workFunctor, workFunctorSize, workFunctor, workFunctorSize);
	}
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
	if (m.hasCancelFunctor) {
		size_t cancelFunctorSize = rIEmTaskFunctor::GetDescendantFunctorSizeByVtable(*(DWORD*)&m.cancelFunctor);
		void* newCancelFunctor = (allocator->*rAllocator::publicMethods.Allocate)(cancelFunctorSize, 0, -1);
		memcpy_s(newCancelFunctor, cancelFunctorSize, &m.cancelFunctor, cancelFunctorSize);
		*rTask::GetCancelFunctor(t) = (rIEmTaskFunctor*)newCancelFunctor;
	}

	if (m.hasWorkFunctor) {
		size_t workFunctorSize = rIEmTaskFunctor::GetDescendantFunctorSizeByVtable(*(DWORD*)&m.workFunctor);
		void* newWorkFunctor = (allocator->*rAllocator::publicMethods.Allocate)(workFunctorSize, 0, -1);
		memcpy_s(newWorkFunctor, workFunctorSize, &m.workFunctor, workFunctorSize);
		*rTask::GetWorkFunctor(t) = (rIEmTaskFunctor*)newWorkFunctor;
	}
}

void fTaskCore::RecordToAdditionalMemento(rTaskCore* c, AdditionalMemento& m) {
	m.numUsed = (c->*rTaskCore::publicMethods.GetNumUsed)();
	size_t taskDataSize = *rTaskCore::GetTaskDataSize(c);
	assert(taskDataSize <= sizeof(fTaskCore::TaskDataBuf));

	int i;
	rTask* cursor;
	for (cursor = rTaskCore::GetTaskHead(c), i = 0; cursor != nullptr; cursor = *rTask::GetNext(cursor), i++) {
		fTask::RecordToAdditionalMemento(cursor, m.tasks[i]);
		memcpy_s(&m.taskdata[i], taskDataSize, *rTask::GetTaskData(cursor), taskDataSize);
	}
}

void fTaskCore::RestoreFromAdditionalMemento(rTaskCore* c, const AdditionalMemento& m) {
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
	assert(taskDataSize <= sizeof(fTaskCore::TaskDataBuf));
	int i;
	for (i = 0; i < m.numUsed; i++) {
		rTask* newTask;
		(c->*rTaskCore::publicMethods.AllocateNewTask)(
			&newTask,
			*rTask::GetPriority((rTask*)&m.tasks[i].rawTask),
			0,
			0
		);
		fTask::RestoreFromAdditionalMemento(newTask, m.tasks[i]);
		memcpy_s(*rTask::GetTaskData(newTask), taskDataSize, &m.taskdata[i], taskDataSize);
	}
}
