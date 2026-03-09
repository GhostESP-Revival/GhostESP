use std::env;
use std::fs;
use std::path::PathBuf;

fn main() {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").expect("missing manifest dir"));
    let out_dir = PathBuf::from(env::var("OUT_DIR").expect("missing out dir"));
    let generated = out_dir.join("embedded_worker.rs");
    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap_or_else(|_| "windows".to_string());
    let worker_name = if target_os == "windows" {
        "rave_worker.exe"
    } else {
        "rave_worker"
    };
    let worker_path = manifest_dir.join("..").join(worker_name);

    println!("cargo:rerun-if-changed={}", worker_path.display());

    let content = if worker_path.exists() {
        let canonical = worker_path
            .canonicalize()
            .unwrap_or(worker_path.clone())
            .to_string_lossy()
            .to_string();
        format!(
            "pub const EMBEDDED_WORKER: Option<&[u8]> = Some(include_bytes!(r#\"{}\"#));\n",
            canonical
        )
    } else {
        "pub const EMBEDDED_WORKER: Option<&[u8]> = None;\n".to_string()
    };

    fs::write(generated, content).expect("failed to write embedded worker metadata");
}
