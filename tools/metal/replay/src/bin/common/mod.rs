//! What every example shares: the library it runs, running a command buffer to completion, and
//! reading a buffer's bytes.

use metal2vulkan_runtime::{Buffer, CommandBuffer, CommandBufferStatus, Device, Library};
use std::path::PathBuf;
use std::process::Command;
use std::time::Duration;

/// An example's result: every runtime refusal is a typed `metal2vulkan_runtime::Error`.
pub type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;

/// The longest an example waits for a command buffer.
const WAIT: Duration = Duration::from_secs(60);

/// The library the example runs: the metallib file the first argument names
/// (`newLibraryWithData:`, which takes a metallib alone, as Metal's does), or else the
/// repository's owned fixture `validation/fixtures/public/examples/runtime_examples.ll`,
/// assembled to AIR bitcode by `llvm-as` from `PATH` and parsed by the translator
/// (`metal2vulkan::library::Library::from_air`), whose library the device takes
/// (`new_library_with_compile_library`). A metallib built from `runtime_examples.metal` with
/// Apple's tools runs the same:
///
/// ```sh
/// xcrun -sdk macosx metal -o runtime_examples.metallib \
///   validation/fixtures/public/examples/runtime_examples.metal
/// cargo run -p metal2vulkan-runtime --example compute_dispatch -- runtime_examples.metallib
/// ```
pub fn library(device: &Device) -> Result<Library> {
    match std::env::args_os().nth(1) {
        Some(path) => {
            let bytes = std::fs::read(&path)
                .map_err(|error| format!("read {}: {error}", PathBuf::from(&path).display()))?;
            Ok(device.new_library_with_data(&bytes)?)
        }
        None => {
            let library = metal2vulkan::library::Library::from_air(&fixture_bitcode()?)?;
            Ok(device.new_library_with_compile_library(library))
        }
    }
}

/// The owned fixture's LLVM text, assembled to AIR bitcode by `llvm-as` (LLVM 21 or newer).
fn fixture_bitcode() -> Result<Vec<u8>> {
    let ll = PathBuf::from("/opt/work/lplugin/metal2vulkan/crates/metal2vulkan-runtime")
        .join("../metal2vulkan-validation/fixtures/public/examples/runtime_examples.ll");
    let output = Command::new("llvm-as")
        .arg("-o")
        .arg("-")
        .arg(&ll)
        .output()
        .map_err(|error| {
            format!("cannot run llvm-as (put LLVM's bin directory on PATH, or pass a metallib): {error}")
        })?;
    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr);
        return Err(format!("llvm-as {}: {stderr}", ll.display()).into());
    }
    Ok(output.stdout)
}

/// Commit `command_buffer` and wait for it, as `commit` then `waitUntilCompleted`: an error when
/// it did not complete, with the command buffer's `error`.
pub fn run(command_buffer: &CommandBuffer) -> Result<()> {
    command_buffer.commit()?;
    command_buffer
        .wait_until_completed_for(WAIT)
        .map_err(|_| format!("the command buffer did not complete within {WAIT:?}"))?;
    match command_buffer.status() {
        CommandBufferStatus::Completed => Ok(()),
        status => Err(format!(
            "the command buffer ended {status:?}: {:?}",
            command_buffer.error()
        )
        .into()),
    }
}

/// A copy of the bytes of `buffer`, whose storage the host reaches (`contents`), once no command
/// buffer writes it.
pub fn bytes(buffer: &Buffer) -> Result<Vec<u8>> {
    let contents = buffer.contents().ok_or("the buffer has no contents")?;
    // SAFETY: `contents` holds the buffer's `length` bytes, and no command buffer writes them
    // now (the caller's contract).
    let bytes = unsafe { std::slice::from_raw_parts(contents.as_ptr(), buffer.length() as usize) };
    Ok(bytes.to_vec())
}
