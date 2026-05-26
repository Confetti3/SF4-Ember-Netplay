#pragma once

namespace sf4e {

	enum class HttpErrorKind {
		None = 0,
		InvalidArgs,
		OpenFailed,
		ConnectFailed,
		Timeout,
		SendFailed,
		ReceiveFailed,
		HttpStatus,
		EmptyBody,
	};

	struct HttpRequestResult {
		bool ok = false;
		HttpErrorKind error = HttpErrorKind::None;
		int statusCode = 0;
	};

	bool DetectLanIPv4(char* outIp, int outIpLen);

	// Strips leading/trailing whitespace and line endings in place.
	void TrimRoomCodeInPlace(char* buf);

	// HTTPS lookup of public IPv4 (api.ipify.org, then icanhazip.com). timeoutMs per request.
	bool FetchPublicIPv4(char* outIp, int outIpLen, int timeoutMs = 5000);

	bool CopyTextToClipboardUtf8(const char* text);

	// HTTP GET; returns response body (UTF-8). useHttps selects port 443 vs 80.
	bool HttpGetUtf8(
		const char* host,
		int port,
		bool useHttps,
		const char* path,
		int timeoutMs,
		char* outBody,
		int outBodyLen,
		HttpRequestResult* outResult = nullptr
	);

	// HTTP POST with application/json body.
	bool HttpPostJsonUtf8(
		const char* host,
		int port,
		bool useHttps,
		const char* path,
		int timeoutMs,
		const char* jsonBody,
		char* outBody,
		int outBodyLen,
		HttpRequestResult* outResult = nullptr
	);

	// HTTPS GET with optional request headers (UTF-8). headers is CRLF-separated, e.g. "Accept: application/json\r\n".
	bool HttpGetUtf8WithHeaders(
		const char* host,
		int port,
		bool useHttps,
		const char* path,
		int timeoutMs,
		const char* extraHeaders,
		char* outBody,
		int outBodyLen
	);

	// Download https?://host/path to a local file. Follows redirects.
	bool HttpDownloadUrlUtf8(const char* url, const wchar_t* destPath, int timeoutMs = 120000);

} // namespace sf4e
