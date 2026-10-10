# Giving the game 4 GB of address space

`SSFIV.exe` is a 32-bit program built without the `IMAGE_FILE_LARGE_ADDRESS_AWARE`
flag, so Windows limits it to 2 GB of address space. With the flag set, a 32-bit
process gets 4 GB on 64-bit Windows. That is the headroom larger textures and
higher resolutions need.

## What the flag is

One bit, `0x0020`, in the `Characteristics` field of the COFF header. In the
Steam build of `SSFIV.exe` (7,026,688 bytes) the field is at file offset `0x166`
and holds `0x0103`; with the flag it holds `0x0123`. So the whole change is one
byte: `0x03` becomes `0x23` at offset `0x166`.

The offset is not fixed in general. It is found from the file itself: the
4-byte value at `0x3c` gives the offset of the `PE\0\0` signature, and
`Characteristics` is 22 bytes after that.

Windows reads the flag from the file when it creates the process. Sidecar runs
inside the process after that point, so it cannot set it.

## What the launcher does

Set the environment variable `SF4E_LARGE_ADDRESS=1` before starting the
launcher. The launcher then:

1. reads `SSFIV.exe` and sets the flag in memory
   (`MarkLargeAddressAware`, `src/launcher/LargeAddress.hxx`);
2. writes the result as `SSFIV.ember-laa.exe` in the game folder, or keeps the
   copy already there when it matches (`LargeAddressExecutable`);
3. starts that copy with Sidecar instead of `SSFIV.exe`.

`SSFIV.exe` is never written to. After a game update the copy is rebuilt on the
next start. If the file is not a 32-bit x86 executable, already has the flag, or
the copy cannot be written, the launcher starts `SSFIV.exe` as before and says
so in its log.

To go back, unset the variable. `SSFIV.ember-laa.exe` can be deleted at any
time.

## Things that follow from the different file name

- The update check that waits for the game to close looks for both names.
- Windows names a `LocalDumps` crash dump after the executable. The registry key
  in `docs/guides/USER_NETPLAY.md` is for `SSFIV.exe`; add the same key for
  `SSFIV.ember-laa.exe` to get dumps from the copy.
- Steam still sees app 45760 through `steam_appid.txt`.

## Doing it by hand

Copy `SSFIV.exe`, then change the byte in the copy:

```powershell
$copy = 'SSFIV.ember-laa.exe'
Copy-Item SSFIV.exe $copy
$bytes = [IO.File]::ReadAllBytes($copy)
$pe = [BitConverter]::ToInt32($bytes, 0x3c)
$bytes[$pe + 22] = $bytes[$pe + 22] -bor 0x20
[IO.File]::WriteAllBytes((Resolve-Path $copy), $bytes)
```

## Not yet known

- Whether the game runs correctly once allocations land above 2 GB. Code that
  treats an address as a signed number breaks there. This is why the switch is
  off by default.
- Whether anything outside Ember checks the executable's name or contents.
