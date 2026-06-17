# libivy
ivy software bus

## Current development status

The current development line has been migrated through
`FEATURE/multi_bus-MT_safe_phase11`.

New C code should prefer the explicit `IvyContext*` API:

- create one `IvyContext` per Ivy bus;
- start it with `IvyContextStart()`;
- drive it with `IvyContextMainLoop()` or `IvyContextIdle()`;
- call the `IvyContext*` variants for bind, send, direct messages, ping,
  timers and application queries.

The historical API (`IvyInit()`, `IvyStart()`, `IvyBindMsg()`,
`IvySendMsg()`, etc.) is still kept as a compatibility facade so old programs
continue to compile.

Recent multibus/tooling work:

- `ivyprobe` can run on several buses in one process. It starts the bus from
  `IVYBUS` when present and adds one bus per `-b` option. Messages sent by the
  probe are sent on all configured buses, and regexps are installed on all of
  them.
- `ivyprobe -t` uses one contextual timer and broadcasts its test messages on
  all configured buses.
- `ivythroughput`, `ivyperf`, `ivytestready`, `ivytranslater` and
  `examples/testUnbind.c` now use the public contextual API.
- The default tool build now includes those select-loop tools.

See `IVY_MTSAFE.md` for the migration plan and status. See
`IVY_CODE_QUALITY_AUDIT.md` for remaining hardening work that is broader than
the multibus migration.

## Installation

The build currently relies on PCRE2 for regular expression support, and readline/history support for the tools target (`ivyprobe`, etc.).

### Linux

On Linux, the PCRE2 runtime library is often already present on modern distributions because many other packages depend on it, but you should not assume it is part of the base install.

For this project, the required development packages are:
- PCRE2 (`pcre2-config`), for regex support
- readline/history, for `ivyprobe` and other tools

Typical build requirements:

- `make`
- `gcc`
- `g++`
- PCRE2 development files
- readline development files (for tools)

Examples:

```bash
# Debian / Ubuntu
sudo apt install build-essential libpcre2-dev libreadline-dev

# Fedora
sudo dnf install gcc gcc-c++ make pcre2-devel readline-devel

# Arch Linux
sudo pacman -S base-devel pcre2 readline
```

Build:

```bash
cd src
make
```

This default build includes tools, so readline/history must be available.

If you only need the library artifacts, build without tools:

```bash
cd src
make static-libs shared-libs
```

Useful regression tests for the current migration line, from the repository
root:

```bash
./tests/run_phase6.sh
./tests/run_phase7.sh
./tests/run_phase8_ivyprobe_timer.sh
./tests/run_phase9_select_wakeup.sh
./tests/run_phase10_tools.sh
./tests/run_phase11_runtime_errors.sh
./tests/run_phase11_interval_regexp.sh
```

Optional OpenMP build:

```bash
cd src
make omp
```

Notes:

- The Linux makefile now defaults x86-64 builds to `-march=x86-64-v3 -mtune=generic`.
- If you want a different CPU target, override `X86_64_CFLAGS` when invoking `make`.

### macOS

The macOS makefiles are intended to use the official Apple compiler from Xcode / Command Line Tools, plus Homebrew for dependencies.

Required:

- Xcode Command Line Tools or full Xcode
- Homebrew
- `pcre2`

Optional:

- `libomp` if you want to try the OpenMP build

Install dependencies:

```bash
xcode-select --install
brew install pcre2
brew install libomp   # optional, only for OpenMP builds
```

Build:

```bash
cd src
make -f Makefile.osx clean
make -f Makefile.osx
```

Optional OpenMP build:

```bash
cd src
make -f Makefile.osx omp
```

Notes:

- The macOS build uses `xcrun clang` / `xcrun clang++`.
- The makefile explicitly uses the macOS SDK sysroot from `xcrun --sdk macosx --show-sdk-path`.
- Homebrew is expected under `/opt/homebrew` on Apple Silicon and `/usr/local` on Intel Macs.
- This path was tested on macOS Tahoe 26.4.

### Windows

The Windows makefiles were refreshed toward a more modern MSVC + PCRE2 setup, but this path is still preparation work and has not been fully validated.

Expected toolchain:

- Visual Studio or Build Tools for Visual Studio
- `nmake`
- a PCRE2 build available as a `.lib`

The Windows makefiles currently expect configurable PCRE2 paths through:

- `PCRE2_INC`
- `PCRE2_LIB`
- `PLATFORM`
- `CONFIGURATION`

Example invocation from a Visual Studio developer shell:

```bat
cd src
nmake /f Makefile.win32 PCRE2_INC=/I"path\to\pcre2\include" PCRE2_LIB="path\to\pcre2-8.lib"
```

Notes:

- The Windows makefiles now target PCRE2 and `ws2_32.lib`.
- The core select loop wakeup path now has a Winsock-compatible socket-pair
  emulation, but it still needs a native Windows validation pass.
- The Windows tool build is intentionally conservative.
- `ivythroughput` is still not treated as a ready Windows target because its source uses POSIX process APIs such as `fork`, `waitpid`, `kill`, and `usleep`.
- In short: the Windows side is a useful starting point, but it still needs Windows-specific follow-up.
