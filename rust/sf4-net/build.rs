use std::{env, path::PathBuf, process::Command};

fn main() {
    println!("cargo:rerun-if-changed=resource.rc");
    println!("cargo:rerun-if-changed=../../src/ui/ember.ico");
    if env::var("CARGO_CFG_TARGET_ENV").as_deref() != Ok("msvc") {
        return;
    }
    let output = PathBuf::from(env::var_os("OUT_DIR").expect("OUT_DIR")).join("ember.res");
    let status = Command::new("rc.exe")
        .args(["/nologo", "/fo"])
        .arg(&output)
        .arg("resource.rc")
        .status()
        .expect(
            "Windows SDK resource compiler must be on PATH (use a Visual Studio developer prompt)",
        );
    assert!(status.success(), "Ember helper resource compilation failed");
    println!("cargo:rustc-link-arg-bins={}", output.display());
}
