#include "room_broker_client.hxx"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/sf4e__NetUtil.hxx"

namespace sf4e {
namespace launcher {

	bool ParseBrokerBaseUrl(const char* baseUrl, BrokerUrlParts& out) {
		if (!baseUrl) {
			return false;
		}
		char buf[512];
		strncpy_s(buf, baseUrl, _TRUNCATE);
		sf4e::TrimRoomCodeInPlace(buf);
		if (!buf[0]) {
			return false;
		}

		out.https = false;
		out.port = 80;
		strncpy_s(out.pathPrefix, "/", _TRUNCATE);

		char* p = buf;
		if (_strnicmp(p, "https://", 8) == 0) {
			out.https = true;
			out.port = 443;
			p += 8;
		}
		else if (_strnicmp(p, "http://", 7) == 0) {
			p += 7;
		}

		char* slash = strchr(p, '/');
		char hostPort[256] = { 0 };
		if (slash) {
			strncpy_s(hostPort, p, slash - p);
			strncpy_s(out.pathPrefix, slash, _TRUNCATE);
		}
		else {
			strncpy_s(hostPort, p, _TRUNCATE);
		}

		char* colon = strchr(hostPort, ':');
		if (colon) {
			*colon = 0;
			out.port = atoi(colon + 1);
		}
		strncpy_s(out.host, hostPort, _TRUNCATE);
		return out.host[0] != 0;
	}

	static void JoinBrokerPath(const BrokerUrlParts& parts, const char* suffix, char* outPath, int outPathLen) {
		const char* prefix = parts.pathPrefix[0] ? parts.pathPrefix : "/";
		size_t plen = strlen(prefix);
		bool prefixEndsSlash = plen > 0 && prefix[plen - 1] == '/';
		const char* suf = suffix && suffix[0] == '/' ? suffix + 1 : suffix;
		if (prefixEndsSlash) {
			snprintf(outPath, outPathLen, "%s%s", prefix, suf ? suf : "");
		}
		else {
			snprintf(outPath, outPathLen, "%s/%s", prefix, suf ? suf : "");
		}
	}

	bool BrokerHttpGet(
		const BrokerUrlParts& parts,
		const char* path,
		char* outBody,
		int outBodyLen,
		int timeoutMs,
		sf4e::HttpRequestResult* outResult
	) {
		char fullPath[512];
		JoinBrokerPath(parts, path, fullPath, sizeof(fullPath));
		return sf4e::HttpGetUtf8(parts.host, parts.port, parts.https, fullPath, timeoutMs, outBody, outBodyLen, outResult);
	}

	bool BrokerHttpPostJson(
		const BrokerUrlParts& parts,
		const char* path,
		const char* jsonBody,
		char* outBody,
		int outBodyLen,
		int timeoutMs,
		sf4e::HttpRequestResult* outResult
	) {
		char fullPath[512];
		JoinBrokerPath(parts, path, fullPath, sizeof(fullPath));
		return sf4e::HttpPostJsonUtf8(
			parts.host,
			parts.port,
			parts.https,
			fullPath,
			timeoutMs,
			jsonBody,
			outBody,
			outBodyLen,
			outResult
		);
	}

} // namespace launcher
} // namespace sf4e