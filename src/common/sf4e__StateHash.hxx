#pragma once

// Canonical semantic state hashing (Phase 6 / desync detection v2).
//
// A tiny, dependency-free 64-bit FNV-1a hasher with an explicit canonical
// encoding: every field is fed byte-by-byte in little-endian order with a
// defined width. Hash input is therefore independent of struct padding,
// object addresses, and host layout — only the semantic values matter.
//
// Deliberately NOT hashed anywhere in this system: raw struct bytes,
// GameMementoKey object bytes, function pointers, object addresses,
// pointer-keyed map keys, allocator metadata, uninitialized memory,
// shallow-copied pointer fields (e.g. inside GameManager), wall-clock
// timing, and log/overlay state.
//
// FNV-1a/64 is not collision-proof, but for desync *diagnostics* (comparing
// two peers' encodings of the same frame) collisions only risk a missed
// detection on a single checkpoint, never a false positive. False positives
// are impossible when encoders match: identical input always hashes equal.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace sf4e {
namespace statehash {

struct Hasher {
	uint64_t h;

	Hasher() : h(14695981039346656037ULL) {}

	void U8(uint8_t v) {
		h ^= (uint64_t)v;
		h *= 1099511628211ULL;
	}
	void U16(uint16_t v) {
		U8((uint8_t)(v & 0xff));
		U8((uint8_t)(v >> 8));
	}
	void U32(uint32_t v) {
		U8((uint8_t)(v & 0xff));
		U8((uint8_t)((v >> 8) & 0xff));
		U8((uint8_t)((v >> 16) & 0xff));
		U8((uint8_t)((v >> 24) & 0xff));
	}
	void U64(uint64_t v) {
		U32((uint32_t)(v & 0xffffffffULL));
		U32((uint32_t)(v >> 32));
	}
	void I16(int16_t v) { U16((uint16_t)v); }
	void I32(int32_t v) { U32((uint32_t)v); }
	// Floats participate as their exact bit pattern. Deterministic
	// simulation must produce bit-identical floats; encoding the pattern
	// (not a rounded value) keeps that property observable.
	void F32(float v) {
		uint32_t bits;
		memcpy(&bits, &v, sizeof(bits));
		U32(bits);
	}
	// A fixed-point value with explicit component widths.
	void Fixed(uint16_t fractional, int16_t integral) {
		U16(fractional);
		I16(integral);
	}

	uint64_t Value() const { return h; }
};

} // namespace statehash
} // namespace sf4e
