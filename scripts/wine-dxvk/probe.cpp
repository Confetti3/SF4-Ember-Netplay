// Wine/DXVK behaviour probe for SF4 Ember Netplay.
// Build: i686-w64-mingw32-g++-posix -O2 -static probe.cpp -o probe.exe -lws2_32 -ld3d9 -lwinmm
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <d3d9.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <algorithm>
#include <atomic>
#include <string>

static double g_tickMs;
static double NowMs() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart * g_tickMs; }

struct Stats { double p50, p95, p99, max, mean; int over1, over2; };
static Stats Summ(std::vector<double> v, double target) {
	std::sort(v.begin(), v.end());
	Stats s{}; double sum = 0;
	for (double x : v) { sum += x; if (x > target + 1.0) s.over1++; if (x > target + 2.0) s.over2++; }
	s.mean = sum / v.size(); s.p50 = v[v.size() / 2]; s.p95 = v[v.size() * 95 / 100];
	s.p99 = v[v.size() * 99 / 100]; s.max = v.back(); return s;
}
static void Print(const char* name, const std::vector<double>& v, double target) {
	Stats s = Summ(v, target);
	printf("  %-46s n=%4zu mean=%7.3f p50=%7.3f p95=%7.3f p99=%7.3f max=%7.3f  >+1ms=%d >+2ms=%d\n",
		name, v.size(), s.mean, s.p50, s.p95, s.p99, s.max, s.over1, s.over2);
}

// ---------------------------------------------------------------- timers
static void TimerTests() {
	printf("\n[timers]\n");
	LARGE_INTEGER f; QueryPerformanceFrequency(&f);
	printf("  QPC frequency: %lld Hz\n", f.QuadPart);
	for (int period = 0; period <= 1; ++period) {
		if (period) timeBeginPeriod(1);
		std::vector<double> s1, s0, w2;
		for (int i = 0; i < 300; ++i) { double t = NowMs(); Sleep(1); s1.push_back(NowMs() - t); }
		for (int i = 0; i < 300; ++i) { double t = NowMs(); Sleep(0); s0.push_back(NowMs() - t); }
		HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		for (int i = 0; i < 300; ++i) { double t = NowMs(); WaitForSingleObject(ev, 2); w2.push_back(NowMs() - t); }
		CloseHandle(ev);
		printf(" %s\n", period ? "with timeBeginPeriod(1):" : "default timer period:");
		Print("Sleep(1)", s1, 1.0); Print("Sleep(0)", s0, 0.0); Print("WaitForSingleObject(event, 2)", w2, 2.0);
		if (period) timeEndPeriod(1);
	}
}

// ------------------------------------------------ HelperClient-style poller
// Mirrors src/platform/HelperClient.cxx: PeekNamedPipe then WaitForSingleObject(stop, 2).
static std::atomic<bool> g_pollStop{false};
static std::atomic<long long> g_polls{0};
static double g_peekUsTotal = 0;
static DWORD WINAPI PipeServer(LPVOID name) {
	HANDLE p = CreateNamedPipeW((LPCWSTR)name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
		PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, nullptr);
	ConnectNamedPipe(p, nullptr);
	while (!g_pollStop.load()) Sleep(50);
	CloseHandle(p); return 0;
}
static DWORD WINAPI Poller(LPVOID name) {
	HANDLE pipe = INVALID_HANDLE_VALUE;
	for (int i = 0; i < 3000 && pipe == INVALID_HANDLE_VALUE; ++i) {
		pipe = CreateFileW((LPCWSTR)name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
		if (pipe == INVALID_HANDLE_VALUE) Sleep(5);
	}
	if (pipe == INVALID_HANDLE_VALUE) { printf("  poller: pipe open failed %lu\n", GetLastError()); return 1; }
	HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	double peekTotal = 0; long long n = 0;
	while (!g_pollStop.load()) {
		DWORD available = 0; double t = NowMs();
		if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) { printf("  poller: PeekNamedPipe failed %lu\n", GetLastError()); break; }
		peekTotal += NowMs() - t; ++n;
		WaitForSingleObject(stop, 2);
	}
	g_polls = n; g_peekUsTotal = n ? peekTotal * 1000.0 / n : 0;
	CloseHandle(stop); CloseHandle(pipe); return 0;
}

// --------------------------------------------------------------- D3D9
struct Gfx { HWND hwnd = nullptr; IDirect3D9* d3d = nullptr; IDirect3DDevice9* dev = nullptr; };
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }
static bool GfxInit(Gfx& g, UINT interval) {
	WNDCLASSW wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"probe";
	RegisterClassW(&wc);
	g.hwnd = CreateWindowW(L"probe", L"probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 640, 360, nullptr, nullptr, wc.hInstance, nullptr);
	g.d3d = Direct3DCreate9(D3D_SDK_VERSION);
	if (!g.d3d) { printf("  Direct3DCreate9 failed\n"); return false; }
	D3DADAPTER_IDENTIFIER9 id = {}; g.d3d->GetAdapterIdentifier(0, 0, &id);
	printf("  adapter: %s (driver %s)\n", id.Description, id.Driver);
	D3DPRESENT_PARAMETERS pp = {}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_UNKNOWN;
	pp.hDeviceWindow = g.hwnd; pp.PresentationInterval = interval;
	HRESULT hr = g.d3d->CreateDevice(0, D3DDEVTYPE_HAL, g.hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &g.dev);
	if (FAILED(hr)) { printf("  CreateDevice failed 0x%08lx\n", hr); return false; }
	return true;
}
static void GfxFrame(Gfx& g, int i, double* presentMs) {
	MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
	g.dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(i & 255, 64, 32), 1.0f, 0);
	g.dev->BeginScene(); g.dev->EndScene();
	double t = NowMs(); g.dev->Present(nullptr, nullptr, nullptr, nullptr); *presentMs = NowMs() - t;
}
static void GfxShutdown(Gfx& g) { if (g.dev) g.dev->Release(); if (g.d3d) g.d3d->Release(); if (g.hwnd) DestroyWindow(g.hwnd); g = Gfx(); }

// ------------------------------------------------------- limiter emulation
// src/Dimps/Dimps__Platform.hxx: "spins until periodSeconds after lastLimiterExit".
// mode 0: pure spin; mode 1: Sleep(1) while >2 ms remain, then spin (a common engine pattern).
static void Limiter(const char* name, int frames, int mode, Gfx* g, bool poller) {
	const double period = 1000.0 / 60.0;
	std::vector<double> frame, present;
	HANDLE server = nullptr, client = nullptr;
	static int pipeSeq = 0; wchar_t pipeName[128];
	swprintf(pipeName, 128, L"\\\\.\\pipe\\ember-probe-%lu-%d", GetCurrentProcessId(), ++pipeSeq);
	if (poller) {
		g_pollStop = false;
		server = CreateThread(nullptr, 0, PipeServer, pipeName, 0, nullptr);
		Sleep(50);
		client = CreateThread(nullptr, 0, Poller, pipeName, 0, nullptr);
		Sleep(100);
	}
	double lastExit = NowMs();
	for (int i = 0; i < frames; ++i) {
		double p = 0;
		if (g) { GfxFrame(*g, i, &p); present.push_back(p); }
		const double deadline = lastExit + period;
		if (mode == 1) while (deadline - NowMs() > 2.0) Sleep(1);
		while (NowMs() < deadline) { YieldProcessor(); }
		const double exit = NowMs();
		frame.push_back(exit - lastExit);
		lastExit = exit;
	}
	if (poller) {
		g_pollStop = true; WaitForSingleObject(client, 5000); WaitForSingleObject(server, 5000);
		CloseHandle(client); CloseHandle(server);
	}
	Print(name, frame, period);
	if (g) Print("   (Present() call duration)", present, 0.0);
	if (poller) printf("   (poller: %lld PeekNamedPipe calls, mean %.1f us each)\n", g_polls.load(), g_peekUsTotal);
}

// --------------------------------------------------------------- winsock
static void SockTests() {
	printf("\n[winsock]\n");
	WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
	// 1. IrohMatchSession port reservation then GGPO-style SO_REUSEADDR INADDR_ANY bind (upstream ggpo udp.cpp).
	int failures = 0, firstErr = 0;
	for (int i = 0; i < 500; ++i) {
		SOCKET r = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		sockaddr_in a = {}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		bind(r, (sockaddr*)&a, sizeof(a)); int len = sizeof(a); getsockname(r, (sockaddr*)&a, &len);
		const u_short port = ntohs(a.sin_port);
		// Helper bridge binds its own loopback ephemeral socket and sends to the
		// reserved port. It stays unconnected (F-008, rust/sf4-net/src/bridge.rs).
		SOCKET bridge = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		sockaddr_in b = {}; b.sin_family = AF_INET; b.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		bind(bridge, (sockaddr*)&b, sizeof(b));
		closesocket(r); // ReleasePortToGgpo
		SOCKET g = socket(AF_INET, SOCK_DGRAM, 0);
		BOOL on = TRUE; setsockopt(g, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on));
		u_long nb = 1; ioctlsocket(g, FIONBIO, &nb);
		sockaddr_in any = {}; any.sin_family = AF_INET; any.sin_addr.s_addr = htonl(INADDR_ANY); any.sin_port = htons(port);
		if (bind(g, (sockaddr*)&any, sizeof(any)) != 0) { if (!failures++) firstErr = WSAGetLastError(); }
		else {
			// Packet from bridge must reach GGPO socket.
			sendto(bridge, "x", 1, 0, (sockaddr*)&a, sizeof(a)); Sleep(0);
			char buf[8]; sockaddr_in from; int fl = sizeof(from); int got = -1;
			for (int k = 0; k < 200 && got < 0; ++k) { got = recvfrom(g, buf, 8, 0, (sockaddr*)&from, &fl); if (got < 0) Sleep(1); }
			if (got != 1) { if (!failures++) firstErr = -1; }
		}
		closesocket(g); closesocket(bridge);
	}
	printf("  reserve->release->GGPO SO_REUSEADDR rebind + bridge delivery x500: %d failures (first error %d)\n", failures, firstErr);

	// 2. The bridge as it was before F-008: a connected UDP socket whose GGPO
	// peer socket has closed (between games). The bridge no longer connects;
	// this reproduces the Wine behaviour that made that change necessary.
	{
		SOCKET peer = socket(AF_INET, SOCK_DGRAM, 0);
		sockaddr_in a = {}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		bind(peer, (sockaddr*)&a, sizeof(a)); int len = sizeof(a); getsockname(peer, (sockaddr*)&a, &len);
		SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
		sockaddr_in b = {}; b.sin_family = AF_INET; b.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		bind(s, (sockaddr*)&b, sizeof(b)); connect(s, (sockaddr*)&a, sizeof(a));
		u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
		closesocket(peer);
		int r1 = send(s, "abc", 3, 0); int e1 = r1 < 0 ? WSAGetLastError() : 0; Sleep(20);
		char buf[16]; int r2 = recv(s, buf, 16, 0); int e2 = r2 < 0 ? WSAGetLastError() : 0;
		int r3 = send(s, "abc", 3, 0); int e3 = r3 < 0 ? WSAGetLastError() : 0; Sleep(20);
		int r4 = send(s, "abc", 3, 0); int e4 = r4 < 0 ? WSAGetLastError() : 0;
		int r5 = recv(s, buf, 16, 0); int e5 = r5 < 0 ? WSAGetLastError() : 0;
		printf("  connected UDP after GGPO socket closed: send=%d(err %d) recv=%d(err %d) send=%d(err %d) send=%d(err %d) recv=%d(err %d)\n",
			r1, e1, r2, e2, r3, e3, r4, e4, r5, e5);
		printf("    (10054=WSAECONNRESET, 10061=WSAECONNREFUSED, 10035=WSAEWOULDBLOCK; the helper tolerates only 10054/10061)\n");
		closesocket(s);
	}
	// 3. GGPO side: unconnected socket, sendto a closed port, then recvfrom.
	{
		SOCKET dead = socket(AF_INET, SOCK_DGRAM, 0);
		sockaddr_in a = {}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		bind(dead, (sockaddr*)&a, sizeof(a)); int len = sizeof(a); getsockname(dead, (sockaddr*)&a, &len); closesocket(dead);
		SOCKET g = socket(AF_INET, SOCK_DGRAM, 0);
		BOOL on = TRUE; setsockopt(g, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on));
		u_long nb = 1; ioctlsocket(g, FIONBIO, &nb);
		sockaddr_in any = {}; any.sin_family = AF_INET; bind(g, (sockaddr*)&any, sizeof(any));
		int r1 = sendto(g, "abc", 3, 0, (sockaddr*)&a, sizeof(a)); int e1 = r1 < 0 ? WSAGetLastError() : 0; Sleep(20);
		char buf[16]; sockaddr_in from; int fl = sizeof(from);
		int r2 = recvfrom(g, buf, 16, 0, (sockaddr*)&from, &fl); int e2 = r2 < 0 ? WSAGetLastError() : 0;
		printf("  unconnected GGPO socket sendto closed port: sendto=%d(err %d) recvfrom=%d(err %d)\n", r1, e1, r2, e2);
		closesocket(g);
	}
	WSACleanup();
}

int main(int argc, char** argv) {
	setvbuf(stdout, nullptr, _IONBF, 0);
	LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_tickMs = 1000.0 / f.QuadPart;
	std::string only = argc > 1 ? argv[1] : "all";
	if (only == "all" || only == "timers") TimerTests();
	if (only == "all" || only == "sock") SockTests();
	if (only == "all" || only == "limiter") {
		printf("\n[limiter emulation, 60 Hz period 16.667 ms, frame = limiter exit to exit]\n");
		Limiter("spin, idle", 600, 0, nullptr, false);
		Limiter("spin + HelperClient-style 2 ms pipe poller", 600, 0, nullptr, true);
		Limiter("sleep(1)+spin, idle", 600, 1, nullptr, false);
		Limiter("sleep(1)+spin + pipe poller", 600, 1, nullptr, true);
		Gfx g;
		printf(" D3D9 PresentationInterval=IMMEDIATE (what Ember forces):\n");
		if (GfxInit(g, D3DPRESENT_INTERVAL_IMMEDIATE)) {
			Limiter("spin + Present", 600, 0, &g, false);
			Limiter("spin + Present + pipe poller", 600, 0, &g, true);
			GfxShutdown(g);
		}
		printf(" D3D9 PresentationInterval=ONE (VSync on, if Ember's override did not apply):\n");
		if (GfxInit(g, D3DPRESENT_INTERVAL_ONE)) {
			Limiter("spin + Present(vsync)", 600, 0, &g, false);
			GfxShutdown(g);
		}
	}
	return 0;
}
