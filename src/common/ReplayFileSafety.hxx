#pragma once

#include "ReplaySlots.hxx"

#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

namespace sf4e { namespace replayfiles {

struct Change {
	std::string name;
	replayslots::Bytes before, after;
	bool existed = false;
};

// Missing files are distinct from empty files; an unreadable file cannot be replaced.
inline bool Snapshot(const std::filesystem::path& path, std::size_t most, Change& change) {
	std::error_code error;
	const auto status = std::filesystem::status(path, error);
	if (status.type() == std::filesystem::file_type::not_found &&
		(!error || error == std::errc::no_such_file_or_directory)) { change.existed = false; change.before.clear(); return true; }
	if (error || !std::filesystem::is_regular_file(status)) return false;
	std::ifstream file(path, std::ios::binary);
	if (!file) return false;
	replayslots::Bytes bytes(most + 1);
	file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	if (file.bad() || (!file.eof() && file.fail()) || static_cast<std::size_t>(file.gcount()) > most) return false;
	bytes.resize(static_cast<std::size_t>(file.gcount()));
	change.before = std::move(bytes); change.existed = true;
	return true;
}

// Only a whole regular replay file proves the archive holds its body CRC.
inline bool ArchivedCrc(const std::filesystem::path& path, std::uint32_t& crc) {
	std::uint32_t named = 0;
	const bool wrapped = replayslots::ArchiveNameCrc(path.filename().wstring(), named);
	if (!wrapped && path.extension() != L".usf4replay") return false;
	Change file;
	if (!Snapshot(path, replayslots::kLargestReplay + replayslots::kExportHeaderBytes, file) || !file.existed) return false;
	const auto& bytes = file.before;
	if (wrapped) {
		if (bytes.size() < replayslots::kExportHeaderBytes || std::memcmp(bytes.data(), replayslots::kExportMagic, 8)) return false;
		const auto* body = bytes.data() + replayslots::kExportHeaderBytes;
		const auto size = bytes.size() - replayslots::kExportHeaderBytes;
		if (!replayslots::Describes(bytes.data() + 8, body, size)) return false;
		crc = replayslots::Crc32(body, size);
		return crc == named;
	}
	replayslots::ReplayHeaderInfo header;
	if (!replayslots::ReadReplayHeader(bytes, header)) return false;
	crc = replayslots::Crc32(bytes.data(), bytes.size());
	return true;
}

// Keep all snapshots until the live table accepts the files. A failed write
// may have changed its file too, so every attempted write is undone.
template<class Writer, class Remover, class Publish>
bool Apply(const std::vector<Change>& changes, const Writer& write, const Remover& remove, const Publish& publish, bool& restored) noexcept {
	std::size_t attempted = 0;
	restored = true;
	try {
		bool written = true;
		for (const auto& change : changes) {
			++attempted;
			if (!write(change.name, change.after)) { written = false; break; }
		}
		if (written && publish()) return true;
	} catch (...) {}
	while (attempted) {
		const auto& change = changes[--attempted];
		try {
			const bool ok = change.existed ? write(change.name, change.before) : remove(change.name);
			if (!ok) restored = false;
		} catch (...) { restored = false; }
	}
	return false;
}

} }
