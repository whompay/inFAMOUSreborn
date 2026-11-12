//! Finding and preparing the tools the build needs: Python (in a private venv), Visual Studio's
//! developer environment with clang-cl, CMake and Ninja. On Linux, either a native development
//! build or (with a CMake toolchain file) the Windows game built with clang-cl, lld-link and
//! Microsoft's headers and libraries, to be run with Proton/Wine.

use crate::bail;
use crate::util::{self, Ctx, Result};
use std::fs;
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::{Arc, Mutex};

// ---------------------------------------------------------------------------------------------
// Python
// ---------------------------------------------------------------------------------------------

/// A Python interpreter: program plus leading args (`py -3`).
#[derive(Clone, Debug)]
pub struct Py {
    pub prog: Vec<String>,
}

impl Py {
    pub fn cmd(&self) -> Command {
        let mut c = Command::new(&self.prog[0]);
        c.args(&self.prog[1..]);
        c
    }
    fn version(&self) -> Option<(u32, u32)> {
        let out = util::capture(
            self.cmd()
                .args(["-c", "import sys;print('%d.%d'%sys.version_info[:2])"]),
        )?;
        let mut it = out.trim().split('.').map(|x| x.parse::<u32>().ok());
        Some((it.next()??, it.next()??))
    }
}

pub fn find_python(explicit: Option<&Path>, min: (u32, u32)) -> Result<(Py, (u32, u32))> {
    let mut cands: Vec<Vec<String>> = Vec::new();
    if let Some(p) = explicit {
        cands.push(vec![util::path_str(p)]);
    } else {
        if cfg!(windows) {
            for v in ["-3.12", "-3.13", "-3.11", "-3.10", "-3"] {
                cands.push(vec!["py".into(), v.into()]);
            }
        }
        cands.push(vec!["python3".into()]);
        cands.push(vec!["python".into()]);
        // Installed without "Add to PATH" (or PATH not refreshed since the install).
        for p in python_install_dirs() {
            cands.push(vec![util::path_str(&p)]);
        }
    }
    let mut seen_old = None;
    for c in cands {
        let py = Py { prog: c };
        if let Some(v) = py.version() {
            if v >= min {
                return Ok((py, v));
            }
            seen_old = Some(v);
        }
    }
    match (explicit, seen_old) {
        (Some(p), _) => bail!("{} is not a working Python {}.{}+", p.display(), min.0, min.1),
        (None, Some(v)) => bail!(
            "found Python {}.{}, but {}.{} or newer is needed. Install it from https://www.python.org/downloads/ \
             (tick \"Add python.exe to PATH\"), or pass --python <path>.",
            v.0, v.1, min.0, min.1
        ),
        (None, None) => bail!(
            "Python {}.{}+ was not found. Install it from https://www.python.org/downloads/ \
             (tick \"Add python.exe to PATH\"), or pass --python <path>.",
            min.0, min.1
        ),
    }
}

/// python.exe / py.exe in the standard python.org install locations, newest first.
pub fn python_install_dirs() -> Vec<PathBuf> {
    let mut out = Vec::new();
    if !cfg!(windows) {
        return out;
    }
    let mut roots = Vec::new();
    if let Ok(l) = std::env::var("LOCALAPPDATA") {
        roots.push(Path::new(&l).join("Programs").join("Python"));
    }
    for v in ["ProgramFiles", "ProgramW6432", "ProgramFiles(x86)"] {
        if let Ok(p) = std::env::var(v) {
            roots.push(PathBuf::from(p));
        }
    }
    roots.push(PathBuf::from(r"C:\"));
    let mut found: Vec<(u32, PathBuf)> = Vec::new();
    for r in roots {
        let Ok(rd) = fs::read_dir(&r) else { continue };
        for e in rd.flatten() {
            let n = e.file_name().to_string_lossy().to_ascii_lowercase();
            if let Some(v) = n
                .strip_prefix("python3")
                .and_then(|x| x.split(['-', '.', '_']).next())
                .and_then(|x| x.parse::<u32>().ok())
            {
                let exe = e.path().join("python.exe");
                if exe.is_file() {
                    found.push((v, exe));
                }
            }
        }
    }
    found.sort_by(|a, b| b.0.cmp(&a.0));
    out.extend(found.into_iter().map(|x| x.1));
    out
}

/// Create (once) `<dir>` as a venv and install the port's Python requirements into it.
pub fn prepare_venv(
    base: &Py,
    dir: &Path,
    requirements: &[PathBuf],
    packages: &[String],
) -> Result<PathBuf> {
    let vpy = if cfg!(windows) {
        dir.join("Scripts").join("python.exe")
    } else {
        dir.join("bin").join("python")
    };
    if !vpy.exists() {
        util::info(&format!(
            "Creating a private Python environment in {}",
            dir.display()
        ));
        util::run(base.cmd().args(["-m", "venv"]).arg(dir))?;
    }
    // Re-install only when the requirement set changes.
    let mut key = String::new();
    for r in requirements {
        key.push_str(&fs::read_to_string(r).unwrap_or_default());
    }
    key.push_str(&packages.join(" "));
    let key = crate::hash::sha256_hex(key.as_bytes());
    let stamp = dir.join(".installer-deps");
    if fs::read_to_string(&stamp)
        .map(|s| s.trim() == key)
        .unwrap_or(false)
    {
        return Ok(vpy);
    }
    if requirements.is_empty() && packages.is_empty() {
        fs::write(&stamp, key)?;
        return Ok(vpy);
    }
    util::info("Installing Python packages (needs internet the first time) ...");
    let mut cmd = Command::new(&vpy);
    cmd.args(["-m", "pip", "install", "--disable-pip-version-check"]);
    for r in requirements {
        cmd.arg("-r").arg(r);
    }
    cmd.args(packages);
    util::run(&mut cmd)?;
    fs::write(&stamp, key)?;
    Ok(vpy)
}

// ---------------------------------------------------------------------------------------------
// Compiler environment
// ---------------------------------------------------------------------------------------------

pub struct BuildEnv {
    /// Variables to set on every build command (Visual Studio's developer environment).
    pub env: Vec<(String, String)>,
    pub cmake: PathBuf,
    pub ninja: PathBuf,
    pub cc: PathBuf,
    pub cxx: PathBuf,
    pub vs_path: Option<PathBuf>,
    /// CMake toolchain file of a Windows build made on another OS (`--cross`).
    pub cross_file: Option<PathBuf>,
}

#[cfg(windows)]
fn env_path(env: &[(String, String)]) -> Option<String> {
    env.iter()
        .find(|(k, _)| k.eq_ignore_ascii_case("PATH"))
        .map(|(_, v)| v.clone())
}

#[cfg(windows)]
fn vs_installation() -> Option<PathBuf> {
    let pf =
        std::env::var("ProgramFiles(x86)").unwrap_or_else(|_| r"C:\Program Files (x86)".into());
    let vswhere = Path::new(&pf).join(r"Microsoft Visual Studio\Installer\vswhere.exe");
    if !vswhere.is_file() {
        return None;
    }
    let base = [
        "-latest",
        "-products",
        "*",
        "-property",
        "installationPath",
        "-requires",
        "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
    ];
    // Prefer an installation that also has "C++ Clang tools for Windows".
    for extra in [Some("Microsoft.VisualStudio.Component.VC.Llvm.Clang"), None] {
        let mut c = Command::new(&vswhere);
        c.args(base);
        if let Some(e) = extra {
            c.arg(e);
        }
        if let Some(out) = util::capture(&mut c) {
            if let Some(line) = out.lines().map(str::trim).find(|l| !l.is_empty()) {
                return Some(PathBuf::from(line));
            }
        }
    }
    None
}

#[cfg(windows)]
fn vs_dev_env(vs: &Path) -> Result<Vec<(String, String)>> {
    use std::os::windows::process::CommandExt;
    let bat = vs.join(r"Common7\Tools\VsDevCmd.bat");
    if !bat.is_file() {
        bail!("{} is missing", bat.display());
    }
    let mut c = Command::new("cmd");
    c.raw_arg(format!(
        "/s /c \"\"{}\" -arch=x64 -host_arch=x64 >nul 2>&1 && set\"",
        bat.display()
    ));
    let out = util::capture(&mut c).ctx(|| format!("could not run {}", bat.display()))?;
    let env: Vec<(String, String)> = out
        .lines()
        .filter_map(|l| l.split_once('='))
        .filter(|(k, _)| !k.is_empty())
        .map(|(k, v)| (k.to_string(), v.to_string()))
        .collect();
    if env_path(&env).is_none() {
        bail!("VsDevCmd.bat produced no environment");
    }
    Ok(env)
}

/// `cross`: a CMake toolchain file for building the Windows game on Linux (ignored on Windows).
pub fn detect_build_env(cross: Option<&Path>) -> Result<BuildEnv> {
    #[cfg(windows)]
    {
        let _ = cross;
        let vs = vs_installation().ctx(|| {
            "Visual Studio 2022 (or Build Tools) with \"Desktop development with C++\" was not found.\n    \
             Install Build Tools from https://visualstudio.microsoft.com/downloads/ and select the workload\n    \
             \"Desktop development with C++\" plus the component \"C++ Clang Compiler for Windows\"."
        })?;
        let env = vs_dev_env(&vs)?;
        let path = env_path(&env);
        let ide = vs.join(r"Common7\IDE\CommonExtensions\Microsoft\CMake");
        let find = |name: &str, fallback: PathBuf| -> Option<PathBuf> {
            util::find_in_path(name, path.as_deref()).or(if fallback.is_file() {
                Some(fallback)
            } else {
                None
            })
        };
        let cc = find("clang-cl", vs.join(r"VC\Tools\Llvm\x64\bin\clang-cl.exe")).ctx(|| {
            "clang-cl was not found. In the Visual Studio Installer, modify your installation and add\n    \
             \"C++ Clang Compiler for Windows\" (Individual components)."
        })?;
        let cmake = find("cmake", ide.join(r"CMake\bin\cmake.exe"))
            .ctx(|| "CMake was not found (add \"C++ CMake tools for Windows\" in the Visual Studio Installer)")?;
        let ninja = find("ninja", ide.join(r"Ninja\ninja.exe"))
            .ctx(|| "Ninja was not found (add \"C++ CMake tools for Windows\" in the Visual Studio Installer)")?;
        Ok(BuildEnv {
            env,
            cmake,
            ninja,
            cxx: cc.clone(),
            cc,
            vs_path: Some(vs),
            cross_file: None,
        })
    }
    #[cfg(not(windows))]
    {
        let cmake = util::find_in_path("cmake", None).ctx(|| "cmake was not found on PATH")?;
        let ninja = util::find_in_path("ninja", None).ctx(|| "ninja was not found on PATH")?;
        if let Some(file) = cross {
            // The same clang-cl build as on Windows; the toolchain file points CMake at lld-link,
            // llvm-rc and the Microsoft headers/libraries (linux/setup.py writes it).
            if !file.is_file() {
                bail!("the cross toolchain file {} is missing", file.display());
            }
            let cc = util::find_in_path("clang-cl", None).ctx(|| {
                "clang-cl was not found on PATH (the Windows build needs LLVM's clang-cl)"
            })?;
            return Ok(BuildEnv {
                env: Vec::new(),
                cmake,
                ninja,
                cxx: cc.clone(),
                cc,
                vs_path: None,
                cross_file: Some(util::absolute(file)),
            });
        }
        let cc = util::find_in_path("clang", None).ctx(|| "clang was not found on PATH")?;
        let cxx = util::find_in_path("clang++", None).ctx(|| "clang++ was not found on PATH")?;
        Ok(BuildEnv {
            env: Vec::new(),
            cmake,
            ninja,
            cc,
            cxx,
            vs_path: None,
            cross_file: None,
        })
    }
}

/// CMake configure arguments a Windows build made on another OS needs. CMakeLists.txt probes the
/// CPU only when it is not cross compiling, so the same test (tools/v242/cpu_probe.c) is done here:
/// without the AVX2 feature set the compatibility build is selected, under the normal exe name.
pub fn cross_cpu_args() -> Vec<String> {
    #[cfg(target_arch = "x86_64")]
    {
        let v3 = std::arch::is_x86_feature_detected!("ssse3")
            && std::arch::is_x86_feature_detected!("sse4.1")
            && std::arch::is_x86_feature_detected!("avx")
            && std::arch::is_x86_feature_detected!("avx2")
            && std::arch::is_x86_feature_detected!("fma")
            && std::arch::is_x86_feature_detected!("bmi1")
            && std::arch::is_x86_feature_detected!("bmi2")
            && std::arch::is_x86_feature_detected!("lzcnt")
            && std::arch::is_x86_feature_detected!("movbe")
            && std::arch::is_x86_feature_detected!("popcnt");
        if v3 {
            return Vec::new();
        }
    }
    vec![
        "-DPORT_CPU_COMPAT=ON".into(),
        "-DPORT_CPU_COMPAT_AUTO=ON".into(),
    ]
}

/// Run a long build command: full output to `log`, ninja's `[n/m]` progress on one console line,
/// error lines echoed as they happen.
pub fn run_logged(cmd: &mut Command, log: &Path) -> Result<()> {
    let shown = util::show_cmd(cmd);
    util::ensure_parent(log)?;
    let file = Arc::new(Mutex::new(
        fs::File::create(log).ctx(|| format!("creating {}", log.display()))?,
    ));
    writeln!(file.lock().unwrap(), "$ {shown}")?;
    let mut child = cmd
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .ctx(|| format!("could not start: {shown}"))?;
    let errors = Arc::new(Mutex::new(Vec::<String>::new()));
    let mut threads = Vec::new();
    for pipe in [
        child
            .stdout
            .take()
            .map(|p| Box::new(p) as Box<dyn std::io::Read + Send>),
        child
            .stderr
            .take()
            .map(|p| Box::new(p) as Box<dyn std::io::Read + Send>),
    ]
    .into_iter()
    .flatten()
    {
        let file = file.clone();
        let errors = errors.clone();
        threads.push(std::thread::spawn(move || {
            let rd = BufReader::new(pipe);
            for line in rd.split(b'\n').map_while(|l| l.ok()) {
                let line = String::from_utf8_lossy(&line).trim_end().to_string();
                let _ = writeln!(file.lock().unwrap(), "{line}");
                let t = line.trim_start();
                if t.starts_with('[') && t.contains('/') && t.find(']').is_some() {
                    let prog = &t[..=t.find(']').unwrap()];
                    let mut tail: String = t[prog.len()..].trim().chars().take(70).collect();
                    if tail.len() == 70 {
                        tail.push_str("...");
                    }
                    print!("\r    {prog} {tail:<75}");
                    let _ = std::io::stdout().flush();
                } else if t.contains("error:")
                    || t.starts_with("FAILED:")
                    || t.contains("LNK1")
                    || t.contains("fatal error")
                {
                    println!("\n    {t}");
                    errors.lock().unwrap().push(t.to_string());
                }
            }
        }));
    }
    for t in threads {
        let _ = t.join();
    }
    let status = child.wait()?;
    println!();
    if !status.success() {
        let n = errors.lock().unwrap().len();
        let lnk = errors.lock().unwrap().iter().any(|e| e.contains("LNK1104"));
        let mut msg = format!(
            "build failed ({n} error line(s)); full log: {}",
            log.display()
        );
        if lnk {
            msg.push_str("\n    LNK1104 usually means the game is still running -- close it and run the installer again.");
        }
        bail!("{msg}");
    }
    Ok(())
}
