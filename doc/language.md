# The zxconfig format

## Schema declaration

Every zxconfig file should open with a version header so tooling can handle format evolution gracefully.
```c
zxconfig 1.0
```

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

The defining file owns its namespace. Put one optional `namespace` declaration
immediately after its version header. Without it, the file declares global
symbols. Namespaces are case-sensitive and may contain dotted segments.

```c
# arch/arch.zx
zxconfig 1.0
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
zxconfig 1.0
include "arch/arch.zx"
include "s390x/s390x.zx" visible arch.ArTargetCpu == s390x
```

Every included file must exist and have a version header, even if its
visibility condition is false. Files are parsed once; `visible` controls
presentation and inherits through included menus and options. Editing the
architecture re-evaluates visibility and redraws the TUI without re-parsing.
Hidden options retain their values. A bare `visible arch.ArTargetCpu` checks
whether the value is nonempty; use `== s390x` to choose one architecture.

Internal identities and cache assignments use `arch.ArTargetCpu`. Generated
headers, Makefiles, CMake files, and JSON use the flat declaration spelling
`ArTargetCpu`, preserving case and adding no implicit `CONFIG_` prefix.
Header generation still accepts an explicit `prefix "CONFIG_"` option.
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

`validate` blocks are hard errors — the configuration cannot be saved. The new `warn` form is advisory only; it produces a visible warning but does not block the save.
```c
option RAM_SIZE : int = 1024
option THREADS  : int = 8

# Hard error
validate "Insufficient memory per thread" {
    RAM_SIZE >= THREADS * 128
}

validate "RAM must be power of two" {
    (RAM_SIZE & (RAM_SIZE - 1)) == 0
}

# Soft warning — user is informed but not blocked
warn "Low memory may degrade performance" {
    RAM_SIZE >= 256
}
```

---

## Meta-programming (macros)

Macros are scoped to the file by default. Use `export macro` to make them available to files that `include` this one.
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
`enabled`, `implies`, inline `warn`, and top-level `warn`/`validate`.
Scalar equality supports numbers, strings, and enums. `{a, b}` denotes a set;
set equality ignores order and duplicate members. `["a", "b"]` denotes a
list; list equality preserves order. Comparing a set with a list is a type
error. Collection ordering (`<`, `>`, etc.)
is unsupported. `contains(Flags, kasan)` tests membership and
`type_of(Flags) == "set"` tests the declared type. Quote arbitrary string
literals; bare enum/set choice names must be declared in `choices`.

```c
option Features : set = {kasan} { choices kasan | ubsan }
option DebugInfo : bool = true {
    visible Features == {ubsan, kasan}
    enabled contains(Features, kasan)
    warn Features == {} : "No debug features selected"
}
validate "Both features are required" { Features == {kasan, ubsan} }
```

Inline `warn EXPR : "message"` reports when EXPR is true. Top-level
`warn "message" { EXPR }` and `validate "message" { EXPR }` are assertions
and report when EXPR is false. Conditions use truthiness: positive numbers,
nonempty strings/enums, and nonempty collections are true. Logical operators
short-circuit when the result is determined and retain tristate semantics.

Bitwise operators, arithmetic, and builtin functions. `shell` and `env` are also valid in expression context (not just defaults).
```c
computed MASK   : hex  = (1 << 8) - 1
computed NPAGES : int  = RAM_SIZE / PAGE_SIZE
computed LABEL  : string = is_defined(CROSS_COMPILE) ? "${CROSS_COMPILE}gcc" : "gcc"

# Builtins: env, shell, is_defined, abs, min, max, log2, defined_and_true
computed HAS_GIT : bool = shell("git rev-parse --is-inside-work-tree 2>/dev/null") fallback false
```
