#pragma once

#include <cstdint>

#include "../common/sf4e__NetUtil.hxx"

namespace sf4e {
namespace launcher {

	struct BrokerUrlParts {
		char host[256] = { 0 };
		int port = 80;
		bool https = false;
		char pathPrefix[128] = "/";
	};

	bool ParseBrokerBaseUrl(const char* baseUrl, BrokerUrlParts& out);

	bool BrokerHttpGet(
		const BrokerUrlParts& parts,
		const char* path,
		char* outBody,
		int outBodyLen,
		int timeoutMs = 8000,
		sf4e::HttpRequestResult* outResult = nullptr
	);
	bool BrokerHttpPostJson(
		const BrokerUrlParts& parts,
		const char* path,
		const char* jsonBody,
		char* outBody,
		int outBodyLen,
		int timeoutMs = 8000,
		sf4e::HttpRequestResult* outResult = nullptr
	);

} // namespace launcher
} // namespace sf4e
