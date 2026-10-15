#include "../common/GameDisplayConfig.hxx"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#include "test_support.hxx"
using sf4e::gameconfig::ParseDisplaySettings;
using sf4e::gameconfig::RenderingThreadOverride;

// Faster display on: the game's live flag is kept off whatever config.ini
// asked, and the game's config write sees the player's own value, then off again.
static void FasterDisplayKeepsTheFlagOffButWritesItAsAsked() {
	for (const int asked : {1, 0}) {
		RenderingThreadOverride override_;
		int live = asked;
		override_.Apply(true, live);
		CHECK(live == 0 && override_.Applied() && !override_.RenderThread());
		int written = -1;
		override_.WriteRequested(live, [&] { written = live; });
		CHECK(written == asked);
		CHECK(live == 0);
		CHECK(override_.Describe() == std::string("rendering thread forced off (Faster display on; game setting asked for ") + (asked ? "ON" : "OFF") + ")");
	}
}

// Faster display off: the flag and its write are the game's alone.
static void FasterDisplayOffLeavesTheFlagToTheGame() {
	for (const int asked : {1, 0}) {
		RenderingThreadOverride override_;
		int live = asked;
		override_.Apply(false, live);
		CHECK(live == asked && override_.Applied() && override_.RenderThread() == (asked != 0));
		int written = -1;
		override_.WriteRequested(live, [&] { written = live; });
		CHECK(written == asked && live == asked);
		CHECK(override_.Describe() == std::string("rendering thread left to the game (Faster display off; RenderingThread=") + (asked ? "ON" : "OFF") + ")");
	}
}

// A write before Apply is the game's own and untouched.
static void RenderingThreadWriteBeforeApplyIsTheGames() {
	const RenderingThreadOverride override_;
	CHECK(!override_.Applied());
	int live = 1, written = -1;
	override_.WriteRequested(live, [&] { written = live; });
	CHECK(written == 1 && live == 1);
}

// A stand-in for the game's side of the flag. The startup parameters hold the
// player's config.ini value; the startup routine copies it into Main and the
// graphics device latches the same value into its release policy. A policy
// that disagrees with Main's flag later is the crash: releases queue forever
// and the texture pool fills.
struct FakeNative {
	int startupFlag = 0;
	int mainFlag = -1;
	int devicePolicy = -1;
	int calls = 0;
	int seenAtStart = -1;

	// The game's startup routine: reads only the parameters.
	int ApplyStartupParams() {
		++calls;
		seenAtStart = startupFlag;
		mainFlag = startupFlag;
		devicePolicy = mainFlag;
		return 7;
	}
	// The game's deferred-release pass; false is the crash condition.
	bool Drain() const { return devicePolicy == mainFlag; }
};

// Faster display and the player's value, both ways each: the startup routine
// observes the overridden parameter, and Main and the device end up with the
// same effective value before it returns.
static void StartupOrderLatchesTheSameValueInMainAndDevice() {
	for (const bool faster : {true, false}) {
		for (const int asked : {1, 0}) {
			FakeNative game;
			game.startupFlag = asked;
			RenderingThreadOverride override_;
			const int expected = faster ? 0 : asked;
			const int result = override_.ApplyThenStart(faster, game.startupFlag, [&] { return game.ApplyStartupParams(); });
			CHECK(result == 7 && game.calls == 1);
			CHECK(game.seenAtStart == expected);
			CHECK(game.mainFlag == expected);
			CHECK(game.devicePolicy == expected);
			CHECK(game.Drain());
			CHECK(override_.Applied() && override_.RenderThread() == (expected != 0));
		}
	}
}

// Repeated config writes: each sees the player's own value, and afterwards
// Main's effective flag is back, still agreeing with the device.
static void RepeatedConfigWritesRestoreTheEffectiveFlag() {
	for (const bool faster : {true, false}) {
		for (const int asked : {1, 0}) {
			FakeNative game;
			game.startupFlag = asked;
			RenderingThreadOverride override_;
			override_.ApplyThenStart(faster, game.startupFlag, [&] { return game.ApplyStartupParams(); });
			const int expected = faster ? 0 : asked;
			for (int i = 0; i < 3; ++i) {
				int written = -1;
				override_.WriteRequested(game.mainFlag, [&] { written = game.mainFlag; });
				CHECK(written == asked);
				CHECK(game.mainFlag == expected);
				CHECK(game.devicePolicy == expected);
				CHECK(game.Drain());
			}
		}
	}
}

// The fake must be able to tell the old order apart: clearing Main's flag
// after the routine has latched the device leaves them disagreeing.
static void ClearingMainAfterStartupLeavesTheDeviceLatchedOn() {
	FakeNative game;
	game.startupFlag = 1;
	game.ApplyStartupParams();
	game.mainFlag = 0;
	CHECK(!game.Drain());
}

int main() {
	FasterDisplayKeepsTheFlagOffButWritesItAsAsked();
	FasterDisplayOffLeavesTheFlagToTheGame();
	RenderingThreadWriteBeforeApplyIsTheGames();
	StartupOrderLatchesTheSameValueInMainAndDevice();
	RepeatedConfigWritesRestoreTheEffectiveFlag();
	ClearingMainAfterStartupLeavesTheDeviceLatchedOn();
	// The file as the game writes it, CRLF included.
	const auto good = ParseDisplaySettings("[DISPLAY]\r\nRefreshRate=240Hz\r\nVSync=OFF\r\nFrameRate=FIXED\r\n[GRAPHICS]\r\nMSAA=NONE\r\n");
	CHECK(good.Recommended() && good.frameRate == "FIXED");
	const auto bad = ParseDisplaySettings("FrameRate = smooth\nMSAA=4X");
	CHECK(!bad.FrameRateOk() && !bad.MsaaOk());
	CHECK(bad.frameRate == "SMOOTH" && bad.msaa == "4X");
	// A missing file or key is not a reason to show the card.
	CHECK(ParseDisplaySettings("").Recommended());
	CHECK(!ParseDisplaySettings("no equals\nMSAA=2x").MsaaOk());
	// VSync is forced off by the mod, so the file's value never matters.
	CHECK(ParseDisplaySettings("VSync=ON").Recommended());
	// The render thread is Faster display's, so no card asks for it either.
	CHECK(ParseDisplaySettings("[SYSTEM]\r\nRenderingThread=ON\r\n").Recommended());
	return 0;
}
