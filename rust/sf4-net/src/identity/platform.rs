//! Windows facilities the identity store needs: current-user DPAPI, Wine
//! detection, the per-user data folder, a restricted directory, and file
//! installs that never replace an existing file.
use std::{io, path::Path, path::PathBuf};

use zeroize::Zeroizing;

/// Fixed, versioned DPAPI entropy. It separates Ember's blobs from other
/// applications'; it is not a secret.
#[cfg(windows)]
const DPAPI_ENTROPY: &[u8] = b"EMBER:KEY-DPAPI:1\n";

/// `%LOCALAPPDATA%\Ember\Identity\v1`, resolved through the shell API.
#[cfg(windows)]
pub fn default_directory() -> io::Result<PathBuf> {
    use std::{ffi::OsString, os::windows::ffi::OsStringExt, ptr};
    use windows_sys::Win32::{
        System::Com::CoTaskMemFree,
        UI::Shell::{FOLDERID_LocalAppData, KF_FLAG_DEFAULT, SHGetKnownFolderPath},
    };
    unsafe {
        let mut path = ptr::null_mut();
        let result = SHGetKnownFolderPath(
            &FOLDERID_LocalAppData,
            KF_FLAG_DEFAULT as u32,
            ptr::null_mut(),
            &mut path,
        );
        if result != 0 || path.is_null() {
            if !path.is_null() {
                CoTaskMemFree(path.cast());
            }
            return Err(io::Error::other(
                "local application data folder is unavailable",
            ));
        }
        let mut length = 0;
        while *path.add(length) != 0 {
            length += 1;
        }
        let folder = OsString::from_wide(std::slice::from_raw_parts(path, length));
        CoTaskMemFree(path.cast());
        Ok(PathBuf::from(folder).join(r"Ember\Identity\v1"))
    }
}

#[cfg(not(windows))]
pub fn default_directory() -> io::Result<PathBuf> {
    Err(io::Error::other("the identity store requires Windows"))
}

/// True inside Wine or Proton. Wine's DPAPI is not assumed to protect keys
/// the way Windows does, so the encrypted-file backend is used there.
#[cfg(windows)]
pub fn running_under_wine() -> bool {
    use windows_sys::{
        Win32::System::LibraryLoader::{GetModuleHandleW, GetProcAddress},
        w,
    };
    unsafe {
        let ntdll = GetModuleHandleW(w!("ntdll.dll"));
        !ntdll.is_null() && GetProcAddress(ntdll, c"wine_get_version".as_ptr().cast()).is_some()
    }
}

#[cfg(not(windows))]
pub fn running_under_wine() -> bool {
    false
}

/// Creates `directory` and its parents. The final directory gets a
/// protected DACL for the current user and SYSTEM only; existing
/// directories keep their ACL.
#[cfg(windows)]
pub fn create_private_directory(directory: &Path) -> io::Result<()> {
    use std::{mem, os::windows::ffi::OsStrExt};
    use windows_sys::Win32::{
        Foundation::{ERROR_ALREADY_EXISTS, GetLastError},
        Security::SECURITY_ATTRIBUTES,
        Storage::FileSystem::CreateDirectoryW,
    };
    if directory.is_dir() {
        return Ok(());
    }
    if let Some(parent) = directory.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let descriptor = crate::ipc::current_user_descriptor(|sid| {
        format!("D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;{sid})")
    })?;
    let attributes = SECURITY_ATTRIBUTES {
        nLength: mem::size_of::<SECURITY_ATTRIBUTES>() as u32,
        lpSecurityDescriptor: descriptor.0,
        bInheritHandle: 0,
    };
    let wide: Vec<u16> = directory.as_os_str().encode_wide().chain(Some(0)).collect();
    if unsafe { CreateDirectoryW(wide.as_ptr(), &attributes) } == 0
        && unsafe { GetLastError() } != ERROR_ALREADY_EXISTS
    {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}

#[cfg(not(windows))]
pub fn create_private_directory(directory: &Path) -> io::Result<()> {
    std::fs::create_dir_all(directory)
}

/// Moves `from` to `to` only if `to` does not exist, writing through to disk.
#[cfg(windows)]
pub fn install_new(from: &Path, to: &Path) -> io::Result<()> {
    use std::os::windows::ffi::OsStrExt;
    use windows_sys::Win32::Storage::FileSystem::{MOVEFILE_WRITE_THROUGH, MoveFileExW};
    let wide =
        |path: &Path| -> Vec<u16> { path.as_os_str().encode_wide().chain(Some(0)).collect() };
    if unsafe {
        MoveFileExW(
            wide(from).as_ptr(),
            wide(to).as_ptr(),
            MOVEFILE_WRITE_THROUGH,
        )
    } == 0
    {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}

#[cfg(not(windows))]
pub fn install_new(from: &Path, to: &Path) -> io::Result<()> {
    std::fs::hard_link(from, to)?;
    std::fs::remove_file(from)
}

/// Moves `from` over `to`, writing through to disk. Used only for the public
/// continuity file and for retiring a store, never to replace a key.
#[cfg(windows)]
pub fn replace(from: &Path, to: &Path) -> io::Result<()> {
    use std::os::windows::ffi::OsStrExt;
    use windows_sys::Win32::Storage::FileSystem::{
        MOVEFILE_REPLACE_EXISTING, MOVEFILE_WRITE_THROUGH, MoveFileExW,
    };
    let wide =
        |path: &Path| -> Vec<u16> { path.as_os_str().encode_wide().chain(Some(0)).collect() };
    let flags = MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH;
    if unsafe { MoveFileExW(wide(from).as_ptr(), wide(to).as_ptr(), flags) } == 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}

#[cfg(not(windows))]
pub fn replace(from: &Path, to: &Path) -> io::Result<()> {
    std::fs::rename(from, to)
}

/// Current-user DPAPI with UI forbidden and no machine scope.
#[cfg(windows)]
pub fn protect(plain: &[u8]) -> io::Result<Vec<u8>> {
    use windows_sys::Win32::Security::Cryptography::{
        CRYPT_INTEGER_BLOB, CRYPTPROTECT_UI_FORBIDDEN, CryptProtectData,
    };
    let input = blob(plain);
    let entropy = blob(DPAPI_ENTROPY);
    let mut output = CRYPT_INTEGER_BLOB {
        cbData: 0,
        pbData: std::ptr::null_mut(),
    };
    let ok = unsafe {
        CryptProtectData(
            &input,
            std::ptr::null(),
            &entropy,
            std::ptr::null(),
            std::ptr::null(),
            CRYPTPROTECT_UI_FORBIDDEN,
            &mut output,
        )
    };
    if ok == 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(take(output, false).to_vec())
}

#[cfg(windows)]
pub fn unprotect(sealed: &[u8]) -> io::Result<Zeroizing<Vec<u8>>> {
    use windows_sys::Win32::Security::Cryptography::{
        CRYPT_INTEGER_BLOB, CRYPTPROTECT_UI_FORBIDDEN, CryptUnprotectData,
    };
    let input = blob(sealed);
    let entropy = blob(DPAPI_ENTROPY);
    let mut output = CRYPT_INTEGER_BLOB {
        cbData: 0,
        pbData: std::ptr::null_mut(),
    };
    let ok = unsafe {
        CryptUnprotectData(
            &input,
            std::ptr::null_mut(),
            &entropy,
            std::ptr::null(),
            std::ptr::null(),
            CRYPTPROTECT_UI_FORBIDDEN,
            &mut output,
        )
    };
    if ok == 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(take(output, true))
}

#[cfg(not(windows))]
pub fn protect(_: &[u8]) -> io::Result<Vec<u8>> {
    Err(io::Error::other("DPAPI requires Windows"))
}

#[cfg(not(windows))]
pub fn unprotect(_: &[u8]) -> io::Result<Zeroizing<Vec<u8>>> {
    Err(io::Error::other("DPAPI requires Windows"))
}

#[cfg(windows)]
fn blob(data: &[u8]) -> windows_sys::Win32::Security::Cryptography::CRYPT_INTEGER_BLOB {
    windows_sys::Win32::Security::Cryptography::CRYPT_INTEGER_BLOB {
        cbData: data.len() as u32,
        pbData: data.as_ptr().cast_mut(),
    }
}

/// Copies a DPAPI output buffer, then wipes (if it held plaintext) and frees
/// the OS allocation.
#[cfg(windows)]
fn take(
    output: windows_sys::Win32::Security::Cryptography::CRYPT_INTEGER_BLOB,
    secret: bool,
) -> Zeroizing<Vec<u8>> {
    use windows_sys::Win32::Foundation::LocalFree;
    use zeroize::Zeroize;
    if output.pbData.is_null() {
        return Zeroizing::new(Vec::new());
    }
    let data = unsafe { std::slice::from_raw_parts_mut(output.pbData, output.cbData as usize) };
    let copy = Zeroizing::new(data.to_vec());
    if secret {
        data.zeroize();
    }
    unsafe {
        LocalFree(output.pbData.cast());
    }
    copy
}

#[cfg(all(test, windows))]
mod tests {
    use super::*;

    #[test]
    fn dpapi_round_trip_and_tamper() {
        let sealed = protect(b"seed material").unwrap();
        assert_eq!(unprotect(&sealed).unwrap().as_slice(), b"seed material");
        let mut tampered = sealed.clone();
        let last = tampered.len() - 1;
        tampered[last] ^= 0x55;
        assert!(unprotect(&tampered).is_err());
        assert!(unprotect(b"not a dpapi blob").is_err());
    }

    #[test]
    fn install_new_never_replaces() {
        let dir = std::env::temp_dir().join(format!("sf4-identity-install-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let (a, b, target) = (dir.join("a"), dir.join("b"), dir.join("target"));
        std::fs::write(&a, b"first").unwrap();
        std::fs::write(&b, b"second").unwrap();
        install_new(&a, &target).unwrap();
        assert!(install_new(&b, &target).is_err());
        assert_eq!(std::fs::read(&target).unwrap(), b"first");
        assert!(b.exists());
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn native_windows_is_not_wine() {
        // The test runner is native Windows; under Wine this reports true.
        if std::env::var_os("WINEPREFIX").is_none() {
            assert!(!running_under_wine());
        }
        assert!(default_directory().unwrap().ends_with(r"Ember\Identity\v1"));
    }
}
