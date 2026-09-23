#!/usr/bin/env python3
"""Record the source and binaries that the isolated rehearsal will open."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

client, patch = (Path(argument).resolve() for argument in sys.argv[1:])


def entry(name):
    with (client / name).open("rb") as handle:
        value = hashlib.file_digest(handle, "sha256").hexdigest()
    return {"path": name, "sha256": value}


manifest = {
    "base_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=client, text=True).strip(),
    "patch_sha256": hashlib.sha256(patch.read_bytes()).hexdigest(),
    "changed_files": [entry(name) for name in (
        "src/chainparams.cpp", "src/init.cpp", "src/kernel/chainparams.cpp",
        "src/oracle/exchange.cpp", "src/util/thawday_lab.h")],
    "binaries": [entry(name) for name in ("src/digibyted", "src/digibyte-cli", "src/qt/digibyte-qt")],
    "build_command": "make -j8 -C src digibyted digibyte-cli qt/digibyte-qt",
}
(client / "THAWDAY_BUILD.json").write_text(json.dumps(manifest, indent=2) + "\n")
