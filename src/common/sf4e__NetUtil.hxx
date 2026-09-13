#pragma once

#include <cstdint>
#include <functional>

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
		WriteFailed,
	};

	struct HttpRequestResult {
		bool ok = false;
		HttpErrorKind error = HttpErrorKind::None;
		int statusCode = 0;
		unsigned long win32Error = 0;
	};

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
	bool HttpDownloadUrlUtf8(
		const char* url,
		const wchar_t* destPath,
		int timeoutMs = 120000,
		const char* extraHeaders = nullptr,
		HttpRequestResult* outResult = nullptr,
        const std::function<bool(std::uint64_t, std::uint64_t)>& progress = {}
	);

} // namespace sf4e
