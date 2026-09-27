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
```c
include "arch/x86/Kconfig"
include "drivers/net/Kconfig" as net        # net.VIRTIO_NET etc.
include "drivers/net/Kconfig" visible NET   # conditional include
```

---

## Host awareness (probing)

Probe expressions now accept a `fallback` clause so parse-time failures are deterministic.
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

Bitwise operators, arithmetic, and builtin functions. `shell` and `env` are also valid in expression context (not just defaults).
```c
computed MASK   : hex  = (1 << 8) - 1
computed NPAGES : int  = RAM_SIZE / PAGE_SIZE
computed LABEL  : string = is_defined(CROSS_COMPILE) ? "${CROSS_COMPILE}gcc" : "gcc"

# Builtins: env, shell, is_defined, abs, min, max, log2, defined_and_true
computed HAS_GIT : bool = shell("git rev-parse --is-inside-work-tree 2>/dev/null") fallback false
```
