"""Select published SDKs without confusing moving workflow refs or reruns."""

import copy
import importlib.util
import json
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("integration", Path(__file__).parents[1] / "scripts/select-sdk-integration.py")
integration = importlib.util.module_from_spec(spec)
spec.loader.exec_module(integration)
COMMIT = "a" * 40
REPOSITORY = "HolderTeam/holder-core"


class GitHub:
    repository = REPOSITORY

    def __init__(self):
        assets = []
        for system, arch, config in integration.publisher.CONFIGURATIONS:
            extension = ".zip" if system == "windows" else ".tar.gz"
            name = f"libholder-0.2.0-{system}-{arch}-{config.lower()}{extension}"
            assets.append({"platform": system, "architecture": arch, "build_type": config,
                           "name": name, "sha256": "b" * 64, "size": 1,
                           "url": f"https://github.com/{REPOSITORY}/releases/download/sdk-{COMMIT}/{name}"})
        self.index = {"schema_version": 1, "repository": REPOSITORY, "commit": COMMIT,
                      "snapshot_tag": f"sdk-{COMMIT}", "version": "0.2.0", "assets": assets,
                      "core_run_id": 1, "sdk_run_id": 2}
        self.tags = {"latest-green": COMMIT, f"sdk-{COMMIT}": COMMIT}
        self.sdk_run = {"status": "completed", "conclusion": "success", "event": "workflow_run",
                        "path": ".github/workflows/sdk-release.yml", "head_branch": "main",
                        "head_sha": "c" * 40, "head_repository": {"full_name": REPOSITORY}}
        self.core_run = {**self.sdk_run, "event": "push", "path": ".github/workflows/core.yml",
                         "head_sha": COMMIT}
        self.release_data = {"draft": False, "prerelease": True, "tag_name": f"sdk-{COMMIT}",
                             "assets": [{"id": 7, "name": "sdk-index.json", "size": 100}] +
                             [{"id": i, "name": a["name"], "size": 1, "digest": "sha256:" + a["sha256"]}
                              for i, a in enumerate(assets)]}

    def api(self, path):
        if path == "actions/runs/3":
            return self.sdk_run
        if path == "actions/runs/1":
            return self.core_run
        if path == f"compare/{COMMIT}...main":
            return {"status": "identical"}
        raise AssertionError(f"Unexpected API call: {path}")

    def tag(self, name):
        return self.tags.get(name)

    def release(self, tag):
        return self.release_data if tag == f"sdk-{COMMIT}" else None

    def download(self, asset_id, output):
        assert asset_id == 7
        output.write_text(json.dumps(self.index))


class SelectionTests(unittest.TestCase):
    def setUp(self):
        self.github = GitHub()
        self.artifact = {**copy.deepcopy(self.github.index), "sdk_run_id": 3, "core_run_id": None}

    def test_manual_selection_verifies_latest_green(self):
        self.assertEqual(integration.select_snapshot(self.github)["commit"], COMMIT)

    def test_artifact_selects_sdk_instead_of_workflow_default_branch_head(self):
        result = integration.select_snapshot(self.github, self.artifact, 3)
        self.assertEqual(result["commit"], COMMIT)
        self.assertNotEqual(result["commit"], self.github.sdk_run["head_sha"])

    def test_rerun_reuses_original_immutable_published_index(self):
        result = integration.select_snapshot(self.github, self.artifact, 3)
        self.assertEqual(result["sdk_run_id"], 2)

    def test_artifact_from_another_sdk_run_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "different SDK run"):
            integration.select_snapshot(self.github, self.artifact, 4)

    def test_failed_or_manual_sdk_run_cannot_trigger_publication_integration(self):
        for change in [{"conclusion": "failure"}, {"event": "workflow_dispatch"}, {"head_branch": "feature"}]:
            with self.subTest(change=change):
                self.github.sdk_run = {**GitHub().sdk_run, **change}
                with self.assertRaisesRegex(ValueError, "successful automatic"):
                    integration.select_snapshot(self.github, self.artifact, 3)

    def test_unpublished_release_is_rejected(self):
        self.github.release_data["draft"] = True
        with self.assertRaisesRegex(ValueError, "not published"):
            integration.select_snapshot(self.github, self.artifact, 3)

    def test_failed_core_run_is_rejected(self):
        self.github.core_run["conclusion"] = "failure"
        with self.assertRaisesRegex(ValueError, "Core CI did not pass"):
            integration.select_snapshot(self.github, self.artifact, 3)

    def test_incomplete_published_sdk_is_rejected(self):
        self.github.release_data["assets"].pop()
        with self.assertRaisesRegex(ValueError, "Incomplete"):
            integration.select_snapshot(self.github, self.artifact, 3)

    def test_missing_latest_green_is_rejected(self):
        self.github.tags.pop("latest-green")
        with self.assertRaisesRegex(ValueError, "No valid published"):
            integration.select_snapshot(self.github)


if __name__ == "__main__":
    unittest.main()
