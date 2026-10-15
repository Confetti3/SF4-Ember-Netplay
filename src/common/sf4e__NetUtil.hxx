#pragma once

#include <cstdint>
#include <functional>
#include <string>

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

    struct HttpPostResult {
        HttpRequestResult request;
        std::string body, retryAfter;
        bool cancelled = false;
    };
    // The report intake (server/ember-reports), fixed at build time: nothing
    // a player types reaches the host or the path.
    constexpr const char* ReportIntakeHost = "embernetplay.link";
    constexpr const wchar_t* ReportIntakePath = L"/report/v1";
    // Reports have their own exact HTTPS host and never follow redirects.
    // `cancelled` is polled while each step waits and ends the upload within
    // a moment, so closing a window never waits on it.
    HttpPostResult HttpPostReport(const char* host, const std::string& contentType,
        const std::string& body, const std::function<bool()>& cancelled);
    namespace testing {
    // HttpPostReport over plain HTTP to 127.0.0.1:`port`, for tests with a
    // local server. Loopback only, so nothing can leave the PC this way.
    HttpPostResult HttpPostReportLoopback(int port, const std::string& contentType,
        const std::string& body, const std::function<bool()>& cancelled);
    }

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
