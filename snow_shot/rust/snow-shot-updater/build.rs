use std::env;
use std::fs;
use std::path::PathBuf;

fn main() {
    println!("cargo:rerun-if-env-changed=SNOW_SHOT_UPDATE_KEYS_PATH");
    println!("cargo:rerun-if-env-changed=SNOW_SHOT_VERSION");
    println!("cargo:rerun-if-env-changed=SNOW_SHOT_ICON_PATH");

    let keys = env::var_os("SNOW_SHOT_UPDATE_KEYS_PATH")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("../../resources/update-trusted-keys.json"));
    println!("cargo:rerun-if-changed={}", keys.display());
    let output = PathBuf::from(env::var_os("OUT_DIR").expect("OUT_DIR is required"));
    fs::copy(&keys, output.join("update-trusted-keys.json")).unwrap_or_else(|error| {
        panic!(
            "failed to embed update keys from {}: {error}",
            keys.display()
        )
    });

    #[cfg(windows)]
    compile_windows_resources(&output);
}

#[cfg(windows)]
fn compile_windows_resources(output: &std::path::Path) {
    let version =
        env::var("SNOW_SHOT_VERSION").unwrap_or_else(|_| env::var("CARGO_PKG_VERSION").unwrap());
    let numeric = version
        .split_once('-')
        .map_or(version.as_str(), |value| value.0);
    let mut parts = numeric.split('.');
    let major = parts.next().expect("major version");
    let minor = parts.next().expect("minor version");
    let patch = parts.next().expect("patch version");
    assert!(
        parts.next().is_none(),
        "Snow Shot version must be major.minor.patch[-suffix]"
    );
    for part in [major, minor, patch] {
        part.parse::<u16>()
            .expect("version components must be integers");
    }

    let mini = env::var_os("CARGO_FEATURE_MINI").is_some();
    let product = if mini { "Snow Shot Mini" } else { "Snow Shot" };
    let updater = if mini {
        "snow-shot-mini-updater"
    } else {
        "snow-shot-updater"
    };
    let resource_path = output.join(format!("{updater}.rc"));
    // Normal startup runs --transaction-state through CreateProcess. Declare
    // asInvoker so Windows never guesses that this "updater" needs elevation.
    // Protected installation writes still use the explicit runas handoff.
    let manifest =
        PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap()).join("windows.manifest");
    println!("cargo:rerun-if-changed={}", manifest.display());
    let manifest_path = manifest.display().to_string().replace('\\', "\\\\");
    let icon = env::var_os("SNOW_SHOT_ICON_PATH").map(PathBuf::from);
    if let Some(icon) = &icon {
        println!("cargo:rerun-if-changed={}", icon.display());
    }
    let icon_line = icon
        .as_ref()
        .map(|path| {
            format!(
                "1 ICON \"{}\"\n",
                path.display().to_string().replace('\\', "\\\\")
            )
        })
        .unwrap_or_default();
    let contents = format!(
        r#"#pragma code_page(65001)
1 24 "{manifest_path}"
{icon_line}1 VERSIONINFO
FILEVERSION {major},{minor},{patch},0
PRODUCTVERSION {major},{minor},{patch},0
FILEFLAGSMASK 0x3fL
FILEFLAGS 0x0L
FILEOS 0x40004L
FILETYPE 0x1L
FILESUBTYPE 0x0L
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "000004b0"
        BEGIN
            VALUE "CompanyName", "Snow Apps\0"
            VALUE "FileDescription", "{product} update service\0"
            VALUE "FileVersion", "{numeric}.0\0"
            VALUE "InternalName", "{updater}\0"
            VALUE "LegalCopyright", "Copyright (C) 2025-2026 mg-chao\0"
            VALUE "OriginalFilename", "{updater}.exe\0"
            VALUE "ProductName", "{product}\0"
            VALUE "ProductVersion", "{version}\0"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x0, 0x04b0
    END
END
"#
    );
    fs::write(&resource_path, contents).expect("failed to write updater resource file");
    let mut resource = winres::WindowsResource::new();
    // winres 0.1.12 locates the Windows SDK by shelling out to `reg query`, which is the
    // only mechanism it knows: the crate predates the WindowsSdkDir / WindowsSDKVersion
    // environment contract. Where reg.exe is unavailable (this machine: it is on the
    // Security Center program blacklist, and that cannot be approved per command) the
    // query fails, winres falls back to an empty toolkit root and then tries the
    // relative path bin\x64\rc.exe, panicking with `Os { code: 3, kind: NotFound }`.
    // vcvars64.bat already exports both variables, so point winres at the same rc.exe
    // explicitly. Only done when that file really exists, so a machine with a working
    // registry keeps winres's original behaviour.
    if let Some(bin_dir) = windows_sdk_rc_bin_dir() {
        resource.set_toolkit_path(bin_dir.to_string_lossy().as_ref());
    }
    resource.set_resource_file(resource_path.to_str().expect("resource path must be UTF-8"));
    resource
        .compile()
        .expect("failed to compile updater resources");
}

/// Directory that holds the Windows SDK `rc.exe` (i.e. `<SDK>\bin\<version>\x64`),
/// derived from the environment instead of the registry. `None` when the variables are
/// missing or the resulting path does not exist, in which case winres keeps its own
/// lookup.
#[cfg(windows)]
fn windows_sdk_rc_bin_dir() -> Option<PathBuf> {
    println!("cargo:rerun-if-env-changed=WindowsSdkDir");
    println!("cargo:rerun-if-env-changed=WindowsSDKVersion");
    let sdk_dir = env::var_os("WindowsSdkDir")?;
    let sdk_version = env::var_os("WindowsSDKVersion")?;
    let bin_dir = PathBuf::from(sdk_dir)
        .join("bin")
        .join(sdk_version)
        .join("x64");
    if bin_dir.join("rc.exe").is_file() {
        Some(bin_dir)
    } else {
        None
    }
}
