#pragma once

#include <windows.h>
#include <objbase.h>
#include <string>

namespace sf4e { namespace platform { namespace videolink {

// An exclusive reservation beside the final video, with an extension no
// replay export uses. The encoder selects its container explicitly.
inline bool ReserveTemporary(const std::wstring& final, std::wstring& temporary) {
	GUID id = {};
	wchar_t text[40] = {};
	if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, text, 40)) return false;
	temporary = final + L"." + text + L".sf4e-video-tmp";
	if (temporary.size() >= 1024) return false;
	const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	CloseHandle(file);
	return true;
}

} } }
