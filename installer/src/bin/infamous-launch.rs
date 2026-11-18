//! `inFamous.exe`: the launcher. A plain native Windows options dialog with the game's settings (the
//! same ones as the in-game Settings menu, pause menu > Settings). Play writes them to `settings.ini` and starts the game
//! the way `installer/launch.txt` says, without a console window.
//!
//! `inFamous.exe --play` skips the window and starts the game straight away (the old behaviour).
//!
//! Dev builds (infamous-recomp/play_pgo.cmd): `infamous-launch.exe --settings-only --cwd <dir> --exe <game exe>`
//! shows the same window, saves `<dir>/settings.ini` and exits 0 on Play (the script then starts the
//! game itself), or exits 1 when the window is closed. Defaults come from the calling script's environment.
#![cfg_attr(windows, windows_subsystem = "windows")]
#![cfg_attr(not(windows), allow(dead_code))]

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

#[cfg_attr(not(windows), allow(dead_code))]
const REPO_URL: &str = "https://github.com/whompay/inFAMOUSreborn";
#[cfg_attr(not(windows), allow(dead_code))]
const REPO_TEXT: &str = "github.com/whompay/inFAMOUSreborn";

// ---- launch.txt ------------------------------------------------------------------------------

fn message(text: &str) {
    #[cfg(windows)]
    {
        #[link(name = "user32")]
        extern "system" {
            fn MessageBoxW(
                hwnd: *mut core::ffi::c_void,
                text: *const u16,
                caption: *const u16,
                kind: u32,
            ) -> i32;
        }
        let w = |s: &str| s.encode_utf16().chain(Some(0)).collect::<Vec<u16>>();
        let (t, c) = (w(text), w("inFamous"));
        // SAFETY: both strings are NUL-terminated UTF-16; MB_OK | MB_ICONERROR.
        unsafe { MessageBoxW(core::ptr::null_mut(), t.as_ptr(), c.as_ptr(), 0x10) };
    }
    #[cfg(not(windows))]
    eprintln!("{text}");
}

fn resolve(root: &Path, p: &str) -> PathBuf {
    let pb = PathBuf::from(p);
    if pb.is_absolute() {
        pb
    } else {
        p.split('/')
            .filter(|s| !s.is_empty())
            .fold(root.to_path_buf(), |a, s| a.join(s))
    }
}

struct Launch {
    root: PathBuf,
    exe: PathBuf,
    cwd: PathBuf,
    args: Vec<PathBuf>,
    env: Vec<(String, String)>,
    out: Option<String>,
    err: Option<String>,
    /// `--settings-only`: Play just saves and returns to the calling script.
    settings_only: bool,
}

impl Launch {
    fn load() -> Result<Self, String> {
        let me = std::env::current_exe().map_err(|e| e.to_string())?;
        let root = me.parent().ok_or("no folder")?.to_path_buf();
        let cfg = root.join("installer").join("launch.txt");
        let txt = std::fs::read_to_string(&cfg).map_err(|_| {
            "The game has not been built yet.\n\nRun inFamous Setup again to finish the installation."
                .to_string()
        })?;
        let (mut exe, mut cwd, mut out, mut err) = (None, None, None, None);
        let mut args = Vec::new();
        let mut env = Vec::new();
        for line in txt.lines() {
            let line = line.trim_end();
            if line.starts_with('#') || line.is_empty() {
                continue;
            }
            let Some((k, v)) = line.split_once('=') else {
                continue;
            };
            match k {
                "exe" => exe = Some(resolve(&root, v)),
                "cwd" => cwd = Some(resolve(&root, v)),
                "arg" => args.push(resolve(&root, v)),
                "stdout" => out = Some(v.to_string()),
                "stderr" => err = Some(v.to_string()),
                "env" => {
                    if let Some((a, b)) = v.split_once('=') {
                        env.push((a.to_string(), b.to_string()));
                    }
                }
                _ => {}
            }
        }
        let exe = exe.ok_or("launch.txt names no game executable")?;
        let cwd = cwd.unwrap_or_else(|| root.clone());
        Ok(Self {
            root,
            exe,
            cwd,
            args,
            env,
            out,
            err,
            settings_only: false,
        })
    }

    /// `--settings-only --cwd <dir> --exe <exe>` (dev scripts).
    fn from_args(args: &[String]) -> Result<Self, String> {
        let get = |name: &str| {
            args.iter()
                .position(|a| a == name)
                .and_then(|i| args.get(i + 1))
                .map(|v| PathBuf::from(v.trim_end_matches(['\\', '/'])))
        };
        let cwd = match get("--cwd") {
            Some(d) => d,
            None => std::env::current_dir().map_err(|e| e.to_string())?,
        };
        let exe = get("--exe").unwrap_or_default();
        let root = std::env::current_exe()
            .ok()
            .and_then(|p| p.parent().map(Path::to_path_buf))
            .unwrap_or_else(|| cwd.clone());
        Ok(Self {
            root,
            exe,
            cwd,
            args: Vec::new(),
            env: Vec::new(),
            out: None,
            err: None,
            settings_only: true,
        })
    }

    /// Where the exe-feature cache goes: the installer folder of an install, else next to the game's settings.
    fn cache_path(&self) -> PathBuf {
        if self.settings_only {
            self.cwd.join("launcher-features.txt")
        } else {
            self.root.join("installer").join("launcher-features.txt")
        }
    }

    fn env_value(&self, key: &str) -> Option<String> {
        self.env
            .iter()
            .rev()
            .find(|(k, _)| k == key)
            .map(|(_, v)| v.clone())
            .or_else(|| std::env::var(key).ok())
    }

    /// `settings.ini` lives in the game's working folder (the in-game menu writes it there too).
    fn settings_path(&self) -> PathBuf {
        self.cwd.join("settings.ini")
    }

    fn start(&self) -> Result<(), String> {
        if !self.exe.is_file() {
            return Err(format!(
                "{} is missing.\n\nRun inFamous Setup again to rebuild the game.",
                self.exe.display()
            ));
        }
        let mut c = Command::new(&self.exe);
        c.args(&self.args)
            .current_dir(&self.cwd)
            .envs(self.env.iter().cloned())
            .stdin(Stdio::null());
        // The previous run's log is kept as <name>.prev.log (play.err.prev.log): a player who hits a problem
        // usually restarts the game before sending the log, and File::create would have emptied it.
        let open = |name: &Option<String>| -> Stdio {
            match name
                .as_deref()
                .map(|n| resolve(&self.cwd, n))
                .and_then(|p| {
                    if p.is_file() {
                        let prev = p.with_extension("prev.log");
                        let _ = std::fs::remove_file(&prev);
                        let _ = std::fs::rename(&p, &prev);
                    }
                    std::fs::File::create(p).ok()
                }) {
                Some(f) => Stdio::from(f),
                None => Stdio::null(),
            }
        };
        c.stdout(open(&self.out)).stderr(open(&self.err));
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            c.creation_flags(0x0800_0000); // CREATE_NO_WINDOW
        }
        c.spawn()
            .map_err(|e| format!("Could not start {}: {e}", self.exe.display()))?;
        Ok(())
    }
}

// ---- the settings (same table as the in-game menu: ps3recomp/libs/video/rsx_ui_overlay.c) -----

struct Row {
    key: &'static str,
    label: &'static str,
    #[allow(dead_code)] // kept with each row for documentation; the dialog has no tooltips
    hint: &'static str,
    vals: &'static [&'static str],
    names: &'static [&'static str],
    /// Shown when neither settings.ini nor the launcher sets the key (menu_init's built-in default).
    fallback: usize,
}

const DISPLAY: &[Row] = &[
    Row {
        key: "FULLSCREEN",
        label: "Display Mode",
        hint: "Also switchable in game with F11.",
        vals: &["0", "1"],
        names: &["Windowed", "Fullscreen"],
        fallback: 0,
    },
    Row {
        key: "RES_SCALE",
        label: "Resolution",
        hint: "Internal rendering resolution.",
        vals: &["1", "1.5", "2", "2.5", "3"],
        names: &["720p", "1080p", "1440p", "1800p", "2160p (4K)"],
        fallback: 0,
    },
    Row {
        key: "PRESENT_VSYNC",
        label: "V-Sync",
        hint: "Synchronise frames with the monitor's refresh rate.",
        vals: &["0", "1"],
        names: &["Off", "On"],
        fallback: 0,
    },
    Row {
        key: "UI_FPS_CAP",
        label: "Frame Rate Limit",
        hint: "Upper limit for the frame rate.",
        vals: &["0", "60", "50", "45", "40", "30"],
        names: &["Off", "60 FPS", "50 FPS", "45 FPS", "40 FPS", "30 FPS"],
        fallback: 0,
    },
];

const UPSCALER_BOTH: Row = Row {
    key: "UPSCALER",
    label: "Upscaling",
    hint: "NVIDIA DLSS needs an RTX graphics card; AMD FSR works on any modern GPU.",
    vals: &[
        "off",
        "dlaa",
        "dlss_quality",
        "dlss_balanced",
        "dlss_performance",
        "fsr_native",
        "fsr_quality",
        "fsr_balanced",
        "fsr_performance",
    ],
    names: &[
        "Off",
        "NVIDIA DLAA",
        "DLSS Quality",
        "DLSS Balanced",
        "DLSS Performance",
        "FSR Native AA",
        "FSR Quality",
        "FSR Balanced",
        "FSR Performance",
    ],
    fallback: 0,
};
const UPSCALER_DLSS: Row = Row {
    key: "UPSCALER",
    label: "Upscaling",
    hint: "NVIDIA DLSS needs an RTX graphics card.",
    vals: &[
        "off",
        "dlaa",
        "dlss_quality",
        "dlss_balanced",
        "dlss_performance",
    ],
    names: &[
        "Off",
        "NVIDIA DLAA",
        "DLSS Quality",
        "DLSS Balanced",
        "DLSS Performance",
    ],
    fallback: 0,
};
const UPSCALER_FSR: Row = Row {
    key: "UPSCALER",
    label: "Upscaling",
    hint: "AMD FidelityFX Super Resolution.",
    vals: &[
        "off",
        "fsr_native",
        "fsr_quality",
        "fsr_balanced",
        "fsr_performance",
    ],
    names: &[
        "Off",
        "FSR Native AA",
        "FSR Quality",
        "FSR Balanced",
        "FSR Performance",
    ],
    fallback: 0,
};
const FRAME_GEN: Row = Row {
    key: "FSR_FG",
    label: "Frame Generation",
    hint: "AMD FSR frame generation.",
    vals: &["0", "1"],
    names: &["Off", "On (FSR)"],
    fallback: 0,
};

const QUALITY: &[Row] = &[
    Row {
        key: "SHADOWS",
        label: "Shadows",
        hint: "",
        vals: &["1", "0"],
        names: &["On", "Off"],
        fallback: 0,
    },
    Row {
        key: "TEX_ANISO",
        label: "Texture Filter",
        hint: "",
        vals: &["-1", "0", "4", "8", "16"],
        names: &[
            "Game Default",
            "Bilinear",
            "Anisotropic 4x",
            "Anisotropic 8x",
            "Anisotropic 16x",
        ],
        fallback: 0,
    },
    Row {
        key: "TEX_MIPS",
        label: "Mipmaps",
        hint: "",
        vals: &["1", "0"],
        names: &["On", "Off"],
        fallback: 0,
    },
];

const CONTROLS: &[Row] = &[
    Row {
        key: "KBM_GLYPHS",
        label: "Button Prompts",
        hint: "Automatic follows whichever device you used last.",
        vals: &["1", "0", "2"],
        names: &["Automatic", "Controller", "Keyboard & Mouse"],
        fallback: 0,
    },
    Row {
        key: "PAD_MOUSE_SPEED",
        label: "Mouse Speed",
        hint: "",
        vals: &["900", "650", "450", "325", "225"],
        names: &["1 (Slow)", "2", "3", "4", "5 (Fast)"],
        fallback: 2,
    },
    Row {
        key: "PAD_RUN_DEFAULT",
        label: "Default Movement",
        hint: "Walk: hold Shift to run. Run: hold Shift to walk. Requires a restart.",
        vals: &["0", "1"],
        names: &["Walk (Shift: Run)", "Run (Shift: Walk)"],
        fallback: 0,
    },
];

const GAME: &[Row] = &[Row {
    key: "PS3_LANG",
    label: "Language",
    hint: "Dialogue, subtitles and HUD. Korean and Chinese are text only.",
    vals: &[
        "english",
        "british",
        "french",
        "german",
        "italian",
        "spanish",
        "dutch",
        "polish",
        "portuguese",
        "russian",
        "korean",
        "chinese_t",
    ],
    names: &[
        "English (US)",
        "English (UK)",
        "French",
        "German",
        "Italian",
        "Spanish",
        "Dutch",
        "Polish",
        "Portuguese",
        "Russian",
        "Korean (text)",
        "Chinese (text)",
    ],
    fallback: 0,
}];

/// Order the in-game menu writes the keys in.
const INI_ORDER: &[&str] = &[
    "FULLSCREEN",
    "PRESENT_VSYNC",
    "UI_FPS_CAP",
    "KBM_GLYPHS",
    "PAD_MOUSE_SPEED",
    "PAD_RUN_DEFAULT",
    "RES_SCALE",
    "SHADOWS",
    "TEX_ANISO",
    "TEX_MIPS",
    "UPSCALER",
    "FSR_FG",
    "PS3_LANG",
];

impl Row {
    fn find(&self, v: &str) -> Option<usize> {
        let v = v.trim();
        if v.is_empty() {
            return None;
        }
        self.vals.iter().position(|x| *x == v).or_else(|| {
            if self.key == "RES_SCALE" {
                let f: f64 = v.parse().ok()?;
                self.vals
                    .iter()
                    .position(|x| (x.parse::<f64>().unwrap_or(-9.0) - f).abs() < 0.01)
            } else {
                None
            }
        })
    }
}

// ---- which upscaler rows this build has --------------------------------------------------------

/// The DLSS / FSR rows only exist in a game built with those SDKs; their menu labels are then
/// compiled into the exe, so look for them. Cached per exe (size + modified time).
#[derive(Clone, Copy, Default)]
struct Features {
    dlss: bool,
    fsr: bool,
}

fn exe_stamp(exe: &Path) -> Option<String> {
    let m = std::fs::metadata(exe).ok()?;
    let t = m
        .modified()
        .ok()?
        .duration_since(std::time::UNIX_EPOCH)
        .ok()?
        .as_secs();
    Some(format!("{} {}", m.len(), t))
}

fn scan_features(exe: &Path, cache: &Path) -> Features {
    let stamp = exe_stamp(exe);
    if let (Some(stamp), Ok(txt)) = (&stamp, std::fs::read_to_string(cache)) {
        let mut it = txt.lines();
        if it.next() == Some(stamp.as_str()) {
            let mut f = Features::default();
            for l in it {
                match l.trim() {
                    "dlss" => f.dlss = true,
                    "fsr" => f.fsr = true,
                    _ => {}
                }
            }
            return f;
        }
    }
    let mut f = Features::default();
    let needles: [&[u8]; 2] = [b"DLSS Quality\0", b"FSR Quality\0"];
    if let Ok(mut file) = std::fs::File::open(exe) {
        use std::io::Read;
        let keep = 16usize;
        let mut buf = vec![0u8; 8 << 20];
        let mut have = 0usize;
        loop {
            let n = match file.read(&mut buf[have..]) {
                Ok(0) | Err(_) => break,
                Ok(n) => n,
            };
            let end = have + n;
            let hay = &buf[..end];
            for (i, nd) in needles.iter().enumerate() {
                let hit = if i == 0 { &mut f.dlss } else { &mut f.fsr };
                if !*hit {
                    *hit = hay.windows(nd.len()).any(|w| w == *nd);
                }
            }
            if f.dlss && f.fsr {
                break;
            }
            let tail = end.min(keep);
            buf.copy_within(end - tail..end, 0);
            have = tail;
        }
        if let Some(stamp) = stamp {
            let mut t = stamp;
            if f.dlss {
                t.push_str("\ndlss");
            }
            if f.fsr {
                t.push_str("\nfsr");
            }
            t.push('\n');
            let _ = std::fs::write(cache, t);
        }
    }
    f
}

// ---- settings model (shared by the window and the tests) ----------------------------------------

/// The cached exe scan, if it matches the exe (instant); `None` means a scan is needed.
fn cached_features(exe: &Path, cache: &Path) -> Option<Features> {
    let stamp = exe_stamp(exe)?;
    let txt = std::fs::read_to_string(cache).ok()?;
    let mut it = txt.lines();
    if it.next() != Some(stamp.as_str()) {
        return None;
    }
    let mut f = Features::default();
    for l in it {
        match l.trim() {
            "dlss" => f.dlss = true,
            "fsr" => f.fsr = true,
            _ => {}
        }
    }
    Some(f)
}

struct State {
    launch: Launch,
    /// Values on disk in settings.ini (all keys, incl. ones the window doesn't show).
    ini: BTreeMap<String, String>,
    /// What the game would use right now for each shown key (index into the row's values).
    effective: BTreeMap<&'static str, usize>,
    cur: BTreeMap<&'static str, usize>,
    features: Option<Features>,
}

impl State {
    fn new(launch: Launch) -> Self {
        let ini = std::fs::read_to_string(launch.settings_path())
            .map(|t| {
                t.lines()
                    .filter(|l| !l.starts_with(';') && !l.starts_with('#'))
                    .filter_map(|l| l.split_once('='))
                    .map(|(k, v)| (k.trim().to_string(), v.trim().to_string()))
                    .collect()
            })
            .unwrap_or_default();
        let features = cached_features(&launch.exe, &launch.cache_path());
        let mut s = Self {
            launch,
            ini,
            effective: BTreeMap::new(),
            cur: BTreeMap::new(),
            features,
        };
        s.sync_rows();
        s
    }

    fn set_features(&mut self, f: Features) {
        self.features = Some(f);
        self.sync_rows();
    }

    /// Groups of rows, in display order (a separator line goes between groups).
    fn groups(&self) -> Vec<Vec<&'static Row>> {
        let mut gfx: Vec<&'static Row> = Vec::new();
        if let Some(f) = self.features {
            match (f.dlss, f.fsr) {
                (true, true) => gfx.push(&UPSCALER_BOTH),
                (true, false) => gfx.push(&UPSCALER_DLSS),
                (false, true) => gfx.push(&UPSCALER_FSR),
                _ => {}
            }
            if f.fsr {
                gfx.push(&FRAME_GEN);
            }
        }
        gfx.extend(QUALITY.iter());
        vec![
            DISPLAY.iter().collect(),
            gfx,
            CONTROLS.iter().collect(),
            GAME.iter().collect(),
        ]
    }

    fn default_index(&self, r: &Row) -> usize {
        self.launch
            .env_value(r.key)
            .and_then(|v| r.find(&v))
            .unwrap_or(r.fallback)
    }

    fn sync_rows(&mut self) {
        for r in self.groups().into_iter().flatten() {
            if self.effective.contains_key(r.key) {
                continue;
            }
            let k = self
                .ini
                .get(r.key)
                .and_then(|v| r.find(v))
                .unwrap_or_else(|| self.default_index(r));
            self.effective.insert(r.key, k);
            self.cur.insert(r.key, k);
        }
    }

    fn get(&self, r: &Row) -> usize {
        self.cur
            .get(r.key)
            .copied()
            .unwrap_or(r.fallback)
            .min(r.vals.len() - 1)
    }

    fn dirty(&self) -> bool {
        self.cur
            .iter()
            .any(|(k, v)| self.effective.get(k) != Some(v))
    }

    fn save(&mut self) -> Result<(), String> {
        let rows: Vec<&'static Row> = self.groups().into_iter().flatten().collect();
        let mut vals = self.ini.clone();
        for (k, &i) in &self.cur {
            if let Some(r) = rows.iter().find(|r| r.key == *k) {
                vals.insert(k.to_string(), r.vals[i.min(r.vals.len() - 1)].to_string());
            }
        }
        let mut t = String::from(
            "; inFamous recomp settings (written by the in-game Settings menu, F10). Loaded at startup; overrides the launcher.\n",
        );
        for k in INI_ORDER {
            if let Some(v) = vals.get(*k) {
                t.push_str(&format!("{k}={v}\n"));
            }
        }
        for (k, v) in &vals {
            if !INI_ORDER.contains(&k.as_str()) {
                t.push_str(&format!("{k}={v}\n"));
            }
        }
        std::fs::write(self.launch.settings_path(), t)
            .map_err(|e| format!("Could not save settings.ini: {e}"))?;
        self.ini = vals;
        self.effective = self.cur.clone();
        Ok(())
    }

    fn restore_defaults(&mut self) {
        for r in self.groups().into_iter().flatten() {
            let d = self.default_index(r);
            self.cur.insert(r.key, d);
        }
    }

    /// Play: save if needed, then start the game (or, `--settings-only`, just report success).
    fn play(&mut self) -> Result<(), String> {
        if self.dirty() {
            self.save()?;
        }
        if self.launch.settings_only {
            Ok(())
        } else {
            self.launch.start()
        }
    }
}

// ---- the window: a plain native Windows dialog (real controls, ClearType, per-monitor DPI) -------

#[cfg(windows)]
mod win {
    use super::*;
    use std::cell::RefCell;
    use std::ffi::c_void;
    use windows_sys::Win32::Foundation::*;
    use windows_sys::Win32::Graphics::Gdi::*;
    use windows_sys::Win32::System::ApplicationInstallationAndServicing::{
        ActivateActCtx, CreateActCtxW, ACTCTXW,
    };
    use windows_sys::Win32::System::LibraryLoader::GetModuleHandleW;
    use windows_sys::Win32::UI::Controls::{
        InitCommonControlsEx, ICC_STANDARD_CLASSES, INITCOMMONCONTROLSEX,
    };
    use windows_sys::Win32::UI::HiDpi::{
        AdjustWindowRectExForDpi, GetDpiForSystem, GetDpiForWindow, SetProcessDpiAwarenessContext,
        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2,
    };
    use windows_sys::Win32::UI::Input::KeyboardAndMouse::{
        EnableWindow, SetFocus, TrackMouseEvent, TME_LEAVE, TRACKMOUSEEVENT,
    };
    use windows_sys::Win32::UI::Shell::ShellExecuteW;
    use windows_sys::Win32::UI::WindowsAndMessaging::*;

    // Message / style numbers spelled out so they don't depend on which module a crate version keeps them in.
    const WM_MOUSELEAVE_: u32 = 0x02A3;
    const WM_DPICHANGED_: u32 = 0x02E0;
    const WM_FEATURES: u32 = 0x8000 + 1; // WM_APP + 1
    const CB_SETMINVISIBLE_: u32 = 0x1701;
    const CBN_SELCHANGE_: u32 = 1;
    const BN_CLICKED_: u32 = 0;
    const ID_PLAY: usize = 1; // IDOK: Enter
    const ID_CANCEL: usize = 2; // IDCANCEL: Esc
    const ID_RESTORE: usize = 10;
    const ID_SAVE: usize = 11;
    const ID_COMBO0: usize = 100;

    // Layout at 96 dpi (100 %); everything is scaled by the window's dpi.
    const CW: i32 = 420; // client width
    const M: i32 = 9; // outer margin
    const BANNER_H: i32 = 76;
    const ROW_H: i32 = 27;
    const SEP_H: i32 = 9;
    const LIST_PAD: i32 = 4;
    const COMBO_W: i32 = 190;
    const BTN_H: i32 = 25;
    const LINK_H: i32 = 16;

    const fn rgb(r: u8, g: u8, b: u8) -> COLORREF {
        r as u32 | (g as u32) << 8 | (b as u32) << 16
    }
    const C_BANNER: COLORREF = rgb(0x0E, 0x0E, 0x0E);
    const C_BANNER_EDGE: COLORREF = rgb(0x50, 0x50, 0x50);
    const C_PLATE: COLORREF = rgb(0x1E, 0x74, 0xC8);
    const C_WHITE: COLORREF = rgb(0xFF, 0xFF, 0xFF);
    const C_INK: COLORREF = rgb(0x00, 0x00, 0x00);
    const C_FRAME: COLORREF = rgb(0xAB, 0xAB, 0xAB);
    const C_RULE: COLORREF = rgb(0xA0, 0xA0, 0xA0);
    const C_LINK: COLORREF = rgb(0x6D, 0x6D, 0x6D);
    const C_LINK_HOT: COLORREF = rgb(0x00, 0x66, 0xCC);

    fn w(s: &str) -> Vec<u16> {
        s.encode_utf16().chain(Some(0)).collect()
    }

    struct Fonts {
        ui: HFONT,
        title: HFONT,
        link: HFONT,
        link_hot: HFONT,
    }

    impl Fonts {
        fn new(dpi: u32) -> Self {
            let mk = |face: &str, px: i32, weight: i32, underline: bool| -> HFONT {
                // SAFETY: LOGFONTW is plain data; zeroed is a valid starting point.
                let mut lf: LOGFONTW = unsafe { std::mem::zeroed() };
                lf.lfHeight = -scale(px, dpi);
                lf.lfWeight = weight;
                lf.lfUnderline = underline as _;
                lf.lfCharSet = DEFAULT_CHARSET as _;
                lf.lfQuality = CLEARTYPE_QUALITY as _;
                for (i, c) in face.encode_utf16().take(31).enumerate() {
                    lf.lfFaceName[i] = c;
                }
                // SAFETY: valid LOGFONTW.
                unsafe { CreateFontIndirectW(&lf) }
            };
            Self {
                ui: mk("Segoe UI", 12, 400, false),
                title: mk("Segoe UI Semibold", 19, 600, false),
                link: mk("Segoe UI", 11, 400, false),
                link_hot: mk("Segoe UI", 11, 400, true),
            }
        }
        fn free(&self) {
            // SAFETY: fonts we created, not selected into any DC any more.
            unsafe {
                for f in [self.ui, self.title, self.link, self.link_hot] {
                    DeleteObject(f as _);
                }
            }
        }
    }

    fn scale(v: i32, dpi: u32) -> i32 {
        ((v as i64 * dpi as i64 + 48) / 96) as i32
    }

    struct Combo {
        hwnd: HWND,
        row: &'static Row,
    }

    struct Win {
        hwnd: HWND,
        state: State,
        dpi: u32,
        fonts: Fonts,
        combos: Vec<Combo>,
        /// Label rows (y range at current dpi) and separator lines, for painting.
        labels: Vec<(i32, &'static str)>,
        seps: Vec<i32>,
        list: RECT,
        btn_restore: HWND,
        btn_save: HWND,
        btn_play: HWND,
        link: RECT,
        link_w: i32,
        link_hot: bool,
        played: bool,
    }

    thread_local! {
        static APP: RefCell<Option<Win>> = const { RefCell::new(None) };
    }

    enum After {
        Nothing,
        Fail(String),
        Close,
    }

    impl Win {
        fn s(&self, v: i32) -> i32 {
            scale(v, self.dpi)
        }

        fn child(&self, class: &str, text: &str, style: u32, id: usize) -> HWND {
            let (c, t) = (w(class), w(text));
            // SAFETY: plain child window creation; hwnd is our live top-level window.
            let h = unsafe {
                CreateWindowExW(
                    WS_EX_NOPARENTNOTIFY,
                    c.as_ptr(),
                    t.as_ptr(),
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
                    0,
                    0,
                    10,
                    10,
                    self.hwnd,
                    id as _,
                    GetModuleHandleW(std::ptr::null()),
                    std::ptr::null(),
                )
            };
            // SAFETY: valid window + font.
            unsafe { SendMessageW(h, WM_SETFONT, self.fonts.ui as WPARAM, 0) };
            h
        }

        /// (Re)create the drop-downs for the current set of rows.
        fn build_combos(&mut self) {
            for c in self.combos.drain(..) {
                // SAFETY: our child window.
                unsafe { DestroyWindow(c.hwnd) };
            }
            let rows: Vec<&'static Row> = self.state.groups().into_iter().flatten().collect();
            for (i, r) in rows.into_iter().enumerate() {
                let h = self.child(
                    "COMBOBOX",
                    "",
                    CBS_DROPDOWNLIST as u32 | WS_VSCROLL,
                    ID_COMBO0 + i,
                );
                for n in r.names {
                    let t = w(n);
                    // SAFETY: NUL-terminated string, copied by the control.
                    unsafe { SendMessageW(h, CB_ADDSTRING, 0, t.as_ptr() as LPARAM) };
                }
                // SAFETY: plain control messages; the height given here is field + open list.
                unsafe {
                    SetWindowPos(
                        h,
                        std::ptr::null_mut(),
                        0,
                        0,
                        self.s(COMBO_W),
                        self.s(300),
                        SWP_NOZORDER | SWP_NOACTIVATE,
                    );
                    SendMessageW(h, CB_SETMINVISIBLE_, 12, 0);
                    SendMessageW(h, CB_SETCURSEL, self.state.get(r) as WPARAM, 0);
                }
                self.combos.push(Combo { hwnd: h, row: r });
            }
            // keep tab order: combos first, then the buttons
            for b in [self.btn_restore, self.btn_save, self.btn_play] {
                // SAFETY: our child windows.
                unsafe {
                    SetWindowPos(
                        b,
                        HWND_BOTTOM,
                        0,
                        0,
                        0,
                        0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE,
                    )
                };
            }
        }

        fn refresh_values(&self) {
            for c in &self.combos {
                // SAFETY: plain control message.
                unsafe { SendMessageW(c.hwnd, CB_SETCURSEL, self.state.get(c.row) as WPARAM, 0) };
            }
            self.refresh_save();
        }

        fn refresh_save(&self) {
            // SAFETY: our child window.
            unsafe { EnableWindow(self.btn_save, self.state.dirty() as BOOL) };
        }

        /// Position everything for the current dpi and size the window to fit. `at`: top-left to use.
        fn layout(&mut self, at: Option<(i32, i32)>) {
            let dpi = self.dpi;
            let s = move |v| scale(v, dpi);
            let cw = s(CW);
            let mut y = s(M) + s(BANNER_H) + s(M);
            let list_top = y;
            y += s(LIST_PAD);
            self.labels.clear();
            self.seps.clear();
            let groups = self.state.groups();
            let n = groups.len();
            let mut ci = 0;
            let combo_w = s(COMBO_W);
            let combo_x = cw - s(M) - s(8) - combo_w;
            for (gi, rows) in groups.into_iter().enumerate() {
                for r in rows {
                    let row_h = s(ROW_H);
                    self.labels.push((y + row_h / 2, r.label));
                    if let Some(c) = self.combos.get(ci) {
                        let mut rc: RECT = RECT {
                            left: 0,
                            top: 0,
                            right: 0,
                            bottom: 0,
                        };
                        // SAFETY: our child window; height = closed field + drop list.
                        unsafe {
                            GetWindowRect(c.hwnd, &mut rc);
                            let field_h = rc.bottom - rc.top;
                            SetWindowPos(
                                c.hwnd,
                                std::ptr::null_mut(),
                                combo_x,
                                y + (row_h - field_h) / 2,
                                combo_w,
                                s(300),
                                SWP_NOZORDER | SWP_NOACTIVATE,
                            );
                        }
                    }
                    ci += 1;
                    y += row_h;
                }
                if gi + 1 < n {
                    self.seps.push(y + s(SEP_H) / 2);
                    y += s(SEP_H);
                }
            }
            y += s(LIST_PAD);
            self.list = RECT {
                left: s(M),
                top: list_top,
                right: cw - s(M),
                bottom: y,
            };
            y += s(M);
            let bh = s(BTN_H);
            // SAFETY: our child windows.
            unsafe {
                let place = |h: HWND, x: i32, wd: i32| {
                    SetWindowPos(
                        h,
                        std::ptr::null_mut(),
                        x,
                        y,
                        wd,
                        bh,
                        SWP_NOZORDER | SWP_NOACTIVATE,
                    );
                };
                place(self.btn_restore, s(M), s(128));
                place(self.btn_play, cw - s(M) - s(80), s(80));
                place(self.btn_save, cw - s(M) - s(80) - s(6) - s(80), s(80));
            }
            y += bh + s(5);
            self.link = RECT {
                left: 0,
                top: y,
                right: cw,
                bottom: y + s(LINK_H),
            };
            // SAFETY: measuring text on the window's DC, released right after.
            unsafe {
                let dc = GetDC(self.hwnd);
                let old = SelectObject(dc, self.fonts.link as _);
                let t: Vec<u16> = REPO_TEXT.encode_utf16().collect();
                let mut sz = SIZE { cx: 0, cy: 0 };
                GetTextExtentPoint32W(dc, t.as_ptr(), t.len() as i32, &mut sz);
                SelectObject(dc, old);
                ReleaseDC(self.hwnd, dc);
                self.link_w = sz.cx;
            }
            y += s(LINK_H) + s(6);
            let ch = y;

            // window size from client size
            let style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
            let mut r = RECT {
                left: 0,
                top: 0,
                right: cw,
                bottom: ch,
            };
            // SAFETY: valid RECT.
            unsafe { AdjustWindowRectExForDpi(&mut r, style, 0, 0, self.dpi) };
            let (ww, wh) = (r.right - r.left, r.bottom - r.top);
            // SAFETY: our window.
            unsafe {
                match at {
                    Some((x, y)) => SetWindowPos(
                        self.hwnd,
                        std::ptr::null_mut(),
                        x,
                        y,
                        ww,
                        wh,
                        SWP_NOZORDER | SWP_NOACTIVATE,
                    ),
                    None => SetWindowPos(
                        self.hwnd,
                        std::ptr::null_mut(),
                        0,
                        0,
                        ww,
                        wh,
                        SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE,
                    ),
                };
                InvalidateRect(self.hwnd, std::ptr::null(), 1);
            }
        }

        fn set_fonts(&mut self) {
            let old = std::mem::replace(&mut self.fonts, Fonts::new(self.dpi));
            for h in self.combos.iter().map(|c| c.hwnd).chain([
                self.btn_restore,
                self.btn_save,
                self.btn_play,
            ]) {
                // SAFETY: our child windows; redraw with the new font.
                unsafe { SendMessageW(h, WM_SETFONT, self.fonts.ui as WPARAM, 1) };
            }
            old.free();
        }

        fn paint(&self, dc: HDC) {
            // SAFETY: GDI drawing on the paint DC; every object we create is deleted again.
            unsafe {
                let s = |v| self.s(v);
                SetBkMode(dc, TRANSPARENT as _);
                let fill = |r: &RECT, c: COLORREF| {
                    let b = CreateSolidBrush(c);
                    FillRect(dc, r, b);
                    DeleteObject(b as _);
                };
                let frame = |r: &RECT, c: COLORREF| {
                    let b = CreateSolidBrush(c);
                    FrameRect(dc, r, b);
                    DeleteObject(b as _);
                };
                let cw = s(CW);

                // ---- title box
                let banner = RECT {
                    left: s(M),
                    top: s(M),
                    right: cw - s(M),
                    bottom: s(M) + s(BANNER_H),
                };
                fill(&banner, C_BANNER);
                frame(&banner, C_BANNER_EDGE);
                let old_font = SelectObject(dc, self.fonts.title as _);
                let words = ["INFAMOUS", "REBORN"];
                let track = s(7);
                let gap = s(14);
                let mut glyphs: Vec<(Vec<u16>, i32, i32)> = Vec::new(); // (char, width, extra gap before)
                for (wi, word) in words.iter().enumerate() {
                    for (ci, ch) in word.encode_utf16().enumerate() {
                        let mut sz = SIZE { cx: 0, cy: 0 };
                        GetTextExtentPoint32W(dc, &ch, 1, &mut sz);
                        glyphs.push((vec![ch], sz.cx, if ci == 0 && wi > 0 { gap } else { 0 }));
                    }
                }
                let text_w: i32 = glyphs.iter().map(|(_, w, g)| w + g).sum::<i32>()
                    + track * (glyphs.len() as i32 - 1);
                let mut tm: TEXTMETRICW = std::mem::zeroed();
                GetTextMetricsW(dc, &mut tm);
                let cy = (banner.top + banner.bottom) / 2;
                let plate_h = s(36);
                let plate = RECT {
                    left: cw / 2 - text_w / 2 - s(18),
                    top: cy - plate_h / 2,
                    right: cw / 2 - text_w / 2 - s(18) + text_w + s(36),
                    bottom: cy - plate_h / 2 + plate_h,
                };
                let rule_h = s(2).max(2);
                fill(
                    &RECT {
                        left: banner.left + s(12),
                        top: cy - rule_h / 2,
                        right: plate.left,
                        bottom: cy - rule_h / 2 + rule_h,
                    },
                    C_PLATE,
                );
                fill(
                    &RECT {
                        left: plate.right,
                        top: cy - rule_h / 2,
                        right: banner.right - s(12),
                        bottom: cy - rule_h / 2 + rule_h,
                    },
                    C_PLATE,
                );
                fill(&plate, C_PLATE);
                SetTextColor(dc, C_WHITE);
                let mut x = cw / 2 - text_w / 2;
                let ty = cy - tm.tmHeight / 2;
                for (ch, wd, g) in &glyphs {
                    x += g;
                    TextOutW(dc, x, ty, ch.as_ptr(), 1);
                    x += wd + track;
                }

                // ---- settings list
                fill(&self.list, C_WHITE);
                frame(&self.list, C_FRAME);
                SelectObject(dc, self.fonts.ui as _);
                SetTextColor(dc, C_INK);
                for (cy, label) in &self.labels {
                    let t = w(label);
                    let mut r = RECT {
                        left: self.list.left + s(10),
                        top: cy - s(ROW_H) / 2,
                        right: cw / 2,
                        bottom: cy + s(ROW_H) / 2,
                    };
                    DrawTextW(
                        dc,
                        t.as_ptr(),
                        -1,
                        &mut r,
                        DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX,
                    );
                }
                for y in &self.seps {
                    fill(
                        &RECT {
                            left: self.list.left + 1,
                            top: *y,
                            right: self.list.right - 1,
                            bottom: y + 1,
                        },
                        C_RULE,
                    );
                }

                // ---- official source link
                SelectObject(
                    dc,
                    if self.link_hot {
                        self.fonts.link_hot
                    } else {
                        self.fonts.link
                    } as _,
                );
                SetTextColor(dc, if self.link_hot { C_LINK_HOT } else { C_LINK });
                let t = w(REPO_TEXT);
                let mut r = self.link;
                DrawTextW(
                    dc,
                    t.as_ptr(),
                    -1,
                    &mut r,
                    DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX,
                );
                SelectObject(dc, old_font);
            }
        }

        /// Clickable part of the link line (just the text).
        fn link_hit(&self, x: i32, y: i32) -> bool {
            let text_w = self.link_w;
            let cx = (self.link.left + self.link.right) / 2;
            y >= self.link.top
                && y < self.link.bottom
                && x >= cx - text_w / 2
                && x < cx + text_w / 2
        }

        fn command(&mut self, id: usize, code: u32) -> After {
            match id {
                ID_PLAY if code == BN_CLICKED_ => match self.state.play() {
                    Ok(()) => {
                        self.played = true;
                        After::Close
                    }
                    Err(e) => After::Fail(e),
                },
                ID_CANCEL => After::Close,
                ID_RESTORE if code == BN_CLICKED_ => {
                    self.state.restore_defaults();
                    self.refresh_values();
                    After::Nothing
                }
                ID_SAVE if code == BN_CLICKED_ => {
                    let r = self.state.save();
                    self.refresh_save();
                    match r {
                        Ok(()) => After::Nothing,
                        Err(e) => After::Fail(e),
                    }
                }
                i if i >= ID_COMBO0 && code == CBN_SELCHANGE_ => {
                    if let Some(c) = self.combos.get(i - ID_COMBO0) {
                        // SAFETY: plain control message.
                        let sel = unsafe { SendMessageW(c.hwnd, CB_GETCURSEL, 0, 0) };
                        if sel >= 0 {
                            self.state.cur.insert(c.row.key, sel as usize);
                        }
                    }
                    self.refresh_save();
                    After::Nothing
                }
                _ => After::Nothing,
            }
        }
    }

    fn lo(v: usize) -> u32 {
        (v & 0xFFFF) as u32
    }
    fn hi(v: usize) -> u32 {
        ((v >> 16) & 0xFFFF) as u32
    }
    fn mouse_xy(l: LPARAM) -> (i32, i32) {
        (
            (l & 0xFFFF) as i16 as i32,
            ((l >> 16) & 0xFFFF) as i16 as i32,
        )
    }

    unsafe extern "system" fn wndproc(hwnd: HWND, msg: u32, wp: WPARAM, lp: LPARAM) -> LRESULT {
        match msg {
            WM_CLOSE => {
                DestroyWindow(hwnd);
                return 0;
            }
            WM_DESTROY => {
                PostQuitMessage(0);
                return 0;
            }
            WM_ERASEBKGND => {} // default: COLOR_BTNFACE from the class brush
            _ => {}
        }
        // Everything else needs the state; never hold it across a modal call (message box).
        let after = APP.with(|a| {
            let Ok(mut g) = a.try_borrow_mut() else {
                return None;
            };
            let app = g.as_mut()?;
            if app.hwnd != hwnd {
                return None;
            }
            match msg {
                WM_PAINT => {
                    let mut ps: PAINTSTRUCT = std::mem::zeroed();
                    let dc = BeginPaint(hwnd, &mut ps);
                    app.paint(dc);
                    EndPaint(hwnd, &ps);
                    Some((0, After::Nothing))
                }
                WM_COMMAND => Some((0, app.command(lo(wp) as usize, hi(wp)))),
                WM_MOUSEMOVE => {
                    let (x, y) = mouse_xy(lp);
                    let hot = app.link_hit(x, y);
                    if hot != app.link_hot {
                        app.link_hot = hot;
                        InvalidateRect(hwnd, &app.link, 0);
                    }
                    let mut t = TRACKMOUSEEVENT {
                        cbSize: std::mem::size_of::<TRACKMOUSEEVENT>() as u32,
                        dwFlags: TME_LEAVE,
                        hwndTrack: hwnd,
                        dwHoverTime: 0,
                    };
                    TrackMouseEvent(&mut t);
                    Some((0, After::Nothing))
                }
                WM_MOUSELEAVE_ => {
                    if app.link_hot {
                        app.link_hot = false;
                        InvalidateRect(hwnd, &app.link, 0);
                    }
                    Some((0, After::Nothing))
                }
                WM_SETCURSOR if app.link_hot && lo(lp as usize) == HTCLIENT => {
                    SetCursor(LoadCursorW(std::ptr::null_mut(), IDC_HAND));
                    Some((1, After::Nothing))
                }
                WM_LBUTTONUP => {
                    let (x, y) = mouse_xy(lp);
                    if app.link_hit(x, y) {
                        let (op, url) = (w("open"), w(REPO_URL));
                        ShellExecuteW(
                            hwnd,
                            op.as_ptr(),
                            url.as_ptr(),
                            std::ptr::null(),
                            std::ptr::null(),
                            SW_SHOWNORMAL,
                        );
                    }
                    Some((0, After::Nothing))
                }
                WM_FEATURES => {
                    let f = Features {
                        dlss: wp & 1 != 0,
                        fsr: wp & 2 != 0,
                    };
                    let before = app.state.groups().into_iter().flatten().count();
                    app.state.set_features(f);
                    if app.state.groups().into_iter().flatten().count() != before {
                        app.build_combos();
                        app.layout(None);
                    }
                    Some((0, After::Nothing))
                }
                WM_DPICHANGED_ => {
                    app.dpi = hi(wp).max(96);
                    app.set_fonts();
                    let r = &*(lp as *const RECT);
                    app.layout(Some((r.left, r.top)));
                    Some((0, After::Nothing))
                }
                _ => None,
            }
        });
        match after {
            Some((ret, After::Nothing)) => ret,
            Some((ret, After::Fail(e))) => {
                message(&e);
                ret
            }
            Some((ret, After::Close)) => {
                DestroyWindow(hwnd);
                ret
            }
            None => DefWindowProcW(hwnd, msg, wp, lp),
        }
    }

    /// Modern (Windows 10/11) control look: activate Common Controls 6 from a tiny manifest.
    fn visual_styles() {
        let manifest = r#"<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <dependency><dependentAssembly>
    <assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls" version="6.0.0.0"
      processorArchitecture="*" publicKeyToken="6595b64144ccf1df" language="*"/>
  </dependentAssembly></dependency>
</assembly>
"#;
        let path = std::env::temp_dir().join("infamous-launcher.manifest");
        if std::fs::read_to_string(&path).ok().as_deref() != Some(manifest)
            && std::fs::write(&path, manifest).is_err()
        {
            return;
        }
        let p = w(&path.to_string_lossy());
        // SAFETY: ACTCTXW is plain data; the path string outlives the call.
        unsafe {
            let mut ctx: ACTCTXW = std::mem::zeroed();
            ctx.cbSize = std::mem::size_of::<ACTCTXW>() as u32;
            ctx.lpSource = p.as_ptr();
            let h = CreateActCtxW(&ctx);
            if h != INVALID_HANDLE_VALUE {
                let mut cookie = 0usize;
                ActivateActCtx(h, &mut cookie);
            }
            let icc = INITCOMMONCONTROLSEX {
                dwSize: std::mem::size_of::<INITCOMMONCONTROLSEX>() as u32,
                dwICC: ICC_STANDARD_CLASSES,
            };
            InitCommonControlsEx(&icc);
        }
    }

    fn make_icon(n: usize) -> HICON {
        let rgba = icon_rgba(n);
        let mut bgra = rgba.clone();
        for p in bgra.chunks_exact_mut(4) {
            p.swap(0, 2);
        }
        let and = vec![0u8; n * n / 8];
        // SAFETY: buffers sized for an n x n 32-bpp icon.
        unsafe {
            CreateIcon(
                GetModuleHandleW(std::ptr::null()),
                n as i32,
                n as i32,
                1,
                32,
                and.as_ptr(),
                bgra.as_ptr(),
            )
        }
    }

    /// Shows the window; returns true when Play was pressed (and succeeded).
    pub fn run(state: State) -> bool {
        // SAFETY: plain Win32 window setup and message loop on this (the only UI) thread.
        unsafe {
            SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            visual_styles();
            let inst = GetModuleHandleW(std::ptr::null());
            let class = w("inFamousRebornLauncher");
            let wc = WNDCLASSEXW {
                cbSize: std::mem::size_of::<WNDCLASSEXW>() as u32,
                style: CS_HREDRAW | CS_VREDRAW,
                lpfnWndProc: Some(wndproc),
                cbClsExtra: 0,
                cbWndExtra: 0,
                hInstance: inst,
                hIcon: make_icon(32),
                hCursor: LoadCursorW(std::ptr::null_mut(), IDC_ARROW),
                hbrBackground: (COLOR_BTNFACE + 1) as usize as HBRUSH,
                lpszMenuName: std::ptr::null(),
                lpszClassName: class.as_ptr(),
                hIconSm: make_icon(16),
            };
            RegisterClassExW(&wc);
            let title = w("inFamous Reborn - Options");
            let style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
            let hwnd = CreateWindowExW(
                WS_EX_CONTROLPARENT,
                class.as_ptr(),
                title.as_ptr(),
                style,
                CW_USEDEFAULT,
                CW_USEDEFAULT,
                100,
                100,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                inst,
                std::ptr::null(),
            );
            if hwnd.is_null() {
                return state.launch.settings_only;
            }
            let mut dpi = GetDpiForWindow(hwnd);
            if dpi == 0 {
                dpi = GetDpiForSystem().max(96);
            }

            // exe scan (first run only; cached afterwards)
            if state.features.is_none() {
                let (exe, cache) = (state.launch.exe.clone(), state.launch.cache_path());
                let target = hwnd as usize;
                std::thread::spawn(move || {
                    let f = scan_features(&exe, &cache);
                    let bits = f.dlss as usize | (f.fsr as usize) << 1;
                    PostMessageW(target as HWND, WM_FEATURES, bits, 0);
                });
            }

            let mut app = Win {
                hwnd,
                state,
                dpi,
                fonts: Fonts::new(dpi),
                combos: Vec::new(),
                labels: Vec::new(),
                seps: Vec::new(),
                list: RECT {
                    left: 0,
                    top: 0,
                    right: 0,
                    bottom: 0,
                },
                btn_restore: std::ptr::null_mut(),
                btn_save: std::ptr::null_mut(),
                btn_play: std::ptr::null_mut(),
                link: RECT {
                    left: 0,
                    top: 0,
                    right: 0,
                    bottom: 0,
                },
                link_w: 0,
                link_hot: false,
                played: false,
            };
            app.btn_restore = app.child(
                "BUTTON",
                "Restore to Defaults",
                BS_PUSHBUTTON as u32,
                ID_RESTORE,
            );
            app.btn_save = app.child("BUTTON", "Save", BS_PUSHBUTTON as u32, ID_SAVE);
            app.btn_play = app.child("BUTTON", "Play", BS_DEFPUSHBUTTON as u32, ID_PLAY);
            app.build_combos();
            app.layout(None);
            app.refresh_save();

            // centre on the work area of the window's monitor
            let mut wr = RECT {
                left: 0,
                top: 0,
                right: 0,
                bottom: 0,
            };
            GetWindowRect(hwnd, &mut wr);
            let mut work = RECT {
                left: 0,
                top: 0,
                right: 0,
                bottom: 0,
            };
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &mut work as *mut RECT as *mut c_void, 0);
            let (ww, wh) = (wr.right - wr.left, wr.bottom - wr.top);
            SetWindowPos(
                hwnd,
                std::ptr::null_mut(),
                work.left + (work.right - work.left - ww) / 2,
                work.top + (work.bottom - work.top - wh) / 2,
                0,
                0,
                SWP_NOSIZE | SWP_NOZORDER,
            );
            let play = app.btn_play;
            APP.with(|a| *a.borrow_mut() = Some(app));

            ShowWindow(hwnd, SW_SHOW);
            UpdateWindow(hwnd);
            SetFocus(play);

            let mut msg: MSG = std::mem::zeroed();
            while GetMessageW(&mut msg, std::ptr::null_mut(), 0, 0) > 0 {
                if IsDialogMessageW(hwnd, &msg) == 0 {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
            APP.with(|a| {
                let app = a.borrow_mut().take();
                app.map(|x| {
                    x.fonts.free();
                    x.played
                })
                .unwrap_or(false)
            })
        }
    }
}

/// The app icon (a zig-zag bolt on a dark rounded square), `n` x `n` RGBA.
#[cfg_attr(not(windows), allow(dead_code))]
fn icon_rgba(n: usize) -> Vec<u8> {
    let k = n as f32 / 64.0;
    let pts: Vec<(f32, f32)> = [(0.64, 0.10), (0.30, 0.52), (0.62, 0.48), (0.36, 0.92)]
        .iter()
        .map(|&(x, y)| (x * n as f32, y * n as f32))
        .collect();
    let dist = |px: f32, py: f32| -> f32 {
        pts.windows(2)
            .map(|w| {
                let ((ax, ay), (bx, by)) = (w[0], w[1]);
                let (dx, dy) = (bx - ax, by - ay);
                let t = (((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)).clamp(0.0, 1.0);
                ((px - ax - t * dx).powi(2) + (py - ay - t * dy).powi(2)).sqrt()
            })
            .fold(f32::MAX, f32::min)
    };
    let mut rgba = vec![0u8; n * n * 4];
    let (r, thick) = (12.0 * k, (5.0 * k).max(1.6));
    for y in 0..n {
        for x in 0..n {
            let (fx, fy) = (x as f32 + 0.5, y as f32 + 0.5);
            let (cx, cy) = (fx.clamp(r, n as f32 - r), fy.clamp(r, n as f32 - r));
            let inside_bg = ((fx - cx).powi(2) + (fy - cy).powi(2)).sqrt() <= r;
            let d = dist(fx, fy);
            let i = (y * n + x) * 4;
            if d < thick {
                let a = (thick - d).clamp(0.0, 1.0);
                let (r0, g0, b0) = (0x10 as f32, 0x13 as f32, 0x19 as f32);
                rgba[i] = (r0 + (0x6C as f32 - r0) * a) as u8;
                rgba[i + 1] = (g0 + (0xC0 as f32 - g0) * a) as u8;
                rgba[i + 2] = (b0 + (0xFF as f32 - b0) * a) as u8;
                rgba[i + 3] = 255;
            } else if inside_bg {
                rgba[i..i + 4].copy_from_slice(&[0x10, 0x13, 0x19, 255]);
            }
        }
    }
    rgba
}

fn main() {
    let argv: Vec<String> = std::env::args().skip(1).collect();
    let loaded = if argv.iter().any(|a| a == "--settings-only") {
        Launch::from_args(&argv)
    } else {
        Launch::load()
    };
    let launch = match loaded {
        Ok(l) => l,
        Err(e) => {
            message(&e);
            std::process::exit(1);
        }
    };
    if !launch.settings_only && argv.iter().any(|a| a == "--play" || a == "-play") {
        if let Err(e) = launch.start() {
            message(&e);
            std::process::exit(1);
        }
        return;
    }
    let settings_only = launch.settings_only;
    #[cfg(windows)]
    let played = win::run(State::new(launch));
    #[cfg(not(windows))]
    let played = {
        // No window off Windows (development only): behave like Play with the saved settings.
        let mut s = State::new(launch);
        match s.play() {
            Ok(()) => true,
            Err(e) => {
                message(&e);
                false
            }
        }
    };
    if settings_only && !played {
        std::process::exit(1);
    }
}
