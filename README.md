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

Pong callback migration in 3.18: `IvyPongCallback` now takes
`(IvyClientPtr app, void *user_data, int round_trip_delay)`, matching the other
callback conventions. Register it with
`IvyContextSetPongCallback(ctx, callback, user_data)` or
`IvySetPongCallback(callback, user_data)`; use `NULL` when no user data is needed.
This changes the source and binary interfaces for pong callbacks and their
registration functions, so existing users must update their calls and callback
signatures and rebuild against the matching library. Ivy does not own the user
data, which must remain valid while any callback may still use it.

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

## Sending and transport errors (3.18)

`IvyContextSendMsg()` and `IvySendMsg()` keep their signatures. They return the
number of complete frames accepted locally, or a negative `IvyStatus` if any
matching send fails. Zero matching subscriptions is success. A peer with several
matching subscriptions is counted once per subscription. Acceptance means the
frame was written to the socket or queued in full; it does not acknowledge
remote delivery. Fan-out continues after an individual failure.

`IvyContextSendMsgEx(ctx, &report, format, ...)` returns `IVY_OK` or the first
failure and fills an `IvySendReport`, including on partial failure:

```c
IvySendReport report;
int status = IvyContextSendMsgEx(ctx, &report, "TRACK %d %s", id, label);
/* report.matched == report.accepted + report.failed */
if (status != IVY_OK)
    fprintf(stderr, "Ivy %d, OS %d: %zu accepted, %zu failed\n",
            status, report.system_error, report.accepted, report.failed);
```

`IvySendMsgEx()` provides the same report for the current/default context.
Errors before matching leave the counts zero. `system_error` contains the
`errno`/WSA code associated with the first reported failure, or zero when that
failure has no OS code. With OpenMP, the first failure depends on scheduling.
The old count-only functions cannot expose partial counts; retrying a whole
message after failure can duplicate frames that other subscriptions accepted.

The transport path distinguishes `IVY_EIO`, `IVY_ENOMEM` and the new
`IVY_EFIFOFULL`. FIFO append is all-or-nothing. If a frame has already been
partly written and its suffix cannot be queued, the connection is closed to
prevent subsequent frames from corrupting the stream. Direct sends use the
same checked transport and status values. Newlines, Ivy separators (bytes 2
and 3), and formatted NUL bytes are rejected before matching; messages are
text, and UTF-8 is allowed.

Install `IvyContextSetTransportErrorCallback(ctx, callback, data)` to observe
connection failures, including failures while flushing an accepted FIFO:

```c
static void on_transport(IvyClientPtr peer, void *data,
                         IvyStatus status, int system_error) {
    /* peer belongs to this context and may be NULL during connection setup. */
}
```

The callback runs on the context's event-loop thread, outside internal locks,
before its disconnection callback. It describes a failed connection and its
pending frames, not an individual message receipt. Recoverable FIFO-full or
allocation rejections are returned synchronously without this notification.
Passing `NULL` disables it; replacing a callback does not wait for an invocation
already in progress, so its data must remain valid until that invocation returns.
The legacy setter is `IvySetTransportErrorCallback()`. Notifications require a
running loop; none are promised during stop or destruction.

Run the fault-injection tests with `./tests/run_transport_errors.sh`. They cover
normal, OpenMP and (when available) GLib builds, partial fan-out, full FIFOs, allocation failures,
partial writes, immediate/direct failures, and deferred flush errors.

## C++23 wrapper

`src/cpp/ivy.hpp` provides `ivy::Bus`, a non-copyable, movable owner of an
explicit `IvyContext`. Its constructor accepts an application name as
`std::string_view`, an optional ready message as
`std::optional<std::string_view>`, and optional application/die callbacks as
`std::move_only_function`. Captures replace user-data arguments at the C++
boundary, including captures of non-copyable objects such as `std::unique_ptr`.
The peer and event arguments currently retain their C API types.

The Linux build has separate targets for the wrapper. Building the C library
and tools does not enable C++23:

```sh
make -C src cpp
# After installing the C library with the same PREFIX:
make -C src install-cpp PREFIX=/usr/local
```

This produces `libivy-cpp.a` and `libivy-cpp.so.3.18` in `src/build/cpp/`,
with `libivy-cpp.so` and `libivy-cpp.so.3` symlinks to the shared library.
The shared wrapper links against the C shared library `libivy.so.3`.
Installation adds both libraries and the symlinks, `Ivy/ivy.hpp`, and
`ivy-cpp.pc`. Compile a consumer with C++23 enabled:

```sh
c++ -std=c++23 examples/cpp/lifecycle.cpp \
    $(pkg-config --cflags --libs ivy-cpp) -o lifecycle
```

The linker selects the shared libraries by default. Selecting the `.a` archives
explicitly retains the static linking option; `pkg-config --static --libs ivy-cpp`
supplies their additional dependencies but does not force static linking.

Construction copies the input strings and creates the context. An absent ready
message (`std::nullopt`) is distinct from an empty message. Embedded NUL bytes
in constructor arguments throw `std::invalid_argument`; C context creation
failures throw `std::system_error`.

`start()` uses `IVYBUS` or Ivy's default address; `start(address)` accepts a
string view, copied before passing it to C. Both overloads are `noexcept` and
return `std::expected<void, std::error_code>`, marked `[[nodiscard]]`. On failure,
the error can be compared with `ivy::make_error_code(IVY_ESTATE)` or another
`IvyStatus`. An embedded NUL in the address returns `IVY_EINVAL`; failure to
allocate the temporary string returns `IVY_ENOMEM`.

```cpp
if (auto result = bus.start("127:2010"); !result) {
    std::cerr << result.error().message() << '\n';
    return 1;
}
```

`stop()` requests an idempotent stop, and destruction releases the context.
A moved-from object has a null `native_handle()` and reports
`IVY_CTX_DESTROYED`; starting it returns an `IVY_ESTATE` error. Moving a bus preserves the
context and callback storage addresses.

The event-loop interface will be designed separately. For now,
`native_handle()` provides borrowed access for C interoperation. Drive the
loop through the contextual C API, and stop and join any external loop thread
before destroying the bus or replacing it by move assignment. Do not destroy
the native context, replace the wrapper's application/die callbacks, or destroy
the bus from an Ivy callback. A rejected C destruction terminates the process
to avoid freeing callback storage that C can still use. Moving or destroying
the bus must not race with operations on that object.

Callbacks retain Ivy's threading and borrowed-peer lifetime rules; captured
references must remain valid, and shared mutable state needs synchronization.
Callback exceptions are caught before returning to C. The wrapper stores the
first exception and requests a stop. After servicing/joining the loop, call
`rethrow_callback_exception()` to rethrow and clear it. `state()` and
`native_handle()` are also available for lifecycle inspection.

Subscriptions use one overloaded `bind` name, with the callback first:

```cpp
auto messages = bus.bind(on_message, R"(^TRACK ([0-9]{2}) 100%$)");
auto filtered = bus.bind(on_message, R"(^TRACK {} ([0-9]{{2}})$)", aircraft_id);
auto direct = bus.bind(on_direct);
```

Regexp subscriptions are start-anchored by default. A `consteval` parameter
type requires constant regexps and format strings to begin with `^`; missing
anchors cause a compilation error. The final regexp is also validated with
PCRE2 after formatting and Ivy interval expansion. For example, `^FOO|BAR`
starts with `^` but is rejected at registration because its second alternative
is unanchored. Runtime arguments cannot introduce such an alternative silently.
The validator checks PCRE2's inferred `PCRE2_ANCHORED` flag without forcing it.
It does not add anchors or groups, preserving Ivy's `^MESSAGE_CLASS` filtering.

Unanchored search requires an explicit operation:

```cpp
auto anywhere = bus.bind_unanchored(on_message, R"(TRACK ([0-9]{2})$)");
auto formatted_anywhere = bus.bind_unanchored(on_message, "TRACK {} (.*)", aircraft_id);
```

For a dynamically constructed regexp that must remain anchored, use
`bus.bind(on_message, ivy::runtime_regexp(expression))`. `runtime_regexp` borrows
the string for this call and does not opt out of validation: it still must
start with `^`, and PCRE2 must recognize it as anchored. A dynamic format can
be processed separately and its resulting regexp passed through this route.

`MessageCallback` is a `std::move_only_function` taking
`(IvyClientPtr, std::span<const std::string_view>)`; `DirectCallback` takes
`(IvyClientPtr, int, std::string_view)`. Captures and direct-message text are
borrowed views valid only for the callback invocation. Callback exceptions
follow the same stop-and-rethrow mechanism as application/die callbacks.

Without formatting arguments, the regexp is passed unchanged, including braces
and percent signs. With one or more arguments, the header's template uses
`std::format_string` and `std::format`: double literal regexp braces as `{{` and
`}}`. Formatting inserts values as supplied, without escaping regexp syntax.
The format is checked at compile time. Anchored regexps are compiled locally
once for validation; remote Ivy peers still compile them for message matching.
The `_unanchored` overloads bypass this local anchoring validation and retain
the C API's remote regexp compilation behavior. Raw C printf formatting is not
exposed by these overloads. Local validation requires a PCRE2-enabled C library.

The regexp overloads return `std::expected<ivy::Subscription, std::error_code>`;
the direct overload returns `std::expected<ivy::DirectSubscription, std::error_code>`.
The compiler selects the type from the arguments, with no variant to inspect.
Check the result and keep it (or move its token value) alive. Both token types
are non-copyable and movable; their destructors unregister the callback.
`subscription.unbind()` performs explicit, idempotent cancellation and returns
an expected result. `subscription.is_bound()` becomes false after cancellation,
Bus stop/destruction, or replacement of the context's single direct callback.
An old direct token cannot cancel its replacement. Tokens can outlive the Bus
without keeping its context alive. Do not concurrently move/destroy a token
while accessing that same token from another thread.

Only `Subscription` provides `change()`, which retains the same native handle,
callback and captured state while replacing the regexp. `DirectSubscription`
provides only `unbind()` and `is_bound()` because a direct registration has no
regexp. Change follows the same raw/formatted distinction as bind:

```cpp
if (messages) {
    auto result = messages->change(R"(^NEW_TRACK ([0-9]{2}) 100%$)");
    if (!result) {
        std::cerr << result.error().message() << '\n';
        return 1;
    }
}
// With arguments, use {{ and }} for literal regexp braces.
if (filtered) {
    auto result = filtered->change(R"(^NEW_TRACK {} ([0-9]{{2}})$)", aircraft_id);
    if (!result) {
        std::cerr << result.error().message() << '\n';
        return 1;
    }
}
```

`change()` enforces anchoring even on a subscription originally created with
`bind_unanchored`. Use `change_unanchored(regexp)` or
`change_unanchored(format, arguments...)` to explicitly allow search away from
the start, or `change(ivy::runtime_regexp(expression))` to check a dynamic
anchored regexp. The default forms deliberately have no implicit runtime
`std::string_view` overload, so dynamic strings cannot bypass the policy.

Both change overloads return `std::expected<void, std::error_code>`. An inactive
token returns `IVY_ESTATE`, and a stopped bus returns `IVY_ESTOPPED`. Input,
formatting and allocation errors use the same conventions as bind. A failed
native change leaves the old regexp in place. Peer updates are asynchronous;
messages already in flight may still reflect the old regexp.

Unbinding can be done inside the callback or from another thread. A callback
already selected by the C++ relay may finish after unbind returns; its captures
remain alive until it returns. Small C user-data relay records are retained
until context destruction because C may have copied their addresses before
unregistration. User captures are released on cancellation/replacement once
in-flight callbacks finish, and on Bus destruction even if tokens survive.
The C handle is pinned during change, allowing concurrent or reentrant unbind
without freeing a handle still in use. Cancellation disables the callback
immediately and defers native removal until all changes finish; a successful
native change overtaken by cancellation reports `IVY_ESTATE`.

Empty callables and embedded NUL bytes in regexps return `IVY_EINVAL`; stopped
buses return `IVY_ESTOPPED`, and moved-from buses return `IVY_ESTATE`. Allocation
failures return `IVY_ENOMEM`. The formatted overload maps `std::format_error` to
`IVY_EINVAL`; other exceptions from user-defined formatters propagate to the caller.
The anchoring policy has its own status, `IVY_EUNANCHORED`: the required leading
`^` is missing or PCRE2 does not recognize the expanded regexp as anchored.
Syntax errors remain `IVY_EINVAL`. The C++ error message for `IVY_EUNANCHORED` is
"regexp must start with '^' and be anchored".

Run `./tests/cpp/run.sh` for the wrapper tests, including a temporary
installation and consumers linked against the static and shared libraries.
`examples/cpp/lifecycle.cpp` demonstrates the available wrapper API and exits
when another Ivy application sends a die request.

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
./tests/run_glib_backend.sh
```

GLib/GTK applications can link against `libglibivy` (pkg-config: `ivy-glib`).
This backend requires GLib 2.36 or newer and implements the contextual channel,
loop, and timer APIs with GLib sources. Each `IvyContext` uses the thread-default
`GMainContext` in effect when it is created, falling back to GLib's global
default context. An application's `g_main_loop_run()` or GTK main loop drives
all Ivy contexts attached to that main context, including their timers.

For separate event-loop threads, create a `GMainContext` for each thread and
push it with `g_main_context_push_thread_default()` while creating its Ivy
contexts, then pop it before handing the loop to that thread. A main context
has one loop owner at a time. `IvyContextMainLoop()` and `IvyContextIdle()` also
drive the associated GLib main context; idle processes one nonblocking
iteration, including due timers. The before/after-select hooks apply to these
Ivy entry points and bracket polling, with callbacks dispatched after the
after-select hook.

Worker threads can post controls, change writable watches, create contextual
timers, and request a stop while the loop is running. Stopping an Ivy context
disables its I/O and timers without quitting the application's GLib loop or
stopping other Ivy contexts. Join an Ivy-owned loop thread before destroying
its context. As with the existing timer API, a timer ID must not be used after
it expires or its removal has been dispatched. Legacy Ivy and timer wrappers
share sources on GLib's global default main context.

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
