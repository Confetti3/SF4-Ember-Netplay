#pragma once

#include <cstdint>

namespace sf4e {
namespace launcher {

	struct BrokerUrlParts {
		char host[256] = { 0 };
		int port = 80;
		bool https = false;
		char pathPrefix[128] = "/";
	};

	bool ParseBrokerBaseUrl(const char* baseUrl, BrokerUrlParts& out);

	bool BrokerHttpGet(const BrokerUrlParts& parts, const char* path, char* outBody, int outBodyLen, int timeoutMs = 8000);
	bool BrokerHttpPostJson(const BrokerUrlParts& parts, const char* path, const char* jsonBody, char* outBody, int outBodyLen, int timeoutMs = 8000);

} // namespace launcher
} // namespace sf4e
