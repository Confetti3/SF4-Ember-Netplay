//! The launcher passes a nonce through a single inherited anonymous pipe. The
//! game connects through a current-user DACL and both processes verify pipe PIDs.
use std::{
    ffi::c_void,
    io, mem,
    os::windows::io::AsRawHandle,
    ptr,
    time::{Duration, Instant},
};

use subtle::ConstantTimeEq;
use tokio::{
    net::windows::named_pipe::{NamedPipeServer, ServerOptions},
    time::timeout,
};
use windows_sys::Win32::{
    Foundation::{CloseHandle, HANDLE, INVALID_HANDLE_VALUE, LocalFree},
    Security::{
        Authorization::{
            ConvertSidToStringSidW, ConvertStringSecurityDescriptorToSecurityDescriptorW,
        },
        GetTokenInformation, SECURITY_ATTRIBUTES, TOKEN_QUERY, TOKEN_USER, TokenUser,
    },
    Storage::FileSystem::ReadFile,
    System::{
        Console::{GetStdHandle, STD_INPUT_HANDLE},
        Pipes::{GetNamedPipeClientProcessId, PeekNamedPipe},
        Threading::{GetCurrentProcess, OpenProcessToken},
    },
};

use crate::wire::{self, ControlFrame};

pub const BOOTSTRAP_BYTES: usize = 4 + 2 + 4 + 32;
// The game may load assets before reaching the normal initialization hook.
// IPC must not start from its DLL entry point under the Windows loader lock.
pub const STARTUP_TIMEOUT: Duration = Duration::from_secs(60);

pub struct Bootstrap {
    pub client_pid: u32,
    nonce: [u8; 32],
}
impl Bootstrap {
    pub fn from_bytes(bytes: &[u8]) -> io::Result<Self> {
        if bytes.len() != BOOTSTRAP_BYTES
            || &bytes[..4] != b"SF4N"
            || bytes[4..6] != wire::VERSION.to_be_bytes()
        {
            return Err(denied());
        }
        let client_pid = u32::from_be_bytes(bytes[6..10].try_into().map_err(|_| denied())?);
        let nonce = bytes[10..].try_into().map_err(|_| denied())?;
        if client_pid == 0 || nonce == [0; 32] {
            return Err(denied());
        }
        Ok(Self { client_pid, nonce })
    }
}
impl Drop for Bootstrap {
    fn drop(&mut self) {
        for byte in &mut self.nonce {
            unsafe {
                ptr::write_volatile(byte, 0);
            }
        }
    }
}
fn denied() -> io::Error {
    io::Error::new(
        io::ErrorKind::PermissionDenied,
        "helper IPC authentication failed",
    )
}

struct Handle(HANDLE);
impl Drop for Handle {
    fn drop(&mut self) {
        unsafe {
            CloseHandle(self.0);
        }
    }
}
struct Local(*mut c_void);
impl Drop for Local {
    fn drop(&mut self) {
        unsafe {
            LocalFree(self.0);
        }
    }
}

/// No secret appears in argv, environment variables, or a persistent file.
pub fn read_inherited_bootstrap() -> io::Result<Bootstrap> {
    let handle = unsafe { GetStdHandle(STD_INPUT_HANDLE) };
    if handle.is_null() || handle == INVALID_HANDLE_VALUE {
        return Err(denied());
    }
    let deadline = Instant::now() + STARTUP_TIMEOUT;
    let mut bytes = [0u8; BOOTSTRAP_BYTES];
    loop {
        let mut available = 0;
        if unsafe {
            PeekNamedPipe(
                handle,
                ptr::null_mut(),
                0,
                ptr::null_mut(),
                &mut available,
                ptr::null_mut(),
            )
        } == 0
        {
            return Err(denied());
        }
        if available >= BOOTSTRAP_BYTES as u32 {
            break;
        }
        if Instant::now() >= deadline {
            return Err(denied());
        }
        std::thread::sleep(Duration::from_millis(5));
    }
    let mut read = 0;
    if unsafe {
        ReadFile(
            handle,
            bytes.as_mut_ptr(),
            bytes.len() as u32,
            &mut read,
            ptr::null_mut(),
        )
    } == 0
        || read as usize != bytes.len()
    {
        return Err(denied());
    }
    let result = Bootstrap::from_bytes(&bytes);
    for byte in &mut bytes {
        unsafe {
            ptr::write_volatile(byte, 0);
        }
    }
    result
}

pub fn valid_pipe_name(name: &str) -> bool {
    name.strip_prefix(r"\\.\pipe\sf4-net-")
        .is_some_and(|suffix| suffix.len() == 32 && suffix.bytes().all(|c| c.is_ascii_hexdigit()))
}

fn current_user_descriptor() -> io::Result<Local> {
    unsafe {
        let mut raw_token = ptr::null_mut();
        if OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut raw_token) == 0 {
            return Err(io::Error::last_os_error());
        }
        let token = Handle(raw_token);
        let mut length = 0;
        GetTokenInformation(token.0, TokenUser, ptr::null_mut(), 0, &mut length);
        if length == 0 || length > 64 * 1024 {
            return Err(denied());
        }
        // Pointer-sized allocation keeps TOKEN_USER/SID pointers aligned.
        let mut user = vec![0usize; (length as usize).div_ceil(mem::size_of::<usize>())];
        if GetTokenInformation(
            token.0,
            TokenUser,
            user.as_mut_ptr().cast(),
            length,
            &mut length,
        ) == 0
        {
            return Err(io::Error::last_os_error());
        }
        let sid = (*(user.as_ptr().cast::<TOKEN_USER>())).User.Sid;
        let mut text = ptr::null_mut();
        if ConvertSidToStringSidW(sid, &mut text) == 0 {
            return Err(io::Error::last_os_error());
        }
        let sid_text = Local(text.cast());
        let mut count = 0;
        while *text.add(count) != 0 {
            count += 1;
        }
        let sid_string =
            String::from_utf16(std::slice::from_raw_parts(text, count)).map_err(|_| denied())?;
        drop(sid_text);
        let descriptor: Vec<u16> = format!("D:P(A;;GA;;;{sid_string})")
            .encode_utf16()
            .chain(Some(0))
            .collect();
        let mut security = ptr::null_mut();
        if ConvertStringSecurityDescriptorToSecurityDescriptorW(
            descriptor.as_ptr(),
            1,
            &mut security,
            ptr::null_mut(),
        ) == 0
        {
            return Err(io::Error::last_os_error());
        }
        Ok(Local(security))
    }
}

pub fn create_server(name: &str) -> io::Result<NamedPipeServer> {
    if !valid_pipe_name(name) {
        return Err(denied());
    }
    let descriptor = current_user_descriptor()?;
    let mut attributes = SECURITY_ATTRIBUTES {
        nLength: mem::size_of::<SECURITY_ATTRIBUTES>() as u32,
        lpSecurityDescriptor: descriptor.0,
        bInheritHandle: 0,
    };
    // The kernel copies the descriptor; both temporary allocations outlive
    // CreateNamedPipe. First-instance protects against an earlier pipe squatter.
    unsafe {
        ServerOptions::new()
            .first_pipe_instance(true)
            .reject_remote_clients(true)
            .max_instances(1)
            .create_with_security_attributes_raw(
                name,
                (&mut attributes as *mut SECURITY_ATTRIBUTES).cast(),
            )
    }
}

pub async fn authenticate(
    mut pipe: NamedPipeServer,
    bootstrap: &Bootstrap,
) -> io::Result<NamedPipeServer> {
    timeout(STARTUP_TIMEOUT, async {
        loop {
            pipe.connect().await?;
            let mut pid = 0;
            if unsafe { GetNamedPipeClientProcessId(pipe.as_raw_handle(), &mut pid) } == 0
                || pid != bootstrap.client_pid
            {
                pipe.disconnect()?;
                continue;
            }
            // A short fixed-size authentication payload is read with the
            // smaller control limit; the wider IPC limit is enabled afterward.
            let frame = wire::read_control(&mut pipe).await?;
            let accepted = frame.message_id == 1
                && frame.payload.len() == 32
                && bool::from(frame.payload.as_slice().ct_eq(&bootstrap.nonce));
            if !accepted {
                return Err(denied());
            }
            wire::write_control(
                &mut pipe,
                &ControlFrame {
                    message_id: 1,
                    payload: b"SF4N".to_vec(),
                },
            )
            .await?;
            return Ok(pipe);
        }
    })
    .await
    .map_err(|_| denied())?
}

#[cfg(test)]
mod tests {
    use super::*;
    use tokio::net::windows::named_pipe::ClientOptions;

    fn bootstrap(pid: u32) -> Bootstrap {
        Bootstrap {
            client_pid: pid,
            nonce: [7; 32],
        }
    }
    fn name() -> String {
        format!(r"\\.\pipe\sf4-net-{:032x}", rand::random::<u128>())
    }

    #[tokio::test]
    async fn pipe_is_exclusive_and_authenticates_expected_process_and_nonce() {
        let name = name();
        let pipe = create_server(&name).unwrap();
        assert!(create_server(&name).is_err());
        let expected = bootstrap(std::process::id());
        let task = tokio::spawn(async move { authenticate(pipe, &expected).await });
        let mut client = ClientOptions::new().open(&name).unwrap();
        wire::write_control(
            &mut client,
            &ControlFrame {
                message_id: 1,
                payload: vec![7; 32],
            },
        )
        .await
        .unwrap();
        assert_eq!(
            wire::read_control(&mut client).await.unwrap().payload,
            b"SF4N"
        );
        assert!(task.await.unwrap().is_ok());
    }

    #[tokio::test]
    async fn incorrect_nonce_is_rejected() {
        let name = name();
        let pipe = create_server(&name).unwrap();
        let expected = bootstrap(std::process::id());
        let task = tokio::spawn(async move { authenticate(pipe, &expected).await });
        let mut client = ClientOptions::new().open(&name).unwrap();
        wire::write_control(
            &mut client,
            &ControlFrame {
                message_id: 1,
                payload: vec![8; 32],
            },
        )
        .await
        .unwrap();
        assert!(task.await.unwrap().is_err());
    }

    #[test]
    fn invalid_bootstrap_and_pipe_names_are_rejected() {
        assert!(Bootstrap::from_bytes(&[0; BOOTSTRAP_BYTES]).is_err());
        assert!(Bootstrap::from_bytes(&[0; BOOTSTRAP_BYTES + 1]).is_err());
        for value in [
            r"\\remote\pipe\sf4-net-0",
            r"\\.\pipe\other",
            r"\\.\pipe\sf4-net-../../../x",
        ] {
            assert!(!valid_pipe_name(value));
        }
    }
}
