# The zxconfig format

## Schema declaration

Every zxconfig file should open with a version header so tooling can handle format evolution gracefully.
```c
zxconfig 1.1
```

Note: The current format is `1.1`. Files declaring `1.0` still parse and receive a
migration diagnostic; version numbers without a minor (`zxconfig 1`) are
rejected as a syntax error. This loader treats no other version as prose.

---

## Basic types and declaration

Every symbol is declared with a type, an optional default, and metadata in a trailing block.
```c
# Bool
option SMP : bool = false {
    label "Symmetric multiprocessing"
    help  "Enable support for multiple CPU cores."
    tags  ["smp", "cpu", "arch"]        # freeform search labels
}

# Tristate — n (off), m (module), y (built-in)
option EXT4 : tristate = m {
    label "ext4 filesystem support"
    help  "n=excluded, m=loadable module, y=built-in."
}

# Integer with range constraint
option NR_CPUS : int = 4 {
    label "Max CPU count"
    range 1 .. 512
    when  SMP
}

# Unsigned integer and hex
option PAGE_SIZE  : uint = 4096    { range 512 .. 65536 }
option BASE_ADDR  : hex  = 0xFFFF0000 { label "Boot base address" }

# String with validation pattern
option KERNEL_VERSION : string = "6.1.0" {
    label   "Kernel version string"
    pattern "^\d+\.\d+\.\d+"
}

# Path — string with filesystem-specific validation
option SYSROOT : path = "/usr/sysroot" {
    label    "Sysroot directory"
    path     must_exist | is_dir          # constraint flags
}

# Typed list
option EXTRA_CFLAGS : list<string> = [] {
    label "Extra C compiler flags"
}

# Enum
option ARCH : enum = x86_64 {
    label   "Target architecture"
    choices x86_64 | arm64 | riscv64 | loongarch
}
```

---

## Visibility vs mutability

`when` previously conflated two distinct concepts. They are now split:

- `visible` — whether the symbol appears in the UI (hides when false)
- `enabled` — whether the symbol is editable (grays out when false; value still participates in expressions)
- `when` remains as a shorthand for `visible` (backward compatible)
```c
option NUMA : bool = false {
    label   "NUMA support"
    visible SMP               # hidden unless SMP
    enabled NR_CPUS >= 4      # grayed out if too few CPUs, but still exists
}
```

---

## Grouping — menu

Menus create nested hierarchy with their own visibility condition. Options inside inherit their parent's `visible` constraint automatically.
```c
menu "Memory management" {
    option SPARSEMEM : bool = false {
        label "Sparse memory model"
    }

    option SPARSEMEM_VMEMMAP : bool = true {
        label   "Use vmemmap for pfn_to_page"
        visible SPARSEMEM
    }

    menu "NUMA" visible SMP {
        option NUMA : bool = false {
            label "Non-uniform memory access"
        }
    }
}
```

### UI ordering

By default the UI renders options in declaration order. Override with `order`:
```c
option DEBUG : bool = false {
    label "Debug mode"
    order 99    # pushed toward the bottom
}
```

---

## Dependencies

`when`/`visible` use natural boolean expressions. `implies` is a soft suggestion — it prompts the user rather than silently forcing a value. When two `implies` conflict, the resolution policy is explicit.
```c
option VIRTIO_NET : bool = false {
    label   "VirtIO network driver"
    visible VIRTIO and NET
    implies VIRTIO_RING
    implies VIRTIO_PCI or VIRTIO_MMIO   # user picks one if neither is set
}

option EXPERT : bool = false {
    label "Expose expert/dangerous options"
}

option KALLSYMS_ALL : bool = false {
    label   "Include all symbols in kallsyms"
    visible KALLSYMS and EXPERT
}
```

### Conflict resolution

When two `implies` targets contradict each other, declare an explicit resolver:
```c
option FORCE_PREEMPT : bool = false {
    label   "Force full preemption"
    implies PREEMPT_MODEL == full
    conflict PREEMPT_MODEL {
        resolve "FORCE_PREEMPT requires full preemption — override PREEMPT_MODEL?"
        # "error" would hard-fail; default is "prompt"
    }
}
```

---

## Deprecation and aliasing

Mark renamed or removed symbols so tooling can migrate old `.config` files gracefully.
```c
# Symbol was renamed; old name still parses, but tools warn and rewrite
deprecated SLAB_DEBUG -> SLUB_DEBUG {
    message "SLAB_DEBUG was renamed to SLUB_DEBUG in zxconfig 1.1"
}

# Stable public alias for an internal name
alias NET_DRIVER_VIRTIO = VIRTIO_NET
```

Alias and deprecation targets resolve like ordinary expressions: the
declaring file's namespace first, then global symbols. The canonical name is
what tooling uses, so a namespaced file can alias a global symbol and the
alias is reachable under its own qualified name. Unknown targets are parse
errors. The deprecated *old* name is a legacy cache spelling: it is kept
exactly as written and never validated against declared symbols.

---

## Choice group
```c
# Exclusive (radio)
option PREEMPT_MODEL : enum = voluntary {
    label   "Preemption model"
    choices none | voluntary | full | realtime
    choice none      { label "No preemption (server/HPC)" }
    choice voluntary { label "Voluntary (desktop default)" }
    choice full      { label "Full preemption" }
    choice realtime  { label "PREEMPT_RT (low latency)"; visible EXPERT }
}

# Non-exclusive (checkboxes)
option DEBUG_FEATURES : set = {} {
    label   "Debug feature flags"
    choices lockdep | kasan | ubsan | kcsan
}
```

### Hidden choices are invalid values

A `choice` block may carry a `visible` condition, exactly like a symbol. A
member whose condition is false is *hidden*: the interface does not offer it,
and — more importantly — a value that currently selects it is invalid. When a
dependency collapses and hides the selected member, the whole configuration
fails to evaluate with a diagnostic naming the symbol and member; a dependent
write (aiming to hide it) is refused, and the user must either restore the
dependency or change the selection first. This mirrors dependency rules in
Kconfig-style systems but fails loudly instead of silently coercing.

```c
option EXPERT : bool = false
option PREEMPT_MODEL : enum = voluntary {
    choices none | voluntary | full | realtime
    choice realtime { visible EXPERT }
}
# With EXPERT = false and PREEMPT_MODEL forced to realtime, evaluation fails:
# 'realtime' is hidden for 'PREEMPT_MODEL'
```

---

## Conditional defaults
```c
option HZ : int {
    label  "Timer frequency (Hz)"
    range  100 .. 1000
    default {
        1000 when PREEMPT_MODEL == realtime
        250  when PREEMPT_MODEL == full
        100
    }
}
```

---

## Computed (read-only) symbols

Derived values the user cannot edit. Now supports conditional branches matching the `default {}` syntax for consistency.
```c
computed HAVE_KVM : bool = ARCH == x86_64 or ARCH == arm64

# Conditional computed
computed MAX_IRQ : int {
    1024 when ARCH == arm64
    256  when ARCH == riscv64
    512
}

computed BIN_DIR : string = "${PREFIX}/bin"
computed LIB_DIR : string = "${PREFIX}/lib/${ARCH}"
```

---

## Includes and namespacing

Project policy is declared at the root manifest's top level:

```c
zxconfig 1.1
export_with_prefix "CONFIG_"
presets_dir "profiles"
presets_dir "vendor/profiles"
presets_dir ""
```

`export_with_prefix` is optional and may occur once. The default is `""`.
It prepends a literal prefix to flat exported names in headers, Makefiles,
CMake, and JSON: `arch.ArTargetCpu` exports as `CONFIG_ArTargetCpu`.
Declaration spelling and case are preserved, as are generated suffixes
such as `_s390x` and `_MODULE`. Internal names, cache assignments, and
template `${arch.ArTargetCpu}` references are unaffected. Prefixes must be
empty or match `[A-Za-z_][A-Za-z0-9_]*`.

`presets_dir` is repeatable. Entries are searched in declaration order and
replace the default `presets/` directory when any are provided. Relative
paths resolve against the root manifest's directory; absolute paths are
accepted, and `""` means that directory itself. For each search directory,
lookup tries the supplied name and then the name with `.zxconfig` appended.
The manifest directory remains a final fallback. Absolute preset filenames
are used directly. These are literal paths, without `${...}` interpolation.

The TUI lists `.zxconfig` files from configured directories, sorted within
each directory in search order. It loads the exact selected file, so two
directories may both contain `ci.zxconfig` without selecting the wrong one.
Missing directories are ignored; lock files and directories are not listed.
Other preset extensions remain loadable through an explicit name or path.

Both directives are project-wide regardless of where they appear in the
root's declaration order. Included files, menus, and macro expansions
cannot change project policy. A per-header `prefix "CUSTOM_"` remains an
explicit override, including `prefix ""` to suppress the project prefix for
that header only. This is replacement, not prefix concatenation.

The defining file owns its namespace. Put one optional `namespace` declaration
immediately after its version header. Without it, the file declares global
symbols. Namespaces are case-sensitive and may contain dotted segments.

```c
# arch/arch.zx
zxconfig 1.1
namespace arch
option ArTargetCpu : enum = s390x { choices s390x | arm64 }
computed ArIsS390 : bool = ArTargetCpu == s390x
```

Inside this file `ArTargetCpu` resolves to `arch.ArTargetCpu`. Other files
reference `arch.ArTargetCpu` explicitly. Unqualified references first search
their defining file's namespace, then global symbols. An include does not
inherit its importer's namespace. The removed `include ... as ...` syntax
reports a migration error.

```c
# ZXConfig
zxconfig 1.1
include "arch/arch.zx"
include "s390x/s390x.zx" visible arch.ArTargetCpu == s390x
```

Every included file must exist and have a version header, even if its
condition is false. Files are parsed once; the condition decides whether
the include is *active*, re-evaluated whenever a referenced value changes.
An inactive include is structurally present but semantically absent: its
symbols cannot be read or written, are not evaluated, are not exported by
any backend, are not listed in the TUI, and their `warn`/`error` clauses do
not fire. Values recorded while it was active are dropped when it
deactivates. When one file is reached on several include edges it is active
if any edge's condition holds; the root manifest is always active.
Editing the architecture re-evaluates visibility and redraws the TUI
without re-parsing. A bare `visible arch.ArTargetCpu` checks whether the
value is nonempty; use `== s390x` to choose one architecture.

Internal identities and cache assignments use `arch.ArTargetCpu`. Generated
headers, Makefiles, CMake files, and JSON use the flat declaration spelling
`ArTargetCpu`, preserving case and adding no implicit `CONFIG_` prefix.
Use `export_with_prefix` for a common prefix across backends, or a header's
explicit `prefix "CONFIG_"` option for a header-only override.
Enum/set member flags use `ArTargetCpu_s390x`; tristate module flags use
`Name_MODULE`. Collisions between exported names, including generated flags,
are errors in every structured output backend.

---

## Host awareness (probing)

The read-only builtins `HOST_OS`, `HOST_ARCH`, `HOST_KERNEL`, and `HOST_NODE`
describe the running host (`uname` system name, machine, kernel release, and
node name). They are fixed for the process, are not target architecture
settings, and are not automatically exported or cached. `HOST_` declaration
names are reserved. OS names use system spelling, e.g. `"Linux"`, `"Darwin"`,
or `"FreeBSD"`.

```c
computed HostPlatform : string = HOST_OS
computed QueryResult : string = HOST_OS == "Linux"
    ? shell("uname -m") : shell("uname -p") fallback "unknown"
```

Probe expressions accept a `fallback` clause for evaluation failures. Only
the selected ternary branch runs. `shell` and `env` expand `${NAME}` in their
string argument before querying; probe results are memoized by expanded
command/name.
```c
option HOSTNAME : string = shell("hostname") fallback "localhost" {
    label "Host system name"
}

option BUILD_USER : string = env("USER") fallback "builder" {
    label "User performing the build"
}

# Probe with error forwarded as validation warning
option CLANG_VERSION : string = shell("clang --version | head -1") fallback "" {
    label "Detected clang version"
    warn  CLANG_VERSION == "" : "clang not found; LLVM features will be disabled"
}
```

---

## String interpolation

`${NAME}` uses the same file-local/qualified/global lookup as expressions.
References in string values, labels, help, menu labels, validation messages,
queries, template bodies, and generated output/template paths expand from
the current runtime values. Bare `${NAME}` is also a typed expression
reference, so `visible ${arch.ArTargetCpu} == s390x` is valid.

Declaration names and include paths remain static. Macro parameter expansion
can construct declarations before parsing, but ordinary value changes do
not rename symbols or change the loaded file graph. This preserves cache
identities and avoids rebuilding a different language program on every edit.

```c
option PREFIX : string = "/usr/local"

computed BIN_DIR : string = "${PREFIX}/bin"
computed LIB_DIR : string = "${PREFIX}/lib/${ARCH}"
```

---

## Cross-symbol validation

All validation clauses share one shape. `warn CONDITION : "message"` and
`error CONDITION : "message"` fire when their condition is TRUE — there is no
assertion style. `warn` is advisory: it is visible in reports and the status
row but never blocks. `error` is a hard failure: `--save` and `--generate`
refuse while an error fires (the CLI offers `--force` to proceed anyway).

Clauses live at the top level (evaluated once, against the whole
configuration) or inside a symbol block (evaluated in that symbol's scope,
after the symbol's value is resolved). Conditions that fail to evaluate — a
misspelled identifier, a failed probe — are themselves reported as errors; a
broken condition never silently reads as false.

```c
option RAM_SIZE : int = 1024
option THREADS  : int = 8

# Hard errors — block --save / --generate while they hold
error RAM_SIZE < THREADS * 128 : "Insufficient memory per thread"
error (RAM_SIZE & (RAM_SIZE - 1)) != 0 : "RAM must be a power of two"

# Advisory — user is informed but not blocked
warn RAM_SIZE >= 256 : "Low memory may degrade performance"
```

---

## Meta-programming (macros)

Macros are private to the defining file by default. `export macro` makes a
macro available to importers, transitively through intermediate includes,
and to child files subsequently included by those importers. Import order
matters: import the shared macros before including their consumers.

A file declaring `namespace utils` defines `utils.Foo(...)`. Use `Foo(...)`
within that namespace and `utils.Foo(...)` elsewhere. Qualified macro names
are also valid as macro arguments. Helper and recursive calls resolve in
the definition environment; generated symbols use the caller's namespace.
Private helper macros can be called by their exported wrapper without being
callable directly from the importer. Repeated imports reuse exported
definitions; conflicting definitions of the same qualified macro are errors.
Expansion is recursive and bounded to 32 levels; unbounded recursion reports
a diagnostic. No compile-time conditional or arithmetic macro dispatch is
provided; finite recursion can use continuation macro parameters.

```c
# utils.zx
zxconfig 1.1
namespace utils
export macro Foo(id) { option ${id} : bool = true }

# ZXConfig
zxconfig 1.1
include "utils.zx"
include "network.zx"

# network.zx: inherits utils.Foo from the importer
zxconfig 1.1
namespace net
utils.Foo(NetEnabled)  # declares net.NetEnabled, exports NetEnabled
```

```c
# File-local macro (default)
macro DRIVER(id, label_name) {
    option DRV_${id} : bool = false {
        label "${label_name} Support"
        tags  ["driver", "${id}"]
    }
}

# Exported macro — visible in includers
export macro PLATFORM_DRIVER(id, label_name, arch_guard) {
    option DRV_${id} : bool = false {
        label   "${label_name} Support"
        visible ${arch_guard}
    }
}

DRIVER(VIRTIO, "VirtIO Virtual Block")
DRIVER(NVME,   "Non-Volatile Memory Express")
```

---

## Output orchestration (generators)

Built-in formats plus a `template` escape hatch for arbitrary output using `${SYMBOL}` interpolation.
```c
generate header   "config.h"     # C/C++ header
generate makefile "config.mk"    # GNU Make fragment
generate json     "config.json"  # Machine-readable state
generate cmake    "config.cmake" # CMake cache variables

# Custom template — any format
generate template "config.ld" from "scripts/linker.ld.tpl" {
    # template file uses ${SYMBOL} syntax
}
```

---

## Advanced expressions

All expression fields share one grammar: defaults, `visible`, `when`,
`enabled`, `implies`, `warn`/`error` conditions, computed definitions, and
macro fragments. Scalar equality supports numbers, strings, and enums.
`{a, b}` denotes a set; set equality ignores order and duplicate members.
`["a", "b"]` denotes a list; list equality preserves order. Comparing a set
with a list is a type error, and collections cannot be ordered with `<`, `>`,
etc. `contains(Features, kasan)` tests membership and
`type_of(Features) == "set"` tests the declared type. Quote arbitrary string
literals; a bare enum/set choice name is only a typed literal where the
surrounding context determines the type — as a default inside that
enum/set's own block, as the other side of a comparison against a direct
enum/set reference, or as the tested member of `contains()` next to a set
reference. An unrelated symbol that happens to mention the same name never
validates a bare name.

```c
option Features : set = {kasan} { choices kasan | ubsan }
option DebugInfo : bool = true {
    visible Features == {ubsan, kasan}
    enabled contains(Features, kasan)
    warn Features == {} : "No debug features selected"
}
error Features != {kasan, ubsan} : "Both debug features are required"
```

`warn CONDITION : "message"` and `error CONDITION : "message"` fire when
CONDITION is true (see Cross-symbol validation). Conditions use truthiness:
any nonzero number — including negatives — is true, as are nonempty
strings/enums and nonempty collections. Tristate values keep their n/m/y
levels (0/1/2) so module-level checks still distinguish "partially on" from
"fully on". Logical operators short-circuit when the result is determined.

Bitwise operators, arithmetic, and builtin functions. `shell` and `env` are also valid in expression context (not just defaults).
```c
computed MASK   : hex  = (1 << 8) - 1
computed NPAGES : int  = RAM_SIZE / PAGE_SIZE
computed LABEL  : string = is_defined(CROSS_COMPILE) ? "${CROSS_COMPILE}gcc" : "gcc"

# Builtins: env, shell, is_defined, abs, min, max, log2, defined_and_true
computed HAS_GIT : bool = shell("git rev-parse --is-inside-work-tree 2>/dev/null") fallback false
```
