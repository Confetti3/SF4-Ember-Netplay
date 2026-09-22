#include "../common/GameDisplayConfig.hxx"
#include <cstdio>
#include <cstdlib>

#include "test_support.hxx"
using sf4e::gameconfig::ParseDisplaySettings;

int main() {
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
	return 0;
}
