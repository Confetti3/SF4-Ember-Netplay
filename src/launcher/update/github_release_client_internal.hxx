#pragma once

// Shared by the translation units that implement the release client. Not a
// public interface; include github_release_client.hxx instead.
#include "github_release_client.hxx"
#include "PackageInstaller.hxx"
#include "../../common/PackageInventory.hxx"

#include "../../common/install_paths.hxx"
#include "../../common/Localization.hxx"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <filesystem>
#include <string>
#include <vector>

#include <windows.h>
#include <bcrypt.h>
#include <pathcch.h>
#include <shellapi.h>
#include <strsafe.h>
#include <shlobj.h>
#include <tlhelp32.h>

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

#pragma comment(lib, "bcrypt.lib")

#include <nlohmann/json.hpp>

#include "../../common/sf4e__NetUtil.hxx"

namespace sf4e {
namespace launcher {
	namespace detail {

		// Defined in github_release_client.cxx.
		void AppendUpdateLog(const char* message);
		bool WidePathToUtf8(const wchar_t* wide, char* out, int outLen);
		bool EnsureParentDirectoryExistsW(const wchar_t* filePath, std::string& outError);

		// Defined in github_release_validation.cxx.
		bool IsAllowedUpdateUrl(const char* url);
		int CompareVersions(const char* a, const char* b);
		bool FindPackageRoot(const wchar_t* searchRoot, wchar_t* outRoot, int outRootChars);
		bool ValidateExtractedTree(const wchar_t* extractRoot);
		bool ValidateStagedPackage(const wchar_t* stagingDir);

		// Defined in github_release_download.cxx.
		bool ComputeFileSha256Hex(const wchar_t* filePath, std::string& outHex);
		bool HexEqualsIgnoreCase(const std::string& a, const std::string& b);
		bool DownloadReleaseZip(
			const char* zipApiUrl,
			const char* zipDownloadUrl,
			const wchar_t* zipPath,
			std::string& outError,
            const std::function<bool(std::uint64_t, std::uint64_t)>& progress
		);

	} // namespace detail
} // namespace launcher
} // namespace sf4e
