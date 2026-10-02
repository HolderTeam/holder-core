#!/usr/bin/env python3
"""Resolve core once, then fetch a verified immutable SDK for each build host."""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import tarfile
import tempfile
import urllib.request
import zipfile

REPOSITORY = "HolderTeam/holder-core"
API = f"https://api.github.com/repos/{REPOSITORY}/"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_url(url):
    require(url.startswith("https://"), "SDK downloads require HTTPS")
    headers = {"User-Agent": "holder-core-sdk-consumer"}
    # Never send the API token to an asset host or redirect destination.
    if url.startswith(API):
        token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
        if token:
            headers["Authorization"] = f"Bearer {token}"
    with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=120) as response:
        return response.read()


def api(path):
    return json.loads(read_url(API + path))


def resolve(core_ref):
    if core_ref == "latest-green":
        ref = api("git/ref/tags/latest-green")["object"]
        require(ref["type"] == "commit", "latest-green must point directly to a commit")
        commit = ref["sha"]
    else:
        require(re.fullmatch(r"[0-9a-f]{40}|v[0-9]+\.[0-9]+\.[0-9]+(?:[-.][A-Za-z0-9.-]+)?", core_ref),
                "Use latest-green, a full core commit SHA, or a version tag")
        commit = api(f"commits/{core_ref}")["sha"]
    require(re.fullmatch(r"[0-9a-f]{40}", commit), "Invalid resolved core commit")
    tag = core_ref if core_ref.startswith("v") else f"sdk-{commit}"
    release = api(f"releases/tags/{tag}")
    require(not release["draft"], "SDK release is unpublished")
    if tag.startswith("sdk-"):
        indexes = [a for a in release["assets"] if a["name"] == "sdk-index.json"]
        require(len(indexes) == 1, "SDK snapshot index missing")
        index = json.loads(read_url(indexes[0]["browser_download_url"]))
        require(index["schema_version"] == 1 and index["repository"] == REPOSITORY
                and index["commit"] == commit and index["snapshot_tag"] == tag,
                "SDK snapshot index does not match the resolved commit")
        assets = index["assets"]
    else:
        assets = []
        for asset in release["assets"]:
            match = re.fullmatch(r"libholder-(.+)-(linux|macos|windows)-(x86_64|arm64)-(release|relwithdebinfo)\.(tar\.gz|zip)", asset["name"])
            if match:
                version, host, arch, config, _ = match.groups()
                require(asset.get("digest", "").startswith("sha256:"), "Tagged SDK lacks a SHA256 digest")
                assets.append({"name": asset["name"], "platform": host, "architecture": arch,
                               "build_type": {"release": "Release", "relwithdebinfo": "RelWithDebInfo"}[config],
                               "sha256": asset["digest"][7:], "size": asset["size"],
                               "url": asset["browser_download_url"]})
        index = {"version": tag[1:]}
    require(len(assets) == 6, "Expected all six core SDK configurations")
    expected = {(system, arch, config) for system, arch in
                [("linux", "x86_64"), ("macos", "arm64"), ("windows", "x86_64")]
                for config in ["Release", "RelWithDebInfo"]}
    require({(a["platform"], a["architecture"], a["build_type"]) for a in assets} == expected,
            "Incomplete or duplicate core SDK configurations")
    return {"repository": REPOSITORY, "commit": commit, "version": index["version"],
            "release_tag": tag, "assets": assets}


def host():
    systems = {"Linux": "linux", "Darwin": "macos", "Windows": "windows"}
    architectures = {"x86_64": "x86_64", "AMD64": "x86_64", "arm64": "arm64", "aarch64": "arm64"}
    return systems[platform.system()], architectures[platform.machine()]


def selected_asset(selection, system, architecture, build_type):
    matches = [a for a in selection["assets"] if (a["platform"], a["architecture"], a["build_type"])
               == (system, architecture, build_type)]
    require(len(matches) == 1, "No unique SDK for this platform, architecture and configuration")
    asset = matches[0]
    require(re.fullmatch(r"[0-9a-f]{64}", asset["sha256"]), "Invalid SDK checksum")
    require(PurePosixPath(asset["name"]).name == asset["name"], "Invalid SDK asset name")
    require(asset["url"] == f"https://github.com/{REPOSITORY}/releases/download/{selection['release_tag']}/{asset['name']}",
            "Unexpected SDK download URL")
    return asset


def unpack(archive, destination):
    def safe(name):
        path = PurePosixPath(name)
        require(not path.is_absolute() and ".." not in path.parts and "\\" not in name
                and path.parts and path.parts[0] == "libholder-sdk", "Unsafe SDK archive path")
    if archive.name.endswith(".zip"):
        with zipfile.ZipFile(archive) as package:
            for entry in package.infolist():
                safe(entry.filename)
                require((entry.external_attr >> 16) & 0o170000 != 0o120000, "SDK archive contains a symlink")
            package.extractall(destination)
    else:
        with tarfile.open(archive, "r:gz") as package:
            for entry in package.getmembers():
                safe(entry.name)
                require(entry.isfile() or entry.isdir(), "SDK archive contains a link or special file")
            package.extractall(destination, **({"filter": "data"} if hasattr(tarfile, "data_filter") else {}))


def fetch(selection, cache, system, architecture, build_type):
    asset = selected_asset(selection, system, architecture, build_type)
    root = cache / selection["commit"] / system / architecture / build_type
    root.mkdir(parents=True, exist_ok=True)
    archive = root / asset["name"]
    if not archive.exists():
        data = read_url(asset["url"])
        require(len(data) == asset["size"] and hashlib.sha256(data).hexdigest() == asset["sha256"],
                "Downloaded SDK checksum or size mismatch")
        archive.write_bytes(data)
    require(archive.stat().st_size == asset["size"]
            and hashlib.sha256(archive.read_bytes()).hexdigest() == asset["sha256"],
            "Cached SDK checksum or size mismatch; remove the damaged archive and retry")
    with tempfile.TemporaryDirectory(dir=root) as temporary:
        unpack(archive, Path(temporary))
        sdk = Path(temporary) / "libholder-sdk"
        manifest = json.loads((sdk / "libholder-manifest.json").read_text())
        for key, expected in {"commit": selection["commit"], "version": selection["version"],
                              "platform": system, "architecture": architecture, "build_type": build_type}.items():
            require(manifest[key] == expected, f"SDK manifest {key} mismatch")
        require((sdk / "share/holder/schema/schema.sql").is_file(), "SDK schema missing")
        require((sdk / "share/holder/resources/WELCOME.md").is_file(), "SDK welcome resource missing")
        require((sdk / "lib/cmake/holder/holderConfig.cmake").is_file(), "SDK CMake package missing")
        if system == "windows":
            require((sdk / "vcpkg/scripts/buildsystems/vcpkg.cmake").is_file(),
                    "Windows SDK lacks bundled development dependencies; select a newer core snapshot")
        target = root / "libholder-sdk"
        if target.exists():
            shutil.rmtree(target)
        shutil.move(str(sdk), target)
    return target.resolve()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    resolution = commands.add_parser("resolve")
    resolution.add_argument("--core-ref", default=os.environ.get("HOLDER_CORE_REF", "latest-green"))
    resolution.add_argument("--output", type=Path, default=Path("out/core-selection.json"))
    download = commands.add_parser("fetch")
    download.add_argument("--selection", type=Path, default=Path("out/core-selection.json"))
    download.add_argument("--cache", type=Path, default=Path(".core-sdk"))
    download.add_argument("--build-type", choices=["Release", "RelWithDebInfo"], default="RelWithDebInfo")
    download.add_argument("--github-env", action="store_true")
    download.add_argument("--expected-commit", help="Require the run-wide resolved commit")
    args = parser.parse_args()
    if args.command == "resolve":
        selection = resolve(args.core_ref)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(selection, indent=2) + "\n")
        print(selection["commit"])
        if os.environ.get("GITHUB_OUTPUT"):
            with open(os.environ["GITHUB_OUTPUT"], "a") as output:
                output.write(f"commit={selection['commit']}\n")
    else:
        selection = json.loads(args.selection.read_text())
        if args.expected_commit:
            require(selection["commit"] == args.expected_commit, "SDK selection differs from the run-wide core commit")
        sdk = fetch(selection, args.cache, *host(), args.build_type)
        cmake_args = ""
        if platform.system() == "Windows":
            cmake_args += (f' -DCMAKE_TOOLCHAIN_FILE="{sdk.as_posix()}/vcpkg/scripts/buildsystems/vcpkg.cmake"'
                           f' -DVCPKG_INSTALLED_DIR="{sdk.as_posix()}/vcpkg/installed"'
                           " -DVCPKG_TARGET_TRIPLET=x64-windows -DVCPKG_MANIFEST_MODE=OFF -DVCPKG_APPLOCAL_DEPS=OFF")
        if args.github_env:
            with open(os.environ["GITHUB_ENV"], "a") as output:
                output.write(f"HOLDER_CORE_SDK={sdk.as_posix()}\nCMAKE_ARGS={cmake_args}\nHOLDER_CORE_BUILD_TYPE={args.build_type}\n")
        if os.environ.get("GITHUB_OUTPUT"):
            with open(os.environ["GITHUB_OUTPUT"], "a") as output:
                output.write(f"sdk={sdk.as_posix()}\n")
        print(sdk)


if __name__ == "__main__":
    main()
