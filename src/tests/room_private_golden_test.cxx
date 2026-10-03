// Private rooms are byte-identical to the revision before server-owned rooms
// (f95cef2). A scripted private room runs from creation to a host transfer; after
// each step the SHA-256 of the room snapshot's JSON and of the authority's
// Checkpoint() JSON are compared with the values the f95cef2 sources produced for
// this same script. `--print` writes the table in the form kGolden below. The
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

// From the f95cef2 sources: the same script built against that revision's room model.
const Step kGolden[] = {
	{"create", true,
		"169021b8ae7fb726cf64a52b20ab5dee7ef89a8ca8dd0919d65242b798b3b3d4",
		"e03ef1ce8fda38df360e42158c94f69c117f506b6185cafe0f282352f1972f9b"},
	{"join host", true,
		"167f40edaededfb6abf1e35eec3f8f672483b328808daf1f87ac2093643017d4",
		"7c5a3f13e85b5c2d0f5a696438cc280e1f60cf169198fb0ae93f3f6fdb645acc"},
	{"join a", true,
		"ef94938f120bc0475463a0f88fd2aeeda08baf9258de6808fdc6f529aeb8a9cb",
		"afd09a48b6aa220bf588ae267d3257330f62c335b2bebc25ea0dad69db179d90"},
	{"join b", true,
		"309348b948398d881a15e9e5cb7d5bab75b0bd75bf6837842c8b597549099df8",
		"38530dfd0a118697152841aa7733bc32b3391a071b02156b5215b31b3859cc01"},
	{"join c", true,
		"d2e51466cc3640c2ca740c260f9a892cd90a33fdd9deee5480f7864f2c050560",
		"f146d1ee2a60b09405a7b395094ffcf4e2b3930480cd7aa5dbb3bdc3bbbcd4b0"},
	{"queue a", true,
		"8a93af277367cced97f537b8fe5c3cc3026178c714372b82749daec3a6b57c1b",
		"42aed2e1dd73d5136e6f3b6efed2440c7dfa53b6473ff4fca71a7b34b4ee7abe"},
	{"queue b", true,
		"d100d37653d36d39101a0b2fe8efa6499f826276f6431f0dda7b0a83db8c685e",
		"69d9ed2edc2b9c29b4221eed5457e65ec1a0b52d4e18667629864306c5ee1158"},
	{"ready a", true,
		"ae241d3a452b45c958111d65d48ff44c00c3a98e36da79ab4559673bb34eeb34",
		"7578be90b06db2d25d08ee1cdb39dbe34aa87c9b66a219d13f225c4bca978878"},
	{"ready b", true,
		"d04957ee2409cb5219cbd193e9d1d788a455a7128a2b155b49d2faf5e1570398",
		"30b060d6ea8053e93bb6de10f1888b8a7c7750cf6eb5697826c911aa0c5edff9"},
	{"begin match", true,
		"71898afe3d86a8c43a1007a541ebf338c5a087293c973afa9cd4771e8e2040f6",
		"6561281022a86eab0b23c4267451d9c1a2d1f35055fc78eec0bf8948bbeda38e"},
	{"advance time", true,
		"71898afe3d86a8c43a1007a541ebf338c5a087293c973afa9cd4771e8e2040f6",
		"bc76fbe1f20a603c1e9276d4a5ccee6a6032539435f51811f3f6968446efce8a"},
	{"result a", true,
		"23c1fda139fe8cca8e2157a76b3c726697d1dbbe2e100718670f894828433363",
		"77eb9c746d19e805156a006f554e89265399f5ed4df7635edc3ad0bf20ae4275"},
	{"result b", true,
		"2a03cb920bec3198efdd58ccd5c3e1a080e10b40b4a7970b7ab304a6c32b0df7",
		"e7abae79c31f11edc63fbc5ce0fe6191b683030e2fc94d0d35e950d6972c36e5"},
	{"end match", false,
		"2a03cb920bec3198efdd58ccd5c3e1a080e10b40b4a7970b7ab304a6c32b0df7",
		"e7abae79c31f11edc63fbc5ce0fe6191b683030e2fc94d0d35e950d6972c36e5"},
	{"acknowledge a", true,
		"749087dc9cf0f81336579e257182c20c713bc3318696cee2796d0ae0ba0f3c4b",
		"4a0fa46fd444bba3f8d40003ad12098b5dc96cc5584374f3817d025316f882f6"},
	{"acknowledge b", true,
		"4bb9d88b9afb7b8d5c0171e09ae845453a80ffa467e81b37ee4ebb52090b3a1f",
		"c5e8cce6938b6111d9337b765d9a945d9e8092e34f38f30e5d540e59eaa31f56"},
	{"chat", true,
		"577a1de7a159f845d6fda2306f5cd35a94e271ae0a0b25967b0f8f5f28450738",
		"e44b5be53fe4b6644ea1cd8b5ad21b7ec29d9b7a3565fef429189d633d03a5bd"},
	{"kick c", true,
		"1ae7c3d2d5fb5d2b0ab4161888052e2f371cc237244fa7c391be12d1ad470a32",
		"1af05f94c63c14286127f6265dff22d5a737f2b2fe8089db4c0973ff35bcd8cf"},
	{"rejoin kicked", false,
		"1ae7c3d2d5fb5d2b0ab4161888052e2f371cc237244fa7c391be12d1ad470a32",
		"1af05f94c63c14286127f6265dff22d5a737f2b2fe8089db4c0973ff35bcd8cf"},
	{"leave b", true,
		"d74c536bd222b1efbed9fa0a4ea659d35efcc974a0068b1e0b717c1623737918",
		"91b363c8a522d1a363ffbb43a8da4ab051cda95290e7e02ea02511bcc6593b7d"},
	{"transfer host", true,
		"3400e48454346cc6ba7d210dd6a6883bd6f5f1411edd41e654e50cdb45e4b771",
		"3acafd7337b7b88cd617516a700cbc4422a182ef5de66efb1abe3a1c1eea8d9a"},
	{"leave host", true,
		"8985b3cba451726fe51ade3bf06b17d93faf5e71d03a9bc2664f634a75abb2c0",
		"561ec155373f49b86bf9952b08d5c0b78b121fc14e0b4a3529550da6d47570a2"},
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
