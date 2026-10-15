#pragma once
// The helper's end of the native pipe, played by the test itself, so an
// IrohRoom runs its production event handling without an sf4-net process.
// It answers the bootstrap handshake, sends the events a test scripts, and
// reads and drops what the room sends back (commands, checkpoint ACKs).
// Windows only: the room's HelperClient talks to a named pipe there.
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../platform/HelperClient.hxx"
#include "test_support.hxx"

class FakeHelperPipe {
public:
	FakeHelperPipe() {
		static unsigned instance = 0;
		wchar_t name[96] = {};
		swprintf_s(name, L"\\\\.\\pipe\\sf4-net-%08lx%08lx%016llx", GetCurrentProcessId(), ++instance, GetTickCount64());
		pipe_ = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 1 << 20, 1 << 20, 0, nullptr);
		CHECK(pipe_ != INVALID_HANDLE_VALUE);
		bootstrap_.helperPid = GetCurrentProcessId();
		wcscpy_s(bootstrap_.pipeName, name);
		for (std::size_t i = 0; i < sizeof(bootstrap_.nonce); ++i) bootstrap_.nonce[i] = static_cast<std::uint8_t>(i * 7 + 1);
	}
	~FakeHelperPipe() { if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_); }
	FakeHelperPipe(const FakeHelperPipe&) = delete;
	FakeHelperPipe& operator=(const FakeHelperPipe&) = delete;

	// Starts `client` on this pipe and completes its handshake.
	void Connect(sf4e::platform::HelperClient& client) {
		CHECK(client.Start(bootstrap_));
		OVERLAPPED operation = {}; operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!ConnectNamedPipe(pipe_, &operation)) {
			const DWORD error = GetLastError();
			CHECK(error == ERROR_PIPE_CONNECTED || error == ERROR_IO_PENDING);
			DWORD ignored = 0;
			if (error == ERROR_IO_PENDING) CHECK(GetOverlappedResult(pipe_, &operation, &ignored, TRUE));
		}
		CloseHandle(operation.hEvent);
		std::uint64_t id = 0; std::string nonce;
		CHECK(ReadFrame(id, nonce) && id == 1 && nonce.size() == sizeof(bootstrap_.nonce));
		CHECK(WriteFrame(1, "SF4N"));
		for (int wait = 0; wait < 5000 && client.State() == sf4e::platform::HelperState::Connecting; ++wait) Sleep(1);
		CHECK(client.State() == sf4e::platform::HelperState::Connected);
	}
	void Send(const nlohmann::json& event) { Drain(); CHECK(WriteFrame(++nextId_, event.dump())); }
	// Reads whatever the room has sent so far; the commands themselves are dropped.
	void Drain() {
		DWORD available = 0;
		while (PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
			std::uint64_t id = 0; std::string payload;
			CHECK(ReadFrame(id, payload));
		}
	}

private:
	bool Transfer(void* data, DWORD size, bool write) {
		OVERLAPPED operation = {}; operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		DWORD done = 0, offset = 0;
		bool ok = true;
		while (ok && offset < size) {
			auto* buffer = static_cast<std::uint8_t*>(data) + offset;
			ResetEvent(operation.hEvent);
			const BOOL started = write ? WriteFile(pipe_, buffer, size - offset, &done, &operation)
				: ReadFile(pipe_, buffer, size - offset, &done, &operation);
			ok = (started || GetLastError() == ERROR_IO_PENDING) && GetOverlappedResult(pipe_, &operation, &done, TRUE) && done;
			offset += done;
		}
		CloseHandle(operation.hEvent);
		return ok;
	}
	// The frame HelperClient reads and writes: a big-endian length of 10 plus
	// the payload, the version bytes 0 1, a big-endian id, then the payload.
	bool WriteFrame(std::uint64_t id, const std::string& payload) {
		std::vector<std::uint8_t> bytes(14 + payload.size());
		const auto length = static_cast<std::uint32_t>(10 + payload.size());
		for (int i = 0; i < 4; ++i) bytes[i] = static_cast<std::uint8_t>(length >> (24 - 8 * i));
		bytes[4] = 0; bytes[5] = 1;
		for (int i = 0; i < 8; ++i) bytes[6 + i] = static_cast<std::uint8_t>(id >> (56 - 8 * i));
		std::copy(payload.begin(), payload.end(), bytes.begin() + 14);
		return Transfer(bytes.data(), static_cast<DWORD>(bytes.size()), true);
	}
	bool ReadFrame(std::uint64_t& id, std::string& payload) {
		std::uint8_t header[14];
		if (!Transfer(header, sizeof(header), false)) return false;
		std::uint32_t length = 0;
		for (int i = 0; i < 4; ++i) length = (length << 8) | header[i];
		id = 0;
		for (int i = 0; i < 8; ++i) id = (id << 8) | header[6 + i];
		if (length <= 10) return false;
		payload.resize(length - 10);
		return Transfer(&payload[0], static_cast<DWORD>(payload.size()), false);
	}

	HANDLE pipe_ = INVALID_HANDLE_VALUE;
	sf4e::platform::HelperBootstrap bootstrap_;
	std::uint64_t nextId_ = 1;
};
