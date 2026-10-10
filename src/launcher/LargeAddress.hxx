#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace sf4e { namespace launcher {

// SSFIV.exe is a 32-bit program built without IMAGE_FILE_LARGE_ADDRESS_AWARE,
// so Windows gives it 2 GB of address space. With the flag a 32-bit process
// gets 4 GB on 64-bit Windows. Windows reads the flag from the file when it
// creates the process, so it cannot be set from Sidecar. The launcher instead
// starts a copy of the executable that has the flag, beside the original,
// which stays as Steam installed it.
inline constexpr wchar_t kLargeAddressExecutableName[] = L"SSFIV.ember-laa.exe";

enum class LargeAddress { Set, AlreadySet, NotPe32 };

// Sets IMAGE_FILE_LARGE_ADDRESS_AWARE (0x0020) in the COFF header of a 32-bit
// x86 executable image held in memory. Anything else is left untouched. The
// optional-header checksum is not updated: Windows checks it for drivers and
// boot-time DLLs only.
inline LargeAddress MarkLargeAddressAware(std::vector<unsigned char>& image) {
	const auto u16 = [&](std::size_t o) { return static_cast<unsigned>(image[o] | image[o + 1] << 8); };
	if (image.size() < 0x40 || image[0] != 'M' || image[1] != 'Z') return LargeAddress::NotPe32;
	const std::size_t pe = image[0x3c] | image[0x3d] << 8 | image[0x3e] << 16 | static_cast<std::size_t>(image[0x3f]) << 24;
	// Signature (4), COFF header (20), optional header magic (2).
	if (pe > image.size() || image.size() - pe < 26) return LargeAddress::NotPe32;
	if (image[pe] != 'P' || image[pe + 1] != 'E' || image[pe + 2] != 0 || image[pe + 3] != 0) return LargeAddress::NotPe32;
	if (u16(pe + 4) != 0x014c || u16(pe + 24) != 0x010b) return LargeAddress::NotPe32;
	const std::size_t characteristics = pe + 22;
	if (image[characteristics] & 0x20) return LargeAddress::AlreadySet;
	image[characteristics] |= 0x20;
	return LargeAddress::Set;
}

// The executable to start for a 4 GB address space: a flagged copy of
// `executable` in `directory`, written or refreshed when it is missing or no
// longer matches the original (a game update). `executable` itself when it
// already has the flag, is not a 32-bit x86 image, or the copy cannot be
// written.
inline std::wstring LargeAddressExecutable(const std::wstring& directory, const std::wstring& executable) {
	const auto read = [](const std::filesystem::path& path, std::vector<unsigned char>& bytes) {
		std::ifstream in(path, std::ios::binary);
		if (!in) return false;
		bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		return !in.bad();
	};
	std::vector<unsigned char> image, existing;
	if (!read(executable, image) || MarkLargeAddressAware(image) != LargeAddress::Set) return executable;
	const auto copy = std::filesystem::path(directory) / kLargeAddressExecutableName;
	if (read(copy, existing) && existing == image) return copy.wstring();
	// Written beside the target and renamed over it, so a failed write never
	// leaves a half-written executable under the name the launcher starts.
	auto partial = copy;
	partial += L".part";
	std::error_code error;
	{
		std::ofstream out(partial, std::ios::binary | std::ios::trunc);
		out.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
		out.close();
		if (!out) { std::filesystem::remove(partial, error); return executable; }
	}
	std::filesystem::rename(partial, copy, error);
	if (error) { std::filesystem::remove(partial, error); return executable; }
	return copy.wstring();
}

} }
