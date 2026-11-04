#pragma once
// The helper (sf4-net.exe, built from rust/sf4-net) is one program that every
// Ember product ships and starts the same way, so its file names and the form
// of its pipe names are Ember's contract with it, not a product's. The other
// side of the pipe name is valid_pipe_name in rust/sf4-net/src/ipc.rs; the two
// change together. Products installed side by side may share the prefix: each
// pipe name carries 128 random bits after it (HelperProcess.cxx).
//
// C++11, no Windows headers: the Discord companion and the launcher read it too.

namespace sf4e { namespace platform { namespace helper_pipe {

// A pipe name is this and 32 lowercase hex digits.
constexpr wchar_t Prefix[] = L"\\\\.\\pipe\\sf4-net-";
constexpr unsigned HexDigits = 32;

// The helper's executable, beside the launcher.
constexpr wchar_t Executable[] = L"sf4-net.exe";
// The helper's log, under the product's logs folder.
constexpr wchar_t Log[] = L"sf4-net.log";

} } }
