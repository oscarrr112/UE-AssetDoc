# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

# canonical_bytes below is a fixture-only transport marker. It is not
# production canonical/semantic evidence and has no asserted DocumentCodec
# relationship to semantic_hash.
def dispatch(request, context):
    import json
    import revision_two as repository_dependency
    if repository_dependency.REPOSITORY_DEPENDENCY_MARKER != "repository":
        raise RuntimeError("repository dependency root mismatch")
    outcome = context.invoke("editor.host_test.echo", 1, request["canonical_document"], ())
    return {"abi_version": 1, "request_id": request["request_id"], "terminal_status": 0,
            "final_phase": 6, "runtime_revision": request["expected_runtime_revision"],
            "diagnostics": (), "canonical_value": outcome["value"],
            "canonical_bytes": b"fixture-only-transport-marker:revision-one",
            "semantic_hash": json.loads('"revision-one"'), "codec_version": 1,
            "changed_set": ()}
