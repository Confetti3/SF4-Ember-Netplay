#include "../common/FrameRateWindow.hxx"
#include <cmath>

#include "test_support.hxx"
using sf4e::pacing::FrameRateSummary;
using sf4e::pacing::FrameRateWindow;

static bool Near(double a, double b) { return std::fabs(a - b) < 1e-9; }

// A minute of steady frames ends in one summary, on the frame that fills it.
static void AFullMinuteIsSummedOnce() {
	FrameRateWindow window;
	FrameRateSummary summary;
	for (int i = 1; i < 3000; ++i) CHECK(!window.Add(20.0, 20.0, summary));
	CHECK(window.Add(20.0, 20.0, summary));
	CHECK(summary.frames == 3000 && summary.late == 0);
	CHECK(Near(summary.seconds, 60.0) && Near(summary.fps, 50.0));
	CHECK(Near(summary.lateMs, 30.0) && Near(summary.longestMs, 20.0));
}

// Late is half a period over; the longest frame is kept; the next window
// starts empty.
static void LateFramesAndTheLongestAreCounted() {
	FrameRateWindow window;
	FrameRateSummary summary;
	const double period = 1000.0 / 60.0;
	CHECK(!window.Add(period * 1.5, period, summary));  // on the line: not late
	CHECK(!window.Add(30.0, period, summary));
	CHECK(!window.Add(45.0, period, summary));
	CHECK(window.Add(FrameRateWindow::WindowMs, period, summary));
	CHECK(summary.frames == 4 && summary.late == 3 && Near(summary.longestMs, FrameRateWindow::WindowMs));
	CHECK(Near(summary.lateMs, 25.0) && Near(summary.seconds, 60.1) && Near(summary.fps, 4000.0 / 60100.0));
	CHECK(window.Add(FrameRateWindow::WindowMs, period, summary));
	CHECK(summary.frames == 1 && summary.late == 1);
}

// With no period from the game, late is measured against 60 fps.
static void NoPeriodMeansSixtyFps() {
	FrameRateWindow window;
	FrameRateSummary summary;
	CHECK(!window.Add(26.0, 0.0, summary));
	CHECK(window.Add(FrameRateWindow::WindowMs, 0.0, summary));
	CHECK(summary.late == 2 && Near(summary.lateMs, 25.0));
}

int main() {
	AFullMinuteIsSummedOnce();
	LateFramesAndTheLongestAreCounted();
	NoPeriodMeansSixtyFps();
	return 0;
}
