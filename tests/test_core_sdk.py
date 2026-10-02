"""SDK selection, cache integrity and provenance checks without network access."""

import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path, PureWindowsPath
import tarfile
import tempfile
import unittest
from unittest.mock import patch
from typing import Any, cast

spec = importlib.util.spec_from_file_location("core_sdk", Path(__file__).parents[1] / "scripts/core-sdk.py")
assert spec is not None and spec.loader is not None
sdk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sdk)
COMMIT = "a" * 40


class SDKTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.manifest = {"commit": COMMIT, "version": "0.2.0", "platform": "linux",
                         "architecture": "x86_64", "build_type": "RelWithDebInfo"}
        self.selection: dict[str, Any] = {"commit": COMMIT, "version": "0.2.0", "release_tag": f"sdk-{COMMIT}",
                          "repository": sdk.REPOSITORY, "assets": []}

    def archive(self, manifest: dict[str, str] | None = None, extra: dict[str, str] | None = None) -> bytes:
        stream = io.BytesIO()
        with tarfile.open(fileobj=stream, mode="w:gz") as package:
            files = {"libholder-sdk/libholder-manifest.json": json.dumps(manifest or self.manifest),
                     "libholder-sdk/share/holder/schema/schema.sql": "schema",
                     "libholder-sdk/share/holder/resources/WELCOME.md": "welcome",
                     "libholder-sdk/lib/cmake/holder/holderConfig.cmake": "cmake"}
            if extra:
                files.update(extra)
            for name, contents in files.items():
                data = contents.encode()
                entry = tarfile.TarInfo(name)
                entry.size = len(data)
                package.addfile(entry, io.BytesIO(data))
        data = stream.getvalue()
        name = "libholder-0.2.0-linux-x86_64-relwithdebinfo.tar.gz"
        asset = {"name": name, "platform": "linux", "architecture": "x86_64",
                 "build_type": "RelWithDebInfo", "size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                 "url": f"https://github.com/{sdk.REPOSITORY}/releases/download/sdk-{COMMIT}/{name}"}
        self.selection["assets"] = [asset]
        return data

    def fetch(self) -> Path:
        return cast(Path, sdk.fetch(self.selection, self.root, "linux", "x86_64", "RelWithDebInfo"))

    def test_cached_archive_is_rechecked_and_extracted_manifest_is_restored(self) -> None:
        data = self.archive()
        with patch.object(sdk, "read_url", return_value=data) as download:
            installed = self.fetch()
            (installed / "libholder-manifest.json").write_text("damaged")
            self.fetch()
            self.assertEqual(download.call_count, 1)
            self.assertEqual(json.loads((installed / "libholder-manifest.json").read_text()), self.manifest)

    def test_corrupt_download_is_not_cached(self) -> None:
        self.archive()
        with patch.object(sdk, "read_url", return_value=b"damaged"):
            with self.assertRaisesRegex(ValueError, "checksum"):
                self.fetch()
        self.assertEqual(list(self.root.rglob("*.tar.gz")), [])

    def test_corrupt_cache_is_rejected_without_network_fallback(self) -> None:
        data = self.archive()
        with patch.object(sdk, "read_url", return_value=data):
            self.fetch()
        next(self.root.rglob("*.tar.gz")).write_bytes(b"damaged")
        with patch.object(sdk, "read_url", side_effect=AssertionError("unexpected download")):
            with self.assertRaisesRegex(ValueError, "Cached SDK checksum"):
                self.fetch()

    def test_mismatched_commit_and_configuration_are_rejected(self) -> None:
        for key, value in [("commit", "b" * 40), ("build_type", "Release")]:
            with self.subTest(key=key):
                data = self.archive({**self.manifest, key: value})
                with patch.object(sdk, "read_url", return_value=data):
                    with self.assertRaisesRegex(ValueError, f"manifest {key}"):
                        self.fetch()
                next(self.root.rglob("*.tar.gz")).unlink()

    def test_archive_cannot_escape_destination(self) -> None:
        data = self.archive(extra={"libholder-sdk/../../escaped": "unsafe"})
        with patch.object(sdk, "read_url", return_value=data):
            with self.assertRaisesRegex(ValueError, "Unsafe SDK archive"):
                self.fetch()
        self.assertFalse((self.root / "escaped").exists())

    def test_unknown_host_configuration_fails(self) -> None:
        self.archive()
        with self.assertRaisesRegex(ValueError, "No unique SDK"):
            sdk.selected_asset(self.selection, "linux", "arm64", "RelWithDebInfo")

    def test_unexpected_download_host_fails(self) -> None:
        self.archive()
        self.selection["assets"][0]["url"] = "https://example.com/archive"
        with self.assertRaisesRegex(ValueError, "Unexpected SDK download URL"):
            self.fetch()

    def test_latest_green_is_resolved_once_into_immutable_release(self) -> None:
        assets = [{"platform": system, "architecture": arch, "build_type": config}
                  for system, arch in [("linux", "x86_64"), ("macos", "arm64"), ("windows", "x86_64")]
                  for config in ["Release", "RelWithDebInfo"]]
        index = {"schema_version": 1, "repository": sdk.REPOSITORY, "commit": COMMIT,
                 "snapshot_tag": f"sdk-{COMMIT}", "version": "0.2.0", "assets": assets}
        release = {"draft": False, "assets": [{"name": "sdk-index.json", "browser_download_url": "https://example.com/index"}]}
        with patch.object(sdk, "api", side_effect=[{"object": {"sha": COMMIT, "type": "commit"}}, release]) as api:
            with patch.object(sdk, "read_url", return_value=json.dumps(index).encode()):
                result = sdk.resolve("latest-green")
        self.assertEqual(result["commit"], COMMIT)
        self.assertEqual(api.call_args_list[1].args, (f"releases/tags/sdk-{COMMIT}",))

    def test_arbitrary_branch_is_not_a_release_pin(self) -> None:
        with self.assertRaisesRegex(ValueError, "full core commit SHA"):
            sdk.resolve("main")

    def test_version_tag_is_resolved_to_commit_and_checksums(self) -> None:
        assets = [{"name": f"libholder-0.2.0-{system}-{arch}-{config}.tar.gz", "digest": "sha256:" + "a" * 64,
                   "size": 1, "browser_download_url": "https://example.com/archive"}
                  for system, arch in [("linux", "x86_64"), ("macos", "arm64"), ("windows", "x86_64")]
                  for config in ["release", "relwithdebinfo"]]
        with patch.object(sdk, "api", side_effect=[{"sha": COMMIT}, {"draft": False, "assets": assets}]):
            result = sdk.resolve("v0.2.0")
        self.assertEqual(result["release_tag"], "v0.2.0")
        self.assertEqual(result["assets"][0]["sha256"], "a" * 64)

    def test_windows_environment_uses_cmake_safe_sdk_paths(self) -> None:
        selection = self.root / "selection.json"
        selection.write_text(json.dumps(self.selection))
        environment = self.root / "github-env"
        windows_sdk = PureWindowsPath("D:/a/holder-python/.core-sdk/libholder-sdk")
        with patch("sys.argv", ["core-sdk.py", "fetch", "--selection", str(selection), "--github-env"]):
            with patch.object(sdk, "fetch", return_value=windows_sdk), patch.object(sdk, "host", return_value=("windows", "x86_64")):
                with patch.object(sdk.platform, "system", return_value="Windows"), patch.dict(os.environ, {"GITHUB_ENV": str(environment)}):
                    sdk.main()
        lines = environment.read_text().splitlines()
        self.assertEqual(lines[0], "HOLDER_CORE_SDK=D:/a/holder-python/.core-sdk/libholder-sdk")
        self.assertNotIn("\\", lines[1])
        self.assertIn("-DVCPKG_APPLOCAL_DEPS=OFF", lines[1])

    def test_fetch_rejects_selection_from_another_run_before_downloading(self) -> None:
        selection = self.root / "selection.json"
        selection.write_text(json.dumps(self.selection))
        with patch("sys.argv", ["core-sdk.py", "fetch", "--selection", str(selection), "--expected-commit", "b" * 40]):
            with patch.object(sdk, "fetch", side_effect=AssertionError("unexpected download")):
                with self.assertRaisesRegex(ValueError, "run-wide core commit"):
                    sdk.main()

    def test_windows_package_configs_use_their_own_installation(self) -> None:
        triplet = self.root / "sdk/vcpkg/installed/x64-windows"
        config = triplet / "share/unofficial-sodium/unofficial-sodiumConfig.cmake"
        config.parent.mkdir(parents=True)
        config.write_text('set(library "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/sodium.lib")\n')
        sdk.relocate_windows_packages(triplet)
        self.assertEqual(config.read_text(), 'set(library "${CMAKE_CURRENT_LIST_DIR}/../../lib/sodium.lib")\n')
        first = config.read_text()
        sdk.relocate_windows_packages(triplet)
        self.assertEqual(config.read_text(), first)


if __name__ == "__main__":
    unittest.main()
