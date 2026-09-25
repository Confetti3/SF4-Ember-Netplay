#pragma once

// Pure parts of the rollback stress audit (SF4E_ROLLBACK_STRESS_AUDIT). The
// harness snapshots engine objects after each stress save and compares them
// after the matching load; anything a load does not put back shows up here.
// No engine types: the harness supplies raw bytes and a node walk, so the
// comparators are tested on synthetic memory (RollbackAuditTest).

#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace sf4e { namespace audit {

// A half-open byte range [begin, end) of an object.
struct Range {
	size_t begin;
	size_t end;
};

// Offsets (within the snapshot) where two equally sized snapshots differ,
// restricted to `ranges`. Differences closer than a dword are merged.
inline std::vector<Range> Diff(const std::vector<uint8_t>& saved, const std::vector<uint8_t>& live,
	const std::vector<Range>& ranges) {
	std::vector<Range> out;
	if (saved.size() != live.size()) {
		out.push_back({ 0, saved.size() > live.size() ? saved.size() : live.size() });
		return out;
	}
	for (const Range& range : ranges) {
		const size_t end = range.end < saved.size() ? range.end : saved.size();
		for (size_t i = range.begin; i < end;) {
			if (saved[i] == live[i]) { i++; continue; }
			size_t last = i;
			for (size_t j = i + 1; j < end && j < last + 4; j++) {
				if (saved[j] != live[j]) last = j;
			}
			if (!out.empty() && out.back().end + 4 > i && out.back().end <= i) {
				out.back().end = last + 1;
			} else {
				out.push_back({ i, last + 1 });
			}
			i = last + 1;
		}
	}
	return out;
}

inline bool Contains(const std::vector<Range>& diff, size_t offset) {
	for (const Range& range : diff) {
		if (offset >= range.begin && offset < range.end) return true;
	}
	return false;
}

// " 0x148+9 0x6e5e+6" for at most `limit` ranges.
inline std::string Describe(const std::vector<Range>& diff, size_t limit = 24) {
	std::string out;
	char buf[32];
	for (size_t i = 0; i < diff.size() && i < limit; i++) {
		std::snprintf(buf, sizeof(buf), " %#zx+%zu", diff[i].begin, diff[i].end - diff[i].begin);
		out += buf;
	}
	return out;
}

// The collision-box fields Action::Actor's memento saves for each node
// (IDA 0x52B7C0): bytes 0..151, +172, +176, and the 0x60 bytes at *(node+160).
constexpr size_t kBoxNodeFieldBytes = 152;
constexpr size_t kBoxNodeCountOffset = 172;
constexpr size_t kBoxNodeFlagsOffset = 176;
constexpr size_t kBoxNodeDataBytes = 0x60;
constexpr size_t kBoxNodeSerializedBytes = kBoxNodeFieldBytes + 4 + 4 + kBoxNodeDataBytes;
// The engine's pools are far smaller; the cap only stops a corrupt chain.
constexpr int kMaxBoxNodes = 256;

inline void AppendBoxNode(std::vector<uint8_t>& out, const uint8_t* node, const uint8_t* data) {
	out.insert(out.end(), node, node + kBoxNodeFieldBytes);
	out.insert(out.end(), node + kBoxNodeCountOffset, node + kBoxNodeCountOffset + 4);
	out.insert(out.end(), node + kBoxNodeFlagsOffset, node + kBoxNodeFlagsOffset + 4);
	if (data) {
		out.insert(out.end(), data, data + kBoxNodeDataBytes);
	} else {
		out.insert(out.end(), kBoxNodeDataBytes, uint8_t(0));
	}
}

// Serializes one list as [uint32 node count][node fields...]. `next(node)`
// and `data(node)` read the engine's pointers, so the walk works on 32-bit
// game memory and on test structures alike. Returns the node count.
template <class Next, class Data>
int SerializeBoxList(std::vector<uint8_t>& out, const uint8_t* first, Next next, Data data) {
	const size_t countAt = out.size();
	out.insert(out.end(), 4, uint8_t(0));
	uint32_t count = 0;
	for (const uint8_t* node = first; node && count < kMaxBoxNodes; node = next(node)) {
		AppendBoxNode(out, node, data(node));
		count++;
	}
	std::memcpy(&out[countAt], &count, sizeof(count));
	return int(count);
}

// Per-battle accounting for one category.
struct Counts {
	uint32_t checked = 0;
	uint32_t missing = 0;
	uint32_t failed = 0;
	bool Passed() const { return checked > 0 && missing == 0 && failed == 0; }
};

enum class Probe { Pending, Detected, Missed };

inline const char* ProbeName(Probe probe) {
	switch (probe) {
	case Probe::Detected: return "detected";
	case Probe::Missed: return "missed";
	default: return "pending";
	}
}

struct Tally {
	Counts actor;
	Counts afterimage;
	Counts engine;
	Counts boxes;
	uint32_t boxNodesChecked = 0;
	Probe probeEngine = Probe::Pending;
	Probe probeBoxes = Probe::Pending;

	std::string Summary() const {
		char buf[512];
		std::snprintf(buf, sizeof(buf),
			"audit afterimage=%u/%u/%u engine=%u/%u/%u boxes=%u/%u/%u boxes_nodes_checked=%u actor=%u/%u/%u "
			"probe_engine=%s probe_boxes=%s (checked/missing/failed)",
			afterimage.checked, afterimage.missing, afterimage.failed,
			engine.checked, engine.missing, engine.failed,
			boxes.checked, boxes.missing, boxes.failed, boxNodesChecked,
			actor.checked, actor.missing, actor.failed,
			ProbeName(probeEngine), ProbeName(probeBoxes));
		return buf;
	}
};

} }
