// Pure unit tests for the address space summary of a stalled load.

#include "../common/AddressSpaceReport.hxx"

#include <string>
#include <vector>

#include "test_support.hxx"

using namespace sf4e::crash;

constexpr std::uint64_t MB = 1024ull * 1024;

static Region Free(std::uint64_t base, std::uint64_t size) { return Region{ base, size, RegionState::Free, RegionType::None, 0 }; }
static Region Used(std::uint64_t base, std::uint64_t size, RegionState state, RegionType type, std::uint64_t allocation) {
	return Region{ base, size, state, type, allocation };
}

static void TestEmptyWalk() {
	const AddressSpaceReport report = SummarizeAddressSpace({});
	CHECK(report.free.bytes == 0 && report.free.regions == 0);
	CHECK(report.largestFree.empty() && report.largestAllocations.empty());
	CHECK(report.freeBlocksOver16MB == 0);
	const auto lines = FormatAddressSpace(report);
	CHECK(lines.size() == 3);
	CHECK(lines[1].find("none") != std::string::npos && lines[2].find("none") != std::string::npos);
}

static void TestTotalsByStateAndType() {
	const std::vector<Region> regions = {
		Free(0x1000, 4 * MB),
		Used(0x400000, 2 * MB, RegionState::Committed, RegionType::Image, 0x400000),
		Used(0x600000, 1 * MB, RegionState::Reserved, RegionType::Private, 0x600000),
		Used(0x700000, 3 * MB, RegionState::Committed, RegionType::Mapped, 0x700000),
		Used(0xA00000, 5 * MB, RegionState::Committed, RegionType::Private, 0xA00000),
	};
	const AddressSpaceReport report = SummarizeAddressSpace(regions);
	CHECK(report.free.bytes == 4 * MB && report.free.regions == 1);
	CHECK(report.reserved.bytes == 1 * MB && report.reserved.regions == 1);
	CHECK(report.committed.bytes == 10 * MB && report.committed.regions == 3);
	CHECK(report.image.bytes == 2 * MB && report.mapped.bytes == 3 * MB);
	CHECK(report.privateMemory.bytes == 6 * MB && report.privateMemory.regions == 2);
}

static void TestFreeBlocks() {
	std::vector<Region> regions;
	const std::uint64_t sizes[] = { 1 * MB, 40 * MB, 16 * MB, 15 * MB, 300 * MB, 2 * MB, 16 * MB, 64 * MB };
	std::uint64_t base = 0x10000;
	for (const std::uint64_t size : sizes) {
		regions.push_back(Free(base, size));
		base += size;
		regions.push_back(Used(base, 0x10000, RegionState::Committed, RegionType::Private, base));
		base += 0x10000;
	}
	const AddressSpaceReport report = SummarizeAddressSpace(regions);
	CHECK(report.largestFree == (std::vector<std::uint64_t>{ 300 * MB, 64 * MB, 40 * MB, 16 * MB, 16 * MB }));
	CHECK(report.freeBlocksOver16MB == 5);
	CHECK(report.free.regions == 8);

	const AddressSpaceReport few = SummarizeAddressSpace({ Free(0, 3 * MB), Free(0x1000000, 1 * MB) });
	CHECK(few.largestFree == (std::vector<std::uint64_t>{ 3 * MB, 1 * MB }));
	CHECK(few.freeBlocksOver16MB == 0);
}

static void TestAllocationsGroupRegionsByBase() {
	// One 6 MB allocation of three regions, among smaller ones.
	const std::vector<Region> regions = {
		Used(0x100000, 1 * MB, RegionState::Committed, RegionType::Private, 0x100000),
		Used(0x200000, 1 * MB, RegionState::Committed, RegionType::Private, 0x200000),
		Used(0x300000, 2 * MB, RegionState::Reserved, RegionType::Private, 0x200000),
		Used(0x500000, 3 * MB, RegionState::Committed, RegionType::Private, 0x200000),
		Free(0x800000, 8 * MB),
		Used(0x1000000, 5 * MB, RegionState::Committed, RegionType::Image, 0x1000000),
	};
	const AddressSpaceReport report = SummarizeAddressSpace(regions);
	CHECK(report.largestAllocations.size() == 3);
	CHECK(report.largestAllocations[0].base == 0x200000 && report.largestAllocations[0].size == 6 * MB);
	CHECK(report.largestAllocations[1].base == 0x1000000 && report.largestAllocations[1].type == RegionType::Image);
	CHECK(report.largestAllocations[2].base == 0x100000);
}

static void TestTenLargestAllocationsOnly() {
	std::vector<Region> regions;
	for (std::uint64_t i = 1; i <= 14; ++i)
		regions.push_back(Used(i * 0x10000000, i * MB, RegionState::Committed, RegionType::Private, i * 0x10000000));
	const AddressSpaceReport report = SummarizeAddressSpace(regions);
	CHECK(report.largestAllocations.size() == LargestAllocationsShown);
	CHECK(report.largestAllocations.front().size == 14 * MB);
	CHECK(report.largestAllocations.back().size == 5 * MB);
}

static void TestFormatCarriesNamesAndSizesOnly() {
	AddressSpaceReport report = SummarizeAddressSpace({
		Free(0x1000, 20 * MB),
		Used(0x400000, 2 * MB, RegionState::Committed, RegionType::Image, 0x400000),
	});
	report.largestAllocations[0].image = "SSFIV.exe";
	const auto lines = FormatAddressSpace(report);
	CHECK(lines.size() == 3);
	CHECK(lines[0] == "Address space: free=20.0MB/1 reserved=0.0MB/0 committed=2.0MB/1 | image=2.0MB/1 mapped=0.0MB/0 private=0.0MB/0 (size/regions)");
	CHECK(lines[1] == "Address space: largest free blocks 20.0MB; free blocks of 16MB or more: 1");
	CHECK(lines[2] == "Address space: largest allocations image 2.0MB SSFIV.exe");
}

int main() {
	TestEmptyWalk();
	TestTotalsByStateAndType();
	TestFreeBlocks();
	TestAllocationsGroupRegionsByBase();
	TestTenLargestAllocationsOnly();
	TestFormatCarriesNamesAndSizesOnly();
	return 0;
}
