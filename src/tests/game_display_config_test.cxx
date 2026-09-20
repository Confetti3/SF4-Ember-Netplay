#include "../common/GameDisplayConfig.hxx"
#include <cstdio>
#include <cstdlib>

#define CHECK(c) do { if (!(c)) { std::printf("Failed line %d: %s\n", __LINE__, #c); std::exit(1); } } while (false)
using sf4e::gameconfig::ParseDisplaySettings;

int main() {
	// The file as the game writes it, CRLF included.
	const auto good = ParseDisplaySettings("[DISPLAY]\r\nRefreshRate=240Hz\r\nVSync=OFF\r\nFrameRate=FIXED\r\n[GRAPHICS]\r\nMSAA=NONE\r\n");
	CHECK(good.Recommended() && good.frameRate == "FIXED");
	const auto bad = ParseDisplaySettings("VSync = on\nFrameRate=SMOOTH\nMSAA=4X");
	CHECK(!bad.FrameRateOk() && !bad.VSyncOk() && !bad.MsaaOk());
	CHECK(bad.frameRate == "SMOOTH" && bad.vsync == "ON" && bad.msaa == "4X");
	// A missing file or key is not a reason to show the card.
	CHECK(ParseDisplaySettings("").Recommended());
	CHECK(!ParseDisplaySettings("no equals\nVSync=ON").VSyncOk());
	return 0;
}
