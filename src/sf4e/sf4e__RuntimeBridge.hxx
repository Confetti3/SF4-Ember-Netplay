#pragma once
// The netplay runtime's only state shared with other threads: the published
// snapshots and the command queue. It lives for the whole process, so a reader
// on the drawing thread never dereferences `internal::runtime`, which
// StopHelper deletes. The game thread publishes; any thread reads or submits.
#include <cstddef>
#include <memory>

#include "sf4e__NetplayFacade.hxx"
#include "../netplay/BoundedMailbox.hxx"

namespace sf4e { namespace NetplayFacade { namespace bridge {

using CommandMailbox = netplay::BoundedMailbox<RuntimeCommand>;

void PublishRuntime(std::shared_ptr<const RuntimeSnapshot> snapshot);
std::shared_ptr<const RuntimeSnapshot> LatestRuntime();
void PublishPresentation(std::shared_ptr<const PresentationSnapshot> snapshot);
std::shared_ptr<const PresentationSnapshot> LatestPresentation();

// The runtime's queue while it accepts commands. A producer that already took
// a closed queue can no longer push into it, and never reaches its successor.
void OpenCommands(std::shared_ptr<CommandMailbox> mailbox);
void CloseCommands();
bool PushCommand(RuntimeCommand command, std::size_t bytes);

// Back to the state before the first publish (the runtime has stopped).
void Reset();

} } } // namespace sf4e::NetplayFacade::bridge
