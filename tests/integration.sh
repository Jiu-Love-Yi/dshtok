#!/usr/bin/env bash
# tests/integration.sh - end-to-end smoke test for the dshtok binary.
#
# Builds the binary if it is not there yet, writes a synthetic ledger into a
# temp dir and drives every command against it: total, daily, models, sessions,
# doctor, the --json variants, the --since / --until filters, --quiet, --csv and
# the documented exit codes.
#
# The fixture has two days, two provider:model pairs, and one session id that
# appears under BOTH days, so the merge path in the ledger loader is exercised.
# The expected headline number is computed here with shell arithmetic from the
# same variables that are written into the JSON and then located in the output
# with grep, so a wrong total cannot slip through. reasoning is deliberately
# non-zero and must stay OUT of the headline total.
#
# Text tooling only: grep, sed, awk, printf. Nothing beyond POSIX shell and
# those four is required, so the script runs on a bare CI runner.
#
# Usage:
#   bash tests/integration.sh
#   DSHTOK_BIN=/abs/path/to/dshtok bash tests/integration.sh
#   CC=clang MAKE=make bash tests/integration.sh
#
# Exits 0 when every check passes and 1 on the first failure.
set -euo pipefail

SECTION="start"
WORK=""

note() { printf 'integration: %s\n' "$*"; }

fail() {
    printf 'integration: FAIL [%s]: %s\n' "$SECTION" "$*" >&2
    exit 1
}

dump() {
    printf 'integration: ---- %s ----\n' "$1" >&2
    sed 's/^/  | /' "$1" >&2
}

cleanup() {
    if [ -n "$WORK" ] && [ -d "$WORK" ]; then
        rm -rf "$WORK"
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# ---------------------------------------------------------------------------
# 0. Locate the repository, a make, and the binary.
# ---------------------------------------------------------------------------

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
[ -f "$repo_root/SPEC.md" ] || fail "SPEC.md not found above $script_dir"

pick_make() {
    if [ -n "${MAKE:-}" ]; then
        printf '%s\n' "$MAKE"
        return 0
    fi
    for candidate in make gmake mingw32-make; do
        if command -v "$candidate" >/dev/null 2>&1; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

find_binary() {
    for candidate in "$repo_root/build/dshtok" "$repo_root/build/dshtok.exe"; do
        if [ -f "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

SECTION="locate"
if [ -n "${DSHTOK_BIN:-}" ]; then
    [ -f "$DSHTOK_BIN" ] || fail "DSHTOK_BIN does not name a file: $DSHTOK_BIN"
    BIN=$DSHTOK_BIN
elif BIN=$(find_binary); then
    :
else
    if ! command -v "${CC:-cc}" >/dev/null 2>&1; then
        fail "no C compiler on PATH (tried '${CC:-cc}')"
    fi
    if ! REAL_MAKE=$(pick_make); then
        fail "no make found on PATH (tried make, gmake, mingw32-make)"
    fi
    note "building with $REAL_MAKE CC=${CC:-cc}"
    ( cd "$repo_root" && "$REAL_MAKE" CC="${CC:-cc}" ) || fail "build failed"
    BIN=$(find_binary) || fail "build produced no binary under build/"
fi
case "$BIN" in
    /* | ?:*) ;;
    *) BIN="$repo_root/$BIN" ;;
esac
note "using binary $BIN"
"$BIN" --version >/dev/null 2>&1 || fail "the binary does not run: $BIN"

WORK=$(mktemp -d 2>/dev/null || mktemp -d -t dshtok)
[ -d "$WORK" ] || fail "could not create a temp dir"

OUT=$WORK/stdout.txt
ERR=$WORK/stderr.txt
ALL=$WORK/combined.txt
LEDGER=$WORK/ledger.json
LEDGER_ARG=$LEDGER
if command -v cygpath >/dev/null 2>&1; then
    LEDGER_ARG=$(cygpath -w "$LEDGER")
fi

# ---------------------------------------------------------------------------
# 1. The synthetic ledger. These variables are the single source of truth: the
#    JSON is written from them and the expectations below are computed from the
#    same numbers.
# ---------------------------------------------------------------------------

D1=2026-01-05
D2=2026-01-06

d1_input=1000
d1_output=200
d1_cache_read=5000
d1_cache_write=100
d1_reasoning=40
d1_calls=8
d1_cost=0.10
d1_api_cost=0.08

d1_shared_input=700
d1_shared_output=150
d1_shared_cache_read=4000
d1_shared_cache_write=100

d1_alpha_only_input=300
d1_alpha_only_output=50
d1_alpha_only_cache_read=1000
d1_alpha_only_cache_write=0

d1_model_a_input=600
d1_model_a_output=100
d1_model_a_cache_read=3000
d1_model_a_cache_write=100

d1_model_b_input=400
d1_model_b_output=100
d1_model_b_cache_read=2000
d1_model_b_cache_write=0

d2_input=2000
d2_output=400
d2_cache_read=8000
d2_cache_write=200
d2_reasoning=90
d2_calls=12
d2_cost=0.20
d2_api_cost=0.16

d2_shared_input=1200
d2_shared_output=250
d2_shared_cache_read=5000
d2_shared_cache_write=200

d2_beta_only_input=800
d2_beta_only_output=150
d2_beta_only_cache_read=3000
d2_beta_only_cache_write=0

d2_model_a_input=1500
d2_model_a_output=300
d2_model_a_cache_read=6000
d2_model_a_cache_write=200

d2_model_b_input=500
d2_model_b_output=100
d2_model_b_cache_read=2000
d2_model_b_cache_write=0

cat >"$LEDGER" <<JSON
{
  "version": 1,
  "config": {
    "currency": "CNY",
    "decimals": 2,
    "pricingCurrency": "USD",
    "exchangeRate": 6.6777
  },
  "days": {
    "$D1": {
      "date": "$D1",
      "input": $d1_input,
      "output": $d1_output,
      "cacheRead": $d1_cache_read,
      "cacheWrite": $d1_cache_write,
      "reasoning": $d1_reasoning,
      "calls": $d1_calls,
      "cost": $d1_cost,
      "apiCost": $d1_api_cost,
      "byProviderModel": {
        "alpha:model-one": {
          "input": $d1_model_a_input,
          "output": $d1_model_a_output,
          "cacheRead": $d1_model_a_cache_read,
          "cacheWrite": $d1_model_a_cache_write,
          "reasoning": 20,
          "calls": 5,
          "cost": 0.06,
          "apiCost": 0.05
        },
        "beta:model-two": {
          "input": $d1_model_b_input,
          "output": $d1_model_b_output,
          "cacheRead": $d1_model_b_cache_read,
          "cacheWrite": $d1_model_b_cache_write,
          "reasoning": 20,
          "calls": 3,
          "cost": 0.04,
          "apiCost": 0.03
        }
      },
      "sessions": [
        {
          "id": "session-shared",
          "input": $d1_shared_input,
          "output": $d1_shared_output,
          "cacheRead": $d1_shared_cache_read,
          "cacheWrite": $d1_shared_cache_write,
          "reasoning": 30,
          "calls": 5,
          "cost": 0.07,
          "apiCost": 0.06,
          "title": "shared work",
          "at": 1767657600000
        },
        {
          "id": "session-alpha-only",
          "input": $d1_alpha_only_input,
          "output": $d1_alpha_only_output,
          "cacheRead": $d1_alpha_only_cache_read,
          "cacheWrite": $d1_alpha_only_cache_write,
          "reasoning": 10,
          "calls": 3,
          "cost": 0.03,
          "apiCost": 0.02,
          "title": "day one only",
          "at": 1767661200000
        }
      ]
    },
    "$D2": {
      "date": "$D2",
      "input": $d2_input,
      "output": $d2_output,
      "cacheRead": $d2_cache_read,
      "cacheWrite": $d2_cache_write,
      "reasoning": $d2_reasoning,
      "calls": $d2_calls,
      "cost": $d2_cost,
      "apiCost": $d2_api_cost,
      "byProviderModel": {
        "alpha:model-one": {
          "input": $d2_model_a_input,
          "output": $d2_model_a_output,
          "cacheRead": $d2_model_a_cache_read,
          "cacheWrite": $d2_model_a_cache_write,
          "reasoning": 70,
          "calls": 9,
          "cost": 0.14,
          "apiCost": 0.12
        },
        "beta:model-two": {
          "input": $d2_model_b_input,
          "output": $d2_model_b_output,
          "cacheRead": $d2_model_b_cache_read,
          "cacheWrite": $d2_model_b_cache_write,
          "reasoning": 20,
          "calls": 3,
          "cost": 0.06,
          "apiCost": 0.04
        }
      },
      "sessions": [
        {
          "id": "session-shared",
          "input": $d2_shared_input,
          "output": $d2_shared_output,
          "cacheRead": $d2_shared_cache_read,
          "cacheWrite": $d2_shared_cache_write,
          "reasoning": 60,
          "calls": 8,
          "cost": 0.12,
          "apiCost": 0.10,
          "at": 1767744000000
        },
        {
          "id": "session-beta-only",
          "input": $d2_beta_only_input,
          "output": $d2_beta_only_output,
          "cacheRead": $d2_beta_only_cache_read,
          "cacheWrite": $d2_beta_only_cache_write,
          "reasoning": 30,
          "calls": 4,
          "cost": 0.08,
          "apiCost": 0.06,
          "title": "day two only",
          "at": 1767747600000
        }
      ]
    }
  }
}
JSON

[ -s "$LEDGER" ] || fail "the synthetic ledger was not written"

# ---------------------------------------------------------------------------
# 2. What the numbers must be, computed from the variables above.
# ---------------------------------------------------------------------------

day1_total=$((d1_input + d1_output + d1_cache_read + d1_cache_write))
day2_total=$((d2_input + d2_output + d2_cache_read + d2_cache_write))
expected_total=$((day1_total + day2_total))
expected_input=$((d1_input + d2_input))
expected_output=$((d1_output + d2_output))
expected_cache_read=$((d1_cache_read + d2_cache_read))
expected_cache_write=$((d1_cache_write + d2_cache_write))
expected_calls=$((d1_calls + d2_calls))
expected_reasoning=$((d1_reasoning + d2_reasoning))

shared_day1=$((d1_shared_input + d1_shared_output + d1_shared_cache_read + d1_shared_cache_write))
shared_day2=$((d2_shared_input + d2_shared_output + d2_shared_cache_read + d2_shared_cache_write))
shared_merged=$((shared_day1 + shared_day2))

alpha_day1=$((d1_model_a_input + d1_model_a_output + d1_model_a_cache_read + d1_model_a_cache_write))
alpha_day2=$((d2_model_a_input + d2_model_a_output + d2_model_a_cache_read + d2_model_a_cache_write))
alpha_total=$((alpha_day1 + alpha_day2))
beta_day1=$((d1_model_b_input + d1_model_b_output + d1_model_b_cache_read + d1_model_b_cache_write))
beta_day2=$((d2_model_b_input + d2_model_b_output + d2_model_b_cache_read + d2_model_b_cache_write))
beta_total=$((beta_day1 + beta_day2))

alpha_only_total=$((d1_alpha_only_input + d1_alpha_only_output + d1_alpha_only_cache_read + d1_alpha_only_cache_write))
beta_only_total=$((d2_beta_only_input + d2_beta_only_output + d2_beta_only_cache_read + d2_beta_only_cache_write))

# The fixture would be inconsistent if these do not add up, so check the fixture
# itself before blaming the binary.
[ "$day1_total" -eq $((alpha_day1 + beta_day1)) ] \
    || fail "fixture bug: day 1 models do not sum to the day total"
[ "$day2_total" -eq $((alpha_day2 + beta_day2)) ] \
    || fail "fixture bug: day 2 models do not sum to the day total"
[ "$day1_total" -eq $((shared_day1 + alpha_only_total)) ] \
    || fail "fixture bug: day 1 sessions do not sum to the day total"
[ "$day2_total" -eq $((shared_day2 + beta_only_total)) ] \
    || fail "fixture bug: day 2 sessions do not sum to the day total"
[ "$expected_total" -eq $((alpha_total + beta_total)) ] \
    || fail "fixture bug: models do not sum to the grand total"

# Group with commas the way dshtok prints numbers, so the expectation can be
# grepped literally. POSIX awk, no locale assumptions.
group_digits() {
    awk -v n="$1" 'BEGIN {
        s = sprintf("%.0f", n)
        out = ""
        while (length(s) > 3) {
            out = "," substr(s, length(s) - 2) out
            s = substr(s, 1, length(s) - 3)
        }
        printf "%s%s\n", s, out
    }'
}
[ "$(group_digits 0)" = "0" ] || fail "grouping helper is broken"
[ "$(group_digits 300)" = "300" ] || fail "grouping helper is broken"
[ "$(group_digits 1000)" = "1,000" ] || fail "grouping helper is broken"
[ "$(group_digits 16900)" = "16,900" ] || fail "grouping helper is broken"

g_total=$(group_digits "$expected_total")
g_input=$(group_digits "$expected_input")
g_output=$(group_digits "$expected_output")
g_cache_read=$(group_digits "$expected_cache_read")
g_cache_write=$(group_digits "$expected_cache_write")
g_reasoning=$(group_digits "$expected_reasoning")
g_calls=$(group_digits "$expected_calls")
g_day1=$(group_digits "$day1_total")
g_day2=$(group_digits "$day2_total")
g_shared=$(group_digits "$shared_merged")
g_alpha=$(group_digits "$alpha_total")
g_beta=$(group_digits "$beta_total")
g_shared_day1=$(group_digits "$shared_day1")

# ---------------------------------------------------------------------------
# 3. Assertion helpers.
# ---------------------------------------------------------------------------

assert_regex() { # description, extended regex, file
    if ! grep -Eq -- "$2" "$3"; then
        printf 'integration: FAIL [%s]: %s\n  wanted regex: %s\n' \
            "$SECTION" "$1" "$2" >&2
        dump "$3"
        exit 1
    fi
}

assert_no_regex() { # description, extended regex, file
    if grep -Eq -- "$2" "$3"; then
        printf 'integration: FAIL [%s]: %s\n  unwanted regex: %s\n' \
            "$SECTION" "$1" "$2" >&2
        dump "$3"
        exit 1
    fi
}

assert_line_with() { # description, needle, also-expected, file
    if ! awk -v needle="$2" -v want="$3" '
        index($0, needle) { if (index($0, want)) seen = 1 }
        END { exit(seen ? 0 : 1) }
    ' "$4"; then
        printf 'integration: FAIL [%s]: %s\n' "$SECTION" "$1" >&2
        printf '  wanted a line containing "%s" and "%s"\n' "$2" "$3" >&2
        dump "$4"
        exit 1
    fi
}

count_lines_with() { # needle, file
    awk -v needle="$1" 'index($0, needle) { n++ } END { print n + 0 }' "$2"
}

run_expect() { # description, expected exit code, argv...
    local label=$1 want=$2 rc=0
    shift 2
    if "$BIN" --no-color "$@" >"$OUT" 2>"$ERR"; then
        rc=0
    else
        rc=$?
    fi
    cat "$OUT" "$ERR" >"$ALL"
    if [ "$rc" -ne "$want" ]; then
        printf 'integration: FAIL [%s]: %s: exit %s, expected %s\n' \
            "$SECTION" "$label" "$rc" "$want" >&2
        dump "$ALL"
        exit 1
    fi
    note "ok: $label (exit $rc)"
}

run_ok() { # description, argv...
    local label=$1
    shift
    run_expect "$label" 0 "$@"
}

# ---------------------------------------------------------------------------
# 4. total
# ---------------------------------------------------------------------------

SECTION="total"
run_ok "total" --path "$LEDGER_ARG" total
assert_regex "total: title line names the tool" 'dshtok 1[.]0[.]0' "$OUT"
assert_regex "total: title line carries the tagline" 'DeepSeek Harness token usage' "$OUT"
assert_regex "total: source line" '^source: .*ledger[.]json' "$OUT"
assert_regex "total: headline total" "^[[:space:]]*total tokens[[:space:]]+${g_total}([^0-9]|$)" "$OUT"
assert_regex "total: input bucket" "^[[:space:]]*input[[:space:]]+${g_input}([^0-9]|$)" "$OUT"
assert_regex "total: output bucket" "^[[:space:]]*output[[:space:]]+${g_output}([^0-9]|$)" "$OUT"
assert_regex "total: cache read bucket" "^[[:space:]]*cache read[[:space:]]+${g_cache_read}([^0-9]|$)" "$OUT"
assert_regex "total: cache write bucket" "^[[:space:]]*cache write[[:space:]]+${g_cache_write}([^0-9]|$)" "$OUT"
assert_regex "total: reasoning reported separately" "^[[:space:]]*reasoning[[:space:]].*${g_reasoning}" "$OUT"
assert_regex "total: call count" "^[[:space:]]*calls[[:space:]]+${g_calls}([^0-9]|$)" "$OUT"
assert_regex "total: day count and range" "^[[:space:]]*days[[:space:]]+2[[:space:]]+$D1.*$D2" "$OUT"
assert_regex "total: cost line uses the ledger currency" "^[[:space:]]*cost[[:space:]]+.*CNY" "$OUT"
# reasoning overlaps output, so a total that added it in would be wrong; the
# fixture makes that mistake impossible to miss.
assert_no_regex "total: reasoning must not be added to the headline" \
    "^[[:space:]]*total tokens[[:space:]]+$(group_digits $((expected_total + expected_reasoning)))([^0-9]|$)" "$OUT"

SECTION="quiet"
run_ok "quiet total" -q --path "$LEDGER_ARG" total
lines=$(awk 'END { print NR }' "$OUT")
[ "$lines" -eq 1 ] || fail "quiet: expected exactly one line, got $lines"
quiet_number=$(tr -cd '0-9' <"$OUT")
[ "$quiet_number" = "$expected_total" ] \
    || fail "quiet: printed $quiet_number, expected $expected_total"

SECTION="doctor"
run_ok "doctor" --path "$LEDGER_ARG" doctor
assert_no_regex "doctor: never reports a missing ledger" 'no ledger found' "$OUT"

# ---------------------------------------------------------------------------
# 5. daily
# ---------------------------------------------------------------------------

SECTION="daily"
run_ok "daily" --path "$LEDGER_ARG" daily
assert_regex "daily: day 1 row" "^[[:space:]]*$D1[[:space:]].*${g_day1}([^0-9]|$)" "$OUT"
assert_regex "daily: day 2 row" "^[[:space:]]*$D2[[:space:]].*${g_day2}([^0-9]|$)" "$OUT"
assert_regex "daily: total row" "^[[:space:]]*total[[:space:]].*${g_total}([^0-9]|$)" "$OUT"
assert_regex "daily: total row repeats the input bucket" \
    "^[[:space:]]*total[[:space:]]+${g_input}[[:space:]]" "$OUT"

# ---------------------------------------------------------------------------
# 6. models - the provider:model keys are merged across days.
# ---------------------------------------------------------------------------

SECTION="models"
run_ok "models" --path "$LEDGER_ARG" models
assert_line_with "models: alpha merged across both days" "alpha:model-one" "$g_alpha" "$OUT"
assert_line_with "models: beta merged across both days" "beta:model-two" "$g_beta" "$OUT"
[ "$(count_lines_with "alpha:model-one" "$OUT")" -eq 1 ] \
    || fail "models: alpha:model-one should appear once, not once per day"
[ "$(count_lines_with "beta:model-two" "$OUT")" -eq 1 ] \
    || fail "models: beta:model-two should appear once, not once per day"

# ---------------------------------------------------------------------------
# 7. sessions - the same session id under both days must be merged into one row.
# ---------------------------------------------------------------------------

SECTION="sessions"
run_ok "sessions" --path "$LEDGER_ARG" sessions
[ "$(count_lines_with "shared work" "$OUT")" -eq 1 ] \
    || fail "sessions: the merged session should appear exactly once"
assert_line_with "sessions: merged row sums both days" "shared work" "$g_shared" "$OUT"
assert_line_with "sessions: merged row dates from the first day" "shared work" "$D1" "$OUT"
assert_line_with "sessions: day one only session" "day one only" \
    "$(group_digits "$alpha_only_total")" "$OUT"
assert_line_with "sessions: day two only session" "day two only" \
    "$(group_digits "$beta_only_total")" "$OUT"
assert_no_regex "sessions: unmerged halves must not appear" \
    "^[[:space:]]*[0-9]+[[:space:]].*${g_shared_day1}([^0-9]|$)" "$OUT"

SECTION="sessions-json"
run_ok "sessions json" --path "$LEDGER_ARG" sessions --json
section() { awk -v needle="$1" 'index($0, needle) { n++ } END { print n + 0 }' "$2"; }
[ "$(section '"id"' "$OUT")" -ge 1 ] || fail "sessions json: no session ids"
[ "$(section 'session-shared' "$OUT")" -eq 1 ] \
    || fail "sessions json: session-shared should be listed exactly once"
assert_regex "sessions json: merged token count" \
    "\"tokens\"[[:space:]]*:[[:space:]]*$shared_merged([^0-9]|$)" "$OUT"
assert_regex "sessions json: tool name" '"tool"[[:space:]]*:[[:space:]]*"dshtok"' "$OUT"

# ---------------------------------------------------------------------------
# 8. --json on total, daily and models
# ---------------------------------------------------------------------------

SECTION="total-json"
run_ok "total json" --path "$LEDGER_ARG" total --json
first=$(head -c 1 "$OUT" 2>/dev/null || printf '')
[ "$first" = "{" ] || fail "total json: output does not start with '{'"
assert_regex "total json: tool name" '"tool"[[:space:]]*:[[:space:]]*"dshtok"' "$OUT"
assert_regex "total json: version" '"version"[[:space:]]*:[[:space:]]*"1[.]0[.]0"' "$OUT"
assert_regex "total json: source" '"source"[[:space:]]*:[[:space:]]*".*ledger[.]json"' "$OUT"
assert_regex "total json: currency" '"currency"[[:space:]]*:[[:space:]]*"CNY"' "$OUT"
assert_regex "total json: range object" '"range"[[:space:]]*:' "$OUT"
assert_regex "total json: tokens" '"tokens"[[:space:]]*:[[:space:]]*'"$expected_total"'([^0-9]|$)' "$OUT"
assert_regex "total json: input" '"input"[[:space:]]*:[[:space:]]*'"$expected_input"'([^0-9]|$)' "$OUT"
assert_regex "total json: output" '"output"[[:space:]]*:[[:space:]]*'"$expected_output"'([^0-9]|$)' "$OUT"
assert_regex "total json: cacheRead" '"cacheRead"[[:space:]]*:[[:space:]]*'"$expected_cache_read"'([^0-9]|$)' "$OUT"
assert_regex "total json: cacheWrite" '"cacheWrite"[[:space:]]*:[[:space:]]*'"$expected_cache_write"'([^0-9]|$)' "$OUT"
assert_regex "total json: calls" '"calls"[[:space:]]*:[[:space:]]*'"$expected_calls"'([^0-9]|$)' "$OUT"

SECTION="daily-json"
run_ok "daily json" --path "$LEDGER_ARG" daily --json
assert_regex "daily json: day one entry" '"date"[[:space:]]*:[[:space:]]*"'"$D1"'"' "$OUT"
assert_regex "daily json: day two entry" '"date"[[:space:]]*:[[:space:]]*"'"$D2"'"' "$OUT"
assert_regex "daily json: day one tokens" "\"tokens\"[[:space:]]*:[[:space:]]*$day1_total([^0-9]|$)" "$OUT"
assert_regex "daily json: day two tokens" "\"tokens\"[[:space:]]*:[[:space:]]*$day2_total([^0-9]|$)" "$OUT"

SECTION="models-json"
run_ok "models json" --path "$LEDGER_ARG" models --json
assert_regex "models json: alpha name" '"name"[[:space:]]*:[[:space:]]*"alpha:model-one"' "$OUT"
assert_regex "models json: beta name" '"name"[[:space:]]*:[[:space:]]*"beta:model-two"' "$OUT"
assert_regex "models json: alpha tokens" '"tokens"[[:space:]]*:[[:space:]]*'"$alpha_total"'([^0-9]|$)' "$OUT"
assert_regex "models json: beta tokens" '"tokens"[[:space:]]*:[[:space:]]*'"$beta_total"'([^0-9]|$)' "$OUT"

# ---------------------------------------------------------------------------
# 9. Range filters
# ---------------------------------------------------------------------------

SECTION="since"
run_ok "total --since day 2" --path "$LEDGER_ARG" --since "$D2" total
assert_regex "since: only the second day counts" \
    "^[[:space:]]*total tokens[[:space:]]+${g_day2}([^0-9]|$)" "$OUT"
assert_no_regex "since: the filtered out day must not leak in" "${g_total}([^0-9]|$)" "$OUT"

SECTION="until"
run_ok "total --until day 1" --path "$LEDGER_ARG" --until "$D1" total
assert_regex "until: only the first day counts" \
    "^[[:space:]]*total tokens[[:space:]]+${g_day1}([^0-9]|$)" "$OUT"
assert_no_regex "until: the filtered out day must not leak in" "${g_total}([^0-9]|$)" "$OUT"

SECTION="range"
run_ok "total --since --until one day" --path "$LEDGER_ARG" --since "$D2" --until "$D2" total
assert_regex "range: single day window" \
    "^[[:space:]]*total tokens[[:space:]]+${g_day2}([^0-9]|$)" "$OUT"

# ---------------------------------------------------------------------------
# 10. Ledger discovery through the environment, and the documented exit codes.
# ---------------------------------------------------------------------------

SECTION="discovery"
DSHTOK_LEDGER=$LEDGER_ARG
export DSHTOK_LEDGER
run_ok "DSHTOK_LEDGER is honoured" total
unset DSHTOK_LEDGER
assert_regex "discovery: reads the ledger from DSHTOK_LEDGER" \
    "^[[:space:]]*total tokens[[:space:]]+${g_total}([^0-9]|$)" "$OUT"

SECTION="exit-codes"
run_expect "missing ledger is a runtime error" 1 --path "$WORK/no-such-ledger.json" total
# An explicit --path is reported by the name it was given. The "no ledger found"
# wording belongs to the discovery path, where no single file was named; that
# one is covered by the unit tests because it needs a cleared environment.
assert_regex "missing ledger: names the file it could not open" 'cannot open file' "$ALL"
run_expect "unknown command is a usage error" 2 frobnicate
run_expect "unknown option is a usage error" 2 --definitely-not-an-option total
# A date that is not a date is a usage mistake rather than missing data: option
# values are validated before the ledger is opened.
run_expect "invalid --since value is a usage error" 2 --path "$LEDGER_ARG" --since nonsense total
run_expect "impossible --since date is a usage error" 2 --path "$LEDGER_ARG" --since 2026-02-30 total
run_ok "--version" --version
assert_regex "--version: prints the version" '1[.]0[.]0' "$OUT"
run_ok "--help" --help
assert_regex "--help: lists the commands" 'sessions' "$OUT"
run_ok "help command" help

# ---------------------------------------------------------------------------
# 11. csv is checked lightly: it must work and must not crash.
# ---------------------------------------------------------------------------

SECTION="csv"
run_ok "daily csv" --path "$LEDGER_ARG" daily --csv
assert_regex "daily csv: header mentions the date column" 'date' "$OUT"
assert_regex "daily csv: day one row" "$D1" "$OUT"
assert_regex "daily csv: day two row" "$D2" "$OUT"

SECTION="done"
note "all checks passed (headline total $g_total, from $expected_total tokens)"
exit 0
