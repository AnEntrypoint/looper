use std::env;
use std::fs;
use std::io::{self, Read};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

fn tool_home() -> PathBuf {
    let exe_dir = env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(|d| d.to_path_buf()))
        .unwrap_or_else(|| env::current_dir().unwrap());
    exe_dir.join("toolchain")
}

fn tmp_dir() -> PathBuf {
    let base = env::temp_dir().join("crun");
    let id = std::process::id();
    let ts = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_nanos())
        .unwrap_or(0);
    let dir = base.join(format!("{}_{}", id, ts));
    fs::create_dir_all(&dir).unwrap();
    dir
}

fn find_compiler(lang: &str) -> Option<PathBuf> {
    let self_size = env::current_exe().ok()
        .and_then(|p| fs::metadata(&p).ok())
        .map(|m| m.len())
        .unwrap_or(0);
    let names: &[&str] = if lang == "cpp" || lang == "c++" {
        &["g++", "c++", "clang++"]
    } else {
        &["gcc", "cc", "clang"]
    };
    for name in names {
        if let Ok(p) = which::which(name) {
            // Skip if same file size as self (it's a copy of crun)
            if self_size > 0 {
                if let Ok(meta) = fs::metadata(&p) {
                    if meta.len() == self_size { continue; }
                }
            }
            return Some(p);
        }
    }
    // Check portable toolchains
    let candidates: Vec<PathBuf> = vec![
        tool_home().join("bin").join(if lang == "cpp" || lang == "c++" { if cfg!(windows) { "g++.exe" } else { "g++" } } else { if cfg!(windows) { "gcc.exe" } else { "gcc" } }),
        tool_home().join("tcc").join("tcc.exe"),
    ];
    for local in candidates {
        if local.exists() { return Some(local); }
    }
    None
}

#[cfg(windows)]
fn find_msvc_cl() -> Option<(PathBuf, Vec<String>, Vec<String>)> {
    let bases = [
        r"C:\Program Files\Microsoft Visual Studio",
        r"C:\Program Files (x86)\Microsoft Visual Studio",
    ];
    let editions = ["2022", "2019"];
    let products = ["BuildTools", "Community", "Professional", "Enterprise"];

    for base in &bases {
        for edition in &editions {
            for product in &products {
                let vc = Path::new(base).join(edition).join(product).join("VC").join("Tools").join("MSVC");
                if !vc.exists() { continue; }
                let mut versions: Vec<_> = fs::read_dir(&vc).ok()?.filter_map(|e| e.ok()).collect();
                versions.sort_by(|a, b| b.file_name().cmp(&a.file_name()));
                for ver in versions {
                    let cl = ver.path().join("bin").join("Hostx64").join("x64").join("cl.exe");
                    if !cl.exists() { continue; }
                    let include = ver.path().join("include");
                    let lib = ver.path().join("lib").join("x64");

                    let mut inc_dirs = vec![];
                    let mut lib_dirs = vec![];
                    if include.exists() { inc_dirs.push(include.to_string_lossy().to_string()); }
                    if lib.exists() { lib_dirs.push(lib.to_string_lossy().to_string()); }

                    let sdk = find_windows_sdk();
                    if let Some((sdk_inc, sdk_lib)) = sdk {
                        inc_dirs.extend(sdk_inc);
                        lib_dirs.extend(sdk_lib);
                    }

                    return Some((cl, inc_dirs, lib_dirs));
                }
            }
        }
    }
    None
}

#[cfg(windows)]
fn find_windows_sdk() -> Option<(Vec<String>, Vec<String>)> {
    let sdk_root = Path::new(r"C:\Program Files (x86)\Windows Kits\10");
    if !sdk_root.exists() { return None; }
    let inc_base = sdk_root.join("Include");
    let lib_base = sdk_root.join("Lib");
    if !inc_base.exists() || !lib_base.exists() { return None; }

    let mut versions: Vec<_> = fs::read_dir(&inc_base).ok()?.filter_map(|e| e.ok())
        .filter(|e| e.file_name().to_string_lossy().starts_with("10.")).collect();
    versions.sort_by(|a, b| b.file_name().cmp(&a.file_name()));
    let ver = versions.first()?;
    let ver_name = ver.file_name();
    let v = ver_name.to_string_lossy();

    let mut incs = vec![];
    let mut libs = vec![];
    for sub in &["ucrt", "um", "shared"] {
        let p = inc_base.join(&*v).join(sub);
        if p.exists() { incs.push(p.to_string_lossy().to_string()); }
    }
    for sub in &["ucrt", "um"] {
        let p = lib_base.join(&*v).join(sub).join("x64");
        if p.exists() { libs.push(p.to_string_lossy().to_string()); }
    }
    Some((incs, libs))
}

#[cfg(not(windows))]
fn find_msvc_cl() -> Option<(PathBuf, Vec<String>, Vec<String>)> { None }

fn find_7z() -> String {
    if let Ok(p) = which::which("7z") {
        return p.to_string_lossy().to_string();
    }
    for c in &[
        r"C:\Program Files\7-Zip\7z.exe",
        r"C:\Program Files (x86)\7-Zip\7z.exe",
        r"C:\ProgramData\chocolatey\bin\7z.exe",
    ] {
        if Path::new(c).exists() { return c.to_string(); }
    }
    "7z".to_string()
}

#[cfg(windows)]
fn install_portable_mingw() -> Result<PathBuf, String> {
    let home = tool_home();
    let marker = home.join(".installed");
    if marker.exists() {
        let gcc = home.join("bin").join("gcc.exe");
        if gcc.exists() { return Ok(gcc); }
    }

    eprintln!("[crun] Installing portable MinGW-w64 to {:?}...", home);
    fs::create_dir_all(&home).map_err(|e| e.to_string())?;

    let url = "https://github.com/niXman/mingw-builds-binaries/releases/download/14.2.0-rt_v12-rev1/x86_64-14.2.0-release-posix-seh-ucrt-rt_v12-rev1.7z";
    let archive = home.join("mingw.7z");

    eprintln!("[crun] Downloading...");
    let status = Command::new("powershell")
        .args(["-NoProfile", "-NonInteractive", "-Command",
            &format!("[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; Invoke-WebRequest -Uri '{}' -OutFile '{}'", url, archive.display())
        ])
        .status().map_err(|e| e.to_string())?;
    if !status.success() { return Err("Download failed".into()); }

    eprintln!("[crun] Extracting...");
    let sz = find_7z();
    let status = Command::new(&sz)
        .args(["x", &archive.to_string_lossy(), &format!("-o{}", home.display()), "-y"])
        .status().map_err(|e| format!("7z ('{}'): {}", sz, e))?;
    if !status.success() { return Err("7z extraction failed".into()); }

    let nested = home.join("mingw64");
    if nested.exists() {
        for entry in fs::read_dir(&nested).map_err(|e| e.to_string())? {
            let entry = entry.map_err(|e| e.to_string())?;
            let dest = home.join(entry.file_name());
            if !dest.exists() { fs::rename(entry.path(), &dest).ok(); }
        }
        fs::remove_dir_all(&nested).ok();
    }

    fs::remove_file(&archive).ok();
    fs::write(&marker, "ok").ok();

    let gcc = home.join("bin").join("gcc.exe");
    if gcc.exists() {
        eprintln!("[crun] Installed.");
        Ok(gcc)
    } else {
        Err("gcc.exe not found after extraction".into())
    }
}

#[cfg(not(windows))]
fn install_portable_mingw() -> Result<PathBuf, String> {
    Err("No C/C++ compiler found. Install gcc/g++ via your package manager.".into())
}

fn compile_gcc(compiler: &Path, src: &Path, bin: &Path, lang: &str, includes: &[String]) -> Result<(), String> {
    let is_tcc = compiler.file_name().map(|n| n.to_string_lossy().contains("tcc")).unwrap_or(false);
    let mut cmd = Command::new(compiler);
    cmd.arg(src).arg("-o").arg(bin);
    if !is_tcc && (lang == "cpp" || lang == "c++") {
        cmd.arg("-std=c++17");
    }
    if !is_tcc { cmd.arg("-lm"); }
    for dir in includes { cmd.arg(format!("-I{}", dir)); }
    let out = cmd.output().map_err(|e| e.to_string())?;
    if !out.status.success() {
        return Err(String::from_utf8_lossy(&out.stderr).to_string());
    }
    Ok(())
}

#[cfg(windows)]
fn compile_msvc(cl: &Path, src: &Path, bin: &Path, lang: &str, inc_dirs: &[String], lib_dirs: &[String], user_includes: &[String]) -> Result<(), String> {
    let mut cmd = Command::new(cl);
    cmd.arg(src);
    cmd.arg(format!("/Fe:{}", bin.display()));
    cmd.arg("/nologo");
    if lang == "cpp" || lang == "c++" {
        cmd.arg("/EHsc").arg("/std:c++17");
    }
    for dir in inc_dirs { cmd.arg(format!("/I{}", dir)); }
    for dir in user_includes { cmd.arg(format!("/I{}", dir)); }
    cmd.arg("/link");
    for dir in lib_dirs { cmd.arg(format!("/LIBPATH:{}", dir)); }

    let out = cmd.output().map_err(|e| e.to_string())?;
    if !out.status.success() {
        let stderr = String::from_utf8_lossy(&out.stderr);
        let stdout = String::from_utf8_lossy(&out.stdout);
        let combined = format!("{}{}", stdout, stderr);
        return Err(combined);
    }
    Ok(())
}

#[cfg(not(windows))]
fn compile_msvc(_cl: &Path, _src: &Path, _bin: &Path, _lang: &str, _inc: &[String], _lib: &[String], _user: &[String]) -> Result<(), String> {
    Err("MSVC not available on this platform".into())
}

fn detect_lang_from_exe() -> Option<&'static str> {
    let exe = env::current_exe().ok()?;
    let stem = exe.file_stem()?.to_string_lossy().to_lowercase();
    if stem.contains("g++") || stem.contains("c++") || stem.contains("clang++") {
        Some("cpp")
    } else if stem.contains("gcc") || stem.contains("cc") || stem.contains("clang") {
        Some("c")
    } else {
        None
    }
}

fn main() {
    let args: Vec<String> = env::args().collect();

    let mut lang = detect_lang_from_exe().unwrap_or("c");
    let mut source_file: Option<String> = None;
    let mut include_dirs: Vec<String> = Vec::new();
    let mut run_args: Vec<String> = Vec::new();
    let mut read_stdin = false;
    let mut gcc_mode = detect_lang_from_exe().is_some();
    let mut gcc_output: Option<String> = None;

    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--lang" | "-l" => { i += 1; if i < args.len() { lang = Box::leak(args[i].clone().into_boxed_str()); } }
            "--include" => { i += 1; if i < args.len() { include_dirs.push(args[i].clone()); } }
            "--stdin" | "-" if !gcc_mode => { read_stdin = true; }
            "--" if !gcc_mode => { run_args = args[i + 1..].to_vec(); break; }
            "-o" if gcc_mode => { i += 1; if i < args.len() { gcc_output = Some(args[i].clone()); } }
            s if gcc_mode && s.starts_with("-I") => {
                if s.len() > 2 { include_dirs.push(s[2..].to_string()); }
                else { i += 1; if i < args.len() { include_dirs.push(args[i].clone()); } }
            }
            s if gcc_mode && (s.starts_with("-l") || s.starts_with("-L") || s.starts_with("-W") || s.starts_with("-O") || s.starts_with("-g") || s.starts_with("-f") || s.starts_with("-D") || s.starts_with("-std=") || s == "-c" || s == "-pipe" || s == "-pthread") => {
                // ignore gcc flags we don't need
            }
            other => {
                if source_file.is_none() && (other.ends_with(".c") || other.ends_with(".cpp") || other.ends_with(".cc") || other.ends_with(".cxx")) {
                    source_file = Some(other.to_string());
                    if other.ends_with(".cpp") || other.ends_with(".cc") || other.ends_with(".cxx") { lang = "cpp"; }
                } else if source_file.is_none() && !other.starts_with("-") {
                    source_file = Some(other.to_string());
                } else if !other.starts_with("-") {
                    run_args.push(other.to_string());
                }
            }
        }
        i += 1;
    }

    let code = if read_stdin || source_file.is_none() {
        let mut buf = String::new();
        if let Some(f) = &source_file {
            buf = fs::read_to_string(f).unwrap_or_else(|e| { eprintln!("Cannot read {}: {}", f, e); std::process::exit(1); });
        } else {
            io::stdin().read_to_string(&mut buf).unwrap_or_else(|e| { eprintln!("stdin: {}", e); std::process::exit(1); });
        }
        buf
    } else {
        fs::read_to_string(source_file.as_ref().unwrap()).unwrap_or_else(|e| {
            eprintln!("Cannot read {}: {}", source_file.as_ref().unwrap(), e);
            std::process::exit(1);
        })
    };

    // In gcc compat mode with -o, use source file directly (don't copy to tmp)
    let (src_path, work, bin_path) = if gcc_mode && source_file.is_some() {
        let src = PathBuf::from(source_file.as_ref().unwrap());
        let bin_ext = if cfg!(windows) { ".exe" } else { "" };
        let out = gcc_output.map(PathBuf::from).unwrap_or_else(|| {
            let stem = src.file_stem().unwrap_or_default().to_string_lossy().to_string();
            src.parent().unwrap_or(Path::new(".")).join(format!("{}{}", stem, bin_ext))
        });
        (src, None, out)
    } else {
        let ext = if lang == "cpp" || lang == "c++" { ".cpp" } else { ".c" };
        let work = tmp_dir();
        let src_path = work.join(format!("code{}", ext));
        fs::write(&src_path, &code).unwrap();
        let bin_ext = if cfg!(windows) { ".exe" } else { "" };
        let bin_path = work.join(format!("code{}", bin_ext));
        (src_path, Some(work), bin_path)
    };

    let result = if let Some(compiler) = find_compiler(lang) {
        compile_gcc(&compiler, &src_path, &bin_path, lang, &include_dirs)
    } else {
        Err("No C/C++ compiler found on PATH or in toolchain/".into())
    };

    if let Err(e) = result {
        eprint!("{}", e);
        if let Some(w) = &work { fs::remove_dir_all(w).ok(); }
        std::process::exit(1);
    }

    // In gcc compat mode, compile-only (rs-exec handles the run phase)
    if gcc_mode {
        std::process::exit(0);
    }

    let status = Command::new(&bin_path)
        .args(&run_args)
        .stdin(Stdio::inherit())
        .stdout(Stdio::inherit())
        .stderr(Stdio::inherit())
        .status()
        .unwrap_or_else(|e| { eprintln!("run: {}", e); std::process::exit(1); });

    if let Some(w) = work { fs::remove_dir_all(&w).ok(); }
    std::process::exit(status.code().unwrap_or(1));
}
