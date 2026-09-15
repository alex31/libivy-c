#!/usr/bin/env python3
"""Check the four Ivy binary packages without installing them on the host."""

import argparse
import io
import os
from pathlib import Path
import re
import shlex
import subprocess
import tarfile
import tempfile
from email.parser import Parser

PACKAGES = {"ivy-c", "ivy-c-dev", "ivy-cpp", "ivy-cpp-dev"}
LIBRARIES = ("ivy", "glibivy", "ivy-cpp", "ivy-cpp-glib")


def run(*args, env=None):
    return subprocess.check_output(args, text=True, env=env).strip()


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def package_files(package):
    data = subprocess.check_output(["dpkg-deb", "--fsys-tarfile", str(package)])
    with tarfile.open(fileobj=io.BytesIO(data)) as archive:
        members = archive.getmembers()
    require(all(m.uid == 0 and m.gid == 0 for m in members),
            f"{package.name}: package files must belong to root")
    return {m.name.removeprefix("./") for m in members if not m.isdir()}


def inspect_packages(directory, extracted):
    packages, metadata, owners = {}, {}, {}
    for package in sorted(directory.glob("*.deb")):
        fields = Parser().parsestr(run("dpkg-deb", "--field", str(package)))
        name = fields["Package"]
        require(name in PACKAGES, f"Unexpected package: {package.name}")
        require(name not in packages, f"Multiple versions/architectures of {name}")
        packages[name] = package
        metadata[name] = fields
    require(set(packages) == PACKAGES, f"Expected {sorted(PACKAGES)}, found {sorted(packages)}")
    versions = {m["Version"] for m in metadata.values()}
    architectures = {m["Architecture"] for m in metadata.values()}
    require(len(versions) == 1 and len(architectures) == 1, "Package versions/architectures differ")
    version = versions.pop()
    require(architectures == {run("dpkg", "--print-architecture")},
            "Run consumer checks on the architecture used to build the packages")
    numbers = re.match(r"(?:\d+:)?(\d+)\.(\d+)", version)
    require(numbers, f"Unrecognized Ivy version: {version}")
    major, minor = numbers.groups()

    dependencies = {
        "ivy-c-dev": ["ivy-c"],
        "ivy-cpp": ["ivy-c"],
        "ivy-cpp-dev": ["ivy-cpp", "ivy-c-dev"],
    }
    for name, required in dependencies.items():
        depends = metadata[name].get("Depends", "")
        for dependency in required:
            require(re.search(r"\b" + re.escape(dependency) + r"\s*\(=\s*" + re.escape(version) + r"\)", depends),
                    f"{name}: missing exact dependency on {dependency} {version}")
    require("libstdc++6" in metadata["ivy-cpp"].get("Depends", ""),
            "C++ runtime dependency was not generated")
    require("libpcre2-dev" in metadata["ivy-c-dev"].get("Depends", ""),
            "Static C consumers need libpcre2-dev")

    for name, package in packages.items():
        files = package_files(package)
        for filename in files:
            require(filename not in owners, f"File conflict: {filename} ({name}, {owners.get(filename)})")
            owners[filename] = name
            require(not filename.startswith("usr/lib/debug/"), f"Unexpected debug artifact: {filename}")
            require(not filename.endswith(("_internal.h", "ivy_internal.hpp")), f"Private header shipped: {filename}")
            if filename.startswith("usr/include/"):
                expected = "ivy-cpp-dev" if filename.endswith(".hpp") else "ivy-c-dev"
                require(name == expected, f"Header in wrong package: {filename}")
        subprocess.run(["dpkg-deb", "--extract", str(package), str(extracted)], check=True)
        controls = extracted.parent / (name + "-control")
        subprocess.run(["dpkg-deb", "--control", str(package), str(controls)], check=True)
        if name in ("ivy-c", "ivy-cpp"):
            require((controls / "shlibs").is_file(), f"{name}: missing shared-library metadata")
            require("ldconfig" in (controls / "triggers").read_text(), f"{name}: missing ldconfig trigger")
        print(f"PASS {name} {version}: {len(files)} files; Depends: {metadata[name].get('Depends', '(none)')}")

    required = {"usr/include/Ivy/ivy.h": "ivy-c-dev", "usr/include/Ivy/timer.h": "ivy-c-dev"}
    for filename in ("ivy.hpp", "ivy_detail.hpp", "ivy_bus_private.hpp", "ivy_thread.hpp", "ivy_glib.hpp"):
        required["usr/include/Ivy/" + filename] = "ivy-cpp-dev"
    for filename in ("application_types", "applications", "callbacks", "filters", "lifecycle", "mainloop",
                     "messages", "regexp", "results", "send", "subscriptions", "timer_types", "timers"):
        required[f"usr/include/Ivy/api/{filename}.hpp"] = "ivy-cpp-dev"
    for library in LIBRARIES:
        runtime = "ivy-cpp" if library.startswith("ivy-cpp") else "ivy-c"
        dev = runtime + "-dev"
        for suffix, owner in ((f".so.{major}.{minor}", runtime), (f".so.{major}", runtime), (".so", dev), (".a", dev)):
            required[f"usr/lib/lib{library}{suffix}"] = owner
        for suffix in (".so", f".so.{major}"):
            link = extracted / f"usr/lib/lib{library}{suffix}"
            require(link.is_symlink() and os.readlink(link) == f"lib{library}.so.{major}.{minor}", f"Invalid library symlink: {link.name}")
        elf = run("readelf", "-d", str(extracted / f"usr/lib/lib{library}.so"))
        require(f"[lib{library}.so.{major}]" in elf, f"Wrong SONAME for {library}")
        require("(RPATH)" not in elf and "(RUNPATH)" not in elf, f"Build path embedded in {library}")
        if library.startswith("ivy-cpp"):
            backend = "glibivy" if library.endswith("glib") else "ivy"
            other = "ivy" if backend == "glibivy" else "glibivy"
            require(f"[lib{backend}.so.{major}]" in elf and f"[lib{other}.so.{major}]" not in elf,
                    f"Wrong C backend linked by {library}")
    for pc in ("ivy-c", "ivy-glib", "ivy-tcl", "ivy-cpp", "ivy-cpp-glib"):
        required[f"usr/lib/pkgconfig/{pc}.pc"] = "ivy-cpp-dev" if pc.startswith("ivy-cpp") else "ivy-c-dev"
    for filename, package in required.items():
        require(owners.get(filename) == package, f"{filename}: expected in {package}, found {owners.get(filename)}")
    for filename in owners:
        path = extracted / filename
        require(not path.is_symlink() or path.exists(), f"Broken link after package extraction: {filename}")


C_SOURCE = r'''
#include <Ivy/ivy.h>
int main(void) {
    IvyContext *context = IvyContextCreate("deb-c-consumer", 0, 0, 0, 0, 0);
    if (!context) return 1;
    return IvyContextDestroy(context) == IVY_OK ? 0 : 2;
}
'''
CPP_SOURCE = r'''
#include <Ivy/api/filters.hpp>
#include <Ivy/ivy_thread.hpp>
#ifdef IVY_CHECK_GLIB
#include <Ivy/ivy_glib.hpp>
#endif
int main() {
#ifdef IVY_CHECK_GLIB
    auto created = ivy::glib::create_bus("deb-cpp-consumer");
#else
    auto created = ivy::Bus::create("deb-cpp-consumer");
#endif
    if (!created) return 1;
    auto& bus = *created;
    if (!bus.set_filters("CHECK")) return 2;
    auto messages = bus.bind([](IvyClientPtr, auto) {}, "^CHECK (.*)$");
    auto timer = bus.bind([](auto) {}, ivy::after(std::chrono::milliseconds(1)));
    auto applications = bus.applications();
    if (!messages || !timer || !applications || !applications->empty()) return 3;
    if (!ivy::validate_anchored_regexp("^CHECK {}$", 42)) return 4;
    return bus.take_callback_error() ? 0 : 5;
}
'''


def check_consumers(extracted):
    # GLib's headers belong to the host build dependencies, not the Ivy packages.
    glib_cflags = shlex.split(run("pkg-config", "--cflags", "glib-2.0"))
    system_pc = run("pkg-config", "--variable=pc_path", "pkg-config")
    env = dict(os.environ, PKG_CONFIG_PATH="", PKG_CONFIG_LIBDIR=f"{extracted}/usr/lib/pkgconfig:{system_pc}",
               PKG_CONFIG_SYSROOT_DIR=str(extracted), LD_LIBRARY_PATH=f"{extracted}/usr/lib", G_DEBUG="fatal-warnings")
    for pc in ("ivy-c", "ivy-glib", "ivy-cpp", "ivy-cpp-glib"):
        cpp, glib = "cpp" in pc, "glib" in pc
        compiler = shlex.split(os.environ.get("CXX" if cpp else "CC", "c++" if cpp else "cc"))
        source = extracted.parent / (pc + (".cpp" if cpp else ".c"))
        source.write_text(CPP_SOURCE if cpp else C_SOURCE)
        cflags = shlex.split(run("pkg-config", "--cflags", pc, env=env))
        if glib:
            cflags += glib_cflags + (["-DIVY_CHECK_GLIB"] if cpp else [])
        for static in (False, True):
            mode = "static" if static else "shared"
            flags = shlex.split(run("pkg-config", *( ["--static"] if static else []), "--libs", pc, env=env))
            if static:
                flags = [str(extracted / f"usr/lib/lib{flag[2:]}.a") if flag in {"-l" + lib for lib in LIBRARIES} else flag for flag in flags]
            executable = extracted.parent / f"{pc}-{mode}"
            subprocess.run([*compiler, "-std=c++23" if cpp else "-std=c11", *cflags, str(source), *flags,
                            "-o", str(executable)], check=True, env=env)
            dynamic = run("readelf", "-d", str(executable))
            ivy_needed = re.findall(r"\[(lib(?:ivy|glibivy)[^\]]*\.so[^\]]*)\]", dynamic)
            require(bool(ivy_needed) != static, f"{pc}: unexpected {mode} linkage")
            if not static:
                loaded = run("ldd", str(executable), env=env)
                for line in loaded.splitlines():
                    if re.match(r"\s*lib(?:ivy|glibivy)", line):
                        require(f"=> {extracted}/usr/lib/" in line, f"Host Ivy library used: {line}")
            subprocess.run([str(executable)], check=True, env=env, timeout=15)
            print(f"PASS {pc}: compile and run with {mode} Ivy libraries from the packages")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, help="directory containing one version of the four .deb files")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="ivy-deb-check.") as temporary:
        extracted = Path(temporary) / "root"
        inspect_packages(args.directory.resolve(), extracted)
        check_consumers(extracted)
    print("All four packages and eight installed-consumer checks passed.")


if __name__ == "__main__":
    main()
