# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Body.Director of a Level Sequence: its Director Blueprint (design §3.6, §7.2), authored as a Blueprint.

The Director Blueprint is an object inside the Level Sequence package (Sequencer's "Open Director Blueprint"). The
region holds the Blueprint profile's own member and graph keys (every key optional; an omitted key keeps the live
state, a present key is its complete desired state)::

    "Director": {
      "ImplementedInterfaces": [...], "Variables": [...], "ClassDefaults": {...},
      "EventDispatchers": [...],
      "UbergraphPages": [...], "FunctionGraphs": [...], "MacroGraphs": [...]
    }

``"Director": {}`` makes sure a Director exists (``seq.edit.v1`` director.create), ``"Director": null`` removes it
(director.remove; refused with Sequencer.DirectorInUse while event keys or director bindings call it). Its parent class
is always LevelSequenceDirector and it has no components or timelines.

Members go through ``blueprint.member.*`` and graphs through the graph executor (K2 adapter): the "blueprint" and
"graph" providers resolve the Level Sequence's own handles to its Director Blueprint (hosted Blueprint), so the edits
share the sequence's package snapshot, transaction and Undo. Errors are reported under /Body/Director.
"""
from __future__ import annotations

import dataclasses

from asset_document.diagnostics import AssetDocumentContractError, RuntimeDiagnostic, contract_error
from asset_document.diff.model import Difference, DiffKind
from asset_document.dto import PrimitiveRequirement
from asset_document.graphs.executor import GraphExecutor, graph_requirements
from asset_document.graphs.extract import extract_graphs
from asset_document.graphs.reconcile import PlanOp, pending_relayout as _pending_relayout
from asset_document.graphs.schemas.k2 import K2_ADAPTER, SECTIONS, SECTION_BY_ROLE, decode_body, encode_graph
from asset_document.profiles import blueprint_dispatchers as dispatchers
from asset_document.profiles import blueprint_renames as renames
from asset_document.profiles import ublueprint_members as members
from asset_document.profiles.ublueprint_graph_compare import section_mismatch_reasons
from asset_document.profiles.ublueprint_runtime import _Members as _BlueprintMembers

BASE = "/Body/Director"
GRAPH_SECTIONS = tuple(section for section, _ in SECTIONS)
MEMBER_KEYS = ("ImplementedInterfaces", "Variables", "ClassDefaults")
KEYS = MEMBER_KEYS + (dispatchers.KEY,) + GRAPH_SECTIONS
# Blueprint profile keys a Director does not have.
_NOT_DIRECTOR = {
    "ParentClass": "A Director Blueprint's parent class is always /Script/LevelSequence.LevelSequenceDirector",
    "Components": "A Director Blueprint has no components",
    "Timelines": "The Blueprint editor offers Timelines only in actor-based Blueprints",
    "BlueprintType": "A Director Blueprint is a Normal Blueprint",
    dispatchers.ALIAS: f"Use {dispatchers.KEY}",
}
MEMBER_PRIMITIVES = tuple(f"blueprint.member.{name}.v1" for name in (
    "query", "interface.add", "interface.remove", "variable.set", "variable.remove", "property.set",
    *dispatchers.PRIMITIVES, *renames.PRIMITIVES))


def requirements(route: str) -> tuple[PrimitiveRequirement, ...]:
    return graph_requirements(route) + tuple(PrimitiveRequirement(item, 1, route) for item in MEMBER_PRIMITIVES)


def default_class(target: str) -> str:
    """The generated class of the Director a sequence at ``target`` (package path) gets
    (ULevelSequence::GetDirectorBlueprintName)."""
    name = target.rsplit("/", 1)[-1]
    return f"{target}.{name}_DirectorBP_C"


def class_of(blueprint_path: str) -> str:
    """/Game/Cine/LS.LS:LS_DirectorBP -> /Game/Cine/LS.LS_DirectorBP_C (the class is in the package)."""
    package = blueprint_path.split(".", 1)[0]
    name = blueprint_path.replace(":", ".").rsplit(".", 1)[-1]
    return f"{package}.{name}_C"


# ---------------------------------------------------------------------------
# Paths: the Blueprint components report /Body/<key>; the Director region is /Body/Director/<key>
# ---------------------------------------------------------------------------
def rebase(path: str) -> str:
    if path == "/Body":
        return BASE
    if path.startswith("/Body/") and not path.startswith(BASE + "/") and path != BASE:
        return BASE + path[len("/Body"):]
    return path


def _rebased(diagnostic: RuntimeDiagnostic) -> RuntimeDiagnostic:
    return dataclasses.replace(diagnostic, path=rebase(diagnostic.path))


class _Rebase:
    """Re-raises a contract error of the Blueprint components at its /Body/Director path."""

    def __enter__(self):
        return self

    def __exit__(self, kind, error, trace):
        if isinstance(error, AssetDocumentContractError):
            raise AssetDocumentContractError(_rebased(error.diagnostic)) from None
        return False


def _ops(ops: list) -> list:
    return [(primitive, values, rebase(path)) for primitive, values, path in ops]


# ---------------------------------------------------------------------------
# Canonical form
# ---------------------------------------------------------------------------
def canonical(value: object, self_class: str) -> dict | None:
    """The canonical Director region (None: no Director)."""
    if value is None:
        return None
    if type(value) is not dict:
        raise contract_error(BASE, "Sequencer.InvalidBody", "Director is an object of Blueprint regions (or null: no "
                             "Director Blueprint).")
    for key in value:
        if key in _NOT_DIRECTOR:
            raise contract_error(f"{BASE}/{key}", "Sequencer.InvalidBody", f"{_NOT_DIRECTOR[key]}; leave {key} out.")
        if key not in KEYS:
            raise contract_error(f"{BASE}/{key}", "Sequencer.InvalidBody",
                                 f"Unknown Director key '{key}'. Keys: {', '.join(KEYS)}.")
    with _Rebase():
        out = members.canonical_members({key: value[key] for key in MEMBER_KEYS if key in value})
        rows = dispatchers.canonical(value)
        if rows is not None:
            out[dispatchers.KEY] = rows
        dispatchers.check_names(out)
        for section in GRAPH_SECTIONS:
            if value.get(section) is not None:
                decode_body({section: value[section]}, self_class=self_class)
                out[section] = value[section]
    return out


# ---------------------------------------------------------------------------
# Transport
# ---------------------------------------------------------------------------
_Members = _BlueprintMembers  # (context, read, mutable, requirements)


def _graph_executor(context, read, mutable, route, self_class):
    read_graph = context.handoff(source_handle=read, target_provider_id="graph", access="read_only")
    write_graph = (context.handoff(source_handle=mutable, target_provider_id="graph", access="mutable")
                   if mutable is not None else None)
    return GraphExecutor(context, read_handle=read_graph, write_handle=write_graph, route_id=route,
                         adapter=K2_ADAPTER, self_class=self_class)


# ---------------------------------------------------------------------------
# Extract
# ---------------------------------------------------------------------------
def extract(context, read, route, by_id, self_class) -> dict:
    """The Director region of a sequence that has a Director Blueprint."""
    query = _Members(context, read, None, by_id).query()
    member_body = members.members_from_query(query)
    body = {key: member_body[key] for key in MEMBER_KEYS}
    body[dispatchers.KEY] = dispatchers.from_query(query)
    executor = _graph_executor(context, read, None, route, self_class)
    live = executor.read_live()
    graphs = extract_graphs(live, sections={role: section for section, role in SECTIONS}, adapter=K2_ADAPTER,
                            self_class=self_class)
    for section in GRAPH_SECTIONS:
        body[section] = [encode_graph(graph) for graph in graphs if graph.id.startswith(section + "/")]
    return body


def pending_relayout(context, read, route, self_class, authored) -> dict:
    """The relayout the authored Director graphs' "Layout": "Auto" (subgraphs included) would make, as the Blueprint
    profile's Diff reports it: /Body/Director/<section>/<n> -> the moves (graphs.reconcile.pending_relayout)."""
    if type(authored) is not dict:
        return {}
    try:
        desired, roles = decode_body({key: authored[key] for key in GRAPH_SECTIONS if key in authored},
                                     self_class=self_class)
    except AssetDocumentContractError:
        return {}  # the Director's other differences (or its apply) report the problem
    if not any(graph.layout == "Auto" for doc in desired for graph in doc.walk()):
        return {}
    live = _graph_executor(context, read, None, route, self_class).read_live(include_sizes=True)
    moved = _pending_relayout(desired, live, managed_roles=roles, adapter=K2_ADAPTER, self_class=self_class)
    return {rebase(path): moves for path, moves in moved.items()}


# ---------------------------------------------------------------------------
# Apply (the Director exists)
# ---------------------------------------------------------------------------
def apply(context, read, mutable, route, by_id, self_class, body: dict) -> tuple[list, list]:
    """Members, then graphs, then class defaults, as the Blueprint profile writes them; (changed, diagnostics)."""
    changed, diagnostics = [], []
    with _Rebase():
        blueprint = _Members(context, read, mutable, by_id)
        query = blueprint.query()
        blueprint.run(_ops(renames.plan_renames(body, query) + dispatchers.plan_removes(body, query)
                           + members.plan_interfaces(body, query) + members.plan_variables(body, query)
                           + dispatchers.plan_sets(body, query)))
        changed.extend(blueprint.changed)
        graphs, roles = decode_body({key: body[key] for key in GRAPH_SECTIONS if key in body}, self_class=self_class)
        if roles:
            executor = _graph_executor(context, read, mutable, route, self_class)
            role_paths = {role: f"{BASE}/{SECTION_BY_ROLE[role]}" for role in roles}
            report = executor.run(graphs, managed_roles=roles, role_paths=role_paths)
            changed.extend(report.changed_set)
            diagnostics.extend(_rebased(item) for item in report.diagnostics)
        if "ClassDefaults" in body:
            values = _Members(context, read, mutable, by_id)
            values.run(_ops(members.plan_values({"ClassDefaults": body["ClassDefaults"]}, values.query())))
            changed.extend(values.changed)
    return changed, diagnostics


def compile_if_needed(context, mutable, route, by_id, self_class) -> tuple[list, list]:
    """Compile the Director when it is not up to date (event endpoints and director bindings call the compiled
    class); compiler errors fail the request (Core rolls back)."""
    with _Rebase():
        if _Members(context, mutable, None, by_id).query().get("status") in ("up_to_date", "up_to_date_with_warnings"):
            return [], []
        executor = _graph_executor(context, mutable, mutable, route, self_class)
        executor._op_graph_finalize(PlanOp("graph.finalize", "", BASE, {}))
        return list(executor.changed), [_rebased(item) for item in executor.diagnostics]


# ---------------------------------------------------------------------------
# Diff
# ---------------------------------------------------------------------------
def diff(authored, live, self_class: str, strict: bool = False) -> list:
    """Differences of the authored Director region (canonical; None = no Director) from the extracted one."""
    if authored is None or live is None:
        if authored is None and live is None:
            return []
        kind = DiffKind.ADD if live is None else DiffKind.REMOVE if authored is None else DiffKind.REPLACE
        return [Difference(BASE, kind, authored is not None, authored, live is not None, live)]
    out = [dataclasses.replace(item, path=rebase(item.path)) for item in
           members.diff_members({key: authored[key] for key in MEMBER_KEYS if key in authored},
                                {key: live.get(key) for key in MEMBER_KEYS}, {}, {}, strict)]
    out += [dataclasses.replace(item, path=rebase(item.path)) for item in dispatchers.diff(authored, live)]
    for section in GRAPH_SECTIONS:
        if section not in authored:
            continue
        reasons = section_mismatch_reasons(section, authored[section], live.get(section, []), self_class=self_class,
                                           strict=strict)
        if reasons:
            out.append(Difference(f"{BASE}/{section}", DiffKind.REPLACE, True, {"Differs": reasons}, True,
                                  {"Graphs": [graph["Name"] for graph in live.get(section, [])]}))
    return out


__all__ = ("KEYS", "apply", "canonical", "class_of", "compile_if_needed", "default_class", "diff", "extract",
           "pending_relayout", "requirements")
