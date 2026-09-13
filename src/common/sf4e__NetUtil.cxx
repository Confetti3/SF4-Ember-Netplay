#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif


#include <windows.h>
#include <winhttp.h>

#include <stdlib.h>
#include <string.h>



#pragma comment(lib, "winhttp.lib")

#include "sf4e__NetUtil.hxx"

namespace sf4e {

	static void Utf8ToWide(const char* utf8, wchar_t* out, int outChars) {
		if (!utf8 || !out || outChars <= 0) {
			if (out && outChars > 0) {
				out[0] = 0;
			}
			return;
		}
		MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, outChars);
	}

	static bool HttpGetUtf8WithHeadersInternal(
		const char* host,
		int port,
		bool useHttps,
		const char* path,
		int timeoutMs,
		const char* extraHeadersUtf8,
		char* outBody,
		int outBodyLen
	) {
		if (!host || !path || !outBody || outBodyLen <= 0) {
			return false;
		}

		wchar_t wHost[256];
		wchar_t wPath[512];
		Utf8ToWide(host, wHost, 256);
		Utf8ToWide(path, wPath, 512);

		HINTERNET hSession = WinHttpOpen(
			L"sf4e-updater/1.0",
			WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
			WINHTTP_NO_PROXY_NAME,
			WINHTTP_NO_PROXY_BYPASS,
			0
		);
		if (!hSession) {
			return false;
		}

		WinHttpSetTimeouts(hSession, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

		INTERNET_PORT winPort = (INTERNET_PORT)(port > 0 ? port : (useHttps ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT));
		HINTERNET hConnect = WinHttpConnect(hSession, wHost, winPort, 0);
		if (!hConnect) {
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
			WinHttpCloseHandle(hConnect);
			WinHttpCloseHandle(hSession);
			return false;
		}

		wchar_t headerBuf[512] = { 0 };
		const wchar_t* headers = WINHTTP_NO_ADDITIONAL_HEADERS;
		DWORD headersLen = 0;
		if (extraHeadersUtf8 && extraHeadersUtf8[0]) {
			Utf8ToWide(extraHeadersUtf8, headerBuf, 512);
			headers = headerBuf;
			headersLen = (DWORD)-1L;
		}

		if (!WinHttpSendRequest(hRequest, headers, headersLen, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
			!WinHttpReceiveResponse(hRequest, NULL)) {
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
		if (status < 200 || status >= 300) {
			WinHttpCloseHandle(hRequest);
			WinHttpCloseHandle(hConnect);
			WinHttpCloseHandle(hSession);
			return false;
		}

		int total = 0;
		outBody[0] = '\0';
		for (;;) {
			DWORD avail = 0;
			if (!WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0) {
				break;
			}
			if (total + (int)avail >= outBodyLen - 1) {
				avail = (DWORD)(outBodyLen - 1 - total);
			}
			DWORD read = 0;
			if (!WinHttpReadData(hRequest, outBody + total, avail, &read) || read == 0) {
				break;
			}
			total += (int)read;
			outBody[total] = '\0';
			if (total >= outBodyLen - 1) {
				break;
			}
		}

		WinHttpCloseHandle(hRequest);
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		return total > 0;
	}

	bool HttpGetUtf8WithHeaders(
		const char* host,
		int port,
		bool useHttps,
		const char* path,
		int timeoutMs,
		const char* extraHeaders,
		char* outBody,
		int outBodyLen
	) {
		return HttpGetUtf8WithHeadersInternal(host, port, useHttps, path, timeoutMs, extraHeaders, outBody, outBodyLen);
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
		if (hSession) {
			DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
			WinHttpSetOption(hSession, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));
			DWORD maxRedirects = 10;
			WinHttpSetOption(hSession, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &maxRedirects, sizeof(maxRedirects));
		}
		if (hRequest) {
			DWORD reqRedirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
			WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &reqRedirect, sizeof(reqRedirect));
			DWORD secureProtocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
			secureProtocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
			WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURE_PROTOCOLS, &secureProtocols, sizeof(secureProtocols));
		}
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

		HINTERNET hSession = WinHttpOpen(
			L"sf4e-updater/1.0",
			WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
			WINHTTP_NO_PROXY_NAME,
			WINHTTP_NO_PROXY_BYPASS,
			0
		);
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

} // namespace sf4e
