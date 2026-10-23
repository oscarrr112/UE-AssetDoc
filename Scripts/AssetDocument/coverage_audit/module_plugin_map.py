#!/usr/bin/env python3
# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Map UE module names to the owning plugin by scanning .uplugin files (Engine plugins + this repo).
Modules not owned by any plugin belong to the Engine itself. Prints JSON {module: plugin}.
ENGINE_DIR must name the engine's Engine folder."""
import glob, json, os, sys

ENGINE = os.environ.get("ENGINE_DIR")
if not ENGINE or not os.path.isdir(ENGINE):
    sys.exit("ENGINE_DIR must name the Engine folder of an Unreal Engine install "
             "(<UE_ROOT>/Engine, the folder that holds Source and Plugins)")
REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))


def build():
    mapping = {}
    for pattern in (ENGINE + "/Plugins/**/*.uplugin", REPO + "/*.uplugin"):
        for path in glob.glob(pattern, recursive=True):
            try:
                data = json.loads(open(path, encoding="utf-8-sig").read())
            except Exception:
                continue
            name = os.path.splitext(os.path.basename(path))[0]
            for module in data.get("Modules", []):
                mapping.setdefault(module["Name"], name)
    return mapping


if __name__ == "__main__":
    json.dump(build(), sys.stdout, indent=1)
