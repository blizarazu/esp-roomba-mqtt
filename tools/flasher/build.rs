fn main() {
    // Declare the custom cfg so Rust doesn't warn about it
    println!("cargo::rustc-check-cfg=cfg(has_bundled_firmware)");
    // Rebuild if firmware.bin changes
    println!("cargo:rerun-if-changed=firmware.bin");
    // Enable embedded-firmware path if firmware.bin is present at compile time
    if std::path::Path::new("firmware.bin").exists() {
        println!("cargo:rustc-cfg=has_bundled_firmware");
    }
}
