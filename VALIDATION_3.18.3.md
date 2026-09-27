# Ivy 3.18.3 — release validation

Validated on Ubuntu 26.04.1 amd64, 27 September 2026.
Release compiler: GCC 15.2.0. Sanitizer evidence: Clang 22.1.2.

## Change

The C++ wrapper owns its IvyContext through a unique_ptr with a custom deleter.
It explicitly resets the context before destroying callback storage and locks,
because C destruction can invoke disconnect callbacks. Exception handling in
those callbacks does not attempt to stop the context after ownership was reset.
Subscription and timer captures are released even if their tokens outlive Bus.

The public API and shared-library SONAMEs are unchanged. native_handle() still
returns a borrowed pointer. The release updates the patch version to 3.18.3 in
version.h, makefiles, pkg-config metadata, Debian packages and the PDF reference.

## Validation performed for this release

| Check | Result |
| --- | --- |
| GCC C++ native suite, tests/cpp/run.sh | Passed |
| GCC C++ GLib suite, tests/cpp/run_glib.sh | Passed |
| Packaged C/C++ native/GLib, static/shared consumers | 8/8 passed |
| pprzlink rebuilt with 3.18.3 package headers/libraries | 12/12 passed |
| Package metadata, SONAMEs, dependencies and C/C++ PDF | Passed |
| APT installation simulation | Only ivy-c and ivy-c-dev upgraded; no removals |
| git diff --check | Passed |

The native suite covers 51 rejected-compilation cases, installed API headers,
examples, callback boundaries, static/shared consumers, timers and loop threads.
The new destruction regression checks disconnect callbacks (including one that
throws), callback-capture lifetimes, tokens surviving Bus and exactly one
context destruction.

Package checks use extracted packages and do not replace the system installation.
pprzlink compiler commands reference the extracted headers; ldd confirms that
both Ivy libraries are loaded from the same extracted package directory.

## Existing sanitizer validation reused

The ten changed C++ source/test files were compared byte-for-byte with the
private GCC, ASan/UBSan/LeakSanitizer and ThreadSanitizer builds tested earlier
in this session. They match. Their comparison hashes and original logs are
retained with the release evidence.

Those runs passed the callback-boundary/destruction regression, native
static/shared integration and main-loop tests, and the GLib C++ suite, without
sanitizer diagnostics. The most recent pprzlink runs also passed 12/12 tests
under each Clang sanitizer configuration using these same private Ivy builds.

Instrumentation and the privately instrumented GLib used by TSan are described
in VALIDATION_3.18.2.md. This release does not claim a new sanitizer build solely
for the version-metadata changes.

## Artifacts and reproduction

Packages and checksums are under build/debian/3.18.3/.
Release scripts, logs, source hashes and sanitizer evidence are retained under
build/debian/3.18.3/validation/.

The temporary validation workspace is /tmp/ivy-release-3.18.3-7y05d_5e/.
Its gcc-src copy keeps test-generated files out of the working repository.
The package builder also uses a disposable source copy.

From the repository root:

    IVY_DEB_OUTPUT_DIR="$PWD/build/debian/3.18.3" sh debian/build.sh -j4
    python3 debian/check-packages.py build/debian/3.18.3
    sh tests/cpp/run.sh
    sh tests/cpp/run_glib.sh

The archived validate-packages.py script rebuilds and tests the local pprzlink
checkout against extracted packages; its local dependency paths must exist.

No Windows, macOS, Qt, GCC 13/Ubuntu 24.04 or physical XBee validation was performed.
