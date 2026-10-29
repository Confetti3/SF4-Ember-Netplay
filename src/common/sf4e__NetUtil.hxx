#pragma once

#include <chrono>
#include <cstddef>
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

    struct HttpResponse {
        HttpRequestResult request;
        std::string body, retryAfter;
        bool cancelled = false;
    };
    // The report intake (server/ember-reports), fixed at build time: nothing
    // a player types reaches the host or the path.
    constexpr const char* ReportIntakeHost = "embernetplay.link";
    constexpr const char* ReportIntakePath = "/report/v1";
    // Reports have their own exact HTTPS host and never follow redirects.
    // `cancelled` is polled while each step waits and ends the upload within
    // a moment, so closing a window never waits on it.
    HttpResponse HttpPostReport(const char* host, const std::string& contentType,
        const std::string& body, const std::function<bool()>& cancelled);
    namespace testing {
    // HttpPostReport over plain HTTP to 127.0.0.1:`port`, for tests with a
    // local server. Loopback only, so nothing can leave the PC this way.
    HttpResponse HttpPostReportLoopback(int port, const std::string& contentType,
        const std::string& body, const std::function<bool()>& cancelled);
    }

	// One HTTP request to host:port, finished within `budget` however slowly the
	// server answers: `cancelled` is polled while each step waits and ends the
	// request at once. Redirects are never followed and cookies and credentials
	// are never sent. `headers` is CRLF-separated UTF-8, e.g. "Accept: text/html\r\n".
	// `body` is sent with the request unless empty. The answer is read up to
	// `maxResponse` bytes. request.ok means a complete answer of any status (see
	// request.statusCode) whose body is within the cap and as long as its
	// Content-Length says; a truncated, oversized or interrupted one is not ok.
	HttpResponse HttpBoundedRequest(
		const char* verb,
		const char* host,
		int port,
		bool https,
		const char* path,
		const std::string& headers,
		const std::string& body,
		std::size_t maxResponse,
		std::chrono::milliseconds budget,
		const std::function<bool()>& cancelled
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
