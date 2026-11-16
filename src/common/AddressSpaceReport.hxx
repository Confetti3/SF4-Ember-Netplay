#pragma once

// A summary of the 32-bit game's address space for a load that stalled: how
// much is free, reserved and committed, by kind of mapping; the largest free
// blocks (a texture or a save state needs one that size); and the largest
// allocations, which say what is using it. It takes the regions a
// VirtualQuery walk found (sf4e__CrashDiagnostics.cxx) and holds sizes,
// counts and file names only: no memory contents and no paths.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

// RegionState and RegionType are CrashReport.hxx's.
#include "CrashReport.hxx"

namespace sf4e {
namespace crash {

// A run of pages the walk found alike. allocationBase is the base of the
// allocation the region belongs to (0 for free space).
struct Region {
	std::uint64_t base = 0;
	std::uint64_t size = 0;
	RegionState state = RegionState::Free;
	RegionType type = RegionType::None;
	std::uint64_t allocationBase = 0;
};

// Free blocks at least this large can still take a big allocation.
constexpr std::uint64_t LargeFreeBlock = 16ull * 1024 * 1024;
constexpr std::size_t LargestFreeShown = 5;
constexpr std::size_t LargestAllocationsShown = 10;

struct Totals {
	std::uint64_t bytes = 0;
	std::uint32_t regions = 0;
	void Add(std::uint64_t size) { bytes += size; ++regions; }
};

struct Allocation {
	std::uint64_t base = 0;
	std::uint64_t size = 0;
	RegionType type = RegionType::Private;
	// The module's base file name, filled by the caller for an image allocation.
	std::string image;
};

struct AddressSpaceReport {
	Totals free, reserved, committed;
	// Reserved and committed space together, by kind of mapping.
	Totals image, mapped, privateMemory;
	std::vector<std::uint64_t> largestFree;
	std::uint32_t freeBlocksOver16MB = 0;
	std::vector<Allocation> largestAllocations;
};

// `regions` in address order, as VirtualQuery returns them.
inline AddressSpaceReport SummarizeAddressSpace(const std::vector<Region>& regions) {
	AddressSpaceReport report;
	std::vector<std::uint64_t> freeSizes;
	std::vector<Allocation> allocations;
	for (const Region& region : regions) {
		if (region.state == RegionState::Free) {
			report.free.Add(region.size);
			freeSizes.push_back(region.size);
			if (region.size >= LargeFreeBlock) ++report.freeBlocksOver16MB;
			continue;
		}
		(region.state == RegionState::Reserved ? report.reserved : report.committed).Add(region.size);
		switch (region.type) {
		case RegionType::Image: report.image.Add(region.size); break;
		case RegionType::Mapped: report.mapped.Add(region.size); break;
		default: report.privateMemory.Add(region.size); break;
		}
		// An allocation is the run of regions that share its base.
		if (allocations.empty() || allocations.back().base != region.allocationBase)
			allocations.push_back(Allocation{ region.allocationBase, 0, region.type, std::string() });
		allocations.back().size += region.size;
	}
	const auto largerFirst = [](std::uint64_t a, std::uint64_t b) { return a > b; };
	const std::size_t freeShown = (std::min)(LargestFreeShown, freeSizes.size());
	std::partial_sort(freeSizes.begin(), freeSizes.begin() + freeShown, freeSizes.end(), largerFirst);
	report.largestFree.assign(freeSizes.begin(), freeSizes.begin() + freeShown);
	const std::size_t shown = (std::min)(LargestAllocationsShown, allocations.size());
	std::partial_sort(allocations.begin(), allocations.begin() + shown, allocations.end(),
		[](const Allocation& a, const Allocation& b) { return a.size != b.size ? a.size > b.size : a.base < b.base; });
	allocations.resize(shown);
	report.largestAllocations = std::move(allocations);
	return report;
}

inline std::string Megabytes(std::uint64_t bytes) {
	char text[32];
	std::snprintf(text, sizeof(text), "%.1fMB", static_cast<double>(bytes) / (1024.0 * 1024.0));
	return text;
}

inline const char* RegionTypeName(RegionType type) {
	switch (type) {
	case RegionType::Image: return "image";
	case RegionType::Mapped: return "mapped";
	default: return "private";
	}
}

// Three log lines: totals, the free blocks, the largest allocations.
inline std::vector<std::string> FormatAddressSpace(const AddressSpaceReport& report) {
	const auto total = [](const char* name, const Totals& totals) {
		return std::string(name) + "=" + Megabytes(totals.bytes) + "/" + std::to_string(totals.regions);
	};
	std::vector<std::string> lines;
	lines.push_back("Address space: " + total("free", report.free) + " " + total("reserved", report.reserved) + " " +
		total("committed", report.committed) + " | " + total("image", report.image) + " " + total("mapped", report.mapped) + " " +
		total("private", report.privateMemory) + " (size/regions)");
	std::string blocks;
	for (const std::uint64_t size : report.largestFree) blocks += (blocks.empty() ? "" : " ") + Megabytes(size);
	lines.push_back("Address space: largest free blocks " + (blocks.empty() ? std::string("none") : blocks) +
		"; free blocks of 16MB or more: " + std::to_string(report.freeBlocksOver16MB));
	std::string allocations;
	for (const Allocation& allocation : report.largestAllocations) {
		allocations += (allocations.empty() ? "" : "; ") + std::string(RegionTypeName(allocation.type)) + " " + Megabytes(allocation.size);
		if (!allocation.image.empty()) allocations += " " + allocation.image;
	}
	lines.push_back("Address space: largest allocations " + (allocations.empty() ? std::string("none") : allocations));
	return lines;
}

} // namespace crash
} // namespace sf4e
