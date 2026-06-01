#include "install_paths.hxx"

#include <pathcch.h>
#include <shlwapi.h>
#include <strsafe.h>

namespace sf4e {
namespace install {

const wchar_t kDllDirName[] = L"dll";

namespace {

bool DirExists(const wchar_t* path) {
	return path && path[0] && PathIsDirectoryW(path) != FALSE;
}

bool PrependPath(const wchar_t* dir) {
	if (!dir || !dir[0]) {
		return false;
	}
	const DWORD kMaxEnv = 32767;
	DWORD nPathChars = GetEnvironmentVariableW(L"PATH", NULL, 0);
	if (nPathChars == 0) {
		return SetEnvironmentVariableW(L"PATH", dir) != FALSE;
	}
	wchar_t* existing = (wchar_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, nPathChars * sizeof(wchar_t));
	if (!existing) {
		return false;
	}
	if (GetEnvironmentVariableW(L"PATH", existing, nPathChars) != nPathChars - 1) {
		HeapFree(GetProcessHeap(), 0, existing);
		return false;
	}
	const size_t newLen = (size_t)nPathChars + wcslen(dir) + 4;
	if (newLen > kMaxEnv) {
		HeapFree(GetProcessHeap(), 0, existing);
		return false;
	}
	wchar_t* combined = (wchar_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, newLen * sizeof(wchar_t));
	if (!combined) {
		HeapFree(GetProcessHeap(), 0, existing);
		return false;
	}
	const HRESULT hr = StringCchPrintfW(combined, newLen, L"%s;%s", dir, existing);
	HeapFree(GetProcessHeap(), 0, existing);
	if (FAILED(hr)) {
		HeapFree(GetProcessHeap(), 0, combined);
		return false;
	}
	const BOOL ok = SetEnvironmentVariableW(L"PATH", combined);
	HeapFree(GetProcessHeap(), 0, combined);
	return ok != FALSE;
}

} // namespace

bool GetInstallRoot(wchar_t* outDir, int outDirChars) {
	if (!outDir || outDirChars <= 0) {
		return false;
	}
	if (GetModuleFileNameW(NULL, outDir, (DWORD)outDirChars) == 0) {
		return false;
	}
	if (!SUCCEEDED(PathCchRemoveFileSpec(outDir, (size_t)outDirChars))) {
		return false;
	}
	// LauncherApp.exe lives in dll/; package root is one level up.
	const wchar_t* leaf = PathFindFileNameW(outDir);
	if (leaf && _wcsicmp(leaf, kDllDirName) == 0) {
		return SUCCEEDED(PathCchRemoveFileSpec(outDir, (size_t)outDirChars));
	}
	return true;
}

bool GetPackageDllDirectory(wchar_t* outDir, int outDirChars) {
	wchar_t root[MAX_PATH] = { 0 };
	if (!GetInstallRoot(root, MAX_PATH)) {
		return false;
	}
	if (FAILED(PathCchCombine(outDir, (size_t)outDirChars, root, kDllDirName))) {
		return false;
	}
	if (DirExists(outDir)) {
		return true;
	}
	return SUCCEEDED(StringCchCopyW(outDir, (size_t)outDirChars, root));
}

bool UsesDllSubdirectory() {
	wchar_t root[MAX_PATH] = { 0 };
	wchar_t dllDir[MAX_PATH] = { 0 };
	if (!GetInstallRoot(root, MAX_PATH)) {
		return false;
	}
	if (FAILED(PathCchCombine(dllDir, MAX_PATH, root, kDllDirName))) {
		return false;
	}
	return DirExists(dllDir);
}

bool ConfigureDllSearch() {
	wchar_t root[MAX_PATH] = { 0 };
	wchar_t dllDir[MAX_PATH] = { 0 };
	if (!GetInstallRoot(root, MAX_PATH)) {
		return false;
	}
	if (FAILED(PathCchCombine(dllDir, MAX_PATH, root, kDllDirName))) {
		return false;
	}
	if (!DirExists(dllDir)) {
		SetDllDirectoryW(root);
		PrependPath(root);
		return true;
	}

	using SetDefaultDllDirectoriesFn = BOOL(WINAPI*)(DWORD);
	using AddDllDirectoryFn = DLL_DIRECTORY_COOKIE(WINAPI*)(PCWSTR);
	HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
	if (kernel) {
		auto setDefault = reinterpret_cast<SetDefaultDllDirectoriesFn>(
			GetProcAddress(kernel, "SetDefaultDllDirectories"));
		auto addDir = reinterpret_cast<AddDllDirectoryFn>(GetProcAddress(kernel, "AddDllDirectory"));
		if (setDefault && addDir) {
			setDefault(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
			addDir(dllDir);
		}
	}
	SetDllDirectoryW(dllDir);
	PrependPath(dllDir);
	return true;
}

bool ResolveInstallFile(const wchar_t* relative, wchar_t* outPath, int outPathChars) {
	if (!relative || !relative[0] || !outPath || outPathChars <= 0) {
		return false;
	}
	wchar_t root[MAX_PATH] = { 0 };
	wchar_t dllDir[MAX_PATH] = { 0 };
	if (!GetInstallRoot(root, MAX_PATH)) {
		return false;
	}
	if (UsesDllSubdirectory()
		&& SUCCEEDED(PathCchCombine(dllDir, MAX_PATH, root, kDllDirName))
		&& SUCCEEDED(PathCchCombine(outPath, (size_t)outPathChars, dllDir, relative))
		&& PathFileExistsW(outPath)) {
		return true;
	}
	return SUCCEEDED(PathCchCombine(outPath, (size_t)outPathChars, root, relative))
		&& PathFileExistsW(outPath);
}

} // namespace install
} // namespace sf4e
