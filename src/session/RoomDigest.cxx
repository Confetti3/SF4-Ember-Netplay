#include "SessionRecovery.hxx"

#include <windows.h>
#include <bcrypt.h>

namespace sf4e { namespace session { namespace recovery_detail {

std::string Sha256(const std::string& input) {
	// One provider handle for the process; CNG algorithm handles may be used
	// from several threads at once (the checkpoint decode worker hashes too).
	static const BCRYPT_ALG_HANDLE algorithm = [] {
		BCRYPT_ALG_HANDLE handle = nullptr;
		return BCryptOpenAlgorithmProvider(&handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 ? handle : nullptr;
	}();
	unsigned char digest[32] = {};
	if (!algorithm || input.size() > (std::numeric_limits<ULONG>::max)() ||
		BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),
			static_cast<ULONG>(input.size()), digest, sizeof(digest)) < 0) return Sha256Portable(input);
	static constexpr char hex[] = "0123456789abcdef";
	std::string result; result.reserve(64);
	for (const auto value : digest) { result.push_back(hex[value >> 4]); result.push_back(hex[value & 0xfU]); }
	return result;
}

} } }
