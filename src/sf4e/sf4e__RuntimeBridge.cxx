#include "sf4e__RuntimeBridge.hxx"

#include <mutex>
#include <utility>

namespace sf4e { namespace NetplayFacade { namespace bridge {
namespace {
struct Shared {
	std::mutex mutex;
	std::shared_ptr<const RuntimeSnapshot> runtime;
	std::shared_ptr<const PresentationSnapshot> presentation;
	std::shared_ptr<CommandMailbox> commands;
};

std::shared_ptr<const RuntimeSnapshot> EmptyRuntime() {
	static const auto empty = std::make_shared<const RuntimeSnapshot>();
	return empty;
}

std::shared_ptr<const PresentationSnapshot> EmptyPresentation() {
	static const auto empty = [] {
		auto presentation = std::make_shared<PresentationSnapshot>();
		presentation->runtime = EmptyRuntime();
		return std::shared_ptr<const PresentationSnapshot>(std::move(presentation));
	}();
	return empty;
}

// Never destroyed: the drawing thread can still read while the DLL unloads,
// and Windows runs static destructors under the loader lock.
Shared& State() {
	static Shared* const shared = [] {
		auto* state = new Shared();
		state->runtime = EmptyRuntime();
		state->presentation = EmptyPresentation();
		return state;
	}();
	return *shared;
}

// Swaps under the lock; the previous value is released by the caller's copy,
// outside it, unless a reader still holds it.
template <typename T> std::shared_ptr<T> Exchange(std::shared_ptr<T>& slot, std::shared_ptr<T> next) {
	std::lock_guard<std::mutex> lock(State().mutex);
	std::swap(slot, next);
	return next;
}

template <typename T> std::shared_ptr<T> Load(const std::shared_ptr<T>& slot) {
	std::lock_guard<std::mutex> lock(State().mutex);
	return slot;
}
}

void PublishRuntime(std::shared_ptr<const RuntimeSnapshot> snapshot) { Exchange(State().runtime, std::move(snapshot)); }
std::shared_ptr<const RuntimeSnapshot> LatestRuntime() { return Load(State().runtime); }
void PublishPresentation(std::shared_ptr<const PresentationSnapshot> snapshot) { Exchange(State().presentation, std::move(snapshot)); }
std::shared_ptr<const PresentationSnapshot> LatestPresentation() { return Load(State().presentation); }

void OpenCommands(std::shared_ptr<CommandMailbox> mailbox) { Exchange(State().commands, std::move(mailbox)); }

void CloseCommands() {
	if (const auto closed = Exchange(State().commands, std::shared_ptr<CommandMailbox>())) closed->Close();
}

bool PushCommand(RuntimeCommand command, std::size_t bytes) {
	const auto mailbox = Load(State().commands);
	return mailbox && mailbox->TryPush(std::move(command), bytes);
}

void Reset() {
	CloseCommands();
	PublishPresentation(EmptyPresentation());
	PublishRuntime(EmptyRuntime());
}

} } } // namespace sf4e::NetplayFacade::bridge
