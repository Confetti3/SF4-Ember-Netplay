#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif


#include <windows.h>
#include <winhttp.h>

#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <chrono>



#pragma comment(lib, "winhttp.lib")

#include "sf4e__NetUtil.hxx"
#include "AsyncHttpRequest.hxx"

namespace sf4e {

    static HINTERNET OpenHttpSession(int timeoutMs, DWORD flags = 0) {
        HINTERNET session = WinHttpOpen(L"sf4e-updater/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, flags);
        if (session) WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
        return session;
    }

	static void Utf8ToWide(const char* utf8, wchar_t* out, int outChars) {
		if (!utf8 || !out || outChars <= 0) {
			if (out && outChars > 0) {
				out[0] = 0;
			}
			return;
		}
		MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, outChars);
	}

	static bool ParseHttpUrl(const char* url, char* outHost, int outHostLen, char* outPath, int outPathLen, bool& outHttps, int& outPort) {
		if (!url || !outHost || !outPath) {
			return false;
		}
		outHost[0] = '\0';
		outPath[0] = '\0';
		outHttps = true;
		outPort = 443;

		const char* p = url;
		if (strncmp(p, "https://", 8) == 0) {
			p += 8;
			outHttps = true;
			outPort = 443;
		}
		else if (strncmp(p, "http://", 7) == 0) {
			p += 7;
			outHttps = false;
			outPort = 80;
		}
		else {
			return false;
		}

		const char* slash = strchr(p, '/');
		const char* colon = strchr(p, ':');
		if (slash && colon && colon < slash) {
			size_t hostLen = (size_t)(colon - p);
			if (hostLen >= (size_t)outHostLen) {
				return false;
			}
			memcpy(outHost, p, hostLen);
			outHost[hostLen] = '\0';
			outPort = atoi(colon + 1);
			strncpy_s(outPath, outPathLen, slash, _TRUNCATE);
			return true;
		}

		if (slash) {
			size_t hostLen = (size_t)(slash - p);
			if (hostLen >= (size_t)outHostLen) {
				return false;
			}
			memcpy(outHost, p, hostLen);
			outHost[hostLen] = '\0';
			strncpy_s(outPath, outPathLen, slash, _TRUNCATE);
			return true;
		}

		strncpy_s(outHost, outHostLen, p, _TRUNCATE);
		strcpy_s(outPath, outPathLen, "/");
		return true;
	}

	static void ApplyWinHttpDownloadOptions(HINTERNET hSession, HINTERNET hRequest) {
		// Redirects stay on HTTPS: GitHub hands a release asset to its storage
		// host over HTTPS, and nothing a download needs is served over HTTP.
		if (hSession) {
			DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
			WinHttpSetOption(hSession, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));
			DWORD maxRedirects = 10;
			WinHttpSetOption(hSession, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &maxRedirects, sizeof(maxRedirects));
		}
		if (hRequest) {
			DWORD reqRedirect = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
			WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &reqRedirect, sizeof(reqRedirect));
			DWORD secureProtocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
			secureProtocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
			WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURE_PROTOCOLS, &secureProtocols, sizeof(secureProtocols));
		}
	}

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
	) {
		HttpResponse result;
		if (!verb || !host || !path || port <= 0 || port > 65535 || headers.size() >= 256) {
			result.request.error = HttpErrorKind::InvalidArgs;
			return result;
		}
		if (cancelled && cancelled()) {
			result.cancelled = true;
			return result;
		}
		wchar_t wVerb[16] = { 0 };
		wchar_t wHost[256] = { 0 };
		wchar_t wPath[2048] = { 0 };
		wchar_t wHeaders[256] = { 0 };
		Utf8ToWide(verb, wVerb, 16);
		Utf8ToWide(host, wHost, 256);
		Utf8ToWide(path, wPath, 2048);
		Utf8ToWide(headers.c_str(), wHeaders, 256);

		struct Internet {
			HINTERNET value;
			~Internet() { if (value) WinHttpCloseHandle(value); }
		};
		// The budget covers the whole request; this only keeps a step from
		// waiting longer than it needs to.
		const int stepTimeoutMs = static_cast<int>((std::min<long long>)(budget.count(), 20000));
		Internet session{ OpenHttpSession(stepTimeoutMs, WINHTTP_FLAG_ASYNC) };
		if (!session.value) {
			result.request.error = HttpErrorKind::OpenFailed;
			return result;
		}
		ApplyWinHttpDownloadOptions(session.value, nullptr);
		Internet connection{ WinHttpConnect(session.value, wHost, static_cast<INTERNET_PORT>(port), 0) };
		if (!connection.value) {
			result.request.error = HttpErrorKind::ConnectFailed;
			return result;
		}
		HINTERNET request = WinHttpOpenRequest(connection.value, wVerb, wPath, nullptr,
			WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
		if (!request) {
			result.request.error = HttpErrorKind::OpenFailed;
			return result;
		}
		ApplyWinHttpDownloadOptions(nullptr, request);
		DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
		DWORD disable = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
		if (!WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects)) ||
			!WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable))) {
			result.request.error = HttpErrorKind::OpenFailed;
			WinHttpCloseHandle(request);
			return result;
		}
		detail::AsyncHttpRequest transport(request, {}, budget);
		request = nullptr; // ownership and all subsequent request use belong to the coordinator
		if (!transport.Initialize()) {
			result.request.error = HttpErrorKind::OpenFailed;
			return result;
		}

		const auto fail = [&](HttpErrorKind kind, DWORD immediateError = 0) {
			const DWORD error = immediateError ? immediateError : transport.Error();
			result.request.error = error == ERROR_WINHTTP_TIMEOUT ? HttpErrorKind::Timeout : kind;
			result.request.win32Error = error;
			return false;
		};
		const auto exchange = [&] {
			if (!transport.Send(wHeaders, static_cast<DWORD>(body.size()), cancelled)) {
				return fail(HttpErrorKind::SendFailed);
			}
			for (std::size_t offset = 0; offset < body.size();) {
				DWORD written = 0;
				const DWORD count = static_cast<DWORD>((std::min)(std::size_t(65536), body.size() - offset));
				if (!transport.Write(body.data() + offset, count, cancelled) || !(written = transport.Bytes())) {
					return fail(HttpErrorKind::SendFailed);
				}
				offset += written;
			}
			if (!transport.Receive(cancelled)) {
				return fail(HttpErrorKind::ReceiveFailed);
			}
			DWORD status = 0;
			DWORD size = sizeof(status);
			if (!WinHttpQueryHeaders(transport.Handle(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) {
				return fail(HttpErrorKind::ReceiveFailed, GetLastError());
			}
			result.request.statusCode = static_cast<int>(status);
			wchar_t retry[128] = { 0 };
			size = sizeof(retry);
			if (WinHttpQueryHeaders(transport.Handle(), WINHTTP_QUERY_RETRY_AFTER, WINHTTP_HEADER_NAME_BY_INDEX,
				retry, &size, WINHTTP_NO_HEADER_INDEX)) {
				char utf8[512] = { 0 };
				WideCharToMultiByte(CP_UTF8, 0, retry, -1, utf8, sizeof(utf8), nullptr, nullptr);
				result.retryAfter = utf8;
			}
			// A declared length must fit the cap and be met exactly; an answer
			// without one (chunked) is bounded by the cap alone.
			DWORD declared = 0;
			size = sizeof(declared);
			const bool hasLength = WinHttpQueryHeaders(transport.Handle(), WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX, &declared, &size, WINHTTP_NO_HEADER_INDEX) != FALSE;
			const DWORD lengthError = hasLength ? ERROR_SUCCESS : GetLastError();
			if (hasLength ? declared > maxResponse : lengthError != ERROR_WINHTTP_HEADER_NOT_FOUND) {
				return fail(HttpErrorKind::ReceiveFailed, lengthError);
			}
			for (;;) {
				char bytes[8192];
				if (!transport.Read(bytes, sizeof(bytes), cancelled)) {
					return fail(HttpErrorKind::ReceiveFailed);
				}
				const DWORD read = transport.Bytes();
				if (!read) {
					break;
				}
				if (result.body.size() + read > maxResponse) {
					return fail(HttpErrorKind::ReceiveFailed);
				}
				result.body.append(bytes, read);
			}
			return !hasLength || result.body.size() == declared || fail(HttpErrorKind::ReceiveFailed);
		};

		result.request.ok = exchange();
		transport.Close();
		// A completely received answer is still evidence of acceptance if
		// Cancel arrives during teardown. Never erase that response/receipt.
		result.cancelled = transport.Cancelled() || (!result.request.ok && cancelled && cancelled());
		if (result.cancelled) {
			result.request.ok = false;
		}
		return result;
	}

	bool HttpDownloadUrlUtf8(
		const char* url,
		const wchar_t* destPath,
		int timeoutMs,
		const char* extraHeaders,
		HttpRequestResult* outResult,
        const std::function<bool(std::uint64_t, std::uint64_t)>& progress
	) {
		if (outResult) {
			*outResult = HttpRequestResult{};
		}
		if (!url || !destPath || !destPath[0]) {
			if (outResult) {
				outResult->error = HttpErrorKind::InvalidArgs;
			}
			return false;
		}

		char host[256] = { 0 };
		char path[2048] = { 0 };
		bool useHttps = true;
		int port = 443;
		if (!ParseHttpUrl(url, host, sizeof(host), path, sizeof(path), useHttps, port)) {
			if (outResult) {
				outResult->error = HttpErrorKind::InvalidArgs;
			}
			return false;
		}

		wchar_t wHost[256];
		wchar_t wPath[2048];
		Utf8ToWide(host, wHost, 256);
		Utf8ToWide(path, wPath, 2048);

		HINTERNET hSession = OpenHttpSession(timeoutMs);
		if (!hSession) {
			if (outResult) {
				outResult->error = HttpErrorKind::OpenFailed;
				outResult->win32Error = GetLastError();
			}
			return false;
		}

		WinHttpSetTimeouts(hSession, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
		ApplyWinHttpDownloadOptions(hSession, NULL);

		INTERNET_PORT winPort = (INTERNET_PORT)(port > 0 ? port : (useHttps ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT));
		HINTERNET hConnect = WinHttpConnect(hSession, wHost, winPort, 0);
		if (!hConnect) {
			if (outResult) {
				outResult->error = HttpErrorKind::ConnectFailed;
				outResult->win32Error = GetLastError();
			}
			WinHttpCloseHandle(hSession);
			return false;
		}

		DWORD flags = useHttps ? WINHTTP_FLAG_SECURE : 0;
		HINTERNET hRequest = WinHttpOpenRequest(
			hConnect,
			L"GET",
			wPath,
			NULL,
			WINHTTP_NO_REFERER,
			WINHTTP_DEFAULT_ACCEPT_TYPES,
			flags
		);
		if (!hRequest) {
			if (outResult) {
				outResult->error = HttpErrorKind::OpenFailed;
				outResult->win32Error = GetLastError();
			}
			WinHttpCloseHandle(hConnect);
			WinHttpCloseHandle(hSession);
			return false;
		}

		ApplyWinHttpDownloadOptions(NULL, hRequest);

		wchar_t headerBuf[512] = { 0 };
		const wchar_t* headers = WINHTTP_NO_ADDITIONAL_HEADERS;
		DWORD headersLen = 0;
		if (extraHeaders && extraHeaders[0]) {
			Utf8ToWide(extraHeaders, headerBuf, 512);
			headers = headerBuf;
			headersLen = (DWORD)-1L;
		}

		if (!WinHttpSendRequest(hRequest, headers, headersLen, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
			if (outResult) {
				outResult->error = HttpErrorKind::SendFailed;
				outResult->win32Error = GetLastError();
			}
			WinHttpCloseHandle(hRequest);
			WinHttpCloseHandle(hConnect);
			WinHttpCloseHandle(hSession);
			return false;
		}
		if (!WinHttpReceiveResponse(hRequest, NULL)) {
			if (outResult) {
				outResult->error = HttpErrorKind::ReceiveFailed;
				outResult->win32Error = GetLastError();
			}
			WinHttpCloseHandle(hRequest);
			WinHttpCloseHandle(hConnect);
			WinHttpCloseHandle(hSession);
			return false;
		}

		DWORD status = 0;
		DWORD statusSize = sizeof(status);
		WinHttpQueryHeaders(
			hRequest,
			WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX,
			&status,
			&statusSize,
			WINHTTP_NO_HEADER_INDEX
		);
		if (outResult) {
			outResult->statusCode = (int)status;
		}
		if (status < 200 || status >= 300) {
			if (outResult) {
				outResult->error = HttpErrorKind::HttpStatus;
			}
			WinHttpCloseHandle(hRequest);
			WinHttpCloseHandle(hConnect);
			WinHttpCloseHandle(hSession);
			return false;
		}

		HANDLE hFile = CreateFileW(destPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hFile == INVALID_HANDLE_VALUE) {
			if (outResult) {
				outResult->error = HttpErrorKind::WriteFailed;
				outResult->win32Error = GetLastError();
			}
			WinHttpCloseHandle(hRequest);
			WinHttpCloseHandle(hConnect);
			WinHttpCloseHandle(hSession);
			return false;
		}

        wchar_t lengthHeader[32] = {}; DWORD lengthBytes = sizeof(lengthHeader);
        std::uint64_t expectedBytes = 0;
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lengthHeader, &lengthBytes, WINHTTP_NO_HEADER_INDEX)) expectedBytes = _wcstoui64(lengthHeader, nullptr, 10);
        bool ok = !progress || progress(0, expectedBytes);
        ULONGLONG totalWritten = 0;
		while (ok) {
			char buf[65536];
			DWORD read = 0;
			if (!WinHttpReadData(hRequest, buf, sizeof(buf), &read)) {
				ok = false;
				if (outResult) {
					outResult->error = HttpErrorKind::ReceiveFailed;
					outResult->win32Error = GetLastError();
				}
				break;
			}
			if (read == 0) {
				break;
			}
			DWORD written = 0;
			if (!WriteFile(hFile, buf, read, &written, NULL) || written != read) {
				ok = false;
				if (outResult) {
					outResult->error = HttpErrorKind::WriteFailed;
					outResult->win32Error = GetLastError();
				}
				break;
			}
			totalWritten += written;
            if (progress && !progress(totalWritten, expectedBytes)) { ok = false; break; }
		}

		CloseHandle(hFile);
		WinHttpCloseHandle(hRequest);
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);

		if (!ok || totalWritten == 0) {
			if (outResult && outResult->error == HttpErrorKind::None) {
				outResult->error = HttpErrorKind::EmptyBody;
			}
			DeleteFileW(destPath);
			return false;
		}

		if (outResult) {
			outResult->ok = true;
			outResult->error = HttpErrorKind::None;
		}
		return true;
	}

    static bool ReportArgs(const std::string& contentType, const std::string& body) {
        return !body.empty() && body.size()<=5*1024*1024 && contentType.find_first_of("\r\n")==std::string::npos;
    }

	// One report upload to `host`:`port`. Only HttpPostReport (the intake over
	// HTTPS) and the loopback test seam call it. The intake answers 202 with a
	// receipt well under 16 KiB.
	static HttpResponse PostReport(const char* host, int port, bool https, const std::string& contentType,
		const std::string& body, const std::function<bool()>& cancelled) {
		auto result = HttpBoundedRequest("POST", host, port, https, ReportIntakePath, "Content-Type: " + contentType + "\r\n",
			body, 16384, detail::DefaultRequestBudget, cancelled);
		if (result.request.ok && result.request.statusCode != 202) {
			result.request.ok = false;
			result.request.error = HttpErrorKind::HttpStatus;
		}
		return result;
	}

    HttpResponse HttpPostReport(const char* host, const std::string& contentType,
        const std::string& body, const std::function<bool()>& cancelled) {
        if (!host || strcmp(host,ReportIntakeHost)!=0 || !ReportArgs(contentType,body)) {
            HttpResponse result; result.request.error=HttpErrorKind::InvalidArgs; return result;
        }
        return PostReport(ReportIntakeHost,443,true,contentType,body,cancelled);
    }

    HttpResponse testing::HttpPostReportLoopback(int port, const std::string& contentType,
        const std::string& body, const std::function<bool()>& cancelled) {
        if (port<=0||port>65535||!ReportArgs(contentType,body)) {
            HttpResponse result; result.request.error=HttpErrorKind::InvalidArgs; return result;
        }
        return PostReport("127.0.0.1",port,false,contentType,body,cancelled);
    }
} // namespace sf4e
