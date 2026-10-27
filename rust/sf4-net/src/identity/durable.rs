//! A file written so a crash leaves the old content or the new, never half of
//! it: a synced temporary file beside it, then a move into place that writes
//! through to disk.
use std::{
    fs::{self, OpenOptions},
    io::{self, Write},
    path::Path,
};

use ember_protocol::encoding::b64u;

use super::{TEMP_PREFIX, platform};

/// What to do with a file already at the target.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Existing {
    /// The new content takes its place.
    Replace,
    /// The old file stays, and the write fails with `AlreadyExists`.
    Keep,
}

#[derive(Debug)]
pub(crate) enum Error {
    /// The system had no random bytes for the temporary file's name.
    Random,
    Io(io::Error),
}

impl From<io::Error> for Error {
    fn from(error: io::Error) -> Self {
        Self::Io(error)
    }
}

/// Writes `bytes` to `path` through a sibling temporary file named
/// `.tmp-<name>-<random>`, which a folder listing by another prefix never
/// sees, and removes the temporary file when anything fails.
pub(crate) fn replace(path: &Path, bytes: &[u8], existing: Existing) -> Result<(), Error> {
    let name = path
        .file_name()
        .ok_or_else(|| Error::Io(io::ErrorKind::InvalidInput.into()))?
        .to_string_lossy();
    let mut suffix = [0u8; 8];
    getrandom::fill(&mut suffix).map_err(|_| Error::Random)?;
    let temp = path.with_file_name(format!("{TEMP_PREFIX}{name}-{}", b64u(&suffix)));
    let result = (|| {
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&temp)?;
        file.write_all(bytes)?;
        file.sync_all()?;
        drop(file);
        match existing {
            Existing::Replace => platform::replace(&temp, path),
            Existing::Keep => platform::install_new(&temp, path),
        }
    })();
    if let Err(error) = result {
        let _ = fs::remove_file(&temp);
        return Err(Error::Io(error));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn folder(tag: &str) -> std::path::PathBuf {
        let mut seed = [0u8; 8];
        getrandom::fill(&mut seed).unwrap();
        let dir = std::env::temp_dir().join(format!("sf4-durable-{tag}-{}", b64u(&seed)));
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn names(dir: &Path) -> Vec<String> {
        let mut names: Vec<String> = fs::read_dir(dir)
            .unwrap()
            .map(|entry| entry.unwrap().file_name().to_string_lossy().into_owned())
            .collect();
        names.sort();
        names
    }

    #[test]
    fn a_write_replaces_the_file_and_leaves_no_temporary_one() {
        let dir = folder("replace");
        let path = dir.join("state.json");
        replace(&path, b"first", Existing::Replace).unwrap();
        assert_eq!(fs::read(&path).unwrap(), b"first");
        replace(&path, b"second", Existing::Replace).unwrap();
        assert_eq!(fs::read(&path).unwrap(), b"second");
        assert_eq!(names(&dir), ["state.json"]);
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn a_write_that_keeps_the_existing_file_fails_and_cleans_up() {
        let dir = folder("keep");
        let path = dir.join("report-1.json");
        replace(&path, b"first", Existing::Keep).unwrap();
        assert_eq!(fs::read(&path).unwrap(), b"first");
        let refused = replace(&path, b"second", Existing::Keep).unwrap_err();
        assert!(matches!(
            refused,
            Error::Io(ref error) if error.kind() == io::ErrorKind::AlreadyExists
        ));
        assert_eq!(fs::read(&path).unwrap(), b"first");
        assert_eq!(names(&dir), ["report-1.json"]);
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn a_write_into_a_missing_folder_fails_and_leaves_nothing() {
        let dir = folder("temp");
        let missing = dir.join("absent").join("report-1.json");
        assert!(replace(&missing, b"x", Existing::Keep).is_err());
        assert!(names(&dir).is_empty());
        fs::remove_dir_all(dir).unwrap();
    }
}
