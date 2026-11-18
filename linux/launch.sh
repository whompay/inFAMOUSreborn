#!/bin/sh
# inFamous Linux setup launcher. No sudo, no package-manager changes, no shell-profile edits:
# uses the system's Python 3.8+ (with Tk for the window) when there is one, else downloads a
# private Python into the chosen folder.
set -eu
in_here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
in_error() {
    printf '%s\n' "$1" >&2
    if [ -z "${in_cli:-}" ] && command -v zenity >/dev/null 2>&1; then
        zenity --error --title='inFamous Setup' --text="$1" || :
    elif [ -z "${in_cli:-}" ] && command -v kdialog >/dev/null 2>&1; then
        kdialog --error "$1" --title 'inFamous Setup' || :
    fi
    exit 1
}
case "$(uname -s)/$(uname -m)" in
    Linux/x86_64|Linux/amd64) ;;
    *) in_error 'inFamous needs x86-64 Linux (the recompiled game is x86-64 code).' ;;
esac
in_cli=
for in_arg in "$@"; do
    case "$in_arg" in --cli|--play|--add-to-steam|--doctor|--help|-h) in_cli=1 ;; esac
done
in_python_ok() {
    "$1" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)' >/dev/null 2>&1
}
# The system's Python first; then, for the copy an install keeps in <folder>/.linux/setup (menu
# entries), the private Pythons next to it.
for in_py in python3 python3.13 python3.12 python3.11 python3.10 python3.9 python3.8 \
        "$in_here/../ui/bin/python3" "$in_here/../env/bin/python3"; do
    if command -v "$in_py" >/dev/null 2>&1 && in_python_ok "$in_py"; then
        if [ -n "$in_cli" ] || "$in_py" -c 'import tkinter' >/dev/null 2>&1; then
            exec "$in_py" "$in_here/setup.py" "$@"
        fi
    fi
done

# No usable desktop Python: download a private one (Python + Tk from conda-forge). Ask for the
# folder first so it lands on the drive the game goes to, not a small system disk.
in_dest=''
if command -v zenity >/dev/null 2>&1; then
    in_dest=$(zenity --file-selection --directory --title='Choose the folder for inFamous (about 40 GB)') || exit 0
elif command -v kdialog >/dev/null 2>&1; then
    in_dest=$(kdialog --getexistingdirectory "$HOME" --title 'Choose the folder for inFamous (about 40 GB)') || exit 0
elif [ -t 0 ]; then
    printf 'Folder for inFamous (full path, about 40 GB free): '
    read -r in_dest
else
    in_error 'Run this file in a terminal: there is no Python with Tk and no folder dialog (zenity/kdialog) on this system.'
fi
case "$in_dest" in /*) ;; *) in_error 'Choose a full folder path (starting with /).' ;; esac
case "$(basename -- "$in_dest")" in inFamous) ;; *) in_dest="$in_dest/inFamous" ;; esac
in_state="$in_dest/.linux"
mkdir -p "$in_state"
in_base="/tmp/infamous-setup-$(id -u)"
if [ ! -e "$in_base" ] && [ ! -L "$in_base" ]; then mkdir -m 700 "$in_base"; fi
[ -d "$in_base" ] && [ ! -L "$in_base" ] && [ "$(stat -c '%u:%a' "$in_base")" = "$(id -u):700" ] ||
    in_error "The temporary folder $in_base is not private to your user."
in_hash=$(printf '%s' "$in_state" | sha256sum)
in_alias="$in_base/$(printf '%.20s' "${in_hash%% *}")"
if [ -L "$in_alias" ]; then
    [ "$(readlink "$in_alias")" = "$in_state" ] || in_error 'The temporary setup alias points elsewhere.'
else
    ln -s "$in_state" "$in_alias"
fi
in_mamba="$in_alias/micromamba"
in_sha=366cd9cd8be14df1ab8ed50352a82111082a36686b2d389fdb79a92c3fafb3e3
in_url=https://github.com/mamba-org/micromamba-releases/releases/download/2.9.0-0/micromamba-linux-64
in_task() {
    in_note=$1
    shift
    printf '%s\n' "$in_note"
    "$@" >"$in_state/bootstrap.log" 2>&1 &
    in_pid=$!
    if command -v zenity >/dev/null 2>&1; then
        (
            printf '#%s\n' "$in_note"
            while kill -0 "$in_pid" 2>/dev/null; do sleep 1; done
            printf '100\n'
        ) | zenity --progress --pulsate --auto-close --no-cancel --title='inFamous Setup' --text="$in_note" || :
    fi
    wait "$in_pid"
}
if [ ! -x "$in_mamba" ] || [ "$(sha256sum "$in_mamba" | cut -d' ' -f1)" != "$in_sha" ]; then
    if command -v curl >/dev/null 2>&1; then
        in_task 'Downloading the setup package manager…' curl --fail --location --proto '=https' --tlsv1.2 \
            --retry 2 --connect-timeout 15 --output "$in_mamba.partial" "$in_url" ||
            in_error 'Download failed. Check the network connection and open the setup again.'
    elif command -v wget >/dev/null 2>&1; then
        in_task 'Downloading the setup package manager…' wget --https-only --tries=3 --timeout=30 \
            -O "$in_mamba.partial" "$in_url" ||
            in_error 'Download failed. Check the network connection and open the setup again.'
    else
        in_error 'curl or wget is needed to download Python on this system.'
    fi
    [ "$(sha256sum "$in_mamba.partial" | cut -d' ' -f1)" = "$in_sha" ] ||
        in_error 'The downloaded package manager failed its SHA-256 check and was not run.'
    chmod 755 "$in_mamba.partial"
    mv "$in_mamba.partial" "$in_mamba"
fi
if [ ! -x "$in_alias/ui/bin/python3" ]; then
    in_task 'Downloading Python for the setup window…' "$in_mamba" --no-rc --root-prefix "$in_alias/mamba-pkgs" \
        create --yes --prefix "$in_alias/ui" --channel conda-forge --strict-channel-priority \
        python=3.12 tk ca-certificates ||
        in_error "Downloading Python failed; see $in_state/bootstrap.log."
fi
SSL_CERT_FILE="$in_alias/ui/ssl/cacert.pem" exec "$in_alias/ui/bin/python3" "$in_here/setup.py" --dir "$in_dest" "$@"
