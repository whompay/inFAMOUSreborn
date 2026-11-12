#!/bin/sh
# Makes inFamous-Linux-Setup.run: a small shell header plus a tar.gz of this folder's setup files.
#   linux/package.sh [--repo OWNER/NAME] [--branch NAME] [--out FILE]
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo='whompay/inFAMOUSreborn'
branch='main'
out="$here/../dist/inFamous-Linux-Setup.run"
while [ $# -gt 0 ]; do
    case "$1" in
        --repo) repo=$2; shift 2 ;;
        --branch) branch=$2; shift 2 ;;
        --out) out=$2; shift 2 ;;
        *) printf 'unknown argument %s\n' "$1" >&2; exit 2 ;;
    esac
done
case "$repo" in
    */*) ;;
    *) printf 'pass --repo OWNER/NAME (the GitHub repository players install and update from)\n' >&2; exit 2 ;;
esac
case "$repo$branch" in *[!A-Za-z0-9._/-]*) printf 'bad --repo or --branch\n' >&2; exit 2 ;; esac
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT HUP INT TERM
mkdir "$work/payload"
cp "$here/launch.sh" "$here/README.md" "$work/payload/"
sed -e "s|^DEFAULT_REPO = .*|DEFAULT_REPO = '$repo'|" \
    -e "s|^DEFAULT_BRANCH = .*|DEFAULT_BRANCH = '$branch'|" "$here/setup.py" >"$work/payload/setup.py"
grep -q "^DEFAULT_REPO = '$repo'" "$work/payload/setup.py"
chmod 755 "$work/payload/launch.sh" "$work/payload/setup.py"
mkdir -p "$(dirname -- "$out")"
cat >"$out" <<'EOF'
#!/bin/sh
# inFamous Linux Setup. Unpacks itself to a temporary folder and starts linux/launch.sh.
set -eu
in_tmp=$(mktemp -d "${TMPDIR:-/tmp}/infamous-setup.XXXXXXXX")
trap 'rm -rf -- "$in_tmp"' EXIT HUP INT TERM
in_line=$(awk '/^__INFAMOUS_PAYLOAD__$/ { print NR + 1; exit }' "$0")
[ -n "$in_line" ] || { printf 'The setup file is damaged (no payload).\n' >&2; exit 1; }
tail -n +"$in_line" "$0" | tar -xz -C "$in_tmp" || { printf 'The setup file is damaged. Download it again.\n' >&2; exit 1; }
sh "$in_tmp/launch.sh" "$@"
exit $?
__INFAMOUS_PAYLOAD__
EOF
tar -C "$work/payload" --owner=0 --group=0 --numeric-owner -czf - . >>"$out"
chmod 755 "$out"
printf '%s (%s bytes) for %s, branch %s\n' "$out" "$(wc -c <"$out")" "$repo" "$branch"
