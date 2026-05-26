#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <iphlpapi.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")

#include "sf4e__NetUtil.hxx"

namespace sf4e {

	void TrimRoomCodeInPlace(char* buf) {
		if (!buf) {
			return;
		}
		size_t len = strlen(buf);
		size_t start = 0;
		while (start < len && (buf[start] == ' ' || buf[start] == '\t' || buf[start] == '\r' || buf[start] == '\n')) {
			start++;
		}
		size_t end = len;
		while (end > start && (buf[end - 1] == ' ' || buf[end - 1] == '\t' || buf[end - 1] == '\r' || buf[end - 1] == '\n')) {
			end--;
		}
		if (start > 0) {
			memmove(buf, buf + start, end - start);
		}
		buf[end - start] = '\0';
	}

	static HttpErrorKind WinHttpErrorKind(DWORD err) {
		if (err == ERROR_WINHTTP_TIMEOUT) {
			return HttpErrorKind::Timeout;
		}
		return HttpErrorKind::ConnectFailed;
	}

	static void InitHttpRequestResult(HttpRequestResult* outResult) {
		if (!outResult) {
			return;
		}
		outResult->ok = false;
		outResult->error = HttpErrorKind::None;
		outResult->statusCode = 0;
	}

	static int ReadHttpResponseBody(HINTERNET hRequest, char* outBody, int outBodyLen) {
		if (!outBody || outBodyLen <= 0) {
			return 0;
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
		return total;
	}

	static bool HttpRequestUtf8(
		const wchar_t* method,
		const wchar_t* host,
		int port,
		bool useHttps,
		const wchar_t* path,
		int timeoutMs,
		const char* requestBody,
		char* outBody,
		int outBodyLen,
		HttpRequestResult* outResult
	) {
		InitHttpRequestResult(outResult);
		if (!host || !path || !outBody || outBodyLen <= 0 || !method) {
			if (outResult) {
				outResult->error = HttpErrorKind::InvalidArgs;
			}
			return false;
		}

		HINTERNET hSession = WinHttpOpen(
			L"sf4e/1.0",
			WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
			WINHTTP_NO_PROXY_NAME,
			WINHTTP_NO_PROXY_BYPASS,
			0
		);
		if (!hSession) {
			if (outResult) {
				outResult->error = HttpErrorKind::OpenFailed;
			}
			return false;
		}

		WinHttpSetTimeouts(hSession, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

		INTERNET_PORT winPort = (INTERNET_PORT)(port > 0 ? port : (useHttps ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT));
		HINTERNET hConnect = WinHttpConnect(hSession, host, winPort, 0);
		if (!hConnect) {
			if (outResult) {
				outResult->error = WinHttpErrorKind(GetLastError());
			}
			WinHttpCloseHandle(hSession);
			return false;
		}

		DWORD flags = useHttps ? WINHTTP_FLAG_SECURE : 0;
		HINTERNET hRequest = WinHttpOpenRequest(
			hConnect,
			method,
			path,
			NULL,
			WINHTTP_NO_REFERER,
			WINHTTP_DEFAULT_ACCEPT_TYPES,
			flags
		);
		if (!hRequest) {
			if (outResult) {
				outResult->error = HttpErrorKind::ConnectFailed;
			}
			WinHttpCloseHandle(hConnect);
			WinHttpCloseHandle(hSession);
			return false;
		}

		const wchar_t* headers = L"Content-Type: application/json\r\n";
		DWORD headersLen = 0;
		if (requestBody && requestBody[0]) {
			headersLen = (DWORD)-1L;
		}
		else {
			headers = WINHTTP_NO_ADDITIONAL_HEADERS;
		}

		DWORD bodyLen = requestBody ? (DWORD)strlen(requestBody) : 0;
		BOOL ok = WinHttpSendRequest(
			hRequest,
			headers,
			headersLen,
			requestBody ? (LPVOID)requestBody : WINHTTP_NO_REQUEST_DATA,
			bodyLen,
			bodyLen,
			0
		);
		if (!ok) {
			if (outResult) {
				outResult->error = WinHttpErrorKind(GetLastError());
			}
			WinHttpCloseHandle(hRequest);
			WinHttpCloseHandle(hConnect);
			WinHttpCloseHandle(hSession);
			return false;
		}
		if (!WinHttpReceiveResponse(hRequest, NULL)) {
			if (outResult) {
				outResult->error = WinHttpErrorKind(GetLastError());
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

		int total = ReadHttpResponseBody(hRequest, outBody, outBodyLen);

		WinHttpCloseHandle(hRequest);
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);

		if (status < 200 || status >= 300) {
			if (outResult) {
				outResult->error = HttpErrorKind::HttpStatus;
			}
			return false;
		}
		if (total <= 0) {
			if (outResult) {
				outResult->error = HttpErrorKind::EmptyBody;
			}
			return false;
		}

		if (outResult) {
			outResult->ok = true;
			outResult->error = HttpErrorKind::None;
		}
		return true;
	}

	static bool HttpGetFirstLine(const wchar_t* host, const wchar_t* path, int timeoutMs, char* outBody, int outBodyLen) {
		if (!host || !path || !outBody || outBodyLen <= 0) {
			return false;
		}
		bool ok = HttpRequestUtf8(L"GET", host, 0, true, path, timeoutMs, NULL, outBody, outBodyLen, nullptr);
		TrimRoomCodeInPlace(outBody);
		return ok;
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

	bool HttpGetUtf8(
		const char* host,
		int port,
		bool useHttps,
		const char* path,
		int timeoutMs,
		char* outBody,
		int outBodyLen,
		HttpRequestResult* outResult
	) {
		wchar_t wHost[256];
		wchar_t wPath[512];
		Utf8ToWide(host, wHost, 256);
		Utf8ToWide(path, wPath, 512);
		return HttpRequestUtf8(L"GET", wHost, port, useHttps, wPath, timeoutMs, NULL, outBody, outBodyLen, outResult);
	}

	bool HttpPostJsonUtf8(
		const char* host,
		int port,
		bool useHttps,
		const char* path,
		int timeoutMs,
		const char* jsonBody,
		char* outBody,
		int outBodyLen,
		HttpRequestResult* outResult
	) {
		wchar_t wHost[256];
		wchar_t wPath[512];
		Utf8ToWide(host, wHost, 256);
		Utf8ToWide(path, wPath, 512);
		return HttpRequestUtf8(L"POST", wHost, port, useHttps, wPath, timeoutMs, jsonBody, outBody, outBodyLen, outResult);
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
		HttpRequestResult* outResult
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

		bool ok = true;
		ULONGLONG totalWritten = 0;
		for (;;) {
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

	bool FetchPublicIPv4(char* outIp, int outIpLen, int timeoutMs) {
		if (!outIp || outIpLen <= 0) {
			return false;
		}

		char body[64] = { 0 };
		if (HttpGetFirstLine(L"api.ipify.org", L"/", timeoutMs, body, sizeof(body))) {
			strncpy_s(outIp, outIpLen, body, _TRUNCATE);
			return true;
		}

		memset(body, 0, sizeof(body));
		if (HttpGetFirstLine(L"icanhazip.com", L"/", timeoutMs, body, sizeof(body))) {
			strncpy_s(outIp, outIpLen, body, _TRUNCATE);
			return true;
		}

		return false;
	}

	bool CopyTextToClipboardUtf8(const char* text) {
		if (!text || !text[0]) {
			return false;
		}

		int wideLen = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
		if (wideLen <= 0) {
			return false;
		}

		HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)wideLen * sizeof(wchar_t));
		if (!hMem) {
			return false;
		}

		wchar_t* wide = (wchar_t*)GlobalLock(hMem);
		if (!wide) {
			GlobalFree(hMem);
			return false;
		}
		MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, wideLen);
		GlobalUnlock(hMem);

		if (!OpenClipboard(NULL)) {
			GlobalFree(hMem);
			return false;
		}
		EmptyClipboard();
		BOOL set = SetClipboardData(CF_UNICODETEXT, hMem) != NULL;
		CloseClipboard();
		if (!set) {
			GlobalFree(hMem);
		}
		return set != FALSE;
	}

	bool DetectLanIPv4(char* outIp, int outIpLen) {
		if (!outIp || outIpLen <= 0) {
			return false;
		}

		ULONG bufLen = 15000;
		PIP_ADAPTER_ADDRESSES addrs = (PIP_ADAPTER_ADDRESSES)malloc(bufLen);
		if (!addrs) {
			strncpy_s(outIp, outIpLen, "127.0.0.1", _TRUNCATE);
			return false;
		}

		ULONG res = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, addrs, &bufLen);
		if (res == ERROR_BUFFER_OVERFLOW) {
			free(addrs);
			addrs = (PIP_ADAPTER_ADDRESSES)malloc(bufLen);
			if (!addrs) {
				strncpy_s(outIp, outIpLen, "127.0.0.1", _TRUNCATE);
				return false;
			}
			res = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, addrs, &bufLen);
		}

		bool found = false;
		if (res == NO_ERROR) {
			for (PIP_ADAPTER_ADDRESSES a = addrs; a; a = a->Next) {
				if (a->OperStatus != IfOperStatusUp) {
					continue;
				}
				for (PIP_ADAPTER_UNICAST_ADDRESS u = a->FirstUnicastAddress; u; u = u->Next) {
					sockaddr_in* sa = (sockaddr_in*)u->Address.lpSockaddr;
					char buf[32];
					InetNtopA(AF_INET, &sa->sin_addr, buf, sizeof(buf));
					if (strncmp(buf, "127.", 4) == 0) {
						continue;
					}
					if (strncmp(buf, "169.254.", 8) == 0) {
						continue;
					}
					strncpy_s(outIp, outIpLen, buf, _TRUNCATE);
					found = true;
					break;
				}
				if (found) {
					break;
				}
			}
		}

		free(addrs);
		if (!found) {
			strncpy_s(outIp, outIpLen, "127.0.0.1", _TRUNCATE);
		}
		return found;
	}

	static bool EnsureWinsockStarted() {
		static bool started = false;
		if (started) {
			return true;
		}
		WSADATA wsa = { 0 };
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
			return false;
		}
		started = true;
		return true;
	}

	static bool IsProcessAlive(unsigned long pid) {
		if (pid == 0) {
			return false;
		}
		HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (!process) {
			return false;
		}
		DWORD exitCode = STILL_ACTIVE;
		const BOOL ok = GetExitCodeProcess(process, &exitCode);
		CloseHandle(process);
		return ok && exitCode == STILL_ACTIVE;
	}

	static bool IsLocalUdpPortOpen(uint16_t port, unsigned long ownerPid) {
		PMIB_UDPTABLE_OWNER_PID table = nullptr;
		ULONG size = 0;
		if (GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) != ERROR_INSUFFICIENT_BUFFER) {
			return false;
		}

		table = (PMIB_UDPTABLE_OWNER_PID)malloc(size);
		if (!table) {
			return false;
		}

		bool found = false;
		if (GetExtendedUdpTable(table, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) == NO_ERROR) {
			for (DWORD i = 0; i < table->dwNumEntries; i++) {
				const MIB_UDPROW_OWNER_PID& row = table->table[i];
				const uint16_t rowPort = ntohs((uint16_t)row.dwLocalPort);
				if (rowPort != port) {
					continue;
				}
				if (ownerPid == 0 || row.dwOwningPid == ownerPid) {
					found = true;
					break;
				}
			}
		}

		free(table);
		return found;
	}

	static bool IsLocalPortBound(uint16_t port) {
		SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (s == INVALID_SOCKET) {
			return false;
		}

		sockaddr_in addr = { 0 };
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		addr.sin_addr.s_addr = htonl(INADDR_ANY);

		const int rc = bind(s, (sockaddr*)&addr, sizeof(addr));
		const int err = (rc == SOCKET_ERROR) ? WSAGetLastError() : 0;
		closesocket(s);
		return rc == SOCKET_ERROR && err == WSAEADDRINUSE;
	}

	static bool ProbeLocalTcpConnect(uint16_t port) {
		SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (s == INVALID_SOCKET) {
			return false;
		}

		u_long nonBlocking = 1;
		ioctlsocket(s, FIONBIO, &nonBlocking);

		sockaddr_in addr = { 0 };
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

		const int rc = connect(s, (sockaddr*)&addr, sizeof(addr));
		if (rc == 0) {
			closesocket(s);
			return true;
		}

		if (WSAGetLastError() == WSAEWOULDBLOCK) {
			fd_set wfds;
			FD_ZERO(&wfds);
			FD_SET(s, &wfds);
			timeval tv = { 0, 100000 };
			const int sel = select(0, NULL, &wfds, NULL, &tv);
			closesocket(s);
			return sel > 0;
		}

		closesocket(s);
		return false;
	}

	bool ProbeRemoteTcpConnect(const char* host, uint16_t port, int timeoutMs) {
		if (!host || !host[0] || port == 0 || timeoutMs <= 0) {
			return false;
		}
		if (!EnsureWinsockStarted()) {
			return false;
		}

		SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (s == INVALID_SOCKET) {
			return false;
		}

		u_long nonBlocking = 1;
		ioctlsocket(s, FIONBIO, &nonBlocking);

		sockaddr_in addr = { 0 };
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
			closesocket(s);
			return false;
		}

		const int rc = connect(s, (sockaddr*)&addr, sizeof(addr));
		if (rc == 0) {
			closesocket(s);
			return true;
		}

		const int connectErr = WSAGetLastError();
		if (connectErr != WSAEWOULDBLOCK && connectErr != WSAEINPROGRESS) {
			closesocket(s);
			return false;
		}

		fd_set wfds;
		fd_set efds;
		FD_ZERO(&wfds);
		FD_ZERO(&efds);
		FD_SET(s, &wfds);
		FD_SET(s, &efds);

		timeval tv = { 0, 0 };
		tv.tv_sec = timeoutMs / 1000;
		tv.tv_usec = (timeoutMs % 1000) * 1000;

		const int sel = select(0, NULL, &wfds, &efds, &tv);
		if (sel <= 0) {
			closesocket(s);
			return false;
		}

		int soError = 0;
		int soErrorLen = sizeof(soError);
		if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&soError, &soErrorLen) != 0) {
			closesocket(s);
			return false;
		}

		closesocket(s);
		return soError == 0;
	}

	bool WaitForLocalTcpPort(uint16_t port, int timeoutMs, unsigned long ownerPid) {
		if (port == 0 || timeoutMs <= 0) {
			return false;
		}
		if (!EnsureWinsockStarted()) {
			return false;
		}

		const ULONGLONG deadline = GetTickCount64() + (ULONGLONG)timeoutMs;
		const ULONGLONG minAliveMs = GetTickCount64() + 400;
		while (GetTickCount64() < deadline) {
			if (ownerPid != 0 && !IsProcessAlive(ownerPid)) {
				return false;
			}
			if (IsLocalUdpPortOpen(port, ownerPid) || IsLocalPortBound(port) || ProbeLocalTcpConnect(port)) {
				return true;
			}
			if (ownerPid != 0 && IsProcessAlive(ownerPid) && GetTickCount64() >= minAliveMs) {
				// GNS binds UDP without always showing up in time; accept a live RelayHost after brief startup.
				return true;
			}
			Sleep(50);
		}
		return false;
	}

} // namespace sf4e
