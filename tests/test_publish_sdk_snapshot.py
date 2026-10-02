"""Exercise SDK publication failures and ordering without publishing to GitHub."""

import copy
import importlib.util
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile


SPEC = importlib.util.spec_from_file_location(
    "publish_sdk_snapshot",
    Path(__file__).resolve().parents[1] / "scripts" / "publish-sdk-snapshot.py",
)
publisher = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(publisher)
COMMIT = "a" * 40
OLDER = "b" * 40
NEWER = "c" * 40
REPOSITORY = "HolderTeam/holder-core"


class FakeGitHub:
    def __init__(self, index, latest=None):
        self.repository = REPOSITORY
        self.tags = {} if latest is None else {"latest-green": latest}
        self.releases = {}
        self.stored_files = {}
        self.events = []
        self.main_status = "ahead"
        self.pointer_status = "ahead"
        self.fail_upload = False
        self.corrupt_digest = False
        self.core_run = {
            "status": "completed", "conclusion": "success", "event": "push",
            "head_branch": "main", "head_sha": index["commit"],
            "path": ".github/workflows/core.yml",
            "head_repository": {"full_name": REPOSITORY},
        }

    def api(self, endpoint):
        if endpoint.startswith("actions/runs/"):
            return copy.deepcopy(self.core_run)
        if endpoint.endswith("...main"):
            return {"status": self.main_status}
        if endpoint.startswith("compare/"):
            return {"status": self.pointer_status}
        raise AssertionError(f"Unexpected API request: {endpoint}")

    def tag(self, name):
        return self.tags.get(name)

    def create_tag(self, name, commit):
        self.tags[name] = commit
        self.events.append(("create-tag", name))

    def update_pointer(self, commit):
        self.tags["latest-green"] = commit
        self.events.append(("promote", commit))

    def release(self, tag):
        return copy.deepcopy(self.releases.get(tag))

    def create_draft(self, index):
        tag = index["snapshot_tag"]
        self.events.append(("create-draft", tag))
        self.releases[tag] = {"id": 1, "tag_name": tag, "draft": True,
                              "prerelease": True, "assets": []}
        return self.release(tag)

    def delete_draft_asset(self, asset_id):
        self.events.append(("delete-draft-asset", asset_id))
        for tag, release in self.releases.items():
            for asset in release["assets"]:
                if asset["id"] == asset_id:
                    self.stored_files.pop((tag, asset["name"]))
            release["assets"] = [a for a in release["assets"] if a["id"] != asset_id]

    def upload(self, tag, files):
        self.events.append(("upload", tag))
        for number, path in enumerate(files, start=1):
            self.stored_files[tag, path.name] = path.read_bytes()
            digest = publisher.sha256(path)
            if self.corrupt_digest and path.name != "sdk-index.json":
                digest = "0" * 64
            self.releases[tag]["assets"].append({
                "id": number, "name": path.name, "size": path.stat().st_size,
                "digest": f"sha256:{digest}",
            })
            if self.fail_upload:
                raise RuntimeError("Upload interrupted")

    def download(self, tag, name, output):
        output.write_bytes(self.stored_files[tag, name])

    def publish_release(self, release_id):
        self.events.append(("publish", release_id))
        for release in self.releases.values():
            if release["id"] == release_id:
                release["draft"] = False


class PublicationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.assets = self.root / "assets"
        self.assets.mkdir()
        for platform, architecture, build_type in sorted(publisher.CONFIGURATIONS):
            manifest = {"version": "0.2.0", "commit": COMMIT, "platform": platform,
                        "architecture": architecture, "build_type": build_type}
            extension = ".zip" if platform == "windows" else ".tar.gz"
            path = self.assets / f"libholder-0.2.0-{platform}-{architecture}-{build_type.lower()}{extension}"
            self.write_archive(path, manifest)
        self.index_path = self.root / "sdk-index.json"
        self.index, self.files = self.index_for_commit(COMMIT)
        self.github = FakeGitHub(self.index)

    @staticmethod
    def write_archive(path, manifest):
        contents = json.dumps(manifest).encode()
        if path.suffix == ".zip":
            with zipfile.ZipFile(path, "w") as archive:
                archive.writestr(publisher.MANIFEST, contents)
        else:
            with tarfile.open(path, "w:gz") as archive:
                member = tarfile.TarInfo(publisher.MANIFEST)
                member.size = len(contents)
                archive.addfile(member, io.BytesIO(contents))

    def index_for_commit(self, commit):
        index, files = publisher.build_index(self.assets, REPOSITORY, commit, 1, 2)
        self.index_path.write_text(json.dumps(index))
        return index, files

    def publish(self):
        return publisher.publish_snapshot(self.github, self.index, self.files, self.index_path)

    def test_index_identifies_all_configurations_and_downloads(self):
        self.assertEqual(len(self.index["assets"]), 6)
        self.assertEqual(self.index["snapshot_tag"], f"sdk-{COMMIT}")
        for asset in self.index["assets"]:
            path = self.assets / asset["name"]
            self.assertEqual(asset["sha256"], publisher.sha256(path))
            self.assertEqual(asset["size"], path.stat().st_size)
            self.assertIn(f"/sdk-{COMMIT}/", asset["url"])

    def test_missing_configuration_is_rejected(self):
        self.files[0].unlink()
        with self.assertRaisesRegex(ValueError, "all six"):
            self.index_for_commit(COMMIT)

    def test_mixed_core_commits_are_rejected(self):
        manifest = publisher.archive_manifest(self.files[0])
        manifest["commit"] = OLDER
        self.write_archive(self.files[0], manifest)
        with self.assertRaisesRegex(ValueError, "commit mismatch"):
            self.index_for_commit(COMMIT)

    def test_mixed_versions_are_rejected(self):
        manifest = publisher.archive_manifest(self.files[0])
        manifest["version"] = "0.3.0"
        self.write_archive(self.files[0], manifest)
        with self.assertRaisesRegex(ValueError, "different versions"):
            self.index_for_commit(COMMIT)

    def test_wrong_filename_cannot_disguise_configuration(self):
        manifest = publisher.archive_manifest(self.files[0])
        manifest["build_type"] = "RelWithDebInfo"
        self.write_archive(self.files[0], manifest)
        with self.assertRaises(ValueError):
            self.index_for_commit(COMMIT)

    def test_unexpected_platform_is_rejected(self):
        manifest = publisher.archive_manifest(self.files[0])
        manifest["platform"] = "android"
        self.write_archive(self.files[0], manifest)
        with self.assertRaisesRegex(ValueError, "Unexpected SDK configuration"):
            self.index_for_commit(COMMIT)

    def test_first_snapshot_is_complete_before_pointer_is_created(self):
        self.assertEqual(self.publish(), "promoted")
        release = self.github.release(self.index["snapshot_tag"])
        self.assertFalse(release["draft"])
        self.assertEqual(len(release["assets"]), 7)
        self.assertEqual(self.github.tag("latest-green"), COMMIT)
        self.assertEqual(self.github.events[-1], ("create-tag", "latest-green"))
        self.assertLess(self.github.events.index(("publish", 1)), len(self.github.events) - 1)

    def test_only_exact_green_main_core_run_can_publish(self):
        changes = [{"conclusion": "failure"}, {"status": "in_progress"},
                   {"event": "pull_request"}, {"head_branch": "feature"},
                   {"head_sha": OLDER}, {"path": ".github/workflows/other.yml"},
                   {"head_repository": {"full_name": "Other/core"}}]
        for change in changes:
            with self.subTest(change=change):
                self.github = FakeGitHub(self.index)
                self.github.core_run.update(change)
                with self.assertRaisesRegex(ValueError, "Core CI did not pass"):
                    self.publish()
                self.assertEqual(self.github.events, [])

    def test_candidate_removed_from_main_cannot_publish(self):
        self.github.main_status = "diverged"
        with self.assertRaisesRegex(ValueError, "no longer on main"):
            self.publish()
        self.assertEqual(self.github.events, [])

    def test_partial_upload_preserves_pointer_and_can_be_retried(self):
        self.github.tags["latest-green"] = OLDER
        self.github.fail_upload = True
        with self.assertRaisesRegex(RuntimeError, "interrupted"):
            self.publish()
        self.assertEqual(self.github.tag("latest-green"), OLDER)
        self.assertTrue(self.github.release(self.index["snapshot_tag"])["draft"])
        self.github.fail_upload = False
        self.assertEqual(self.publish(), "promoted")
        self.assertTrue(any(event[0] == "delete-draft-asset" for event in self.github.events))
        self.assertEqual(len(self.github.release(self.index["snapshot_tag"])["assets"]), 7)

    def test_bad_upload_checksum_cannot_publish_or_promote(self):
        self.github.tags["latest-green"] = OLDER
        self.github.corrupt_digest = True
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            self.publish()
        self.assertEqual(self.github.tag("latest-green"), OLDER)
        self.assertTrue(self.github.release(self.index["snapshot_tag"])["draft"])

    def test_newer_candidate_advances_pointer(self):
        self.github.tags["latest-green"] = OLDER
        self.assertEqual(self.publish(), "promoted")
        self.assertEqual(self.github.events[-1], ("promote", COMMIT))

    def test_older_candidate_keeps_newer_pointer(self):
        self.github.tags["latest-green"] = NEWER
        self.github.pointer_status = "behind"
        self.assertEqual(self.publish(), "kept newer latest-green")
        self.assertEqual(self.github.tag("latest-green"), NEWER)
        self.assertFalse(self.github.release(self.index["snapshot_tag"])["draft"])

    def test_diverged_candidate_cannot_move_pointer(self):
        self.github.tags["latest-green"] = NEWER
        self.github.pointer_status = "diverged"
        with self.assertRaisesRegex(ValueError, "have diverged"):
            self.publish()
        self.assertEqual(self.github.tag("latest-green"), NEWER)

    def test_rerun_does_not_replace_a_published_snapshot(self):
        self.publish()
        previous_assets = copy.deepcopy(self.github.stored_files)
        self.github.events.clear()
        # A fresh build of the same source can differ in archive bytes.
        self.files[-1].write_bytes(self.files[-1].read_bytes() + b"fresh-build")
        self.index, self.files = self.index_for_commit(COMMIT)
        self.github.pointer_status = "identical"
        self.assertEqual(self.publish(), "already latest-green")
        self.assertEqual(self.github.events, [])
        self.assertEqual(self.github.stored_files, previous_assets)

    def test_missing_published_archive_cannot_promote(self):
        self.publish()
        self.github.tags["latest-green"] = OLDER
        self.github.releases[self.index["snapshot_tag"]]["assets"].pop(0)
        with self.assertRaisesRegex(ValueError, "Incomplete"):
            self.publish()
        self.assertEqual(self.github.tag("latest-green"), OLDER)

    def test_tag_pointing_elsewhere_cannot_be_reused(self):
        self.github.tags[self.index["snapshot_tag"]] = OLDER
        with self.assertRaisesRegex(ValueError, "points elsewhere"):
            self.publish()
        self.assertEqual(self.github.events, [])

    def test_missing_server_digest_uses_downloaded_checksum(self):
        self.publish()
        for asset in self.github.releases[self.index["snapshot_tag"]]["assets"]:
            asset.pop("digest")
        self.github.pointer_status = "identical"
        self.assertEqual(self.publish(), "already latest-green")

    def test_api_errors_are_not_treated_as_missing_releases(self):
        github = publisher.GitHub(REPOSITORY)
        with patch.object(publisher.subprocess, "run") as run:
            run.return_value.returncode = 1
            run.return_value.stderr = "gh: Unauthorized (HTTP 401)"
            with self.assertRaisesRegex(RuntimeError, "401"):
                github.release("sdk-test")
            run.return_value.stderr = "gh: Not Found (HTTP 404)"
            self.assertIsNone(github.release("sdk-test"))


if __name__ == "__main__":
    unittest.main()
