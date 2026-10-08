use std::path::PathBuf;

pub struct Temp(pub PathBuf);
impl Temp {
    pub fn new() -> Self {
        let path =
            std::env::temp_dir().join(format!("ember-reports-test-{}", uuid::Uuid::new_v4()));
        std::fs::create_dir(&path).unwrap();
        Self(path)
    }
}
impl Drop for Temp {
    fn drop(&mut self) {
        assert_eq!(self.0.parent(), Some(std::env::temp_dir().as_path()));
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
