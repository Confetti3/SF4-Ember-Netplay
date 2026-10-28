//! Opens a sign-in page in the player's browser. Under Wine and Proton the
//! browser is the system's. Each caller checks the address first, so only a
//! page of the site the sign-in is for is ever opened.
use super::{Code, Failure};

/// The longest address handed to the shell.
const MAX_URL: usize = 2048;

/// Nothing in `url` could break out of it on its way to the shell.
pub(super) fn is_plain(url: &str) -> bool {
    url.len() <= MAX_URL
        && url
            .bytes()
            .all(|byte| byte.is_ascii_graphic() && byte != b'"')
}

#[cfg(windows)]
pub(super) fn open(url: &str) -> Result<(), Failure> {
    use windows_sys::Win32::UI::Shell::ShellExecuteW;
    let wide = |text: &str| text.encode_utf16().chain(Some(0)).collect::<Vec<u16>>();
    let (verb, target) = (wide("open"), wide(url));
    // SAFETY: both strings are NUL-terminated and outlive the call.
    let result = unsafe {
        ShellExecuteW(
            std::ptr::null_mut(),
            verb.as_ptr(),
            target.as_ptr(),
            std::ptr::null(),
            std::ptr::null(),
            1,
        )
    };
    if result as isize > 32 {
        Ok(())
    } else {
        Err(Failure::new(Code::BrowserUnavailable))
    }
}

#[cfg(not(windows))]
pub(super) fn open(_: &str) -> Result<(), Failure> {
    Err(Failure::new(Code::BrowserUnavailable))
}
