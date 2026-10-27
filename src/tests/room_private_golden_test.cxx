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
		"03fe2457f0262655aa575b559bbaec4a8f26964d39c5c84690af69facd11acdc",
		"42d28fd18855247b43ba37524c7008c5e48d3a024275f360d268e1ebddce68b0"},
	{"join host", true,
		"dbff974b58eaba1bdc23b9de8b4a711e097716fb875ae6f5727f596c5d2a21f4",
		"e9d0d17ac7f50738c14e626f6f3a8b4d12a5ec469ea121d8ad0cd1ea45a9a43b"},
	{"join a", true,
		"22ed2355481ad0969a5e8a6656c3652462ad778cb0d022f132bc43b80dc18b58",
		"5723541d79d771eeaee4ec9599c8949960c00a157a05bc7d2a1cd8f556016365"},
	{"join b", true,
		"fe28b9f6dcaf4bcc90157a074b3ff2899c19f16e734e12b6568ac12a1a08c5d4",
		"03263d597f7d44946b0d30c86a4b58e044551e167c3837a13bc1ccc95f3f2eef"},
	{"join c", true,
		"acd3b4577b9492d5daec3389eb7b538591b2ec1035a35284d17acc589f2ee9fd",
		"2cfc74edafc77699780fa76385592f7bf91f82bcd719c831bc0bf1ec0329d97a"},
	{"queue a", true,
		"cb4216cdcfaa574dc3cf8296e873a20a610147f5031ab9a30d0ff1a4cef6e956",
		"85744b016e77d33998eeb26ea9e6c74d068b4da2cea16a77f55e51e0bd2287c7"},
	{"queue b", true,
		"8ff520289e653fb57c1bb29a476b55c0e2690065e851bd092658e64ad857ff81",
		"37b798e2f9064380d192bef3b56fd5074352e474531b78c78e7fcaeb3179e29e"},
	{"ready a", true,
		"5686fd0879c34aa0e9919f06b03674b54f9485e3d074cab4c1b7b400bf682a67",
		"2bdaa15ba51962d4a11c86b30ed198cb1cd94de6939845ec75ad365c0e727e66"},
	{"ready b", true,
		"c6fae2d0c2456541544b23074e22ba1a654e83a7a1601d24c9160269b57e149d",
		"e38e75eb792a5da9656df6a3f4b89ea2356a63ddd77683b311625d87e5857877"},
	{"begin match", true,
		"39ea13d62ae4d6ee1ecf049fd40f927246882e212d0fd55dad0e1b9d7b3e79d2",
		"462db93d55fab76e0ff8851595635439b7b218fe8cf047da656bca86beda7247"},
	{"advance time", true,
		"39ea13d62ae4d6ee1ecf049fd40f927246882e212d0fd55dad0e1b9d7b3e79d2",
		"714950284d31462cb56f952d99b049ed153472ccfa959670f01551f12f45e1f9"},
	{"result a", true,
		"8f2b89d77be6d79d743da01cf90b21b4f2df6ac27d91368a787fda64d431b41f",
		"4ffb410f070c9d23af7296398b161cfd63eefe266e47278f26a081629b2fe93b"},
	{"result b", true,
		"c3085f70264c210c055cd560a76b23726553aea950383c218e952c6e951ab7c3",
		"17c86966f71226baece013b7d847081116f88c6092340da235c07721f7062444"},
	{"end match", false,
		"c3085f70264c210c055cd560a76b23726553aea950383c218e952c6e951ab7c3",
		"17c86966f71226baece013b7d847081116f88c6092340da235c07721f7062444"},
	{"acknowledge a", true,
		"3876c82e2c65be8e00f7dc9c065f9d08f6e321e3267b9586a9bbd24adb22dfe0",
		"fe9cac49eeae9ddd5e96b784e43bf4c654d3fe72cc93e6e65badd6c15c3af6e8"},
	{"acknowledge b", true,
		"603a4c72e49d21ecf91b079d26b86b8bd6dd12402679e4d57b9ff1667a934017",
		"3ecbe55e66078f47047830968470014276d1976c01e0b9fea7e3cb17ef32c8d4"},
	{"chat", true,
		"033e8d2ee07eb25afad7c351abf34e8773d72e04e8fc5cb4bf69e06977d13b4b",
		"31df6d14fa623939d32087cda614f3791bd1f18ca300adf23d410f8f81354dab"},
	{"kick c", true,
		"c7a7025d0bd60518fa1eef9c85e206f204e72a4e6a7b3124ca80f3aa89912868",
		"1193522de96cf32e95bd44efef58134ac164c76cc5397cc9a79f5308f6307607"},
	{"rejoin kicked", false,
		"c7a7025d0bd60518fa1eef9c85e206f204e72a4e6a7b3124ca80f3aa89912868",
		"1193522de96cf32e95bd44efef58134ac164c76cc5397cc9a79f5308f6307607"},
	{"leave b", true,
		"419f7359409ee03a29564bd5a82c8d7710a4f77b66a1d7305cad60ae7c5c3186",
		"a375023bd8cc125639a962ba0a8c542d2f6e78e31be8a48eb53a4f32017e8d10"},
	{"transfer host", true,
		"d1a16e3231ddaefb8edba8faa14a4ab09ea31740ac3978be39dee602d5ff7e00",
		"2c262acf37f5f840dc9f420173c985c36c684689e6b2cc0c29d9276c3bdbd69c"},
	{"leave host", true,
		"6236ea0ca1b060902b737a449b6c42064d810699d7515df0cb6942543a609fc5",
		"c540a884371b1baf6faf504351140d8a4548c3dbb0cbc59eb623d72b91654c25"},
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
