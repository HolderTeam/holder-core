#!/usr/bin/env python3
"""Archive a CMake-installed libholder SDK and test that archive in isolation."""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import zipfile


def run(*args, env=None):
    subprocess.run(args, check=True, env=env)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--install", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--vcpkg-installed", type=Path)
    parser.add_argument("--vcpkg-root", type=Path)
    args = parser.parse_args()

    manifest = json.loads((args.install / "libholder-manifest.json").read_text())
    if manifest["commit"] != args.commit:
        parser.error("installed SDK commit does not match the checkout")
    if manifest["build_type"] not in ("RelWithDebInfo", "Release"):
        parser.error("unsupported SDK build type")
    for required in ("include/holder/holder.h", "share/holder/schema/schema.sql",
                     "share/holder/resources/WELCOME.md"):
        if not (args.install / required).is_file():
            parser.error(f"installed SDK is missing {required}")
    platform = manifest["platform"]
    library = "holder.lib" if platform == "windows" else "libholder.a"
    if not (args.install / "lib" / library).is_file():
        parser.error(f"installed SDK is missing lib/{library}")
    extension = ".zip" if platform == "windows" else ".tar.gz"
    name = (f"libholder-{manifest['version']}-{platform}-"
            f"{manifest['architecture']}-{manifest['build_type'].lower()}{extension}")
    args.output.mkdir(parents=True, exist_ok=True)
    archive = args.output / name

    with tempfile.TemporaryDirectory(prefix="libholder-sdk-") as temp:
        root = Path(temp)
        staged = root / "libholder-sdk"
        shutil.copytree(args.install, staged)
        if platform == "windows":
            if not args.vcpkg_installed:
                parser.error("Windows SDKs require --vcpkg-installed")
            triplet = args.vcpkg_installed / "x64-windows"
            dlls = list((triplet / "bin").glob("*.dll"))
            if not dlls:
                parser.error(f"no vcpkg runtime DLLs found in {triplet / 'bin'}")
            (staged / "bin").mkdir(exist_ok=True)
            for dll in dlls:
                shutil.copy2(dll, staged / "bin" / dll.name)
            # Runtime libraries carry their vcpkg license notices.
            licenses = staged / "share" / "vcpkg-licenses"
            licenses.mkdir(parents=True)
            for copyright_file in (triplet / "share").glob("*/copyright"):
                shutil.copy2(copyright_file, licenses / f"{copyright_file.parent.name}.txt")

        if platform == "windows":
            with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as package:
                for file in sorted(staged.rglob("*")):
                    if file.is_file():
                        package.write(file, file.relative_to(root))
        else:
            with tarfile.open(archive, "w:gz") as package:
                package.add(staged, arcname="libholder-sdk")

        # Use only files extracted from the archive for the Holder SDK.
        extracted = root / "extracted"
        extracted.mkdir()
        if platform == "windows":
            with zipfile.ZipFile(archive) as package:
                package.extractall(extracted)
        else:
            with tarfile.open(archive, "r:gz") as package:
                package.extractall(extracted)
        sdk = extracted / "libholder-sdk"
        source = root / "consumer"
        source.mkdir()
        (source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.22)\n"
            "project(libholder_sdk_smoke LANGUAGES CXX)\n"
            "find_package(holder CONFIG REQUIRED)\n"
            "add_executable(smoke main.cpp)\n"
            "target_link_libraries(smoke PRIVATE Holder::Core)\n"
            "enable_testing()\nadd_test(NAME smoke COMMAND smoke)\n"
        )
        (source / "main.cpp").write_text(
            '#include <holder/holder.h>\n#include <string>\n'
            'int main() { return std::string(holder_version_string()) == '
            f'"{manifest["version"]}" ? 0 : 1; }}\n'
        )
        build = root / "consumer-build"
        configure = ["cmake", "-S", str(source), "-B", str(build),
                     "-G", "Ninja", f"-DCMAKE_BUILD_TYPE={manifest['build_type']}",
                     f"-Dholder_DIR={sdk / 'lib' / 'cmake' / 'holder'}",
                     "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF",
                     "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF"]
        if platform == "windows":
            if not args.vcpkg_root:
                parser.error("Windows smoke test requires --vcpkg-root")
            configure += [f"-DCMAKE_TOOLCHAIN_FILE={args.vcpkg_root / 'scripts' / 'buildsystems' / 'vcpkg.cmake'}",
                          f"-DVCPKG_INSTALLED_DIR={args.vcpkg_installed}",
                          "-DVCPKG_TARGET_TRIPLET=x64-windows", "-DVCPKG_MANIFEST_MODE=OFF"]
        run(*configure)
        run("cmake", "--build", str(build), "--parallel")
        environment = os.environ.copy()
        if platform == "windows":
            environment["PATH"] = str(sdk / "bin") + os.pathsep + environment["PATH"]
        run("ctest", "--test-dir", str(build), "--output-on-failure", env=environment)
    print(archive)


if __name__ == "__main__":
    main()
