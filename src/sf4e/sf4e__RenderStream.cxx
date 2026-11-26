#include "sf4e__RenderStream.hxx"

#include <atomic>
#include <cstdint>

#include "spdlog/spdlog.h"

#include "../common/BattleBufferEdit.hxx"
#include "../common/RenderStreamCheck.hxx"
#include "sf4e__CodePages.hxx"
#include "sf4e__StartupLog.hxx"

using rBattleBuffer = Dimps::Platform::BattleCommandBuffer;
namespace render = sf4e::render;

namespace {
// The largest merged frame since the last battle close, the frames whose
// unsound ranges were left out in that time, and whether one was logged.
std::atomic<std::uint32_t> s_peakDwords{ 0 }, s_peakRanges{ 0 }, s_cutFrames{ 0 };
std::atomic<bool> s_cutLogged{ false };

void NotePeak(std::atomic<std::uint32_t>& peak, std::uint32_t value) {
	if (value > peak.load(std::memory_order_relaxed)) peak.store(value, std::memory_order_relaxed);
}
} // namespace

void sf4e::RenderStream::Install() {
	sf4e::CodePages pages;
	if (render::WidenBattleBuffer(rBattleBuffer::rangesPush, rBattleBuffer::dwordsPush, pages))
		StartupLog::Log(spdlog::level::info, fmt::format("Render stream: battle buffer widened to {:#x} dwords, {:#x} ranges", render::WideDwords, render::WideRanges));
	else
		StartupLog::Log(spdlog::level::warn, "Render stream: battle buffer not widened (other bytes than this game build's, or not writable)");
}

// The merged buffer is made by the merge at exactly the size its sources
// wrote, the table right after the stream, so its pointers are consistent;
// an empty merge leaves all of them null and no ranges. A range left out of
// the walk keeps the resource references its commands hold. executeFrame
// still ends the scene and resets the buffer, which puts the cursor back.
bool sf4e::RenderStream::Check(Dimps::Platform::CommandBuffer* merged) {
	if (!merged) return false;
	const std::uint32_t written = (std::uint32_t)(merged->cursor - merged->base);
	render::Range* table = (render::Range*)merged->tableBase;
	const std::uint32_t ranges = (std::uint32_t)((render::Range*)merged->tableCursor - table);
	NotePeak(s_peakDwords, written);
	NotePeak(s_peakRanges, ranges);
	const render::KeptRanges kept = render::KeepSoundRanges(merged->base, written, table, ranges);
	if (kept.kept == ranges) return true;
	merged->tableCursor = table + kept.kept;
	s_cutFrames.fetch_add(1, std::memory_order_relaxed);
	if (!s_cutLogged.exchange(true)) {
		spdlog::warn("Render stream: {} of {} ranges left out, the first ({}) {} (start dword {:#x}); frame of {} dwords",
			ranges - kept.kept, ranges, kept.firstBad, render::FaultName(kept.fault), kept.badStart, written);
	}
	return true;
}

void sf4e::RenderStream::BattleClosed() {
	const Dimps::Platform::CommandBuffer* battle = rBattleBuffer::instance;
	spdlog::info("Render stream: largest frame {} dwords, {} ranges (battle buffer {:#x} dwords, {:#x} ranges); {} frames with ranges left out",
		s_peakDwords.exchange(0), s_peakRanges.exchange(0), battle->capacityBytes / 4, battle->tableCapacityBytes / 8,
		s_cutFrames.exchange(0));
	s_cutLogged = false;
}
