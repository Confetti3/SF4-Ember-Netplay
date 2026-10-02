#pragma once
#include <string>

namespace sf4e {
// Lowercase hex of a byte container, two digits a byte: room IDs and the
// like as the helper and the bridge write them.
template <class Bytes>
std::string HexLower(const Bytes& bytes) {
	static constexpr char digits[] = "0123456789abcdef";
	std::string text;
	text.reserve(bytes.size() * 2);
	for (const auto value : bytes) {
		const auto byte = static_cast<unsigned char>(value);
		text += digits[byte >> 4];
		text += digits[byte & 15];
	}
	return text;
}
}
