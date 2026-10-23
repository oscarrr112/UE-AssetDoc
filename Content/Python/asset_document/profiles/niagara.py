# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Registered Niagara Profiles: NiagaraSystem, NiagaraEmitter, NiagaraScript and NiagaraParameterCollection on
the ``fx_profile`` route.

Extract, Inspect and Diff read the assets through ``fx.niagara.read.v1`` (provider "fx",
AssetDocumentNiagara): systems and emitters through the Niagara editor's own stack view models built
headlessly, scripts and scratch pads as their graphs, parameter collections as their parameters.  The
document shapes are in ``niagara_shape``.

Systems apply (system level, ``niagara_apply``; module stacks, ``niagara_stack``): Details ``Properties``
through the generic property admission, then the Body (user and declared parameters, emitters, emitter properties,
renderers, module stacks and their inputs, event handlers, simulation stages, lightweight emitters) through
``fx.niagara.edit.v1``, then one recompile (``fx.niagara.compile.v1``): errors the system did not have before
fail the apply (``Niagara.CompileFailed``; Core rolls the package back), errors it already had are warnings.
Writes are covered by Core's package snapshot (existing target) or the factory rollback (created target), so
preflight returns ``SNAPSHOT_COVERED``.  Scratch pads (``niagara_script``) are edited before each pass's stack
edits: the Scratch Pad panel's add / rename (system pads), their graphs through the shared graph.* primitives
(``graphs.schemas.niagara``), their Details and parameters, then the pad's Apply; unlisted system pads are deleted
after the stacks stop using them.

Script assets: Usage (a new asset is made again by the factory of its usage), Versions (the version menu;
editing the exposed version of a versioned script needs ``EditExposedVersion``), Settings, the graph, the
parameters' Details, then the script editor's Compile (new errors fail at the node's path) and Apply (loaded
dependents refresh and recompile: ``Niagara.DependentsUpdated``; a dependent with unsaved changes refuses the apply
before any change: ``Niagara.DependentHasUnsavedChanges``).

Emitter assets: the emitter editor without its UI.  The edits run on its working copy (a transient placeholder
system whose one emitter is a copy of the asset's exposed version), which its Apply writes back over the asset; the
Body is the system planner's emitter entry (Parent, Properties, Renderers, stacks, ScratchPads: the emitter's list is
complete here, the emitter editor's Scratch Pad panel) plus ``Versions`` / ``EditExposedVersion`` (as scripts) and
``From`` (what a new asset is copied from; ``Parent`` on a new asset makes it a child, as the New Niagara Emitter
dialog does).  Then the Compile (new errors fail) and the Apply's refresh of the loaded emitters and systems that
inherit from the asset (``Niagara.DependentsUpdated``; a dependent with unsaved changes or open in an editor refuses
the apply before any change).

Parameter collections (``niagara_parameter_collection``): Details (Namespace, Source Material Collection), then
the collection editor's parameter list and values.
"""
from __future__ import annotations

import dataclasses

from asset_document.diagnostics import (
    AssetDocumentContractError, DiagnosticSeverity, RuntimeDiagnostic, with_diagnostics,
)
from asset_document.graphs.executor import graph_requirements
from asset_document.graphs.schemas.niagara import NiagaraGraphExecutor
from asset_document import inspect_summary
from asset_document.diff.model import Difference, DiffKind
from asset_document.meta import (
    ComparisonPolicy, DefaultPolicy, DeletePolicy, IdentityPolicy, OmissionPolicy, OrderPolicy, ProfileMeta,
    RegionMeta, RegionOwnership,
)
from asset_document.mutation_intent_projection import SNAPSHOT_COVERED
from asset_document.profile_runtime import RuntimeProfileRegistration, RuntimeTargetPolicy
from asset_document.profiles import data_table_common as common
from asset_document.profiles import niagara_apply as planner
from asset_document.profiles import niagara_parameter_collection as collections
from asset_document.profiles import niagara_script as scripts
from asset_document.profiles import niagara_shape as shape
from asset_document.regions.primitive_catalog import NamedOpaqueHandleArgument
from asset_document.regions.target_lifecycle import generic_factory_lifecycle, lifecycle_requirements
from asset_document.dto import PrimitiveRequirement

ROUTE = "fx_profile"
PROVIDER = "fx"
READ = "fx.niagara.read.v1"
EDIT = "fx.niagara.edit.v1"
COMPILE = "fx.niagara.compile.v1"
PREFIX = "Niagara"
LIFECYCLE = generic_factory_lifecycle(ROUTE)
REQUIREMENTS = (tuple(PrimitiveRequirement(item, 1, ROUTE) for item in (common.CLASS_RESOLVE, common.RESOLVE, READ, EDIT,
                                                                         COMPILE))
                + lifecycle_requirements(LIFECYCLE) + graph_requirements(ROUTE))
BY_ID = {item.primitive_id: item for item in REQUIREMENTS}

SYSTEM_CLASS = "/Script/Niagara.NiagaraSystem"
EMITTER_CLASS = "/Script/Niagara.NiagaraEmitter"
SCRIPT_CLASS = "/Script/Niagara.NiagaraScript"
COLLECTION_CLASS = "/Script/Niagara.NiagaraParameterCollection"

SYSTEM_KEYS = ("UserParameters", "Parameters", "SystemSpawn", "SystemUpdate", "Emitters", "ScratchPads")
EMITTER_KEYS = ("Parent", "Properties", "EmitterSpawn", "EmitterUpdate", "ParticleSpawn", "ParticleUpdate",
                "EventHandlers", "SimulationStages", "Renderers", "ScratchPads", "Stateless")
SCRIPT_KEYS = scripts.SCRIPT_KEYS
COLLECTION_KEYS = ("Parameters",)
# Emitter assets: the emitter entry plus the asset-level keys (From: what a new asset is copied from; Versions: the
# version menu; EditExposedVersion).
EMITTER_ASSET_ONLY = ("From", "Versions", "EditExposedVersion")
EMITTER_ASSET_KEYS = EMITTER_KEYS + EMITTER_ASSET_ONLY
EMITTER_BODY_OWNED = {"VersionData": "Body (Properties, module stacks, renderers, scratch pads, parent)",
                      "ExposedVersion": "Body.Versions", "bVersioningEnabled": "Body.Versions",
                      "UniqueEmitterName": "the asset name"}
COLLECTION_BODY_OWNED = {"Parameters": "Body.Parameters", "DefaultInstance": "Body.Parameters (their values)"}
_EMITTER_PATH = "/Body/Emitters/0"
# The system's Details properties that the Body authors (never generic Properties).
# The script's state the Body authors (the version data holds Settings, Parameters and Graph).
SCRIPT_BODY_OWNED = {"Usage": "Body.Usage", "UsageId": "Body.Usage", "VersionData": "Body (Settings, Parameters, Graph)",
                     "ExposedVersion": "Body.Versions", "bVersioningEnabled": "Body.Versions"}
SYSTEM_BODY_OWNED = {"EmitterHandles": "Body.Emitters", "ExposedParameters": "Body.UserParameters",
                     "SystemSpawnScript": "Body.SystemSpawn", "SystemUpdateScript": "Body.SystemUpdate",
                     "ScratchPadScripts": "Body.ScratchPads", "EditorData": "Body", "EditorParameters": "Body"}
# Passes of one apply: add emitters; then their renderers, event handlers, stages and modules; then the new
# groups' modules and the new modules' settings and inputs; then the inputs those reveal; the last pass finds
# nothing left to do.
MAX_PASSES = 8


def read(context, handle, detail="document"):
    """(first page, every row) of one paged fx.niagara.read.v1 read."""
    proxy = context.handoff(source_handle=handle, target_provider_id=PROVIDER, access="read_only")
    handles = (NamedOpaqueHandleArgument("asset", proxy),)

    # Inside a scripts.node_defaults block the node rows also carry the Details of a new node of their kind, kept
    # aside for the comparison of authored node Properties the sparse read leaves out.
    found = scripts.collecting_node_defaults() if detail == "document" else None
    args = {"schema_version": 1, "detail": detail}
    if found is not None:
        args["node_defaults"] = True

    def page(offset):
        outcome = common.invoke(context, BY_ID, READ, {**args, "offset": offset}, handles, path="/Body", prefix=PREFIX)
        if type(outcome.value) is not dict:
            raise common.error("/PrimitiveOutcome", f"{READ} returned no object.", f"{PREFIX}.InvalidPrimitiveResult")
        if found is not None:
            for row in outcome.value.get("rows", []):
                if type(row) is dict and row.get("kind") == "node" and "property_defaults" in row:
                    found[row.get("id")] = row.pop("property_defaults")
        return outcome.value

    first = page(0)
    rows = list(first.get("rows", []))
    offset = first.get("next_offset", -1)
    while type(offset) is int and offset >= 0:
        following = page(offset)
        rows += following.get("rows", [])
        offset = following.get("next_offset", -1)
    return first, rows


def compile_errors(context, handle):
    first, _ = read(context, handle, "compile")
    return list(first.get("errors", [])), list(first.get("warnings", []))


_FIXED_DEFAULTS = ("emitter", "event_handler", "stateless", "script")
_CLASS_DEFAULTS = ("renderers", "data_interfaces", "stages", "stateless_modules")
_DEFAULTS = {"emitter": None, "event_handler": None, "stateless": None, "script": None, "renderers": {},
             "data_interfaces": {}, "stages": {}, "stateless_modules": {}}


def class_defaults(context, handle, renderers=(), interfaces=(), stages=(), modules=()):
    """Defaults the sparse Properties leave out (an emitter's, an event handler's, a lightweight emitter's, and the
    given renderer / data interface / simulation stage / lightweight module classes'); cached for the session
    (constant for a class)."""
    wanted = dict(zip(_CLASS_DEFAULTS, (renderers, interfaces, stages, modules)))
    missing = {key: [label for label in labels if label not in _DEFAULTS[key]] for key, labels in wanted.items()}
    if any(_DEFAULTS[key] is None for key in _FIXED_DEFAULTS) or any(missing.values()):
        proxy = context.handoff(source_handle=handle, target_provider_id=PROVIDER, access="read_only")
        outcome = common.invoke(context, BY_ID, READ, {"schema_version": 1, "detail": "defaults", **missing},
                                (NamedOpaqueHandleArgument("asset", proxy),), path="/Body", prefix=PREFIX)
        value = outcome.value if type(outcome.value) is dict else {}
        for key in _FIXED_DEFAULTS:
            _DEFAULTS[key] = dict(value.get(key, {}))
        for key, labels in missing.items():
            found = value.get(key, {})
            for label in labels:
                _DEFAULTS[key][label] = dict(found.get(label, {}))
    scripts.set_settings_defaults(_DEFAULTS["script"])
    return cached_defaults()


def cached_defaults():
    out = {key: _DEFAULTS[key] or {} for key in _FIXED_DEFAULTS}
    out.update({key: dict(_DEFAULTS[key]) for key in _CLASS_DEFAULTS})
    return out


def _edit(context, mutable, ops, values=None):
    """Run ops ([(op, document path)]) through fx.niagara.edit.v1; values collects each call's result."""
    changed = False
    changes = []
    for chunk in common.chunk_ops(ops):
        op_paths = [path for _, path in chunk]

        def where(primitive_path, op_paths=op_paths):
            return planner.op_path(primitive_path, op_paths)
        outcome = common.invoke(context, BY_ID, EDIT, {"schema_version": 1, "ops": [op for op, _ in chunk]},
                                (NamedOpaqueHandleArgument("asset", mutable),), paths=where, prefix=PREFIX)
        changes.extend(outcome.changed_set)
        changed = changed or (type(outcome.value) is dict and outcome.value.get("changed") is True)
        if values is not None:
            values.append(outcome.value if type(outcome.value) is dict else {})
    return changed, changes


def compile_outcome(before, result):
    """(fatal diagnostic or None, warnings) of the recompile: new errors fail the apply, errors the system already
    had are warnings (compile state not worse); new compile warnings and a compile that did not finish are warnings."""
    errors_before, warnings_before = before
    errors = list(result.get("errors", []))
    new = [error for error in errors if error not in errors_before]
    if new:
        return common.error("/Body", "The system does not compile: " + " | ".join(new[:8]), f"{PREFIX}.CompileFailed"), ()
    warnings = [RuntimeDiagnostic(path="/Body", code=f"{PREFIX}.CompileError", message=error,
                                  severity=DiagnosticSeverity.WARNING) for error in errors]
    warnings += [RuntimeDiagnostic(path="/Body", code=f"{PREFIX}.CompileWarning", message=warning,
                                   severity=DiagnosticSeverity.WARNING)
                 for warning in result.get("warnings", []) if warning not in warnings_before]
    if result.get("timed_out") is True:
        warnings.append(RuntimeDiagnostic(path="/Body", code=f"{PREFIX}.CompileTimeout", message=(
            "The recompile did not finish in time; the compile state is not known yet."), severity=DiagnosticSeverity.WARNING))
    return None, tuple(warnings)


class _ScriptSession:
    """The handles, changes and diagnostics of one script / scratch pad apply."""

    def __init__(self, context, read_handle, mutable_handle):
        self.context = context
        self.read_handle = read_handle
        self.mutable_handle = mutable_handle
        self.fx = context.handoff(source_handle=mutable_handle, target_provider_id=PROVIDER, access="mutable")
        self._graph_handles = None
        self.changes = []
        self.diagnostics = []

    def edit(self, ops, values=None):
        changed, changes = _edit(self.context, self.fx, ops, values)
        self.changes.extend(changes)
        return changed

    def bodies(self):
        """scope -> live script body."""
        _, rows = read(self.context, self.read_handle)
        return shape.script_bodies(rows)

    def executor(self, graph_path):
        if self._graph_handles is None:
            self._graph_handles = (
                self.context.handoff(source_handle=self.read_handle, target_provider_id="graph", access="read_only"),
                self.context.handoff(source_handle=self.mutable_handle, target_provider_id="graph", access="mutable"))
        return NiagaraGraphExecutor(self.context, read_handle=self._graph_handles[0], write_handle=self._graph_handles[1],
                                    route_id=ROUTE, graph_path=graph_path)

    def content(self, authored, live, path, scope, graph_path):
        """Settings, the graph, then the parameters' Details of one script (the edited version); True if edited."""
        changed = False
        if "Settings" in authored:
            op = scripts.settings_op(authored["Settings"], live.get("Settings", {}), scope)
            if op is not None:
                self.edit([(op, f"{path}/Settings")])
                changed = True
        graph_changed = graph_differs = False
        if "Graph" in authored and scripts.graph_differences(authored["Graph"], live.get("Graph", scripts.EMPTY_GRAPH),
                                                             f"{path}/Graph", strict=True):
            graph_differs = True
            # Map Get default pins take links and values once their parameter's Default Mode is Custom: they wait
            # for the parameters (a second graph round below).
            report = self.executor(graph_path).apply(authored["Graph"], f"{path}/Graph", hold_default_pins=True)
            if report is not None:
                self.changes.extend(report.changed_set)
                self.diagnostics.extend(report.diagnostics)
                graph_changed = changed = True
        purge = {"op": "purge_parameters"}
        if scope is not None:
            purge["scope"] = scope
        if graph_changed:
            # The parameter list as a reload makes it (the editor keeps unused entries until then).
            self.edit([(purge, f"{path}/Parameters")])
        if authored.get("Parameters"):
            current = self.bodies().get(scope or "", {}) if graph_changed else live
            listed = {entry.get("Name") for entry in current.get("Parameters", [])}
            # A parameter only a binding uses joins the list once that binding is set: a second round.
            deferred = {name for name in scripts.bound_targets(authored["Parameters"]) if name not in listed}
            ops = scripts.parameter_ops(authored["Parameters"], current.get("Parameters", []), f"{path}/Parameters",
                                        scope, defer=deferred)
            if ops:
                self.edit(ops)
                changed = True
            if deferred and any(entry["Name"] in deferred for entry in authored["Parameters"]):
                self.edit([(purge, f"{path}/Parameters")])
                current = self.bodies().get(scope or "", {})
                ops = scripts.parameter_ops(authored["Parameters"], current.get("Parameters", []), f"{path}/Parameters",
                                            scope, only=deferred)
                if ops:
                    self.edit(ops)
                changed = True
        if graph_differs:
            report = self.executor(graph_path).apply(authored["Graph"], f"{path}/Graph")
            if report is not None:
                self.changes.extend(report.changed_set)
                self.diagnostics.extend(report.diagnostics)
                changed = True
        return changed


def _script_live(session):
    return session.bodies().get("", {"Usage": "", "Settings": {}, "Parameters": [], "Graph": dict(scripts.EMPTY_GRAPH)})


def apply_script(document, read_handle, mutable_handle, context):
    """Usage, Versions, Settings, graph, parameters; then Compile (new errors fail) and the dependents' refresh."""
    body = scripts.check_script_body(document.get("Body", {}))
    properties = document.get("Properties", {})
    region = script_properties()
    region.check_owned(properties)
    session = _ScriptSession(context, read_handle, mutable_handle)
    live = _script_live(session)
    initialize = ("Usage" in body and body["Usage"] != live.get("Usage")) or not live["Graph"]["Nodes"]
    if not initialize and not scripts.script_differences(body, live):
        # Only the asset's other Details (validation rules, preview movie) can differ: nothing to compile.
        return tuple(region.apply(properties, read_handle, mutable_handle, context))
    before, _ = compile_errors(context, read_handle)
    values = []
    session.edit([({"op": "check_dependents"}, "/Body")], values)
    dependents = list(values[-1].get("dependents", [])) if values else []
    if initialize:
        session.edit([({"op": "initialize_script", "usage": body.get("Usage") or live.get("Usage") or "Module"},
                       "/Body/Usage")])
        live = _script_live(session)
        before = []
    if body.get("EditExposedVersion") is not True and scripts.edits_exposed_version(body, live):
        exposed = next(entry for entry in live["Versions"] if entry.get("Exposed"))
        raise common.error("/Body", f"The document edits version {exposed['Number']}, the exposed version of this "
                           "versioned script, which every user of the script follows ("
                           + (", ".join(dependents[:10]) if dependents else "no loaded asset uses it now")
                           + "). Add a new version and expose it (a new Versions entry with Exposed: true), or set "
                           "EditExposedVersion: true to edit the exposed version as the editor would.",
                           f"{PREFIX}.ExposedVersionEdit")
    if "Versions" in body:
        op = scripts.versions_op(body["Versions"], live.get("Versions", []))
        if op is not None:
            session.edit([(op, "/Body/Versions")])
            live = _script_live(session)
    # After initialize_script (which gives a new script the state of its usage's default script).
    session.changes.extend(region.apply(properties, read_handle, mutable_handle, context))
    session.content(body, live, "/Body", None, "Graph")
    outcome = common.invoke(context, BY_ID, COMPILE, {"schema_version": 1},
                            (NamedOpaqueHandleArgument("asset", session.fx),), prefix=PREFIX)
    session.changes.extend(outcome.changed_set)
    result = outcome.value if type(outcome.value) is dict else {}
    nodes = [node.get("Id") for node in body.get("Graph", {}).get("Nodes", [])]
    failures = scripts.compile_failure(before, result, nodes)
    if failures:
        raise common.error(failures[0][0], "The script does not compile: " + " | ".join(
            message for _, message in failures[:8]), f"{PREFIX}.CompileFailed")
    _, warnings = compile_outcome((before, []), result)
    values = []
    session.edit([({"op": "update_dependents"}, "/Body")], values)
    updated = list(values[-1].get("dependents", [])) if values else []
    diagnostics = list(session.diagnostics) + list(warnings)
    if updated:
        diagnostics.append(RuntimeDiagnostic(path="/Body", code=f"{PREFIX}.DependentsUpdated", message=(
            "Loaded assets that use the script were refreshed and recompiled (left unsaved, as the script editor's "
            "Apply does): " + ", ".join(updated[:20]) + ("" if len(updated) <= 20 else f" (+{len(updated) - 20})")
            + "."), severity=DiagnosticSeverity.WARNING))
    return with_diagnostics(session.changes, diagnostics)


class _PadState:
    def __init__(self):
        self.guids = False


def _pad_targets(body, live):
    """[(entry, live pad, document path, scope)] of the listed scratch pads the system has."""
    targets = []
    pads = {entry["Name"]: entry for entry in live.get("ScratchPads", [])}
    for index, entry in enumerate(body.get("ScratchPads", [])):
        if entry["Name"] in pads:
            targets.append((entry, pads[entry["Name"]], f"/Body/ScratchPads/{index}", f"/{entry['Name']}"))
    emitters = {entry["Name"]: entry for entry in live.get("Emitters", [])}
    for index, emitter in enumerate(body.get("Emitters", [])):
        current = emitters.get(emitter["Name"])
        if current is None or "ScratchPads" not in emitter:
            continue
        pads = {entry["Name"]: entry for entry in current.get("ScratchPads", [])}
        for pad_index, entry in enumerate(emitter["ScratchPads"]):
            if entry["Name"] in pads:
                targets.append((entry, pads[entry["Name"]], f"/Body/Emitters/{index}/ScratchPads/{pad_index}",
                                f"{emitter['Name']}/{entry['Name']}"))
    return targets


def apply_scratch_pads(session, body, live, state):
    """One pass of scratch pad edits (before the pass's stack edits); True if anything was edited."""
    added = False
    if "ScratchPads" in body:
        ops, _ = scripts.pad_structure(body["ScratchPads"], live.get("ScratchPads", []), "/Body/ScratchPads")
        if ops:
            session.edit(ops)
            added = True
    emitters = {entry["Name"]: entry for entry in live.get("Emitters", [])}
    for index, emitter in enumerate(body.get("Emitters", [])):
        current = emitters.get(emitter["Name"])
        if "ScratchPads" in emitter and current is not None and current.get("Mode") != "Stateless":
            scripts.check_emitter_pads(emitter["ScratchPads"], current.get("ScratchPads", []),
                                       f"/Body/Emitters/{index}/ScratchPads")
    if added:
        _, rows = read(session.context, session.read_handle)
        live = shape.system_body(rows)
    pending = [target for target in _pad_targets(body, live)
               if scripts.script_differences(target[0], target[1], target[2])]
    if not pending:
        return added
    if added or not state.guids:
        session.edit([({"op": "scratch_pad_guids"}, "/Body/ScratchPads")])
        state.guids = True
    for entry, current, path, scope in pending:
        if "Usage" in entry and entry["Usage"] != current.get("Usage"):
            raise common.error(f"{path}/Usage", f"A scratch pad's usage is set when it is added ({current.get('Usage')}): "
                               "add a new scratch pad instead.", f"{PREFIX}.UsageImmutable")
        session.content(entry, current, path, scope, scope)
        session.edit([({"op": "refresh_scratch_pad", "scope": scope}, path)])
    return True


def apply_system(document, read_handle, mutable_handle, context):
    """Properties, then the Body in passes, then one recompile when anything changed."""
    body = planner.check_body(document.get("Body", {}))
    properties = document.get("Properties", {})
    region = system_properties()
    region.check_owned(properties)
    changes = list(region.apply(properties, read_handle, mutable_handle, context))
    changed = bool(changes)
    before = compile_errors(context, read_handle)
    mutable = context.handoff(source_handle=mutable_handle, target_provider_id=PROVIDER, access="mutable")
    session = _ScriptSession(context, read_handle, mutable_handle)
    pads = _PadState()
    plan = None
    live = {}
    for _ in range(MAX_PASSES):
        _, rows = read(context, read_handle)
        live = shape.system_body(rows)
        if apply_scratch_pads(session, body, live, pads):
            changed = True
            _, rows = read(context, read_handle)
            live = shape.system_body(rows)
        defaults = class_defaults(context, read_handle, *planner.defaults_wanted(body, live))
        plan = planner.plan_system(body, live, defaults)
        if plan.refusals:
            raise plan.refusals[0]
        if not plan.ops:
            break
        pass_changed, pass_changes = _edit(context, mutable, plan.ops)
        changes.extend(pass_changes)
        if not pass_changed:
            # The editor holds these values already (a default the read leaves out): nothing left to do.
            plan.ops = []
            break
        changed = True
    if plan is not None and plan.ops:
        raise common.error(plan.ops[0][1], f"The system did not take the document's value after {MAX_PASSES} passes "
                           "(the editor changes it back); still different: "
                           + ", ".join(sorted({path for _, path in plan.ops})[:10]) + ".", f"{PREFIX}.ApplyDidNotConverge")
    if "ScratchPads" in body:
        # Deleted last: the stacks no longer use them (a delete clears the calls that still do).
        _, deletes = scripts.pad_structure(body["ScratchPads"], live.get("ScratchPads", []), "/Body/ScratchPads")
        if deletes:
            session.edit([({"op": "delete_scratch_pad", "name": name}, "/Body/ScratchPads") for name in deletes])
            changed = True
    changes.extend(session.changes)
    warnings = ()
    if changed:
        outcome = common.invoke(context, BY_ID, COMPILE, {"schema_version": 1},
                                (NamedOpaqueHandleArgument("asset", mutable),), prefix=PREFIX)
        changes.extend(outcome.changed_set)
        failure, warnings = compile_outcome(before, outcome.value if type(outcome.value) is dict else {})
        if failure is not None:
            raise failure
    return with_diagnostics(changes, tuple(session.diagnostics) + tuple(warnings))


# ---------------------------------------------------------------------------
# Emitter assets
# ---------------------------------------------------------------------------
def check_emitter_asset_body(body):
    """Structural checks of an emitter asset Body: one emitter entry (without Name / Enabled / Mode) plus From,
    Versions and EditExposedVersion."""
    if type(body) is not dict:
        raise common.error("/Body", "The Body is an object.", f"{PREFIX}.InvalidBody")
    for key in body:
        if key not in EMITTER_ASSET_KEYS:
            raise common.error(f"/Body/{common.escape(key)}", f"Unknown emitter asset key '{key}'. Keys: "
                               + ", ".join(EMITTER_ASSET_KEYS) + ".", f"{PREFIX}.UnknownKey")
    if "Stateless" in body:
        raise common.error("/Body/Stateless", "An emitter asset is a standard emitter: lightweight emitters are made "
                           "inside a system (an Emitters entry with Mode: \"Stateless\").", f"{PREFIX}.StatelessEmitter")
    if "From" in body:
        planner._emitter_reference(body["From"], "/Body/From", "From")
        if body.get("Parent") is not None:
            raise common.error("/Body/From", "From makes the new asset a copy of another emitter (no parent), Parent "
                               "makes it a child of one: give one of them.", f"{PREFIX}.InvalidBody")
    if "Versions" in body:
        scripts.check_versions(body["Versions"], "/Body/Versions")
    if "EditExposedVersion" in body and type(body["EditExposedVersion"]) is not bool:
        raise common.error("/Body/EditExposedVersion", "EditExposedVersion is a boolean.", f"{PREFIX}.InvalidBody")
    planner._check_emitter(_emitter_entry(body, "Emitter"), "/Body")
    return body


def _emitter_entry(body, name):
    """The system Emitters entry the planner compares (the asset-level keys left out)."""
    return {"Name": name, **{key: value for key, value in body.items() if key not in EMITTER_ASSET_ONLY}}


def _rebase(path):
    """/Body/Emitters/0/... (the planner's path of the working copy's emitter) -> /Body/..."""
    if path.startswith(_EMITTER_PATH):
        return "/Body" + path[len(_EMITTER_PATH):]
    return "/Body" if path == "/Body/Emitters" else path


def plan_emitter_asset(body, name, live_system, defaults):
    """The planner's edits of the working copy's emitter (named by its unique emitter name), at document paths."""
    plan = planner.plan_system({"Emitters": [_emitter_entry(body, name)]}, live_system, defaults)
    plan.ops = [(op, _rebase(path)) for op, path in plan.ops]
    plan.differences = [dataclasses.replace(item, path=_rebase(item.path)) for item in plan.differences]
    plan.refusals = [AssetDocumentContractError(dataclasses.replace(item.diagnostic, path=_rebase(item.diagnostic.path)))
                     for item in plan.refusals]
    return plan


def _emitter_live(context, handle):
    """(the emitter_asset row, the working copy as a system Body)."""
    _, rows = read(context, handle)
    return shape.emitter_asset_row(rows), shape.system_body(rows)


def _emitter_name(row, live):
    """The working copy's handle name (what the ops and reads use); a new asset's unique emitter name may be unset."""
    return next((entry["Name"] for entry in live.get("Emitters", [])), None) or row.get("name") or "Emitter"


def _dependents_warning(updated, what):
    return RuntimeDiagnostic(path="/Body", code=f"{PREFIX}.DependentsUpdated", message=(
        f"Loaded assets that use the {what} were refreshed and recompiled (left unsaved, as the {what} editor's Apply "
        "does): " + ", ".join(updated[:20]) + ("" if len(updated) <= 20 else f" (+{len(updated) - 20})") + "."),
        severity=DiagnosticSeverity.WARNING)


def _exposed_version_error(live_versions, dependents, what):
    exposed = next(entry for entry in live_versions if entry.get("Exposed"))
    return common.error("/Body", f"The document edits version {exposed['Number']}, the exposed version of this "
                        f"versioned {what}, which every user of the {what} follows ("
                        + (", ".join(dependents[:10]) if dependents else "no loaded asset uses it now")
                        + "). Add a new version and expose it (a new Versions entry with Exposed: true), or set "
                        "EditExposedVersion: true to edit the exposed version as the editor would.",
                        f"{PREFIX}.ExposedVersionEdit")


def apply_emitter_pads(session, name, body, live, state):
    """One pass of the emitter editor's Scratch Pad panel (its list is complete: add / rename here, deletes at the
    end), then the listed pads' content and their Apply; True if anything was edited."""
    if "ScratchPads" not in body:
        return False
    current = next(iter(live.get("Emitters", [])), {})
    added = False
    ops, _ = scripts.pad_structure(body["ScratchPads"], current.get("ScratchPads", []), "/Body/ScratchPads")
    if ops:
        session.edit(ops)
        added = True
        _, live = _emitter_live(session.context, session.read_handle)
        current = next(iter(live.get("Emitters", [])), {})
        name = current.get("Name", name)  # the write-back names a new asset's emitter after the asset
    pads = {entry["Name"]: entry for entry in current.get("ScratchPads", [])}
    pending = []
    for index, entry in enumerate(body["ScratchPads"]):
        path = f"/Body/ScratchPads/{index}"
        if entry["Name"] in pads and scripts.script_differences(entry, pads[entry["Name"]], path):
            pending.append((entry, pads[entry["Name"]], path, f"{name}/{entry['Name']}"))
    if not pending:
        return added
    if added or not state.guids:
        session.edit([({"op": "scratch_pad_guids"}, "/Body/ScratchPads")])
        state.guids = True
    for entry, pad, path, scope in pending:
        if "Usage" in entry and entry["Usage"] != pad.get("Usage"):
            raise common.error(f"{path}/Usage", f"A scratch pad's usage is set when it is added ({pad.get('Usage')}): "
                               "add a new scratch pad instead.", f"{PREFIX}.UsageImmutable")
        session.content(entry, pad, path, scope, scope)
        session.edit([({"op": "refresh_scratch_pad", "scope": scope}, path)])
    return True


def _initialize_op(body, row, live):
    """The initialize_emitter op of a new asset made from another emitter (From: a copy, Parent: a child), else None."""
    if row.get("new") is not True:
        return None
    current = next(iter(live.get("Emitters", [])), {})
    if "From" in body:
        reference, inherit, path = body["From"], False, "/Body/From"
    elif body.get("Parent") is not None and current.get("Parent") is None:
        reference, inherit, path = body["Parent"], True, "/Body/Parent"
    else:
        return None
    reference = planner._emitter_reference(reference, path, "emitter")
    op = {"op": "initialize_emitter", "emitter": planner._object_path(reference["Emitter"]), "inherit": inherit}
    if "Version" in reference:
        op["version"] = reference["Version"]
    return op, path


def apply_emitter(document, read_handle, mutable_handle, context):
    """The emitter editor without its UI: the edits run on its working copy, which its Apply writes back over the
    asset; then the Compile, and the Apply's refresh of the loaded emitters and systems that inherit from it."""
    body = check_emitter_asset_body(document.get("Body", {}))
    properties = document.get("Properties", {})
    region = emitter_properties()
    region.check_owned(properties)
    row, live = _emitter_live(context, read_handle)
    name = _emitter_name(row, live)
    initialize = _initialize_op(body, row, live)
    defaults = class_defaults(context, read_handle, *planner.defaults_wanted(
        {"Emitters": [_emitter_entry(body, name)]}, live))
    plan = plan_emitter_asset(body, name, live, defaults)
    versions = row.get("versions", [])
    version_changes = scripts.version_differences(body["Versions"], versions, "/Body/Versions") if "Versions" in body else []
    if initialize is None and not plan.differences and not version_changes:
        # Only the asset's own Details can differ: nothing to write back or compile.
        return tuple(region.apply(properties, read_handle, mutable_handle, context))
    session = _ScriptSession(context, read_handle, mutable_handle)
    values = []
    session.edit([({"op": "check_dependents"}, "/Body")], values)
    dependents = list(values[-1].get("dependents", [])) if values else []
    if initialize is not None:
        session.edit([initialize])
    # A new asset reads as its emitter once the edit session has made it (the factory's emitter, or the initialized one).
    row, live = _emitter_live(context, read_handle)
    name = _emitter_name(row, live)
    versions = row.get("versions", [])
    before = compile_errors(context, read_handle)
    if body.get("EditExposedVersion") is not True:
        defaults = class_defaults(context, read_handle, *planner.defaults_wanted(
            {"Emitters": [_emitter_entry(body, name)]}, live))
        content = plan_emitter_asset(body, name, live, defaults).differences
        if scripts.edits_exposed_version(body, {"Versions": versions}, changed=bool(content)):
            raise _exposed_version_error(versions, dependents, "emitter")
    if "Versions" in body:
        op = scripts.versions_op(body["Versions"], versions)
        if op is not None:
            session.edit([(op, "/Body/Versions")])
    session.changes.extend(region.apply(properties, read_handle, mutable_handle, context))
    pads = _PadState()
    plan = None
    for _ in range(MAX_PASSES):
        # A write-back names the emitter after its asset (UNiagaraEmitter::PostDuplicate), as the editor's Apply does.
        _, live = _emitter_live(context, read_handle)
        name = _emitter_name(row, live)
        if apply_emitter_pads(session, name, body, live, pads):
            # The pads' write-back can name the emitter too.
            _, live = _emitter_live(context, read_handle)
            name = _emitter_name(row, live)
        defaults = class_defaults(context, read_handle, *planner.defaults_wanted(
            {"Emitters": [_emitter_entry(body, name)]}, live))
        plan = plan_emitter_asset(body, name, live, defaults)
        if plan.refusals:
            raise plan.refusals[0]
        if not plan.ops or not session.edit(plan.ops):
            plan.ops = []
            break
    if plan is not None and plan.ops:
        raise common.error(plan.ops[0][1], f"The emitter did not take the document's value after {MAX_PASSES} passes "
                           "(the editor changes it back); still different: "
                           + ", ".join(sorted({path for _, path in plan.ops})[:10]) + ".", f"{PREFIX}.ApplyDidNotConverge")
    if "ScratchPads" in body:
        current = next(iter(live.get("Emitters", [])), {})
        _, deletes = scripts.pad_structure(body["ScratchPads"], current.get("ScratchPads", []), "/Body/ScratchPads")
        if deletes:
            session.edit([({"op": "delete_scratch_pad", "name": pad}, "/Body/ScratchPads") for pad in deletes])
    # The emitter editor's Compile (its working copy compiles through the placeholder system) and Apply.
    outcome = common.invoke(context, BY_ID, COMPILE, {"schema_version": 1},
                            (NamedOpaqueHandleArgument("asset", session.fx),), prefix=PREFIX)
    session.changes.extend(outcome.changed_set)
    failure, warnings = compile_outcome(before, outcome.value if type(outcome.value) is dict else {})
    if failure is not None:
        raise failure
    values = []
    session.edit([({"op": "update_dependents"}, "/Body")], values)
    updated = list(values[-1].get("dependents", [])) if values else []
    diagnostics = list(session.diagnostics) + list(warnings)
    if updated:
        diagnostics.append(_dependents_warning(updated, "emitter"))
    return with_diagnostics(session.changes, diagnostics)


class _EmitterProperties(common.PropertiesRegion):
    """The emitter asset's Details outside its version data (the emitter editor's asset settings)."""


def emitter_properties():
    return _EmitterProperties(EMITTER_CLASS, EMITTER_BODY_OWNED, PREFIX)


def emitter_diff(authored, extracted):
    changes = list(common.header_diff(authored, extracted))
    changes += common.properties_diff(authored.get("Properties", {}), extracted.get("Properties", {}), False)
    body = authored.get("Body", {})
    live = extracted.get("Body", {})
    live_system = {"Emitters": [_emitter_entry(live, "Emitter")]}
    changes += plan_emitter_asset(body, "Emitter", live_system, cached_defaults()).differences
    if "Versions" in body:
        changes += scripts.version_differences(body["Versions"], live.get("Versions", []), "/Body/Versions")
    return tuple(changes)


# ---------------------------------------------------------------------------
# Parameter collections
# ---------------------------------------------------------------------------
class _CollectionProperties(common.PropertiesRegion):
    """The collection's Details (Namespace, Source Material Collection)."""


def collection_properties():
    return _CollectionProperties(COLLECTION_CLASS, COLLECTION_BODY_OWNED, PREFIX, editor_state=("CompileId",))


def _collection_interfaces(body, live):
    labels = set()
    for source in (body, live):
        for entry in source.get("Parameters", []) if type(source.get("Parameters")) is list else []:
            value = entry.get("Value") if type(entry) is dict else None
            if type(value) is dict and type(value.get("DataInterface")) is dict \
                    and type(value["DataInterface"].get("Class")) is str:
                labels.add(value["DataInterface"]["Class"])
    return sorted(labels)


def apply_collection(document, read_handle, mutable_handle, context):
    """Details first (Namespace, the source material collection, whose parameters then join the list), then the
    collection editor's parameter list in passes (a new or retyped parameter's value is set in the pass after)."""
    body = collections.check_body(document.get("Body", {}))
    properties = document.get("Properties", {})
    region = collection_properties()
    changes = list(region.apply(properties, read_handle, mutable_handle, context))
    if "Namespace" in properties:
        _, rows = read(context, read_handle)
        live_namespace = next((row.get("namespace") for row in rows if row["kind"] == "collection"), None)
        if live_namespace != properties["Namespace"]:
            raise common.error("/Properties/Namespace", f"Another parameter collection uses the namespace "
                               f"'{properties['Namespace']}' (the editor renamed it to '{live_namespace}'): choose a "
                               "namespace no other collection uses.", f"{PREFIX}.NamespaceInUse")
    mutable = context.handoff(source_handle=mutable_handle, target_provider_id=PROVIDER, access="mutable")
    plan = None
    for _ in range(MAX_PASSES):
        _, rows = read(context, read_handle)
        live = shape.collection_body(rows)
        defaults = class_defaults(context, read_handle, (), _collection_interfaces(body, live))
        plan = collections.plan_collection(body, live, defaults["data_interfaces"])
        if plan.refusals:
            raise plan.refusals[0]
        if not plan.ops:
            break
        pass_changed, pass_changes = _edit(context, mutable, plan.ops)
        changes.extend(pass_changes)
        if not pass_changed:
            plan.ops = []
            break
    if plan is not None and plan.ops:
        raise common.error(plan.ops[0][1], f"The collection did not take the document's value after {MAX_PASSES} "
                           "passes; still different: " + ", ".join(sorted({path for _, path in plan.ops})[:10]) + ".",
                           f"{PREFIX}.ApplyDidNotConverge")
    return tuple(changes)


def collection_diff(authored, extracted, exact):
    changes = list(common.header_diff(authored, extracted))
    changes += common.properties_diff(authored.get("Properties", {}), extracted.get("Properties", {}), exact)
    changes += collections.plan_collection(authored.get("Body", {}), extracted.get("Body", {}),
                                           cached_defaults()["data_interfaces"]).differences
    return tuple(changes)


class _SystemProperties(common.PropertiesRegion):
    """The system's Details panel (generic property admission)."""


def system_properties():
    return _SystemProperties(SYSTEM_CLASS, SYSTEM_BODY_OWNED, PREFIX)


def system_diff(authored, extracted, exact):
    changes = list(common.header_diff(authored, extracted))
    changes += common.properties_diff(authored.get("Properties", {}), extracted.get("Properties", {}), exact)
    body = authored.get("Body", {})
    live = extracted.get("Body", {})
    changes += planner.plan_system(body, live, cached_defaults()).differences
    return tuple(changes)


def diff(authored, extracted, keys, exact):
    """Does the asset hold what the document authors?  Omitted Body keys keep; present ones compare whole."""
    changes = common.header_diff(authored, extracted)
    changes += common.properties_diff(authored.get("Properties", {}), extracted.get("Properties", {}), exact)
    body = authored.get("Body", {})
    live = extracted.get("Body", {})
    for key in keys:
        if key in body and not common.same(body[key], live.get(key), exact):
            changes.append(Difference(f"/Body/{key}", DiffKind.REPLACE, True, body[key], key in live, live.get(key)))
    return tuple(changes)


class _NiagaraReadProfile:
    """One Niagara Profile: subclasses name the class, Body keys and the row shaping, and the write phases."""

    runtime_entry_module = "asset_document/bootstrap.py"
    preflight_scratch = False
    profile_id = ""
    exact_asset_class_path = ""
    body_keys = ()
    template_body = {}

    def body(self, rows):
        raise NotImplementedError

    def required_primitives(self):
        return REQUIREMENTS

    def inspect_profile(self):
        return {"Class": self.exact_asset_class_path, "BodySections": list(self.body_keys),
                "ReadOnlySections": list(self.body_keys), "ApplyAvailable": False}

    def canonicalize(self, document):
        common.validate_header(document, (self.exact_asset_class_path,), PREFIX, self.body_keys)
        return common.snapshot(document)

    def validate(self, document):
        self.canonicalize(document)

    def create_template(self, target):
        return common.document(common.require_target(target, PREFIX), self.exact_asset_class_path, {},
                               dict(self.template_body))

    def create_target_context(self, document, context):
        raise NotImplementedError

    def preflight(self, document, scratch_handle, context):
        raise NotImplementedError

    def apply(self, document, read_asset_handle, mutable_asset_handle, context):
        raise NotImplementedError

    def repair(self, document, asset_handle, context):
        raise NotImplementedError

    def document(self, target, page, rows):
        return common.document(target, self.exact_asset_class_path, dict(page.get("properties", {})), self.body(rows))

    def extract(self, *, target, asset_handle, context, include_defaults=False):
        page, rows = read(context, asset_handle)
        with shape.sparse_data_interfaces(not include_defaults):
            return self.document(common.require_target(target, PREFIX), page, rows)

    def summarize_inspect(self, inspected):
        """The inspect over HTTP (unless detail is true): the shared summary with NiagaraDetails summarized
        (niagara_shape.summarize)."""
        if type(inspected.get("NiagaraDetails")) is not dict:
            return inspect_summary.summarize(inspected)
        return inspect_summary.summarize(inspected, shape.SUMMARY_NOTE, facts={"NiagaraDetails": shape.summarize})

    def inspect(self, *, mode, exact_class_path, exact_asset_object_path, editor_asset_handle, context):
        if mode != "live_asset" or exact_class_path != self.exact_asset_class_path or editor_asset_handle is None:
            raise common.error("/Inspect", "Niagara inspection reads a live Niagara asset.", f"{PREFIX}.InspectNotAdmitted")
        page, rows = read(context, editor_asset_handle, "inspect")
        document = self.document(exact_asset_object_path.rsplit(".", 1)[0], page, rows)
        return {**document, "NiagaraDetails": shape.inspect_facts(rows, page)}

    def diff(self, authored, extracted):
        return diff(self.canonicalize(authored), self.canonicalize(extracted), self.body_keys, exact=False)

    def diff_exact(self, authored, extracted):
        return diff(self.canonicalize(authored), self.canonicalize(extracted), self.body_keys, exact=True)

    def extract_with_defaults(self, *, target, asset_handle, context):
        """Diff / Verify: the document and the defaults the sparse read leaves out (the Details of a new node of each
        script graph node's kind; the Settings of a new script, cached by class_defaults)."""
        class_defaults(context, asset_handle)
        with scripts.node_defaults({}) as found:
            # Every Properties value (include_defaults): an authored value equal to its default must read back present.
            document = self.extract(target=target, asset_handle=asset_handle, context=context, include_defaults=True)
        return document, {"nodes": found}

    def diff_with_defaults(self, authored, extracted, live_defaults, exact):
        with scripts.node_defaults(dict(live_defaults.get("nodes", {}))):
            return self.diff_exact(authored, extracted) if exact else self.diff(authored, extracted)


class NiagaraSystemProfile(_NiagaraReadProfile):
    """Systems: Apply (system level, module stacks, scratch pads)."""

    profile_id = "profile.niagara_system.v1"
    exact_asset_class_path = SYSTEM_CLASS
    body_keys = SYSTEM_KEYS
    template_body = {"UserParameters": [], "Emitters": []}

    def body(self, rows):
        return shape.system_body(rows)

    def inspect_profile(self):
        return {"Class": self.exact_asset_class_path, "BodySections": list(self.body_keys), "ReadOnlySections": [],
                "ApplyAvailable": True}

    def canonicalize(self, document):
        canonical = super().canonicalize(document)
        planner.check_body(canonical.get("Body", {}))
        return canonical

    def create_target_context(self, document, context):
        self.canonicalize(document)
        return ()

    def preflight(self, document, scratch_handle, context):
        canonical = self.canonicalize(document)
        system_properties().validate(canonical.get("Properties", {}), context)
        return SNAPSHOT_COVERED

    def apply(self, document, read_asset_handle, mutable_asset_handle, context):
        # The apply's reads also report the new nodes' Details (the comparison of authored node Properties).
        with scripts.node_defaults({}):
            return apply_system(self.canonicalize(document), read_asset_handle, mutable_asset_handle, context)

    def repair(self, document, asset_handle, context):
        self.canonicalize(document)
        return ()

    def extract(self, *, target, asset_handle, context, include_defaults=False):
        page, rows = read(context, asset_handle)
        with shape.sparse_data_interfaces(not include_defaults):
            body = self.body(rows)
        class_defaults(context, asset_handle, *planner.defaults_wanted({}, body))
        return common.document(common.require_target(target, PREFIX), SYSTEM_CLASS,
                               system_properties().extract(asset_handle, context, include_defaults), body)

    def inspect(self, *, mode, exact_class_path, exact_asset_object_path, editor_asset_handle, context):
        if mode != "live_asset" or exact_class_path != self.exact_asset_class_path or editor_asset_handle is None:
            raise common.error("/Inspect", "Niagara inspection reads a live Niagara asset.", f"{PREFIX}.InspectNotAdmitted")
        page, rows = read(context, editor_asset_handle, "inspect")
        document = common.document(exact_asset_object_path.rsplit(".", 1)[0], SYSTEM_CLASS,
                                   system_properties().extract(editor_asset_handle, context), self.body(rows))
        return {**document, "NiagaraDetails": shape.inspect_facts(rows, page)}

    def diff(self, authored, extracted):
        return system_diff(self.canonicalize(authored), self.canonicalize(extracted), exact=False)

    def diff_exact(self, authored, extracted):
        return system_diff(self.canonicalize(authored), self.canonicalize(extracted), exact=True)


class _ApplyingProfile(_NiagaraReadProfile):
    """A Niagara Profile that applies: subclasses name the Details region, the Body check and the apply / diff."""

    def properties_region(self):
        raise NotImplementedError

    def check_body(self, body):
        raise NotImplementedError

    def inspect_profile(self):
        return {"Class": self.exact_asset_class_path, "BodySections": list(self.body_keys), "ReadOnlySections": [],
                "ApplyAvailable": True}

    def canonicalize(self, document):
        canonical = super().canonicalize(document)
        self.properties_region().check_owned(canonical.get("Properties", {}))
        self.check_body(canonical.get("Body", {}))
        return canonical

    def create_target_context(self, document, context):
        self.canonicalize(document)
        return ()

    def preflight(self, document, scratch_handle, context):
        canonical = self.canonicalize(document)
        self.properties_region().validate(canonical.get("Properties", {}), context)
        return SNAPSHOT_COVERED

    def repair(self, document, asset_handle, context):
        self.canonicalize(document)
        return ()

    def extract(self, *, target, asset_handle, context, include_defaults=False):
        page, rows = read(context, asset_handle)
        with shape.sparse_data_interfaces(not include_defaults):
            body = self.body(rows)
        return common.document(common.require_target(target, PREFIX), self.exact_asset_class_path,
                               self.properties_region().extract(asset_handle, context, include_defaults), body)

    def inspect(self, *, mode, exact_class_path, exact_asset_object_path, editor_asset_handle, context):
        if mode != "live_asset" or exact_class_path != self.exact_asset_class_path or editor_asset_handle is None:
            raise common.error("/Inspect", "Niagara inspection reads a live Niagara asset.", f"{PREFIX}.InspectNotAdmitted")
        page, rows = read(context, editor_asset_handle, "inspect")
        document = common.document(exact_asset_object_path.rsplit(".", 1)[0], self.exact_asset_class_path,
                                   self.properties_region().extract(editor_asset_handle, context), self.body(rows))
        return {**document, "NiagaraDetails": shape.inspect_facts(rows, page)}


class NiagaraEmitterProfile(_ApplyingProfile):
    """Emitter assets: the emitter editor's working copy, its Apply, Compile and Versions."""

    profile_id = "profile.niagara_emitter.v1"
    exact_asset_class_path = EMITTER_CLASS
    body_keys = EMITTER_ASSET_KEYS
    # A new asset keeps what the New Niagara Emitter factory made unless From / Parent name an emitter to start from.
    template_body = {}

    def body(self, rows):
        return shape.emitter_asset_body(rows)

    def properties_region(self):
        return emitter_properties()

    def check_body(self, body):
        check_emitter_asset_body(body)

    def apply(self, document, read_asset_handle, mutable_asset_handle, context):
        # The apply's reads also report the new nodes' Details (the comparison of authored node Properties).
        with scripts.node_defaults({}):
            return apply_emitter(self.canonicalize(document), read_asset_handle, mutable_asset_handle, context)

    def extract(self, *, target, asset_handle, context, include_defaults=False):
        document = super().extract(target=target, asset_handle=asset_handle, context=context,
                                   include_defaults=include_defaults)
        body = document["Body"]
        class_defaults(context, asset_handle, *planner.defaults_wanted({}, {"Emitters": [_emitter_entry(body, "E")]}))
        return document

    def diff(self, authored, extracted):
        return emitter_diff(self.canonicalize(authored), self.canonicalize(extracted))

    def diff_exact(self, authored, extracted):
        return emitter_diff(self.canonicalize(authored), self.canonicalize(extracted))


class _ScriptProperties(common.PropertiesRegion):
    """The script asset's Details outside the version data (validation rules, preview movie)."""


def script_properties():
    return _ScriptProperties(SCRIPT_CLASS, SCRIPT_BODY_OWNED, PREFIX)


def script_diff(authored, extracted):
    changes = list(common.header_diff(authored, extracted))
    changes += common.properties_diff(authored.get("Properties", {}), extracted.get("Properties", {}), False)
    changes += scripts.script_differences(authored.get("Body", {}), extracted.get("Body", {}))
    return tuple(changes)


class NiagaraScriptProfile(_NiagaraReadProfile):
    """Module / function / dynamic input scripts: Apply (usage, versions, settings, graph, parameters)."""

    profile_id = "profile.niagara_script.v1"
    exact_asset_class_path = SCRIPT_CLASS
    body_keys = SCRIPT_KEYS
    # A new script starts as the New-asset factory of its usage makes it (input map, Map Get / Set, output); the
    # template leaves the graph out so that applying it keeps those nodes.
    template_body = {"Usage": "Module"}

    def body(self, rows):
        return shape.script_asset_body(rows)

    def inspect_profile(self):
        return {"Class": self.exact_asset_class_path, "BodySections": list(self.body_keys), "ReadOnlySections": [],
                "ApplyAvailable": True}

    def canonicalize(self, document):
        canonical = super().canonicalize(document)
        script_properties().check_owned(canonical.get("Properties", {}))
        scripts.check_script_body(canonical.get("Body", {}))
        return canonical

    def create_target_context(self, document, context):
        self.canonicalize(document)
        return ()

    def preflight(self, document, scratch_handle, context):
        canonical = self.canonicalize(document)
        script_properties().validate(canonical.get("Properties", {}), context)
        return SNAPSHOT_COVERED

    def apply(self, document, read_asset_handle, mutable_asset_handle, context):
        # The apply's reads also report the new nodes' Details (the comparison of authored node Properties).
        with scripts.node_defaults({}):
            class_defaults(context, read_asset_handle)
            return apply_script(self.canonicalize(document), read_asset_handle, mutable_asset_handle, context)

    def repair(self, document, asset_handle, context):
        self.canonicalize(document)
        return ()

    def diff(self, authored, extracted):
        return script_diff(self.canonicalize(authored), self.canonicalize(extracted))

    def diff_exact(self, authored, extracted):
        return script_diff(self.canonicalize(authored), self.canonicalize(extracted))


class NiagaraParameterCollectionProfile(_ApplyingProfile):
    """Parameter collections: the collection editor's parameter list and values, and its Details."""

    profile_id = "profile.niagara_parameter_collection.v1"
    exact_asset_class_path = COLLECTION_CLASS
    body_keys = COLLECTION_KEYS
    template_body = {"Parameters": []}

    def body(self, rows):
        return shape.collection_body(rows)

    def properties_region(self):
        return collection_properties()

    def check_body(self, body):
        collections.check_body(body)

    def apply(self, document, read_asset_handle, mutable_asset_handle, context):
        return apply_collection(self.canonicalize(document), read_asset_handle, mutable_asset_handle, context)

    def extract(self, *, target, asset_handle, context, include_defaults=False):
        document = super().extract(target=target, asset_handle=asset_handle, context=context,
                                   include_defaults=include_defaults)
        class_defaults(context, asset_handle, (), _collection_interfaces({}, document["Body"]))
        return document

    def diff(self, authored, extracted):
        return collection_diff(self.canonicalize(authored), self.canonicalize(extracted), exact=False)

    def diff_exact(self, authored, extracted):
        return collection_diff(self.canonicalize(authored), self.canonicalize(extracted), exact=True)


def _registration(handler_class, regions):
    rows = tuple(RegionMeta(f"niagara.{name}.v1", f"/Body/{key}", ownership[0] if ownership else RegionOwnership.OBSERVED,
                            identity, OrderPolicy.PRESERVE, DefaultPolicy.EXPLICIT, OmissionPolicy.PRESERVE,
                            DeletePolicy.EXPLICIT_ONLY, ComparisonPolicy.ORDERED)
                 for name, key, identity, *ownership in regions)
    meta = ProfileMeta(handler_class.profile_id, ROUTE, handler_class.exact_asset_class_path, rows, REQUIREMENTS)
    return RuntimeProfileRegistration(meta, handler_class, RuntimeTargetPolicy.CREATE_OR_UPDATE, LIFECYCLE)


NIAGARA_SYSTEM_RUNTIME_REGISTRATION = _registration(NiagaraSystemProfile, (
    ("system.user_parameters", "UserParameters", IdentityPolicy.STABLE_KEY, RegionOwnership.MANAGED),
    ("system.parameters", "Parameters", IdentityPolicy.STABLE_KEY, RegionOwnership.MANAGED),
    ("system.system_spawn", "SystemSpawn", IdentityPolicy.STABLE_KEY, RegionOwnership.MANAGED),
    ("system.system_update", "SystemUpdate", IdentityPolicy.STABLE_KEY, RegionOwnership.MANAGED),
    ("system.emitters", "Emitters", IdentityPolicy.STABLE_KEY, RegionOwnership.MANAGED),
    ("system.scratch_pads", "ScratchPads", IdentityPolicy.STABLE_KEY, RegionOwnership.MANAGED)))
NIAGARA_EMITTER_RUNTIME_REGISTRATION = _registration(NiagaraEmitterProfile, tuple(
    (f"emitter.{key.lower()}", key, IdentityPolicy.STABLE_KEY, RegionOwnership.MANAGED)
    for key in EMITTER_KEYS + ("Versions",) if key != "Stateless"))
NIAGARA_SCRIPT_RUNTIME_REGISTRATION = _registration(NiagaraScriptProfile, tuple(
    (f"script.{key.lower()}", key, IdentityPolicy.STABLE_KEY, RegionOwnership.MANAGED)
    for key in ("Usage", "Settings", "Versions", "Parameters", "Graph")))
NIAGARA_PARAMETER_COLLECTION_RUNTIME_REGISTRATION = _registration(NiagaraParameterCollectionProfile, (
    ("parameter_collection.parameters", "Parameters", IdentityPolicy.STABLE_KEY, RegionOwnership.MANAGED),))
NIAGARA_RUNTIME_REGISTRATIONS = (NIAGARA_SYSTEM_RUNTIME_REGISTRATION, NIAGARA_EMITTER_RUNTIME_REGISTRATION,
                                 NIAGARA_SCRIPT_RUNTIME_REGISTRATION, NIAGARA_PARAMETER_COLLECTION_RUNTIME_REGISTRATION)

__all__ = ("NIAGARA_RUNTIME_REGISTRATIONS", "NiagaraSystemProfile", "NiagaraEmitterProfile", "NiagaraScriptProfile",
           "NiagaraParameterCollectionProfile", "REQUIREMENTS", "read", "compile_outcome", "system_diff", "script_diff",
           "emitter_diff", "collection_diff", "apply_script", "apply_scratch_pads", "apply_emitter", "apply_collection",
           "check_emitter_asset_body", "plan_emitter_asset")
