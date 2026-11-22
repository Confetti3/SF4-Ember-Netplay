#pragma once

// A crash inside a Windows speech engine (sapi.dll, or the engine it loads,
// such as MSTTSEngine_OneCore.dll) kills the game on the speech thread a
// moment after the menu opens, and with speech still on it does so at every
// start. The crash handler leaves a marker beside the crash record; the next
// start turns menu speech off once and tells the player. This is the part
// that needs no Windows: which module counts as a speech engine, and what the
// marker says.

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <string>

namespace sf4e {
namespace crash {

// The marker's file name, in the same folder as sf4e-crash.log.
constexpr const wchar_t* SpeechCrashMarkerFile = L"sf4e-speech-crash.marker";

namespace detail {
inline char LowerAscii(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
inline bool StartsWithNoCase(const char* text, const char* prefix) {
	for (; *prefix; ++text, ++prefix)
		if (LowerAscii(*text) != LowerAscii(*prefix)) return false;
	return true;
}
}

// Whether a module file name (no folder) is part of Windows' speech stack:
// sapi.dll, the API Ember calls, and MSTTSEngine*.dll, Microsoft's own voice
// engines (the field crash was in MSTTSEngine_OneCore.dll). A voice from
// another maker loads its own DLL under a name no list can hold, so a crash in
// one is not matched.
inline bool IsSpeechEngineModule(const char* name) {
	if (!name || !name[0]) return false;
	if (detail::StartsWithNoCase(name, "MSTTSEngine")) return true;
	return detail::StartsWithNoCase(name, "sapi.dll") && name[8] == '\0';
}

// The marker's text: a fixed first word and the module that faulted, so the
// next start can log it. Returns the length written, 0 when it did not fit.
inline size_t FormatSpeechCrashMarker(char* out, size_t size, const char* module) {
	const int printed = snprintf(out, size, "speech-crash module=%s\n", module ? module : "");
	return printed > 0 && static_cast<size_t>(printed) < size ? static_cast<size_t>(printed) : 0;
}

// Reads a marker back. False when the text is not one (an empty or foreign
// file is no reason to turn speech off); module gets the name it holds.
inline bool ParseSpeechCrashMarker(const char* text, size_t length, std::string* module) {
	static const char Head[] = "speech-crash module=";
	const size_t head = sizeof(Head) - 1;
	if (!text || length < head || memcmp(text, Head, head) != 0) return false;
	size_t end = head;
	while (end < length && text[end] != '\n' && text[end] != '\r' && text[end] != '\0') ++end;
	if (module) module->assign(text + head, end - head);
	return true;
}

} // namespace crash
} // namespace sf4e
