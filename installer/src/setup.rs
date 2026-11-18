//! The setup program's engine: requirement checks and installs, game checks, and the install
//! worker. No GUI code here; `src/bin/infamous-setup.rs` draws the pages.

use crate::{game, iso, lock, manifest::Manifest, payload, pipeline, toolchain, util};
use std::io::{BufReader, Read};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{mpsc, Arc, Mutex};

pub const NO_WINDOW: u32 = 0x0800_0000;

pub fn hide(c: &mut Command) -> &mut Command {
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        c.creation_flags(NO_WINDOW);
    }
    c
}

// ---------------------------------------------------------------------------------------------
// Requirements
// ---------------------------------------------------------------------------------------------

/// Re-read PATH from the registry, so tools installed after this program started are found.
pub fn refresh_path() {
    #[cfg(windows)]
    {
        fn reg(key: &str) -> Option<String> {
            let out = hide(Command::new("reg").args(["query", key, "/v", "Path"]))
                .output()
                .ok()?;
            let s = String::from_utf8_lossy(&out.stdout).into_owned();
            let line = s
                .lines()
                .find(|l| l.trim_start().to_ascii_lowercase().starts_with("path"))?;
            let i = line.find("REG_")?;
            let rest = &line[i..];
            let v = rest.split_once(char::is_whitespace)?.1.trim();
            Some(expand(v))
        }
        fn expand(s: &str) -> String {
            let mut out = String::new();
            let mut rest = s;
            while let Some(a) = rest.find('%') {
                out.push_str(&rest[..a]);
                let r = &rest[a + 1..];
                match r.find('%') {
                    Some(b) => {
                        let name = &r[..b];
                        out.push_str(&std::env::var(name).unwrap_or_else(|_| format!("%{name}%")));
                        rest = &r[b + 1..];
                    }
                    None => {
                        out.push('%');
                        rest = r;
                    }
                }
            }
            out.push_str(rest);
            out
        }
        let m = reg(r"HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment");
        let u = reg(r"HKCU\Environment");
        if m.is_some() || u.is_some() {
            let joined = [m, u].into_iter().flatten().collect::<Vec<_>>().join(";");
            std::env::set_var("PATH", joined);
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub enum Check {
    Unknown,
    Checking,
    Ok(String),
    Missing(String),
    Installing(String),
}

pub fn check_python() -> Check {
    match toolchain::find_python(None, (3, 10)) {
        Ok((p, v)) => Check::Ok(format!("Python {}.{} ({})", v.0, v.1, p.prog.join(" "))),
        Err(e) => Check::Missing(e.to_string()),
    }
}

pub fn check_build_tools() -> Check {
    match toolchain::detect_build_env(None) {
        Ok(env) => Check::Ok(match env.vs_path {
            Some(p) => format!("Visual Studio Build Tools with Clang ({})", p.display()),
            None => format!("clang ({})", env.cc.display()),
        }),
        Err(e) => Check::Missing(e.to_string()),
    }
}

pub fn has_winget() -> bool {
    util::find_in_path("winget", None).is_some()
}

/// Run a command, sending its output lines to `log`; Ok if it exits 0.
pub fn run_logged(cmd: &mut Command, log: &(dyn Fn(String) + Sync)) -> Result<(), String> {
    log(format!("> {}", util::show_cmd(cmd)));
    let mut child = hide(cmd)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|e| e.to_string())?;
    let (tx, rx) = mpsc::channel::<String>();
    let pumps: Vec<_> = [
        child
            .stdout
            .take()
            .map(|p| Box::new(p) as Box<dyn Read + Send>),
        child
            .stderr
            .take()
            .map(|p| Box::new(p) as Box<dyn Read + Send>),
    ]
    .into_iter()
    .flatten()
    .map(|p| {
        let tx = tx.clone();
        std::thread::spawn(move || pump(p, &tx))
    })
    .collect();
    drop(tx);
    for l in rx {
        log(l);
    }
    for p in pumps {
        let _ = p.join();
    }
    let st = child.wait().map_err(|e| e.to_string())?;
    if st.success() {
        Ok(())
    } else {
        Err(format!("exit code {}", st.code().unwrap_or(-1)))
    }
}

/// Lines from a pipe; `\r` also ends a line (progress output), blank lines dropped.
fn pump(p: Box<dyn Read + Send>, tx: &mpsc::Sender<String>) {
    let mut r = BufReader::new(p);
    let mut buf = Vec::new();
    let mut byte = [0u8; 1];
    loop {
        match r.read(&mut byte) {
            Ok(0) | Err(_) => break,
            Ok(_) => {
                if byte[0] == b'\n' || byte[0] == b'\r' {
                    if !buf.is_empty() {
                        let s = String::from_utf8_lossy(&buf).trim_end().to_string();
                        if !s.trim().is_empty() && tx.send(s).is_err() {
                            return;
                        }
                        buf.clear();
                    }
                } else {
                    buf.push(byte[0]);
                }
            }
        }
    }
    if !buf.is_empty() {
        let _ = tx.send(String::from_utf8_lossy(&buf).trim_end().to_string());
    }
}

pub fn install_python(log: &(dyn Fn(String) + Sync)) -> Result<(), String> {
    if !has_winget() {
        return Err("winget is not available on this PC. Install Python 3.12 from python.org (tick \"Add python.exe to PATH\").".into());
    }
    let r = run_logged(
        Command::new("winget").args([
            "install",
            "-e",
            "--id",
            "Python.Python.3.12",
            "--scope",
            "user",
            "--silent",
            "--accept-package-agreements",
            "--accept-source-agreements",
            "--disable-interactivity",
            "--override",
            "/quiet InstallAllUsers=0 PrependPath=1 Include_launcher=1 Include_test=0",
        ]),
        log,
    );
    refresh_path();
    match (r, check_python()) {
        (_, Check::Ok(_)) => Ok(()),
        (Err(e), _) => Err(format!("the Python installer failed ({e})")),
        (Ok(()), c) => Err(format!(
            "Python was installed but can't be found yet: {c:?}"
        )),
    }
}

const VS_COMPONENTS: &[&str] = &[
    "Microsoft.VisualStudio.Workload.VCTools",
    "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
    "Microsoft.VisualStudio.Component.Windows11SDK.22621",
    "Microsoft.VisualStudio.Component.VC.CMake.Project",
    "Microsoft.VisualStudio.Component.VC.Llvm.Clang",
    "Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset",
];

/// Any Visual Studio 2022 installation (with or without the C++ parts).
fn any_vs() -> Option<PathBuf> {
    let pf =
        std::env::var("ProgramFiles(x86)").unwrap_or_else(|_| r"C:\Program Files (x86)".into());
    let vswhere = Path::new(&pf).join(r"Microsoft Visual Studio\Installer\vswhere.exe");
    let out = util::capture(Command::new(vswhere).args([
        "-products",
        "*",
        "-version",
        "[17.0,18.0)",
        "-latest",
        "-property",
        "installationPath",
    ]))?;
    out.lines()
        .map(str::trim)
        .find(|l| !l.is_empty())
        .map(PathBuf::from)
}

pub fn install_build_tools(log: &(dyn Fn(String) + Sync)) -> Result<(), String> {
    let mut adds = Vec::new();
    for c in VS_COMPONENTS {
        adds.push("--add".to_string());
        adds.push(c.to_string());
    }
    let r = if let Some(vs) = any_vs() {
        // Add the missing parts to the existing installation (asks for administrator rights).
        log(format!(
            "Adding the C++ and Clang components to {}",
            vs.display()
        ));
        let pf =
            std::env::var("ProgramFiles(x86)").unwrap_or_else(|_| r"C:\Program Files (x86)".into());
        let setup = Path::new(&pf).join(r"Microsoft Visual Studio\Installer\setup.exe");
        let mut args = vec![
            "modify".to_string(),
            "--installPath".into(),
            util::path_str(&vs),
        ];
        args.extend(adds);
        args.extend(["--includeRecommended", "--passive", "--norestart"].map(String::from));
        let list = args
            .iter()
            .map(|a| format!("'{}'", a.replace('\'', "''")))
            .collect::<Vec<_>>()
            .join(",");
        let ps = format!(
            "$p = Start-Process -FilePath '{}' -ArgumentList {} -Verb RunAs -Wait -PassThru; exit $p.ExitCode",
            util::path_str(&setup).replace('\'', "''"),
            list
        );
        run_logged(
            Command::new("powershell").args([
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-Command",
                &ps,
            ]),
            log,
        )
    } else if has_winget() {
        let ov = format!(
            "--wait --passive --norestart --includeRecommended {}",
            adds.join(" ")
        );
        run_logged(
            Command::new("winget").args([
                "install",
                "-e",
                "--id",
                "Microsoft.VisualStudio.2022.BuildTools",
                "--accept-package-agreements",
                "--accept-source-agreements",
                "--disable-interactivity",
                "--override",
                &ov,
            ]),
            log,
        )
    } else {
        Err("winget is not available. Install \"Build Tools for Visual Studio 2022\" from visualstudio.microsoft.com with \"Desktop development with C++\" and \"C++ Clang tools for Windows\".".into())
    };
    refresh_path();
    match (r, check_build_tools()) {
        (_, Check::Ok(_)) => Ok(()),
        (Err(e), _) => Err(format!("the Visual Studio installer did not finish ({e})")),
        (Ok(()), Check::Missing(m)) => Err(m),
        (Ok(()), c) => Err(format!("{c:?}")),
    }
}

// ---------------------------------------------------------------------------------------------
// The game
// ---------------------------------------------------------------------------------------------

#[derive(Clone, Debug)]
pub enum GameSource {
    Iso { path: PathBuf, key: Option<PathBuf> },
    Folder(PathBuf),
}

#[derive(Clone, Debug)]
pub struct GameInfo {
    pub title: String,
    pub title_id: String,
    /// Bytes to copy (disc images) or 0 (folders are linked, not copied).
    pub copy_bytes: u64,
    pub encrypted: bool,
    /// The program file still has to be decrypted (by RPCS3).
    pub needs_rpcs3: bool,
}

#[derive(Clone, Debug)]
pub enum GameProblem {
    NeedsKey,
    BadKey,
    WrongGame { title: String, title_id: String },
    Other(String),
}

impl std::fmt::Display for GameProblem {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        match self {
            GameProblem::NeedsKey => {
                f.write_str("This disc image is encrypted. Choose its disc key (.dkey) file.")
            }
            GameProblem::BadKey => f.write_str("That disc key doesn't belong to this disc image."),
            GameProblem::WrongGame { title, title_id } => write!(
                f,
                "This is {title} [{title_id}], not the game this port is for."
            ),
            GameProblem::Other(s) => f.write_str(s),
        }
    }
}

pub fn check_game(src: &GameSource, title_ids: &[String]) -> Result<GameInfo, GameProblem> {
    let other = |e: util::Error| GameProblem::Other(e.to_string());
    let (sfo, copy_bytes, encrypted, needs_rpcs3) = match src {
        GameSource::Iso { path, key } => {
            let key = match key {
                Some(k) => Some(iso::read_key_file(k).map_err(other)?),
                None => None,
            };
            let mut img = match iso::open_ready(path, key) {
                Ok(i) => i,
                Err(e) if e.0.contains("needs its disc key") => return Err(GameProblem::NeedsKey),
                Err(e) if e.0.contains("does not fit") => return Err(GameProblem::BadKey),
                Err(e) => return Err(other(e)),
            };
            let e = img.find("PS3_GAME/PARAM.SFO").ok_or_else(|| {
                GameProblem::Other("This is not a PS3 game disc (no PS3_GAME/PARAM.SFO).".into())
            })?;
            let d = img.read_file(&e).map_err(other)?;
            let sfo = game::parse_sfo(&d)
                .ok_or_else(|| GameProblem::Other("The disc's PARAM.SFO is unreadable.".into()))?;
            let size = img
                .files
                .iter()
                .filter(|e| !e.path.to_ascii_uppercase().starts_with("PS3_UPDATE/"))
                .map(|e| e.size)
                .sum();
            (sfo, size, img.decrypting, true)
        }
        GameSource::Folder(p) => {
            let g = game::locate(p).map_err(other)?;
            let elf_ok = matches!(game::classify(&g.eboot_elf()), Ok(game::Kind::Elf));
            let bin_plain = matches!(
                game::classify(&g.eboot_bin()),
                Ok(game::Kind::Elf | game::Kind::DebugSelf)
            );
            (g.sfo, 0, false, !(elf_ok || bin_plain))
        }
    };
    let title = sfo.get("TITLE").cloned().unwrap_or_else(|| "?".into());
    let title_id = sfo.get("TITLE_ID").cloned().unwrap_or_else(|| "?".into());
    if !title_ids.is_empty() && !title_ids.contains(&title_id) {
        return Err(GameProblem::WrongGame { title, title_id });
    }
    Ok(GameInfo {
        title,
        title_id,
        copy_bytes,
        encrypted,
        needs_rpcs3,
    })
}

/// rpcs3.exe in the usual places.
pub fn find_rpcs3() -> Option<PathBuf> {
    let mut roots: Vec<PathBuf> = Vec::new();
    for v in ["ProgramFiles", "ProgramW6432", "LOCALAPPDATA", "APPDATA"] {
        if let Ok(p) = std::env::var(v) {
            roots.push(PathBuf::from(p));
        }
    }
    if let Ok(h) = std::env::var("USERPROFILE") {
        let h = PathBuf::from(h);
        for s in [
            "Desktop",
            "Downloads",
            "Documents",
            "Games",
            "Emulators",
            "OneDrive\\Desktop",
            "OneDrive\\Documents",
        ] {
            roots.push(h.join(s));
        }
        roots.push(h);
    }
    for d in [
        "C:\\",
        "D:\\",
        "E:\\",
        "C:\\Games",
        "D:\\Games",
        "C:\\Emulators",
        "D:\\Emulators",
    ] {
        roots.push(PathBuf::from(d));
    }
    for r in roots {
        let direct = r.join("rpcs3.exe");
        if direct.is_file() {
            return Some(direct);
        }
        let Ok(rd) = std::fs::read_dir(&r) else {
            continue;
        };
        for e in rd.flatten() {
            if e.file_name()
                .to_string_lossy()
                .to_ascii_lowercase()
                .contains("rpcs3")
            {
                let p = e.path().join("rpcs3.exe");
                if p.is_file() {
                    return Some(p);
                }
            }
        }
    }
    None
}

pub fn default_location(title: &str) -> PathBuf {
    let home = std::env::var("USERPROFILE")
        .or_else(|_| std::env::var("HOME"))
        .unwrap_or_else(|_| ".".into());
    PathBuf::from(home)
        .join("Games")
        .join(format!("{title} PC"))
}

/// Space the install needs: the port files, the game (if copied) and the generated code + build.
pub fn space_needed(payload_bytes: u64, game_bytes: u64) -> u64 {
    payload_bytes + game_bytes + 6 * (1 << 30)
}

// ---------------------------------------------------------------------------------------------
// Where the port's files come from
// ---------------------------------------------------------------------------------------------

/// The packaged setup carries the port inside the .exe; the plain build output
/// (`installer/target/release/infamous-setup.exe`) uses the checkout it sits in.
#[derive(Clone, Debug)]
pub enum Source {
    Embedded(PathBuf),
    Checkout {
        root: PathBuf,
        files: Vec<String>,
        extra: Vec<(String, PathBuf)>,
    },
}

pub struct SourceInfo {
    pub source: Source,
    pub manifest: Manifest,
    pub bytes: u64,
}

pub fn find_source(exe: &Path) -> Result<SourceInfo, String> {
    if let Some(p) = payload::Payload::open(exe).map_err(|e| e.to_string())? {
        let d = p
            .read("installer/infamous.toml")
            .map_err(|e| e.to_string())?
            .ok_or("This setup program is damaged (no manifest inside).")?;
        let manifest = Manifest::parse(&String::from_utf8_lossy(&d)).map_err(|e| e.to_string())?;
        return Ok(SourceInfo {
            source: Source::Embedded(exe.to_path_buf()),
            manifest,
            bytes: p.bytes(),
        });
    }
    let dir = exe.parent().ok_or("no exe folder")?.to_path_buf();
    let root = dir
        .ancestors()
        .find(|d| d.join("installer").join("infamous.toml").is_file())
        .map(util::absolute)
        .ok_or("This setup program has no port files inside it, and it is not inside a port checkout.\nUse dist\\inFamous Setup.exe, or build it with `infamous-installer package`.")?;
    let manifest =
        Manifest::load(&root.join("installer").join("infamous.toml")).map_err(|e| e.to_string())?;
    if lock::Lock::load(&root.join("installer").join("generated.lock"))
        .map_err(|e| e.to_string())?
        .is_none()
    {
        return Err("This checkout has no patch set yet (installer/generated.lock). Run `infamous-installer make-patches` first.".into());
    }
    let ext = if cfg!(windows) { ".exe" } else { "" };
    let cli = dir.join(format!("infamous-installer{ext}"));
    let launcher = dir.join(format!("infamous-launch{ext}"));
    for p in [&cli, &launcher] {
        if !p.is_file() {
            return Err(format!("{} is missing. Build everything with `cargo build --release` in the installer folder.", p.display()));
        }
    }
    let skip: Vec<String> = manifest.link_name.iter().cloned().collect();
    let files = pipeline::export_files(&manifest, &root, &skip).map_err(|e| e.to_string())?;
    let bytes = files
        .iter()
        .filter_map(|f| std::fs::metadata(util::join_slash(&root, f)).ok())
        .map(|m| m.len())
        .sum();
    let extra = vec![
        (format!("installer/infamous-installer{ext}"), cli),
        (format!("{}{ext}", manifest.game_title), launcher),
    ];
    Ok(SourceInfo {
        source: Source::Checkout { root, files, extra },
        manifest,
        bytes,
    })
}

/// Put the port's files into `dest`; `progress(done, total)` in bytes. Returns their paths.
fn place_files(
    src: &Source,
    dest: &Path,
    progress: &mut dyn FnMut(u64, u64),
) -> Result<Vec<String>, String> {
    match src {
        Source::Embedded(exe) => {
            let pl = payload::Payload::open(exe)
                .map_err(|e| e.to_string())?
                .ok_or("this setup program carries no port files")?;
            let names = pl.names().map_err(|e| e.to_string())?;
            pl.extract(dest, progress).map_err(|e| e.to_string())?;
            Ok(names)
        }
        Source::Checkout { root, files, extra } => {
            let same = util::absolute(root) == util::absolute(dest);
            let mut list: Vec<(String, PathBuf)> = Vec::new();
            if !same {
                list.extend(files.iter().map(|f| (f.clone(), util::join_slash(root, f))));
            }
            list.extend(extra.iter().cloned());
            let total: u64 = list
                .iter()
                .filter_map(|(_, p)| std::fs::metadata(p).ok())
                .map(|m| m.len())
                .sum();
            let mut done = 0u64;
            for (rel, from) in &list {
                let to = util::join_slash(dest, rel);
                if util::absolute(from) != util::absolute(&to) {
                    util::copy_file(from, &to).map_err(|e| e.to_string())?;
                }
                done += std::fs::metadata(from).map(|m| m.len()).unwrap_or(0);
                progress(done, total);
            }
            Ok(list.into_iter().map(|(rel, _)| rel).collect())
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Installing
// ---------------------------------------------------------------------------------------------

#[derive(Clone)]
pub struct InstallOpts {
    pub source: Source,
    pub location: PathBuf,
    pub game: GameSource,
    pub rpcs3: Option<PathBuf>,
    pub desktop_shortcut: bool,
    pub start_menu: bool,
    pub game_title: String,
    pub link_name: String,
}

#[derive(Default)]
pub struct Progress {
    pub fraction: f32,
    pub phase: String,
    pub detail: String,
    pub log: Vec<String>,
    pub done: Option<Result<(), String>>,
    pub started: Option<std::time::Instant>,
}

impl Progress {
    fn push(&mut self, l: String) {
        if self.log.len() > 20_000 {
            self.log.drain(..5_000);
        }
        self.log.push(l);
    }
}

pub struct Installer {
    pub progress: Arc<Mutex<Progress>>,
    pub cancel: Arc<AtomicBool>,
    child: Arc<Mutex<Option<u32>>>,
}

impl Installer {
    pub fn start(opts: InstallOpts, repaint: Arc<dyn Fn() + Send + Sync>) -> Installer {
        let progress = Arc::new(Mutex::new(Progress {
            started: Some(std::time::Instant::now()),
            ..Default::default()
        }));
        let cancel = Arc::new(AtomicBool::new(false));
        let child = Arc::new(Mutex::new(None));
        let (p, c, ch) = (progress.clone(), cancel.clone(), child.clone());
        std::thread::spawn(move || {
            let r = install(&opts, &p, &c, &ch, &*repaint);
            let mut g = p.lock().unwrap();
            if let Err(e) = &r {
                g.push(format!("ERROR: {e}"));
            } else {
                g.fraction = 1.0;
                g.phase = "Finished".into();
            }
            g.done = Some(r);
            drop(g);
            repaint();
        });
        Installer {
            progress,
            cancel,
            child,
        }
    }

    /// Update an existing installation from GitHub (`infamous-installer update`).
    pub fn start_update(
        location: PathBuf,
        game_title: String,
        start_menu: bool,
        source: Source,
        update_repo: Option<String>,
        update_branch: String,
        repaint: Arc<dyn Fn() + Send + Sync>,
    ) -> Installer {
        let progress = Arc::new(Mutex::new(Progress {
            started: Some(std::time::Instant::now()),
            ..Default::default()
        }));
        let cancel = Arc::new(AtomicBool::new(false));
        let child = Arc::new(Mutex::new(None));
        let (p, c, ch) = (progress.clone(), cancel.clone(), child.clone());
        std::thread::spawn(move || {
            let r = run_update(
                &location,
                &game_title,
                start_menu,
                &source,
                update_repo.as_deref(),
                &update_branch,
                &p,
                &c,
                &ch,
                &*repaint,
            );
            let mut g = p.lock().unwrap();
            if let Err(e) = &r {
                g.push(format!("ERROR: {e}"));
            } else {
                g.fraction = 1.0;
                g.phase = "Finished".into();
            }
            g.done = Some(r);
            drop(g);
            repaint();
        });
        Installer {
            progress,
            cancel,
            child,
        }
    }

    pub fn stop(&self) {
        self.cancel.store(true, Ordering::SeqCst);
        if let Some(pid) = *self.child.lock().unwrap() {
            kill_tree(pid);
        }
    }
}

/// A folder holding a finished installation (the setup's files plus the installer's state).
pub fn is_installed(dir: &Path) -> bool {
    let ext = if cfg!(windows) { ".exe" } else { "" };
    dir.join("installer")
        .join(format!("infamous-installer{ext}"))
        .is_file()
        && dir.join(".installer").join("state").is_file()
}

#[derive(Clone, Debug)]
pub struct UpdateInfo {
    pub available: bool,
    pub files: u64,
    pub bytes: u64,
    pub version: String,
}

/// Ask the installation's own installer whether GitHub has something newer (`update --check`).
pub fn check_update(
    dir: &Path,
    update_repo: Option<&str>,
    update_branch: &str,
) -> Result<UpdateInfo, String> {
    let ext = if cfg!(windows) { ".exe" } else { "" };
    let cli = dir
        .join("installer")
        .join(format!("infamous-installer{ext}"));
    let mut cmd = Command::new(&cli);
    cmd.arg("update")
        .arg("--check")
        .arg("--root")
        .arg(dir)
        .current_dir(dir);
    // The Setup may be repairing an installation made by a fork or an older package. Always use
    // the update source embedded in this Setup instead of trusting that installation's manifest.
    if let Some(repo) = update_repo {
        cmd.arg("--repo").arg(repo);
    }
    cmd.arg("--branch").arg(update_branch);
    let out = hide(&mut cmd)
        .stdin(Stdio::null())
        .output()
        .map_err(|e| format!("could not start {}: {e}", cli.display()))?;
    let text =
        String::from_utf8_lossy(&out.stdout).into_owned() + &String::from_utf8_lossy(&out.stderr);
    if let Some(l) = text.lines().find(|l| l.starts_with("UPDATE-CHECK ")) {
        let f: Vec<&str> = l.split_whitespace().collect();
        if f.len() >= 5 {
            return Ok(UpdateInfo {
                available: f[1] == "available",
                version: f[2].chars().take(10).collect(),
                files: f[3].parse().unwrap_or(0),
                bytes: f[4].parse().unwrap_or(0),
            });
        }
    }
    if text.contains("unknown argument 'update'") || text.contains("unknown argument") {
        return Err("This installation is too old to update itself. Install again into the same folder with this setup once; it only redoes what changed.".into());
    }
    let err = text
        .lines()
        .filter_map(|l| l.trim().strip_prefix("ERROR:"))
        .next()
        .map(|e| e.trim().to_string());
    Err(err.unwrap_or_else(|| "could not check for updates".into()))
}

fn run_update(
    dir: &Path,
    game_title: &str,
    start_menu: bool,
    source: &Source,
    update_repo: Option<&str>,
    update_branch: &str,
    p: &Arc<Mutex<Progress>>,
    cancel: &AtomicBool,
    child: &Arc<Mutex<Option<u32>>>,
    repaint: &(dyn Fn() + Send + Sync),
) -> Result<(), String> {
    let set = |f: f32, phase: &str, detail: &str| {
        let mut g = p.lock().unwrap();
        g.fraction = f;
        if !phase.is_empty() {
            g.phase = phase.to_string();
        }
        g.detail = detail.to_string();
        drop(g);
        repaint();
    };
    let log = |l: String| {
        p.lock().unwrap().push(l);
        repaint();
    };
    let stopped = || cancel.load(Ordering::SeqCst);
    remove_internal_documents(dir, &log);
    set(0.005, "Repairing the launcher and updater", "");
    let placed = place_files(source, dir, &mut |done, total| {
        set(0.005 + 0.01 * done as f32 / total.max(1) as f32, "", "")
    })?;
    if let Err(e) = crate::update::record_installed(dir, &placed) {
        log(format!("note: could not record the repaired files: {e}"));
    }
    let ext = if cfg!(windows) { ".exe" } else { "" };
    let cli = dir
        .join("installer")
        .join(format!("infamous-installer{ext}"));
    if !cli.is_file() {
        return Err(format!(
            "{} is not an installation of {game_title} (no installer inside)",
            dir.display()
        ));
    }
    set(0.01, "Checking GitHub for a new version", "");
    let mut cmd = Command::new(&cli);
    cmd.arg("update").arg("--root").arg(dir);
    // See check_update: this deliberately migrates fork-configured and stale installations back
    // to the repository selected by the Setup the player is currently running.
    if let Some(repo) = update_repo {
        cmd.arg("--repo").arg(repo);
    }
    cmd.arg("--branch").arg(update_branch);
    run_cli(cmd, dir, p, child, &stopped, &set, &log)?;
    if start_menu {
        add_update_shortcut(dir, game_title, &log);
    }
    Ok(())
}

/// Start menu: "Update <game>" -> the installation's `Update <game>.cmd`.
fn add_update_shortcut(dir: &Path, game_title: &str, log: &dyn Fn(String)) {
    let script = dir.join(format!("Update {game_title}.cmd"));
    if !script.is_file() {
        return;
    }
    let ext = if cfg!(windows) { ".exe" } else { "" };
    let icon = dir.join(format!("{game_title}{ext}"));
    if let Some(d) = shell_folder("Programs") {
        let l = d.join(format!("Update {game_title}.lnk"));
        match make_shortcut_icon(
            &l,
            &script,
            dir,
            if icon.is_file() { &icon } else { &script },
        ) {
            Ok(()) => log(format!("Shortcut: {}", l.display())),
            Err(e) => log(format!("Could not create {}: {e}", l.display())),
        }
    }
}

fn kill_tree(pid: u32) {
    #[cfg(windows)]
    let _ = hide(Command::new("taskkill").args(["/T", "/F", "/PID", &pid.to_string()])).output();
    #[cfg(not(windows))]
    let _ = Command::new("kill").arg(pid.to_string()).output();
}

fn install(
    o: &InstallOpts,
    p: &Arc<Mutex<Progress>>,
    cancel: &AtomicBool,
    child: &Arc<Mutex<Option<u32>>>,
    repaint: &(dyn Fn() + Send + Sync),
) -> Result<(), String> {
    let set = |f: f32, phase: &str, detail: &str| {
        let mut g = p.lock().unwrap();
        g.fraction = f;
        if !phase.is_empty() {
            g.phase = phase.to_string();
        }
        g.detail = detail.to_string();
        drop(g);
        repaint();
    };
    let log = |l: String| {
        p.lock().unwrap().push(l);
        repaint();
    };
    let stopped = || cancel.load(Ordering::SeqCst);
    let estr = |e: util::Error| e.to_string();

    // 1. The port's files.
    std::fs::create_dir_all(&o.location)
        .map_err(|e| format!("can't create {}: {e}", o.location.display()))?;
    remove_internal_documents(&o.location, &log);
    set(0.0, "Copying the PC port's files", "");
    let placed = place_files(&o.source, &o.location, &mut |d, t| {
        set(0.03 * d as f32 / t.max(1) as f32, "", "")
    })?;
    let n = placed.len();
    log(format!("Copied {n} files to {}", o.location.display()));
    // Remember what the setup put here, so `update` can remove files the repository drops
    // (without this, an older setup's leftovers stayed forever and broke the first update's build).
    if let Err(e) = crate::update::record_installed(&o.location, &placed) {
        log(format!(
            "note: could not record the installed file list: {e}"
        ));
    }
    if stopped() {
        return Err("cancelled".into());
    }

    // 2. The game files.
    let game_dir = match &o.game {
        GameSource::Folder(f) => f.clone(),
        GameSource::Iso { path, key } => {
            let dest = o.location.join(&o.link_name);
            let meta = std::fs::metadata(path).map_err(|e| e.to_string())?;
            let stamp = format!("{}|{}", path.display(), meta.len());
            let marker = dest.join(".copied-from-disc-image");
            if std::fs::read_to_string(&marker)
                .map(|s| s == stamp)
                .unwrap_or(false)
            {
                log("The game files were already copied from this disc image.".into());
            } else {
                set(0.03, "Copying the game from your disc image", "");
                let key = match key {
                    Some(k) => Some(iso::read_key_file(k).map_err(estr)?),
                    None => None,
                };
                let mut img = iso::open_ready(path, key).map_err(estr)?;
                img.files
                    .retain(|e| !e.path.to_ascii_uppercase().starts_with("PS3_UPDATE/"));
                let mut last = 0f32;
                let mut err = None;
                img.extract(&dest, &mut |d, t, file| {
                    let f = d as f32 / t.max(1) as f32;
                    if f - last > 0.002 || d == t {
                        last = f;
                        set(
                            0.03 + 0.22 * f,
                            "",
                            &format!(
                                "{} of {}  ·  {}",
                                util::human_bytes(d),
                                util::human_bytes(t),
                                file
                            ),
                        );
                    }
                    if stopped() && err.is_none() {
                        err = Some(());
                    }
                })
                .map_err(estr)?;
                if err.is_some() {
                    return Err("cancelled".into());
                }
                std::fs::write(&marker, stamp).map_err(|e| e.to_string())?;
                log(format!("Game files copied to {}", dest.display()));
            }
            dest
        }
    };
    if stopped() {
        return Err("cancelled".into());
    }

    // 3. Regenerate, patch and build (the command-line installer).
    let ext = if cfg!(windows) { ".exe" } else { "" };
    let cli = o
        .location
        .join("installer")
        .join(format!("infamous-installer{ext}"));
    let mut cmd = Command::new(&cli);
    cmd.arg("install")
        .arg("--root")
        .arg(&o.location)
        .arg("--game")
        .arg(&game_dir)
        .arg("-y");
    if let Some(r) = &o.rpcs3 {
        cmd.arg("--rpcs3").arg(r);
    }
    run_cli(cmd, &o.location, p, child, &stopped, &set, &log)?;

    // 4. Shortcuts.
    set(0.995, "Creating shortcuts", "");
    let launcher = o.location.join(format!("{}{ext}", o.game_title));
    let mut links = Vec::new();
    if o.desktop_shortcut {
        if let Some(d) = shell_folder("Desktop") {
            links.push(d.join(format!("{}.lnk", o.game_title)));
        }
    }
    if o.start_menu {
        if let Some(d) = shell_folder("Programs") {
            links.push(d.join(format!("{}.lnk", o.game_title)));
        }
    }
    for l in links {
        match make_shortcut(&l, &launcher, &o.location) {
            Ok(()) => log(format!("Shortcut: {}", l.display())),
            Err(e) => log(format!("Could not create {}: {e}", l.display())),
        }
    }
    if o.start_menu {
        add_update_shortcut(&o.location, &o.game_title, &log);
    }
    Ok(())
}

/// Remove maintainer-only coordination documents left by older packages. These are never player
/// files and must not survive a repair/update just because an old setup placed them first.
fn remove_internal_documents(root: &Path, log: &dyn Fn(String)) {
    fn visit(dir: &Path, removed: &mut Vec<PathBuf>) {
        let Ok(entries) = std::fs::read_dir(dir) else {
            return;
        };
        for entry in entries.flatten() {
            let path = entry.path();
            let Ok(kind) = entry.file_type() else {
                continue;
            };
            if kind.is_dir() {
                visit(&path, removed);
                continue;
            }
            if !kind.is_file() {
                continue;
            }
            let name = entry.file_name().to_string_lossy().to_ascii_lowercase();
            let private = name.contains("handoff")
                || name.starts_with("agents_")
                || name.starts_with("coord_")
                || name.starts_with("thread_perf");
            if private && std::fs::remove_file(&path).is_ok() {
                removed.push(path);
            }
        }
    }

    let mut removed = Vec::new();
    for dir in [root.join("infamous-recomp"), root.join(".installer")] {
        visit(&dir, &mut removed);
    }
    for path in removed {
        log(format!("Removed internal document: {}", path.display()));
    }
}

/// Run the command-line installer, mapping its output to the progress bar and log.
fn run_cli(
    mut cmd: Command,
    dir: &Path,
    p: &Arc<Mutex<Progress>>,
    child: &Arc<Mutex<Option<u32>>>,
    stopped: &dyn Fn() -> bool,
    set: &dyn Fn(f32, &str, &str),
    log: &dyn Fn(String),
) -> Result<(), String> {
    cmd.current_dir(dir)
        .env("PYTHONUNBUFFERED", "1")
        .env("PYTHONUTF8", "1");
    hide(&mut cmd)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    log(format!("> {}", util::show_cmd(&cmd)));
    let mut ch = cmd
        .spawn()
        .map_err(|e| format!("could not start {}: {e}", util::show_cmd(&cmd)))?;
    *child.lock().unwrap() = Some(ch.id());
    let (tx, rx) = mpsc::channel::<String>();
    for pipe in [
        ch.stdout
            .take()
            .map(|x| Box::new(x) as Box<dyn Read + Send>),
        ch.stderr
            .take()
            .map(|x| Box::new(x) as Box<dyn Read + Send>),
    ]
    .into_iter()
    .flatten()
    {
        let tx = tx.clone();
        std::thread::spawn(move || pump(pipe, &tx));
    }
    drop(tx);
    let mut error: Vec<String> = Vec::new();
    let mut in_error = false;
    let mut base = 0.25f32;
    for line in rx {
        let t = line.trim();
        if in_error {
            error.push(t.to_string());
        } else if let Some(e) = t.strip_prefix("ERROR:") {
            in_error = true;
            error.push(e.trim().to_string());
        } else if let Some(h) = t.strip_prefix("==> ") {
            if let Some((frac, phase)) = phase_for(h) {
                base = frac;
                set(frac, &phase, "");
            }
        } else if t.starts_with('[') && base < 0.25 {
            let mut g = p.lock().unwrap();
            g.detail = t.chars().take(110).collect();
        } else if t.starts_with('[') {
            // ninja: [n/m] ...
            if let Some((n, m)) = t[1..].split_once(']').and_then(|(a, _)| a.split_once('/')) {
                if let (Ok(n), Ok(m)) = (n.trim().parse::<f32>(), m.trim().parse::<f32>()) {
                    if base >= 0.58 && m > 0.0 {
                        let rest: String = t
                            .split_once(']')
                            .map(|x| x.1.trim())
                            .unwrap_or("")
                            .chars()
                            .take(90)
                            .collect();
                        set(
                            0.58 + 0.40 * n / m,
                            "",
                            &format!("{} of {} parts  ·  {rest}", n as u64, m as u64),
                        );
                    }
                }
            }
        } else if t.starts_with("Lifting") || t.contains("lifted ") || t.starts_with("Writing") {
            let mut g = p.lock().unwrap();
            g.detail = t.chars().take(110).collect();
        }
        log(line);
    }
    let st = ch.wait().map_err(|e| e.to_string())?;
    *child.lock().unwrap() = None;
    if stopped() {
        return Err("cancelled".into());
    }
    if !st.success() {
        let msg = if error.is_empty() {
            format!(
                "the installer stopped (exit code {})",
                st.code().unwrap_or(-1)
            )
        } else {
            error.join("\n")
        };
        return Err(msg);
    }
    Ok(())
}

/// The installer's `==> ` headings -> (progress, friendly phase).
fn phase_for(h: &str) -> Option<(f32, String)> {
    if let Some(r) = h.strip_prefix('[') {
        let (nm, title) = r.split_once(']')?;
        let (n, m) = nm.split_once('/')?;
        let (n, m): (f32, f32) = (n.trim().parse().ok()?, m.trim().parse().ok()?);
        return Some((
            0.27 + 0.26 * (n - 1.0) / m.max(1.0),
            format!("Rebuilding the game code: {}", title.trim()),
        ));
    }
    let x = match h {
        "Python" => (
            0.25,
            "Setting up Python (the first time downloads a few packages)",
        ),
        "Game files" => (0.25, "Checking your game"),
        "Checking the generated sources" => (0.26, "Checking for an earlier installation"),
        "Executable" => (0.26, "Unlocking the game's program file"),
        "Regenerating the game code from your files" => (0.27, "Rebuilding the game code"),
        "Applying the port's patches" => (0.53, "Applying the PC port's changes"),
        "Compiler" => (0.55, "Preparing the compiler"),
        "Configuring (CMake)" => (0.57, "Preparing the build"),
        "Done" => (0.99, "Finishing"),
        "Checking for updates" => (0.02, "Checking GitHub for a new version"),
        "Downloading the update" => (0.05, "Downloading the changed files"),
        "Up to date" => (0.99, "Already up to date"),
        _ if h.starts_with("Compiling") => (0.58, "Compiling the game (the long part)"),
        _ => return None,
    };
    Some((x.0, x.1.to_string()))
}

fn shell_folder(name: &str) -> Option<PathBuf> {
    #[cfg(windows)]
    {
        let ps = format!("[Environment]::GetFolderPath('{name}')");
        let out = util::capture(Command::new("powershell").args(["-NoProfile", "-Command", &ps]))?;
        let s = out.trim();
        if s.is_empty() {
            None
        } else {
            Some(PathBuf::from(s))
        }
    }
    #[cfg(not(windows))]
    {
        let _ = name;
        None
    }
}

fn make_shortcut(lnk: &Path, target: &Path, cwd: &Path) -> Result<(), String> {
    make_shortcut_icon(lnk, target, cwd, target)
}

fn make_shortcut_icon(lnk: &Path, target: &Path, cwd: &Path, icon: &Path) -> Result<(), String> {
    let q = |p: &Path| util::path_str(p).replace('\'', "''");
    let ps = format!(
        "$s = (New-Object -ComObject WScript.Shell).CreateShortcut('{}'); $s.TargetPath = '{}'; $s.WorkingDirectory = '{}'; $s.IconLocation = '{},0'; $s.Save()",
        q(lnk), q(target), q(cwd), q(icon)
    );
    let st = hide(Command::new("powershell").args([
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-Command",
        &ps,
    ]))
    .stdout(Stdio::null())
    .stderr(Stdio::null())
    .status()
    .map_err(|e| e.to_string())?;
    if st.success() {
        Ok(())
    } else {
        Err("PowerShell could not create it".into())
    }
}

pub fn open_folder(p: &Path) {
    #[cfg(windows)]
    let _ = Command::new("explorer").arg(p).spawn();
    #[cfg(not(windows))]
    let _ = Command::new("xdg-open").arg(p).spawn();
}

pub fn open_url(u: &str) {
    #[cfg(windows)]
    let _ = hide(Command::new("cmd").args(["/c", "start", "", u])).spawn();
    #[cfg(not(windows))]
    let _ = Command::new("xdg-open").arg(u).spawn();
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn phases() {
        assert_eq!(phase_for("Python").unwrap().0, 0.25);
        let (f, s) = phase_for("[2/19] Lift the PPU code to C++ (the long one)").unwrap();
        assert!(f > 0.27 && f < 0.3, "{f}");
        assert!(s.contains("Lift the PPU"));
        assert!(
            phase_for("Compiling -- the lifted code is huge; expect a long first build").is_some()
        );
        assert!(phase_for("something else").is_none());
    }

    #[test]
    fn repair_removes_internal_documents() {
        let root = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("target")
            .join(format!("internal-doc-cleanup-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(root.join("infamous-recomp/notes")).unwrap();
        std::fs::create_dir_all(root.join(".installer/old")).unwrap();
        std::fs::write(root.join("infamous-recomp/HANDOFF_old.md"), b"private").unwrap();
        std::fs::write(
            root.join("infamous-recomp/notes/coord_session.txt"),
            b"private",
        )
        .unwrap();
        std::fs::write(root.join(".installer/old/AGENTS_notes.md"), b"private").unwrap();
        std::fs::write(root.join("infamous-recomp/README.md"), b"public").unwrap();
        let lines = Mutex::new(Vec::new());

        remove_internal_documents(&root, &|line| lines.lock().unwrap().push(line));

        assert!(!root.join("infamous-recomp/HANDOFF_old.md").exists());
        assert!(!root
            .join("infamous-recomp/notes/coord_session.txt")
            .exists());
        assert!(!root.join(".installer/old/AGENTS_notes.md").exists());
        assert!(root.join("infamous-recomp/README.md").exists());
        assert_eq!(lines.lock().unwrap().len(), 3);
        let _ = std::fs::remove_dir_all(root);
    }
}
