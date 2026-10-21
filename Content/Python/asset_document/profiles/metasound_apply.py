"""Apply of the MetaSound Profiles (phases M1-M3: Sources, Patches and presets, graph pages).

The document is written the way the MetaSound editor writes it, through ``audio.metasound.edit.v1`` (one transaction
per call, the asset's own document builder) and ``audio.metasound.compile.v1`` (registration as the editor does after
its edits, then validation).  The apply is planned in phases, each against a fresh read of the asset:

1. the removal of pages no longer listed, the editor's open-time input template injection (when the read simulated
   one) and ``Interfaces``;
2. ``Metadata``, members (``Inputs`` / ``Outputs``: add, rename by ``RenamedFrom``, fields, input widgets, page
   defaults) and each page's ``Variables`` (a type change is remove + add, as the editor does; their nodes come back
   in phase 4);
3. kept nodes' classes: the editor's Update Node (``Version``) and node Details configuration (``Configuration``),
   which change their pins;
4. per page: nodes (deletes, creates, settings, connections), comments; then the removal of members no longer listed;
5. the pages the document adds (the Pages Details "+": a copy of the MetaSound's last page), then phases 2-4 again for
   them.

Graph ops name their page (``"page"``; Default when absent).  Pages must be pages of the project's
``UMetaSoundSettings`` (``MetaSound.UnknownPage``); ``Default`` is never removed.

A preset (``Preset: {Parent}``) is made by the factory with its parent (``create_target_context``); its apply writes
``Metadata`` and the input overrides (a listed input overrides the parent's default, an unlisted one inherits it).
``Preset: null`` on a preset is the editor's Convert From Preset.  ``Preset`` on an existing non-preset is refused:
the editor cannot convert a MetaSound into a preset.

``Properties`` (Details panel values, ``OutputFormat`` included) are written first, as the editor's Details edits
are separate from graph edits (a new OutputFormat changes the output members the graph then connects).

An apply whose document equals the live one (``diff``) writes nothing and does not register.  Otherwise it ends with
one registration; new validation errors fail it (``MetaSound.ValidationFailed``, rolled back), errors the asset
already had and warnings come back as warnings.  The compile's operator build check fails it the same way
(``MetaSound.BuildFailed``) when the registered graph does not build into an operator.
"""
from __future__ import annotations

from asset_document.diagnostics import DiagnosticSeverity, RuntimeDiagnostic, with_diagnostics
from asset_document.profiles import data_table_common as common
from asset_document.profiles import metasound as ms
from asset_document.regions.primitive_catalog import NamedOpaqueHandleArgument

DEFAULT = ms.DEFAULT_PAGE
# New nodes without Position: columns by graph depth, as Material places new expressions.
COLUMN = 320
ROW = 160
_METADATA_FIELDS = {"DisplayName": "display_name", "Description": "description", "Author": "author",
                    "Keywords": "keywords", "Categories": "categories", "Referenceable": "referenceable",
                    "Deprecated": "deprecated"}
_MEMBER_FIELDS = {"DisplayName": "display_name", "Description": "description", "SortOrder": "sort_order",
                  "Advanced": "advanced"}
# Op argument -> document field, for errors the edit primitive reports at an op argument.
_OP_FIELDS = {"type": "Type", "default": "Default", "constructor": "Constructor", "display_name": "DisplayName",
              "description": "Description", "sort_order": "SortOrder", "advanced": "Advanced", "class": "Class",
              "asset": "Class", "major": "Version", "member": "Member", "position": "Position", "text": "Text",
              "size": "Size", "color": "Color", "font_size": "FontSize", "color_bubble": "ColorBubble",
              "move_mode": "MoveMode", "comment": "Comment", "comment_visible": "CommentVisible",
              "hide_unconnected": "HideUnconnectedPins", "keywords": "Keywords", "categories": "Categories",
              "author": "Author", "referenceable": "Referenceable", "deprecated": "Deprecated",
              "rename_to": "RenamedFrom", "configuration": "Configuration", "inherits": "Default",
              "page_defaults": "PageDefaults", "name": "Name"}
_ABSENT = object()


def error(path, message, code):
    return common.error(path, message, code)


def graph_path(page, region=None):
    path = f"/Body/Graphs/{common.escape(page)}"
    return f"{path}/{region}" if region else path


def on_page(op, page):
    """A graph op on ``page`` (ops without "page" edit the Default page)."""
    if page != DEFAULT:
        op["page"] = page
    return op


class Live:
    """One read of the asset (with pin facts), assembled as a document."""

    def __init__(self, calls):
        self.records = ms.read_records(calls, pins=True)
        self.header = self.records[0]
        self.body, self.facts, self.keys_by_page = ms.assemble(self.records, pins=True)
        self.graphs = self.body.get("Graphs", {})
        self.keys = self.keys_by_page.get(DEFAULT, {})
        self.nodes = self.page_nodes(DEFAULT)
        self.comments = self.page_comments(DEFAULT)
        self.inputs = {row["name"]: row for row in self.records if row["kind"] == "input"}
        self.outputs = {row["name"]: row for row in self.records if row["kind"] == "output"}
        self.variables_by_page = {}
        for row in self.records:
            if row["kind"] == "variable":
                self.variables_by_page.setdefault(row["page"], {})[row["name"]] = row
        self.variables = self.page_variables(DEFAULT)
        self.pages = [item["name"] for item in self.header.get("pages", [])]
        # Pages a document may use (Project Settings > MetaSound > Pages).
        self.configured = list(self.header.get("configured_pages", [DEFAULT]))

    def page_nodes(self, page):
        return self.graphs.get(page, {}).get("Nodes", {})

    def page_comments(self, page):
        return self.graphs.get(page, {}).get("Comments", {})

    def page_variables(self, page):
        return self.variables_by_page.get(page, {})

    def changes(self, canonical):
        return ms.body_changes(canonical, self.body, self.facts)


# ---------------------------------------------------------------------------
# Transport
# ---------------------------------------------------------------------------
def op_path(primitive_path, op_paths):
    """/ops/<n>/<field>/... -> the document path of op n."""
    parts = [part for part in (primitive_path or "").split("/") if part]
    if len(parts) >= 2 and parts[0] == "ops" and parts[1].isdigit() and int(parts[1]) < len(op_paths):
        base = op_paths[int(parts[1])]
        rest = parts[2:]
        if not rest or ("/Nodes/" in base and "/Inputs/" in base):
            return base
        if rest[0] in ("defaults", "values", "page_defaults") and len(rest) > 1:
            region = {"defaults": "Defaults", "values": "Widget", "page_defaults": "PageDefaults"}[rest[0]]
            return f"{base if base.endswith('/' + region) else base + '/' + region}/{rest[1]}"
        if rest[0] == "configuration" and len(rest) > 1:
            base = base if base.endswith("/Configuration") else base + "/Configuration"
            return f"{base}/Type" if rest[1] == "type" else f"{base}/Properties/{rest[2]}" if len(rest) > 2 else f"{base}/Properties"
        if rest[0] in ("from", "to", "from_pin", "to_pin"):
            return f"{base}/Inputs"
        if rest[0] == "page":
            return base
        field = _OP_FIELDS.get(rest[0])
        if field and not base.endswith(f"/{field}"):
            return f"{base}/{field}"
        return base
    return "/Body"


def edit(calls, ops):
    changed = []
    for chunk in common.chunk_ops(ops):
        op_paths = [path for _, path in chunk]

        def where(primitive_path, op_paths=op_paths):
            return op_path(primitive_path, op_paths)
        outcome = common.invoke(calls.context, ms.BY_ID, ms.EDIT, {"schema_version": 1, "ops": [op for op, _ in chunk]},
                                (NamedOpaqueHandleArgument("asset", calls.write),), paths=where, prefix=ms.PREFIX)
        changed.extend(outcome.changed_set)
    return changed


# ---------------------------------------------------------------------------
# Refusals
# ---------------------------------------------------------------------------
def is_preset(live):
    return live.header.get("preset") is True


def converts_from_preset(body, live):
    """Preset: null on a preset (the editor's Convert From Preset)."""
    return is_preset(live) and "Preset" in body and body["Preset"] is None


def preset_body_is_regular(body):
    """A converting Body that describes the converted graph (applied after the conversion) rather than overrides."""
    return not ms.is_conversion_only(body)


def refuse_unsupported(body, live, changes):
    """Stable refusals of what the editor cannot do or the project's page settings do not allow; ``changes`` are the
    Body differences."""
    authored_preset = body.get("Preset", _ABSENT)
    if is_preset(live):
        if authored_preset is None:
            if not preset_body_is_regular(body):
                ms.check_preset_body(body, live.inputs)
        else:
            parent = live.header.get("preset_parent", "")
            if type(authored_preset) is dict and ms.object_path(authored_preset.get("Parent")) != parent:
                raise error("/Body/Preset/Parent", f"This preset's parent is {parent}; the editor cannot re-parent a preset. "
                            "Create a new preset of the other MetaSound.", "MetaSound.PresetParentChanged")
            ms.check_preset_body(body, live.inputs)
    elif authored_preset is not _ABSENT and authored_preset is not None:
        raise error("/Body/Preset", "The MetaSound editor cannot convert an existing MetaSound into a preset; Create a new "
                    "asset with Preset {\"Parent\": ...} (the Content Browser's Create Preset).", "MetaSound.CannotConvertToPreset")
    check_pages(body, live)


def _unknown_page(path, page, live):
    return error(path, f"The project's MetaSound page settings (Project Settings > MetaSound > Pages) define no page "
                 f"'{page}'; pages: {', '.join(live.configured)}.", "MetaSound.UnknownPage")


def check_pages(body, live):
    """Graphs and PageDefaults name pages of the project settings (or pages the MetaSound already has)."""
    configured = set(live.configured)
    for page in body.get("Graphs", {}):
        if page not in configured and page not in live.pages:
            raise _unknown_page(graph_path(page), page, live)
    for name, entry in body.get("Inputs", {}).items():
        values = entry.get("PageDefaults")
        if values is None:
            continue
        path = f"/Body/Inputs/{common.escape(name)}/PageDefaults"
        if type(values) is not dict:
            raise error(path, "PageDefaults is {\"<page>\": value} for the pages other than Default.", "MetaSound.InvalidBody")
        record = live.inputs.get(name) or live.inputs.get(entry.get("RenamedFrom"))
        existing = {item["page"] for item in record.get("defaults", [])} if record is not None else set()
        for page in values:
            if page == DEFAULT:
                raise error(f"{path}/{DEFAULT}", "The Default page's value is the input's Default.", "MetaSound.InvalidBody")
            if page not in configured and page not in existing:
                raise _unknown_page(f"{path}/{common.escape(page)}", page, live)


# ---------------------------------------------------------------------------
# Presets
# ---------------------------------------------------------------------------
def phase_preset(body, live):
    """Metadata and the input overrides of a preset: a listed input overrides (its Default, else the value it has),
    an unlisted one inherits the parent's default again (the registration's preset rebuild restores it)."""
    ops = _metadata_ops(body, live)
    if "Inputs" not in body:
        return ops
    inherits = set(live.header.get("inherits", []))
    authored = body["Inputs"]
    for name in sorted(live.inputs):
        path = f"/Body/Inputs/{common.escape(name)}"
        entry = authored.get(name)
        if entry is None:
            if name not in inherits:
                ops.append(({"op": "input.inherit", "name": name, "inherits": True}, path))
            continue
        wanted = entry.get("Default", _ABSENT)
        pages = entry.get("PageDefaults", {})
        pages_differ = not common.same(pages, _live_page_defaults(live.inputs[name]), exact=False)
        if wanted is not _ABSENT and (name in inherits or not common.same(wanted, _live_default(live.inputs[name]), exact=False)):
            # SetGraphInputDefault also clears the input's inheritance.
            ops.append(({"op": "input.set", "name": name, "default": wanted}, f"{path}/Default"))
        elif name in inherits and not pages_differ:
            ops.append(({"op": "input.inherit", "name": name, "inherits": False}, path))
        if pages_differ:
            ops.append(({"op": "input.set", "name": name, "page_defaults": pages}, f"{path}/PageDefaults"))
    return ops


# ---------------------------------------------------------------------------
# Phase 1: open-time injection, interfaces
# ---------------------------------------------------------------------------
def phase_interfaces(body, live):
    ops = []
    if "Graphs" in body:
        # A page's delete button in the Pages Details (RemoveGraphPage); Default always stays.
        for page in live.pages:
            if page != DEFAULT and page not in body["Graphs"]:
                ops.append(({"op": "page.remove", "name": page}, graph_path(page)))
    if live.header.get("injected"):
        ops.append(({"op": "normalize"}, "/Body/Graphs"))
    if "Interfaces" in body:
        declared = {item["name"] for item in live.header.get("interfaces", [])}
        wanted = set(body["Interfaces"])
        have = set(live.body.get("Interfaces", []))
        add = sorted(wanted - declared)
        remove = sorted(have - wanted)
        if add or remove:
            ops.append(({"op": "interfaces", "add": add, "remove": remove}, "/Body/Interfaces"))
    return ops


# ---------------------------------------------------------------------------
# Phase 2: metadata, members, variables
# ---------------------------------------------------------------------------
def _metadata_ops(body, live):
    if "Metadata" not in body:
        return []
    current = live.body.get("Metadata", {})
    op = {"op": "metadata"}
    for key, value in body["Metadata"].items():
        if not common.same(value, current.get(key, ms.METADATA_DEFAULTS[key]), exact=False):
            op[_METADATA_FIELDS[key]] = value
    return [(op, "/Body/Metadata")] if len(op) > 1 else []


def _live_default(record):
    return next((item["value"] for item in record.get("defaults", []) if item["page"] == DEFAULT), None)


def _live_page_defaults(record):
    """The input's values for the pages other than Default, as the extract shows them."""
    values = {item["page"]: item["value"] for item in record.get("defaults", []) if item["page"] != DEFAULT}
    if record.get("type") == ms.TRIGGER:
        values = {page: value for page, value in values.items() if value is not False}
    return values


def _renames(authored, records, interface, path):
    """{new name: old name} of the entries that name a live member in RenamedFrom."""
    renames = {}
    for name, entry in authored.items():
        source = entry.get("RenamedFrom")
        if source is None or name in records:
            continue
        if type(source) is not str or source not in records or source in authored or source in renames.values() \
                or source in interface:
            raise error(f"{path}/{common.escape(name)}/RenamedFrom", f"RenamedFrom names a member of this MetaSound "
                        f"that the document no longer lists (got {source!r}).", "MetaSound.InvalidRename")
        renames[name] = source
    return renames


def _member_ops(body, live, region, output_node_ids):
    """Adds, renames and field changes of Inputs or Outputs (removals are phase 3)."""
    if region not in body:
        return []
    is_input = region == "Inputs"
    kind = "input" if is_input else "output"
    authored = body[region]
    records = live.inputs if is_input else live.outputs
    interface = live.facts["interface_inputs" if is_input else "interface_outputs"]
    renames = _renames(authored, records, interface, f"/Body/{region}")
    ops = []
    for name, entry in authored.items():
        path = f"/Body/{region}/{common.escape(name)}"
        record = records.get(renames.get(name, name))
        if record is None:
            if type(entry.get("Type")) is not str:
                raise error(f"{path}/Type", f"A new {kind} names its Type (a MetaSound data type such as Float, "
                            "Audio, Trigger, Int32:Array; list_types kind metasound_datatype).", "MetaSound.InvalidBody")
            op = {"op": f"{kind}.add", "name": name, "type": entry["Type"]}
            if entry.get("Constructor") is True:
                op["constructor"] = True
            if not is_input and name in output_node_ids:
                op["node_id"] = output_node_ids[name]
            for key, field in _MEMBER_FIELDS.items():
                if key in entry and (key == "SortOrder" or entry[key] not in ("", False)):
                    op[field] = entry[key]
            if is_input and entry.get("Default") is not None:
                op["default"] = entry["Default"]
            if is_input and entry.get("PageDefaults"):
                op["page_defaults"] = entry["PageDefaults"]
            ops.append((op, path))
            if is_input and entry.get("Widget"):
                ops.append(({"op": "input.widget", "name": name, "values": entry["Widget"]}, f"{path}/Widget"))
            continue
        is_interface = record["name"] in interface
        op = {"op": f"{kind}.set", "name": record["name"]}
        if name != record["name"]:
            op["rename_to"] = name
        retyped = "Type" in entry and entry["Type"] != record["type"]
        if retyped:
            if is_interface:
                raise error(f"{path}/Type", f"'{name}' belongs to an interface; its type is fixed.", "MetaSound.InvalidMember")
            op["type"] = entry["Type"]
        if "Constructor" in entry or not is_interface:
            wanted = entry.get("Constructor", False)
            if wanted != record["constructor"]:
                op["constructor"] = wanted
        for key, field in _MEMBER_FIELDS.items():
            if key in entry:
                wanted = entry[key]
            elif is_interface or key == "SortOrder":
                continue
            else:
                wanted = ms._MEMBER_DEFAULTS[key]
            if not common.same(wanted, record[field], exact=False):
                op[field] = wanted
        if is_input and "Default" in entry and (retyped or not common.same(entry["Default"], _live_default(record),
                                                                           exact=False)):
            op["default"] = entry["Default"]
        if is_input:
            pages = entry.get("PageDefaults", None if is_interface else {})
            current = _live_page_defaults(record)
            if pages is not None and (not common.same(pages, current, exact=False) or (retyped and (pages or current))):
                op["page_defaults"] = pages
        if len(op) > 2:
            ops.append((op, path))
        if is_input:
            widget = entry.get("Widget", None if is_interface else {})
            # Complete over the settings object's defaults (a listed default reads back omitted).
            settings = record.get("widget_defaults") or {}
            if widget is not None and (not common.same({**settings, **widget}, {**settings, **record.get("widget", {})},
                                                       exact=False)
                                       or (retyped and widget)):
                ops.append(({"op": "input.widget", "name": name, "values": widget}, f"{path}/Widget"))
    return ops


def _variable_ops(graph, live, page=DEFAULT):
    if "Variables" not in graph:
        return []
    authored = graph["Variables"]
    records = live.page_variables(page)
    variables = graph_path(page, "Variables")
    renames = _renames(authored, records, (), variables)
    ops = []
    for name in sorted(set(records) - set(authored) - set(renames.values())):
        ops.append((on_page({"op": "variable.remove", "name": name}, page), f"{variables}/{common.escape(name)}"))
    for name, entry in authored.items():
        path = f"{variables}/{common.escape(name)}"
        record = records.get(renames.get(name, name))
        if record is not None and "Type" in entry and entry["Type"] != record["type"]:
            # The editor's type change: remove the variable and its nodes, add it again (phase 3 adds the nodes).
            ops.append((on_page({"op": "variable.remove", "name": record["name"]}, page), path))
            record = None
        if record is None:
            if type(entry.get("Type")) is not str:
                raise error(f"{path}/Type", "A new variable names its Type.", "MetaSound.InvalidBody")
            op = on_page({"op": "variable.add", "name": name, "type": entry["Type"]}, page)
            for key, field in (("Default", "default"), ("DisplayName", "display_name"), ("Description", "description")):
                if entry.get(key) not in (None, ""):
                    op[field] = entry[key]
            ops.append((op, path))
            continue
        op = on_page({"op": "variable.set", "name": record["name"]}, page)
        if name != record["name"]:
            op["rename_to"] = name
        if "Default" in entry and not common.same(entry["Default"], record.get("default"), exact=False):
            op["default"] = entry["Default"]
        for key, field in (("DisplayName", "display_name"), ("Description", "description")):
            wanted = entry.get(key, "")
            if wanted != record.get(field, ""):
                op[field] = wanted
        if len(op) > (3 if "page" in op else 2):
            ops.append((op, path))
    return ops


def _output_node_ids(body):
    """{output name: node Id} of the Output entries that carry one (a new output keeps its document Id; an output's
    node has the same ID on every page, the Default page's entry wins)."""
    graphs = body.get("Graphs", {})
    ids = {}
    for page in [DEFAULT] + [page for page in graphs if page != DEFAULT]:
        for entry in graphs.get(page, {}).get("Nodes", {}).values():
            if entry.get("Class") == "Output" and type(entry.get("Member")) is str and type(entry.get("Id")) is str:
                ids.setdefault(entry["Member"], entry["Id"])
    return ids


def _live_pages(body, live):
    """(page, authored graph) of the document's pages the MetaSound has (new pages come after phase_pages)."""
    return [(page, graph) for page, graph in body.get("Graphs", {}).items() if page in live.pages and type(graph) is dict]


def phase_node_classes(body, live):
    """Kept nodes whose class version or configuration changes: the editor's Update Node and the node Details
    configuration, before the graph phase plans connections against the pins they leave."""
    ops = []
    for page, graph in _live_pages(body, live):
        nodes = graph.get("Nodes", {})
        live_nodes = live.page_nodes(page)
        facts = ms.page_facts(live.facts, page)
        matched = ms.match_nodes(nodes, live_nodes)
        for key, entry in nodes.items():
            before = live_nodes.get(matched.get(key))
            if before is None or before.get("Class") != entry.get("Class") or before.get("Member") != entry.get("Member") \
                    or entry.get("Class") in ms.MEMBER_CLASSES:
                continue
            path = f"{graph_path(page, 'Nodes')}/{common.escape(key)}"
            if entry.get("Version", 1) != before.get("Version", 1):
                ops.append((on_page({"op": "node.update", "id": before["Id"], "major": entry.get("Version", 1)}, page),
                            f"{path}/Version"))
            config_defaults = facts.get("configuration_defaults", {}).get(before.get("Id"))
            if not common.same(ms.configuration_properties(entry, config_defaults),
                               ms.configuration_properties(before, config_defaults), exact=False):
                ops.append((on_page({"op": "node.set", "id": before["Id"], "configuration": _configuration_arg(entry)}, page),
                            f"{path}/Configuration"))
    return ops


def _configuration_arg(entry):
    value = entry.get("Configuration")
    arg = {"properties": ms.configuration_properties(entry)}
    if type(value) is dict and type(value.get("Type")) is str:
        arg["type"] = value["Type"]
    return arg


def phase_members(body, live):
    ops = (_metadata_ops(body, live) + _member_ops(body, live, "Inputs", {})
           + _member_ops(body, live, "Outputs", _output_node_ids(body)))
    for page, graph in _live_pages(body, live):
        ops += _variable_ops(graph, live, page)
    return ops


def phase_pages(body, live):
    """The pages the document adds: the Pages Details "+" (AddGraphPage, a copy of the MetaSound's last page, which the
    phases before have already brought to the document), then reconciled by phases 2-4 like any page."""
    return [({"op": "page.add", "name": page}, graph_path(page)) for page in body.get("Graphs", {})
            if page not in live.pages]


# ---------------------------------------------------------------------------
# Phase 3: nodes, connections, comments, member removals
# ---------------------------------------------------------------------------
def _parse_source(source, path):
    if type(source) is not str or not source:
        raise error(path, "A source is \"<node key>\" or \"<node key>.<output pin>\".", "MetaSound.InvalidSource")
    if source.startswith("Input:"):
        raise error(path, f"'{source}' names an input without an input node; add an Input node "
                    f"(Class \"Input\", Member \"{source[6:]}\") and connect from its key.", "MetaSound.InvalidSource")
    key, _, pin = source.partition(".")
    return key, (pin or None)


def _depths(nodes):
    depth = {}

    def visit(key, trail):
        if key in depth:
            return depth[key]
        if key in trail or key not in nodes:
            return 0
        trail.add(key)
        sources = [_parse_source(value, "")[0] for value in nodes[key].get("Inputs", {}).values()
                   if type(value) is str and value and not value.startswith("Input:")]
        depth[key] = 1 + max((visit(source, trail) for source in sources), default=-1)
        trail.discard(key)
        return depth[key]
    for key in nodes:
        visit(key, set())
    return depth


def _point_of(value):
    return list(value) if type(value) in (list, tuple) and len(value) == 2 \
        and all(type(item) in (int, float) for item in value) else None


def _layout(nodes, keys, placed=None):
    """Positions of the new nodes ``keys`` (no Position, not Hidden), by graph depth as Material places new expressions:
    a node one column left of the leftmost placed node it feeds, else one column right of the placed nodes feeding it,
    else in its depth's column; then down to the first row that overlaps no node. ``placed``: {key: position} of the
    nodes that keep theirs (new nodes no longer land on existing ones or right of the output they feed)."""
    depth = _depths(nodes)
    sources, consumers = {}, {}
    for key, entry in nodes.items():
        for value in (entry.get("Inputs") or {}).values():
            if type(value) is str and value and not value.startswith("Input:"):
                source = _parse_source(value, "")[0]
                sources.setdefault(key, []).append(source)
                consumers.setdefault(source, []).append(key)
    taken = {key: point for key, point in ((key, _point_of(value)) for key, value in (placed or {}).items()) if point}
    positions = {}
    # The nodes nearest the outputs first, so a new chain is placed from the node it feeds back.
    for key in sorted(keys, key=lambda item: -depth.get(item, 0)):
        fed = [taken[item] for item in consumers.get(key, ()) if item in taken]
        feeding = [taken[item] for item in sources.get(key, ()) if item in taken]
        if fed:
            x, y = min(point[0] for point in fed) - COLUMN, min(point[1] for point in fed)
        elif feeding:
            x, y = max(point[0] for point in feeding) + COLUMN, min(point[1] for point in feeding)
        else:
            x, y = depth.get(key, 0) * COLUMN, 0
        while any(abs(x - other[0]) < COLUMN and abs(y - other[1]) < ROW for other in taken.values()):
            y += ROW
        taken[key] = positions[key] = [x, y]
    return positions


def _create_op(key, entry, new_id, path):
    cls = entry["Class"]
    op = {"op": "node.create", "id": new_id}
    if cls == "Input":
        op.update(kind="input", member=entry.get("Member"))
    elif cls in ("Get", "GetDelayed", "Set"):
        op.update(kind={"Get": "get", "GetDelayed": "get_delayed", "Set": "set"}[cls], member=entry.get("Member"))
    elif cls == ms.REROUTE:
        op["kind"] = "reroute"
    elif cls.startswith("/"):
        op.update(kind="asset", asset=cls)
    else:
        op.update(kind="class", **{"class": cls})
        if entry.get("Version", 1) != 1:
            op["major"] = entry["Version"]
    if cls in ms.MEMBER_CLASSES and type(entry.get("Member")) is not str:
        raise error(f"{path}/Member", f"A {cls} node names its Member.", "MetaSound.InvalidBody")
    return op


def phase_graph(body, live):
    ops = []
    removed_outputs = set()
    if "Outputs" in body:
        removed_outputs = set(live.outputs) - set(body["Outputs"]) - live.facts["interface_outputs"]
    for page, graph in _live_pages(body, live):
        if "Nodes" in graph:
            ops += _node_ops(graph["Nodes"], live, removed_outputs, page)
        if "Comments" in graph:
            ops += _comment_ops(graph["Comments"], live, page)
    for region, records, kind in (("Inputs", live.inputs, "input"), ("Outputs", live.outputs, "output")):
        if region not in body:
            continue
        interface = live.facts["interface_inputs" if kind == "input" else "interface_outputs"]
        for name in sorted(set(records) - set(body[region]) - interface):
            ops.append(({"op": f"{kind}.remove", "name": name}, f"/Body/{region}/{common.escape(name)}"))
    return ops


def _node_ops(nodes, live, removed_outputs, page=DEFAULT):
    nodes_path = graph_path(page, "Nodes")
    live_nodes = live.page_nodes(page)
    facts = ms.page_facts(live.facts, page)
    seen_outputs = {}
    for key, entry in nodes.items():
        if entry.get("Class") == "Output":
            member = entry.get("Member")
            if member in seen_outputs:
                raise error(f"{nodes_path}/{common.escape(key)}", f"Output '{member}' already has the node "
                            f"'{seen_outputs[member]}'; an output has one node per page.", "MetaSound.DuplicateOutputNode")
            seen_outputs[member] = key
    matched = ms.match_nodes(nodes, live_nodes)
    class_defaults = facts.get("class_defaults", {})
    outputs = facts.get("outputs", {})
    deletes, creates, reroutes, sets, disconnects, connects = [], [], [], [], [], []
    refs = {}            # authored key -> node reference of the ops
    live_of = {}         # authored key -> live entry (same node kept)
    gone = set()         # live node IDs deleted
    for key, entry in nodes.items():
        before = live_nodes.get(matched.get(key))
        if before is not None and before.get("Class") == entry.get("Class") and before.get("Member") == entry.get("Member"):
            live_of[key] = before
    kept = {matched[key] for key in live_of}
    for live_key, before in live_nodes.items():
        if live_key in kept:
            continue
        if before.get("Class") == "Output":
            if before.get("Member") in removed_outputs or ms.is_bare_output(before):
                continue
            # The editor's delete of an output node: its edges and location go, the vertex node stays.
            deletes.append(({"op": "node.delete", "id": f"output:{before['Member']}"}, nodes_path))
            continue
        deletes.append(({"op": "node.delete", "id": before["Id"]}, nodes_path))
        gone.add(before["Id"])
    new_keys = []
    for key, entry in nodes.items():
        path = f"{nodes_path}/{common.escape(key)}"
        if entry.get("Class") == "Output":
            if key not in live_of:
                raise error(f"{path}/Member", f"The MetaSound has no output '{entry.get('Member')}' (declare it in "
                            "Outputs, or add the interface that has it).", "MetaSound.UnknownMember")
            refs[key] = f"output:{entry['Member']}"
            continue
        if key in live_of:
            refs[key] = live_of[key]["Id"]
            continue
        refs[key] = ms.node_id(key, entry)
        new_keys.append(key)
    unplaced = [key for key in new_keys if "Position" not in nodes[key] and nodes[key].get("Hidden") is not True]
    unplaced += [key for key, before in live_of.items() if "Position" not in nodes[key]
                 and nodes[key].get("Hidden") is not True and "Position" not in before]
    placed = {key: entry["Position"] for key, entry in nodes.items() if "Position" in entry}
    placed.update({key: before["Position"] for key, before in live_of.items()
                   if key not in placed and "Position" in before and nodes[key].get("Hidden") is not True})
    layout = _layout(nodes, unplaced, placed)

    def source_ref(source, path):
        key, pin = _parse_source(source, path)
        if key not in nodes:
            raise error(path, f"No node '{key}' in Nodes (a source is \"<node key>\" or \"<node key>.<output pin>\")."
                        f"{common.describe_candidates(key, sorted(nodes), 'Nodes')}", "MetaSound.NodeNotFound")
        if pin is None and key in live_of:
            pins = outputs.get(live_of[key]["Id"], [])
            pin = pins[0] if len(pins) == 1 else None
        return refs[key], pin

    pending = []
    for key in new_keys:
        entry = nodes[key]
        path = f"{nodes_path}/{common.escape(key)}"
        op = _create_op(key, entry, refs[key], path)
        position = entry.get("Position", layout.get(key))
        if position is not None:
            op["position"] = position
        if op["kind"] == "reroute":
            pending.append((key, op, path))
        else:
            creates.append((op, path))
    # Reroutes take the type of what they connect: their source, or a consumer, must exist first.
    made = set(live_of) | {key for key in new_keys if key not in {item[0] for item in pending}}
    while pending:
        progress = False
        for item in list(pending):
            key, op, path = item
            source = nodes[key].get("Inputs", {}).get("Value")
            if source is not None:
                if _parse_source(source, f"{path}/Inputs/Value")[0] in made:
                    ref, pin = source_ref(source, f"{path}/Inputs/Value")
                    op["from"] = ref
                    if pin is not None:
                        op["from_pin"] = pin
            else:
                consumer = next(((other, pin) for other, entry in nodes.items() if other in made
                                 for pin, value in entry.get("Inputs", {}).items()
                                 if type(value) is str and value.partition(".")[0] == key), None)
                if consumer is not None:
                    op["to"] = refs[consumer[0]]
                    op["to_pin"] = consumer[1]
            if "from" in op or "to" in op:
                reroutes.append((op, path))
                pending.remove(item)
                made.add(key)
                progress = True
        if not progress:
            raise error(pending[0][2], "A reroute takes the data type of what it connects: give it Inputs.Value, or "
                        "connect a node's input to it.", "MetaSound.RerouteType")
    for key, entry in nodes.items():
        path = f"{nodes_path}/{common.escape(key)}"
        before = live_of.get(key)
        op = {"op": "node.set", "id": refs[key]}
        if before is not None:
            if "Position" in entry:
                if not common.same(entry["Position"], before.get("Position"), exact=False):
                    op["position"] = entry["Position"]
            elif entry.get("Hidden") is True:
                if "Position" in before:
                    op["hidden"] = True
            elif key in layout:
                op["position"] = layout[key]
            if not ms.same_defaults(entry.get("Defaults", {}), before.get("Defaults", {}),
                                    class_defaults.get(before.get("Id"), {}), exact=False):
                op["defaults"] = entry.get("Defaults", {})
        else:
            if ms.configuration_properties(entry) or type(entry.get("Configuration")) is dict:
                # A new node's configuration (node Details) before its defaults: it can change the pins.
                op["configuration"] = _configuration_arg(entry)
            if entry.get("Defaults"):
                op["defaults"] = entry["Defaults"]
        if before is None and entry.get("Class") == "Set" and "defaults" not in op:
            # A new Set node starts with the variable's value as its literal (AddGraphVariableMutatorNode); the
            # document's Defaults are complete.
            op["defaults"] = {}
        for field, arg, default in (("Comment", "comment", ""), ("CommentVisible", "comment_visible", False),
                                    ("HideUnconnectedPins", "hide_unconnected", False)):
            wanted = entry.get(field, default)
            if wanted != (before.get(field, default) if before is not None else default):
                op[arg] = wanted
        if len(op) > 2:
            sets.append((op, path))
        # Connections: the document's Inputs are complete.
        mine = entry.get("Inputs", {})
        theirs = before.get("Inputs", {}) if before is not None else {}
        for pin in sorted(set(mine) | set(theirs)):
            pin_path = f"{path}/Inputs/{common.escape(pin)}"
            wanted = source_ref(mine[pin], pin_path) if pin in mine else None
            current = None
            if pin in theirs:
                source_key, _, source_pin = theirs[pin].partition(".")
                source_entry = live_nodes.get(source_key)
                if source_entry is not None and source_entry.get("Id") not in gone:
                    if not source_pin:
                        pins = outputs.get(source_entry["Id"], [])
                        source_pin = pins[0] if len(pins) == 1 else None
                    current = (source_entry["Id"], source_pin)
            if wanted == current:
                continue
            if current is not None:
                disconnects.append(({"op": "connect", "id": refs[key], "pin": pin, "from": None}, pin_path))
            if wanted is not None:
                op = {"op": "connect", "id": refs[key], "pin": pin, "from": wanted[0]}
                if wanted[1] is not None:
                    op["from_pin"] = wanted[1]
                connects.append((op, pin_path))
    # Disconnect first, so that rewiring never meets a transient loop.
    return [(on_page(op, page), path) for op, path in deletes + creates + reroutes + sets + disconnects + connects]


def _comment_ops(comments, live, page=DEFAULT):
    live_comments = live.page_comments(page)
    region = graph_path(page, "Comments")
    matched = ms.match_comments(comments, live_comments)
    ops = []
    for live_key in sorted(set(live_comments) - set(matched.values())):
        ops.append((on_page({"op": "comment.remove", "id": live_comments[live_key]["Id"]}, page), region))
    for key, entry in comments.items():
        path = f"{region}/{common.escape(key)}"
        before = live_comments.get(matched.get(key), {})
        op = {"op": "comment.set", "id": ms.comment_id(key, entry)}
        for field, arg in (("Text", "text"), ("Position", "position"), ("Size", "size"), ("Color", "color"),
                           ("FontSize", "font_size"), ("MoveMode", "move_mode")):
            if field in entry and (not before or not common.same(entry[field], before.get(field), exact=False)):
                op[arg] = entry[field]
        bubble = entry.get("ColorBubble", False)
        if bubble != before.get("ColorBubble", False) or (not before and bubble):
            op["color_bubble"] = bubble
        if not before or len(op) > 2:
            ops.append((on_page(op, page), path))
    return ops


# ---------------------------------------------------------------------------
# Validation outcome
# ---------------------------------------------------------------------------
def _messages(value):
    return [item for item in (value or {}).get("messages", []) if type(item) is dict]


def compile_outcome(before, result, node_paths):
    """(fatal diagnostic or None, warnings): new validation errors fail the apply, errors the asset already had and
    warnings are reported, and so are the loaded MetaSounds that use this one (dependents, §5.3)."""
    def where(item):
        page = item.get("page") or DEFAULT
        for node in [node for node in item.get("node_ids") or () if node] or [item.get("node_id", "")]:
            path = node_paths.get((page, node)) or node_paths.get(node)
            if path:
                return path
        return "/Body"

    def key(item):
        return item.get("message"), item.get("node_id"), item.get("page") or DEFAULT

    def is_build(item):
        return item.get("source") == "Build"
    old = {key(item) for item in _messages(before) if item.get("severity") == "Error"}
    errors = [item for item in _messages(result) if item.get("severity") == "Error"]
    new = [item for item in errors if key(item) not in old]
    new_validation = [item for item in new if not is_build(item)]
    if new_validation:
        message = "The MetaSound does not validate: " + " | ".join(item.get("message", "") for item in new_validation[:8])
        return error(where(new_validation[0]), message, "MetaSound.ValidationFailed"), ()
    if new:
        # The operator build check: what the editor's Play would log (design §5.1).
        message = "The MetaSound does not build: " + " | ".join(item.get("message", "") for item in new[:8])
        return error(where(new[0]), message, "MetaSound.BuildFailed"), ()
    warnings = [RuntimeDiagnostic(path=where(item), code="MetaSound.BuildError" if is_build(item) else "MetaSound.ValidationError",
                                  message=item.get("message", ""), severity=DiagnosticSeverity.WARNING) for item in errors]
    warnings += [RuntimeDiagnostic(path=where(item), code="MetaSound.ValidationWarning", message=item.get("message", ""),
                                   severity=DiagnosticSeverity.WARNING)
                 for item in _messages(result) if item.get("severity") in ("Warning", "Info")]
    dependents = [item for item in (result or {}).get("dependents", []) if type(item) is dict]
    if dependents:
        warnings.append(RuntimeDiagnostic(
            path="/Body", code="MetaSound.DependentsUpdated", severity=DiagnosticSeverity.WARNING,
            message="Loaded MetaSounds that use this one as a node were registered again: "
                    + ", ".join(item.get("path", "") for item in dependents[:20])))
        unsaved = [item.get("path", "") for item in dependents if item.get("dirty_before")]
        if unsaved:
            warnings.append(RuntimeDiagnostic(
                path="/Body", code="MetaSound.DependentHasUnsavedChanges", severity=DiagnosticSeverity.WARNING,
                message="These MetaSounds that use this one have unsaved changes; they now see its new interface: "
                        + ", ".join(unsaved[:20])))
    return None, tuple(warnings)


def _node_paths(body, live):
    """{(page, node id): document path} of the authored nodes after the apply (and {node id: path} of the Default
    page's, for messages without a page)."""
    paths = {}
    for page, graph in body.get("Graphs", {}).items():
        nodes = graph.get("Nodes", {}) if type(graph) is dict else {}
        live_nodes = live.page_nodes(page)
        matched = ms.match_nodes(nodes, live_nodes)
        for key, entry in nodes.items():
            path = f"{graph_path(page, 'Nodes')}/{common.escape(key)}"
            ids = [ms.node_id(key, entry)] + ([live_nodes[matched[key]].get("Id")] if key in matched else [])
            for node in ids:
                paths[(page, node)] = path
                if page == DEFAULT:
                    paths[node] = path
    return paths


# ---------------------------------------------------------------------------
# Apply
# ---------------------------------------------------------------------------
_CONVERT = "preset.convert_from"


def _phases(body, live):
    regular = (phase_interfaces, phase_members, phase_node_classes, phase_graph,
               phase_pages, phase_members, phase_node_classes, phase_graph)
    if not is_preset(live):
        return regular
    if converts_from_preset(body, live):
        # Convert From Preset; a Body that describes the graph is applied to the converted MetaSound, otherwise the
        # Body's overrides and metadata are applied to the preset first.
        return (_CONVERT,) + regular if preset_body_is_regular(body) else (phase_preset, _CONVERT)
    return (phase_preset,)


def apply(canonical, read, mutable, context):
    """Core calls apply only when the document differs from the asset (its verify-phase Diff), and the editor's
    property reads are verify-only: the authored Properties are written as the Details panel does (an equal value
    changes nothing), then the Body is applied against a fresh read."""
    body = canonical.get("Body", {})
    region = ms._properties(canonical["Class"])
    properties = canonical.get("Properties", {})
    region.check_owned(properties)
    calls = ms.AudioCalls(context, read, mutable)
    live = Live(calls)
    changes = live.changes(canonical)
    if changes:
        refuse_unsupported(body, live, changes)
    before = calls.query(ms.READ, {"validate": True})
    changed = []
    if properties:
        fixed = live.facts.get("fixed_interfaces")
        changed.extend(region.apply(properties, read, mutable, context))
        live = Live(calls)
        # OutputFormat swaps the output-format interface (PostEditChangeOutputFormat): the graph changed.
        format_changed = live.facts.get("fixed_interfaces") != fixed
        changes = live.changes(canonical)
        if changes:
            refuse_unsupported(body, live, changes)
        if not changes and not format_changed:
            # Only Details settings changed: nothing to register.
            return tuple(changed)
    elif not changes:
        return ()
    for phase in _phases(body, live):
        if phase is _CONVERT:
            changed.extend(edit(calls, [({"op": "preset.convert_from"}, "/Body/Preset")]))
            live = Live(calls)
            continue
        ops = phase(body, live)
        if ops:
            changed.extend(edit(calls, ops))
            live = Live(calls)
    outcome = common.invoke(context, ms.BY_ID, ms.COMPILE, {"schema_version": 1},
                            (NamedOpaqueHandleArgument("asset", calls.write),), prefix=ms.PREFIX)
    changed.extend(outcome.changed_set)
    failure, warnings = compile_outcome(before, outcome.value if type(outcome.value) is dict else {},
                                        _node_paths(body, live))
    if failure is not None:
        raise failure
    return with_diagnostics(changed, warnings)


def preflight(canonical, read, context):
    """Refusals that need no write, on an existing asset."""
    calls = ms.AudioCalls(context, read)
    live = Live(calls)
    changes = live.changes(canonical)
    if changes:
        refuse_unsupported(canonical.get("Body", {}), live, changes)


__all__ = ("apply", "preflight", "compile_outcome", "op_path", "phase_interfaces", "phase_members", "phase_node_classes",
           "phase_graph", "phase_pages", "phase_preset", "refuse_unsupported", "check_pages", "Live")
