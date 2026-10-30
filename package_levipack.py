#!/usr/bin/env python3
"""Packs libLeviShaders.so + manifest.json into a .levipack (a zip), same layout BedrockTools uses."""
import argparse, json, zipfile
from pathlib import Path

MANIFEST = {
    "type": "preload-native",
    "name": "LeviShaders",
    "author": "you",
    "description": "Java shader pack (composite/final) post-processing for Minecraft Bedrock.",
    "version": "0.1.0",
    "entry": "libLeviShaders.so",
    "minecraft_versions": [],
}

ap = argparse.ArgumentParser()
ap.add_argument("--library", required=True, type=Path)
ap.add_argument("--output", required=True, type=Path)
a = ap.parse_args()
assert a.library.is_file(), f"missing {a.library}"
a.output.parent.mkdir(parents=True, exist_ok=True)
with zipfile.ZipFile(a.output, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    z.writestr("manifest.json", json.dumps(MANIFEST, indent=2) + "\n")
    z.write(a.library, "libLeviShaders.so")
print(a.output)
