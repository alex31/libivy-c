# libivy

Ivy is a software bus for exchanging text messages between applications.
This library provides both a C API and a C++23 API, using the same Ivy protocol
and supporting several independent buses in one process.

| API | Public header | Entry point |
| --- | --- | --- |
| C++23 | `<Ivy/ivy.hpp>` | `ivy::Bus::create()` |
| C with explicit contexts | `<Ivy/ivy.h>` | `IvyContextCreate()` |
| Legacy C | `<Ivy/ivy.h>` | `IvyInit()` |

The C++ API manages bus and subscription lifetimes with RAII, accepts lambdas
and move-only callbacks, and reports errors with `std::expected`. It supports
raw or typed message subscriptions, formatted sends, direct messages, timers,
peer information and filters. Native and GLib/GTK event loops are available;
the [Qt6 example](examples/ivyqt/README.md) uses the optional `ivy::LoopThread`.

Start with the [C++ quick start](#quick-start), then use the
[C++23 API guide](#c23-wrapper) below. For C applications, see the
[context API guidance](#current-development-status). Both APIs are included
in the [Debian/Ubuntu packages](#debian--ubuntu-packages) and the
[Doxygen reference](#api-reference-and-source-layout).

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

## Per-context filters (3.18)

Filters are owned by each `IvyContext` and initially empty. Configure them with
`IvyContextSetFilter(ctx, count, words)`, `IvyContextAddFilter(ctx, word)` and
`IvyContextRemoveFilter(ctx, word)`. Creating, modifying or destroying one bus
never changes another bus's filters. An empty list disables filtering.

`SetFilter` now atomically **replaces** the list, as its name implies; older
implementations appended. Class words are copied, deduplicated and validated
(nonempty ASCII letters, digits, underscores or hyphens). Invalid input returns
`IVY_EINVAL`; allocation failures return `IVY_ENOMEM` and leave the old policy
intact. Calls after stop return `IVY_ESTOPPED`. Updates and subscription
processing share the context's bindings lock.

The legacy `IvySetFilter`, `IvyAddFilter` and `IvyRemoveFilter` delegate to the
calling thread's current/default context. Inside a native callback they target
that callback's bus. Code that previously relied on process-wide filters must
configure each context explicitly. `ivyprobe` applies its CLI filter list to
each configured bus, and `ivyperf` configures its own context.

Filtering remains an optimization on **remote regexp advertisements**, used
when selecting recipients for sends. It checks the leading literal class after
`^`; prefixes such as `^TRA.*` remain compatible with class `TRACK`, and general
expressions without an extractable class are kept. Changes apply to future
advertisements, without reprocessing already accepted or rejected subscriptions.
Rejections generate `IvyFilterBind` outside internal locks. Configure before
start when the policy must cover initial advertisements.

Run `./tests/run_context_filters.sh` for lifecycle, replacement, allocation
failure and concurrent-update checks. `./tests/cpp/run.sh` additionally verifies
actual filtering and legacy callback routing on two independent buses.

## C++23 wrapper

Include `<Ivy/ivy.hpp>` to use `ivy::Bus`. The wrapper owns its C context and
releases it when the Bus is destroyed. Each subscription is an owned token:
keep it alive to keep receiving messages. Use `bind_raw()` for capture views
or `bind_convert()` for conversion to `long`, `double`, `std::string_view` and
`bool`. Both accept an optional sender as the first callback argument;
`bind_convert()` then passes `ivy::ConvertStatus` before the converted values.

### Quick start

This complete listener receives `TRACK <id> <altitude>` messages and stops on
`STOP`. Save it as `cpp-listener.cpp`:

```cpp
#include <Ivy/ivy.hpp>
#include <iostream>

int main() {
    const auto check = [](const auto& result) {
        if (!result) std::cerr << result.error().message() << '\n';
        return result.has_value();
    };
    auto created = ivy::Bus::create("cpp-listener", "cpp-listener ready");
    if (!check(created)) return 1;
    auto& bus = *created;

    auto tracks = bus.bind_convert(
        [&bus, &check](IvyClientPtr peer, ivy::ConvertStatus status,
                      long id, double altitude) {
            if (status != ivy::ConvertStatus::OK) {
                std::cerr << bus.conversion_error() << '\n';
                return;
            }
            auto sender = bus.application(peer);
            if (!check(sender)) return;
            std::cout << sender->first << ": track " << id
                      << " at " << altitude << '\n';
        }, R"(^TRACK (\S+) (\S+)$)");

    // The sender can be omitted when only the message matters.
    auto stop = bus.bind_raw([&bus, &check](auto) {
        check(bus.request_stop());
    }, "^STOP$");

    if (!check(tracks) || !check(stop) || !check(bus.start())) return 1;
    if (!check(bus.run())) return 1;
    return check(bus.take_callback_error()) ? 0 : 1;
}
```

With `ivy-c-dev` installed, compile using a C++23 compiler and standard library
(GCC 13 or newer is supported):

```sh
c++ -std=c++23 cpp-listener.cpp $(pkg-config --cflags --libs ivy-cpp) -o cpp-listener
IVYBUS=127.255.255.255:2010 ./cpp-listener
```

From another terminal, run `IVYBUS=127.255.255.255:2010 ivyprobe` and send
`TRACK 42 123.5`, then `STOP`. A message such as `TRACK bad 123.5` demonstrates
the conversion diagnostic while leaving the listener running.

`start()` connects the bus; `run()` services callbacks on the calling thread
until a stop request. It creates no background thread. The `tracks` and `stop`
tokens stay alive throughout the loop, and the sender and capture views are
borrowed for the duration of each callback. See
[callback errors and subscriptions](#callback-errors-and-subscriptions) for
the complete conversion and lifetime rules, and
[building and linking](#building-and-linking) to build from source.

### API reference and source layout

`src/cpp/ivy.hpp` provides `ivy::Bus`, a non-copyable, movable owner of an
explicit `IvyContext`. Its `Bus::create()` factory accepts an application name as
`std::string_view`, an optional ready message as
`std::optional<std::string_view>`, and optional application/die callbacks as
`std::move_only_function`. Captures replace user-data arguments at the C++
boundary, including captures of non-copyable objects such as `std::unique_ptr`.
The peer and event arguments currently retain their C API types.

`src/cpp/ivy.hpp` is the public entry point, reading map and complete annotated
example. The example covers startup, subscriptions, direct/broadcast sends,
callbacks, timers and application information, with a pointer to the relevant
`api/*.hpp` beside each Ivy operation. It assembles smaller headers in `src/cpp/api/`, each retaining the complete documentation and examples
for its subject:

| Subject | Header to read |
| --- | --- |
| Creation, start/stop, state, callback errors and the complete first example | `api/lifecycle.hpp` |
| Blocking execution and asynchronous stop request | `api/mainloop.hpp` |
| Optional loop thread and checked join | `ivy_thread.hpp` |
| Regexp and direct message subscriptions | `api/messages.hpp` |
| Broadcasts, reports, direct messages and control sends | `api/send.hpp` |
| Pong, remote subscriptions and transport errors | `api/callbacks.hpp` |
| Periodic, limited and one-shot timers | `api/timers.hpp` |
| Application lookup and owned snapshots | `api/applications.hpp` |
| Application name, numeric IP and advertised TCP port | `api/application_types.hpp` |
| Filter replacement and incremental changes | `api/filters.hpp` |
| Error codes, result conventions and SendReport | `api/results.hpp` |
| Regexp types, standalone validation and formatting | `api/regexp.hpp` |
| Message/event subscription lifetimes | `api/subscriptions.hpp` |
| Timer settings and token lifetime | `api/timer_types.hpp` |

Applications still only need `#include <Ivy/ivy.hpp>` and use the same `Bus`
methods. A direct include of an API section also assembles the complete API.
The member sections are inserted inside the single `Bus` declaration; this
avoids introducing inheritance or changing the API just to split its files.
`ivy_detail.hpp`, included at the end, keeps template/constexpr implementations.
`ivy_bus_private.hpp` contains Bus's private declarations, so the entry point
shows the annotated example and public API map without exposing those details.

The compiled implementation is split by responsibility: `ivy.cpp` handles bus
lifecycle, errors and notifications, `ivy_subscription.cpp` handles subscriptions
and their message callbacks, `ivy_events.cpp` handles single event callbacks,
`ivy_timer.cpp` handles timers, `ivy_filters.cpp` configures filters, and
`ivy_send.cpp` handles sending and `ivy_application.cpp` handles application queries.
They share private state
through `ivy_internal.hpp`, which is not installed. Run `doxygen Doxyfile` from
the repository root to generate both C and C++ API documentation in
`doc/doxygen/html/`. Documentation generation also requires Python 3:
`doc/doxygen_cpp_filter.py` assembles the declarations for Doxygen, which does
not inline headers inside class declarations. The compiler reads the original
headers directly, and Doxygen reads each section's guide on its own file page.
The reference opens with a C/C++ API comparison and has two top-level groups:
**C API** (explicit contexts, shared types, low-level timers and legacy wrappers)
and **C++23 API** (the `ivy` types and `Bus` methods). C filtering is split into
explicit-context operations and legacy current/default-context wrappers;
C++ per-Bus filtering has its own guide.
The PDF has separate **C API** and **C++23 API** parts, including each family's
types and headers. `doc/organize_pdf.py` arranges the generated LaTeX using
Doxygen's XML index while preserving all reference sections and labels.
Within those parts, the reference follows usage order: initialization, message
subscriptions, broadcasts, direct messages, filters, events and optional helpers.
`ivy::Bus` and message subscriptions precede the less frequently needed types.
`doc/reference_order.py` defines this order; the Doxygen filter reorders only its
documentation input, and `doc/DoxygenLayout.xml` puts operations before callback
typedefs and ownership machinery. Header declarations used by the compiler are
not reordered. Add new operations to that ordering policy when extending the
API; unknown declarations cause documentation generation to fail explicitly.
Alphabetical indexes are retained for symbol lookup.

Generate the PDF reference with `sh doc/build_pdf.sh`. The script checks for
Python 3, Doxygen, Graphviz (`dot`), Make, `pdflatex` and `makeindex`, then writes
`doc/doxygen/ivy-api.pdf`. A TeX Live installation with the LaTeX base,
recommended/extra packages, plain/generic packages and recommended fonts is required. Intermediate
LaTeX files are written to `doc/doxygen/latex/`; generated documentation is
excluded from version control.
The PDF uses printed cross-references to avoid incorrect C++ alias hyperlinks
produced by Doxygen 1.9.x.

Debian/Ubuntu builds produce two packages: `ivy-c` contains the C and C++23
shared libraries, and `ivy-c-dev` contains their headers, static libraries,
pkg-config files and the PDF at `/usr/share/doc/ivy-c-dev/ivy-api.pdf`.
The PDF is rebuilt from source when packaging; Doxygen and LaTeX are build
requirements only. See [the packaging instructions](debian/README).

### Building and linking

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
Installation adds both libraries and the symlinks, `Ivy/ivy.hpp`,
`Ivy/ivy_detail.hpp`, `Ivy/ivy_bus_private.hpp`, the public `Ivy/api/*.hpp`
sections, and `ivy-cpp.pc`. Compile a consumer with C++23 enabled:

```sh
c++ -std=c++23 examples/cpp/lifecycle.cpp \
    $(pkg-config --cflags --libs ivy-cpp) -o lifecycle
```

The linker selects the shared libraries by default. Selecting the `.a` archives
explicitly retains the static linking option; `pkg-config --static --libs ivy-cpp`
supplies their additional dependencies but does not force static linking.

### Creating and running a bus

Creation returns `std::expected<ivy::Bus, std::error_code>` and copies the input
strings. An absent ready message (`std::nullopt`) is distinct from an empty
message. Embedded NUL bytes or invalid lengths return `IVY_EINVAL`; allocation
failures return `IVY_ENOMEM`. Native creation/registration errors are returned
without leaving a partially initialized bus.

```cpp
auto created = ivy::Bus::create("receiver", "receiver ready");
if (!created) {
    std::cerr << created.error().message() << '\n';
    return 1;
}
auto& bus = *created;
```

The runtime wrapper operations are `noexcept`; errors are reported through
`std::expected` or `SendReport`. Callback storage is constructed inside the
checked calls, so pass lambdas/functors directly, or move an existing
`std::move_only_function`. Allocations and other work performed by the caller
while evaluating arguments happen before entering the wrapper.

These changes replace the former throwing constructor with `Bus::create()`.
Check `stop()`'s expected result and use `take_callback_error()` to collect
callback failures without exception handling.

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

`stop()` requests an idempotent stop and returns
`std::expected<void, std::error_code>`. Destruction releases the context.
A moved-from object has a null `native_handle()` and reports
`IVY_CTX_DESTROYED`; starting it returns an `IVY_ESTATE` error. Moving a bus preserves the
context and callback storage addresses.

`bus.run()` runs the blocking event loop on the calling thread, after `start()`.
It returns `std::expected<void, std::error_code>`: success after stop, `IVY_ESTATE`
for an unstarted, moved-from, already-driven or recursively driven bus,
`IVY_ESTOPPED` for a stopped bus, or a backend error. Callback failures stay
separate and are retrieved with `take_callback_error()` after the loop returns.
The new C entry point `IvyContextRun()` supplies checked native results; the old
`IvyContextMainLoop()` remains available as a void facade.

```cpp
if (auto result = bus.run(); !result) {
    std::cerr << result.error().message() << '\n';
    return 1;
}
if (auto result = bus.take_callback_error(); !result) {
    std::cerr << result.error().message() << '\n';
    return 1;
}
```

No loop thread is created automatically. The wrapper exposes no manual iteration
(`idle()`/`poll()`). `native_handle()` remains available for C interoperation.
Stop and join any separately created loop thread
before destroying the bus or replacing it by move assignment. Do not destroy
the native context, replace the wrapper's application/die/transport callbacks, or destroy
the bus from an Ivy callback. A rejected C destruction terminates the process
to avoid freeing callback storage that C can still use. Moving or destroying
the bus must not race with operations on that object.

### Optional loop thread and Qt6 example

`<Ivy/ivy_thread.hpp>` provides `ivy::LoopThread::create(bus[, completion])` for
an already started Bus. It returns `expected<LoopThread, error_code>` and
translates native thread-creation failures into result values. The helper owns
the thread and borrows the Bus, which must stay alive at the same address.
Its destructor requests stop and joins as a fallback. It is available with
both wrapper variants and has no Qt dependency.

`bus.request_stop()` (also available on the helper) requests shutdown without
waiting for the loop's stop-completion condition. `loop.join()` waits for the
thread and returns its driver/completion error; Bus callback errors remain in
`bus.take_callback_error()`. The optional completion callback runs on the Ivy
thread after `run()` returns, potentially before `create()` returns, and can
post a notification to a GUI. Its captures must already be valid.

The native C counterpart is `IvyContextRequestStop()`; the existing `stop()` /
`IvyContextStop()` synchronization contract is unchanged. Internal locks can
briefly delay either request; a stop request is not proof that a thread has exited.

[examples/ivyqt](examples/ivyqt/README.md) now uses C++23 throughout: direct sends
from the Qt GUI and demo workers, one native Ivy thread, and queued Qt signals
for incoming messages and completion. Its normal close path continues processing
Qt events until the producers finish. Build it against pkg-config **ivy-cpp**;
no Qt-specific Ivy library or backend is required. The example now subscribes
to `(.*)` and displays full messages with numeric IP, advertised TCP port,
application name and reception timestamp. It also accepts free-form messages
via Send/Enter and measures Ivy ping RTT for every connected agent, including
silent agents and agents sharing a name. Run `tests/cpp/run_qt.sh` for its
offscreen integration test from a temporary installation.

### GLib / GTK integration

Include `<Ivy/ivy_glib.hpp>` and select **ivy-cpp-glib** instead of ivy-cpp at
link time. This variant links to `libglibivy`; the normal wrapper links to
`libivy`. The two C backends export the same symbols and must not be loaded
in the same process. Choose one wrapper variant per application.

```sh
make -C src cpp-glib
# After installing the C libraries with the same PREFIX:
make -C src install-cpp-glib PREFIX=/usr/local
c++ -std=c++23 examples/cpp/glib.cpp \
    $(pkg-config --cflags --libs ivy-cpp-glib) -o glib-example
```

`ivy::glib::create_bus("app", "ready")` uses the current thread-default
`GMainContext`, falling back to GLib's global default. For a private context,
use `ivy::glib::create_bus(context, "app", "ready")`. Both return the same
`Bus::CreateResult` as `Bus::create()`, accept move-only application/die callbacks,
and preserve the usual Bus API. The helper restores the previous thread-default
context on success and failure. Create on the context's owner thread or before
starting its loop thread; another owner causes an `IVY_ESTATE` result.

After `start()`, the application's GLib/GTK loop services Ivy's sources and
timers directly. No periodic polling call is needed. Stopping a bus leaves the
host loop and other buses running. The C backend keeps its own GLib context
references. Finish dispatch before destroying the bus, and join a separately
created loop thread first. `bus.run()` is also available with this variant and
drives the whole associated GLib context, including other application sources.
GLib's own allocation/error conventions apply to GLib operations.

The optional header is the only public C++ entry point that includes GLib;
ordinary `<Ivy/ivy.hpp>` consumers need no GLib headers. See `api/mainloop.hpp`
for the run contract and `ivy_glib.hpp` for the host-loop guide.
`./tests/cpp/run.sh` validates the native variant and
`./tests/cpp/run_glib.sh` validates GLib, both in static/shared and installed builds.

### Callback errors and subscriptions

Callbacks retain Ivy's threading and borrowed-peer lifetime rules; captured
references must remain valid, and shared mutable state needs synchronization.
Callback failures are contained before returning to C. The wrapper stores the
first error and requests a stop. After servicing/joining the loop, check
`take_callback_error()`, which returns `std::expected<void, std::error_code>`
and clears the stored error. Callback allocation failures use `IVY_ENOMEM`;
other user callback failures use `ivy::Error::callback_failed` in the
`"ivy-cpp"` error category. `state()` and `native_handle()` are also available
for lifecycle inspection.

Subscriptions use explicit method names, with the callback first:

| Method | Subscription |
| --- | --- |
| `bind_raw(callback, regexp)` | Regexp captures as `std::string_view` values |
| `bind_convert(callback, regexp)` | Conversion status and typed regexp captures |
| `bind_direct(callback)` | Direct message: sender, identifier and text |
| `bind_event(callback, selector)` | Pong, remote subscription changes or timers |

```cpp
auto messages = bus.bind_raw(on_message, R"(^TRACK ([0-9]{2}) 100%$)");
auto filtered = bus.bind_raw(on_message, R"(^TRACK {} ([0-9]{{2}})$)", aircraft_id);
auto direct = bus.bind_direct(on_direct);
```

Migration: the former `bind(callback, regexp)`, `bind(callback)` and
`bind(callback, selector)` calls become `bind_raw`, `bind_direct` and
`bind_event`, respectively. `bind_unanchored` becomes `bind_raw_unanchored`.
The old names are removed. Callback signatures, result types and subscription
ownership retain their existing contracts.

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
auto anywhere = bus.bind_raw_unanchored(on_message, R"(TRACK ([0-9]{2})$)");
auto formatted_anywhere = bus.bind_raw_unanchored(on_message, "TRACK {} (.*)", aircraft_id);
```

For a dynamically constructed regexp that must remain anchored, use
`bus.bind_raw(on_message, ivy::runtime_regexp(expression))`. `runtime_regexp` borrows
the string for this call and does not opt out of validation: it still must
start with `^`, and PCRE2 must recognize it as anchored. A dynamic format can
be processed separately and its resulting regexp passed through this route.

`MessageCallback` is a `std::move_only_function` taking
`(IvyClientPtr, std::span<const std::string_view>)`. `bind_raw` and
`bind_raw_unanchored` also accept callbacks taking only
`(std::span<const std::string_view>)`, including generic lambdas such as
`[](auto captures) { /* ... */ }`. If a callable accepts both forms, it receives
the sender. This applies to constant, dynamic and formatted regexps.
`DirectCallback` takes `(IvyClientPtr, int, std::string_view)`. Captures and
direct-message text are borrowed views valid only for the callback invocation. Callback failures
follow the same stop-and-record mechanism as application/die callbacks.

Both regexp APIs place the optional sender first:

| API | With sender | Without sender |
| --- | --- | --- |
| `bind_raw` | `(IvyClientPtr, std::span<const std::string_view>)` | `(std::span<const std::string_view>)` |
| `bind_convert` | `(IvyClientPtr, ivy::ConvertStatus, values...)` | `(ivy::ConvertStatus, values...)` |

The sender is borrowed; use `bus.application_info(peer)` during the callback to
copy its name, address and port. Existing `bind_convert` callbacks taking
`(ConvertStatus, IvyClientPtr, ...)` must move `IvyClientPtr` before the status.

Use `bind_convert(callback, regexp)` to receive converted captures directly:

```cpp
auto tracks = bus.bind_convert(
    [&bus](ivy::ConvertStatus status, long id, double altitude, std::string_view name, bool active) {
        if (status != ivy::ConvertStatus::OK) {
            std::cerr << bus.conversion_error() << '\n';
            return;
        }
        std::cout << id << ": " << name << " at " << altitude
                  << (active ? " (active)\n" : " (inactive)\n");
    }, R"(^TRACK (\S+) (\S+) (\S+) (\S+)$)");
if (!tracks) {
    std::cerr << tracks.error().message() << '\n';
    return 1;
}
// Keep tracks alive while servicing the loop.
```

An optional `IvyClientPtr` first parameter receives the sender, followed by the
mandatory `ivy::ConvertStatus` (first when the sender is omitted). The remaining
signature determines the conversions, in capture order: only `long`, `double`,
`std::string_view` and `bool`, passed by value, are accepted for captures.
The callback must return `void` and have an explicit, unambiguous signature:
generic lambdas, overloaded call operators and other parameter types are rejected at compile time.
Move-only captures, mutable/noexcept lambdas and function pointers work as with
`bind_raw`. Dynamic expressions use `ivy::runtime_regexp(text)`; formatting uses
`bind_convert(callback, "^TRACK {} (.*)$", id)` with the same rules as `bind_raw`.

Integers are decimal and doubles accept decimal/scientific notation with a dot,
independently of the current locale. Both accept a leading `+` or `-` and require
the entire capture: whitespace, trailing text, empty numbers and out-of-range
values are rejected. Doubles must be finite (`nan`/`inf` are rejected).
Booleans first recognize complete decimal integers with an optional sign:
zero (`0`, `-0`, `+000`) is false and every nonzero integer is true, including
integers too large for `long`. Otherwise the first character decides:
`f`/`F` is false, `t`/`T`/`v`/`V` is true (`false`, `TRUE`, `vrai`, etc.).
Other initial characters and empty captures produce `CONVERT_ERROR`.
No whitespace is trimmed; `0.0`, `1e2`, `yes` and ` true` are invalid booleans.
String views preserve the capture without copying, including an empty string,
and remain valid only during the callback.

The callback runs for every received message, with `ivy::ConvertStatus::OK`,
`COUNT_ERROR` if the number of captures differs, or `CONVERT_ERROR` if a capture
cannot be converted. On either error, all capture parameters are default values
(`0`, `0.0`, an empty view, `false`), including captures that converted successfully;
the optional sender is preserved. The bus continues listening and
`take_callback_error()` remains clear. Exceptions thrown by the user callback
retain the usual callback error behavior.

During the callback, `bus.conversion_error()` returns a `std::string_view` with
the exact diagnostic, for example `capture count mismatch: expected 4, received 3`
or `capture 2: cannot convert "bad" to double: invalid numeric syntax`.
Conversion diagnostics identify the first failing capture (numbered from 1),
its text, expected type and failure reason. The view remains valid until the
callback returns; copy it to retain it. Repeated reads and nested callbacks
preserve it, and each thread has its own callback context. The method returns
an empty view on success or outside a typed callback on this bus and thread.
If allocating the diagnostic fails, a fixed fallback message is used and the
callback still runs.

These checks happen on receipt, including after `subscription.change()`;
registration validates the regexp but does not check its capture count.

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
regexp. Change follows the same raw/formatted distinction as bind_raw:

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
`bind_raw_unanchored`. Use `change_unanchored(regexp)` or
`change_unanchored(format, arguments...)` to explicitly allow search away from
the start, or `change(ivy::runtime_regexp(expression))` to check a dynamic
anchored regexp. The default forms deliberately have no implicit runtime
`std::string_view` overload, so dynamic strings cannot bypass the policy.

Both change overloads return `std::expected<void, std::error_code>`. An inactive
token returns `IVY_ESTATE`, and a stopped bus returns `IVY_ESTOPPED`. Input,
formatting and allocation errors use the same conventions as bind_raw. A failed
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
failures return `IVY_ENOMEM`. Formatted overloads map formatting/length errors to
`IVY_EINVAL`; other custom formatter failures return
`ivy::make_error_code(ivy::Error::formatter_failed)`.
The anchoring policy has its own status, `IVY_EUNANCHORED`: the required leading
`^` is missing or PCRE2 does not recognize the expanded regexp as anchored.
Syntax errors remain `IVY_EINVAL`. The C++ error message for `IVY_EUNANCHORED` is
"regexp must start with '^' and be anchored".

### Application queries and standalone validation

`bus.application_info(peer)` returns `expected<ivy::ApplicationInfo, error_code>`
with owned `name`, numeric `address` and advertised Ivy TCP listening `port`.
The copy is made under the C bindings lock without reverse DNS. The port is zero
before the handshake has advertised it. The strings remain valid after peer
changes/disconnection. The existing `application(peer)` pair of name/host remains
available with its original hostname-resolution behavior.


`find_application(name)` returns
`std::expected<std::optional<IvyClientPtr>, std::error_code>`. An empty optional
is a successful lookup with no matching application. The handle is borrowed and
remains valid only while that peer is connected. Names need not be unique; the
first match is returned, following the C API.

`application(peer)` returns an owned `(name, host)` pair in an expected result.
`applications()` returns an owned vector of connected application names, and
`application_regexps(peer)` returns an owned vector of the regexps currently
known and accepted from that peer. Empty vectors are valid results and order
is unspecified. Each query captures one coherent snapshot; separate queries
can observe changes to the bus in between.

```cpp
auto info = bus.application(peer);
if (!info) {
    std::cerr << info.error().message() << '\n';
    return 1;
}
const auto& [name, host] = *info;
std::cout << name << " on " << host << '\n';
```

The strings survive peer disconnection or Bus destruction. A private C bridge
copies them under the context's bindings lock; the wrapper then constructs
standard C++ values and frees the C snapshot through the same library that
allocated it. `src/ivy_query_internal.h` is not installed and adds no public
query declarations to `ivy.h`. Build the C library and wrapper from matching
sources, because the wrapper uses these internal link symbols. Host resolution
retains the C API's diagnostic strings if a hostname cannot be obtained.

For validation without a subscription, use
`ivy::validate_anchored_regexp(expression)` or the formatted form:

```cpp
auto valid = ivy::validate_anchored_regexp("^TRACK {} (.*)$", 42);
if (!valid) {
    std::cerr << valid.error().message() << '\n';
    return 1;
}
```

Both forms return `std::expected<void, std::error_code>` and do not need a Bus.
Missing/effective unanchoring returns `IVY_EUNANCHORED`; invalid syntax, embedded
NUL or interval expansion errors return `IVY_EINVAL`. Allocation failures return
`IVY_ENOMEM`, and a C library without PCRE2 returns `IVY_ESTATE`. A literal missing
`^` returns an error here, whereas bind_raw() checks its constant regexp at compilation.

### Configuring filters in C++23

`Bus::set_filters()` accepts either a parameter pack or a collection/range whose
elements convert to `std::string_view`. Each call replaces this bus's previous
list; it never appends or affects another bus. All forms return
`std::expected<void, std::error_code>` and are `noexcept`.

```cpp
auto configured = bus.set_filters("TRACK", "STATUS");
if (!configured) {
    std::cerr << configured.error().message() << '\n';
    return 1;
}

std::vector<std::string> classes{"PING", "PONG"};
if (auto changed = bus.set_filters(classes); !changed) {
    std::cerr << changed.error().message() << '\n';
    return 1;
}
```

Arrays, spans, initializer lists (`set_filters({"A", "B"})`) and C++23 input
ranges/views are supported. Single-pass ranges are consumed once and strings
returned temporarily by a view are copied before iteration advances. The
complete replacement is prepared before committing it; input/iteration failures
return `IVY_EINVAL`, allocation failures `IVY_ENOMEM`, preserving the previous
filters. Input storage need not survive the call.

`bus.set_filters()`, an empty collection, or `bus.clear_filters()` clears this
bus's filter list. `add_filter(word)` and `remove_filter(word)` provide checked
incremental changes. As for the C API, updates affect future remote regexp
advertisements. The original received-message subscriptions are unaffected.

### Pong, remote subscriptions and timers

`bind_event()` accepts a selector after its callback:

| Registration | Callback arguments | Returned token |
| --- | --- | --- |
| `bus.bind_event(callback, ivy::pong)` | `IvyClientPtr`, delay as `int` microseconds (negative on timeout) | `EventSubscription` |
| `bus.bind_event(callback, ivy::remote_bindings)` | `IvyClientPtr`, regexp ID, `std::string_view`, `IvyBindEvent` | `EventSubscription` |
| `bus.bind_event(callback, ivy::every(1s))` | `std::chrono::milliseconds` of lateness relative to scheduled expiry | `TimerSubscription` |

All return `std::expected<Token, std::error_code>`. Selectors distinguish event
kinds even with generic lambdas. Keep the result or its token alive. Tokens are
movable, non-copyable, unsubscribe on destruction and may outlive their Bus.
A successful pong bind replaces the previous pong callback; a successful remote
observer bind replaces the previous observer. Old tokens become inactive and
cannot cancel their replacements. The two registrations are independent.
Remote observers report subsequent subscription advertisements/changes/removals;
they do not replay subscriptions already known by the bus. Removal events can
carry empty regexp text: use the `(peer, id)` pair to identify the subscription.

`bus.send_ping(peer)` initiates a ping and returns
`std::expected<void, std::error_code>`. The bus must be running, the peer must
belong to it and a pong subscription must be active. Success means local
acceptance; the reply or timeout is delivered later to the pong callback.

```cpp
using namespace std::chrono_literals;
auto timer = bus.bind_event([](std::chrono::milliseconds lateness) {
    std::cout << "Tick, " << lateness.count() << " ms late\n";
}, ivy::every(1s));
if (!timer) {
    std::cerr << timer.error().message() << '\n';
    return 1;
}
if (auto changed = timer->set_period(500ms); !changed) {
    std::cerr << changed.error().message() << '\n';
    return 1;
}
```

`ivy::every(period)` repeats indefinitely; `ivy::every(period, count)` invokes
the callback a positive number of times. `ivy::after(delay)` invokes it once.
Periods must be positive milliseconds representable as a C `long`; after() also
accepts zero to run at the next native loop opportunity, never synchronously
inside bind_event(). Negative delays and zero/negative repetition counts are errors.
Every registration creates an independent timer on the bus's event loop. Creation, `set_period()` and `unbind()` may be called from another thread
or from within the callback. Changing the period starts a new schedule with the
same captures and number of invocations remaining; it does not reset the count.
A limited timer becomes inactive when its last callback is selected and releases
its captures after that invocation finishes. An expired timer cannot be rearmed
with set_period(), including from its own last callback. Failed rescheduling
preserves the previous schedule, unless it independently expires or is cancelled.
Cancellation disables
future callbacks immediately; one already in progress may finish. Native timers
are retired by the loop at their next expiry, or freed when the Bus is destroyed,
so cancellation never mutates the C timer list from a foreign thread.

Both event and timer tokens provide `unbind()` and `is_bound()`. Timer tokens
also provide `set_period()`; regexp `change()` is not exposed on either type.
Callback failures follow the same `take_callback_error()` contract as the other
callbacks. `examples/cpp/callbacks.cpp` demonstrates all three registrations,
ping initiation and detailed send reports without exception handling.
`examples/cpp/inspection.cpp` demonstrates owned application snapshots, one-shot
and limited timers, and stops its own bus after four seconds.

### Sending messages

After `start()` succeeds, `send()` accepts either raw text or a compile-time
checked `std::format` string followed by its arguments:

```cpp
auto plain = bus.send("100% ready, {} stays literal");
auto formatted = bus.send("TRACK {} {:.2f}", 42, 1.25);
// Both return std::expected<std::size_t, std::error_code>.
if (!formatted)
    std::cerr << formatted.error().message() << '\n';

auto direct = bus.send(peer, 7, "reload");
auto direct_formatted = bus.send(peer, 7, "TRACK {}", 42);
// Direct sends return std::expected<void, std::error_code>.
```

The raw overload accepts `std::string_view`, including views that are not
NUL-terminated. It preserves `%` and braces. Only overloads with formatting
arguments interpret braces. Input views are consumed during the call. Messages
containing NUL, newline or Ivy argument separators return `IVY_EINVAL`;
UTF-8 text is accepted. A direct peer must belong to this bus and remain valid
according to Ivy's borrowed-peer rules. Sends before start or on a moved-from
bus return `IVY_ESTATE`; sends after stop return `IVY_ESTOPPED`.

The successful count is the number of frames accepted locally, with zero a
valid success. Any failure produces an error, even if other frames succeeded.
For broadcasts, use `send_report()` **instead of** `send()` when the partial
counts matter. This method sends the message once to matching remote
subscriptions and returns `ivy::SendReport`; it is not a direct-message send
and does not retrieve a previous send's result:

```cpp
auto report = bus.send_report("TRACK {} {:.2f}", 42, 1.25);
if (report.error)
    std::cerr << report.error.message() << ": " << report.accepted
              << " accepted, " << report.failed << " failed\n";
```

It supports the same raw and formatted overloads. The report holds `matched`,
`accepted`, `failed`, the Ivy `error`, and an optional OS `system_error` as
`std::error_code`. It retains the C API's partial-send and local-acceptance
semantics. Allocation failures map to `IVY_ENOMEM`; `std::format_error` and
unrepresentable lengths map to `IVY_EINVAL`. Other custom formatter failures return
`ivy::make_error_code(ivy::Error::formatter_failed)`. Both raw and formatted
overloads are `noexcept`.

`send_die(peer)` asks one connected peer to terminate. `send_error(peer, id, text)`
sends an Ivy protocol error frame, with a formatted overload such as
`send_error(peer, id, "Unknown command: {}", command)`. Both return
`std::expected<void, std::error_code>`, require a running bus and validate that
the peer belongs to it. Error text follows the same restrictions and formatting
rules as normal sends. Native control sends validate peer membership under the
bindings lock as direct sends already do. Local acceptance is not confirmation
that the peer has received the error or exited.

An optional `std::move_only_function` receives transport failures:

```cpp
auto configured = bus.set_transport_error_callback(
    [name = std::string("sender")]
    (IvyClientPtr peer, std::error_code error, int system_error) {
        std::cerr << name << ": " << error.message()
                  << " (OS " << system_error << ")\n";
    });
```

The setter returns `std::expected<void, std::error_code>`; an empty callable
disables notification. It may be called before start or from the callback.
Replacing it keeps an invocation already in progress and its captures alive
until it returns. Callback failures use the same stop-and-record policy as
the other wrapper callbacks. The C transport callback's threading and
connection-level semantics apply.

`examples/cpp/lifecycle.cpp` demonstrates subscriptions and sending. It runs until
another Ivy application sends a die request. Run the wrapper tests with:

```sh
./tests/cpp/run.sh
```

The tests exercise both static linking and shared linking from a temporary
installation, including the shared library names and dependency on `libivy.so.3`.
They also check compilation rejection, PCRE2 anchoring, format arguments that
introduce alternatives, Ivy interval expansion, unanchored message delivery,
raw/formatted/direct sends, reports, and transport callback ownership/errors.

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

### Debian / Ubuntu packages

The Debian packaging builds two packages: `ivy-c` contains the C and C++23
runtime libraries, and `ivy-c-dev` contains their development files and the
Doxygen PDF reference at `/usr/share/doc/ivy-c-dev/ivy-api.pdf`. Native and
GLib backends are included. The former `ivy-cpp` and `ivy-cpp-dev` packages
are replaced automatically during installation.

```bash
./debian/build.sh
python3 debian/check-packages.py build/debian
```

`make -C src deb` uses the same packaging rules. Builds run in a temporary
source copy and write the packages to `build/debian/`. See
[debian/README](debian/README) for Ubuntu prerequisites, dependencies and
installation instructions.

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
