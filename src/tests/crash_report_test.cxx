// Pure unit tests for the crash record model (F-015).

#include "../common/CrashReport.hxx"

#include <string>
#include <vector>

#include "test_support.hxx"

using namespace sf4e::crash;

static std::vector<std::string> Lines(const LogRing<4, 16>& ring) {
	std::vector<std::string> lines;
	ring.ForEach([&](const char* line) { lines.push_back(line); });
	return lines;
}

static void TestRingKeepsTheLastLinesInOrder() {
	LogRing<4, 16> ring;
	CHECK(ring.Count() == 0);
	CHECK(Lines(ring).empty());

	ring.Push("one", 3);
	ring.Push("two", 3);
	CHECK(ring.Count() == 2);
	CHECK(Lines(ring) == (std::vector<std::string>{"one", "two"}));

	ring.Push("three", 5);
	ring.Push("four", 4);
	ring.Push("five", 4);
	ring.Push("six", 3);
	CHECK(ring.Count() == 4);
	CHECK(Lines(ring) == (std::vector<std::string>{"three", "four", "five", "six"}));
}

static void TestRingCutsLongLines() {
	LogRing<4, 16> ring;
	const char* line = "0123456789abcdefXYZ"; // longer than the slot
	ring.Push(line, strlen(line));
	CHECK(Lines(ring) == (std::vector<std::string>{"0123456789abcde"}));
	ring.Push("", 0);
	CHECK(Lines(ring).back().empty());
}

static void TestExitCodeNames() {
	CHECK(std::string(ExitCodeName(0)) == "clean exit");
	CHECK(std::string(ExitCodeName(1)).find("GGPO assertion") != std::string::npos);
	CHECK(std::string(ExitCodeName(3)) == "abort()");
	CHECK(std::string(ExitCodeName(0xC0000005u)) == "access violation");
	CHECK(std::string(ExitCodeName(0xC0000374u)) == "heap corruption");
	CHECK(std::string(ExitCodeName(0xC0000409u)).find("fail-fast") == 0);
	CHECK(std::string(ExitCodeName(0xC00000FDu)) == "stack overflow");
	CHECK(std::string(ExitCodeName(0xE06D7363u)) == "unhandled C++ exception");
	CHECK(std::string(ExitCodeName(0xC0000135u)).find("not found") != std::string::npos);
	CHECK(std::string(ExitCodeName(0xC0000139u)).find("entry point not found") == 0);
	CHECK(std::string(ExitCodeName(0xC0000139u)).find("GGPO.dll") != std::string::npos);
	CHECK(std::string(ExitCodeName(0x12345678u)) == "unknown");
}

static void TestHeaderNamesTheFault() {
	char out[256];
	CrashFacts facts = { "unhandled_exception", 0xC0000005u, 0x0052F3D0ull, "SSFIV.exe", 0x12F3D0ull, 4242u, "" };
	const size_t written = FormatCrashHeader(out, sizeof(out), facts);
	CHECK(written == strlen(out));
	CHECK(std::string(out) ==
		"kind=unhandled_exception code=0xC0000005 (access violation) address=0x0052F3D0 "
		"module=SSFIV.exe+0x12F3D0 thread=4242 message=\n");
}

static void TestHeaderToleratesMissingFacts() {
	char out[256];
	CrashFacts facts = { "ggpo_assertion", 0, 0, "", 0, 7u, "Assertion failed: frame == _last_saved_frame + 1" };
	FormatCrashHeader(out, sizeof(out), facts);
	CHECK(std::string(out).find("module=?+0x0") != std::string::npos);
	CHECK(std::string(out).find("message=Assertion failed: frame == _last_saved_frame + 1\n") != std::string::npos);

	CrashFacts empty = { nullptr, 0, 0, nullptr, 0, 0u, nullptr };
	FormatCrashHeader(out, sizeof(out), empty);
	CHECK(std::string(out).find("kind= code=0x00000000 (clean exit)") == 0);

	char small[24];
	const size_t cut = FormatCrashHeader(small, sizeof(small), facts);
	CHECK(cut == sizeof(small) - 1);
	CHECK(small[cut] == '\0');
	CHECK(FormatCrashHeader(nullptr, 0, facts) == 0);
}

static void TestAddressSpaceFold() {
	AddressSpaceSummary summary;
	CHECK(summary.largestFree == 0 && summary.totalFree == 0 && summary.freeRegions == 0);
	summary.AddFreeRegion(4096);
	summary.AddFreeRegion(1 << 20);
	summary.AddFreeRegion(65536);
	CHECK(summary.largestFree == (1u << 20));
	CHECK(summary.totalFree == 4096u + (1u << 20) + 65536u);
	CHECK(summary.freeRegions == 3);
}

int main() {
	TestRingKeepsTheLastLinesInOrder();
	TestRingCutsLongLines();
	TestExitCodeNames();
	TestHeaderNamesTheFault();
	TestHeaderToleratesMissingFacts();
	TestAddressSpaceFold();
	printf("crash_report_test: all tests passed\n");
	return 0;
}
