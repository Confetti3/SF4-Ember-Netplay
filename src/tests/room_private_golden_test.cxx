// Private rooms are byte-identical to the revision before server-owned rooms
// (f95cef2), but for the eight tables room protocol 4 gives every room. A
// scripted private room runs from creation to a host transfer; after each step
// the SHA-256 of the room snapshot's JSON and of the authority's Checkpoint()
// JSON are compared with the values the protocol 4 sources produced for this
// same script. `--print` writes the table in the form kGolden below. The
// script uses only API that exists at f95cef2 and fixed times, so it can be
// built against either revision.
#include "../session/SessionRecovery.hxx"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using namespace sf4e::room;
using sf4e::session::recovery_detail::Sha256Portable;

namespace {
struct Step {
	const char* name;
	bool accepted;
	const char* snapshot;
	const char* checkpoint;
};

// From the protocol 4 sources: the same script built against that revision's room model.
const Step kGolden[] = {
	{"create", true,
		"37efd500e41bd6fef88854c5a74e0e2edd36c4ae571f84e2971ff5dedb559f4e",
		"9ab97a2dcb1aa49bdefc1ad789ab41cac2f26b4521a5604f7e55437d2ea22245"},
	{"join host", true,
		"863028c1041f228688c3c2968160a75fcee3a3442649da4d6e804b721333ae90",
		"6ac2ecda8d45a30b09d1d15ed975a4373151c416a666d0b1dcc484ce9c49794d"},
	{"join a", true,
		"200046a139f4a7f3d7b89b1d4a617b2c29b3787c684b06d30fa3671d3e3704ed",
		"c35f183dff1cfc2ae9d97e53da9b1d3bbd631074ba21918250a3c52da0322f37"},
	{"join b", true,
		"4c0db70f467a8f8cf5626c27df91aee3055610061d0a10f3f173beb9352f6c52",
		"963470c632b24a69144c240f6abb8a2b56116685da94b7832e34dbd283731af8"},
	{"join c", true,
		"cb62cfa37da4cb7124fd33651231fbbee18159f123c6a2d1284b76f572313ef2",
		"676300bad022090d3d11654b3bafc74fa4a4a6df159e2b2495b5b77e46bcc834"},
	{"queue a", true,
		"2d45569d03ffced2c48736a50b90ec733074fef14d10ee6c79c6d18ead21841c",
		"8910203ae3346a3cc255b93d37eec28431ff0b3ee90d9f8804b2c957a15e01d5"},
	{"queue b", true,
		"eff8973bca7c5945b6abb1afe17224c0b364b866d007eb0815358eeece0eb189",
		"9b177843bc956fd3d58397956a4be67223fc98381d806005d7e99a2048d85dbf"},
	{"ready a", true,
		"d89543c594864932504c01341314f6219667bb623936fba759e86cac85698cd8",
		"d64dd87dc5d770c84b0890c1d5166eaffa00b46948e4d7362759beb6ed986995"},
	{"ready b", true,
		"7d9eaa1633da24426b28bd15cd9877cc1c42ce234853c5a8aef0d3374319be3f",
		"0290f28c04d4245744149d402ba3a0c1c6f46ee7dcf68227488fd21d7431fefe"},
	{"begin match", true,
		"6b163805e86a85fccc8b4ea1ea00fd3de923327a25980cc55b8529ca7eeaf8b0",
		"de4b8f4cd7bec27b9c849982efb78978c7e58dbdfb9a1a86594584f54761b601"},
	{"advance time", true,
		"6b163805e86a85fccc8b4ea1ea00fd3de923327a25980cc55b8529ca7eeaf8b0",
		"a98c3adadced2c233e6ebfa228bdaec8fd9b85696f994c07e155c97cfea6e5ca"},
	{"result a", true,
		"feb2476ceec69d9f70374d0aa6968133a74ee9ac5d3610f9558d84a3d8ed4e8d",
		"499176e27e6dfb731b6065cd689e217414527b5fab57afa0d159d0c746f1d712"},
	{"result b", true,
		"b0f7f0c35ddf2fb4c9d284100c907de7195b27a119b78fb90878aa85b29c2186",
		"c5f6ebf6935c3b12bbde8e18b311767c88b71cff22c61b053311905ae03700e8"},
	{"end match", false,
		"b0f7f0c35ddf2fb4c9d284100c907de7195b27a119b78fb90878aa85b29c2186",
		"c5f6ebf6935c3b12bbde8e18b311767c88b71cff22c61b053311905ae03700e8"},
	{"acknowledge a", true,
		"23a9c17898be07487114c85f802857b405f5537792078a22b622ea804f8f3923",
		"a4b38d7aa95ee508e1bed98f5a7eca7be548a23804f99c8c8db7c3cbe70f4383"},
	{"acknowledge b", true,
		"35c733e5d42b6cfab36c1811866f48ae885998aebc7ec3bd61e2bc6b0117eee0",
		"46da744f08686a86e8178f573a541a1e7d82c0057ea74aaee690c9358cee33e8"},
	{"chat", true,
		"4549de8bc39e2b6eaec75ba6ecd60e92fc8c0e09b140b43a1d30f48d4e8bdfc8",
		"9b9e938b6c17d557e2e914ad39d1fc49d71b4708e7685ee8669daf657a0fc437"},
	{"kick c", true,
		"14129abe3a1d542d4bcf88df57abb982d1b81db8782089d0062a322aa5b47da7",
		"6280d9d095d9dd0bbb8c6ac33294b8d00c8d189a74506ee691c04dd649f24fc7"},
	{"rejoin kicked", false,
		"14129abe3a1d542d4bcf88df57abb982d1b81db8782089d0062a322aa5b47da7",
		"6280d9d095d9dd0bbb8c6ac33294b8d00c8d189a74506ee691c04dd649f24fc7"},
	{"leave b", true,
		"770bab34a2ff08d4ffed81f6939080fed70d79cbc56616e4b1a680fa71fbeaf5",
		"918738f5664440904784dc9603f1ce588b47f423392d12293e98f1181799885e"},
	{"transfer host", true,
		"4c1591a530b953607234b9a9ca734770524b69e54ffc26c2db7385a49f3f2c16",
		"07a25664fecad6485ae1551ed2b995ea48510f525f5b634b62b930b0fa149745"},
	{"leave host", true,
		"1b707234ff298ff6b3ed5ea62fbb49a7cbdeb37e816e0fda7dbb3234a9f0e87b",
		"17599b30551dfb4ea234a0661bf071858730f9599d306edc54b490def83cd664"},
};

struct Line { std::string name; bool accepted; std::string snapshot, checkpoint; };

std::vector<Line> Run() {
	std::vector<Line> lines;
	RoomAuthority authority("Golden", 8, 11);
	const auto record = [&](const char* name, bool accepted) {
		lines.push_back({name, accepted, Sha256Portable(nlohmann::json(authority.SnapshotCopy()).dump()),
			Sha256Portable(authority.Checkpoint().dump())});
	};
	const auto peer = [](int index) { return ConnectionRef{"host", std::to_string(index)}; };
	const auto act = [&](MemberId member, ActionKind kind) {
		Action action;
		action.kind = kind;
		action.roomEpoch = authority.SnapshotView().roomEpoch;
		action.revision = authority.SnapshotView().revision;
		action.table = 0;
		action.tableRevision = authority.SnapshotView().tables[0].revision;
		action.actionId = member * 1000 + authority.SnapshotView().revision + 1;
		return action;
	};
	record("create", true);
	const auto join = [&](const char* name, int index, bool host) {
		const auto result = authority.Join(name, peer(index), host);
		record(name, result.accepted);
		return result.accepted ? result.snapshot.members.back().id : MemberId(0);
	};
	const auto host = join("join host", 0, true);
	const auto a = join("join a", 1, false);
	const auto b = join("join b", 2, false);
	const auto c = join("join c", 3, false);
	const auto apply = [&](const char* name, MemberId member, const Action& action) {
		record(name, authority.Apply(member, action).accepted);
	};
	apply("queue a", a, act(a, ActionKind::Queue));
	apply("queue b", b, act(b, ActionKind::Queue));
	apply("ready a", a, act(a, ActionKind::Ready));
	apply("ready b", b, act(b, ActionKind::Ready));
	authority.AdvanceTime(1000);
	const auto begun = authority.BeginMatch(0, a, b);
	record("begin match", begun.accepted);
	authority.AdvanceTime(2000);
	record("advance time", true);
	const auto generation = authority.SnapshotView().tables[0].matchGeneration;
	auto report = act(a, ActionKind::RecordResult);
	report.matchGeneration = generation; report.result = MatchResult::P1Win;
	apply("result a", a, report);
	report = act(b, ActionKind::RecordResult);
	report.matchGeneration = generation; report.result = MatchResult::P1Win;
	apply("result b", b, report);
	record("end match", authority.EndMatch(0, generation, MatchResult::P1Win).accepted);
	for (const auto member : {a, b}) {
		auto ack = act(member, ActionKind::AcknowledgeTerminal);
		ack.matchGeneration = generation;
		apply(member == a ? "acknowledge a" : "acknowledge b", member, ack);
	}
	authority.AdvanceTime(5000);
	auto chat = act(c, ActionKind::Chat);
	chat.text = "hello table";
	apply("chat", c, chat);
	auto kick = act(host, ActionKind::Kick);
	kick.target = c;
	apply("kick c", host, kick);
	record("rejoin kicked", authority.Join("c again", peer(3)).accepted);
	record("leave b", authority.Leave(b).accepted);
	record("transfer host", authority.TransferHost(host, a).accepted);
	record("leave host", authority.Leave(host).accepted);
	return lines;
}
}

int main(int argc, char** argv) {
	const auto lines = Run();
	if (argc > 1 && !std::strcmp(argv[1], "--print")) {
		for (const auto& line : lines)
			std::printf("\t{\"%s\", %s,\n\t\t\"%s\",\n\t\t\"%s\"},\n", line.name.c_str(), line.accepted ? "true" : "false",
				line.snapshot.c_str(), line.checkpoint.c_str());
		return 0;
	}
	int failures = 0;
	const auto count = sizeof(kGolden) / sizeof(kGolden[0]);
	if (lines.size() != count) { std::printf("FAIL: %zu steps, %zu expected\n", lines.size(), count); ++failures; }
	for (std::size_t i = 0; i < lines.size() && i < count; ++i) {
		const auto& golden = kGolden[i];
		if (lines[i].name != golden.name || lines[i].accepted != golden.accepted) {
			std::printf("FAIL step %zu (%s): outcome %d, expected %s %d\n", i, lines[i].name.c_str(), lines[i].accepted, golden.name, golden.accepted);
			++failures;
		}
		if (lines[i].snapshot != golden.snapshot) { std::printf("FAIL step %zu (%s): snapshot JSON differs\n", i, golden.name); ++failures; }
		if (lines[i].checkpoint != golden.checkpoint) { std::printf("FAIL step %zu (%s): checkpoint JSON differs\n", i, golden.name); ++failures; }
	}
	if (failures) return 1;
	std::puts("RoomPrivateGolden test passed");
	return 0;
}
