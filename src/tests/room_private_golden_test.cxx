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
		"9af76555fc1b309dc783d1a8e339408bfab5dd0871fafca8449fa130a1649208",
		"2bc06919bbbcd0195f3151da4af09b69f4e3e7e006b525dd4eb8292010fa93a3"},
	{"join host", true,
		"e0f6f0ab317a7ea722161cc311d4eed3f5377d1299ac82b9f2474f303385e44c",
		"e554ae0547786f75ac286f9dbd21bc6bf7790beea5b2a2f958aa7d6c1ddee4ca"},
	{"join a", true,
		"00fe28fc232fa4be685467904476d9d9d4e4f40a54e20482629f64a07b199045",
		"32884bbcd3e784d95a39bd57fc3b5a632e051bf503124379a94b216dc7dfb4cc"},
	{"join b", true,
		"d96e716310d58324e02e9055e7d7428b0709fefec1bb0f681ab38163c26340e1",
		"c12f8b4f866128b53dd244807ccbc22707ff4d4cee7099555568e9bd022d1db3"},
	{"join c", true,
		"0b7c5e5ed58801836a21b520dddff7d8c2967453303c751bac4554f986acdfdb",
		"8badd26be25800fb8302ceae9c104ebceb04dd0c23766ded0fd6710f018abde2"},
	{"queue a", true,
		"c797b8b8da54145259cb72b7feac2b8636ba872a840be4076fa005c39fe4ff61",
		"451d3303f529c26fbc6576d533b4881ea52ab69c3788d12371fc6be44da34395"},
	{"queue b", true,
		"45e0bc25442a3804bbd48bcddcb8ce980c5a1b72a9e89ad25fd03268f0989c1d",
		"ef1a12d18083d5addb2fa53a058873684721a874a1f02409ac4a7af210b766fa"},
	{"ready a", true,
		"c541f07759433b3f887d322bb26e841d571c4cd8f858d108c53bec3dd3ba1ba3",
		"a3ffec5cb9ab38aa7baa5dededbed1e52678d06f7988c50c4700812f90835a63"},
	{"ready b", true,
		"e3ab93f14dae6447f05fa968e7de3ca6527baad0fdb01f0329474f382632e86f",
		"ffaa790ec886a85fedab32f566b7bfaaeb9cf60e40c707adb12247fcd99aabf3"},
	{"begin match", true,
		"b2f67fc75a788b162155c47ba41958ac339798b5cf6f6f56fc40f7492e61c68e",
		"ac93b3f7690700334fa12731df1cf22e14ed0b7826ede06fda75d01b0cc39450"},
	{"advance time", true,
		"b2f67fc75a788b162155c47ba41958ac339798b5cf6f6f56fc40f7492e61c68e",
		"e3a821b1bec715d47fabffa877ed478a80779c1856cc2ec5244a1c2be91231ad"},
	{"result a", true,
		"3d1e0525eca36497e51c4dd9c8ea34a092739eea3e1b204d8bc5bb00dc5d372d",
		"befbbe33bdd77144644b48dce9350cc23a6462038bf662006bdd118d46d18da4"},
	{"result b", true,
		"a10bc5e63438ded0cf4626a5dcccfb8d35b368ab399ced46243c070acbf7e202",
		"7344040dceb99e247edbafd6461ef5789a4d893b1f186f9f3859454f924f1fec"},
	{"end match", false,
		"a10bc5e63438ded0cf4626a5dcccfb8d35b368ab399ced46243c070acbf7e202",
		"7344040dceb99e247edbafd6461ef5789a4d893b1f186f9f3859454f924f1fec"},
	{"acknowledge a", true,
		"83aa7a308c19a9ad3568d3ddb49a10c712f95364fed88eafc1e5147b4dea613b",
		"3c2fe6ae9bfc6cf432de341c59c72d761907c3d35550973f9cea54a508415505"},
	{"acknowledge b", true,
		"09920924001cde5761771f638d6e9fbbab69d909e6fc625c52a9cea61d18f35b",
		"f6c2d661e5344873a06e776aaa2b4c0393b3bec859d4df03d4f6971867d74620"},
	{"chat", true,
		"b999abecef4199c0d1acc0f03f38f7fc171a4de65e1cd9efe4122e99fb49c349",
		"c3db253d2c286daa7673a8fb8cfbe1bf8acab67ceb403548163f6f0c8e8b62b4"},
	{"kick c", true,
		"406adf1fd2a7941cc215212595782c99ed21e8d61f5ec862b615fb78d3d6f2ea",
		"b2f36ad00c690d5895b3a8584c07f80dc9dcf5ea434f6561e922ce87aa65f474"},
	{"rejoin kicked", false,
		"406adf1fd2a7941cc215212595782c99ed21e8d61f5ec862b615fb78d3d6f2ea",
		"b2f36ad00c690d5895b3a8584c07f80dc9dcf5ea434f6561e922ce87aa65f474"},
	{"leave b", true,
		"4e1df65b9b330980742528388f11d7225f44f34965ad3b77ae1bce6c77bbad68",
		"735ac6be53d6b0db12db2bafec067da5685183c056f7f61d0515d26e64ba9256"},
	{"transfer host", true,
		"ec10db437d6988ad66e11e8530d17bec1b522b6986d4ba90041f40fc297c5c9c",
		"16271a8d857116beec0ffe2c265a998ee79c567e35c4caf6bf1348f92ec6b4e9"},
	{"leave host", true,
		"168e9c357d4ae66254a8919ade1ff6d760d52af18b44dd5a9909a570f546d2f4",
		"a9238376624bb9908eaf7a4ce2dff83add938d202d08f500bccbffe8cc6abd01"},
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
