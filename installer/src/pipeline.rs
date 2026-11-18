//! The generation pipeline (game files -> lifted sources and assets), the patch set that turns
//! its output into the port's maintained sources, and the export of a clean source tree.

use crate::bail;
use crate::diff;
use crate::game;
use crate::hash;
use crate::lock::{Entry, Lock, State};
use crate::manifest::{Manifest, Step};
use crate::util::{self, Ctx, Result};
use std::collections::BTreeSet;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

/// Directory names never walked into or exported.
pub const SKIP_DIRS: &[&str] = &[".installer", ".git", "__pycache__"];

// ---------------------------------------------------------------------------------------------
// Placeholders
// ---------------------------------------------------------------------------------------------

#[derive(Clone, Default)]
pub struct Vars(pub Vec<(String, String)>);

impl Vars {
    pub fn set(&mut self, k: &str, v: impl Into<String>) {
        let v = v.into();
        if let Some(e) = self.0.iter_mut().find(|(n, _)| n == k) {
            e.1 = v;
        } else {
            self.0.push((k.to_string(), v));
        }
    }
    pub fn set_path(&mut self, k: &str, p: &Path) {
        self.set(k, util::path_str(p));
    }
    /// `{name}` -> value; `{{` / `}}` -> literal braces; unknown names are an error.
    pub fn expand(&self, s: &str) -> Result<String> {
        let mut out = String::new();
        let mut rest = s;
        while let Some(i) = rest.find(['{', '}']) {
            out.push_str(&rest[..i]);
            let r = &rest[i..];
            if r.starts_with("{{") || r.starts_with("}}") {
                out.push_str(&r[..1]);
                rest = &r[2..];
                continue;
            }
            if r.starts_with('}') {
                bail!("unbalanced '}}' in \"{s}\"");
            }
            let end = r
                .find('}')
                .ctx(|| format!("unterminated placeholder in \"{s}\""))?;
            let name = &r[1..end];
            let v = self
                .0
                .iter()
                .find(|(n, _)| n == name)
                .map(|(_, v)| v.as_str());
            match v {
                Some(v) => out.push_str(v),
                None => bail!("unknown placeholder {{{name}}} in \"{s}\""),
            }
            rest = &r[end + 1..];
        }
        out.push_str(rest);
        Ok(out)
    }
    pub fn expand_all(&self, v: &[String]) -> Result<Vec<String>> {
        v.iter().map(|s| self.expand(s)).collect()
    }
}

pub fn workspace_vars(m: &Manifest, root: &Path, installer_dir: &Path) -> Vars {
    let mut v = Vars::default();
    v.set_path("root", root);
    v.set_path("port", &util::join_slash(root, &m.port));
    v.set_path("toolkit", &util::join_slash(root, &m.toolkit));
    v.set_path("work", &root.join(".installer").join("work"));
    v.set_path("installer", installer_dir);
    v
}

pub fn game_vars(v: &mut Vars, g: &game::Game, elf: &Path) {
    v.set_path("game", &g.root);
    v.set_path("usrdir", &g.usrdir);
    v.set_path("elf", elf);
}

// ---------------------------------------------------------------------------------------------
// The game folder link (tools in the port find the game at ../<link_name>)
// ---------------------------------------------------------------------------------------------

pub fn link_game(ws_root: &Path, link_name: &str, game_root: &Path) -> Result<()> {
    let dst = ws_root.join(link_name);
    let target = util::absolute(game_root);
    if let Ok(md) = fs::symlink_metadata(&dst) {
        if util::absolute(&dst) == target {
            return Ok(());
        }
        if md.file_type().is_symlink() {
            remove_link(&dst)?;
        } else {
            bail!(
                "{} already exists and is not your game folder ({}).\n    Move it away or point --game at it.",
                dst.display(),
                target.display()
            );
        }
    }
    util::info(&format!(
        "Linking {} -> {}",
        dst.display(),
        target.display()
    ));
    #[cfg(windows)]
    {
        let mut c = Command::new("cmd");
        c.arg("/c").arg("mklink").arg("/J").arg(&dst).arg(&target);
        c.stdout(std::process::Stdio::null());
        util::run(&mut c).ctx(|| "creating the game folder junction")?;
    }
    #[cfg(unix)]
    std::os::unix::fs::symlink(&target, &dst).ctx(|| format!("linking {}", dst.display()))?;
    Ok(())
}

pub fn remove_link(p: &Path) -> Result<()> {
    // A junction/dir symlink is removed with remove_dir on Windows, remove_file elsewhere;
    // neither touches the target's contents.
    if fs::remove_dir(p).is_err() {
        fs::remove_file(p).ctx(|| format!("removing link {}", p.display()))?;
    }
    Ok(())
}

// ---------------------------------------------------------------------------------------------
// Steps
// ---------------------------------------------------------------------------------------------

fn run_builtin(name: &str, args: &[String]) -> Result<()> {
    match name {
        "extract-jobmods" => {
            let [elf, out] = args else {
                bail!("extract-jobmods takes [elf, out_dir]")
            };
            let names = game::extract_jobmods(Path::new(elf), Path::new(out))?;
            util::info(&format!("{} WWS job modules -> {out}", names.len()));
            Ok(())
        }
        "carve" => {
            let [src, off, len, dst] = args else {
                bail!("carve takes [src, offset, length, dst]")
            };
            let num = |s: &str| -> Result<u64> {
                let r = match s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")) {
                    Some(h) => u64::from_str_radix(h, 16),
                    None => s.parse(),
                };
                r.ctx(|| format!("bad number '{s}'"))
            };
            game::carve(Path::new(src), num(off)?, num(len)?, Path::new(dst))
        }
        "copy" => {
            let [src, dst] = args else {
                bail!("copy takes [src, dst]")
            };
            util::copy_file(Path::new(src), Path::new(dst))
        }
        _ => bail!("unknown builtin '{name}' (known: extract-jobmods, carve, copy)"),
    }
}

fn run_one(step: &Step, vars: &Vars, root: &Path) -> Result<()> {
    for d in &step.mkdirs {
        let d = vars.expand(d)?;
        fs::create_dir_all(&d).ctx(|| format!("creating {d}"))?;
    }
    if let Some(b) = &step.builtin {
        return run_builtin(b, &vars.expand_all(&step.args)?);
    }
    let argv = vars.expand_all(&step.run)?;
    let mut cmd = Command::new(&argv[0]);
    cmd.args(&argv[1..]);
    cmd.current_dir(match &step.cwd {
        Some(c) => PathBuf::from(vars.expand(c)?),
        None => root.to_path_buf(),
    });
    // Python tools print non-ASCII; a cp1252 console must not turn that into a crash.
    cmd.env("PYTHONUTF8", "1")
        .env("PYTHONIOENCODING", "utf-8")
        .env("PYTHONDONTWRITEBYTECODE", "1");
    if !cfg!(windows) {
        let shim = winlike_dir(root);
        let path = match std::env::var_os("PYTHONPATH").filter(|p| !p.is_empty()) {
            Some(p) => {
                let mut v = vec![shim];
                v.extend(std::env::split_paths(&p));
                std::env::join_paths(v).ctx(|| "PYTHONPATH")?
            }
            None => shim.into_os_string(),
        };
        cmd.env("PYTHONPATH", path);
    }
    for (k, v) in &step.env {
        cmd.env(k, vars.expand(v)?);
    }
    util::run(&mut cmd)
}

/// The folder whose sitecustomize.py makes the Python tools write files as they do on Windows.
fn winlike_dir(root: &Path) -> PathBuf {
    root.join(".installer").join("winlike")
}

/// Run the manifest's steps in `root`. Steps already done for this EBOOT are skipped until one
/// has to run; from then on everything after it runs too. Returns the optional steps that failed.
pub fn run_steps(
    m: &Manifest,
    root: &Path,
    vars: &Vars,
    eboot_sha: &str,
    regen: bool,
) -> Result<Vec<String>> {
    let state_path = root.join(".installer").join("state");
    let mut st = State::load(&state_path);
    if regen || st.eboot.as_deref() != Some(eboot_sha) {
        st = State {
            eboot: Some(eboot_sha.to_string()),
            ..Default::default()
        };
        st.save(&state_path)?;
    }
    fs::create_dir_all(root.join(".installer").join("work"))?;
    if !cfg!(windows) {
        util::write_file(
            &winlike_dir(root).join("sitecustomize.py"),
            include_bytes!("winlike/sitecustomize.py"),
        )?;
    }
    let mut failed = Vec::new();
    let mut rerun_rest = false;
    let n = m.steps.len();
    for (i, step) in m.steps.iter().enumerate() {
        if !rerun_rest && st.done.contains(&step.id) {
            util::info(&format!("[{}/{n}] {} (already done)", i + 1, step.title));
            continue;
        }
        rerun_rest = true;
        st.applied = false;
        util::heading(&format!("[{}/{n}] {}", i + 1, step.title));
        if let Some(note) = &step.note {
            util::info(note);
        }
        let res = match &step.for_each {
            None => run_one(step, vars, root),
            Some(pat) => {
                let pat = vars.expand(pat)?;
                let rel = util::rel_slash(root, Path::new(&pat)).unwrap_or(pat.replace('\\', "/"));
                let files = util::select_files(root, std::slice::from_ref(&rel), &[], SKIP_DIRS)?;
                if files.is_empty() {
                    Err(util::Error(format!("no files match {rel}")))
                } else {
                    let mut r = Ok(());
                    for f in files {
                        let mut v = vars.clone();
                        v.set_path("file", &util::join_slash(root, &f));
                        if let Err(e) = run_one(step, &v, root) {
                            r = Err(e);
                            break;
                        }
                    }
                    r
                }
            }
        };
        match res {
            Ok(()) => {
                st.done.retain(|d| d != &step.id);
                st.done.push(step.id.clone());
                st.save(&state_path)?;
            }
            Err(e) if step.optional => {
                util::warn(&format!("optional step '{}' failed: {e}", step.id));
                failed.push(step.id.clone());
            }
            Err(e) => bail!("step '{}' failed: {e}", step.id),
        }
    }
    Ok(failed)
}

// ---------------------------------------------------------------------------------------------
// Applying the patch set (install)
// ---------------------------------------------------------------------------------------------

/// True when every file the lock describes is already in its final state (the patch set was
/// made from this tree, or a previous install finished). Loose files are not in the lock.
pub fn lock_satisfied(root: &Path, lock: &Lock) -> Result<bool> {
    if lock.files.is_empty() {
        return Ok(false);
    }
    for e in &lock.files {
        let p = util::join_slash(root, &e.path);
        let ok = match &e.fin {
            None => !p.exists(),
            Some(fin) => p.is_file() && hash::sha256_file(&p)? == *fin,
        };
        if !ok {
            return Ok(false);
        }
    }
    Ok(true)
}

/// A tool that writes a text file without choosing its line endings gives CRLF on Windows (where
/// the lock was made) and LF elsewhere. If the other line ending gives exactly `want`, rewrite the
/// file that way and return true.
fn eol_to_hash(p: &Path, want: &str) -> Result<bool> {
    let d = fs::read(p)?;
    if diff::is_binary(&d) {
        return Ok(false);
    }
    let lf: Vec<u8> = {
        let mut v = Vec::with_capacity(d.len());
        let mut i = 0;
        while i < d.len() {
            if d[i] == b'\r' && d.get(i + 1) == Some(&b'\n') {
                i += 1;
            }
            v.push(d[i]);
            i += 1;
        }
        v
    };
    let mut crlf = Vec::with_capacity(lf.len() + lf.len() / 16);
    for &b in &lf {
        if b == b'\n' {
            crlf.push(b'\r');
        }
        crlf.push(b);
    }
    for v in [crlf, lf] {
        if v != d && hash::sha256_hex(&v) == want {
            util::write_file(p, &v)?;
            return Ok(true);
        }
    }
    Ok(false)
}

pub fn patch_path(installer_dir: &Path, rel: &str) -> PathBuf {
    util::join_slash(&installer_dir.join("patches"), &format!("{rel}.patch"))
}

/// `tolerate`: globs of outputs of optional steps that failed; those files may be missing.
pub fn apply_lock(
    m: &Manifest,
    root: &Path,
    installer_dir: &Path,
    lock: &Lock,
    tolerate: &[String],
) -> Result<()> {
    let present: BTreeSet<String> =
        util::select_files(root, &m.gen_include, &m.gen_exclude, SKIP_DIRS)?
            .into_iter()
            .collect();
    let known: BTreeSet<&str> = lock.files.iter().map(|e| e.path.as_str()).collect();
    let mut problems: Vec<String> = Vec::new();
    let (mut patched, mut created, mut removed, mut same) = (0, 0, 0, 0);
    for e in &lock.files {
        let p = util::join_slash(root, &e.path);
        let mut cur = if p.is_file() {
            Some(hash::sha256_file(&p)?)
        } else {
            None
        };
        match (&e.base, &e.fin) {
            (_, None) => {
                if cur.is_some() {
                    fs::remove_file(&p).ctx(|| format!("removing {}", p.display()))?;
                    removed += 1;
                }
            }
            (base, Some(fin)) => {
                if cur.as_deref() == Some(fin.as_str()) {
                    same += 1;
                    continue; // already final (identical output, or patched earlier)
                }
                if cur.is_none() && base.is_some() && util::any_match(tolerate, &e.path) {
                    util::warn(&format!("skipping {} (its optional step failed)", e.path));
                    continue;
                }
                if let (Some(c), Some(b)) = (&cur, base) {
                    if c != b && eol_to_hash(&p, b)? {
                        cur = Some(b.clone());
                    }
                }
                if cur != *base {
                    problems.push(match (&cur, base) {
                        (None, _) => format!("missing      {}", e.path),
                        (Some(_), None) => format!("unexpected   {}", e.path),
                        (Some(_), Some(_)) => format!("differs      {}", e.path),
                    });
                    continue;
                }
                let old = if base.is_some() {
                    fs::read(&p)?
                } else {
                    Vec::new()
                };
                let pp = patch_path(installer_dir, &e.path);
                let patch = fs::read(&pp).ctx(|| format!("reading {}", pp.display()))?;
                let new = diff::apply_patch(&old, &patch)
                    .map_err(|x| util::Error(format!("{}: {x}", pp.display())))?;
                if hash::sha256_hex(&new) != *fin {
                    bail!(
                        "{} applied but the result has the wrong hash -- the patch set is damaged",
                        pp.display()
                    );
                }
                util::write_file(&p, &new)?;
                if base.is_some() {
                    patched += 1
                } else {
                    created += 1
                }
            }
        }
    }
    let extra: Vec<&String> = present
        .iter()
        .filter(|p| !known.contains(p.as_str()) && !m.is_loose(p))
        .collect();
    for x in &extra {
        let _ = fs::remove_file(util::join_slash(root, x));
    }
    if !problems.is_empty() {
        let shown: Vec<String> = problems.iter().take(25).cloned().collect();
        bail!(
            "{} generated file(s) do not match what the port's patches were made from:\n      {}{}\n\n    \
             This usually means a different game version/region, or different Python package versions\n    \
             (delete the .installer folder and run again to start clean).",
            problems.len(),
            shown.join("\n      "),
            if problems.len() > 25 { "\n      ..." } else { "" }
        );
    }
    util::info(&format!(
        "{same} file(s) already right, {patched} patched, {created} created, {removed} removed{}",
        if extra.is_empty() {
            String::new()
        } else {
            format!(", {} stray removed", extra.len())
        }
    ));
    Ok(())
}

// ---------------------------------------------------------------------------------------------
// Building the patch set (maintainer)
// ---------------------------------------------------------------------------------------------

pub struct Finding {
    pub path: String,
    pub why: String,
}

/// Compare the fresh pipeline output in `fresh` with the maintained files in `root`, write
/// patches + lock into `installer_dir`. Returns files that must not be published as patches
/// (binary differences, or patches that are mostly the generated content itself).
pub fn build_patch_set(
    m: &Manifest,
    fresh: &Path,
    root: &Path,
    installer_dir: &Path,
    eboot_sha: &str,
) -> Result<Vec<Finding>> {
    let mut base_files = util::select_files(fresh, &m.gen_include, &m.gen_exclude, SKIP_DIRS)?;
    let mut fin_files = util::select_files(root, &m.gen_include, &m.gen_exclude, SKIP_DIRS)?;
    // Machine-dependent assets: not part of the patch set; just say how they compare.
    for rel in fin_files.iter().filter(|f| m.is_loose(f)) {
        let fresh_p = util::join_slash(fresh, rel);
        let note = if !fresh_p.is_file() {
            "the pipeline did not produce it (its optional step failed?)"
        } else if hash::sha256_file(&fresh_p)? == hash::sha256_file(&util::join_slash(root, rel))? {
            "identical"
        } else {
            "differs (expected: drawn with this machine's fonts)"
        };
        util::info(&format!("loose {rel}: {note}"));
    }
    base_files.retain(|f| !m.is_loose(f));
    fin_files.retain(|f| !m.is_loose(f));
    let all: BTreeSet<&String> = base_files.iter().chain(fin_files.iter()).collect();
    let bset: BTreeSet<&String> = base_files.iter().collect();
    let fset: BTreeSet<&String> = fin_files.iter().collect();

    let pdir = installer_dir.join("patches");
    if pdir.exists() {
        fs::remove_dir_all(&pdir).ctx(|| format!("clearing {}", pdir.display()))?;
    }
    let mut lock = Lock {
        eboot: Some(eboot_sha.to_string()),
        files: Vec::new(),
    };
    let mut findings = Vec::new();
    let (mut same, mut patched, mut created, mut removed) = (0, 0, 0, 0);
    let (mut patch_bytes, mut gen_bytes) = (0u64, 0u64);
    for rel in all {
        let base = if bset.contains(rel) {
            Some(fs::read(util::join_slash(fresh, rel))?)
        } else {
            None
        };
        let fin = if fset.contains(rel) {
            Some(fs::read(util::join_slash(root, rel))?)
        } else {
            None
        };
        let bh = base.as_ref().map(|d| hash::sha256_hex(d));
        let fh = fin.as_ref().map(|d| hash::sha256_hex(d));
        gen_bytes += fin.as_ref().map(|d| d.len() as u64).unwrap_or(0);
        lock.files.push(Entry {
            path: rel.clone(),
            base: bh.clone(),
            fin: fh.clone(),
        });
        let Some(fin) = fin else {
            removed += 1;
            continue;
        };
        if bh == fh {
            same += 1;
            continue;
        }
        let old = base.as_deref().unwrap_or(&[]);
        if diff::is_binary(old) || diff::is_binary(&fin) {
            findings.push(Finding {
                path: rel.clone(),
                why: if base.is_none() {
                    "binary file the pipeline does not produce -- add a step that generates it".into()
                } else {
                    "binary file differs from the pipeline's output -- fix the step that generates it".into()
                },
            });
            continue;
        }
        util::info(&format!(
            "diffing {rel} ({})",
            util::human_bytes(fin.len() as u64)
        ));
        let (patch, st) = diff::make_patch(old, &fin, rel, 3);
        // Self-check before anything is written.
        match diff::apply_patch(old, &patch) {
            Ok(r) if r == fin => {}
            _ => bail!("internal error: the patch for {rel} does not reproduce the file"),
        }
        let pp = patch_path(installer_dir, rel);
        util::write_file(&pp, &patch)?;
        patch_bytes += patch.len() as u64;
        if base.is_none() {
            created += 1;
        } else {
            patched += 1;
        }
        let ratio = st.added_bytes as f64 / fin.len().max(1) as f64;
        if !util::any_match(&m.full_patch_ok, rel)
            && fin.len() > 4096
            && (base.is_none() || ratio > 0.5)
        {
            findings.push(Finding {
                path: rel.clone(),
                why: format!(
                    "patch carries {:.0}% of the file ({} added) -- likely generated content; add/fix a step, or list it in generated.full_patch_ok if it is hand-written",
                    ratio * 100.0,
                    util::human_bytes(st.added_bytes as u64)
                ),
            });
        }
    }
    lock.save(&installer_dir.join("generated.lock"))?;
    util::heading("Patch set written");
    util::info(&format!(
        "{same} identical, {patched} patched, {created} created by patch, {removed} removed"
    ));
    util::info(&format!(
        "patches: {} for {} of generated files ({}/patches)",
        util::human_bytes(patch_bytes),
        util::human_bytes(gen_bytes),
        installer_dir.display()
    ));
    Ok(findings)
}

// ---------------------------------------------------------------------------------------------
// Export: the tree that goes on GitHub -- the port, the toolkit, the installer, and nothing that
// came out of the game.
// ---------------------------------------------------------------------------------------------

pub fn export_files(m: &Manifest, root: &Path, skip_extra: &[String]) -> Result<Vec<String>> {
    let dir_pruned = |dir: &str| -> bool {
        let probe = format!("{dir}/\u{1}");
        let excl = m
            .export_exclude
            .iter()
            .any(|p| (p.ends_with('/') || p.ends_with("/**")) && util::glob_match(p, &probe));
        let kept = m.export_keep.iter().any(|p| util::may_match_below(p, dir));
        let skip = skip_extra.iter().any(|s| {
            dir.eq_ignore_ascii_case(s)
                || dir
                    .to_ascii_lowercase()
                    .starts_with(&format!("{}/", s.to_ascii_lowercase()))
        });
        skip || (excl && !kept)
    };
    let files = util::walk_files(root, SKIP_DIRS, &|d| !dir_pruned(d))?;
    let files: Vec<String> = files
        .into_iter()
        .filter(|f| {
            if m.is_generated(f) {
                return false;
            }
            util::any_match(&m.export_keep, f) || !util::any_match(&m.export_exclude, f)
        })
        .collect();
    let bad = forbidden(m, &files);
    if !bad.is_empty() {
        crate::bail!(
            "the export would include {} file(s) listed in [export] forbid (private, never published):\n  {}\nAdd them to [export] exclude.",
            bad.len(),
            bad.join("\n  ")
        );
    }
    Ok(files)
}

/// The files of `files` that `[export] forbid` names.
pub fn forbidden(m: &Manifest, files: &[String]) -> Vec<String> {
    files
        .iter()
        .filter(|f| util::any_match(&m.export_forbid, f))
        .cloned()
        .collect()
}

pub fn export_tree(m: &Manifest, root: &Path, out: &Path) -> Result<(usize, u64)> {
    let mut skip = Vec::new();
    if let Some(l) = &m.link_name {
        skip.push(l.clone());
    }
    if let Some(r) = util::rel_slash(&util::absolute(root), &util::absolute(out)) {
        skip.push(r);
    }
    let files = export_files(m, root, &skip)?;
    let mut bytes = 0;
    for f in &files {
        let src = util::join_slash(root, f);
        let dst = util::join_slash(out, f);
        util::copy_file(&src, &dst)?;
        bytes += fs::metadata(&dst).map(|x| x.len()).unwrap_or(0);
    }
    Ok((files.len(), bytes))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn eol_to_hash_matches_only_the_recorded_bytes() {
        let dir = std::env::temp_dir().join(format!("infamous-eol-{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        let p = dir.join("ppu_recomp.h");
        fs::write(&p, b"#pragma once\n#include <stdint.h>\n").unwrap();
        let crlf = b"#pragma once\r\n#include <stdint.h>\r\n";
        // Unrelated content: left alone.
        assert!(!eol_to_hash(&p, &hash::sha256_hex(b"other\r\n")).unwrap());
        assert_eq!(
            fs::read(&p).unwrap(),
            b"#pragma once\n#include <stdint.h>\n"
        );
        // LF output, CRLF recorded: converted.
        assert!(eol_to_hash(&p, &hash::sha256_hex(crlf)).unwrap());
        assert_eq!(fs::read(&p).unwrap(), crlf);
        // And back.
        assert!(eol_to_hash(
            &p,
            &hash::sha256_hex(b"#pragma once\n#include <stdint.h>\n")
        )
        .unwrap());
        fs::remove_dir_all(&dir).unwrap();
    }
}
