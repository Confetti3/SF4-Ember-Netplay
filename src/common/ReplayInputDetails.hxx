#pragma once

#include "ReplayInputs.hxx"

#include <filesystem>
#include <fstream>

namespace sf4e { namespace replayinputs {

// Reading, parsing and formatting are one failure boundary. No partial
// detail should be published when this returns false.
template<class Format>
bool ReadDetail(const std::string& path, const Format& format) noexcept {
	try {
		std::vector<std::uint8_t> bytes(replayslots::kLargestReplay + replayslots::kExportHeaderBytes + 1);
		std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
		if (!file) return false;
		file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		if (file.bad() || (!file.eof() && file.fail()) || static_cast<std::size_t>(file.gcount()) == bytes.size()) return false;
		bytes.resize(static_cast<std::size_t>(file.gcount()));
		Match match;
		if (!Parse(bytes.data(), bytes.size(), match)) return false;
		format(match);
		return true;
	} catch (...) { return false; }
}

} }
