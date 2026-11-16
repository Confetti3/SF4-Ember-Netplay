#pragma once

// What a fault in unmapped code leaves to name its cause: the modules ntdll
// last unloaded, and a table of image ranges (the loaded ones the host knows
// and the unloaded ones) that addresses are annotated against by comparing
// numbers only, so the crash path makes no loader call for them. Nothing here
// allocates or takes a lock.
//
// Pure component: no game or platform dependencies, unit tested.

#include "CrashReport.hxx"

namespace sf4e {
namespace crash {

// ntdll keeps the last modules it unloaded in a circular array, which names
// the image a thread was still running in when its code vanished. This is
// RTL_UNLOAD_EVENT_TRACE as a 32-bit process has it; the SDK headers lack it.
struct UnloadEventTrace32 {
	uint32_t baseAddress;
	uint32_t sizeOfImage;
	uint32_t sequence;
	uint32_t timeDateStamp;
	uint32_t checkSum;
	uint16_t imageName[32];
	uint32_t version[2];
};
static_assert(offsetof(UnloadEventTrace32, imageName) == 20 && sizeof(UnloadEventTrace32) == 92, "RTL_UNLOAD_EVENT_TRACE layout");

constexpr size_t UnloadEntryNeeded = offsetof(UnloadEventTrace32, imageName) + sizeof(uint16_t) * 32;
constexpr uint32_t UnloadTraceMaxElements = 64;   // RTL_UNLOAD_EVENT_TRACE_NUMBER
constexpr uint32_t UnloadTraceMaxElementSize = 256;
constexpr size_t UnloadTraceMaxBytes = (size_t)UnloadTraceMaxElements * UnloadTraceMaxElementSize;
constexpr unsigned UnloadedKept = 8;

struct UnloadedImage {
	uint64_t base = 0;
	uint64_t size = 0;
	uint32_t sequence = 0;
	char name[33] = {};
};

// The newest unloads, newest first.
struct UnloadedImages {
	UnloadedImage items[UnloadedKept];
	unsigned count = 0;
};

// The bytes of an array ntdll reports as `elementCount` elements of
// `elementSize`, or 0 when the report cannot be right. Entries are stepped by
// that size, which may exceed the struct above.
inline size_t UnloadTraceBytes(uint32_t elementSize, uint32_t elementCount) {
	if (elementSize < UnloadEntryNeeded || elementSize > UnloadTraceMaxElementSize) return 0;
	if (!elementCount || elementCount > UnloadTraceMaxElements) return 0;
	return (size_t)elementSize * elementCount;
}

// The newest entries of a copy of ntdll's array. An empty slot, and an entry
// that does not look like an image (no 64 KB aligned base, no size, no
// printable name), are skipped, so memory that is not the array parses to
// nothing. Names are narrowed; a character outside ASCII becomes '?'.
inline UnloadedImages ParseUnloadTrace(const uint8_t* array, uint32_t elementSize, uint32_t elementCount) {
	UnloadedImages found;
	if (!array || !UnloadTraceBytes(elementSize, elementCount)) return found;
	for (uint32_t i = 0; i < elementCount; ++i) {
		UnloadEventTrace32 entry;
		memset(&entry, 0, sizeof(entry));
		memcpy(&entry, array + (size_t)i * elementSize, sizeof(entry) < elementSize ? sizeof(entry) : elementSize);
		if (!entry.baseAddress || (entry.baseAddress & 0xFFFFu) || !entry.sizeOfImage || entry.sizeOfImage >= 0x40000000u) continue;
		if (entry.imageName[0] < 0x20 || entry.imageName[0] > 0x7E) continue;
		UnloadedImage image;
		image.base = entry.baseAddress;
		image.size = entry.sizeOfImage;
		image.sequence = entry.sequence;
		for (size_t c = 0; c < 32 && entry.imageName[c]; ++c)
			image.name[c] = entry.imageName[c] >= 0x20 && entry.imageName[c] <= 0x7E ? (char)entry.imageName[c] : '?';
		unsigned at = 0;
		while (at < found.count && found.items[at].sequence >= image.sequence) ++at;
		if (at >= UnloadedKept) continue;
		for (unsigned j = found.count < UnloadedKept ? found.count : UnloadedKept - 1; j > at; --j) found.items[j] = found.items[j - 1];
		found.items[at] = image;
		if (found.count < UnloadedKept) ++found.count;
	}
	return found;
}

// The newest unloaded image whose range holds `address`, or nullptr.
inline const UnloadedImage* FindUnloaded(const UnloadedImages& images, uint64_t address) {
	for (unsigned i = 0; i < images.count; ++i) {
		const UnloadedImage& image = images.items[i];
		if (address >= image.base && address - image.base < image.size) return &image;
	}
	return nullptr;
}

// "unloaded modules: name@0xBase+0xSize ...", newest first, as one terminated
// line; nothing when the trace holds none.
inline size_t FormatUnloadedList(char* out, size_t size, const UnloadedImages& images) {
	if (!out || !size) return 0;
	out[0] = '\0';
	if (!images.count) return 0;
	size_t length = 0;
	if (!Advance(out, size, length, snprintf(out, size, "unloaded modules:"))) return 0;
	for (unsigned i = 0; i < images.count; ++i) {
		const UnloadedImage& image = images.items[i];
		if (!Advance(out, size, length, snprintf(out + length, size - length, " %s@0x%08llX+0x%llX", image.name,
			(unsigned long long)image.base, (unsigned long long)image.size))) break;
	}
	if (length + 1 >= size) { out[0] = '\0'; return 0; }
	out[length++] = '\n';
	out[length] = '\0';
	return length;
}

// "unloaded module: name+0xOffset" when the faulting address is inside an
// unloaded image. For each of ebx, esi and edi inside one, "esi is in the
// former range of name+0xOffset": the heap often reuses an unloaded image's
// range, so a register there is a lead, not a finding. A line each; nothing
// when none does.
inline size_t FormatUnloadedHits(char* out, size_t size, const UnloadedImages& images, uint64_t faultAddress, const CrashRegisters& r) {
	if (!out || !size) return 0;
	out[0] = '\0';
	size_t length = 0;
	if (const UnloadedImage* image = FindUnloaded(images, faultAddress)) {
		if (!Advance(out, size, length, snprintf(out, size, "unloaded module: %s+0x%llX\n", image->name,
			(unsigned long long)(faultAddress - image->base)))) return 0;
	}
	struct Probe { const char* name; uint64_t address; };
	const Probe probes[] = { { "ebx", r.ebx }, { "esi", r.esi }, { "edi", r.edi } };
	for (const Probe& probe : probes) {
		const UnloadedImage* image = FindUnloaded(images, probe.address);
		if (!image) continue;
		if (!Advance(out, size, length, snprintf(out + length, size - length, "%s is in the former range of %s+0x%llX\n", probe.name,
			image->name, (unsigned long long)(probe.address - image->base)))) break;
	}
	return length;
}

// One image's address range in the table.
struct ImageRange {
	uint64_t base = 0;
	uint64_t size = 0;
	bool unloaded = false;
	char name[33] = {};
};

// The loaded images the host knows without asking the loader, then the
// unloaded ones: a loaded image wins where they overlap.
class ImageTable {
public:
	static const unsigned Capacity = 2 + UnloadedKept;

	// False when the table is full or the range is empty.
	bool Add(const char* name, uint64_t base, uint64_t size, bool unloaded) {
		if (count_ >= Capacity || !base || !size) return false;
		ImageRange& range = items_[count_++];
		range.base = base;
		range.size = size;
		range.unloaded = unloaded;
		snprintf(range.name, sizeof(range.name), "%s", name ? name : "");
		return true;
	}
	void AddUnloaded(const UnloadedImages& images) {
		for (unsigned i = 0; i < images.count; ++i) Add(images.items[i].name, images.items[i].base, images.items[i].size, true);
	}
	// The first range holding `address`, or nullptr.
	const ImageRange* Find(uint64_t address) const {
		for (unsigned i = 0; i < count_; ++i)
			if (address >= items_[i].base && address - items_[i].base < items_[i].size) return &items_[i];
		return nullptr;
	}
private:
	ImageRange items_[Capacity];
	unsigned count_ = 0;
};

// "name+0xOffset", with " (unloaded)" for an image that is gone, as one
// string without a newline.
inline size_t FormatImageOffset(char* out, size_t size, const char* name, uint64_t offset, bool unloaded) {
	if (!out || !size) return 0;
	out[0] = '\0';
	size_t length = 0;
	Advance(out, size, length, snprintf(out, size, "%s+0x%llX%s", name && name[0] ? name : "?", (unsigned long long)offset, unloaded ? " (unloaded)" : ""));
	return length;
}

// What the table calls `address`: its image and offset, or nothing.
inline size_t DescribeImage(char* out, size_t size, uint64_t address, const ImageTable& table) {
	if (out && size) out[0] = '\0';
	const ImageRange* range = table.Find(address);
	return range ? FormatImageOffset(out, size, range->name, address - range->base, range->unloaded) : 0;
}

// The raw stack words from esp that point into an image of the table, one
// line each ("stack+0x04: 0x69FFA5E0 name+0x5E0 (unloaded)") under a line that
// counts the words and the ones left out. Words that point nowhere in the
// table are not printed: a stack holds paths, names and other private data.
inline size_t FormatStackWords(char* out, size_t size, uint32_t esp, const uint32_t* words, size_t count, const ImageTable& table) {
	if (!out || !size) return 0;
	out[0] = '\0';
	if (!words || !count) return 0;
	unsigned inImages = 0;
	for (size_t i = 0; i < count; ++i) if (table.Find(words[i])) ++inImages;
	size_t length = 0;
	if (!Advance(out, size, length, snprintf(out, size, "stack from esp=0x%08X: %u words, %u in images, %u left out\n",
		esp, (unsigned)count, inImages, (unsigned)count - inImages))) return 0;
	for (size_t i = 0; i < count; ++i) {
		if (!table.Find(words[i])) continue;
		char label[16], description[96];
		snprintf(label, sizeof(label), "stack+0x%02X", (unsigned)(i * 4));
		DescribeImage(description, sizeof(description), words[i], table);
		const size_t line = FormatLocatedAddress(out + length, size - length, label, words[i], description);
		if (!line) break;
		length += line;
	}
	return length;
}

} // namespace crash
} // namespace sf4e
