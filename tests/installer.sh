#!/bin/sh
# End-to-end test for scripts/install.sh.
#
# The installer hard codes GitHub as its download base, so this test points it
# at a local directory with DSHTOK_BASE_URL and exercises the whole path:
# URL construction, download, SHA256SUMS parsing, the install mode, and the two
# ways verification can fail. Nothing here touches the network.
#
# Run from the repository root after building:  sh tests/installer.sh

set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

installer=scripts/install.sh

fail() {
    printf 'installer test: %s\n' "$*" >&2
    exit 1
}

# --- preconditions ---------------------------------------------------------

[ -f "$installer" ] || fail "missing $installer"

if ! command -v sh >/dev/null 2>&1; then
    printf 'skipped: no sh\n'
    exit 0
fi

sh -n "$installer" || fail "syntax error in $installer"

if [ ! -x build/dshtok ]; then
    printf 'skipped: build the binary first (make)\n'
    exit 0
fi

if ! command -v curl >/dev/null 2>&1; then
    printf 'skipped: the installer prefers curl and this test needs it\n'
    exit 0
fi

case "$(uname -s)" in
    Linux) os_name=linux ;;
    Darwin) os_name=macos ;;
    *)
        printf 'skipped: installer targets Linux and macOS\n'
        exit 0
        ;;
esac

# Mirror the installer's own mapping, so a change to one without the other
# shows up here as a failed download rather than as a silently skipped test.
case "$(uname -m)" in
    x86_64|amd64)
        if [ "$os_name" = linux ]; then
            asset=dshtok-linux-x86_64
        else
            asset=dshtok-macos-universal
        fi
        ;;
    arm64|aarch64)
        if [ "$os_name" = macos ]; then
            asset=dshtok-macos-universal
        else
            printf 'skipped: no prebuilt binary for %s/arm64\n' "$os_name"
            exit 0
        fi
        ;;
    *)
        printf 'skipped: no asset name for this architecture\n'
        exit 0
        ;;
esac

tmp=$(mktemp -d) || fail "cannot create a temporary directory"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

rel="$tmp/release"
bin="$tmp/bin"
mkdir -p "$rel"

cp build/dshtok "$rel/$asset"
chmod 755 "$rel/$asset"

if command -v sha256sum >/dev/null 2>&1; then
    ( cd "$rel" && sha256sum "$asset" > SHA256SUMS )
elif command -v shasum >/dev/null 2>&1; then
    ( cd "$rel" && shasum -a 256 "$asset" > SHA256SUMS )
else
    printf 'skipped: no sha256sum or shasum\n'
    exit 0
fi

base="file://$rel"

# --- happy path ------------------------------------------------------------

if ! DSHTOK_BASE_URL="$base" DSHTOK_INSTALL_DIR="$bin" sh "$installer" > "$tmp/ok.txt" 2>&1; then
    cat "$tmp/ok.txt" >&2
    fail "installer failed on a good release"
fi

[ -x "$bin/dshtok" ] || fail "installed binary is missing or not executable"
"$bin/dshtok" --version > /dev/null || fail "installed binary does not run"

grep -q "sha256  ok" "$tmp/ok.txt" || {
    cat "$tmp/ok.txt" >&2
    fail "installer did not report a verified checksum"
}

# The binary that was installed must be byte for byte the one served.
if command -v cmp >/dev/null 2>&1; then
    cmp -s "$rel/$asset" "$bin/dshtok" || fail "installed binary differs from the release asset"
fi

# --- a corrupted checksum must abort the install ---------------------------

printf 'deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef  %s\n' \
    "$asset" > "$rel/SHA256SUMS"

if DSHTOK_BASE_URL="$base" DSHTOK_INSTALL_DIR="$tmp/bin-bad" sh "$installer" \
        > "$tmp/bad.txt" 2>&1; then
    cat "$tmp/bad.txt" >&2
    fail "installer accepted a bad checksum"
fi

grep -q "checksum mismatch" "$tmp/bad.txt" || {
    cat "$tmp/bad.txt" >&2
    fail "installer did not explain the checksum mismatch"
}

[ ! -f "$tmp/bin-bad/dshtok" ] || fail "a bad checksum still installed a binary"

# --- a release without SHA256SUMS still installs, with a warning -----------

rm -f "$rel/SHA256SUMS"

if ! DSHTOK_BASE_URL="$base" DSHTOK_INSTALL_DIR="$tmp/bin-nosums" sh "$installer" \
        > "$tmp/nosums.txt" 2>&1; then
    cat "$tmp/nosums.txt" >&2
    fail "installer refused a release without SHA256SUMS"
fi

[ -x "$tmp/bin-nosums/dshtok" ] || fail "no binary installed when SHA256SUMS was absent"
grep -q "no SHA256SUMS" "$tmp/nosums.txt" || {
    cat "$tmp/nosums.txt" >&2
    fail "installer did not warn about the missing SHA256SUMS"
}

# --- a missing asset must fail with a clear message ------------------------

mkdir -p "$tmp/empty"
if DSHTOK_BASE_URL="file://$tmp/empty" DSHTOK_INSTALL_DIR="$tmp/bin-missing" \
        sh "$installer" > "$tmp/missing.txt" 2>&1; then
    cat "$tmp/missing.txt" >&2
    fail "installer succeeded although the asset does not exist"
fi

printf 'ok - installer verified, installed, and rejected bad input\n'
