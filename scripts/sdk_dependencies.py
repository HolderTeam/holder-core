"""Make bundled Windows package configs independent of a consumer's vcpkg root."""

from pathlib import Path
import os


def relocate_windows_packages(triplet: Path) -> None:
    # Some vcpkg ports (notably sodium) find their files using the consumer's
    # active vcpkg installation instead of their own config's location. A
    # consumer may need a separate installation for additional dependencies.
    for config in (triplet / "share").rglob("*.cmake"):
        text = config.read_text()
        relative = os.path.relpath(triplet, config.parent).replace(os.sep, "/")
        local = "${CMAKE_CURRENT_LIST_DIR}/" + relative
        updated = text
        for root in ("_VCPKG_INSTALLED_DIR", "VCPKG_INSTALLED_DIR"):
            updated = updated.replace("${" + root + "}/${VCPKG_TARGET_TRIPLET}", local)
        if updated != text:
            config.write_text(updated)
