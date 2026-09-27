# The cache and presets

Two file kinds, one format.  Both are plain text and safe to diff.

- **The cache** — `.zxconfig.cache`, beside the manifest.  A snapshot of the
  chosen values, stamped with a digest of the manifest set.
- **A preset** — any path you name (`presets/ci.zxconfig` by convention).  The
  same format; its digest is ignored, because a preset is policy, not a
  snapshot.

## Format

```
zxcache 1
writer zxconfig 26h2
manifest "/home/me/project/ZXConfig"
digest 6b2c…64 hex digits…
set NR_CPUS int "16"
set arch.BOOT_PROTOCOL enum "legacy"
```

One `set` per assignment: symbol name, declared type, quoted value.  `#`
starts a comment.  Assignments are written in symbol order, so an unchanged
configuration produces a byte-identical file.  Writes are atomic: a temporary
sibling is renamed over the destination, so a reader never sees a partial
file.  A `.lock` sibling carries the advisory lock held during saves.

An empty or blank file reads as *absent* — clearing the cache is how you
start over.  Lock files are never offered as presets.

## The lifecycle

The cache is compared against a digest of the manifest set — every file, in
load order.  The verdict decides what happens:

| Verdict | Meaning | Default behavior |
| --- | --- | --- |
| absent | no cache, or an empty one | start from defaults |
| clean | digest matches | apply every assignment |
| stale | the manifest changed | **not applied**; press `c` in the interface, or run with `--cache-force`, to keep the assignments whose symbols still exist |
| broken | unreadable | **not applied**; the message names the line and the reason |

Every refusal says why.  A stale cache is rescuable; a broken one has nothing
to rescue and the engine starts from defaults.

## Priority

**defaults → cache → presets → `--set`/`--unset`**, later wins on overlap.
This is the same rule as editing in the interface: a preset beats the cache
where they collide, and the last command-line word beats the preset.  A
preset does *not* replace state wholesale — symbols it does not mention keep
their values.

For deterministic CI, keep the cache out of version control (it is
gitignored here).  A clean workspace then has no cache, and the preset is the
whole input.

## Presets

A preset is written from the command line or the interface:

```bash
zxconfig --set RAM_SIZE=2048 --save presets/ci.zxconfig   # from a script
# …or configure in the interface and press s, then copy .zxconfig.cache
```

and read back anywhere a value can come from:

```bash
zxconfig --preset ci --generate          # bare names resolve through presets/
```

In the interface, `p` opens a finder over the manifest's `presets/`
directory; a bare name resolves `presets/<name>`, `presets/<name>.zxconfig`,
then the manifest's own directory.  Loading a preset marks the state unsaved
— it no longer matches the cache — so `s` afterwards persists it.

Deprecated names migrate on every load: an assignment under an old name is
rewritten to its replacement with the message from the manifest's
`deprecated` block, and when a file holds both spellings the new one wins and
the clash is reported.
