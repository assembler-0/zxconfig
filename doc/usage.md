# Usage and behavior

## Quick start

```bash
cd demo && ../build/zxconfig      # the demo project
```

The root manifest is `./ZXConfig` unless `-f` names another.  The demo
carries 16,755 symbols across 2,622 menus: every type, shared macros,
file namespaces, probes, validation, and five generators. See the
[stress laboratory](../demo/README.md) for workloads and benchmarks.

## The pipeline

Every run walks the same steps:

1. **Load** the manifest and its includes (confined to the manifest's
   directory unless `--root` says otherwise).
2. **Parse** into the tree; errors here stop the run and are reported by
   file, line, and column.
3. **Evaluate**: defaults (including conditional and computed ones),
   visibility, then the validation clauses.
4. **Reconcile the cache** — see [cache.md](cache.md).
5. **Apply inputs** in command-line order: presets, `--set`, `--unset`.
6. **Write** if asked: `--save`, `--generate`, or the interface's `s`.

## Behavior rules worth knowing

- **`error` clauses block writing.**  `--save` and `--generate` refuse while
  one stands (`--force` overrides); the interface's `s` refuses with the
  report one key away.  `warn` clauses never block.  Clauses fire when their
  condition is TRUE; a condition that fails to evaluate is itself an error.
- **Hiding a selection is refused.**  A value that picks a choice member
  whose `choice ... visible` condition is false is invalid.  A write that
  would hide the current selection fails with a diagnostic naming the hidden
  member; restore the dependency or change the selection first.
- **`implies` prompts, it never forces.**  Turning a symbol on with an
  unsatisfied `implies` raises a suggestion: the alternatives, or Skip.  On
  the command line the suggestions are logged as warnings.
- **`conflict` decides the fate.**  `resolve "…"` supplies the prompt text;
  `error "…"` blocks the save until resolved.
- **Aliases resolve everywhere** — in expressions, in cache loads, and on
  the command line.  **Deprecated names migrate** on load with their
  declared message.
- **Values are validated on entry**: range, pattern, choices, and path
  constraints.  A refused edit says why and changes nothing.

## The interface

```
┌ header ── title, state, err / warn / set counters ──────────────┐
│ configuration                     │ details                     │
│ [-] Architecture                  │ NR_CPUS                     │
│     [*] Symmetric multipro…   …   │ Max CPU count               │
│     …                             │ … type, default, range …    │
│                                   │ exports NR_CPUS             │
│                                   │ from Zconfig -> arch.zx:4   │
├ [ ERROR ] first finding ─────────────────────────────────────────┤
│ log ────────────────────────────────────────────────────────────│
├ keys ───────────────────────────────────────────────────────────┤
```

| Key | Action |
| --- | --- |
| `space` | toggle boolean or tristate (cycles n → m → y) |
| `enter` / double-click | edit; chooser for enum and set |
| `left` / `right`, `h` / `l` | collapse / expand |
| `r` | reset symbol to its default |
| `s` | save the cache and run the generators |
| `c` | apply a refused cache (stale or broken) |
| `p` | load a preset |
| `/` | find symbols (fuzzy; browses before you type) |
| `v` | validation report |
| `d` | symbol details |
| `?` | help |
| `[` / `]` | scroll the log |
| `q` | quit — asks when there are unsaved changes |

Text fields take every character, `q` and `j` and `k` included; the arrows
move the selection there.  In the tree, `j`/`k` navigate.  Popups are closed
with `Esc` or their buttons.

The mouse works: click to select, click `[+]`/`[-]` to toggle a menu, wheel
to scroll, double-click to edit, click buttons.

### State, plainly

The header's `state` word is `ready`, `modified`, or `saved`.  The finding
strip appears above the log whenever an `error` or `warn` clause fires;
counters in the header are lit chips when non-zero.  The enum and set
choosers only offer alternatives whose choice conditions are true.  The
details panel
always shows the selected symbol's label, help, type, value, default,
constraints, its name in the generated outputs (`exports`), and where it
came from (`from`, with the full include chain).

### Prompts

`implies` and `conflict` raise a **suggestion** dialog: the candidate fixes,
or Skip.  Applying a fix marks the state unsaved like any other edit.

## Recipes

```bash
# CI: regenerate outputs from a preset, nothing else
zxconfig --preset ci --generate

# interactive, starting from a preset
zxconfig --preset ci

# change the stress demo's memory budget, then keep the result
zxconfig --set BenchMemoryMiB=16384 --save --generate

# what changed between two presets? they are plain text
diff presets/ci.zxconfig presets/big.zxconfig
```

## See also

- [cli.md](cli.md) — every flag and exit code
- [cache.md](cache.md) — cache lifecycle and presets
- [language.md](language.md) — the language
