# The command line

```
zxconfig [options] [file]
```

`file` is the root manifest; the default is `./ZXConfig`.  Everything the
interface does is reachable from here, so CI never needs a terminal.

## Options

| Option | Effect |
| --- | --- |
| `-f, --file <path>` | root manifest (default `./ZXConfig`) |
| `--root <path>` | directory includes may not escape (default: the manifest's directory) |
| `--preset <path>` | apply a preset through `presets_dir` search paths; repeatable |
| `--set <N>=<V>` | set a symbol; repeatable |
| `--unset <name>` | return a symbol to its default; repeatable |
| `--save [path]` | write the cache after applying inputs (default: the cache; else `path`) |
| `--generate` | run the manifest's `generate` directives and exit |
| `--force` | proceed past validation errors |
| `--cache-force` | apply a stale cache, keeping what still matches |
| `--parse-only` | parse and report, then exit |
| `--dump-tokens` | print the token stream of the root file and exit |
| `--headless` | never open the interface |
| `-v, --verbose`, `--trace` | record at debug / trace level |
| `-q, --quiet` | record failures only |
| `--log <path>` | write the log to a file instead of standard error |
| `--color <when>` | `always`, `never`, or `auto` (default) |
| `--version`, `-h, --help` | version and help |

## Inputs apply in order

Presets, sets, and unsets share one ordered list and are applied exactly as
written — they are editing steps, not a batch:

```bash
zxconfig --preset ci --set THREADS=32 --unset SMP --set SMP=y --generate
```

The full precedence is: **defaults → cache → inputs in command-line order**.
The last word wins.  `--set` accepts the same text the interface editor
accepts (`SMP=y`, `NR_CPUS=16`, `ARCH=arm64`, `DEBUG=kasan,lockdep`); a bad
value fails loudly with exit 1 rather than continuing.

## Validation gates writing

`error` clauses are the hard errors: `--generate` and `--save` refuse while one
stands, per the language specification.  Fix it or pass `--force` when
regeneration is the point.  `warn` clauses never block.  A refused `--set`
(that would leave a hidden selection or violate a constraint) fails atomically
and leaves no partial preset behind.

## Examples

```bash
# CI: regenerate every output from a checked-in preset
zxconfig --preset ci --generate

# ad-hoc overrides
zxconfig --set NR_CPUS=16 --set ARCH=arm64 --generate

# author a preset without the interface
zxconfig --preset ci --set THREADS=32 --save presets/big.zxconfig

# report for CI logs; exit code gates the pipeline
zxconfig --parse-only

# deliberate regeneration despite a failing rule
zxconfig --generate --force
```

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | success |
| 1 | parse or evaluation failure, a refused `--set`/`--unset`, validation errors blocked a write, or a generator failed |
| 2 | usage error |
| 4 | the interface failed internally; the terminal is restored |
