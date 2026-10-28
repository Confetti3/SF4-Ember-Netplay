#pragma once
// The frame between the game and its helper, the same on every platform: a
// big-endian length of 10 plus the payload, the version bytes 0 1, a
// big-endian message id, then the payload. HelperClient.cxx (the helper's
// named pipe) and HelperClientPosix.cxx (its stdin and stdout) share it.
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

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
    if (id == 0 || id > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)())) { return false; }
    payloadSize = length - 10;
    return true;
}

// The IPC contract's own version (wire::IPC_VERSION). The 0 1 in every frame
// header is the version shared with QUIC control frames and game packets and
// never changes; this one does. The helper's answer to the authentication
// frame is "SF4N" and this version, and a room opens only on a helper whose
// answer carried it, so a game and helper from different builds fail closed.
const uint16_t IpcVersion = 2;
inline std::string AuthenticationAnswer() {
    std::string answer = "SF4N";
    answer.push_back(static_cast<char>(IpcVersion >> 8));
    answer.push_back(static_cast<char>(IpcVersion & 0xff));
    return answer;
}
// The IPC version an authentication answer carries: 0 for a bare "SF4N",
// which is what a helper from before the version and the Discord companion
// send; false for anything else.
inline bool ReadAuthenticationAnswer(const std::string& answer, uint16_t& version) {
    if (answer.size() != 4 && answer.size() != 6) { return false; }
    if (answer.compare(0, 4, "SF4N") != 0) { return false; }
    version = answer.size() == 4 ? 0 : static_cast<uint16_t>(
        (static_cast<uint8_t>(answer[4]) << 8) | static_cast<uint8_t>(answer[5]));
    return true;
}

// A payload that starts with RawBody carries a JSON header and a raw body: the
// byte RawBody, the header's size as a big-endian u32, the header (a JSON
// object without the body's field), then at least one byte of body. A JSON
// payload starts with '{'. Checkpoint chunks carry their bytes this way
// ("data"), and room messages and sends their text ("payload"), so neither is
// base64 encoded nor escaped into a JSON string.
const char RawBody = 0;
const size_t RawPrefix = 1 + 4;
// For Raw, the header is `headerSize` bytes from RawPrefix and the body is
// everything after it. Kept to C++14, which the helper platform builds with.
enum class PayloadKind { Json, Raw, Invalid };
inline PayloadKind SplitPayload(const std::string& payload, size_t& headerSize) {
    if (payload.empty() || payload[0] != RawBody) { return PayloadKind::Json; }
    if (payload.size() < RawPrefix) { return PayloadKind::Invalid; }
    uint32_t size = 0;
    for (size_t i = 1; i < RawPrefix; ++i) { size = (size << 8) | static_cast<uint8_t>(payload[i]); }
    if (size < 2 || size >= payload.size() - RawPrefix || payload[RawPrefix] != '{') { return PayloadKind::Invalid; }
    headerSize = size;
    return PayloadKind::Raw;
}
// Empty when the header is not an object, the body is empty or the frame
// would pass MaxPayload; HelperClient::Send refuses an empty payload.
inline std::string EncodeRawBody(const std::string& header, const char* body, size_t size) {
    std::string payload;
    const size_t limit = MaxPayload;
    if (header.size() < 2 || header[0] != '{' || !size || size > limit || header.size() > limit - size ||
        RawPrefix > limit - size - header.size()) { return payload; }
    const uint32_t headerSize = static_cast<uint32_t>(header.size());
    payload.reserve(RawPrefix + header.size() + size);
    payload.push_back(RawBody);
    for (int i = 0; i < 4; ++i) { payload.push_back(static_cast<char>(headerSize >> (24 - 8 * i))); }
    payload.append(header);
    payload.append(body, size);
    return payload;
}

} } }
