"""Registered Environment Query Profile (profile.env_query.v1) on the ``env_query_profile`` route.

The query's content is its editor graph: options (a generator each, wired to Root, run left to right) and their tests
(``UEnvQuery::Options`` is rebuilt from the graph by the editor, so it is never authored directly).  The graph is read
and edited through ``ai.env_query.*`` (AssetDocumentAI), which drive the Environment Query editor's own actions.
Document shape, Extract and Diff: ``env_query_model``; Apply: ``env_query_apply``.  Create goes through the editor's
Environment Query factory (an empty query; its graph is made on the first edit, as on the editor's first open).
"""
from __future__ import annotations

from asset_document.meta import (
    ComparisonPolicy, DefaultPolicy, DeletePolicy, IdentityPolicy, OmissionPolicy, OrderPolicy, ProfileMeta,
    RegionMeta, RegionOwnership,
)
from asset_document.mutation_intent_projection import SNAPSHOT_COVERED
from asset_document.profile_runtime import RuntimeProfileRegistration, RuntimeTargetPolicy
from asset_document.profiles import data_table_common as common
from asset_document.profiles import env_query_model as model
from asset_document.profiles.env_query_model import CLASS, PREFIX, error

# UEnvQuery's options are the Body (rebuilt from the editor graph); nothing else of the query is in its Details panel.
BODY_OWNED = {"Options": "Body.Options"}


def properties():
    return common.PropertiesRegion(CLASS, BODY_OWNED, PREFIX)


def canonicalize(document):
    body = common.validate_header(document, (CLASS,), PREFIX, model.BODY_KEYS)
    canonical = common.snapshot(document)
    canonical["Body"] = model.check_body(body)
    properties().check_owned(canonical.get("Properties", {}))
    return canonical


def extract_with_defaults(target, read, context, authored=None):
    """(document, defaults): the extract and the new-node Details its sparse Properties (and ``authored``'s) are
    relative to."""
    calls = model.Calls(context, read)
    live, body = model.read_live(calls, (authored or {}).get("Body"))
    document = common.document(target, CLASS, properties().extract(read, context), body)
    return document, {"properties": live.defaults, "labels": live.labels}


def extract(target, read, context, include_defaults=False):
    """The query's document; ``include_defaults`` (extract with diff_only false) lists every Details value of the
    generators and tests, not only those that differ from a new node's."""
    calls = model.Calls(context, read)
    if include_defaults:
        live, body = model.read_live(calls)
        body = model.with_defaults(body, live.defaults)
    else:
        body = model.extract_body(calls.live())
    return common.document(target, CLASS, properties().extract(read, context, include_defaults), body)


def diff(authored, extracted, exact, defaults=None):
    changes = list(common.header_diff(authored, extracted))
    changes += common.properties_diff(authored.get("Properties", {}), extracted.get("Properties", {}), exact)
    body = authored.get("Body", {})
    if defaults is not None:
        body = model.relabel(body, defaults.get("labels", {}))
    changes += model.body_diff(body, extracted.get("Body", {}), exact,
                               None if defaults is None else defaults.get("properties", {}))
    return tuple(changes)


def inspect_payload(document, live):
    """The extract plus what the Environment Query editor shows around the graph: validation (the node error badges),
    the query parameters (named params a Run EQS Query binds), per-node facts and the assets that use the query."""
    severity = {"error": "Error", "warning": "Warning"}
    messages = [{"Severity": severity.get(item.get("severity", ""), "Info"), "Message": item.get("message", ""),
                 **({"Option": item["option"]} if item.get("option") else {}),
                 **({"Test": item["test"]} if item.get("test") else {})} for item in live.messages]
    status = ("Error" if any(item["Severity"] == "Error" for item in messages)
              else "UpToDateWithWarnings" if messages else "UpToDate")
    facts = {}
    for option in live.options:
        facts[option.name] = {"Kind": "Option", "Class": option.label, "ItemType": option.item_type,
                              "Connected": option.connected, "RuntimeIndex": option.runtime_index}
        if option.error:
            facts[option.name]["Error"] = option.error
        for name in option.tests:
            test = live.tests.get(name)
            if test is not None:
                facts[name] = {"Kind": "Test", "Class": test.label, "Option": option.name, "Enabled": test.enabled,
                               "SupportsItemType": test.supported,
                               **({"WeightPercent": test.weight} if type(test.weight) in (int, float) and test.weight >= 0
                                  else {})}
    return {**document, "Status": {"Status": status, "Messages": messages},
            "Params": [{"Name": item.get("name", ""), "Type": item.get("type", "")} for item in live.params],
            "NodeFacts": facts,
            "Dependents": [{"Asset": item.get("asset") or item.get("package", ""), "Class": item.get("class", "")}
                           for item in live.dependents],
            "OpenInEditor": live.open_in_editor}


class EnvQueryRuntimeProfile:
    runtime_entry_module = "asset_document/bootstrap.py"
    preflight_scratch = False
    profile_id = model.PROFILE_ID
    exact_asset_class_path = CLASS

    def required_primitives(self):
        return model.REQUIREMENTS

    def inspect_profile(self):
        return {"Class": CLASS, "BodySections": list(model.BODY_KEYS), "ReadOnlySections": [], "ApplyAvailable": True}

    def create_template(self, target):
        # The editor's factory makes an empty query; its first open shows Root alone at (0, 0).
        return common.document(common.require_target(target, PREFIX), CLASS, {},
                               {"Root": {"Position": [0, 0]}, "Options": [], "Comments": {}})

    def validate(self, document):
        canonicalize(document)

    def canonicalize(self, document):
        return canonicalize(document)

    def create_target_context(self, document, context):
        canonicalize(document)
        return ()

    def preflight(self, document, scratch_handle, context):
        canonical = canonicalize(document)
        properties().validate(canonical.get("Properties", {}), context)
        return SNAPSHOT_COVERED

    def apply(self, document, read_asset_handle, mutable_asset_handle, context):
        return _apply.apply(canonicalize(document), read_asset_handle, mutable_asset_handle, context, properties())

    def repair(self, document, asset_handle, context):
        canonicalize(document)
        return ()

    def extract(self, *, target, asset_handle, context, include_defaults=False):
        return extract(common.require_target(target, PREFIX), asset_handle, context, include_defaults)

    def extract_with_defaults(self, *, target, asset_handle, context, authored=None):
        return extract_with_defaults(common.require_target(target, PREFIX), asset_handle, context,
                                     canonicalize(authored) if authored is not None else None)

    def inspect(self, *, mode, exact_class_path, exact_asset_object_path, editor_asset_handle, context):
        if mode != "live_asset" or exact_class_path != CLASS or editor_asset_handle is None:
            raise error("/Inspect", "EnvQuery inspection reads a live Environment Query.", f"{PREFIX}.InspectNotAdmitted")
        calls = model.Calls(context, editor_asset_handle)
        live = calls.live(dependents=True)
        document = common.document(exact_asset_object_path.rsplit(".", 1)[0], CLASS,
                                   properties().extract(editor_asset_handle, context), model.extract_body(live))
        return inspect_payload(document, live)

    def diff(self, authored, extracted):
        return diff(canonicalize(authored), canonicalize(extracted), exact=False)

    def diff_exact(self, authored, extracted):
        return diff(canonicalize(authored), canonicalize(extracted), exact=True)

    def diff_with_defaults(self, authored, extracted, live_defaults, exact):
        return diff(canonicalize(authored), canonicalize(extracted), exact, live_defaults)


def _meta():
    rows = (("root", "/Body/Root", IdentityPolicy.STABLE_KEY), ("options", "/Body/Options", IdentityPolicy.STABLE_KEY),
            ("comments", "/Body/Comments", IdentityPolicy.STABLE_KEY))
    regions = tuple(RegionMeta(f"env_query.{name}.v1", path, RegionOwnership.MANAGED, identity, OrderPolicy.PRESERVE,
                               DefaultPolicy.EXPLICIT, OmissionPolicy.PRESERVE, DeletePolicy.EXPLICIT_ONLY,
                               ComparisonPolicy.CANONICAL)
                    for name, path, identity in rows)
    return ProfileMeta(model.PROFILE_ID, model.ROUTE, CLASS, regions, model.REQUIREMENTS)


# Module-level on purpose (see metasound.py): the native runtime restores sys.modules between phase calls.
from asset_document.profiles import env_query_apply as _apply  # noqa: E402

ENV_QUERY_RUNTIME_REGISTRATION = RuntimeProfileRegistration(_meta(), EnvQueryRuntimeProfile,
                                                            RuntimeTargetPolicy.CREATE_OR_UPDATE, model.LIFECYCLE)

__all__ = ("ENV_QUERY_RUNTIME_REGISTRATION", "EnvQueryRuntimeProfile", "canonicalize", "diff")
