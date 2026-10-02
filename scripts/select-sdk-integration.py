#!/usr/bin/env python3
"""Select a verified published SDK for downstream integration testing."""

import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import tempfile

spec = importlib.util.spec_from_file_location("publisher", Path(__file__).with_name("publish-sdk-snapshot.py"))
publisher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publisher)


def select_snapshot(github, trigger_index=None, sdk_run_id=None):
    if trigger_index is not None:
        commit = trigger_index["commit"]
        publisher.validate_index(trigger_index, github.repository, commit)
        publisher.require(trigger_index["sdk_run_id"] == sdk_run_id,
                          "Snapshot artifact belongs to a different SDK run")
        run = github.api(f"actions/runs/{sdk_run_id}")
        publisher.require(run["status"] == "completed" and run["conclusion"] == "success"
                          and run["event"] == "workflow_run" and run["head_branch"] == "main"
                          and run["path"] == ".github/workflows/sdk-release.yml"
                          and run["head_repository"]["full_name"] == github.repository,
                          "Trigger is not a successful automatic main SDK publication")
        # workflow_run's head_sha can describe a newer default-branch commit.
        # The checked archive manifests in this run's artifact identify the SDK.
    else:
        commit = github.tag("latest-green")
    publisher.require(isinstance(commit, str) and re.fullmatch(r"[0-9a-f]{40}", commit),
                      "No valid published core commit selected")
    tag = f"sdk-{commit}"
    release = github.release(tag)
    publisher.require(release is not None and not release["draft"],
                      "Selected SDK snapshot is not published")
    if trigger_index is None:
        indexes = [asset for asset in release["assets"] if asset["name"] == "sdk-index.json"]
        publisher.require(len(indexes) == 1 and indexes[0]["size"] < 1024 * 1024,
                          "Missing or oversized SDK snapshot index")
        with tempfile.TemporaryDirectory(prefix="sdk-integration-") as temporary:
            path = Path(temporary) / "sdk-index.json"
            github.download(indexes[0]["id"], path)
            trigger_index = json.loads(path.read_text())
        publisher.validate_index(trigger_index, github.repository, commit)
    published = publisher.verify_release(github, release, trigger_index)
    publisher.require_green_core(github, published)
    return published


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--index", type=Path)
    parser.add_argument("--sdk-run-id", type=int)
    args = parser.parse_args()
    if bool(args.index) != bool(args.sdk_run_id):
        parser.error("--index and --sdk-run-id must be supplied together")
    index = select_snapshot(publisher.GitHub(args.repository),
                            json.loads(args.index.read_text()) if args.index else None,
                            args.sdk_run_id)
    print(f"Verified {index['snapshot_tag']} for Python integration")
    if os.environ.get("GITHUB_OUTPUT"):
        with open(os.environ["GITHUB_OUTPUT"], "a") as output:
            output.write(f"commit={index['commit']}\n")


if __name__ == "__main__":
    main()
