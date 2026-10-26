#pragma once
// The frame between the game and its helper, the same on every platform: a
// big-endian length of 10 plus the payload, the version bytes 0 1, a
// big-endian message id, then the payload. HelperClient.cxx (the helper's
// named pipe) and HelperClientPosix.cxx (its stdin and stdout) share it.
#include <cstddef>
#include <cstdint>
#include <limits>

namespace sf4e { namespace platform { namespace helper_frame {

const uint32_t MaxPayload = 512 * 1024;
const size_t MaxMessages = 128;
const size_t MaxQueuedBytes = 8 * 1024 * 1024;
// A frame that has started moves within this, or the client fails.
const uint32_t IoTimeoutMs = 15000;
const uint32_t HeaderSize = 14;

inline void EncodeHeader(uint64_t id, size_t payloadSize, uint8_t (&header)[HeaderSize]) {
    const uint32_t length = static_cast<uint32_t>(10 + payloadSize);
    for (int i = 0; i < 4; ++i) { header[i] = static_cast<uint8_t>(length >> (24 - 8 * i)); }
    header[4] = 0; header[5] = 1;
    for (int i = 0; i < 8; ++i) { header[6 + i] = static_cast<uint8_t>(id >> (56 - 8 * i)); }
}

// False for a header no helper sends: an empty or oversized payload, another
// version, or an id of 0 or past the largest signed 64-bit value.
inline bool DecodeHeader(const uint8_t (&header)[HeaderSize], uint64_t& id, uint32_t& payloadSize) {
    uint32_t length = 0;
    for (int i = 0; i < 4; ++i) { length = (length << 8) | header[i]; }
    if (length <= 10 || length > MaxPayload + 10 || header[4] != 0 || header[5] != 1) { return false; }
    id = 0;
    for (int i = 0; i < 8; ++i) { id = (id << 8) | header[6 + i]; }
    if (id == 0 || id > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) { return false; }
    payloadSize = length - 10;
    return true;
}

} } }
