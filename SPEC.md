# dshtok — Implementation Contract (v1.0.0)

`dshtok` reports **how many tokens you have burned in DeepSeek Harness, in total**.
It is a zero-dependency C99 command line tool that reads the ledger written by the
`dsh-cost-meter` plugin.

This file is the frozen interface contract. It describes what `dshtok` promises, not how
it is organised internally. If a change needs a different contract, that is a deliberate
decision to be made here first — update this file and the headers in the same commit as
the code, or not at all.

## Target platforms

Windows (MinGW-w64 / MSVC) and Linux/macOS (gcc/clang). C99. Standard library only.
No VLAs. No compiler-specific extensions except the `_WIN32` / `__unix__` guards.
Build must be warning-clean at `-Wall -Wextra -Wpedantic`.

## Binary

`dshtok` / `dshtok.exe`.

```
dshtok [OPTIONS] [COMMAND]

Commands
  total      cumulative totals (default)
  daily      one line per day
  models     totals grouped by provider:model
  sessions   per-session leaderboard
  doctor     show where the ledger was looked for and what was found
  help       show help

Options
  -p, --path FILE      read this ledger.json instead of auto-detecting
      --profile NAME   also probe profiles/NAME when auto-detecting
      --since DATE     include days >= DATE (YYYY-MM-DD)
      --until DATE     include days <= DATE (YYYY-MM-DD)
      --top N          rows shown by `sessions` / `models` (default: all)
      --json           machine readable output for the chosen command
      --csv            CSV output for `daily` / `models` / `sessions`
  -u, --unicode        allow UTF-8 box drawing, currency symbol and titles
      --no-color       disable ANSI colour
      --color          force ANSI colour even when stdout is not a terminal
      --no-header      omit the title line (useful in scripts)
  -q, --quiet          print only the headline token number
  -v, --verbose        include the model breakdown in `total`
  -h, --help
  -V, --version
```

Exit codes: `0` success, `1` runtime error (ledger missing/unreadable/malformed),
`2` usage error (unknown option or command, or a bad option *value* such as a malformed
`--since`). Option validation happens before the ledger is opened, so a usage mistake
never depends on whether the ledger happens to be readable.

A date given to `--since` / `--until` must name a day that exists: `2026-02-30`,
`2025-02-29` (2025 is not a leap year) and `2026-13-99` are usage errors, not empty
reports. Rejecting them keeps "no data in that range" and "that range cannot exist"
distinguishable to a script, which only sees the exit code.

## The number

Headline **total tokens** = `input + output + cacheRead + cacheWrite`.

`reasoning` is **excluded** from the total because it overlaps `output` — it is a
sub-count of output tokens that the provider reports separately. It is still reported
on its own line. `input`, `cacheRead` and `cacheWrite` are mutually exclusive buckets,
so adding them never double counts. This is documented in `docs/data-source.md`.

## Public headers (`include/dshtok/`)

Frozen. Every translation unit includes them as
`#include "dshtok/<name>.h"`; the build adds `-Iinclude`.

| Header | Purpose |
| --- | --- |
| `version.h` | `DSHTOK_VERSION_STRING`, `DSHTOK_VERSION_MAJOR/MINOR/PATCH` |
| `util.h` | allocation, string copy, whole-file read, error formatting |
| `json.h` | arena + DOM JSON parser |
| `ledger.h` | ledger model, loader, range filters |
| `format.h` | number/currency formatting |
| `path.h` | ledger path discovery |
| `dshtok.h` | umbrella header |

## The source tree

| Path | Role |
| --- | --- |
| `SPEC.md`, `include/dshtok/*.h` | the contract, frozen |
| `tests/mini_test.h` | header-only test harness |
| `src/util.c` | allocation, file read, path helpers |
| `src/main.c` | CLI parse + dispatch |
| `src/ledger.c` | ledger model, merging by day/session/model |
| `src/format.c` | number and currency formatting |
| `src/path.c` | ledger path discovery |
| `src/report.c` | rendering of every command |
| `src/json.c` | JSON parser + arena |
| `tests/test_json.c`, `tests/fixtures/json_*.json` | parser tests and fixtures |
| `tests/test_format.c`, `tests/test_ledger.c` | formatting and ledger-model tests |
| `tests/test_cli.c` | spawns the built binary, checks exit codes and output |
| `tests/test_path.c` | candidate list, de-duplication and the `/home/<USER>` fallback |
| `tests/integration.sh` | end-to-end against a synthetic ledger |
| `tests/installer.sh` | drives `scripts/install.sh` against a local `file://` release |
| `scripts/install.sh` | one-line installer for Linux and macOS |
| `Makefile`, `CMakeLists.txt`, `.github/workflows/ci.yml`, `.github/workflows/release.yml` | build, CI and release |
| `README.md`, `LICENSE`, `CHANGELOG.md`, `CONTRIBUTING.md`, `docs/*.md`, `docs/demo.svg` | documentation |

`tests/test_cli.c` exercises `src/report.c` through the CLI rather than linking it
directly — the rendering is the contract, so it is tested as a contract.

## Build contract

```
make            # -> build/dshtok (or build/dshtok.exe)
make test       # -> builds and runs every test binary; non-zero on failure
make integration  # -> tests/integration.sh (needs a POSIX shell)
make installer    # -> tests/installer.sh (needs a POSIX shell and curl)
make clean
```

Every test file `tests/test_*.c` is compiled into its own binary `build/test_<name>`
together with `src/util.c src/json.c src/ledger.c src/format.c src/path.c src/report.c`
(all objects except `src/main.c`) and run from the repository root.

Object files live in `build/`. `make` must work from a clean tree with no arguments
beyond the standard `CC`, `CFLAGS`, `LDFLAGS` variables, defaulting to
`CC=cc`, `CFLAGS=-std=c99 -O2 -Wall -Wextra -Wpedantic -Iinclude`.

Tagged releases (`v*`) are built by `.github/workflows/release.yml`, which runs the test
suite first and only then publishes three assets — `dshtok-linux-x86_64`,
`dshtok-macos-universal` (arm64 and x86_64 in one file, checked with `lipo -verify_arch`)
and `dshtok-windows-x86_64.exe` — together with `SHA256SUMS`. `scripts/install.sh` and the
workflow share those names: renaming an asset means changing the installer in the same
commit, and `make installer` is the test that notices when they drift apart.

## Output formats

### `dshtok total` (ASCII, default)

```
dshtok 1.0.0 - DeepSeek Harness token usage
source: C:\Users\me\.dsh\storages\cost-meter\ledger.json

  total tokens             5,208,779,846   (5.21B)
  --------------------------------------------------------
  input                      162,463,635   3.1%
  output                      35,198,843   0.7%
  cache read               5,011,117,368   96.2%
  cache write                          0   0.0%
  --------------------------------------------------------
  reasoning (in output)        6,325,370   18.0% of output
  calls                           37,974
  days                                34   2026-10-12 .. 2026-11-23
  cost                      CNY 1,921.10   (api CNY 1,835.46)
```

Rules:
* the samples in this document come from one real 34 day ledger; the values are
  illustrative and will not match yours, the layout is identical
* numbers are grouped with `,` (ASCII) — same in every locale
* the compact form in parentheses appears only when the value is >= 10,000
* column 1 label width is 21, right-aligned numbers with width 17, then the share
* the rule is 56 characters wide in `total`
* `cost` line is omitted when the ledger has no `config`
* `--unicode` replaces `-` with `—` in the title, `-` rules with box drawing and
  `CNY` with the ledger symbol

### `dshtok daily`

```
  date                input         output       cache read    cache write           total    calls
  2026-10-12          7,214            101                0              0           7,315        1
  2026-10-13        484,325         95,323        5,050,168              0       5,629,816       90
  ...
  2026-11-23      4,902,836      2,445,765      519,210,368              0     526,558,969    2,534
  total         162,463,635     35,198,843    5,011,117,368              0   5,208,779,846   37,974
```

### `dshtok models`

```
  #  provider:model                                  total tokens    share
  1  minimax-plan:MiniMax-M3                        3,099,666,305    59.5%
  2  opencode-go:deepseek-v4.1-flash                1,346,107,277    25.8%
```

### `dshtok sessions`

```
  #  date          total tokens     calls  title
  1  2026-11-09   3,378,375,591    20,092  src漏洞挖掘详细步骤
```

### `dshtok doctor`

A report about the environment, not about usage, so it has no `--json` or `--csv` form and
`-q` does not suppress it. Two blocks:

* `search` — `DSH_HOME`, `DSHTOK_LEDGER` (`(unset)` when absent), and one line per
  candidate path. A candidate that exists is prefixed `[found]`, and the winner also gets
  `<- selected`. When `--path` short-circuits discovery the list is
  `(none: --path was given, no search was done)`.
* `ledger` — `path` (`unknown` when none), `size`, `status`, and when the file parsed:
  `days`, `range`, `sessions`, `models`, `calls`, `tokens`, `currency`.

`size` is the exact byte count grouped with `,`, followed by a 1024-based human form, e.g.
`2,466,015 bytes (2.4 MB)`. Under 1024 bytes the parenthetical is omitted — `512 bytes`,
never `512 bytes (512 bytes)` — because the byte count already is the human form. A
missing or unreadable file prints `size                 unknown`.

Exit code is `0` whether or not a ledger was found: the report is the answer, and its
`status` line says what is wrong when something is.

### `--json`

One JSON object on stdout, keys in this order:

```json
{
  "tool": "dshtok",
  "version": "1.0.0",
  "source": "…/ledger.json",
  "currency": "CNY",
  "range": {"since": null, "until": null},
  "total": {"tokens": 5208779846, "input": 162463635, "output": 35198843,
            "cacheRead": 5011117368, "cacheWrite": 0, "reasoning": 6325370,
            "calls": 37974, "cost": 1921.10, "apiCost": 1835.46},
  "days": [{"date": "2026-10-12", "tokens": 7315, "input": 7214, …}],
  "models": [{"name": "minimax-plan:MiniMax-M3", "tokens": 3099666305}],
  "sessions": [{"id": "session-…", "date": "2026-11-09", "tokens": 3378375591,
                "calls": 20092, "title": "…"}]
}
```

`"days"` is present for `daily`, `"models"` for `models`, `"sessions"` for `sessions`,
and all three for `total` when `--verbose`. Integers are printed without a decimal
point when exact, otherwise with up to 2 decimals. Strings are escaped per RFC 8259;
non-ASCII is emitted as UTF-8 without escaping.

### `--csv`

Header row plus one row per entry, comma separated, `\n` line endings, numbers
unescaped, text fields wrapped in `"` with `"` doubled. No BOM.

## Ledger semantics (authoritative)

Read `docs/data-source.md` for the full story. The essentials the loader must honour:

1. Only `days` is authoritative. Never recompute totals from
   `days[*].sessions[]` raw events or from the usage cache — those are incomplete.
2. `days` is an object keyed by `YYYY-MM-DD`; iterate in string order (already
   chronological).
3. **The same session id appears under several days** (a session that ran past
   midnight, or a resumed session). Merge by `id` when building the leaderboard and
   take the title from any day that has one.
4. `days[d].byProviderModel` is an object keyed `provider:model` with the same numeric
   fields; merge those keys across days for the `models` table.
5. Missing numeric fields default to 0. Fields may be integers or floats.
6. Unknown fields are ignored, never an error. A future `version` bump must not crash.
7. Totals come from the per-day top-level numbers, not from the per-model or
   per-session breakdowns (they agree today, but the top level is the record of truth).

## Unknown-ledger policy

If `storages/cost-meter/ledger.json` does not exist, print:

```
dshtok: no ledger found
  looked for:
    C:\Users\me\.dsh\storages\cost-meter\ledger.json
  This file is written by the dsh-cost-meter plugin, which must be installed and
  must have recorded at least one request. See docs/data-source.md.
```

and exit `1`. `doctor` shows the same information on stdout and exits `0`; when a file is
found it also reports that file's size and the ledger's shape (days, range, sessions,
models, calls, tokens, currency).
