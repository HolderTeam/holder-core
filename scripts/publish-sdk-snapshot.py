#!/usr/bin/env python3
"""Validate a complete SDK snapshot, publish it, and advance latest-green."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile
import zipfile


CONFIGURATIONS = {
    (platform, architecture, build_type)
    for platform, architecture in
    (("linux", "x86_64"), ("macos", "arm64"), ("windows", "x86_64"))
    for build_type in ("Release", "RelWithDebInfo")
}
MANIFEST = "libholder-sdk/libholder-manifest.json"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def archive_manifest(path):
    if path.name.endswith(".zip"):
        with zipfile.ZipFile(path) as archive:
            require(archive.namelist().count(MANIFEST) == 1,
                    f"{path.name}: missing or duplicate SDK manifest")
            require(archive.getinfo(MANIFEST).file_size < 1024 * 1024,
                    f"{path.name}: oversized SDK manifest")
            return json.loads(archive.read(MANIFEST))
    require(path.name.endswith(".tar.gz"), f"Unexpected SDK file: {path.name}")
    with tarfile.open(path, "r:gz") as archive:
        members = [member for member in archive.getmembers() if member.name == MANIFEST]
        require(len(members) == 1 and members[0].isfile(),
                f"{path.name}: missing or duplicate SDK manifest")
        require(members[0].size < 1024 * 1024, f"{path.name}: oversized SDK manifest")
        with archive.extractfile(members[0]) as stream:
            return json.load(stream)


def validate_index(index, repository, commit):
    require(index.get("schema_version") == 1, "Unsupported snapshot index schema")
    require(index.get("repository") == repository, "Snapshot repository mismatch")
    require(index.get("commit") == commit, "Snapshot commit mismatch")
    require(index.get("snapshot_tag") == f"sdk-{commit}", "Snapshot tag mismatch")
    version = index.get("version", "")
    require(re.fullmatch(r"\d+\.\d+\.\d+(?:[-+][A-Za-z0-9.-]+)?", version),
            "Invalid SDK version")
    assets = index.get("assets", [])
    require(len(assets) == len(CONFIGURATIONS), "Snapshot requires all six SDK archives")
    configurations = set()
    for asset in assets:
        configuration = (asset["platform"], asset["architecture"], asset["build_type"])
        require(configuration in CONFIGURATIONS, "Unexpected SDK configuration")
        require(configuration not in configurations, "Duplicate SDK configuration")
        configurations.add(configuration)
        platform, architecture, build_type = configuration
        extension = ".zip" if platform == "windows" else ".tar.gz"
        name = f"libholder-{version}-{platform}-{architecture}-{build_type.lower()}{extension}"
        require(asset["name"] == name, "SDK archive filename mismatch")
        require(re.fullmatch(r"[0-9a-f]{64}", asset["sha256"]), "Invalid SDK checksum")
        require(isinstance(asset["size"], int) and asset["size"] > 0, "Invalid SDK size")
        require(asset["url"] ==
                f"https://github.com/{repository}/releases/download/sdk-{commit}/{name}",
                "SDK download URL mismatch")


def build_index(root, repository, commit, core_run_id=None, sdk_run_id=None):
    require(re.fullmatch(r"[0-9a-f]{40}", commit), "Expected a full core commit SHA")
    require(re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository),
            "Invalid GitHub repository")
    files = sorted(path for path in root.rglob("*") if path.is_file())
    require(len(files) == len(CONFIGURATIONS), "Snapshot requires all six SDK archives")
    versions = set()
    assets = []
    for path in files:
        manifest = archive_manifest(path)
        require(manifest["commit"] == commit, f"{path.name}: SDK commit mismatch")
        versions.add(manifest["version"])
        assets.append({
            "name": path.name,
            "platform": manifest["platform"],
            "architecture": manifest["architecture"],
            "build_type": manifest["build_type"],
            "size": path.stat().st_size,
            "sha256": sha256(path),
            "url": f"https://github.com/{repository}/releases/download/sdk-{commit}/{path.name}",
        })
    require(len(versions) == 1, "SDK archives have different versions")
    index = {
        "schema_version": 1,
        "repository": repository,
        "commit": commit,
        "version": versions.pop(),
        "snapshot_tag": f"sdk-{commit}",
        "core_run_id": core_run_id,
        "sdk_run_id": sdk_run_id,
        "assets": assets,
    }
    validate_index(index, repository, commit)
    return index, files


class GitHub:
    def __init__(self, repository):
        self.repository = repository

    def api(self, endpoint, method="GET", data=None, missing_ok=False):
        command = ["gh", "api", f"repos/{self.repository}/{endpoint}", "--method", method]
        if data is not None:
            command += ["--input", "-"]
        result = subprocess.run(command, input=json.dumps(data) if data is not None else None,
                                text=True, capture_output=True)
        if result.returncode:
            if missing_ok and "(HTTP 404)" in result.stderr:
                return None
            raise RuntimeError(result.stderr.strip())
        return json.loads(result.stdout) if result.stdout.strip() else None

    def tag(self, name):
        ref = self.api(f"git/ref/tags/{name}", missing_ok=True)
        if ref is None:
            return None
        require(ref["object"]["type"] == "commit", f"{name} must be a lightweight commit tag")
        return ref["object"]["sha"]

    def create_tag(self, name, commit):
        self.api("git/refs", "POST", {"ref": f"refs/tags/{name}", "sha": commit})

    def update_pointer(self, commit):
        self.api("git/refs/tags/latest-green", "PATCH", {"sha": commit, "force": True})

    def release(self, tag):
        return self.api(f"releases/tags/{tag}", missing_ok=True)

    def create_draft(self, index):
        commit = index["commit"]
        return self.api("releases", "POST", {
            "tag_name": index["snapshot_tag"], "target_commitish": commit,
            "name": f"Development SDK {commit[:12]}", "draft": True,
            "prerelease": True, "make_latest": "false",
            "body": (f"Validated SDKs for core commit `{commit}`.\n\n"
                     f"Core CI: https://github.com/{self.repository}/actions/runs/{index['core_run_id']}\n"
                     f"SDK CI: https://github.com/{self.repository}/actions/runs/{index['sdk_run_id']}\n\n"
                     "Use sdk-index.json for archive checksums and configuration selection. "
                     "Published snapshot assets are never replaced by this workflow."),
        })

    def delete_draft_asset(self, asset_id):
        self.api(f"releases/assets/{asset_id}", "DELETE")

    def upload(self, tag, files):
        subprocess.run(["gh", "release", "upload", tag, *map(str, files),
                        "--repo", self.repository], check=True)

    def download(self, tag, name, output):
        subprocess.run(["gh", "release", "download", tag, "--pattern", name,
                        "--output", str(output), "--repo", self.repository], check=True)

    def publish_release(self, release_id):
        self.api(f"releases/{release_id}", "PATCH",
                 {"draft": False, "prerelease": True, "make_latest": "false"})


def require_green_core(github, index):
    require(isinstance(index["core_run_id"], int) and index["core_run_id"] > 0,
            "Publication requires a successful main-branch core CI run")
    require(isinstance(index["sdk_run_id"], int) and index["sdk_run_id"] > 0,
            "Publication requires an SDK workflow run ID")
    run = github.api(f"actions/runs/{index['core_run_id']}")
    require(run["status"] == "completed" and run["conclusion"] == "success"
            and run["event"] == "push" and run["head_branch"] == "main"
            and run["head_sha"] == index["commit"]
            and run["path"] == ".github/workflows/core.yml"
            and run["head_repository"]["full_name"] == index["repository"],
            "Core CI did not pass for this exact main-branch commit")
    comparison = github.api(f"compare/{index['commit']}...main")
    require(comparison["status"] in ("ahead", "identical"),
            "Candidate commit is no longer on main")


def verify_release(github, release, expected, draft=False):
    require(release["draft"] == draft and release["prerelease"],
            "Unexpected SDK snapshot release state")
    tag = expected["snapshot_tag"]
    require(release["tag_name"] == tag, "Release tag mismatch")
    require(github.tag(tag) == expected["commit"], "Snapshot tag commit mismatch")
    remote_assets = {asset["name"]: asset for asset in release["assets"]}
    require(len(remote_assets) == len(release["assets"]), "Duplicate release asset name")
    require("sdk-index.json" in remote_assets, "Snapshot is missing sdk-index.json")
    require(remote_assets["sdk-index.json"]["size"] < 1024 * 1024,
            "Oversized snapshot index")
    with tempfile.TemporaryDirectory(prefix="sdk-publication-") as temporary:
        root = Path(temporary)
        index_path = root / "sdk-index.json"
        github.download(tag, "sdk-index.json", index_path)
        index = json.loads(index_path.read_text())
        validate_index(index, expected["repository"], expected["commit"])
        require(index["version"] == expected["version"], "Published SDK version mismatch")
        if draft:
            require(index == expected, "Uploaded snapshot index mismatch")
        require(set(remote_assets) == {"sdk-index.json", *(a["name"] for a in index["assets"])},
                "Incomplete or unexpected snapshot release assets")
        for asset in index["assets"]:
            remote = remote_assets[asset["name"]]
            require(remote["size"] == asset["size"], "Uploaded SDK size mismatch")
            digest = remote.get("digest")
            if digest is None:
                downloaded = root / asset["name"]
                github.download(tag, asset["name"], downloaded)
                digest = f"sha256:{sha256(downloaded)}"
            require(digest == f"sha256:{asset['sha256']}", "Uploaded SDK checksum mismatch")
    return index


def publish_snapshot(github, index, files, index_path):
    require_green_core(github, index)
    tag = index["snapshot_tag"]
    existing_tag = github.tag(tag)
    require(existing_tag in (None, index["commit"]), "Snapshot tag already points elsewhere")
    if existing_tag is None:
        github.create_tag(tag, index["commit"])
    release = github.release(tag)
    if release is not None and not release["draft"]:
        # A rerun must reuse the original published snapshot, even when a fresh
        # build of the same commit has different archive timestamps or tools.
        verify_release(github, release, index)
    else:
        if release is None:
            release = github.create_draft(index)
        # An interrupted upload can be retried. Only unpublished draft assets
        # can be replaced; latest-green still points at the previous snapshot.
        for asset in release["assets"]:
            github.delete_draft_asset(asset["id"])
        github.upload(tag, [*files, index_path])
        verify_release(github, github.release(tag), index, draft=True)
        github.publish_release(release["id"])
        require(not github.release(tag)["draft"], "SDK snapshot is still a draft")

    # Publication jobs are serialized by the workflow. Read the pointer only
    # after the snapshot is complete, and compare graph ancestry, not run times.
    current = github.tag("latest-green")
    if current is None:
        github.create_tag("latest-green", index["commit"])
        return "promoted"
    comparison = github.api(f"compare/{current}...{index['commit']}")["status"]
    if comparison == "behind":
        return "kept newer latest-green"
    if comparison == "identical":
        return "already latest-green"
    require(comparison == "ahead", "Candidate and latest-green have diverged")
    github.update_pointer(index["commit"])
    return "promoted"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--index", type=Path, required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--core-run-id", type=int)
    parser.add_argument("--sdk-run-id", type=int)
    parser.add_argument("--validate-only", action="store_true")
    args = parser.parse_args()
    index, files = build_index(args.assets, args.repository, args.commit,
                               args.core_run_id, args.sdk_run_id)
    args.index.parent.mkdir(parents=True, exist_ok=True)
    args.index.write_text(json.dumps(index, indent=2) + "\n")
    if args.validate_only:
        print(f"Validated {len(files)} SDK archives for {args.commit}")
    else:
        result = publish_snapshot(GitHub(args.repository), index, files, args.index)
        print(f"{index['snapshot_tag']}: {result}")


if __name__ == "__main__":
    main()
