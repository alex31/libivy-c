# Ivy 3.18.2 — sanitizer validation

Validation on Ubuntu 26.04.1 (amd64), 27 September 2026.
Compilers: Clang 22.1.2 and GCC 15.2.0.

## Defects corrected

- GLib writable-event requests from another thread modified a poll mask while
  GLib was reading it. Requests now update Ivy metadata under its mutex, wake
  the context, and apply the poll mask in the source's prepare callback.
- The legacy default channel/socket states were skipped during destruction.
  Termination now closes sockets, releases subscriptions and timers, discards
  queued controls, and resets the retained default state for reinitialization.
  Context data remains alive until disconnect callbacks have finished.
- Bulk regexp dictionary cleanup now frees each entry using a traversal that
  saves the next entry before deletion.
- ivythroughput borrows the bus address from argv/environment instead of
  leaking a duplicated string.

## Regression coverage

- tests/run_legacy_cleanup.sh exchanges real Ivy messages between a legacy
  context and an explicit context, stops from a message callback, and repeats
  initialization/termination twenty times with both native and GLib backends.
  It checks disconnect callbacks, stopped-context behavior during cleanup,
  cancellation of pending timers/controls, idempotent termination, and stable
  file-descriptor counts on Linux.
- The same test fails against the installed 3.18.1 library: the expected
  disconnect callback is absent.
- tests/run_glib_backend.sh now performs one hundred write requests from outside
  the GLib thread, on both default and private main contexts, with static and
  shared libraries. A send concurrent with an explicitly requested stop may
  return IVY_ESTOPPED; other errors still fail the test.
- The pre-existing phase5 callback-safety and phase3/phase10 throughput tests
  cover the original sanitizer failures.

## Instrumentation

Separate private builds instrument the C core, C++ wrapper and consumers with:

- ASan/UBSan: -fsanitize=address,undefined
- TSan: -fsanitize=thread
- Both: -g -O1 -fno-omit-frame-pointer -fno-sanitize-recover=all

ASAN_OPTIONS enables detect_leaks=1 and halt_on_error=1.
UBSAN_OPTIONS and TSAN_OPTIONS enable halt_on_error=1; LLVM 22 supplies the
symbolizer. No sanitizer suppressions are used. A deliberately racy two-thread
control program must be diagnosed by TSan before the test suites run.

The builds use compiler wrappers for cc/gcc/c++/g++ because some existing shell
tests invoke those names directly. Clang's instrumented shared libraries resolve
sanitizer symbols in the executable; --no-undefined is omitted in these private
builds only. The pthread_create failure-injection shim remains uninstrumented
because it is also preloaded into the uninstrumented timeout utility; the
corresponding Ivy test executables and libraries remain instrumented.

TSan validation uses GLib 2.88.0 rebuilt with Clang 22 and -Db_sanitize=thread,
loaded through LD_LIBRARY_PATH. System GLib's opaque GMutex implementation
otherwise causes misleading reports on correctly locked Ivy control queues.
Instrumenting GLib removes those reports and exposes the actual poll-mask race
fixed here.

## Results

All selected validation checks pass, without sanitizer diagnostics:

| Coverage | Result |
| --- | --- |
| Ivy ASan/UBSan/LSan | 22 shell suites passed |
| Ivy TSan | 21 shell suites plus the binding-thread cleanup test passed |
| pprzlink ASan/UBSan/LSan | 11/11 tests passed |
| pprzlink TSan | 11/11 tests passed |
| pprzlink GCC 15, using headers and libraries extracted from the 3.18.2 packages | 11/11 tests passed |
| Packaged C/C++ native/GLib, static/shared consumers | 8/8 passed |

The C++ suite includes 51 rejected-compilation cases, standalone installed-header
checks, examples, static/shared consumers, callbacks, timers and loop threads.
TSan's binding cleanup test is compiled separately because its existing shell
runner specifically selects ASan.

Both Debian packages, version metadata, SONAMEs, dependencies and the complete
C/C++ PDF reference are verified by debian/check-packages.py.
An apt-get simulation upgrades only ivy-c and ivy-c-dev, removing no package.
The package libraries and headers are used directly from an extracted staging
directory for the pprzlink GCC build; the system installation is not modified.

Local logs and the validation harness are retained under
build/debian/3.18.2/validation/. Focused regressions can be rerun from the repository:

    sh tests/run_legacy_cleanup.sh
    sh tests/run_glib_backend.sh
    sh tests/run_phase5.sh
    sh tests/run_transport_errors.sh
    python3 debian/check-packages.py build/debian/3.18.2

The sanitizer runs additionally require the instrumentation setup described above.

OpenMP transport, partial fan-out, FIFO and deferred-error tests also pass with
GCC 15, together with the legacy-cleanup, GLib-backend and phase5 tests.

This validation does not cover Windows, macOS, Qt, real radio hardware or GCC 13.
OpenMP is exercised with GCC; it is not part of the Clang sanitizer runs.
