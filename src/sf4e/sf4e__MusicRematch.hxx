#pragma once

namespace sf4e { namespace Music { namespace rematch {
// Private native audio adapter. Calls other than Install run on the outer
// game thread, outside rollback. The native BgmPlayer address never changes.
bool Install();
void TrackPlayer(void* player);
void Prepare(const char* path, int choice);
enum class Progress { Waiting, Applied, Failed };
Progress Tick(bool mayApply);
void CancelPending();
} } }
