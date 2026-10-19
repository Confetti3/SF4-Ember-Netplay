#include "../common/GameDisplayConfig.hxx"
#include <cstdio>
#include <functional>
#include <cstdlib>
#include <initializer_list>

#include "test_support.hxx"
using sf4e::gameconfig::ParseDisplaySettings;
using sf4e::gameconfig::RenderingThreadOverride;
using sf4e::gameconfig::BorderlessOverride;
using sf4e::gameconfig::DisplayConfigOverrides;
using sf4e::gameconfig::DisplayMode;
using sf4e::gameconfig::Fit16By9;

// Main's display block in the tests: the config write puts modes here.
struct FakeMain {
	DisplayMode mode;
	void operator()(const DisplayMode& put) { mode = put; }
};

// Faster display on: the game's live flag is kept off whatever config.ini
// asked, and the game's config write sees the player's own value, then off again.
static void FasterDisplayKeepsTheFlagOffButWritesItAsAsked() {
	for (const int asked : {1, 0}) {
		DisplayConfigOverrides overrides;
		RenderingThreadOverride& override_ = overrides.renderingThread;
		int live = asked;
		override_.Apply(true, live);
		CHECK(live == 0 && override_.Applied() && !override_.RenderThread());
		int written = -1;
		FakeMain main;
		overrides.WriteRequested(live, main.mode, std::ref(main), [&] { written = live; });
		CHECK(written == asked);
		CHECK(live == 0);
		CHECK(override_.Describe() == std::string("rendering thread forced off (Faster display on; game setting asked for ") + (asked ? "ON" : "OFF") + ")");
	}
}

// Faster display off: the flag and its write are the game's alone.
static void FasterDisplayOffLeavesTheFlagToTheGame() {
	for (const int asked : {1, 0}) {
		DisplayConfigOverrides overrides;
		RenderingThreadOverride& override_ = overrides.renderingThread;
		int live = asked;
		override_.Apply(false, live);
		CHECK(live == asked && override_.Applied() && override_.RenderThread() == (asked != 0));
		int written = -1;
		FakeMain main;
		overrides.WriteRequested(live, main.mode, std::ref(main), [&] { written = live; });
		CHECK(written == asked && live == asked);
		CHECK(override_.Describe() == std::string("rendering thread left to the game (Faster display off; RenderingThread=") + (asked ? "ON" : "OFF") + ")");
	}
}

// A write before Apply is the game's own and untouched.
static void RenderingThreadWriteBeforeApplyIsTheGames() {
	const DisplayConfigOverrides overrides;
	CHECK(!overrides.renderingThread.Applied());
	int live = 1, written = -1;
	FakeMain main;
	overrides.WriteRequested(live, main.mode, std::ref(main), [&] { written = live; });
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
			DisplayConfigOverrides overrides;
			overrides.renderingThread.ApplyThenStart(faster, game.startupFlag, [&] { return game.ApplyStartupParams(); });
			const int expected = faster ? 0 : asked;
			for (int i = 0; i < 3; ++i) {
				int written = -1;
				FakeMain main;
				overrides.WriteRequested(game.mainFlag, main.mode, std::ref(main), [&] { written = game.mainFlag; });
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

// Borderless renders a whole 16:9 multiple, centred, with bars on a wider or
// taller monitor and none on a 16:9 one.
static void BorderlessAreaIsTheLargestWhole16By9() {
	const auto full = Fit16By9(1920, 1080);
	CHECK(full.x == 0 && full.y == 0 && full.width == 1920 && full.height == 1080);
	const auto wide = Fit16By9(3440, 1440);
	CHECK(wide.x == 440 && wide.y == 0 && wide.width == 2560 && wide.height == 1440);
	const auto tall = Fit16By9(1920, 1200);
	CHECK(tall.x == 0 && tall.y == 60 && tall.width == 1920 && tall.height == 1080);
	CHECK(!Fit16By9(15, 9).Valid() && !Fit16By9(0, 0).Valid());
}

static bool Holds(const DisplayMode& mode, uint32_t width, uint32_t height, uint32_t fullScreen) {
	return mode == DisplayMode{width, height, fullScreen};
}

// Borderless builds the device windowed at its monitor's size, Main's copy
// with it, and the game's config write sees the player's mode, then the
// Borderless one again.
static void BorderlessWritesThePlayersModeAsAsked() {
	DisplayConfigOverrides overrides;
	FakeMain main;
	main.mode = overrides.borderless.Apply({1280, 720, 1}, 3440, 1440);
	CHECK(Holds(main.mode, 3440, 1440, 0));
	// Each Reset builds the parameters again; the player's mode is kept.
	main.mode = overrides.borderless.Apply(main.mode, 3440, 1440);
	DisplayMode written;
	int flag = 0;
	overrides.WriteRequested(flag, main.mode, std::ref(main), [&] { written = main.mode; });
	CHECK(Holds(written, 1280, 720, 1) && Holds(main.mode, 3440, 1440, 0));
}

// A mode the game's display options put in the device is the player's new
// choice: it is saved, and the device is built at the monitor's size again.
static void BorderlessSavesAModeChosenInTheGamesOptions() {
	DisplayConfigOverrides overrides;
	overrides.borderless.Apply({1280, 720, 0}, 1920, 1080);
	FakeMain main;
	main.mode = overrides.borderless.Apply({1600, 900, 1}, 1920, 1080);
	CHECK(Holds(main.mode, 1920, 1080, 0));
	DisplayMode written;
	int flag = 0;
	overrides.WriteRequested(flag, main.mode, std::ref(main), [&] { written = main.mode; });
	CHECK(Holds(written, 1600, 900, 1));
}

// When Borderless gives up, both copies get the player's mode back once and
// later writes are the game's alone, as they are without Borderless.
static void BorderlessRestoreHandsTheModeBack() {
	DisplayConfigOverrides overrides;
	CHECK(!overrides.borderless.Restore());
	overrides.borderless.Apply({1280, 720, 1}, 1920, 1080);
	const auto player = overrides.borderless.Restore();
	CHECK(player && Holds(*player, 1280, 720, 1));
	CHECK(!overrides.borderless.Restore());
	FakeMain main;
	main.mode = {1600, 900, 0};
	DisplayMode written;
	int flag = 0;
	overrides.WriteRequested(flag, main.mode, std::ref(main), [&] { written = main.mode; });
	CHECK(Holds(written, 1600, 900, 0));
}

// Both overrides at once: the write sees the player's flag and mode
// together, and both live values are back after it.
static void OneWriteGivesBothOverridesThePlayersValues() {
	DisplayConfigOverrides overrides;
	int flag = 1;
	overrides.renderingThread.Apply(true, flag);
	FakeMain main;
	main.mode = overrides.borderless.Apply({1280, 720, 1}, 1920, 1080);
	int writtenFlag = -1;
	DisplayMode writtenMode;
	overrides.WriteRequested(flag, main.mode, std::ref(main), [&] { writtenFlag = flag; writtenMode = main.mode; });
	CHECK(writtenFlag == 1 && Holds(writtenMode, 1280, 720, 1));
	CHECK(flag == 0 && Holds(main.mode, 1920, 1080, 0));
}

int main() {
	BorderlessAreaIsTheLargestWhole16By9();
	BorderlessWritesThePlayersModeAsAsked();
	BorderlessSavesAModeChosenInTheGamesOptions();
	BorderlessRestoreHandsTheModeBack();
	OneWriteGivesBothOverridesThePlayersValues();
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
