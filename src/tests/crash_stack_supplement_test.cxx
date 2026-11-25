// A frozen Proton game's dump held no live main stack: the stack pointer sat
// below the TEB's StackLimit, so DbgHelp stored the outer 1 MB only. These
// numbers are that dump's (esp 0xEBF1FC, limit 0xFC2000, base 0x10C0000).

#include "../common/CrashStackSupplement.hxx"
#include "test_support.hxx"

using namespace sf4e::crash;

static void BelowTheLimitTheLiveFramesAreAdded() {
	const auto ranges = PlanStackSupplement({ { 404, 0xEBF1FC, 0xFC2000, 0x10C0000 } });
	CHECK(ranges.size() == 1);
	CHECK(ranges[0].threadId == 404 && ranges[0].start == 0xEBF1F8);
	// The whole 1 MB gap to the limit, so it joins the stored stack.
	CHECK(ranges[0].start + ranges[0].size == 0xFC2000);
	// A gap shorter than the cap ends exactly at the limit, so the two meet.
	const auto near = PlanStackSupplement({ { 7, 0xFC1F00, 0xFC2000, 0x10C0000 } });
	CHECK(near.size() == 1 && near[0].start == 0xFC1EFC && near[0].start + near[0].size == 0xFC2000);
}

static void AStackDbgHelpKeepsAddsNothing() {
	// esp - 4 inside [limit, base]: DbgHelp stores from there.
	CHECK(PlanStackSupplement({ { 1, 0x2000000, 0x1FFC000, 0x2000000 + 0x4000 } }).empty());
	CHECK(PlanStackSupplement({ { 1, 0x1FFC004, 0x1FFC000, 0x2004000 } }).empty());
	// No stack pointer, an esp above the base, bounds that cannot be a stack.
	CHECK(PlanStackSupplement({ { 1, 0, 0x1000, 0x2000 } }).empty());
	CHECK(PlanStackSupplement({ { 1, 0x3000000, 0x1000000, 0x2000000 } }).empty());
	CHECK(PlanStackSupplement({ { 1, 0x1000, 0x3000, 0x3000 } }).empty());
	// A stack pointer further below the limit than any stack reserves.
	CHECK(PlanStackSupplement({ { 1, 0x100000, 0x10000000, 0x10100000 } }).empty());
}

static void AllThreadsTogetherStayBounded() {
	std::vector<ThreadStackBounds> threads;
	for (uint32_t i = 0; i < 6; ++i) threads.push_back({ 100 + i, 0x1000000 + i * 0x1000000, 0x1000000 + i * 0x1000000 + 0x1C0000, 0x1000000 + i * 0x1000000 + 0x200000 });
	uint32_t total = 0;
	for (const auto& range : PlanStackSupplement(threads)) total += range.size;
	CHECK(total == StackSupplementTotal);
}

static void TheStreamKeepsRangesAndBytes() {
	const auto stream = EncodeStackSupplement({ { 5, 0x1000, { 1, 2, 3 } }, { 6, 0x2000, { 4, 5 } } });
	CHECK(stream.size() == 8 + 2 * 12 + 5);
	CHECK(stream[0] == StackSupplementVersion && stream[4] == 2);
	CHECK(stream[8] == 5 && stream[12] == 0x00 && stream[13] == 0x10 && stream[16] == 3);
	CHECK(stream[32] == 1 && stream[36] == 5);
	CHECK(EncodeStackSupplement({}).empty());
}

int main() {
	BelowTheLimitTheLiveFramesAreAdded();
	AStackDbgHelpKeepsAddsNothing();
	AllThreadsTogetherStayBounded();
	TheStreamKeepsRangesAndBytes();
	std::puts("crash_stack_supplement_test: all tests passed");
	return 0;
}
