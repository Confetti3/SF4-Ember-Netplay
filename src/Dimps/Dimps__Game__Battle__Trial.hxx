#pragma once

#include <windows.h>
#include <cstring>

namespace Dimps {
	namespace Game {
		namespace Battle {
			namespace Trial {
				void Locate(HMODULE peRoot);

				// The game's std::string as SetTask reads it (MSVC x86: a 16-byte
				// buffer or a pointer, then size and capacity). The game only
				// reads it, so a long text points at the caller's buffer and
				// nothing is ever freed.
				struct Text {
					union { char buffer[16]; const char* pointer; };
					unsigned size, capacity;
					void Set(const char* text, size_t length) {
						size = length;
						if (length < sizeof buffer) { memcpy(buffer, text, length); buffer[length] = 0; capacity = sizeof buffer - 1; }
						else { pointer = text; capacity = length; }
					}
				};
				static_assert(sizeof(Text) == 0x18, "the game's string is 24 bytes");

				// The task-list widget Trial mode embeds at +0xc94: the Scaleform
				// movie ui\trial_mode\trial_tasks.emz with the fighter's command
				// texts. Init starts the loads; Update creates the movie once they
				// are in and sends the pending cursor; Draw queues it for the frame.
				struct TaskList
				{
					typedef struct __publicMethods {
						TaskList* (TaskList::* Construct)();
						void (TaskList::* Destruct)();
						void (TaskList::* Init)(int fighterId);
						void (TaskList::* Update)();
						void (TaskList::* Release)();
						void (TaskList::* Draw)();
						// One zero-length movie step, run after the rows are set.
						void (TaskList::* Advance)();
						// Row texts: three task ids and a trailing one Trial mode leaves empty.
						void (TaskList::* SetTask)(int index, const Text& a, const Text& b, const Text& c, const Text& d);
						// Pending until the next Update with a movie.
						void (TaskList::* SetTaskCursor)(int index);
						void (TaskList::* ClearAllTask)();
						void (TaskList::* SetPosition)(float x, float y);
					} __publicMethods;

					static void Locate(HMODULE peRoot);
					static __publicMethods publicMethods;

					// The constructor clears fields up to +0x4c and the next
					// member of Trial mode's object starts at +0xce4.
					static const size_t Size = 0x50;
					// Set to ask Update for a fresh movie; Trial mode does so at
					// every trial start.
					static int* GetReloadRequest(TaskList* t) { return (int*)((unsigned int)t + 0x38); }
					// Set by Update once the movie exists; Trial mode clears it and
					// fills the rows.
					static int* GetMovieCreated(TaskList* t) { return (int*)((unsigned int)t + 0x4c); }
				};
			}
		}
	}
}
