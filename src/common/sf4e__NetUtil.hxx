#pragma once

namespace sf4e {

	bool DetectLanIPv4(char* outIp, int outIpLen);

	// Strips leading/trailing whitespace and line endings in place.
	void TrimRoomCodeInPlace(char* buf);

	// HTTPS lookup of public IPv4 (api.ipify.org, then icanhazip.com). timeoutMs per request.
	bool FetchPublicIPv4(char* outIp, int outIpLen, int timeoutMs = 5000);

	bool CopyTextToClipboardUtf8(const char* text);

	// HTTP GET; returns response body (UTF-8). useHttps selects port 443 vs 80.
	bool HttpGetUtf8(const char* host, int port, bool useHttps, const char* path, int timeoutMs, char* outBody, int outBodyLen);

	// HTTP POST with application/json body.
	bool HttpPostJsonUtf8(const char* host, int port, bool useHttps, const char* path, int timeoutMs, const char* jsonBody, char* outBody, int outBodyLen);

} // namespace sf4e
