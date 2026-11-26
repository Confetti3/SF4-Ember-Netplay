#pragma once

// The game's render command stream. Its writers check no capacity, and a
// heavy battle frame wrote past the battle buffer's 0x50000 dwords into the
// range table behind them (B182: at most 0x3D1 dwords past, by the merged
// table's offset). The walk then faulted or spun forever on a zero word.
// - Install doubles the battle buffer before the game makes it.
// - Check is the last resort for a frame that still overran: only its unsound
//   ranges are left out (common/RenderStreamCheck.hxx).
// - BattleClosed logs the largest frame since the last close.

#include "../Dimps/Dimps__Platform.hxx"

namespace sf4e {
	namespace RenderStream {
		// At Sidecar's load, before the game's app init.
		void Install();
		// The drawing thread, before the game replays the frame's merged
		// buffer. False: there is no buffer to replay.
		bool Check(Dimps::Platform::CommandBuffer* merged);
		// The game thread, as a battle closes.
		void BattleClosed();
	}
}
