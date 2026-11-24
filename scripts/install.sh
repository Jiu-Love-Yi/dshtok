#!/bin/sh
# dshtok installer for Linux and macOS.
#
# Downloads the prebuilt binary for this machine from the project's GitHub
# Releases, checks it against the published SHA256SUMS, and installs it as
# `dshtok` in ${DSHTOK_INSTALL_DIR:-$HOME/.local/bin}.
#
#   curl -fsSL https://raw.githubusercontent.com/Jiu-Love-Yi/dshtok/HEAD/scripts/install.sh | sh
#
# Environment variables:
#   DSHTOK_INSTALL_DIR  directory to install into  (default: $HOME/.local/bin)
#   DSHTOK_VERSION      release tag to install     (default: latest)
#   DSHTOK_OWNER        GitHub owner               (default: Jiu-Love-Yi)
#   DSHTOK_REPO         GitHub repository name     (default: dshtok)
#   DSHTOK_BASE_URL     override the download base (default: the GitHub
#                       Releases URL; used by the test suite to point the
#                       installer at a local directory)
#
# Windows is not handled here: download dshtok-windows-x86_64.exe from the
# Releases page and put it anywhere on %PATH%. It is a single self-contained
# binary with no runtime dependencies.
#
# The script never needs root -- it installs under your home directory unless
# DSHTOK_INSTALL_DIR points somewhere else.

set -eu

OWNER="${DSHTOK_OWNER:-Jiu-Love-Yi}"
REPO="${DSHTOK_REPO:-dshtok}"
VERSION="${DSHTOK_VERSION:-latest}"

say() {
    printf '%s\n' "$*"
}

die() {
    printf 'install.sh: %s\n' "$*" >&2
    exit 1
}

# ---------------------------------------------------------------------------
# which release asset is this machine
# ---------------------------------------------------------------------------

os=$(uname -s 2>/dev/null || echo unknown)
arch=$(uname -m 2>/dev/null || echo unknown)

case "$os" in
    Linux)  os_name=linux ;;
    Darwin) os_name=macos ;;
    *)
        die "unsupported operating system '$os'. Build from source instead: make"
        ;;
esac

case "$arch" in
    x86_64|amd64)
        case "$os_name" in
            linux) asset=dshtok-linux-x86_64 ;;
            macos) asset=dshtok-macos-universal ;;
        esac
        ;;
    arm64|aarch64)
        case "$os_name" in
            # The macOS release is a universal binary, so one asset covers both
            # Apple silicon and Intel.
            macos) asset=dshtok-macos-universal ;;
            *)
                die "no prebuilt binary for $os_name/$arch yet. Build from source: make"
                ;;
        esac
        ;;
    *)
        die "unsupported architecture '$arch' on $os_name. Build from source: make"
        ;;
esac

if [ -n "${DSHTOK_INSTALL_DIR:-}" ]; then
    install_dir=$DSHTOK_INSTALL_DIR
elif [ -n "${HOME:-}" ]; then
    install_dir="$HOME/.local/bin"
else
    die "HOME is not set. Set DSHTOK_INSTALL_DIR to choose where to install"
fi

if [ -n "${DSHTOK_BASE_URL:-}" ]; then
    base=$DSHTOK_BASE_URL
elif [ "$VERSION" = latest ]; then
    base="https://github.com/$OWNER/$REPO/releases/latest/download"
else
    base="https://github.com/$OWNER/$REPO/releases/download/$VERSION"
fi

# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

fetch() {
    # $1 = url, $2 = destination file. curl is preferred because -f turns a
    # 404 into an error instead of saving an HTML error page as the binary.
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$1" -o "$2"
    elif command -v wget >/dev/null 2>&1; then
        wget -qO "$2" "$1"
    else
        die "need curl or wget to download the release"
    fi
}

sha256_of() {
    # Prints the lowercase hex digest of $1, or fails when no tool exists.
    if command -v sha256sum >/dev/null 2>&1; then
        set -- "$(sha256sum "$1")"
    elif command -v shasum >/dev/null 2>&1; then
        set -- "$(shasum -a 256 "$1")"
    else
        return 1
    fi
    printf '%s\n' "${1%% *}"
}

tmp=$(mktemp -d 2>/dev/null) || die "cannot create a temporary directory"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

binary="$tmp/$asset"
url="$base/$asset"

say "dshtok installer"
say "  os      $os_name/$arch"
say "  asset   $asset"
say "  from    $url"

if ! fetch "$url" "$binary"; then
    die "download failed. Check that the release exists, or build from source: make"
fi

# ---------------------------------------------------------------------------
# checksum
# ---------------------------------------------------------------------------

if got=$(sha256_of "$binary" 2>/dev/null); then
    if fetch "$base/SHA256SUMS" "$tmp/SHA256SUMS" 2>/dev/null; then
        want=$(awk -v a="$asset" '
            {
                name = $2
                sub(/^\*/, "", name)
                if (name == a) {
                    print $1
                    exit
                }
            }' "$tmp/SHA256SUMS")
        if [ -z "$want" ]; then
            say "warning: $asset is not listed in SHA256SUMS; skipping verification"
        elif [ "$got" = "$want" ]; then
            say "  sha256  ok"
        else
            die "checksum mismatch for $asset: expected $want, got $got"
        fi
    else
        say "warning: this release publishes no SHA256SUMS; skipping verification"
    fi
else
    say "warning: no sha256sum or shasum here; skipping verification"
fi

# ---------------------------------------------------------------------------
# install
# ---------------------------------------------------------------------------

mkdir -p "$install_dir" || die "cannot create $install_dir"
cp "$binary" "$install_dir/dshtok" || die "cannot write $install_dir/dshtok"
chmod 755 "$install_dir/dshtok"

if "$install_dir/dshtok" --version >/dev/null 2>&1; then
    say "  version $("$install_dir/dshtok" --version)"
fi

say "installed $install_dir/dshtok"

case ":${PATH:-}:" in
    *":$install_dir:"*)
        ;;
    *)
        say ""
        say "$install_dir is not on your PATH. Add it with:"
        say ""
        say "  export PATH=\"$install_dir:\$PATH\""
        ;;
esac

say ""
say "Next: run 'dshtok' for your total, or 'dshtok doctor' to see which ledger was used."
