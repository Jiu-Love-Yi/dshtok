# Architecture

`dshtok` is a single C99 program that turns one JSON file into a report. It is
deliberately small: seven translation units, and no dependencies beyond the C standard
library (plus `libm` for `floor` / `ceil` / `pow` / `fabs`, which is part of every C
implementation) and the two or three POSIX/Win32 declarations needed to tell whether a
file exists and whether stdout is a terminal. There is no configuration layer, no plugin
system, no network code and no persistent state of its own.

This page describes the parts, how data flows between them, and the constraints that
shape the design. It is written against the frozen headers in `include/dshtok/`, which
are the contract; where implementation and header disagree, the header is wrong and gets
fixed first.

## Module map

| Unit | Header | Responsibility |
| --- | --- | --- |
| `src/main.c` | — | parse `argv` into `Options`, dispatch to a command, own exit codes |
| `src/util.c` | `util.h` | allocation wrappers, whole-file read, path join, errors |
| `src/json.c` | `json.h` | RFC 8259 parser producing a `JsonValue` tree in one arena |
| `src/ledger.c` | `ledger.h` | interpret the parsed tree as a `Ledger`; merge, sort, filter |
| `src/format.c` | `format.h` | number/percentage/money formatting, padding, UTF-8-safe truncation |
| `src/path.c` | `path.h` | build the ordered candidate list of ledger paths |
| `src/report.c` | `dshtok.h` | render `total` / `daily` / `models` / `sessions` / `doctor` |

Headers also expose `version.h` (name, tagline, version numbers) as compile-time
constants, so `--version` and the report header cannot drift from the build.

### Why this split

The seams are drawn where the *kinds of correctness* differ, so each can be tested in
isolation:

- `json.c` must be correct about bytes: UTF-8, escapes, numbers, nesting depth. It knows
  nothing about ledgers. It is the only module with its own test fixtures.
- `ledger.c` must be correct about *meaning*: which fields are buckets, how sessions
  merge, what a missing field means. It never touches formatting.
- `format.c` must be correct about *presentation*: grouping separators, K/M/B/T
  compaction, percent rounding, alignment, and never splitting a UTF-8 sequence when
  truncating a title.
- `report.c` composes the above into output and knows the contract's exact layout, label
  and column widths (`DSHTOK_LABEL_WIDTH` 21, `DSHTOK_NUMBER_WIDTH` 17), and the JSON and
  CSV envelopes.
- `path.c` is the only place that knows where DSH keeps things, and the only place that
  reads environment variables for discovery.

A module that needs a fact from another asks through its header. There are no globals,
and no module reaches into another's storage.

## Data flow

```text
argv ──► main.c: Options
               │
               ▼
     path.c: candidate list ──► first readable file  (or "no ledger found", exit 1)
               │
               ▼
     util.c: read_entire_file  ──► one malloc'd buffer
               │
               ▼
     json.c: json_parse        ──► JsonValue tree, one arena, one free
               │
               ▼
     ledger.c: ledger_load     ──► Ledger { days[], models[], sessions[], total }
               │
               ├─ ledger_filter_range(since, until)   (inclusive, on date strings)
               │
               ├─ ledger_models_sorted()   / ledger_sessions_sorted()
               │
               ▼
     report.c: render text | --json | --csv  ──► stdout
               │
               ▼
     ledger_free + json_free + free(buffer)   ──► exit 0
```

Each stage has exactly one failure mode and it is visible:

- path resolution fails → "no ledger found", the candidate list is printed, exit 1;
- read fails → `read_entire_file` writes `err`, exit 1;
- parse fails → the message names line and column, e.g.
  `line 3 column 17: expected ':' after object key`, exit 1;
- semantic load fails → same contract, exit 1.

`--path FILE` short-circuits discovery and skips the candidate list entirely.

## Memory strategy

One arena, one free.

`json_parse` allocates a bump-pointer arena, hands out every `JsonValue`, string and
member array from it, and `json_free` releases the arena in a single call. The parser
never fails halfway and leaves a half-built tree to be walked: on error it frees the
arena and returns `NULL`. There are no ownership cycles and no reference counts, so
there is nothing for a caller to remember.

Above the parser, `Ledger` owns its own memory in the ordinary C way: `ledger_load`
fills it, `ledger_free` releases it. Titles and model names are copied out of the JSON
tree into the ledger, because the tree does not survive the load — the ledger is the
long-lived object, and it must not alias an arena it cannot free.

All allocation goes through `xmalloc` / `xcalloc` / `xrealloc` / `xstrdup` / `xstrndup`
from `util.h`. These terminate the process on out-of-memory with a message rather than
returning `NULL` to a caller that would have to check. This is a deliberate trade: a CLI
that cannot allocate a few megabytes is not going to recover, and threading null checks
through every call site costs more in review attention than it buys. `read_entire_file`
handles read errors, which are recoverable and therefore *are* checked.

The `days` array grows geometrically — it starts at 32 entries and doubles through
`day_capacity`, so a ledger with thousands of days is O(n) amortized. Per-day `sessions`
and `models` arrays are a different case: their size is known the moment the day is
parsed, so each is allocated once with `xcalloc` for exactly the JSON array or object
size.

Merging is sort-then-collapse rather than a hash map. To build the `models` table,
`ledger_build_models` counts every per-day model entry, copies them into one temporary
array, `qsort`s by name, then walks equal runs and `usage_add`s them. Sessions work the
same way in `ledger_build_sessions`, with the id as the key; within a run it keeps the
earliest day, the latest `at` timestamp, and the first non-`NULL` title. That keeps the
merge O(n log n) with no auxiliary index, and it is why a session spanning midnight comes
out as a single row. The arrays returned by `ledger_models_sorted` /
`ledger_sessions_sorted` are copies owned by the caller, which releases them; the
ledger's own copies stay untouched.

The whole ledger is in memory at once. On a real machine it is about 2.4 MB, which is small
enough that the arena approach is chosen for simplicity and speed, not because streaming
would be needed.

## Why C99 with no dependencies

- **It builds anywhere.** A C99 compiler and `make` are enough: no package manager, no
  interpreter, no network — which is the situation the tool is for. A binary you can copy
  to a headless box and run is worth more than a nicer language. Seven `.c` files also
  compile by hand in one command if `make` is not available.
- **The payload is small.** One JSON parse, four sums, some formatting. There is no data
  structure that needs a library.
- **Auditability.** "It reads one file and adds four numbers" is a claim a user can check
  by reading the source in an afternoon. Every dependency is a claim you cannot check, and
  a supply-chain surface in a tool that reports your billing data.

Consequences that are accepted, not hidden:

- No zstd, so session logs are out of scope (see `docs/data-source.md`).
- No regex, so date validation is a hand-written character check
  (`ledger_valid_date` in `ledger.c`, plus the calendar check `cli_date_is_real` in
  `main.c`).
- Locale-independent number formatting is implemented by hand in `format.c`, which is why
  grouping separators are always `,` regardless of environment.

Platform differences are confined to six `#if defined(_WIN32)` blocks — two in `util.c`, one in `report.c`, three in `path.c` — with the POSIX branch always the `#else`:
`_isatty` versus `isatty` for the colour decision (`report.c`), `fopen` versus `stat` for
"does this file exist" (`util.c`), `USERPROFILE` versus `HOME` as the first place to look
for a home directory, case-insensitive equality for candidate de-duplication on Windows,
and the `/home/<USER>` fallback, which is compiled out on Windows because there is no
`/home` there to look in (`path.c`). The code never tests `__unix__` or any other platform macro. Nothing
else in the program knows what platform it is on. No compiler extensions are used, no
VLAs, and the tree is expected to compile warning-clean under
`-std=c99 -Wall -Wextra -Wpedantic`; `-pedantic-errors` is the stricter bar that
`CONTRIBUTING.md` asks a patch to meet.

## Command dispatch and exit codes

`main.c` maps the first non-option argument to a command name (`total` is the default),
validates option combinations, and calls `report_run(&opt, ledger_path)`. Unknown
commands and unknown options are usage errors, detected before any file is opened.
`help` and `--help` / `--version` are handled without touching the filesystem.

| Code | Meaning |
| --- | --- |
| `0` | success (also `doctor` with no ledger: it reports the situation on stdout) |
| `1` | runtime error: the ledger is missing, unreadable or malformed |
| `2` | usage error: unknown option or command, missing option argument, invalid option value |

In this build every option-validation failure exits `2`: an unknown option or command, a
missing argument, an unparsable `--since` / `--until` / `--top` value, a `--since` /
`--until` date that names no real day (`2026-02-30`, `2026-13-99`), `--since` later than
`--until`, and the two option combinations `report.c` rejects (`--json` with `--csv`, and
`--csv` on `total`).

A date the user typed gets a stricter test than a date read out of the ledger, and the
difference is deliberate. `ledger_valid_date` in `ledger.c` checks the shape only — four
digits, dash, two, dash, two — because the ledger is the authority on which days exist,
and a hand-edited file with an odd key should not make the whole report fail. A value from
the command line is not an authority on anything, so `main.c` asks whether it names a day
on the calendar (`cli_date_is_real`, including the month lengths and the leap-year rule).
Without that, `--since 2026-13-99` would filter every day out and exit `0`, which a script
cannot tell apart from "you have spent nothing".

The distinction is what makes the tool scriptable: `2` means "you called me wrong, do not
retry", `1` means "the environment is not what you promised", `0` means "the output is
meaningful".

## Output contract

`report.c` owns the three renderings and the invariant that they agree:

- **Text** — the default. Pure ASCII unless `--unicode` is passed, in which case box
  drawing and the ledger's own currency symbol are used. Colour is ANSI SGR bold around
  the headline number, used only when stdout is a terminal and `--no-color` was not
  given. `total --verbose` appends the `provider:model` table after the summary block; the
  title and `source:` banner is printed exactly once, by the renderer that owns it.
- **JSON** (`--json`) — one object, deterministic key order (`tool`, `version`, `source`,
  `currency`, `range`, then `days_count` for `total`, then `total`, then the command's
  arrays). RFC 8259 escaping; non-ASCII is emitted as UTF-8, not `\u` escapes. Only the
  arrays relevant to the command are present; `total --verbose` emits all three.
- **CSV** (`--csv`) — header row plus one row per entry, `\n` endings, no BOM, numbers
  unescaped, text quoted with `"` and inner quotes doubled. Available for `daily`,
  `models` and `sessions`.

`doctor` is the exception to "three renderings": it is a report about the environment, not
data about your usage, so it always prints the same human block. It lists the candidates
that were probed (or says that `--path` skipped the search), which one won, that file's
size in bytes and in a human form, and the shape of the ledger — version, days, range,
sessions, models, calls, tokens and currency. `--json`, `--csv` and `-q` do not apply to
it, because reading the report is the entire purpose of that command. It exits `0` even
with no ledger at all: finding nothing is a valid answer to "what is on this machine", and
the message explains what has to be installed for that to change.

Nothing is printed to stdout on the error paths; errors go to stderr, so a pipeline can
treat a non-empty stdout as data.

## Test strategy

Unit tests are small C programs under `tests/`, each linking every object except
`src/main.c` and built into `build/test_<name>` by the same Makefile. They use
`tests/mini_test.h`. Areas: JSON parsing (`test_json.c`, with fixtures under
`tests/fixtures/`), ledger semantics (`test_ledger.c`), formatting (`test_format.c`), path
candidate construction (`test_path.c`), and end-to-end CLI behaviour including exit codes
(`test_cli.c`). Two shell scripts cover what C tests cannot reach cheaply:
`tests/integration.sh` drives the built binary as a user would, and `tests/installer.sh`
runs `scripts/install.sh` against a local `file://` release to exercise the download,
checksum and install paths without the network. `make test`, `make integration` and
`make installer` run them, and CI runs all three on the POSIX legs.

The design for testability follows from the module map: `json_parse` and `ledger_load`
are pure functions of their inputs, `format.c` has no globals, and `report.c` takes an
`Options` struct rather than reading `argv` itself, so a test can construct any
combination directly.
