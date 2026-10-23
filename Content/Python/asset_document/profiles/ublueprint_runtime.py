# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Registered UBlueprint Profile (profile.ublueprint.v2): read, create and update exact Blueprints.

Members go through ``blueprint.member.*`` primitives (provider "blueprint") and graphs
through the graph executor (provider "graph").  Writes are covered by Core's package
snapshot (existing target) or the factory rollback (created target); preflight
therefore returns ``SNAPSHOT_COVERED`` instead of per-step contributions.
See docs/architecture/asset-document-ublueprint-matrix.md for the support matrix.
"""
from __future__ import annotations

import re

from asset_document.canonical.document_codec import DocumentCodec
from asset_document.diagnostics import (
    AssetDocumentContractError, contract_error, contract_errors, primitive_error, with_diagnostics,
)
from asset_document.diff.model import Difference, DiffKind
from asset_document.dto import PrimitiveRequirement
from asset_document.graphs.executor import LINK_CHECK, GraphExecutor, graph_requirements
from asset_document.graphs.extract import extract_graphs
from asset_document.graphs.live import LiveAsset
from asset_document.graphs.reconcile import PlanOp, pending_relayout
from asset_document.graphs.schemas.k2 import K2_ADAPTER, SECTIONS, SECTION_BY_ROLE, decode_body, encode_graph
from asset_document.meta import (
    ComparisonPolicy, DefaultPolicy, DeletePolicy, DocumentAction, IdentityPolicy, OmissionPolicy, OrderPolicy,
    ProfileMeta, RegionMeta, RegionOwnership,
)
from asset_document.mutation_intent_projection import SNAPSHOT_COVERED
from asset_document.profile_runtime import RuntimeProfileRegistration, RuntimeTargetLifecycleSpec, RuntimeTargetPolicy
from asset_document.profiles import blueprint_dispatchers as dispatchers
from asset_document.profiles import blueprint_renames as renames
from asset_document.profiles import blueprint_timelines as timelines
from asset_document.profiles import ublueprint_link_check as link_check
from asset_document.profiles import ublueprint_members as members
from asset_document.profiles.ublueprint_graph_compare import PIN_ENUM_NAMES, pin_enum_names, section_mismatch_reasons
from asset_document.regions.primitive_catalog import (
    NamedOpaqueHandleArgument, PrimitiveOutcome, PrimitiveOutcomeStatus, _single_handle,
)
from asset_document.read_only_extract import admits_read_only

UBLUEPRINT_CLASS = "/Script/Engine.Blueprint"
ROUTE = "blueprint_profile"
PROFILE_ID = "profile.ublueprint.v2"
GRAPH_SECTIONS = tuple(section for section, _ in SECTIONS)
TYPE_KEY = "BlueprintType"
BODY_KEYS = (TYPE_KEY,) + members.MEMBER_KEYS + (dispatchers.KEY, timelines.KEY) + GRAPH_SECTIONS
# Body.BlueprintType -> the Content Browser factory that creates it (Blueprint Class is the
# lifecycle's default UBlueprintFactory; a const parent class makes it a const Blueprint).
TYPE_FACTORIES = {"Normal": None, "MacroLibrary": "/Script/UnrealEd.BlueprintMacroFactory",
                  "Interface": "/Script/UnrealEd.BlueprintInterfaceFactory",
                  "FunctionLibrary": "/Script/UnrealEd.BlueprintFunctionLibraryFactory"}
# blueprint.member.query.v1 blueprint_type -> Body.BlueprintType ("const" is a Normal Blueprint of a const class).
_LIVE_TYPES = {"normal": "Normal", "const": "Normal", "macro_library": "MacroLibrary", "interface": "Interface",
               "function_library": "FunctionLibrary", "level_script": "LevelScript"}
# The only parent class the Interface / Function Library factories create with (ParentClass default).
TYPE_PARENTS = {"Interface": "/Script/CoreUObject.Interface", "FunctionLibrary": "/Script/Engine.BlueprintFunctionLibrary"}
# Body regions each Blueprint type has in the editor (My Blueprint "+" buttons, Components panel,
# Class Defaults): FBlueprintEditor::NewDocument_IsVisibleForType, FBlueprintEditorUtils::DoesSupport*.
_TYPE_REGIONS = {
    "MacroLibrary": ("MacroGraphs",),
    "Interface": ("FunctionGraphs", "ClassDefaults"),
    "FunctionLibrary": ("FunctionGraphs",),
}
_TYPED_REGIONS = ("ImplementedInterfaces", "Variables", "Components", "ClassDefaults", dispatchers.KEY, timelines.KEY,
                  "UbergraphPages", "FunctionGraphs", "MacroGraphs")
CLASS_RESOLVE = "editor.class.resolve_canonical.v1"
RESOLVE = "editor.asset.resolve_existing.v1"
REFERENCE_RESOLVE = "editor.reference.resolve_asset.v1"
LIFECYCLE = ("editor.blueprint.scratch.clone.v1", "editor.blueprint.scratch.create.v1",
             "editor.blueprint.asset.create.v1", "editor.asset.rollback_created.v1")
MEMBER_PRIMITIVES = tuple(f"blueprint.member.{name}.v1" for name in (
    "query", "parent.set", "interface.add", "interface.remove", "variable.set", "variable.remove",
    "component.add", "component.attach", "component.remove", "property.set",
    *dispatchers.PRIMITIVES, *timelines.PRIMITIVES, *renames.PRIMITIVES))
REQUIREMENTS = (tuple(PrimitiveRequirement(item, 1, ROUTE) for item in (CLASS_RESOLVE, RESOLVE, *LIFECYCLE))
                + graph_requirements(ROUTE)
                + tuple(PrimitiveRequirement(item, 1, ROUTE) for item in (*MEMBER_PRIMITIVES, LINK_CHECK)))
# The parent class of a new Blueprint's Validate trial target (validate_trial_context): a need of the generic route,
# as the trial target itself (registered_runtime.TRIAL_TARGET), which declares it.
TRIAL_PARENT_RESOLVE = PrimitiveRequirement(REFERENCE_RESOLVE, 1, "route.generic.v1")
BY_ID = {item.primitive_id: item for item in REQUIREMENTS}
_TARGET = re.compile(r"/Game/[A-Za-z0-9_/-]+\Z")


def _error(path, message, code="UBlueprint.InvalidDocument"):
    return contract_error(path, code, message)


def _require_target(value: object) -> str:
    if admits_read_only(value):
        return value
    if type(value) is not str or _TARGET.fullmatch(value) is None:
        raise _error("/Target", "Target must be a canonical /Game package path such as /Game/Blueprints/BP_Door.",
                     "UBlueprint.NonCanonicalTarget")
    return value


def self_class(target: str) -> str:
    return f"{target}.{target.rsplit('/', 1)[-1]}_C"


def _snapshot(value):
    codec = DocumentCodec()
    return codec.decode(codec.encode(value))


# ---------------------------------------------------------------------------
# Primitive transport
# ---------------------------------------------------------------------------
def _invoke(context, primitive, values, handles=(), path="/Body", requirements=None):
    # requirements: the calling Profile's declared requirement table (AnimBlueprint reuses this transport).
    requirement = (BY_ID if requirements is None else requirements)[primitive]
    outcome = context.invoke(requirement=requirement, value_arguments=values, handle_arguments=handles)
    if type(outcome) is not PrimitiveOutcome:
        raise _error("/PrimitiveOutcome", "A typed primitive outcome is required.", "UBlueprint.InvalidPrimitiveResult")
    if outcome.status is not PrimitiveOutcomeStatus.SUCCEEDED:
        if outcome.diagnostics:
            source = outcome.diagnostics[0]
            # Report the Body location the AI wrote; the primitive detail goes to the diagnostic context.
            raise primitive_error(path, source, primitive)
        raise _error(path, f"{primitive} did not succeed.", "UBlueprint.PrimitiveFailed")
    return outcome


MEMBER_PAGE_ARRAYS = ("variables", "dispatchers", "timelines", "components")
MEMBER_PAGE_OBJECTS = ("class_defaults", "properties")


def query_member_pages(page) -> dict:
    """blueprint.member.query.v1 paged by "offset" (page(offset) -> one result) merged into one result.

    Every page repeats the scalar header; the array / object sections carry consecutive entries.
    """
    merged, offset = None, 0
    while offset is not None:
        value = page(offset)
        if type(value) is not dict or (value.get("next_offset") is not None and type(value["next_offset"]) is not int):
            raise _error("/PrimitiveOutcome", "Member query returned no object.", "UBlueprint.InvalidPrimitiveResult")
        if merged is None:
            merged = {key: item for key, item in value.items() if key not in ("total", "next_offset")}
        else:
            for key in MEMBER_PAGE_ARRAYS:
                merged[key] = [*merged.get(key, []), *value.get(key, [])]
            for key in MEMBER_PAGE_OBJECTS:
                merged[key] = {**merged.get(key, {}), **value.get(key, {})}
        following = value.get("next_offset")
        if following is not None and following <= offset:
            raise _error("/PrimitiveOutcome", "Member query paging did not advance.", "UBlueprint.InvalidPrimitiveResult")
        offset = following
    return merged


class _Members:
    """Member primitive calls against one Blueprint (read handle + optional mutable handle)."""

    def __init__(self, context, read, mutable=None, requirements=None):
        self.context = context
        self.requirements = requirements
        self.read = context.handoff(source_handle=read, target_provider_id="blueprint", access="read_only")
        self.write = (context.handoff(source_handle=mutable, target_provider_id="blueprint", access="mutable")
                      if mutable is not None else None)
        self.changed = []

    def query(self) -> dict:
        def page(offset):
            outcome = _invoke(self.context, "blueprint.member.query.v1", {"schema_version": 1, "offset": offset},
                              (NamedOpaqueHandleArgument("asset", self.read),), requirements=self.requirements)
            return outcome.value
        return query_member_pages(page)

    def defaults_of(self, classes) -> dict:
        """The default values of these classes' default objects (instanced values are read sparse over them)."""
        outcome = _invoke(self.context, "blueprint.member.query.v1", {"schema_version": 1, "defaults_of": sorted(classes)},
                          (NamedOpaqueHandleArgument("asset", self.read),), requirements=self.requirements)
        found = outcome.value.get("defaults_of", {}) if type(outcome.value) is dict else {}
        return dict(found) if type(found) is dict else {}

    def run(self, ops) -> None:
        for primitive, values, path in ops:
            outcome = _invoke(self.context, primitive, {"schema_version": 1, **values},
                              (NamedOpaqueHandleArgument("asset", self.write),), path, self.requirements)
            self.changed.extend(outcome.changed_set)


def _graph_executor(context, read, mutable, target):
    read_graph = context.handoff(source_handle=read, target_provider_id="graph", access="read_only")
    write_graph = (context.handoff(source_handle=mutable, target_provider_id="graph", access="mutable")
                   if mutable is not None else None)
    return GraphExecutor(context, read_handle=read_graph, write_handle=write_graph, route_id=ROUTE,
                         adapter=K2_ADAPTER, self_class=self_class(target))


# ---------------------------------------------------------------------------
# Document shape
# ---------------------------------------------------------------------------
def _validate_header(document) -> None:
    if type(document) is not dict:
        raise _error("/", "A UBlueprint document is a JSON object.")
    if type(document.get("SchemaVersion")) is not int or document["SchemaVersion"] != 1:
        raise _error("/SchemaVersion", "SchemaVersion must be 1.", "Meta.InvalidSchemaVersion")
    _require_target(document.get("Target"))
    if document.get("Class") != UBLUEPRINT_CLASS:
        raise _error("/Class", "Class must be /Script/Engine.Blueprint.", "UBlueprint.ClassMismatch")
    if document.get("Action") not in {item.value for item in DocumentAction}:
        raise _error("/Action", "Action is Create, Update or CreateOrUpdate.", "UBlueprint.InvalidAction")
    for key in ("Definitions", "Properties"):
        if type(document.get(key, {})) is not dict:
            raise _error(f"/{key}", f"{key} must be an object.")
    if document.get("Definitions"):
        raise _error("/Definitions", "UBlueprint documents do not use Definitions.", "UBlueprint.UnsupportedRegion")
    body = document.get("Body", {})
    if type(body) is not dict:
        raise _error("/Body", "Body must be an object.")
    for key in body:
        if key not in BODY_KEYS:
            raise _error(f"/Body/{key}", f"Unknown Body key '{key}'. Keys: {', '.join(BODY_KEYS)}.",
                         "UBlueprint.UnknownBodyKey")


def _parent_required(path):
    # Message kept identical to the legacy C++ profile (public surface parity).
    return _error(path, "UBlueprint creation requires Body.ParentClass", "UBlueprint.ParentClassRequired")


def _blueprint_type(body: dict) -> str | None:
    if TYPE_KEY not in body:
        return None
    value = body[TYPE_KEY]
    if value not in TYPE_FACTORIES:
        raise _error(f"/Body/{TYPE_KEY}", f"BlueprintType is one of {', '.join(TYPE_FACTORIES)} (default Normal).",
                     "UBlueprint.InvalidBlueprintType")
    return value


def _check_type_regions(kind: str, out: dict) -> None:
    """A Macro Library / Interface / Function Library has only the regions the editor offers for it."""
    allowed = _TYPE_REGIONS.get(kind)
    if allowed is None:
        return
    for key in _TYPED_REGIONS:
        if key not in allowed and out.get(key):
            raise _error(f"/Body/{key}", f"A {kind} Blueprint has no {key} (it has {', '.join(allowed)}).",
                         "UBlueprint.BlueprintTypeRegionUnsupported")


def canonicalize(document) -> dict:
    _validate_header(document)
    DocumentCodec().validate(document)
    body = document.get("Body", {})
    canonical = {key: value for key, value in document.items() if key != "Body"}
    canonical.setdefault("Definitions", {})
    canonical.setdefault("Properties", {})
    kind = _blueprint_type(body)
    out = members.canonical_members(body)
    if kind is not None:
        out[TYPE_KEY] = kind
    if document.get("Action") == DocumentAction.CREATE.value and "ParentClass" not in out:
        if kind not in TYPE_PARENTS:
            raise _parent_required("/Body/ParentClass" if "Body" in document else "/Body")
        out["ParentClass"] = TYPE_PARENTS[kind]
    for key, rows in ((dispatchers.KEY, dispatchers.canonical(body)), (timelines.KEY, timelines.canonical(body))):
        if rows is not None:
            out[key] = rows
    dispatchers.check_names(out)
    target = document["Target"]
    for section in GRAPH_SECTIONS:
        if section in body and body[section] is not None:
            # Decode validates the authored graphs; the authored spelling (aliases, ids) is kept.
            decode_body({section: body[section]}, self_class=self_class(target))
            out[section] = body[section]
    _check_type_regions(kind or "Normal", out)
    if out.get(timelines.KEY) is not None and "UbergraphPages" in out:
        graphs, _ = decode_body({"UbergraphPages": out["UbergraphPages"]}, self_class=self_class(target))
        timelines.check_nodes(out[timelines.KEY], graphs, True)
    canonical["Body"] = out
    return _snapshot(canonical)


# ---------------------------------------------------------------------------
# Apply
# ---------------------------------------------------------------------------
def apply(document, read, mutable, context) -> tuple:
    body = document["Body"]
    target = document["Target"]
    blueprint = _Members(context, read, mutable)
    first = blueprint.query()
    check_blueprint_type(body, first)
    # Each stage plans from a fresh query: a reparent changes inherited members, removals promote children.
    blueprint.run(members.plan_header(document.get("Properties", {}), body, first))
    query = blueprint.query()
    graphs, roles = decode_body(body, self_class=self_class(target))
    # Removals first (a dispatcher may become a variable and back), then members, then the
    # timelines (their nodes go into event graphs that the graph stage below then reconciles).
    blueprint.run(renames.plan_renames(body, query) + dispatchers.plan_removes(body, query) + timelines.plan_removes(body, query)
                  + members.plan_interfaces(body, query) + members.plan_variables(body, query)
                  + dispatchers.plan_sets(body, query) + members.plan_component_structure(body, query))
    query = blueprint.query()
    blueprint.run(members.plan_component_attach(body, query)
                  + timelines.plan_sets(body, query, timelines.timeline_nodes(graphs)))
    # Component values before the graphs: the compile that finalizes them validates the components
    # (a Mover component's starting mode must name one of its instanced movement modes).
    blueprint.run(members.plan_component_values(body, blueprint.query(),
                                                _instance_defaults(blueprint, body, "Components")))
    blueprint.run(members.plan_component_sockets(body, blueprint.query()))
    changed = list(blueprint.changed)
    diagnostics = []
    if roles:
        executor = _graph_executor(context, read, mutable, target)
        role_paths = {role: f"/Body/{SECTION_BY_ROLE[role]}" for role in roles}
        try:
            report = executor.run(graphs, managed_roles=roles, role_paths=role_paths)
        except AssetDocumentContractError as failure:
            raise _with_graph_findings(failure, graphs, read, context, target) from None
        changed.extend(report.changed_set)
        diagnostics.extend(report.diagnostics)
    values = _Members(context, read, mutable)
    values.run(members.plan_class_defaults(body, values.query(), _instance_defaults(values, body, "ClassDefaults")))
    changed.extend(values.changed)
    # One record per primitive and object: a large graph makes thousands of edits of a few kinds, and
    # the changed set crosses the Host transport (at most RuntimeValueMaximumNodes records).
    return with_diagnostics(tuple(dict.fromkeys(changed)), diagnostics)


def _instance_defaults(blueprint, body: dict, key: str) -> dict | None:
    """The defaults of the instanced objects' classes a Body region lists (a listed default is not an edit)."""
    classes = members.instance_classes(body.get(key))
    return blueprint.defaults_of(classes) if classes else None


def _link_check_invoke(read, context, requirements=None):
    handle = (NamedOpaqueHandleArgument("asset", context.handoff(source_handle=read, target_provider_id="graph",
                                                                 access="read_only")),)

    def invoke(primitive, values):
        try:
            return _invoke(context, primitive, {"schema_version": 1, **values}, handle, requirements=requirements).value
        except AssetDocumentContractError:
            return None  # an early check only: Apply makes each link and reports it
    return invoke


def validate_links(document, read, context, *, skip=None, requirements=None) -> None:
    """Validate's graph check of the K2 graph sections (ublueprint_link_check), read on ``read``: the existing target
    or a trial Blueprint of the same class (trial_parent_context). Shared by the Widget and Anim Blueprint Profiles
    (only their K2 sections are checked); ``skip(key)``: keys left to Apply; ``requirements``: the calling Profile's
    requirement table (it declares graph.link.check.v1)."""
    target = document["Target"]
    body = document["Body"]
    graphs, _ = decode_body({section: body[section] for section in GRAPH_SECTIONS if section in body},
                            self_class=self_class(target))
    if not graphs:
        return
    link_check.check_links(_link_check_invoke(read, context, requirements), graphs, self_class=self_class(target),
                           target=target, skip=skip)


def _pointer(key: str) -> str:
    return key.replace("~", "~0").replace("/", "~1")


def pick_check_items(document: dict) -> list:
    """The values a document writes that may hold a table row handle or an input key (blueprint.member.query.v1
    check_picks): variable defaults, ClassDefaults, component Properties and the Blueprint's own Properties. Only
    structured or text values are sent (a number or a flag holds neither)."""
    body = document.get("Body") or {}
    items = []

    def add(at, value, **address):
        if type(value) in (dict, list, str):
            items.append({"at": at, "value": value, **address})
    for index, row in enumerate(body.get("Variables") or ()):
        if type(row) is dict and type(row.get("Name")) is str and row.get("DefaultValue") is not None and "Type" in row:
            try:
                dto = members.type_dto(row["Type"], f"/Body/Variables/{index}/Type")
            except AssetDocumentContractError:
                continue  # Apply reports the type
            add(f"/Body/Variables/{index}/DefaultValue", row["DefaultValue"], name=row["Name"], type=dto)
    for key, value in (body.get("ClassDefaults") or {}).items():
        add(f"/Body/ClassDefaults/{_pointer(key)}", value, target="class_defaults", property=key)
    for index, row in enumerate(body.get("Components") or ()):
        if type(row) is not dict or type(row.get("Name")) is not str or type(row.get("Properties")) is not dict:
            continue
        address = {"target": "component", "component": {"name": row["Name"], "scope": str(row.get("Scope", "Owned")).lower()}}
        if type(row.get("Class")) is str:
            address["class"] = row["Class"]
        for key, value in row["Properties"].items():
            add(f"/Body/Components/{index}/Properties/{_pointer(key)}", value, property=key, **address)
    properties = document.get("Properties")
    for key, value in (properties.items() if type(properties) is dict else ()):
        add(f"/Properties/{_pointer(key)}", value, target="blueprint", property=key)
    return items


def check_picks(document: dict, read, context, requirements=None) -> None:
    """Validate's check of the table rows and input keys the document writes: the Details panel picks a row
    handle's RowName from its DataTable's rows and an input key from the registered keys, so a name neither lists
    (RowName "Bost" for "Boost") is refused here with the close names, as Apply refuses it (Property.RowNotFound /
    Property.KeyNotFound). Read on the existing target or the trial Blueprint (validate_trial_context)."""
    items = pick_check_items(document)
    if not items:
        return
    try:
        outcome = _invoke(context, "blueprint.member.query.v1", {"schema_version": 1, "check_picks": items},
                          (NamedOpaqueHandleArgument("asset", context.handoff(source_handle=read, target_provider_id="blueprint",
                                                                             access="read_only")),),
                          requirements=requirements)
    except AssetDocumentContractError:
        return  # an early check only: Apply writes each value and reports it
    found = outcome.value.get("check_picks") if type(outcome.value) is dict else None
    problems = [contract_error(item["at"], item["code"], item["message"]).diagnostic for item in found or ()
                if type(item) is dict and all(type(item.get(key)) is str for key in ("at", "code", "message"))]
    if problems:
        raise contract_errors(problems)


def listed_component_events(body: dict):
    """``skip`` of Validate's graph check: a bound event (K2:ComponentBoundEvent:...@<component>) of a component the
    document lists as its own (Scope Owned, the default). The same document may add that component, which the
    Blueprint the check reads (a trial Blueprint of the parent class, or the target before the apply) does not have
    yet, so the key is left to Apply, which adds the component first (a Widget Blueprint's widgets:
    widget_blueprint_runtime)."""
    owned = {row["Name"].casefold() for row in body.get("Components") or ()
             if type(row) is dict and type(row.get("Name")) is str and str(row.get("Scope", "Owned")).casefold() == "owned"}
    if not owned:
        return None
    return lambda key: key.startswith("K2:ComponentBoundEvent:") and key.rpartition("@")[2].casefold() in owned


def trial_parent_context(context, body: dict, values: bool = False) -> tuple:
    """validate_trial_context of a new Blueprint (Class, Widget or Animation Blueprint) with K2 graphs (or, when
    ``values``, values check_picks reads): its parent class as the trial target's create context
    (AssetDocumentBlueprintTrialTarget.cpp); () = no check."""
    if not values and not any(body.get(section) for section in GRAPH_SECTIONS):
        return ()
    parent = body.get("ParentClass")
    if parent is None:
        return ()
    try:
        outcome = _invoke(context, REFERENCE_RESOLVE, {"expected_class": "/Script/CoreUObject.Class", "path": parent},
                          (), "/Body/ParentClass", {REFERENCE_RESOLVE: TRIAL_PARENT_RESOLVE})
    except AssetDocumentContractError:
        return ()  # Apply reports the parent class
    return (NamedOpaqueHandleArgument("factory_context", _single_handle(outcome, "asset")),)


def _with_graph_findings(failure, graphs, read, context, target):
    """Apply's graph failure plus the other independent problems Validate's graph check finds in the same
    document (best effort: the check only reads the palette), so one pass reports them all."""
    try:
        found = link_check.check_graphs(_link_check_invoke(read, context), graphs, self_class=self_class(target),
                                        target=target)
    except Exception:  # noqa: BLE001 - the Apply failure is what matters
        return failure
    seen = {(item.path, item.code) for item in failure.diagnostics}
    extra = tuple(item for item in found if (item.path, item.code) not in seen)
    return AssetDocumentContractError(failure.diagnostic, failure.additional + extra) if extra else failure


def live_blueprint_type(query: dict) -> str:
    return _LIVE_TYPES.get(query.get("blueprint_type", "normal"), "Normal")


def check_blueprint_type(body: dict, query: dict) -> None:
    """The Blueprint type is chosen at creation (its factory); the editor cannot change it later."""
    live = live_blueprint_type(query)
    if TYPE_KEY in body and body[TYPE_KEY] != live:
        raise _error(f"/Body/{TYPE_KEY}", f"This Blueprint is a {live} Blueprint; the editor cannot change a Blueprint's "
                     f"type after creation (create a new {body[TYPE_KEY]} asset instead).", "UBlueprint.BlueprintTypeImmutable")


def repair(document, mutable, context) -> tuple:
    """Compile once if anything is left uncompiled; compiler errors fail the request (Core rolls back)."""
    blueprint = _Members(context, mutable)
    if blueprint.query().get("status") in ("up_to_date", "up_to_date_with_warnings"):
        return ()
    return compile_with_diagnostics(_graph_executor(context, mutable, mutable, document["Target"]), document)


def compile_with_diagnostics(executor, document) -> tuple:
    """Bare compile whose errors and warnings point at the document nodes (the executor binds them only if needed)."""
    graphs, roles = decode_body(document["Body"], self_class=self_class(document["Target"]))
    role_paths = {role: f"/Body/{SECTION_BY_ROLE[role]}" for role in roles if role in SECTION_BY_ROLE}
    sections = {role: section for section, role in SECTIONS}
    executor.binder = lambda bound: bound.bind_document(graphs, managed_roles=roles, role_paths=role_paths,
                                                        sections=sections)
    executor._op_graph_finalize(PlanOp("graph.finalize", "", "/Body", {}))
    return with_diagnostics(executor.changed, executor.diagnostics)


# ---------------------------------------------------------------------------
# Extract and diff
# ---------------------------------------------------------------------------
def extract(target, read, context, include_defaults: bool = False) -> dict:
    return _extract_with_query(target, read, context, include_defaults)[0]


# live_defaults key: graphs whose "Layout": "Auto" would move nodes (doc path -> the moves).
PENDING_RELAYOUT = "__pending_relayout"


def read_live_with_relayout(executor, decode_authored, *, adapter, self_class: str | None) -> tuple[LiveAsset, dict]:
    """The live graphs, and the relayout the authored graphs' ``"Layout": "Auto"`` (subgraphs included) would make:
    doc path -> moves (graphs.reconcile.pending_relayout).  ``Layout`` is a request, not state, so a Diff lays the
    live graphs out to tell whether it would move nodes.  ``decode_authored()`` -> (desired graphs, managed roles), or
    None without an authored document; a document that does not decode leaves its apply / other differences to
    report it."""
    try:
        decoded = decode_authored()
    except AssetDocumentContractError:
        decoded = None
    desired, roles = decoded if decoded is not None else ((), frozenset())
    relayout = any(graph.layout == "Auto" for doc in desired for graph in doc.walk())
    live: LiveAsset = executor.read_live(include_sizes=relayout)
    if not relayout:
        return live, {}
    return live, pending_relayout(desired, live, managed_roles=roles, adapter=adapter, self_class=self_class)


def relayout_differences(defaults: dict | None) -> list:
    """The Diff operation's differences of PENDING_RELAYOUT: a graph's "Layout": "Auto" that would move nodes is a
    change to apply (``/Body/<section>/<n>/Layout``, a subgraph's own path)."""
    return [Difference(f"{path}/Layout", DiffKind.REPLACE, True, "Auto", True, {"Moves": moved})
            for path, moved in sorted(((defaults or {}).get(PENDING_RELAYOUT) or {}).items())]


def authored_k2_graphs(body: dict | None, target: str, sections=GRAPH_SECTIONS):
    """decode_authored for read_live_with_relayout: the K2 graph sections of an authored Body."""
    if type(body) is not dict:
        return lambda: None
    return lambda: decode_body({section: body[section] for section in sections if section in body},
                               self_class=self_class(target))


def _extract_with_query(target, read, context, include_defaults: bool = False,
                        authored_body: dict | None = None) -> tuple[dict, dict]:
    blueprint = _Members(context, read)
    query = blueprint.query()
    body = members.members_from_query(query)
    kind = live_blueprint_type(query)
    if kind != "Normal":
        body = {TYPE_KEY: kind, **body}
    body[dispatchers.KEY] = dispatchers.from_query(query)
    body[timelines.KEY] = timelines.from_query(query)
    executor = _graph_executor(context, read, None, target)
    live, pending = read_live_with_relayout(executor, authored_k2_graphs(authored_body, target), adapter=K2_ADAPTER,
                                            self_class=self_class(target))
    if pending:
        query = {**query, PENDING_RELAYOUT: pending}
    graphs = extract_graphs(live, sections={role: section for section, role in SECTIONS}, adapter=K2_ADAPTER,
                            self_class=self_class(target), include_defaults=include_defaults)
    for section in GRAPH_SECTIONS:
        body[section] = [encode_graph(graph) for graph in graphs if graph.id.startswith(section + "/")]
    if include_defaults:
        query = {**query, PIN_ENUM_NAMES: pin_enum_names(live)}
    return {"SchemaVersion": 1, "Target": target, "Class": UBLUEPRINT_CLASS,
            "Action": DocumentAction.CREATE_OR_UPDATE.value, "Definitions": {},
            "Properties": dict(query.get("properties", {})), "Body": body}, query


def extract_with_defaults(target, read, context, authored=None):
    """The extraction and the default values of the classes of the instanced objects the authored document lists
    (ClassDefaults and component Properties): an instanced object's Properties are read sparse over its class's
    defaults, so a listed default is absent from the read. Graph pins report values equal to their defaults
    too, so a PinOverride that restates a pin default compares as present."""
    body = authored.get("Body", {}) if type(authored) is dict else {}
    document, query = _extract_with_query(target, read, context, include_defaults=True,
                                          authored_body=body if type(body) is dict else None)
    classes = members.instance_classes([body.get("ClassDefaults", {}), body.get("Components", [])])
    defaults = _Members(context, read).defaults_of(classes) if classes else {}
    # AttachTo may name a native component by its subobject name (CollisionCylinder for CapsuleComponent).
    names = members.native_component_names(query)
    extra = {members.NATIVE_NAMES: names} if names else {}
    enum_names = members.variable_enum_names(query)
    if enum_names:
        extra[members.ENUM_NAMES] = enum_names
    if query.get(PIN_ENUM_NAMES):
        extra[PIN_ENUM_NAMES] = query[PIN_ENUM_NAMES]
    if query.get(PENDING_RELAYOUT):
        extra[PENDING_RELAYOUT] = query[PENDING_RELAYOUT]
    return document, ({**defaults, **extra} if extra else defaults)


def diff(authored: dict, extracted: dict, strict: bool = False, defaults: dict | None = None) -> tuple:
    body, live = authored["Body"], extracted["Body"]
    out = []
    if TYPE_KEY in body and body[TYPE_KEY] != live.get(TYPE_KEY, "Normal"):
        out.append(Difference(f"/Body/{TYPE_KEY}", DiffKind.REPLACE, True, body[TYPE_KEY], True, live.get(TYPE_KEY, "Normal")))
    out += members.diff_members(body, live, authored.get("Properties", {}), extracted.get("Properties", {}), strict,
                                defaults)
    out += dispatchers.diff(body, live) + timelines.diff(body, live)
    target = authored["Target"]
    for section in GRAPH_SECTIONS:
        if section not in body:
            continue
        reasons = section_mismatch_reasons(section, body[section], live.get(section, []),
                                           self_class=self_class(target), strict=strict,
                                           enum_names=(defaults or {}).get(PIN_ENUM_NAMES))
        if reasons:
            out.append(Difference(f"/Body/{section}", DiffKind.REPLACE, True,
                                  {"Differs": reasons}, True,
                                  {"Graphs": [graph["Name"] for graph in live.get(section, [])]}))
    if strict:
        out += relayout_differences(defaults)
    return tuple(out)


# ---------------------------------------------------------------------------
# Handler
# ---------------------------------------------------------------------------
class UBlueprintRuntimeProfile:
    profile_id = PROFILE_ID
    exact_asset_class_path = UBLUEPRINT_CLASS
    runtime_entry_module = "asset_document/bootstrap.py"

    def required_primitives(self) -> tuple[PrimitiveRequirement, ...]:
        return REQUIREMENTS

    def inspect_profile(self) -> dict:
        return {"Class": UBLUEPRINT_CLASS, "BodySections": list(BODY_KEYS)}

    def create_template(self, target: str) -> dict:
        return {"SchemaVersion": 1, "Target": _require_target(target), "Class": UBLUEPRINT_CLASS,
                "Action": DocumentAction.CREATE_OR_UPDATE.value, "Definitions": {}, "Properties": {},
                "Body": {"ParentClass": "/Script/Engine.Actor", "ImplementedInterfaces": [], "Variables": [],
                         "Components": [], "ClassDefaults": {}, "EventDispatchers": [], "Timelines": [],
                         "UbergraphPages": [], "FunctionGraphs": [],
                         "MacroGraphs": []}}

    def validate(self, document) -> None:
        canonicalize(document)

    def canonicalize(self, document) -> dict:
        return canonicalize(document)

    def create_target_context(self, document, context):
        document = canonicalize(document)
        parent = document["Body"].get("ParentClass")
        if parent is None:
            parent = TYPE_PARENTS.get(document["Body"].get(TYPE_KEY))
        if parent is None:
            raise _parent_required("/Body/ParentClass")
        outcome = _invoke(context, CLASS_RESOLVE, {"class": parent, "expected_base": None}, (), "/Body/ParentClass")
        handles = (NamedOpaqueHandleArgument("parent_class", _single_handle(outcome, "class")),)
        factory = TYPE_FACTORIES[document["Body"].get(TYPE_KEY, "Normal")]
        if factory is not None:
            outcome = _invoke(context, CLASS_RESOLVE, {"class": factory, "expected_base": None}, (), f"/Body/{TYPE_KEY}")
            handles += (NamedOpaqueHandleArgument("factory_class", _single_handle(outcome, "class")),)
        return handles

    def preflight(self, document, scratch_handle, context):
        document = canonicalize(document)
        decode_body(document["Body"], self_class=self_class(document["Target"]))
        return SNAPSHOT_COVERED

    def apply(self, document, read_asset_handle, mutable_asset_handle, context):
        return apply(canonicalize(document), read_asset_handle, mutable_asset_handle, context)

    def validate_checks(self, document, read_asset_handle, context):
        """Validate's graph check (ublueprint_link_check.check_graphs): node keys, pin names and links, read on the
        existing target, or on a transient Blueprint of the parent class for a new one (validate_trial_context)."""
        document = canonicalize(document)
        found = []
        skip = listed_component_events(document["Body"])
        for check in (lambda: validate_links(document, read_asset_handle, context, skip=skip),
                      lambda: check_picks(document, read_asset_handle, context)):
            try:
                check()
            except AssetDocumentContractError as failure:
                found.extend((failure.diagnostic,) + failure.additional)
        if found:
            raise contract_errors(found)

    def validate_trial_context(self, document, context):
        """The create context of a new Blueprint's trial target (AssetDocumentBlueprintTrialTarget.cpp): its parent
        class. Only a Blueprint Class (BlueprintType Normal) with graphs or written values is checked this way."""
        document = canonicalize(document)
        body = document["Body"]
        if body.get(TYPE_KEY, "Normal") != "Normal":
            return ()
        return trial_parent_context(context, body, bool(pick_check_items(document)))

    def repair(self, document, asset_handle, context):
        return repair(canonicalize(document), asset_handle, context)

    def extract(self, *, target, asset_handle, context, include_defaults=False):
        return extract(_require_target(target), asset_handle, context, include_defaults)

    def inspect(self, *, mode, exact_class_path, exact_asset_object_path, editor_asset_handle, context):
        if mode != "live_asset" or exact_class_path != UBLUEPRINT_CLASS or editor_asset_handle is None:
            raise _error("/Inspect", "UBlueprint inspection reads a live Blueprint asset.", "UBlueprint.InspectNotAdmitted")
        return self.canonicalize(self.extract(target=exact_asset_object_path.rsplit(".", 1)[0],
                                              asset_handle=editor_asset_handle, context=context))

    def diff(self, authored, extracted):
        """Post-apply verification: lenient about values the editor normalises or omits."""
        return diff(canonicalize(authored), canonicalize(extracted))

    def diff_exact(self, authored, extracted):
        """The Diff operation: every value the document states must hold on the live asset."""
        return diff(canonicalize(authored), canonicalize(extracted), strict=True)

    def extract_with_defaults(self, *, target, asset_handle, context, authored=None):
        return extract_with_defaults(_require_target(target), asset_handle, context, authored)

    def diff_with_defaults(self, authored, extracted, live_defaults, exact):
        """Verify / Diff with the defaults of the instanced objects' classes (``extract_with_defaults``)."""
        return diff(canonicalize(authored), canonicalize(extracted), strict=exact, defaults=live_defaults)


def create_ublueprint_runtime_handler():
    return UBlueprintRuntimeProfile()


_REGIONS = tuple(
    RegionMeta(f"ublueprint.{name}.v2", f"/Body/{key}", RegionOwnership.MANAGED, identity, OrderPolicy.CANONICAL,
               DefaultPolicy.EXPLICIT, OmissionPolicy.PRESERVE, DeletePolicy.EXPLICIT_ONLY, ComparisonPolicy.CANONICAL)
    for name, key, identity in (
        ("blueprint_type", TYPE_KEY, IdentityPolicy.POSITIONAL),
        ("parent_class", "ParentClass", IdentityPolicy.POSITIONAL),
        ("implemented_interfaces", "ImplementedInterfaces", IdentityPolicy.STABLE_KEY),
        ("variables", "Variables", IdentityPolicy.STABLE_KEY),
        ("components", "Components", IdentityPolicy.STABLE_KEY),
        ("class_defaults", "ClassDefaults", IdentityPolicy.STABLE_KEY),
        ("event_dispatchers", "EventDispatchers", IdentityPolicy.STABLE_KEY),
        ("timelines", "Timelines", IdentityPolicy.STABLE_KEY),
        ("ubergraph_pages", "UbergraphPages", IdentityPolicy.STABLE_KEY),
        ("function_graphs", "FunctionGraphs", IdentityPolicy.STABLE_KEY),
        ("macro_graphs", "MacroGraphs", IdentityPolicy.STABLE_KEY),
    ))
UBLUEPRINT_PROFILE_META = ProfileMeta(PROFILE_ID, ROUTE, UBLUEPRINT_CLASS, _REGIONS, REQUIREMENTS)
UBLUEPRINT_RUNTIME_REGISTRATION = RuntimeProfileRegistration(
    UBLUEPRINT_PROFILE_META, create_ublueprint_runtime_handler, RuntimeTargetPolicy.CREATE_OR_UPDATE,
    RuntimeTargetLifecycleSpec("blueprint_exact.v1", "blueprint_exact.v1", "blueprint_factory.v1",
                               *(BY_ID[item] for item in LIFECYCLE), ("parent_class", "factory_class"),
                               ("factory_class",)))
