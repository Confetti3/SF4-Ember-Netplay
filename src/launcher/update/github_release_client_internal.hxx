#pragma once

// Shared by the translation units that implement the release client. Not a
// public interface; include github_release_client.hxx instead.
#include "github_release_client.hxx"
#include "github_release_download.hxx"
#include "PackageInstaller.hxx"
#include "../../common/PackageInventory.hxx"

#include "../../common/install_paths.hxx"
#include "../../common/Localization.hxx"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <windows.h>
#include <pathcch.h>
#include <shellapi.h>
#include <strsafe.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <nlohmann/json.hpp>

#include "../../common/sf4e__NetUtil.hxx"

namespace sf4e {
namespace launcher {
	namespace detail {

		// Defined in github_release_client.cxx.
		void AppendUpdateLog(const char* message);
		bool WidePathToUtf8(const wchar_t* wide, char* out, int outLen);
		bool EnsureParentDirectoryExistsW(const wchar_t* filePath, std::string& outError);
		// Runs a hidden child in a kill-on-close job. `cancel` is read each 250 ms
		// slice; it, or five minutes, ends the child and its children.
		bool RunProcessAndWaitHidden(const wchar_t* application, const wchar_t* cmdLine, DWORD* outExitCode,
			const std::atomic<bool>& cancel);

		// Defined in github_release_validation.cxx.
		bool IsAllowedUpdateUrl(const char* url);
		bool FindPackageRoot(const wchar_t* searchRoot, wchar_t* outRoot, int outRootChars);
		bool ValidateExtractedTree(const wchar_t* extractRoot);
		bool ValidateStagedPackage(const wchar_t* stagingDir, const std::atomic<bool>& cancel = NeverCancelled, const Progress& progress = {});

		// Defined in github_release_download.cxx.
		bool HexEqualsIgnoreCase(const std::string& a, const std::string& b);

	} // namespace detail
} // namespace launcher
} // namespace sf4e
