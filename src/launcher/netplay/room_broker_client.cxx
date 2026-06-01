#include "room_broker_client.hxx"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/sf4e__NetUtil.hxx"

namespace sf4e {
namespace launcher {

	namespace {

		static bool AllowLocalBrokerHost() {
			const char* env = getenv("SF4E_ALLOW_LOCAL_BROKER");
			return env && (env[0] == '1' || _stricmp(env, "true") == 0);
		}

		static bool AllowHttpBroker() {
			const char* env = getenv("SF4E_ALLOW_HTTP_BROKER");
			return env && (env[0] == '1' || _stricmp(env, "true") == 0);
		}

		static bool IsBlockedBrokerHostLiteral(const char* host) {
			if (!host || !host[0]) {
				return true;
			}
			if (_stricmp(host, "localhost") == 0) {
				return !AllowLocalBrokerHost();
			}
			if (_stricmp(host, "metadata.google.internal") == 0
				|| _stricmp(host, "metadata.goog") == 0) {
				return true;
			}

			unsigned a = 0;
			unsigned b = 0;
			unsigned c = 0;
			unsigned d = 0;
			if (sscanf_s(host, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
				return false;
			}

			if (a == 127) {
				return !AllowLocalBrokerHost();
			}
			if (a == 10) {
				return true;
			}
			if (a == 172 && b >= 16 && b <= 31) {
				return true;
			}
			if (a == 192 && b == 168) {
				return true;
			}
			if (a == 169 && b == 254) {
				return true;
			}
			if (a == 100 && b >= 64 && b <= 127) {
				return true;
			}
			if (a == 0) {
				return true;
			}
			return false;
		}

	} // namespace

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
		else {
			return false;
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
			if (out.port < 1 || out.port > 65535) {
				return false;
			}
		}
		strncpy_s(out.host, hostPort, _TRUNCATE);
		if (out.host[0] == 0) {
			return false;
		}
		if (IsBlockedBrokerHostLiteral(out.host)) {
			return false;
		}
		if (!out.https && !AllowHttpBroker()) {
			return false;
		}
		return true;
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
