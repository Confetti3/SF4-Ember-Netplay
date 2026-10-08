#pragma once

// Shared by the tests of the game's file readers: little-endian writes into a
// byte vector, and a read from a heap block of exactly the given size, so a
// sanitizer build sees any read past the end.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using Bytes = std::vector<std::uint8_t>;

inline void Put16(Bytes& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}
inline void Put32(Bytes& out, std::uint32_t value) { Put16(out, value); Put16(out, value >> 16); }
inline void Set32(Bytes& bytes, std::size_t at, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) bytes[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
}
// Reads with the Read of the model's own namespace (bcm, bac or clg).
template <class Model> bool ReadExact(const Bytes& bytes, std::size_t size, Model& model, std::string& error) {
    std::unique_ptr<std::uint8_t[]> exact(new std::uint8_t[size ? size : 1]);
    if (size) std::memcpy(exact.get(), bytes.data(), size);
    return Read(exact.get(), size, model, error);
}
