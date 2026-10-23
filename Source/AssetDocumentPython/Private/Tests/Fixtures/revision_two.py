# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

# canonical_bytes below is a fixture-only transport marker. It is not
# production canonical/semantic evidence and has no asserted DocumentCodec
# relationship to semantic_hash.
REPOSITORY_DEPENDENCY_MARKER = "repository"

def dispatch(request, context):
    import json
    outcome = context.invoke("editor.host_test.echo", 1, request["canonical_document"], ())
    return {"abi_version": 1, "request_id": request["request_id"], "terminal_status": 0,
            "final_phase": 6, "runtime_revision": request["expected_runtime_revision"],
            "diagnostics": (), "canonical_value": outcome["value"],
            "canonical_bytes": b"fixture-only-transport-marker:revision-two",
            "semantic_hash": json.loads('"revision-two"'), "codec_version": 1,
            "changed_set": ()}
