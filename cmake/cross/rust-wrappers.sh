#!/usr/bin/env bash
# Wrappers the Rust helper's build needs for x86_64-pc-windows-msvc on Linux:
# build.rs calls rc.exe, the cc crate calls clang-cl, ml64.exe and the linker.
# $1 is the folder they go in. Needs SF4E_XWIN and SF4E_LLVM_BIN.
set -euo pipefail
out="$1"; mkdir -p "$out"
X="$SF4E_XWIN"; L="$SF4E_LLVM_BIN"
inc="/imsvc $X/crt/include /imsvc $X/sdk/include/ucrt /imsvc $X/sdk/include/um /imsvc $X/sdk/include/shared /imsvc $X/sdk/include/winrt"

cat > "$out/clang-cl-x64" <<W
#!/usr/bin/env bash
exec "$L/clang-cl" --target=x86_64-pc-windows-msvc $inc "\$@"
W

cat > "$out/lld-link-x64" <<W
#!/usr/bin/env bash
export LIB="$X/crt/lib/x86_64;$X/sdk/lib/ucrt/x86_64;$X/sdk/lib/um/x86_64"
exec "$L/lld-link" "\$@"
W

cat > "$out/rc.exe" <<W
#!/usr/bin/env bash
args=(); for a in "\$@"; do case "\${a,,}" in /nologo|-nologo) ;; *) args+=("\$a");; esac; done
exec "$L/llvm-rc" /I $X/sdk/include/um /I $X/sdk/include/shared /I $X/sdk/include/ucrt /I $X/crt/include "\${args[@]}"
W

cat > "$out/ml64.exe" <<W
#!/usr/bin/env bash
exec "$L/llvm-ml" -m64 "\$@"
W

chmod +x "$out"/*
