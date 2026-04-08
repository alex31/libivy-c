# libivy
ivy software bus

## Installation

The build currently relies on PCRE2 for regular expression support.

### Linux

On Linux, the PCRE2 runtime library is often already present on modern distributions because many other packages depend on it, but you should not assume it is part of the base install. For building this project, the important part is the development package, because the makefiles use `pcre2-config`.

Typical build requirements:

- `make`
- `gcc`
- `g++`
- PCRE2 development files

Examples:

```bash
# Debian / Ubuntu
sudo apt install build-essential libpcre2-dev

# Fedora
sudo dnf install gcc gcc-c++ make pcre2-devel

# Arch Linux
sudo pacman -S base-devel pcre2
```

Build:

```bash
cd src
make
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
- The Windows tool build is intentionally conservative.
- `ivythroughput` is still not treated as a ready Windows target because its source uses POSIX process APIs such as `fork`, `waitpid`, `kill`, and `usleep`.
- In short: the Windows side is a useful starting point, but it still needs Windows-specific follow-up.
