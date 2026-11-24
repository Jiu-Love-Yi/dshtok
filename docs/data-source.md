# Where the numbers come from

`dshtok` does not measure anything itself. It reads one JSON file that another program
maintains, and reports what that file says using rules written down here. This page is
the full derivation: what the file contains, which fields are authoritative, why the
arithmetic is what it is, and what can make the result wrong.

If you only remember one thing: **`dshtok` reports one user's ledger, not the world's
truth.** It is as right as the ledger is, and the ledger is as right as the plugin that
wrote it.

## The source: `dsh-cost-meter`

The ledger is written by **dsh-cost-meter**, a third-party plugin for DeepSeek Harness,
MIT licensed, maintained at <https://github.com/Han-1413141/dsh-cost-meter> (the version
this documentation was written against is 1.9.33). `dshtok` is not affiliated with that
project, does not bundle it, and does not patch it. It is a plain downstream reader of a
file the plugin produces for its own purposes.

That distinction matters for support: a wrong number is a data question (the plugin's
ledger) or a reporting question (`dshtok`'s arithmetic), and this page is written so you
can tell which one you are looking at.

Default location:

```text
$DSH_HOME/storages/cost-meter/ledger.json
```

`$DSH_HOME` defaults to `%USERPROFILE%\.dsh` on Windows and `~/.dsh` on Linux and macOS.
If `$DSH_HOME` is unset but `$DSH_PROFILE_DIR` is set — it points at a profile directory,
`.../profiles/<name>` — the home is taken to be the parent of that `profiles` component,
which is how the host derives it too. A DSH profile can also keep its own storage under
`$DSH_HOME/profiles/<profile>/storages/cost-meter/ledger.json`; `dshtok` probes that
form too. Override everything with `$DSHTOK_LEDGER` or `--path FILE`.

On Linux and macOS, when neither `HOME` nor `USERPROFILE` resolves, one last fallback
tries `/home/$USER/.dsh/...`, because containers, cron jobs and WSL shells can all run
with a stripped environment while the real home directory still holds `.dsh`. The `USER`
value is refused if it is empty, contains a separator, or is `.`/`..`, so it cannot become
a path outside `/home`. Windows has no such fallback.

A machine can have more than one such file — one per DSH home or profile. `dshtok`
reads exactly one of them per run, the first that exists, and tells you which in its
header line and in `--json`'s `source` field. There is no multi-ledger aggregation.

## File shape

The file is one JSON object. Its top level (as observed in a current ledger) is:

| Key | Role |
| --- | --- |
| `version` | ledger format version; currently `1` |
| `config` | currency, symbol, decimal places, exchange rate, `historyDays` |
| `days` | **the only key `dshtok` trusts for totals** |
| `usageCursors` | bookkeeping cursors over the raw usage cache |
| `writeReceipts`, `balanceRef`, `migrations` | plugin-internal state |
| `planSamples`, `planHourBuckets` | plan-quota sampling for the plugin's own charts |
| `pricingHistory`, `openrouterPriceHashes` | price table bookkeeping |
| `historyReset` | marker for a history wipe |

`days` is an object keyed by date. The shape below is illustrative — the numbers are made
up to show which fields sit next to each other, not to be realistic:

```json
{
  "days": {
    "2026-11-23": {
      "input": 1234,
      "output": 567,
      "cacheRead": 89012,
      "cacheWrite": 0,
      "reasoning": 34,
      "calls": 5,
      "cost": 1.03,
      "apiCost": 1.02,
      "costCny": 6.88,
      "apiCostCny": 6.81,
      "byProviderModel": { "opencode-go:deepseek-v4.1-flash": { "...": "..." } },
      "sessions": [
        { "id": "session-...", "at": 1795363250489, "input": 900,
          "output": 300, "cacheRead": 80000, "cacheWrite": 0,
          "reasoning": 20, "calls": 3, "cost": 0.9, "apiCost": 0.9 }
      ]
    }
  }
}
```

Each day carries the ten token/cost fields above, a `byProviderModel` map keyed
`provider:model`, and a `sessions` array. Session entries repeat the same numeric fields
flat, next to `id`, `at` (a millisecond timestamp) and usually `title`; they also carry
their own `byProviderModel`. A current ledger additionally has `costEur` / `apiCostEur`
per day and per session; `dshtok` ignores currency variants it does not display.

The exact per-day keys `dshtok` reads are: `input`, `output`, `cacheRead`, `cacheWrite`,
`reasoning`, `calls`, `cost`, `apiCost`, `costCny`, `apiCostCny`, `byProviderModel`,
`sessions`. A missing numeric field is treated as `0`; a number may be written as an
integer or with a fractional part; unknown fields are ignored. That tolerance is
deliberate — it lets a newer plugin add fields without breaking this tool.

## The four token buckets

| Field | Meaning |
| --- | --- |
| `input` | prompt tokens that missed the prompt cache and were billed as fresh input |
| `output` | tokens the model generated |
| `cacheRead` | prompt tokens served from the provider's prompt cache |
| `cacheWrite` | prompt tokens written into that cache |

The headline total is:

```text
total = input + output + cacheRead + cacheWrite
```

### Why the three prompt buckets can be added

Because they are mutually exclusive. Every prompt token on the wire is exactly one of:
a cache read, a cache write, or a plain input token. It is never two of them, so adding
them cannot count a token twice.

From the plugin's source, `lib/pricing.js`:

> 宿主的 input/cacheRead/cacheWrite 是互斥桶；输入长度包含缓存，输出不参与阈值。

(Translation: the host's `input` / `cacheRead` / `cacheWrite` are mutually exclusive
buckets; the input length already includes cached tokens; output does not participate in
the threshold.) The same function, `costOf`, prices a request as
`(input * cacheMiss + output * output + cacheRead * cacheHit + cacheWrite * cacheWritePrice
+ reasoning * reasoningPrice) / 1e6`, where the reasoning term applies only when the price
table defines a separate reasoning rate. That sum is only meaningful if the buckets are
disjoint.

### Why `reasoning` is not added

Reasoning tokens are shown by the plugin as a category of their own, but they can overlap
`output`: they are part of what the model generated. Adding them to the total would
therefore count some tokens twice, so `dshtok` leaves them out.

From the plugin's own documentation (`docs/billing-statistics.md`):

> Cache hit rate is `cacheRead / (input + cacheRead + cacheWrite)`. ... Reported reasoning
> tokens can overlap output and are shown separately; the configured reasoning rate
> determines any additional charge.

`dshtok` therefore prints reasoning on its own line as a share **of output**
(`reasoning / output`), never as a share of the total.

### What the total means, and does not mean

It is a token count: how much text passed through the models. It is not a bill.

- Cache reads are far cheaper than fresh input, so a workload that is 96% cache reads
  can be 96% of the tokens and a small fraction of the cost. The two are different
  questions and `dshtok` reports both.
- The `cost` and `apiCost` fields come from the plugin's price table, which models
  plan-based and API-based billing separately. If you need a billing figure, read
  `cost` / `apiCost` (and their CNY variants) — do not estimate it from tokens.

## Why only `days[]`

Two other data sources exist on disk. `dshtok` reads neither, on purpose.

### Not the raw usage cache

`ledger.json.usage-cache/*.json` (a directory that lives next to the ledger) contains
records of individual log events — `session`, `turn/start`, `step/start`,
`request/header`, `assistant/message`, `tool/call`, `turn/end`. Summing the
`assistant/message` usage records looks like an independent way to compute the same
total, and the plugin uses those records for its own reconciliation.

They are not a complete record, though: the cache is cursor-driven and pruned, so a
record can be summarized away once a later event supersedes it. Measured on one real
machine, the `assistant/message` usage records summed to about **2.5 billion tokens**,
while the ledger for the same period totalled about **5.2 billion** — the raw events
accounted for only about **48%** of the ledger's own number. The ledger keeps growing as
usage accrues, so both figures drift and the ratio drifts slowly downward; the gap itself
is the point, not the exact numbers.

A tool that reported 48% of the truth would be worse than useless, because the error is
systematic and invisible: no warning, no gap, just a plausible-looking smaller number.
So `dshtok` does not use this path at all.

### Not the session logs

`$DSH_HOME/sessions/**/session.v4.jsonl.zstd` holds the full transcript of every session,
and it can be decoded — the plugin does it with Node's `zlib.zstdDecompressSync`
(which needs Node 22.15+). Two reasons `dshtok` stays away:

1. **zstd is not in the C standard library.** Supporting it would mean vendoring a
   decompressor — thousands of lines of third-party C — which contradicts the project's
   one-file, zero-dependency premise.
2. **It would duplicate a billing engine.** Turning turns into tokens means re-deriving
   the plugin's deduplication rules, cache pricing, plan-versus-API split and currency
   handling, in a second implementation that would drift from the first. Two programs
   that disagree about the same bill is worse than one program that reads the bill.

The ledger is already the plugin's reconciled answer to that question. Reading it is
cheap, exact, and cannot drift.

### The trade-off, stated plainly

`dshtok` inherits whatever the plugin recorded. If the plugin misclassified a field, the
number is wrong here too, with no independent check to catch it. That is the accepted
cost of not reimplementing the plugin. In exchange, the number is trivially auditable:
one file, open it, add four fields, compare.

## Aggregation rules

These follow the frozen contract in `SPEC.md`:

- Only `days` contributes to totals. A day's top-level numbers are authoritative; the
  `byProviderModel` and `sessions` breakdowns are used for their own views, never to
  recompute a day total.
- Days are iterated in `YYYY-MM-DD` string order, which is chronological order.
- **Sessions merge by id.** The same session id can appear under several days when a
  session spans midnight or is resumed. `dshtok` merges those entries into one row and
  takes the title from any day that has one. On one real ledger, 199 day-level session
  entries collapsed to 150 distinct sessions, 23 of which appeared on more than one day.
- `byProviderModel` is keyed `provider:model` and merged across days by that key.
- `--since` / `--until` filter days inclusively before anything is aggregated, so a
  session partially inside the range contributes only the in-range days.

## What could make the number wrong

In rough order of how often it happens:

1. **Retention truncation.** The plugin keeps a bounded number of days, controlled by
   `config.historyDays` (default 180, clamped to 7..3650); older days are deleted from
   the ledger when a new day is added. A long-lived install silently reports a smaller
   lifetime total. If your total looks low, check `config.historyDays` and the date range
   in the header — the header always shows the first and last day present.
2. **A different DSH home or profile.** Each home has its own ledger. Reading a
   freshly-created profile shows its own (probably empty) history rather than the one you
   have been using. `dshtok doctor` prints the candidate list and which path won.
3. **The plugin was installed late, or not at all.** Nothing recorded before
   installation exists. There is no backfill.
4. **A history reset.** The ledger carries a `historyReset` marker; if the plugin was
   made to forget its history, the days are simply gone.
5. **Provider-side reporting quirks.** Cache write accounting in particular is not
   uniform across providers; a provider that does not report cache writes will show `0`
   there even if caching happened.
6. **Reasoning double-count confusion.** If you compute a total yourself and add
   `reasoning`, you will overstate it relative to `dshtok`. That is a bug in your
   arithmetic, not in the ledger — see the quote above.
7. **A ledger being written while it is read.** The plugin debounces writes (about two
   seconds), but a read that races a write could see a truncated file, which surfaces as
   a JSON parse error with a line and column, not as a silently low number.

What is *not* a source of error: rounding. Totals are summed as doubles over per-day
counts and printed exactly. Percentages are rounded for display only; `--json` and
`--csv` carry the same underlying numbers.

## Verifying a number yourself

The whole point of reading one file is that you can check it. Sum the four buckets with
any JSON tool and compare the result with `dshtok -q total`:

```sh
python -c "import json; d = json.load(open('ledger.json', encoding='utf-8'))['days']; print(sum(v.get('input', 0) + v.get('output', 0) + v.get('cacheRead', 0) + v.get('cacheWrite', 0) for v in d.values()))"
```

Pass `encoding='utf-8'` on Windows: the default text encoding there is often a legacy
code page, and the ledger contains non-ASCII session titles.

They should agree exactly. If they do not, one of the two is reading a different file;
check `dshtok doctor` and the `source` field of `--json`.
