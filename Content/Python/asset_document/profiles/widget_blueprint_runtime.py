"""Registered Widget Blueprint Profile (profile.widget_blueprint.v1): read, create and update UWidgetBlueprints.

Composition:

* Blueprint members (ParentClass, ImplementedInterfaces, Variables, ClassDefaults, Class Settings in
  ``Properties``) reuse :mod:`ublueprint_members`, EventDispatchers :mod:`blueprint_dispatchers`, over
  the ``blueprint.member.*`` primitives.
* Graphs (UbergraphPages, FunctionGraphs, MacroGraphs) reuse the K2 graph executor.
* The Designer (WidgetTree, Bindings) is :mod:`widget_blueprint_tree` over ``umg.tree.*`` /
  ``umg.widget.*`` / ``umg.binding.*``; DesiredFocus, PaletteCategory and DesignSize (the Designer
  toolbar size) use ``umg.desired_focus.set`` / ``umg.palette_category.set`` / ``umg.design_size.set``;
  Animations are :mod:`widget_blueprint_animation` over ``umg.anim.*`` (provider "umg").

Writes are covered by Core's package snapshot (existing target) or the factory rollback (created
target); preflight therefore returns ``SNAPSHOT_COVERED``.  Apply order: header and members ->
widget tree -> graphs -> ClassDefaults -> focus / palette / design size / bindings -> animations;
repair compiles once.
See docs/architecture/asset-document-widget-blueprint-matrix.md for the support matrix.
"""
from __future__ import annotations

from asset_document.canonical.document_codec import DocumentCodec
from asset_document.diagnostics import (AssetDocumentContractError, DiagnosticSeverity, RuntimeDiagnostic, contract_error,
                                        contract_errors, primitive_error, with_diagnostics)
from asset_document.diff.model import Difference, DiffKind
from asset_document.dto import PrimitiveRequirement
from asset_document.graphs.executor import LINK_CHECK, graph_requirements
from asset_document.graphs.extract import extract_graphs
from asset_document.graphs.schemas.k2 import K2_ADAPTER, SECTIONS, SECTION_BY_ROLE, decode_body, encode_graph
from asset_document.meta import (
    ComparisonPolicy, DefaultPolicy, DeletePolicy, DocumentAction, IdentityPolicy, OmissionPolicy, OrderPolicy,
    ProfileMeta, RegionMeta, RegionOwnership,
)
from asset_document.mutation_intent_projection import SNAPSHOT_COVERED
from asset_document.profile_runtime import RuntimeProfileRegistration, RuntimeTargetLifecycleSpec, RuntimeTargetPolicy
from asset_document.profiles import blueprint_dispatchers as dispatchers
from asset_document.profiles import blueprint_renames as renames
from asset_document.profiles import ublueprint_members as members
from asset_document.profiles import widget_blueprint_animation as animation
from asset_document.profiles import widget_blueprint_tree as tree
from asset_document.profiles.ublueprint_graph_compare import section_mismatches
from asset_document.profiles.ublueprint_runtime import (
    CLASS_RESOLVE, LIFECYCLE, MEMBER_PRIMITIVES, PENDING_RELAYOUT, RESOLVE, ROUTE, _graph_executor, _require_target,
    _snapshot, authored_k2_graphs, query_member_pages, read_live_with_relayout, relayout_differences,
    check_picks, pick_check_items, trial_parent_context, validate_links,
    repair as _repair_blueprint, self_class,
)
from asset_document.regions.primitive_catalog import (
    NamedOpaqueHandleArgument, PrimitiveOutcome, PrimitiveOutcomeStatus, _single_handle,
)

WIDGET_BLUEPRINT_CLASS = "/Script/UMGEditor.WidgetBlueprint"
USER_WIDGET_CLASS = "/Script/UMG.UserWidget"
PROFILE_ID = "profile.widget_blueprint.v1"
GRAPH_SECTIONS = tuple(section for section, _ in SECTIONS)
MEMBER_KEYS = ("ParentClass", "ImplementedInterfaces", "Variables", "ClassDefaults")
DESIGNER_KEYS = ("WidgetTree", "DesiredFocus", "PaletteCategory", "DesignSize", "Bindings", "Animations")
DESIGN_SIZE_MODES = ("FillScreen", "Custom", "CustomOnScreen", "Desired", "DesiredOnScreen")
DEFAULT_DESIGN_SIZE = {"Mode": "FillScreen", "Width": 100, "Height": 100}
BODY_KEYS = MEMBER_KEYS + (dispatchers.KEY,) + DESIGNER_KEYS + GRAPH_SECTIONS
LEGACY_KEYS = ("Palette", "EditorOptions", "WidgetVariableGuids", "Components", "Timelines")
UMG_PRIMITIVES = ("umg.tree.query.v1", "umg.tree.add.v1", "umg.tree.move.v1", "umg.tree.remove.v1",
                  "umg.widget.rename.v1", "umg.widget.variable.set.v1", "umg.widget.property.set.v1", "umg.widget.navigation.set.v1",
                  "umg.binding.set.v1", "umg.binding.remove.v1", "umg.desired_focus.set.v1",
                  "umg.palette_category.set.v1", "umg.design_size.set.v1", "umg.anim.query.v1",
                  "umg.anim.set.v1",
                  "umg.anim.remove.v1", "umg.anim.track.set.v1", "umg.anim.track.remove.v1")
REQUIREMENTS = (tuple(PrimitiveRequirement(item, 1, ROUTE) for item in (CLASS_RESOLVE, RESOLVE, *LIFECYCLE))
                + graph_requirements(ROUTE)
                + tuple(PrimitiveRequirement(item, 1, ROUTE) for item in MEMBER_PRIMITIVES + UMG_PRIMITIVES + (LINK_CHECK,)))
BY_ID = {item.primitive_id: item for item in REQUIREMENTS}
PAGE = 200


def _bound_event(key: str) -> bool:
    """A widget's bound event (K2:ComponentBoundEvent:...@Widget): the widget may be one the same document adds,
    so Validate leaves the key to Apply."""
    return key.startswith("K2:ComponentBoundEvent:")


def _error(path, message, code="Widget.InvalidDocument"):
    return contract_error(path, code, message)


def _widget_classes(body: dict) -> dict:
    """The document's widgets by folded name -> class path (WidgetTree, named slots included)."""
    classes = {}

    def walk(node):
        if type(node) is not dict:
            return
        if type(node.get("Name")) is str and type(node.get("Class")) is str:
            classes[node["Name"].casefold()] = node["Class"]
        for child in node.get("Children") or ():
            walk(child)
        for child in (node.get("NamedSlots") or {}).values() if type(node.get("NamedSlots")) is dict else ():
            walk(child)
    widget_tree = body.get("WidgetTree")
    if type(widget_tree) is dict:
        walk(widget_tree.get("RootWidget"))
        slots = widget_tree.get("NamedSlotBindings")
        for child in slots.values() if type(slots) is dict else ():
            walk(child)
    return classes


def _binding_signature(graph: dict) -> dict | None:
    """A function graph's Signature as umg.tree.query.v1 check_bindings reads it; None when the graph states none (the
    Blueprint's function is read) or a type does not read (Apply reports it)."""
    signature = graph.get("Signature")
    if type(signature) is not dict:
        return None
    out = {"inputs": [], "outputs": [], "pure": bool(signature.get("Pure") or signature.get("Const"))}
    for key, label in (("inputs", "Inputs"), ("outputs", "Outputs")):
        for pin in signature.get(label) or ():
            if type(pin) is not dict or "Type" not in pin:
                return None
            try:
                out[key].append(members.type_dto(pin["Type"], "/Type"))
            except AssetDocumentContractError:
                return None
    return out


def binding_check_items(document: dict) -> list:
    """The Function bindings Validate checks as Apply's umg.binding.set.v1 does: the widget's
    class (the document's, else the live widget's), the property, and the function's Signature from the document's
    FunctionGraphs (else the Blueprint's function)."""
    body = document.get("Body") or {}
    bindings = body.get("Bindings")
    if type(bindings) is not list:
        return []
    classes = _widget_classes(body)
    graphs = {row["Name"].casefold(): row for row in body.get("FunctionGraphs") or ()
              if type(row) is dict and type(row.get("Name")) is str}
    items = []
    for index, row in enumerate(bindings):
        if type(row) is not dict or row.get("Kind") != "Function" or not all(
                type(row.get(key)) is str for key in ("Widget", "Property", "Function")):
            continue
        item = {"at": f"/Body/Bindings/{index}/Function", "widget": row["Widget"], "property": row["Property"],
                "function": row["Function"]}
        widget_class = classes.get(row["Widget"].casefold())
        if widget_class is not None:
            item["widget_class"] = widget_class
        graph = graphs.get(row["Function"].casefold())
        signature = _binding_signature(graph) if graph is not None else None
        if signature is not None:
            item["signature"] = signature
        items.append(item)
    return items


def check_bindings(document: dict, read, context) -> None:
    """Validate's binding signature check (Widget.InvalidBindingFunction / BindingTargetNotFound), naming what differs
    and the fix; read on the existing target or the trial Widget Blueprint."""
    items = binding_check_items(document)
    if not items:
        return
    try:
        found = _Calls(context, "umg", read).query("umg.tree.query.v1", {"check_bindings": items}).get("check_bindings")
    except AssetDocumentContractError:
        return  # an early check only: Apply binds each function and reports it
    problems = [contract_error(item["at"], item["code"], item["message"]).diagnostic for item in found or ()
                if type(item) is dict and all(type(item.get(key)) is str for key in ("at", "code", "message"))]
    if problems:
        raise contract_errors(problems)


# ---------------------------------------------------------------------------
# Primitive transport
# ---------------------------------------------------------------------------
def _invoke(context, primitive, values, handles=(), path="/Body"):
    outcome = context.invoke(requirement=BY_ID[primitive], value_arguments=values, handle_arguments=handles)
    if type(outcome) is not PrimitiveOutcome:
        raise _error("/PrimitiveOutcome", "A typed primitive outcome is required.", "Widget.InvalidPrimitiveResult")
    if outcome.status is not PrimitiveOutcomeStatus.SUCCEEDED:
        if outcome.diagnostics:
            source = outcome.diagnostics[0]
            # Report the Body location the AI wrote; the primitive detail goes to the diagnostic context.
            raise primitive_error(path, source, primitive)
        raise _error(path, f"{primitive} did not succeed.", "Widget.PrimitiveFailed")
    return outcome


class _Calls:
    """Calls to one provider ("blueprint" or "umg") against one Widget Blueprint."""

    def __init__(self, context, provider, read, mutable=None):
        self.context = context
        self.read = context.handoff(source_handle=read, target_provider_id=provider, access="read_only")
        self.write = (context.handoff(source_handle=mutable, target_provider_id=provider, access="mutable")
                      if mutable is not None else None)
        self.changed = []

    def query(self, primitive, values=None, path="/Body") -> dict:
        outcome = _invoke(self.context, primitive, {"schema_version": 1, **(values or {})},
                          (NamedOpaqueHandleArgument("asset", self.read),), path)
        if type(outcome.value) is not dict:
            raise _error("/PrimitiveOutcome", f"{primitive} returned no object.", "Widget.InvalidPrimitiveResult")
        return outcome.value

    def members(self) -> dict:
        """blueprint.member.query.v1, every page merged."""
        return query_member_pages(lambda offset: self.query("blueprint.member.query.v1", {"offset": offset}))

    def run(self, ops) -> list:
        """Runs the calls; returns each call's result value."""
        values_out = []
        for primitive, values, path in ops:
            outcome = _invoke(self.context, primitive, {"schema_version": 1, **values},
                              (NamedOpaqueHandleArgument("asset", self.write),), path)
            self.changed.extend(outcome.changed_set)
            values_out.append(outcome.value)
        return values_out


def query_tree(umg: _Calls) -> tuple[list, dict]:
    """All widget rows (paged) and the first page (bindings, inherited named slots)."""
    first = umg.query("umg.tree.query.v1", {"offset": 0, "limit": PAGE})
    rows = list(first.get("widgets", []))
    offset = first.get("next_offset", -1)
    while offset >= 0:
        page = umg.query("umg.tree.query.v1", {"offset": offset, "limit": PAGE})
        rows += page.get("widgets", [])
        offset = page.get("next_offset", -1)
    return rows, first


def query_defaults(umg: _Calls) -> dict:
    """Widget name -> {"Properties": {...}, "Slot": {...}} of the Designer values at their class default."""
    out, offset = {}, 0
    while offset >= 0:
        page = umg.query("umg.tree.query.v1", {"offset": offset, "limit": PAGE, "defaults": True})
        for row in page.get("widgets", []):
            out[row["name"]] = {"Properties": row.get("property_defaults", {}), "Slot": row.get("slot_defaults", {})}
        offset = page.get("next_offset", -1)
    return out


def refine_default_differences(differences, authored, extracted, read, context):
    body, live = authored.get("Body", {}), extracted.get("Body", {})
    candidates = [item for item in differences if item.kind is DiffKind.REMOVE and not item.after_present
                  and item.path.startswith("/Body/WidgetTree/") and item.path.rsplit("/", 2)[-2] in ("Properties", "Slot")]
    if not candidates or "WidgetTree" not in body or "WidgetTree" not in live:
        return differences
    names = {pointer: name for name, pointer in
             tree._pointers(tree.effective_tree(body["WidgetTree"], live["WidgetTree"])).items()}
    defaults = query_defaults(_Calls(context, "umg", read))
    out, wanted = [], {id(item) for item in candidates}
    for item in differences:
        if id(item) in wanted:
            widget_pointer, section, key = item.path.rsplit("/", 2)
            key = key.replace("~1", "/").replace("~0", "~")
            values = defaults.get(names.get(widget_pointer), {}).get(section, {})
            if key in values:
                if members.values_equal(item.before, values[key]):
                    continue
                item = Difference(item.path, DiffKind.REPLACE, True, item.before, True, values[key])
        out.append(item)
    return out


def query_animation(umg: _Calls, name: str) -> dict:
    detail = umg.query("umg.anim.query.v1", {"name": name})
    tracks = list(detail.get("tracks", []))
    offset = detail.get("next_track_offset", -1)
    while offset >= 0:
        page = umg.query("umg.anim.query.v1", {"name": name, "track_offset": offset})
        tracks += page.get("tracks", [])
        offset = page.get("next_track_offset", -1)
    return {**detail, "tracks": tracks}


def live_animation(umg: _Calls, name: str) -> dict:
    return animation.canonical_animations([animation.animation_from_live(query_animation(umg, name))])[0]


# ---------------------------------------------------------------------------
# Document shape
# ---------------------------------------------------------------------------
def _validate_header(document) -> None:
    if type(document) is not dict:
        raise _error("/", "A Widget Blueprint document is a JSON object.")
    if type(document.get("SchemaVersion")) is not int or document["SchemaVersion"] != 1:
        raise _error("/SchemaVersion", "SchemaVersion must be 1.", "Meta.InvalidSchemaVersion")
    _require_target(document.get("Target"))
    if document.get("Class") != WIDGET_BLUEPRINT_CLASS:
        raise _error("/Class", f"Class must be {WIDGET_BLUEPRINT_CLASS}.", "Widget.ClassMismatch")
    if document.get("Action") not in {item.value for item in DocumentAction}:
        raise _error("/Action", "Action is Create, Update or CreateOrUpdate.", "Widget.InvalidAction")
    for key in ("Definitions", "Properties"):
        if type(document.get(key, {})) is not dict:
            raise _error(f"/{key}", f"{key} must be an object.")
    if document.get("Definitions"):
        raise _error("/Definitions", "Widget Blueprint documents do not use Definitions.", "Widget.UnsupportedRegion")
    body = document.get("Body", {})
    if type(body) is not dict:
        raise _error("/Body", "Body must be an object.")
    for key in body:
        if key not in BODY_KEYS and key not in LEGACY_KEYS:
            raise _error(f"/Body/{key}", f"Unknown Body key '{key}'. Keys: {', '.join(BODY_KEYS)}.",
                         "Widget.UnknownBodyKey")
    for key, reason in (("Components", "Widget Blueprints have no components; widgets go in WidgetTree"),
                        ("Timelines", "the Blueprint editor offers Timelines only in actor-based Blueprints "
                                      "(FBlueprintEditorUtils::DoesSupportTimelines)")):
        if body.get(key) not in (None, []):
            raise _error(f"/Body/{key}", f"{key} are not supported: {reason}. Give [] or omit the key.",
                         "Widget.UnsupportedRegion")


def _legacy(document: dict) -> tuple[dict, dict]:
    """Legacy C++ Profile spellings -> canonical Body and Properties."""
    body = dict(document.get("Body", {}))
    properties = dict(document.get("Properties", {}))
    palette = body.pop("Palette", None)
    if palette is not None:
        if type(palette) is not dict or set(palette) - {"Category"}:
            raise _error("/Body/Palette", "Palette is {\"Category\": \"...\"} (ClassDefaults.PaletteCategory).")
        if "Category" in palette:
            body.setdefault("PaletteCategory", palette["Category"])
    options = body.pop("EditorOptions", None)
    if options is not None:
        if type(options) is not dict:
            raise _error("/Body/EditorOptions", "EditorOptions is an object of Widget Blueprint Class Settings.")
        for key, value in options.items():
            properties.setdefault(key, value)
    defaults = body.get("ClassDefaults")
    if type(defaults) is dict and ("DesiredFocusWidget" in defaults or "PaletteCategory" in defaults):
        # Edited through dedicated Details rows, not the generic Class Defaults grid.
        defaults = dict(defaults)
        if "DesiredFocusWidget" in defaults:
            focus = defaults.pop("DesiredFocusWidget")
            body.setdefault("DesiredFocus", focus.get("WidgetName", "") if type(focus) is dict else focus)
        if "PaletteCategory" in defaults:
            body.setdefault("PaletteCategory", defaults.pop("PaletteCategory"))
        body["ClassDefaults"] = defaults
    # Editor-managed (FWidgetBlueprintEditorUtils keeps one GUID per widget variable).
    body.pop("WidgetVariableGuids", None)
    body.pop("Components", None)
    body.pop("Timelines", None)
    return body, properties


def _parent_required(path):
    return _error(path, "Widget Blueprint creation requires Body.ParentClass (UserWidget or a subclass).",
                  "Widget.ParentClassRequired")


def canonicalize(document) -> dict:
    _validate_header(document)
    DocumentCodec().validate(document)
    body, properties = _legacy(document)
    canonical = {key: value for key, value in document.items() if key not in ("Body", "Properties")}
    canonical.setdefault("Definitions", {})
    canonical["Properties"] = properties
    out = members.canonical_members(body)
    if document.get("Action") == DocumentAction.CREATE.value and "ParentClass" not in out:
        raise _parent_required("/Body/ParentClass" if "Body" in document else "/Body")
    rows = dispatchers.canonical(body)
    if rows is not None:
        out[dispatchers.KEY] = rows
        dispatchers.check_names(out)
    if "WidgetTree" in body:
        out["WidgetTree"], _ = tree.canonical_tree(body["WidgetTree"])
    if "DesiredFocus" in body:
        focus = body["DesiredFocus"]
        if focus is None or focus == "None":
            focus = ""
        if type(focus) is not str:
            raise _error("/Body/DesiredFocus", "DesiredFocus is the name of a widget in WidgetTree (\"\" for none).")
        out["DesiredFocus"] = focus
    if "PaletteCategory" in body:
        category = body["PaletteCategory"]
        if type(category) is not str:
            raise _error("/Body/PaletteCategory", "PaletteCategory is the palette section name (\"\" for the default).")
        out["PaletteCategory"] = category.strip()
    if "DesignSize" in body:
        out["DesignSize"] = _canonical_design_size(body["DesignSize"])
    if "Bindings" in body:
        out["Bindings"] = tree.canonical_bindings(body["Bindings"])
    if "Animations" in body:
        out["Animations"] = animation.canonical_animations(body["Animations"])
    target = document["Target"]
    for section in GRAPH_SECTIONS:
        if section in body and body[section] is not None:
            decode_body({section: body[section]}, self_class=self_class(target))
            out[section] = body[section]
    canonical["Body"] = out
    return _snapshot(canonical)


# ---------------------------------------------------------------------------
# Apply
# ---------------------------------------------------------------------------
def _dependents_warnings(renames, results) -> list:
    """Widget.DependentsUpdated: the other loaded Blueprints whose graphs a rename updated (left unsaved, as in the
    editor)."""
    out = []
    for (_, values, path), result in zip(renames, results):
        packages = sorted({row["package"] for row in (result or {}).get("dependents", [])
                           if type(row) is dict and type(row.get("package")) is str})
        if packages:
            out.append(RuntimeDiagnostic(
                path=path, code="Widget.DependentsUpdated", severity=DiagnosticSeverity.WARNING,
                message=f"Renaming widget '{values['name']}' to '{values['new_name']}' also renamed its references in "
                        f"the loaded Blueprints that use it, which are now unsaved as in the editor (save them to keep "
                        f"the update): {', '.join(packages)}."))
    return out


def _apply_tree(body, umg: _Calls) -> list:
    rows, _ = query_tree(umg)
    _, paths = tree.canonical_tree(body["WidgetTree"])
    renames = tree.plan_renames(body["WidgetTree"], rows, paths)
    diagnostics = []
    if renames:
        diagnostics = _dependents_warnings(renames, umg.run(renames))
        rows, _ = query_tree(umg)
    desired = tree.effective_tree(body["WidgetTree"], tree.tree_from_rows(rows))
    umg.run(tree.plan_structure(desired, rows, paths))
    rows, _ = query_tree(umg)
    umg.run(tree.plan_values(desired, rows, paths))
    return diagnostics


def _apply_animations(body, umg: _Calls) -> None:
    listed = {row["name"] for row in umg.query("umg.anim.query.v1").get("animations", [])}
    wanted = {row["Name"] for row in body["Animations"]}
    umg.run([("umg.anim.remove.v1", {"name": name}, "/Body/Animations") for name in sorted(listed - wanted)])
    for index, desired in enumerate(body["Animations"]):
        path = f"/Body/Animations/{index}"
        live = live_animation(umg, desired["Name"]) if desired["Name"] in listed else None
        header = animation.header_values(desired, live)
        if header is not None:
            umg.run([("umg.anim.set.v1", header, path)])
            # A new display rate moves every key's display frame: plan tracks from a fresh read.
            live = live_animation(umg, desired["Name"])
        umg.run(animation.plan_tracks(desired, live, path))


def apply(document, read, mutable, context) -> tuple:
    body = document["Body"]
    target = document["Target"]
    blueprint = _Calls(context, "blueprint", read, mutable)
    blueprint.run(members.plan_header(document.get("Properties", {}), body,
                                      blueprint.members()))
    query = blueprint.members()
    blueprint.run(renames.plan_renames(body, query) + dispatchers.plan_removes(body, query) + members.plan_interfaces(body, query)
                  + members.plan_variables(body, query) + dispatchers.plan_sets(body, query))
    umg = _Calls(context, "umg", read, mutable)
    diagnostics = _apply_tree(body, umg) if "WidgetTree" in body else []
    graphs, roles = decode_body(body, self_class=self_class(target))
    changed = list(blueprint.changed) + list(umg.changed)
    if roles:
        executor = _graph_executor(context, read, mutable, target)
        role_paths = {role: f"/Body/{SECTION_BY_ROLE[role]}" for role in roles}
        report = executor.run(graphs, managed_roles=roles, role_paths=role_paths)
        changed.extend(report.changed_set)
        diagnostics.extend(report.diagnostics)
    values = _Calls(context, "blueprint", read, mutable)
    values.run(members.plan_values(body, values.members()))
    changed.extend(values.changed)
    late = _Calls(context, "umg", read, mutable)
    if "Bindings" in body or "DesiredFocus" in body or "PaletteCategory" in body or "DesignSize" in body:
        first = late.query("umg.tree.query.v1", {"offset": 0, "limit": 1})
        if "DesiredFocus" in body and body["DesiredFocus"] != first.get("desired_focus", ""):
            late.run([("umg.desired_focus.set.v1", {"widget": body["DesiredFocus"]}, "/Body/DesiredFocus")])
        if "PaletteCategory" in body and body["PaletteCategory"] != first.get("palette_category", ""):
            late.run([("umg.palette_category.set.v1", {"category": body["PaletteCategory"]}, "/Body/PaletteCategory")])
        if "DesignSize" in body and body["DesignSize"] != _design_size_from_live(first):
            size = body["DesignSize"]
            late.run([("umg.design_size.set.v1", {"mode": size["Mode"], "width": size["Width"], "height": size["Height"]},
                       "/Body/DesignSize")])
        if "Bindings" in body:
            late.run(tree.plan_bindings(body["Bindings"], first.get("bindings", [])))
    if "Animations" in body:
        _apply_animations(body, late)
    changed.extend(late.changed)
    return with_diagnostics(changed, diagnostics)


# ---------------------------------------------------------------------------
# Extract and diff
# ---------------------------------------------------------------------------
def extract(target, read, context, include_defaults: bool = False) -> dict:
    return _extract(target, read, context, include_defaults)[0]


def extract_with_defaults(target, read, context, authored=None):
    """Verify / Diff's read (include_defaults) and the relayout the authored graphs' "Layout": "Auto" would make
    (PENDING_RELAYOUT, for diff's Diff-operation differences)."""
    body = authored.get("Body") if type(authored) is dict else None
    document, pending = _extract(target, read, context, True, body)
    return document, ({PENDING_RELAYOUT: pending} if pending else {})


def _extract(target, read, context, include_defaults: bool = False, authored_body: dict | None = None) -> tuple:
    blueprint = _Calls(context, "blueprint", read)
    query = blueprint.members()
    live = members.members_from_query(query)
    body = {key: live[key] for key in MEMBER_KEYS if key in live}
    body[dispatchers.KEY] = dispatchers.from_query(query)
    umg = _Calls(context, "umg", read)
    rows, first = query_tree(umg)
    body["WidgetTree"] = tree.tree_from_rows(rows)
    body["DesiredFocus"] = first.get("desired_focus", "")
    body["PaletteCategory"] = first.get("palette_category", "")
    body["DesignSize"] = _design_size_from_live(first)
    body["Bindings"] = tree.bindings_from_live(first.get("bindings", []))
    body["Animations"] = [animation.animation_from_live(query_animation(umg, row["name"]))
                          for row in umg.query("umg.anim.query.v1").get("animations", [])]
    executor = _graph_executor(context, read, None, target)
    live, pending = read_live_with_relayout(executor, authored_k2_graphs(authored_body, target), adapter=K2_ADAPTER,
                                            self_class=self_class(target))
    graphs = extract_graphs(live, sections={role: section for section, role in SECTIONS},
                            adapter=K2_ADAPTER, self_class=self_class(target), include_defaults=include_defaults)
    for section in GRAPH_SECTIONS:
        body[section] = [encode_graph(graph) for graph in graphs if graph.id.startswith(section + "/")]
    return {"SchemaVersion": 1, "Target": target, "Class": WIDGET_BLUEPRINT_CLASS,
            "Action": DocumentAction.CREATE_OR_UPDATE.value, "Definitions": {},
            "Properties": dict(query.get("properties", {})), "Body": body}, pending


def _canonical_design_size(value) -> dict:
    """Designer toolbar size: {Mode, Width?, Height?}; null = the UUserWidget default (Fill Screen, 100 x 100)."""
    if value is None:
        return dict(DEFAULT_DESIGN_SIZE)
    if type(value) is not dict or set(value) - {"Mode", "Width", "Height"}:
        raise _error("/Body/DesignSize", "DesignSize is {\"Mode\": \"FillScreen\"|\"Custom\"|\"CustomOnScreen\"|\"Desired\"|"
                     "\"DesiredOnScreen\", \"Width\": px, \"Height\": px}.", "Widget.InvalidValue")
    mode = value.get("Mode", DEFAULT_DESIGN_SIZE["Mode"])
    mode = mode.rsplit("::", 1)[-1] if type(mode) is str else mode
    if mode not in DESIGN_SIZE_MODES:
        raise _error("/Body/DesignSize/Mode", f"Mode is one of {', '.join(DESIGN_SIZE_MODES)}.", "Widget.InvalidValue")
    out = {"Mode": mode}
    for key in ("Width", "Height"):
        size = value.get(key, DEFAULT_DESIGN_SIZE[key])
        if type(size) is float and size.is_integer():
            size = int(size)
        if type(size) is not int or not 1 <= size <= 100000:
            raise _error(f"/Body/DesignSize/{key}", f"{key} is a whole number of pixels (1..100000).", "Widget.InvalidValue")
        out[key] = size
    return out


def _design_size_from_live(first: dict) -> dict:
    live = first.get("design_size")
    if not live:
        return dict(DEFAULT_DESIGN_SIZE)
    return {"Mode": live["mode"], "Width": int(live["width"]), "Height": int(live["height"])}


def diff(authored: dict, extracted: dict, strict: bool = False, defaults: dict | None = None) -> tuple:
    body, live = authored["Body"], extracted["Body"]
    out = members.diff_members(body, live, authored.get("Properties", {}), extracted.get("Properties", {}), strict)
    out += dispatchers.diff(body, live)
    out += tree.diff_designer(body, live, strict)
    for key in ("DesiredFocus", "PaletteCategory"):
        if key in body and body[key] != live.get(key, ""):
            out.append(Difference(f"/Body/{key}", DiffKind.REPLACE, True, body[key], True, live.get(key, "")))
    if "DesignSize" in body and body["DesignSize"] != live.get("DesignSize", DEFAULT_DESIGN_SIZE):
        out.append(Difference("/Body/DesignSize", DiffKind.REPLACE, True, body["DesignSize"], True,
                              live.get("DesignSize", DEFAULT_DESIGN_SIZE)))
    if "Animations" in body:
        out += animation.diff_animations(body["Animations"], live.get("Animations", []))
    target = authored["Target"]
    for section in GRAPH_SECTIONS:
        if section not in body:
            continue
        names = section_mismatches(section, body[section], live.get(section, []), self_class=self_class(target),
                                   strict=strict)
        if names:
            out.append(Difference(f"/Body/{section}", DiffKind.REPLACE, True, {"Graphs": names}, True,
                                  {"Graphs": [graph["Name"] for graph in live.get(section, [])]}))
    if strict:
        out += relayout_differences(defaults)
    return tuple(out)


# ---------------------------------------------------------------------------
# Handler
# ---------------------------------------------------------------------------
class WidgetBlueprintRuntimeProfile:
    profile_id = PROFILE_ID
    exact_asset_class_path = WIDGET_BLUEPRINT_CLASS
    runtime_entry_module = "asset_document/bootstrap.py"

    def required_primitives(self) -> tuple[PrimitiveRequirement, ...]:
        return REQUIREMENTS

    def inspect_profile(self) -> dict:
        return {"Class": WIDGET_BLUEPRINT_CLASS, "BodySections": list(BODY_KEYS)}

    def create_template(self, target: str) -> dict:
        return {"SchemaVersion": 1, "Target": _require_target(target), "Class": WIDGET_BLUEPRINT_CLASS,
                "Action": DocumentAction.CREATE_OR_UPDATE.value, "Definitions": {}, "Properties": {},
                "Body": {"ParentClass": USER_WIDGET_CLASS, "ImplementedInterfaces": [], "Variables": [],
                         "ClassDefaults": {},
                         "WidgetTree": {"RootWidget": {"Name": "RootCanvas", "Class": "/Script/UMG.CanvasPanel",
                                                       "IsVariable": False},
                                        "NamedSlotBindings": {}},
                         "DesiredFocus": "", "PaletteCategory": "",
                         "DesignSize": dict(DEFAULT_DESIGN_SIZE), "Bindings": [], "Animations": [], "UbergraphPages": [], "FunctionGraphs": [],
                         "MacroGraphs": []}}

    def validate(self, document) -> None:
        canonicalize(document)

    def canonicalize(self, document) -> dict:
        return canonicalize(document)

    def create_target_context(self, document, context):
        document = canonicalize(document)
        parent = document["Body"].get("ParentClass")
        if parent is None:
            raise _parent_required("/Body/ParentClass")
        outcome = _invoke(context, CLASS_RESOLVE, {"class": parent, "expected_base": USER_WIDGET_CLASS}, (),
                          "/Body/ParentClass")
        return (NamedOpaqueHandleArgument("parent_class", _single_handle(outcome, "class")),)

    def preflight(self, document, scratch_handle, context):
        document = canonicalize(document)
        decode_body(document["Body"], self_class=self_class(document["Target"]))
        return SNAPSHOT_COVERED

    def apply(self, document, read_asset_handle, mutable_asset_handle, context):
        return apply(canonicalize(document), read_asset_handle, mutable_asset_handle, context)

    def validate_checks(self, document, read_asset_handle, context):
        """Validate's graph check of the K2 graphs (the UBlueprint Profile's, ublueprint_link_check): node keys, pin
        names and links, read on the existing target, or on a transient Widget Blueprint of the parent class for a
        new one (validate_trial_context). Widgets' bound events are left to Apply."""
        document = canonicalize(document)
        found = []
        for check in (lambda: validate_links(document, read_asset_handle, context, skip=_bound_event, requirements=BY_ID),
                      lambda: check_picks(document, read_asset_handle, context, requirements=BY_ID),
                      lambda: check_bindings(document, read_asset_handle, context)):
            try:
                check()
            except AssetDocumentContractError as failure:
                found.extend((failure.diagnostic,) + failure.additional)
        if found:
            raise contract_errors(found)

    def validate_trial_context(self, document, context):
        document = canonicalize(document)
        values = bool(pick_check_items(document) or binding_check_items(document))
        return trial_parent_context(context, document["Body"], values)

    def repair(self, document, asset_handle, context):
        return _repair_blueprint(canonicalize(document), asset_handle, context)

    def extract(self, *, target, asset_handle, context, include_defaults=False):
        return extract(_require_target(target), asset_handle, context, include_defaults)

    def inspect(self, *, mode, exact_class_path, exact_asset_object_path, editor_asset_handle, context):
        if mode != "live_asset" or exact_class_path != WIDGET_BLUEPRINT_CLASS or editor_asset_handle is None:
            raise _error("/Inspect", "Widget Blueprint inspection reads a live asset.", "Widget.InspectNotAdmitted")
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
        """Verify / Diff; the Diff operation (exact) also reports a pending "Layout": "Auto" relayout."""
        return diff(canonicalize(authored), canonicalize(extracted), strict=exact, defaults=live_defaults)

    def refine_diff(self, differences, authored, extracted, read, context):
        """The live tree lists changed Designer values only, so a property the document sets that the
        asset holds at its class default first diffs with no current value: give it the real default
        (Replace), and drop it when the document sets exactly that default."""
        return refine_default_differences(differences, canonicalize(authored), canonicalize(extracted), read, context)


def create_widget_blueprint_runtime_handler():
    return WidgetBlueprintRuntimeProfile()


_REGIONS = tuple(
    RegionMeta(f"widget_blueprint.{name}.v1", f"/Body/{key}", RegionOwnership.MANAGED, identity, OrderPolicy.CANONICAL,
               DefaultPolicy.EXPLICIT, OmissionPolicy.PRESERVE, DeletePolicy.EXPLICIT_ONLY, ComparisonPolicy.CANONICAL)
    for name, key, identity in (
        ("parent_class", "ParentClass", IdentityPolicy.POSITIONAL),
        ("implemented_interfaces", "ImplementedInterfaces", IdentityPolicy.STABLE_KEY),
        ("variables", "Variables", IdentityPolicy.STABLE_KEY),
        ("class_defaults", "ClassDefaults", IdentityPolicy.STABLE_KEY),
        ("event_dispatchers", "EventDispatchers", IdentityPolicy.STABLE_KEY),
        ("widget_tree", "WidgetTree", IdentityPolicy.STABLE_KEY),
        ("desired_focus", "DesiredFocus", IdentityPolicy.POSITIONAL),
        ("palette_category", "PaletteCategory", IdentityPolicy.POSITIONAL),
        ("design_size", "DesignSize", IdentityPolicy.POSITIONAL),
        ("bindings", "Bindings", IdentityPolicy.STABLE_KEY),
        ("animations", "Animations", IdentityPolicy.STABLE_KEY),
        ("ubergraph_pages", "UbergraphPages", IdentityPolicy.STABLE_KEY),
        ("function_graphs", "FunctionGraphs", IdentityPolicy.STABLE_KEY),
        ("macro_graphs", "MacroGraphs", IdentityPolicy.STABLE_KEY),
    ))
WIDGET_BLUEPRINT_PROFILE_META = ProfileMeta(PROFILE_ID, ROUTE, WIDGET_BLUEPRINT_CLASS, _REGIONS, REQUIREMENTS)
WIDGET_BLUEPRINT_RUNTIME_REGISTRATION = RuntimeProfileRegistration(
    WIDGET_BLUEPRINT_PROFILE_META, create_widget_blueprint_runtime_handler, RuntimeTargetPolicy.CREATE_OR_UPDATE,
    RuntimeTargetLifecycleSpec("widget_blueprint.v1", "widget_blueprint.v1", "blueprint_factory.v1",
                               *(BY_ID[item] for item in LIFECYCLE), ("parent_class",)))
