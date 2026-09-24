// Stand-in SSFIV.exe for scripts/wine-dxvk/run.sh: Sidecar hooks land in sled.s.
#include <windows.h>
void __stdcall start(void) {
	HANDLE f = CreateFileW(L"stub_ran.txt", GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
	DWORD w; WriteFile(f, "stub main running\r\n", 19, &w, 0); CloseHandle(f);
	Sleep(20000);
	ExitProcess(0);
}
