// Pure unit tests for the crash record model (F-015).

#include "../common/CrashReport.hxx"
#include "../common/CrashUnloaded.hxx"
#include "../common/SpeechCrash.hxx"

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

static void TestSpeechEngineModules() {
	CHECK(IsSpeechEngineModule("sapi.dll"));
	CHECK(IsSpeechEngineModule("SAPI.DLL"));
	CHECK(IsSpeechEngineModule("MSTTSEngine_OneCore.dll"));
	CHECK(IsSpeechEngineModule("msttsengine.dll"));
	CHECK(!IsSpeechEngineModule("sapi.dll.bak"));
	CHECK(!IsSpeechEngineModule("SSFIV.exe"));
	CHECK(!IsSpeechEngineModule("Sidecar.dll"));
	CHECK(!IsSpeechEngineModule("ntdll.dll"));
	CHECK(!IsSpeechEngineModule(""));
	CHECK(!IsSpeechEngineModule(nullptr));
}

static void TestSpeechCrashMarker() {
	char text[160];
	const size_t length = FormatSpeechCrashMarker(text, sizeof(text), "MSTTSEngine_OneCore.dll");
	CHECK(length > 0);
	std::string module;
	CHECK(ParseSpeechCrashMarker(text, length, &module));
	CHECK(module == "MSTTSEngine_OneCore.dll");
	// Too small a buffer writes nothing rather than half a marker.
	char small[8];
	CHECK(FormatSpeechCrashMarker(small, sizeof(small), "MSTTSEngine_OneCore.dll") == 0);
	// An empty or foreign file is not a marker, so it never turns speech off.
	const auto parses = [&](const char* line) { return ParseSpeechCrashMarker(line, strlen(line), &module); };
	CHECK(!parses(""));
	CHECK(!parses("kind=unhandled_exception\n"));
	CHECK(!parses("speech-crash"));
	// A marker with no module still reads, and one with trailing lines keeps the first.
	CHECK(parses("speech-crash module=\n") && module.empty());
	CHECK(parses("speech-crash module=sapi.dll\r\nmore\n") && module == "sapi.dll");
	CHECK(ParseSpeechCrashMarker(text, length, nullptr));
}

static void TestExitCodeNames() {
	CHECK(std::string(ExitCodeName(0)) == "clean exit");
	CHECK(std::string(ExitCodeName(1)).find("GGPO assertion") != std::string::npos);
	CHECK(std::string(ExitCodeName(3)) == "abort()");
	CHECK(std::string(ExitCodeName(0xC0000005u)) == "access violation");
	CHECK(std::string(ExitCodeName(0xC0000006u)).find("in-page error") == 0);
	CHECK(std::string(ExitCodeName(0xC0000006u)).find("file the game had open") != std::string::npos);
	CHECK(IsCrashExit(0xC0000006u));
	CHECK(std::string(ExitCodeName(0xC0000374u)) == "heap corruption");
	CHECK(std::string(ExitCodeName(0xC0000409u)).find("fail-fast") == 0);
	CHECK(std::string(ExitCodeName(0xC00000FDu)) == "stack overflow");
	CHECK(std::string(ExitCodeName(0xE06D7363u)) == "unhandled C++ exception");
	CHECK(std::string(ExitCodeName(0xC0000135u)).find("DLL not found") == 0);
	CHECK(std::string(ExitCodeName(0xC0000139u)).find("entry point not found") == 0);
	CHECK(std::string(ExitCodeName(0xC0000139u)).find("GGPO.dll") != std::string::npos);
	// Smart App Control or another code integrity policy: named, and not a crash to report.
	CHECK(std::string(ExitCodeName(0xC0E90002u)).find("code integrity") != std::string::npos);
	CHECK(!IsCrashExit(0xC0E90002u));
	// A frozen game the player closed: named, and still reported.
	CHECK(std::string(ExitCodeName(0xCFFFFFFFu)).find("Not Responding") != std::string::npos);
	CHECK(IsCrashExit(0xCFFFFFFFu));
	CHECK(std::string(ExitCodeName(0x12345678u)) == "unknown");
	CHECK(IsCrashExit(0xC0000374u));
	CHECK(IsCrashExit(0xC0000005u));
	CHECK(IsCrashExit(0xC0000409u));
	CHECK(IsCrashExit(0xE06D7363u));
	CHECK(!IsCrashExit(0));
	CHECK(!IsCrashExit(0xC0000139u));
	CHECK(!IsCrashExit(0xC0000135u));
	CHECK(IsCrashExit(255));
	CHECK(std::string(ExitCodeName(255)).find("fatal runtime error") != std::string::npos);
}

static void TestStartFailureClassification() {
	// Ended before Sidecar signalled: a start failure, whatever the time.
	CHECK(ClassifyStartFailure(255, true, false, 0) == StartPhase::BeforeSignal);
	// Shortly after the signal, up to the window.
	CHECK(ClassifyStartFailure(255, false, true, 1800) == StartPhase::AfterSignal);
	CHECK(ClassifyStartFailure(255, false, true, StartFailureWindowMs) == StartPhase::AfterSignal);
	// Later it is a crash of a running game.
	CHECK(ClassifyStartFailure(255, false, true, StartFailureWindowMs + 1) == StartPhase::None);
	// Never signalled and not seen ending in start-up (the 60 s wait ran out).
	CHECK(ClassifyStartFailure(255, false, false, 0) == StartPhase::None);
	// Only the start-failure exit code counts: a loader failure, a fault and a freeze are not.
	for (const uint32_t code : {0u, 1u, 3u, 0xC0000005u, 0xC0000135u, 0xC0000409u, 0xCFFFFFFFu})
		CHECK(ClassifyStartFailure(code, true, false, 0) == StartPhase::None);
	CHECK(std::string(StartPhaseName(StartPhase::BeforeSignal)) == "before_signal");
	CHECK(std::string(StartPhaseName(StartPhase::AfterSignal)) == "after_signal");
	CHECK(std::string(StartPhaseName(StartPhase::None)).empty());
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

static void TestEndsInCall() {
	// Each case is the 7 bytes before a return address.
	const uint8_t direct[7] = {0x90, 0x90, 0xE8, 0x10, 0x20, 0x30, 0x40};       // call rel32
	const uint8_t import[7] = {0x90, 0xFF, 0x15, 0x00, 0x10, 0x40, 0x00};       // call [disp32]
	const uint8_t reg[7] = {0x90, 0x90, 0x90, 0x90, 0x90, 0xFF, 0xD0};          // call eax
	const uint8_t member[7] = {0x90, 0x90, 0x90, 0x90, 0xFF, 0x50, 0x08};       // call [eax+8]
	const uint8_t memberFar[7] = {0x90, 0xFF, 0x90, 0x00, 0x01, 0x00, 0x00};    // call [eax+0x100]
	const uint8_t indirect[7] = {0x90, 0x90, 0x90, 0x90, 0x90, 0xFF, 0x12};     // call [edx]
	const uint8_t sib[7] = {0x90, 0x90, 0x90, 0xFF, 0x54, 0x24, 0x04};          // call [esp+4]
	CHECK(EndsInCall(direct));
	CHECK(EndsInCall(import));
	CHECK(EndsInCall(reg));
	CHECK(EndsInCall(member));
	CHECK(EndsInCall(memberFar));
	CHECK(EndsInCall(indirect));
	CHECK(EndsInCall(sib));
	const uint8_t table[7] = {0xFF, 0x14, 0x85, 0x00, 0x10, 0x40, 0x00};       // call [eax*4+0x401000]
	CHECK(EndsInCall(table));
	const uint8_t sibShort[7] = {0x90, 0x90, 0x90, 0x90, 0xFF, 0x14, 0x24};    // call [esp]
	CHECK(EndsInCall(sibShort));
	// FF 14 with SIB base 101 needs a disp32, so as the last 3 bytes it is not a call.
	const uint8_t sibNoDisp[7] = {0x90, 0x90, 0x90, 0x90, 0xFF, 0x14, 0x85};
	CHECK(!EndsInCall(sibNoDisp));
	const uint8_t data[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	const uint8_t jump[7] = {0x90, 0x90, 0x90, 0x90, 0x90, 0xFF, 0xE0};         // jmp eax
	const uint8_t push[7] = {0x90, 0x90, 0x90, 0x90, 0x90, 0xFF, 0x30};         // push [eax]
	CHECK(!EndsInCall(data));
	CHECK(!EndsInCall(jump));
	CHECK(!EndsInCall(push));
}

static void TestCallersLine() {
	char line[48] = "callers:";
	size_t length = strlen(line);
	CHECK(AppendCaller(line, sizeof(line), length, "SSFIV.exe", 0x1DAAB0));
	CHECK(std::string(line) == "callers: SSFIV.exe+0x1DAAB0");
	CHECK(AppendCaller(line, sizeof(line), length, "", 0x10));
	CHECK(std::string(line) == "callers: SSFIV.exe+0x1DAAB0 ?+0x10");
	// One that does not fit leaves the line as it was.
	CHECK(!AppendCaller(line, sizeof(line), length, "Sidecar.dll", 0x12345678));
	CHECK(std::string(line) == "callers: SSFIV.exe+0x1DAAB0 ?+0x10");
	CHECK(length == strlen(line));
}

static void TestRegistersLine() {
	char out[256];
	const CrashRegisters registers = { 0, 0x0A64E0064u, 1, 2, 3, 4, 0x0019FF80u, 0x0019FF40u, 0x006ED0A3u };
	const size_t written = FormatRegisters(out, sizeof(out), registers);
	CHECK(written == strlen(out));
	CHECK(std::string(out) ==
		"registers: eax=0x00000000 ebx=0xA64E0064 ecx=0x00000001 edx=0x00000002 esi=0x00000003 edi=0x00000004 "
		"ebp=0x0019FF80 esp=0x0019FF40 eip=0x006ED0A3\n");
	// One line only, and nothing the crash record's own header search could mistake for its fields.
	CHECK(std::string(out).find('\n') == strlen(out) - 1);
	CHECK(std::string(out).find(" code=0x") == std::string::npos);
	CHECK(std::string(out).find(" address=0x") == std::string::npos);

	char small[24];
	const size_t cut = FormatRegisters(small, sizeof(small), registers);
	CHECK(cut == sizeof(small) - 1);
	CHECK(small[cut] == '\0');
	CHECK(FormatRegisters(nullptr, 0, registers) == 0);
}

static void TestCodeBytesLine() {
	char out[128];
	const uint8_t bytes[16] = {0x8B, 0x40, 0x10, 0x85, 0xC0, 0x74, 0x05, 0x8B, 0x48, 0x04, 0x89, 0x4D, 0xFC, 0xC3, 0x90, 0xCC};
	size_t written = FormatCodeBytes(out, sizeof(out), 0x006ED0A3ull, bytes, 16);
	CHECK(written == strlen(out));
	CHECK(std::string(out) == "code at 0x006ED0A3: 8B 40 10 85 C0 74 05 8B 48 04 89 4D FC C3 90 CC\n");

	// Fewer bytes than asked for are listed as read: a fault at the end of a mapped page.
	written = FormatCodeBytes(out, sizeof(out), 0x006ED0A3ull, bytes, 3);
	CHECK(std::string(out) == "code at 0x006ED0A3: 8B 40 10\n");

	// Nothing readable, such as a jump to an unmapped address.
	written = FormatCodeBytes(out, sizeof(out), 0, bytes, 0);
	CHECK(std::string(out) == "code at 0x00000000: unreadable\n");
	written = FormatCodeBytes(out, sizeof(out), 0x12345678ull, nullptr, 16);
	CHECK(std::string(out) == "code at 0x12345678: unreadable\n");
	CHECK(written == strlen(out));

	// A line cut by a small buffer stays terminated and in one line.
	char small[24];
	written = FormatCodeBytes(small, sizeof(small), 0x006ED0A3ull, bytes, 16);
	CHECK(written == strlen(small));
	CHECK(written < sizeof(small));
	CHECK(strchr(small, '\n') == nullptr || strchr(small, '\n') == small + written - 1);
	CHECK(FormatCodeBytes(nullptr, 0, 0, bytes, 16) == 0);
}

// A fake of ntdll's unload array: `slots` elements of `elementSize` bytes, which
// may be more than the struct, with the entries written into the slots named.
// The real element is 92 bytes on x86.
struct FakeTrace {
	std::vector<uint8_t> bytes;
	uint32_t elementSize;
	uint32_t slots;
	FakeTrace(uint32_t size, uint32_t count) : bytes((size_t)size * count, 0), elementSize(size), slots(count) {}
	void Set(uint32_t slot, uint32_t base, uint32_t imageSize, uint32_t sequence, const char* name) {
		UnloadEventTrace32 entry;
		memset(&entry, 0, sizeof(entry));
		entry.baseAddress = base;
		entry.sizeOfImage = imageSize;
		entry.sequence = sequence;
		for (size_t i = 0; name[i] && i < 32; ++i) entry.imageName[i] = (uint8_t)name[i];
		memcpy(&bytes[(size_t)slot * elementSize], &entry, sizeof(entry) < elementSize ? sizeof(entry) : elementSize);
	}
	UnloadedImages Parse() const { return ParseUnloadTrace(bytes.data(), elementSize, slots); }
};

// Three unloads in a 16-slot array of 92-byte elements, out of slot order.
static UnloadedImages ThreeUnloads() {
	FakeTrace trace(92, 16);
	trace.Set(0, 0x70000000u, 0x10000u, 2, "old.dll");
	trace.Set(3, 0x64A10000u, 0x40000u, 5, "shellext.dll");
	trace.Set(7, 0x69FF0000u, 0x30000u, 9, "other.dll");
	return trace.Parse();
}

// The game and Sidecar, as the host knows them, then the three unloads.
static ImageTable GameAndUnloads() {
	ImageTable table;
	table.Add("SSFIV.exe", 0x00400000u, 0x100000u, false);
	table.Add("Sidecar.dll", 0x10000000u, 0x200000u, false);
	table.AddUnloaded(ThreeUnloads());
	return table;
}

static void TestUnloadTraceBytes() {
	CHECK(UnloadTraceBytes(92, 64) == 92u * 64u);
	CHECK(UnloadTraceBytes(96, 1) == 96u);
	CHECK(UnloadTraceBytes((uint32_t)UnloadEntryNeeded, 1) == UnloadEntryNeeded);
	// A report that cannot be right reads nothing.
	CHECK(UnloadTraceBytes(10, 1) == 0);
	CHECK(UnloadTraceBytes(92, 0) == 0);
	CHECK(UnloadTraceBytes(92, UnloadTraceMaxElements + 1) == 0);
	CHECK(UnloadTraceBytes(UnloadTraceMaxElementSize + 1, 1) == 0);
	CHECK(UnloadTraceMaxBytes == (size_t)UnloadTraceMaxElements * UnloadTraceMaxElementSize);
}

static void TestUnloadTraceParsing() {
	const UnloadedImages images = ThreeUnloads();
	CHECK(images.count == 3);
	// Newest first, whichever slot each sits in.
	CHECK(std::string(images.items[0].name) == "other.dll");
	CHECK(images.items[0].base == 0x69FF0000u && images.items[0].size == 0x30000u);
	CHECK(std::string(images.items[1].name) == "shellext.dll");
	CHECK(std::string(images.items[2].name) == "old.dll");

	// A larger element than the struct is stepped by its own size.
	FakeTrace larger(112, 8);
	larger.Set(2, 0x64A10000u, 0x40000u, 1, "shellext.dll");
	larger.Set(5, 0x69FF0000u, 0x30000u, 2, "other.dll");
	CHECK(larger.Parse().count == 2 && std::string(larger.Parse().items[0].name) == "other.dll");

	// Elements of exactly the needed size, without the trailing Version words.
	FakeTrace shortElements((uint32_t)UnloadEntryNeeded, 4);
	shortElements.Set(1, 0x64A10000u, 0x40000u, 1, "shellext.dll");
	CHECK(shortElements.Parse().count == 1);

	// More unloads than are kept: the newest remain.
	FakeTrace many(92, 20);
	for (uint32_t i = 0; i < 20; ++i) many.Set(i, 0x10000000u + i * 0x10000u, 0x1000u, 100 + i, "m.dll");
	const UnloadedImages newest = many.Parse();
	CHECK(newest.count == UnloadedKept);
	CHECK(newest.items[0].sequence == 119 && newest.items[UnloadedKept - 1].sequence == 112);

	// Nothing unloaded, no array, and memory that is not the array.
	CHECK(FakeTrace(92, 16).Parse().count == 0);
	CHECK(ParseUnloadTrace(nullptr, 92, 16).count == 0);
	FakeTrace unaligned(92, 2);
	unaligned.Set(0, 0x64A10123u, 0x40000u, 1, "shellext.dll");   // not a 64 KB base
	unaligned.Set(1, 0x64A10000u, 0, 2, "shellext.dll");          // no size
	CHECK(unaligned.Parse().count == 0);
	FakeTrace unnamed(92, 1);
	unnamed.Set(0, 0x64A10000u, 0x40000u, 1, "");
	CHECK(unnamed.Parse().count == 0);

	// A name outside ASCII is narrowed, not dropped.
	FakeTrace wide(92, 1);
	wide.Set(0, 0x64A10000u, 0x40000u, 1, "ab");
	UnloadEventTrace32 entry;
	memcpy(&entry, wide.bytes.data(), sizeof(entry));
	entry.imageName[2] = 0x00E9;
	entry.imageName[3] = 'c';
	memcpy(wide.bytes.data(), &entry, sizeof(entry));
	CHECK(std::string(wide.Parse().items[0].name) == "ab?c");
}

static void TestUnloadedModuleIsNamedWithItsOffset() {
	const UnloadedImages images = ThreeUnloads();
	char out[512];
	// The fault in other.dll, with esi and edi into its former range and ebx a heap pointer.
	const CrashRegisters registers = { 2, 0x0F5A9650u, 0, 0, 0x69FFA670u, 0x69FFA670u, 0, 0x0012FE98u, 0x69FFA5E0u };
	size_t written = FormatUnloadedHits(out, sizeof(out), images, 0x69FFA5E0u, registers);
	CHECK(written == strlen(out));
	CHECK(std::string(out) ==
		"unloaded module: other.dll+0xA5E0\n"
		"esi is in the former range of other.dll+0xA670\n"
		"edi is in the former range of other.dll+0xA670\n");

	// The first and the last byte of a range are in it; the byte after is not.
	const CrashRegisters none = {};
	written = FormatUnloadedHits(out, sizeof(out), images, 0x64A10000u, none);
	CHECK(std::string(out) == "unloaded module: shellext.dll+0x0\n");
	written = FormatUnloadedHits(out, sizeof(out), images, 0x64A4FFFFu, none);
	CHECK(std::string(out) == "unloaded module: shellext.dll+0x3FFFF\n");
	written = FormatUnloadedHits(out, sizeof(out), images, 0x64A50000u, none);
	CHECK(written == 0 && out[0] == '\0');

	// A data pointer alone is worded as a lead, without a line for the address.
	const CrashRegisters dataOnly = { 0, 0, 0, 0, 0, 0x64A10040u, 0, 0, 0 };
	written = FormatUnloadedHits(out, sizeof(out), images, 0x00401000u, dataOnly);
	CHECK(std::string(out) == "edi is in the former range of shellext.dll+0x40\n");

	// Outside every range, and with no trace at all: nothing printed.
	const CrashRegisters elsewhere = { 2, 0x0F5A9650u, 0, 0, 0x00401000u, 0x7FFE0000u, 0, 0, 0 };
	written = FormatUnloadedHits(out, sizeof(out), images, 0x00402000u, elsewhere);
	CHECK(written == 0 && out[0] == '\0');
	UnloadedImages empty;
	written = FormatUnloadedHits(out, sizeof(out), empty, 0x69FFA5E0u, registers);
	CHECK(written == 0 && out[0] == '\0');
	written = FormatUnloadedList(out, sizeof(out), empty);
	CHECK(written == 0 && out[0] == '\0');

	written = FormatUnloadedList(out, sizeof(out), images);
	CHECK(written == strlen(out));
	CHECK(std::string(out) ==
		"unloaded modules: other.dll@0x69FF0000+0x30000 shellext.dll@0x64A10000+0x40000 old.dll@0x70000000+0x10000\n");

	// A list that does not fit is cut at an entry and stays one terminated line.
	char small[64];
	written = FormatUnloadedList(small, sizeof(small), images);
	CHECK(written == strlen(small) && written < sizeof(small));
	CHECK(written == 0 || small[written - 1] == '\n');
	CHECK(FormatUnloadedHits(nullptr, 0, images, 0, none) == 0);
}

static void TestImageTable() {
	const ImageTable table = GameAndUnloads();
	char out[96];
	size_t written = DescribeImage(out, sizeof(out), 0x00401234u, table);
	CHECK(written == strlen(out));
	CHECK(std::string(out) == "SSFIV.exe+0x1234");
	written = DescribeImage(out, sizeof(out), 0x69FFA670u, table);
	CHECK(written == strlen(out));
	CHECK(std::string(out) == "other.dll+0xA670 (unloaded)");
	// Neither: empty.
	CHECK(DescribeImage(out, sizeof(out), 0x12345678u, table) == 0);
	CHECK(out[0] == '\0');
	CHECK(table.Find(0x500000u) == nullptr);       // the first byte past the game
	CHECK(table.Find(0x4FFFFFu) != nullptr);
	CHECK(DescribeImage(nullptr, 0, 0, table) == 0);

	// A loaded image wins where an unloaded one held the same range.
	ImageTable reloaded;
	reloaded.Add("reloaded.dll", 0x69FF0000u, 0x10000u, false);
	reloaded.AddUnloaded(ThreeUnloads());
	DescribeImage(out, sizeof(out), 0x69FF0010u, reloaded);
	CHECK(std::string(out) == "reloaded.dll+0x10");

	// A full table refuses more, and an empty range is not added.
	ImageTable full;
	for (unsigned i = 0; i < ImageTable::Capacity; ++i) CHECK(full.Add("a.dll", 0x10000u * (i + 1), 0x1000u, false));
	CHECK(!full.Add("b.dll", 0x7000000u, 0x1000u, false));
	CHECK(!ImageTable().Add("c.dll", 0, 0x1000u, false));
	CHECK(!ImageTable().Add("c.dll", 0x10000u, 0, false));

	written = FormatImageOffset(out, sizeof(out), "start.dll", 0x20u, false);
	CHECK(std::string(out) == "start.dll+0x20" && written == strlen(out));
}

static void TestLocatedAddressLine() {
	char out[128];
	size_t written = FormatLocatedAddress(out, sizeof(out), "start address", 0x69FFA5E0u, "other.dll+0xA5E0 (unloaded)");
	CHECK(written == strlen(out));
	CHECK(std::string(out) == "start address: 0x69FFA5E0 other.dll+0xA5E0 (unloaded)\n");
	written = FormatLocatedAddress(out, sizeof(out), "start address", 0x1234u, "");
	CHECK(std::string(out) == "start address: 0x00001234\n");
	char small[16];
	written = FormatLocatedAddress(small, sizeof(small), "start address", 0x1234u, "");
	CHECK(written == 0 && small[0] == '\0');
}

static void TestStackWordsKeepOnlyImageWords() {
	const ImageTable table = GameAndUnloads();
	// A UTF-16 path as it sits on a stack: "C:\U" as two words, and a pointer into the heap.
	const uint32_t words[] = { 0x0000003Au, 0x0055005Cu, 0x0040C0DEu, 0x69FFA5E0u, 0x12345678u, 0x00600000u };
	char out[512];
	const size_t written = FormatStackWords(out, sizeof(out), 0x0012FE98u, words, 6, table);
	CHECK(written == strlen(out));
	CHECK(std::string(out) ==
		"stack from esp=0x0012FE98: 6 words, 2 in images, 4 left out\n"
		"stack+0x08: 0x0040C0DE SSFIV.exe+0xC0DE\n"
		"stack+0x0C: 0x69FFA5E0 other.dll+0xA5E0 (unloaded)\n");
	// Words that are not in an image are never printed, whatever they hold.
	CHECK(std::string(out).find("0055005C") == std::string::npos);
	CHECK(std::string(out).find("12345678") == std::string::npos);

	// No word in an image: the count alone.
	const uint32_t plain[] = { 1, 2, 3 };
	FormatStackWords(out, sizeof(out), 0x0012FE98u, plain, 3, table);
	CHECK(std::string(out) == "stack from esp=0x0012FE98: 3 words, 0 in images, 3 left out\n");

	// Nothing to show: nothing printed.
	CHECK(FormatStackWords(out, sizeof(out), 0x0012FE98u, words, 0, table) == 0);
	CHECK(out[0] == '\0');
	CHECK(FormatStackWords(out, sizeof(out), 0x0012FE98u, nullptr, 4, table) == 0);

	// A buffer that holds some lines keeps whole lines only.
	char small[90];
	const size_t cut = FormatStackWords(small, sizeof(small), 0x0012FE98u, words, 6, table);
	CHECK(cut == strlen(small) && cut < sizeof(small));
	CHECK(small[cut - 1] == '\n');
	CHECK(std::string(small).find("stack+0x0C") == std::string::npos);
}

static void TestAddressRegionLine() {
	char out[160];
	AddressRegion region;
	region.state = RegionState::Free;
	region.protect = 1;
	region.regionBase = 0x64A1A000u;
	region.regionSize = 0x5E000u;
	size_t written = FormatAddressRegion(out, sizeof(out), 0x64A1A5E0u, region);
	CHECK(written == strlen(out));
	CHECK(std::string(out) == "address 0x64A1A5E0: free region=0x64A1A000+0x5E000 protect=0x1\n");

	region.state = RegionState::Committed;
	region.type = RegionType::Image;
	region.protect = 0x20;
	region.allocationBase = 0x00400000u;
	region.regionBase = 0x00401000u;
	region.regionSize = 0x1000u;
	written = FormatAddressRegion(out, sizeof(out), 0x00401234u, region);
	CHECK(std::string(out) == "address 0x00401234: committed image region=0x00401000+0x1000 protect=0x20 allocation_base=0x00400000\n");

	region.state = RegionState::Reserved;
	region.type = RegionType::Private;
	written = FormatAddressRegion(out, sizeof(out), 0x00401234u, region);
	CHECK(std::string(out).find(": reserved private region=") != std::string::npos);

	// VirtualQuery failed.
	written = FormatAddressRegion(out, sizeof(out), 0x10u, AddressRegion());
	CHECK(std::string(out) == "address 0x00000010: not queryable\n");

	char small[24];
	written = FormatAddressRegion(small, sizeof(small), 0x00401234u, region);
	CHECK(written == 0 && small[0] == '\0');
	CHECK(FormatAddressRegion(nullptr, 0, 0, region) == 0);
}

int main() {
	TestRingKeepsTheLastLinesInOrder();
	TestRingCutsLongLines();
	TestExitCodeNames();
	TestSpeechEngineModules();
	TestSpeechCrashMarker();
	TestStartFailureClassification();
	TestHeaderNamesTheFault();
	TestHeaderToleratesMissingFacts();
	TestAddressSpaceFold();
	TestEndsInCall();
	TestCallersLine();
	TestRegistersLine();
	TestCodeBytesLine();
	TestUnloadTraceBytes();
	TestUnloadTraceParsing();
	TestUnloadedModuleIsNamedWithItsOffset();
	TestImageTable();
	TestLocatedAddressLine();
	TestStackWordsKeepOnlyImageWords();
	TestAddressRegionLine();
	printf("crash_report_test: all tests passed\n");
	return 0;
}
