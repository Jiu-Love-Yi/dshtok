# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Nothing yet.

## [1.0.0] - 2026-11-25

First release. `dshtok` reads the `dsh-cost-meter` ledger and reports token usage.

### Added

- `total` command (the default): cumulative token total as
  `input + output + cacheRead + cacheWrite`, with per-bucket counts and shares,
  reasoning shown separately as a share of output, request count, day count and date
  range, plus cost and API cost when the ledger carries a `config` block.
- `daily` command: one line per day with per-bucket counts, total and request count.
- `models` command: totals grouped by `provider:model`, merged across days.
- `sessions` command: per-session leaderboard. Sessions are merged by id, so a session
  that spans several days appears once; the title comes from any day that has one.
- `doctor` command: prints every ledger path that was probed and which one was found,
  or the full "no ledger found" explanation when none was. When a file is found it also
  reports that file's size (raw bytes and a human form) and the ledger's shape: version,
  day count, date range, session count, model count, call count, token total and currency.
- `help` command, confirming the full command list and option set.
- Options: `--path FILE` / `-p`, `--profile N`, `--since DATE`, `--until DATE`,
  `--top N`, `--json`, `--csv`, `--unicode` / `-u`, `--no-color`, `--no-header`,
  `--quiet` / `-q`, `--verbose` / `-v`, `--help` / `-h`, `--version` / `-V`.
- Ledger discovery: `--path`, then `$DSHTOK_LEDGER`, then
  `$DSH_HOME/storages/cost-meter/ledger.json`, then the default DSH home
  (`%USERPROFILE%\.dsh` on Windows, `~/.dsh` elsewhere), including the profile-scoped
  form `$DSH_HOME/profiles/<profile>/storages/cost-meter/ledger.json`, then a
  `.dsh` directory in the working directory.
- One more discovery fallback, Linux and macOS only: `/home/<USER>/.dsh/...` when `HOME`
  and `USERPROFILE` are both unset but `USER` is set — the container and WSL case where
  the shell has no `HOME` but the real home directory still holds `.dsh`. A `USER` value
  that is empty, contains `/`, or is `.`/`..` is refused, so it cannot become a path
  outside `/home`.
- `--json` output: a single object with stable key order (`tool`, `version`, `source`,
  `currency`, `range`, `days_count` for `total`, `total`, then the command's arrays),
  RFC 8259 escaping, non-ASCII text emitted as UTF-8.
- `--csv` output for `daily`, `models` and `sessions`: header row, comma separated, `\n`
  endings, no BOM, quoted text with doubled inner quotes.
- Tolerant ledger reading: missing numeric fields default to zero, integers and
  fractions are both accepted, unknown fields are ignored, so a newer plugin format does
  not break the tool.
- Informative failure mode when the ledger is absent: the message names every path that
  was tried and points at the plugin that has to produce the file.
- Exit codes: `0` success, `1` runtime or data error, `2` usage error.
- `--since` / `--until` reject dates that do not exist (`2026-02-30`, `2025-02-29`,
  `2026-13-99`) instead of silently filtering everything out and reporting zero.
- Build: `make`, `make test`, `make integration`, `make installer`, `make release`,
  `make clean` and `make install`, with `make` producing `build/dshtok`; a CMake build with
  the same targets and a CTest case per test binary; no dependencies beyond a C99 compiler,
  the standard library and `libm`.
- Releases: `.github/workflows/release.yml` runs the test suite and then builds
  `dshtok-linux-x86_64`, `dshtok-macos-universal` (arm64 and x86_64 in one file, checked
  with `lipo -verify_arch`) and `dshtok-windows-x86_64.exe` when a `v*` tag is pushed, and
  publishes the three binaries with a `SHA256SUMS` file.
- `scripts/install.sh`: POSIX one-line installer for Linux and macOS. Picks the right
  asset, verifies it against `SHA256SUMS`, installs to `$DSHTOK_INSTALL_DIR` (default
  `~/.local/bin`), and refuses to install when the checksum does not match. Exercised end
  to end by `tests/installer.sh` against a local fake release (`make installer`).
- Documentation: this changelog, `README.md`, `CONTRIBUTING.md`, `LICENSE`, and
  `docs/data-source.md` (where the numbers come from, and what can make them wrong) and
  `docs/architecture.md` (module map, data flow, memory strategy).
- `docs/demo.svg`: the output of one real `dshtok total` run, drawn as a terminal card so
  the README has something to look at before it has anything to read.

### Fixed

- Building with a strict `-std=c99` compiler. `fileno()` and `isatty()` are POSIX
  rather than ISO C, and glibc hides them under `-std=c99`, so the Linux and pedantic
  builds failed on an implicit declaration even though MinGW declares them regardless.
  `src/report.c` now asks for `_POSIX_C_SOURCE 200809L` on non-Windows builds, and the
  tests do the same for `setenv()` / `mkdir()`.
- A `-D_FORTIFY_SOURCE` warning in `format_fixed_grouped`, where two `strncat()` calls
  recomputed their own bound. Replaced with a bounds-checked append, so the group
  separator can no longer be silently dropped by the compiler's truncation guard.

### Notes

- `dshtok` is not affiliated with DeepSeek Harness or with the `dsh-cost-meter` plugin,
  and does not bundle either. It reads the ledger file that plugin writes.
- `reasoning` tokens are intentionally not added to the total, because they overlap
  `output`. See `docs/data-source.md`.
- The tool is fully offline. It reads one local file and makes no network calls.
