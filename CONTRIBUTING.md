# Contributing to dshtok

Thanks for looking. `dshtok` is a small tool with a frozen interface, so most of what
follows is about keeping it small.

## Before you start

Read two files:

1. `SPEC.md` — the frozen interface contract. Command names, option names, output
   formats, exit codes and the token arithmetic are all specified there. If you think the
   contract is wrong, open an issue; do not change behaviour and adjust the spec to match.
2. `docs/architecture.md` — the module map and data flow, so you know which file a change
   belongs in.

## Build and test

```sh
make                # -> build/dshtok  (build/dshtok.exe on Windows)
make test           # build and run every tests/test_*.c
make integration    # end-to-end check with tests/integration.sh (needs bash)
make installer      # tests/installer.sh against a local fake release (needs bash + curl)
make release        # clean rebuild with -O2 -DNDEBUG
make clean
```

`make integration` and `make installer` need a POSIX shell, so they do not run on plain
Windows; `make test` is the gate that runs everywhere.

Requirements: a C99 compiler and `make`. Nothing else — no third-party libraries, no
package manager, no code generator. The only external archive is `libm`, which is part of
every C implementation and is linked by default on platforms that keep it separate. CMake
users can build the same targets from `CMakeLists.txt` and run the suite with `ctest`.

On Windows, `make` recipes are POSIX `sh`, so run `mingw32-make` from an MSYS2 or Git Bash
shell. Plain `cmd.exe` is not a supported recipe shell.

Every change must satisfy all of:

```sh
make test
cc -std=c99 -Wall -Wextra -Wpedantic -Iinclude -c src/*.c   # warning-clean
gcc -std=c99 -pedantic-errors -Iinclude -c src/<file>.c     # no extensions
```

If `make test` cannot pass, the change is not ready. Do not disable a test to land a
change; if a test is wrong, say why in the pull request.

## Language rules

- **C99, standard library only.** No third-party code, no vendored libraries, no
  generated code checked in.
- **No compiler extensions.** The only permitted platform conditionals are the `_WIN32` /
  `__unix__` guards SPEC.md allows. Today the sources use only `_WIN32`, with the POSIX
  path in the `#else` branch — keep it that way unless a real need appears. If a compiler
  needs a flag to accept your code, the code is wrong.
- **No VLAs** and no variable-length arrays by another name. All automatic storage is a
  compile-time constant.
- **Warning-clean at `-Wall -Wextra -Wpedantic`.** Not "mostly clean". If a warning is a
  false positive, the fix is to restructure the code, not to cast it away.
- **No network, ever.** This is a load-bearing property of the tool, not a preference. A
  change that opens a socket, downloads a price table or phones home will be rejected.
- **One file in, one report out.** Do not read session logs, do not read the usage cache,
  do not write state. See `docs/data-source.md` for why.

## Coding style

- 4 spaces, never tabs. No trailing whitespace. Keep lines under 100 columns where
  practical; long format strings and tables may exceed it when splitting them would hurt
  readability.
- `/* ... */` comments only. `//` line comments are not used anywhere in this tree, even
  where C99 permits them.
- Braces on their own line for functions; K&R braces are used inside function bodies for
  short blocks, matching the existing files. Read the neighbouring code and match it —
  consistency inside a file beats any style rule.
- Declarations at the top of a block, as the rest of the C99 sources do.
- Naming: `snake_case` for functions and variables, `UPPER_CASE` for macros,
  `lower_case` for struct tags and typedefs (`Usage`, `Ledger`, `DayRow`). Public symbols
  carry the `dshtok_` / `ledger_` / `json_` / `path_` / `fmt_` prefix of their module;
  helpers shared only inside a file are `static`.
- Every allocation goes through the `xmalloc` family in `util.h` unless the failure is
  recoverable (file reads). Ownership is documented at the point of return: if a function
  hands back a pointer, the comment says who frees it.
- Public functions get a header comment when the name alone does not explain the
  contract: preconditions, what is mutated, what the caller owns.

## Tests

Tests are small C programs under `tests/`, one binary per file, using `tests/mini_test.h`.
They may not call into `main.c` — test binaries link the core objects only, so anything
you want to test must be reachable through a header.

- Add a test with every behavioural change. A bug fix without a regression test will be
  asked for a second time.
- Fixtures belong in `tests/fixtures/`. `tests/test_ledger.c` uses small synthetic
  ledgers, never a real one — do not commit a user's ledger.
- CLI-level behaviour (exit codes, `--json` shape, `--csv` quoting) belongs in
  `tests/test_cli.c`, which spawns the built binary.
- Use `$(DSHTOK_BIN)` in `tests/integration.sh`; `make integration` sets it.

## Where things live

The tree is layered, and the layer a change belongs to decides what else it may touch:

| Layer | Paths | Rule |
| --- | --- | --- |
| Contract | `SPEC.md`, `include/dshtok/*.h` | Frozen. The public surface — option names, exit codes, output layout, `--json` keys. Changing it is a deliberate, documented decision, not a side effect of a fix. |
| Test harness | `tests/mini_test.h` | Shared by every suite. Extend it, do not fork it. |
| Implementation | `src/*.c` | Free to change as long as the contract and the tests still hold. |
| Build | `Makefile`, `CMakeLists.txt`, `.github/workflows/ci.yml` | Must keep working on gcc, clang and MinGW-w64. |
| Release | `.github/workflows/release.yml`, `scripts/install.sh` | The workflow publishes three asset names; the installer downloads exactly those. Change one and change the other, then run `make installer`. |
| Docs | `README.md`, `docs/*.md`, `docs/demo.svg`, `CHANGELOG.md` | Sample output in the docs is captured from a real run; see below. |

Headers are the contract. If a change genuinely needs a new public function, open an issue
with the signature you want and the reason, rather than adding it in passing.

Two conventions that catch people out:

- **`README.md` samples are real output.** Every fenced block showing `dshtok` output was
  captured from an actual run and then checked line by line. If you change the layout,
  regenerate the samples in the same commit or the README starts lying. `docs/demo.svg` is
  the same thing as a picture: it was drawn from one capture of `dshtok total`, one token
  per `<text>` element so the columns line up whatever font the viewer picks. If you change
  the total layout, redraw it too.
- **Numbers in prose drift.** The ledger grows, so a hard-coded "5,208,779,846" in
  explanatory text goes stale. Describe magnitudes and ratios ("about 5 billion", "about
  48%"), and put exact numbers only in captured sample output where they are clearly
  illustrative.


## Adding a command or an option

This is the most disruptive kind of change, because the CLI surface is frozen and
documented in three places at once (the spec, the `--help` text, and the README). If you
want to propose one:

1. Open an issue describing the user question the new output answers, not the code.
2. Include the exact output layout — columns, widths, ordering — since the layout *is* the
   feature here.
3. Say what happens with `--json` and `--csv`. Every command must have a defined shape in
   all three representations.

Adding an option that only a single command uses is usually a sign the command is two
commands.

## Documentation

Docs live in `docs/`. If a change affects the observable behaviour, update the README and
`CHANGELOG.md` in the same pull request. The changelog is
[Keep a Changelog](https://keepachangelog.com/) format; add your entry under
`## [Unreleased]` with the right heading (`Added`, `Changed`, `Fixed`, `Removed`) and
move nothing else.

Factual claims about the data format must be traceable to `SPEC.md`, the ledger itself, or
the `dsh-cost-meter` source. Do not soften or round a claim to make the docs read better.

## Commit messages

Short imperative subject line, 72 characters at most, no trailing period:

```text
ledger: merge sessions that span midnight
format: never split a UTF-8 sequence when truncating
docs: explain why reasoning is not in the total
```

Prefix with the module or area when it helps (`json:`, `ledger:`, `report:`, `build:`,
`docs:`, `tests:`). The body explains *why*, not what — the diff already says what. If a
change has a surprising consequence, write it down; a future reader will not reconstruct
it from the code.

One logical change per commit. Do not mix a formatting pass with a behaviour change.

## Pull request checklist

- [ ] `make test` passes.
- [ ] Builds warning-clean at `-std=c99 -Wall -Wextra -Wpedantic`.
- [ ] New behaviour has a test; new output has a docs update.
- [ ] `CHANGELOG.md` has an entry under `[Unreleased]`.
- [ ] No new dependencies, no new network use, no files outside your ownership.
- [ ] No trailing whitespace, no tabs.

## Reporting a wrong number

A wrong total is usually a data question, not a code question. Before filing, check:

1. `dshtok doctor` — which ledger path won?
2. The header's date range — does it cover the period you expect? The plugin retains only
   `config.historyDays` days.
3. `docs/data-source.md`, section "What could make the number wrong".

If after that you believe the arithmetic is wrong, include the ledger's `config.historyDays`,
the date range shown by `dshtok`, and the two totals (yours and `dshtok`'s). Do not attach
a real ledger unless you are comfortable publishing your usage history — a hand-written
mini-ledger that reproduces the discrepancy is far more useful.
