// Release zip download and SHA-256 verification for the release client.
#include "github_release_client_internal.hxx"

namespace sf4e {
namespace launcher {
	namespace detail {

		// Streaming SHA-256 of a file, returned as lowercase hex. Used to verify a
		// downloaded release zip against the digest GitHub publishes for the asset,
		// so a tampered or corrupted download is rejected before we extract and run
		// any of its contents.
		bool ComputeFileSha256Hex(const wchar_t* filePath, std::string& outHex) {
			outHex.clear();

			BCRYPT_ALG_HANDLE hAlg = NULL;
			if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, NULL, 0))) {
				return false;
			}

			DWORD cbHashObject = 0;
			DWORD cbData = 0;
			DWORD cbHash = 0;
			bool ok = NT_SUCCESS(BCryptGetProperty(
						  hAlg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&cbHashObject, sizeof(DWORD), &cbData, 0)) &&
				NT_SUCCESS(BCryptGetProperty(
					hAlg, BCRYPT_HASH_LENGTH, (PUCHAR)&cbHash, sizeof(DWORD), &cbData, 0));

			PUCHAR pbHashObject = ok ? (PUCHAR)HeapAlloc(GetProcessHeap(), 0, cbHashObject) : NULL;
			PUCHAR pbHash = ok ? (PUCHAR)HeapAlloc(GetProcessHeap(), 0, cbHash) : NULL;
			BCRYPT_HASH_HANDLE hHash = NULL;
			HANDLE hFile = INVALID_HANDLE_VALUE;
			if (!pbHashObject || !pbHash) {
				ok = false;
			}

			if (ok && !NT_SUCCESS(BCryptCreateHash(hAlg, &hHash, pbHashObject, cbHashObject, NULL, 0, 0))) {
				ok = false;
			}

			if (ok) {
				hFile = CreateFileW(
					filePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
					FILE_FLAG_SEQUENTIAL_SCAN, NULL);
				if (hFile == INVALID_HANDLE_VALUE) {
					ok = false;
				}
			}

			if (ok) {
				BYTE buffer[8192];
				DWORD cbRead = 0;
				for (;;) {
					if (!ReadFile(hFile, buffer, sizeof(buffer), &cbRead, NULL)) {
						ok = false;
						break;
					}
					if (cbRead == 0) {
						break;
					}
					if (!NT_SUCCESS(BCryptHashData(hHash, buffer, cbRead, 0))) {
						ok = false;
						break;
					}
				}
			}

			if (ok && !NT_SUCCESS(BCryptFinishHash(hHash, pbHash, cbHash, 0))) {
				ok = false;
			}

			if (ok) {
				char pair[3];
				for (DWORD i = 0; i < cbHash; i++) {
					snprintf(pair, sizeof(pair), "%02x", pbHash[i]);
					outHex += pair;
				}
			}

			if (hFile != INVALID_HANDLE_VALUE) {
				CloseHandle(hFile);
			}
			if (hHash) {
				BCryptDestroyHash(hHash);
			}
			if (pbHashObject) {
				HeapFree(GetProcessHeap(), 0, pbHashObject);
			}
			if (pbHash) {
				HeapFree(GetProcessHeap(), 0, pbHash);
			}
			BCryptCloseAlgorithmProvider(hAlg, 0);
			return ok;
		}

		// Case-insensitive equality for hex digests.
		bool HexEqualsIgnoreCase(const std::string& a, const std::string& b) {
			if (a.size() != b.size()) {
				return false;
			}
			for (size_t i = 0; i < a.size(); i++) {
				if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
					return false;
				}
			}
			return true;
		}

		static std::string HttpDownloadErrorMessage(const HttpRequestResult& httpResult) {
			const unsigned long win32 = httpResult.win32Error ? httpResult.win32Error : GetLastError();
			char buf[160];
			switch (httpResult.error) {
			case HttpErrorKind::HttpStatus:
				if (httpResult.statusCode > 0) {
					snprintf(buf, sizeof(buf), "HTTP %d", httpResult.statusCode);
					return buf;
				}
				return "HTTP error";
			case HttpErrorKind::Timeout:
				return "timed out";
			case HttpErrorKind::ConnectFailed:
				snprintf(buf, sizeof(buf), "could not connect (Win32 %lu)", win32);
				return buf;
			case HttpErrorKind::ReceiveFailed:
				snprintf(buf, sizeof(buf), "transfer interrupted (Win32 %lu)", win32);
				return buf;
			case HttpErrorKind::EmptyBody:
				return "empty response";
			case HttpErrorKind::SendFailed:
				snprintf(buf, sizeof(buf), "request failed (Win32 %lu)", win32);
				return buf;
			case HttpErrorKind::OpenFailed:
				snprintf(buf, sizeof(buf), "connection open failed (Win32 %lu)", win32);
				return buf;
			case HttpErrorKind::WriteFailed:
				snprintf(buf, sizeof(buf), "could not write temp file (Win32 %lu)", win32);
				return buf;
			case HttpErrorKind::InvalidArgs:
				return "invalid download URL";
			default:
				snprintf(buf, sizeof(buf), "unknown error (kind %d, Win32 %lu)", (int)httpResult.error, win32);
				return buf;
			}
		}

		static bool TryHttpDownload(
			const char* label,
			const char* url,
			const char* headers,
			const wchar_t* zipPath,
			std::string& outError,
            const std::function<bool(std::uint64_t, std::uint64_t)>& progress
		) {
			if (!url || !url[0]) {
				outError = "missing URL";
				return false;
			}
			if (!IsAllowedUpdateUrl(url)) {
				outError = "download URL host is not allowlisted";
				AppendUpdateLog((std::string(label) + " blocked: disallowed host").c_str());
				return false;
			}
			if (!EnsureParentDirectoryExistsW(zipPath, outError)) {
				AppendUpdateLog((std::string(label) + " mkdir failed: " + outError).c_str());
				return false;
			}
			HttpRequestResult httpResult;
			if (HttpDownloadUrlUtf8(url, zipPath, 20000, headers, &httpResult, progress)) {
				AppendUpdateLog((std::string(label) + " OK").c_str());
				return true;
			}
			outError = HttpDownloadErrorMessage(httpResult);
			AppendUpdateLog((std::string(label) + " failed: " + outError).c_str());
			return false;
		}

		bool DownloadReleaseZip(
			const char* zipApiUrl,
			const char* zipDownloadUrl,
			const wchar_t* zipPath,
			std::string& outError,
            const std::function<bool(std::uint64_t, std::uint64_t)>& progress
		) {
			const char* apiHeaders = "Accept: application/octet-stream\r\nUser-Agent: sf4e-updater/1.0\r\n";
			const char* browserHeaders = "User-Agent: sf4e-updater/1.0\r\n";
			std::vector<std::string> attempts;
			std::string attemptError;

			auto recordFailure = [&](const char* label) {
				if (!attemptError.empty()) {
					attempts.push_back(std::string(label) + ": " + attemptError);
				}
				DeleteFileW(zipPath);
			};

			AppendUpdateLog("download start");

			if (zipDownloadUrl && zipDownloadUrl[0]) {
				if (TryHttpDownload("browser", zipDownloadUrl, browserHeaders, zipPath, attemptError, progress)) {
					return true;
				}
				recordFailure("browser");
                if (progress && !progress(0,0)) { outError = "Cancelled"; return false; }
			}

			if (zipApiUrl && zipApiUrl[0]) {
				if (TryHttpDownload("api", zipApiUrl, apiHeaders, zipPath, attemptError, progress)) {
					return true;
				}
				recordFailure("api");
			}

			std::string detail;
			for (size_t i = 0; i < attempts.size(); i++) {
				if (i > 0) {
					detail += "; ";
				}
				detail += attempts[i];
			}
			if (detail.empty()) {
				outError = "all download methods failed";
			}
			else {
				outError = detail;
			}
			AppendUpdateLog(("download failed: " + outError).c_str());
			return false;
		}

	} // namespace detail
} // namespace launcher
} // namespace sf4e
