// Release zip download and digest comparison for the release client.
#include "github_release_client_internal.hxx"

namespace sf4e {
namespace launcher {
	namespace detail {

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
			const std::atomic<bool>& cancel,
			const Progress& progress
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
			// The HTTP helper asks after each block whether to go on.
			const auto received = [&](std::uint64_t done, std::uint64_t total) {
				if (progress) progress(UpdateStage::Downloading, done, total);
				return !cancel;
			};
			if (HttpDownloadUrlUtf8(url, zipPath, 20000, headers, &httpResult, received)) {
				AppendUpdateLog((std::string(label) + " OK").c_str());
				return true;
			}
			outError = HttpDownloadErrorMessage(httpResult);
			AppendUpdateLog((std::string(label) + " failed: " + outError).c_str());
			return false;
		}

		bool DownloadReleaseZip(
			const UpdateCheckResult& offer,
			const wchar_t* zipPath,
			std::string& outError,
			const std::atomic<bool>& cancel,
			const Progress& progress
		) {
			outError.clear();
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

			if (!offer.zipDownloadUrl.empty()) {
				if (TryHttpDownload("browser", offer.zipDownloadUrl.c_str(), browserHeaders, zipPath, attemptError, cancel, progress)) {
					return true;
				}
				recordFailure("browser");
				if (cancel) {
					AppendUpdateLog("download cancelled");
					outError = loc::T("update.cancelled");
					return false;
				}
			}

			if (!offer.zipApiUrl.empty()) {
				if (TryHttpDownload("api", offer.zipApiUrl.c_str(), apiHeaders, zipPath, attemptError, cancel, progress)) {
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
				detail = "all download methods failed";
			}
			AppendUpdateLog(("download failed: " + detail).c_str());
			outError = loc::Tf("update.download_failed", detail, offer.releaseUrl);
			return false;
		}

	} // namespace detail
} // namespace launcher
} // namespace sf4e
