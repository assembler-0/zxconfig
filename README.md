# ZXConfig

A configuration engine for kernel and bare-metal projects.  Describe your
options in a small English-like language; validate them, keep the chosen
values in a plain-text cache, and generate build inputs — from a full-screen
terminal interface or entirely from the command line.

## Build

C++23, CMake 3.31+, [FTXUI](https://github.com/ArthurSonzogni/FTXUI) 7, and
GoogleTest for the tests.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
cmake --install build
```

## Try it

```bash
cd demo && ../build/zxconfig
```

16,755 symbols and 2,622 menus: nested subsystem forests, architecture
switching, recursive macros, and a 256-hop dependency chain. See the
[stress demo](demo/README.md) for presets and repeatable benchmarks.

## Use it

```bash
zxconfig                              # the interface, manifest ./ZXConfig
zxconfig --preset ci --generate       # CI: no interface, no interaction
```

## Documentation

- [doc/usage.md](doc/usage.md) — behavior, the interface, typical workflows
- [doc/cli.md](doc/cli.md) — the complete command line
- [doc/cache.md](doc/cache.md) — the cache and presets
- [doc/language.md](doc/language.md) — the language specification
- [doc/dsl-review.md](doc/dsl-review.md) — remaining language consistency and stability gaps

## License

Apache License 2.0.  See [LICENSE](LICENSE).
