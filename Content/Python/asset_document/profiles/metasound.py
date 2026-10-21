"""Registered MetaSound Profiles (profile.metasound_source.v1, profile.metasound_patch.v1), phases M1-M3.

Document (docs/architecture/asset-document-metasound-design.md §3)::

    "Body": {
      "Interfaces": ["UE.Source.OneShot"],                 # user-modifiable declared interfaces
      "Metadata": {"Description": "...", "Keywords": ["ui"], "Referenceable": true},
      "Inputs": {"Pitch": {"Type": "Float", "Default": 0.0, "SortOrder": 0,
                           "Widget": {"Range": {"X": -12, "Y": 12}, "WidgetType": "Slider"}}},
      "Outputs": {},
      "Graphs": {"Default": {
        "Nodes": {
          "Pitch":  {"Class": "Input", "Member": "Pitch", "Id": "...", "Position": [-500, 200]},
          "Sine":   {"Class": "UE.Sine.Audio", "Id": "...", "Position": [-200, 0],
                     "Inputs": {"Frequency": "Pitch"}, "Defaults": {"Phase Offset": 0.25}},
          "UE_OutputFormat_Mono_Audio_0": {"Class": "Output", "Member": "UE.OutputFormat.Mono.Audio:0", "Id": "...",
                     "Position": [200, 0], "Inputs": {"UE.OutputFormat.Mono.Audio:0": "Sine"}}},
        "Variables": {"Count": {"Type": "Int32", "Default": 0}},
        "Comments": {"Comment": {"Id": "...", "Text": "...", "Position": [-600, -100], "Size": [400, 200]}}}},
      "Preset": {"Parent": "/Game/Audio/MSS_Base.MSS_Base"}  # presets only (their graph is derived)
    }

* A node's ``Configuration`` ``{Type, Properties}`` is extracted when it differs from the configuration a new node of
  its class gets; omitted means that default.

* The read presents the MetaSound as the MetaSound editor shows it after opening: the open-time input template
  injection is simulated on a copy (nodes it would add are listed by Inspect as ``InjectedNodes``).  Reading never
  registers the MetaSound, dirties it or leaves objects behind.
* Node keys are document-local: the member name for member nodes (``Input``/``Output``/``Get``/``GetDelayed``/``Set``),
  otherwise the class name part, made of letters, digits, ``_`` and ``-`` with ``_2``, ``_3``, ... in node-ID order.
  ``Id`` is the frontend node ID (the durable identity).  A node without an editor location is ``"Hidden": true``.
* Sources are ``"<key>"`` (a node with one output) or ``"<key>.<output pin>"``.  ``Defaults`` lists the input
  literals that differ from the class default.  Literal forms follow the pin's data type.
* ``Id`` is optional when writing: an entry without one is the node (or comment) with the ID derived from its key
  (``keyed_id``), so an AI-written document keeps matching what its first apply created.  An ``Output`` entry is
  the output's node, matched by ``Member``; an output without an entry is a node without location or edges.
* Omitted: member ``Default`` / ``SortOrder`` and node ``Position`` keep the live value; other member fields and
  node ``Inputs`` / ``Defaults`` / ``Comment`` are complete (omitted = none / class default).  Apply is
  ``metasound_apply``.
* Pages (M3): ``Graphs`` has one entry per graph page, ``Default`` first, then the pages of the project's
  ``UMetaSoundSettings`` in settings order (a page the settings no longer define is named by its 32 hex digits).
  ``Inputs.<n>.PageDefaults`` ``{"<page>": value}`` holds an input's values for the pages other than Default.
"""
from __future__ import annotations

import dataclasses
import hashlib
import json
import re

from asset_document.diagnostics import AssetDocumentContractError
from asset_document.diff.model import Difference, DiffKind
from asset_document.dto import PrimitiveRequirement
from asset_document.meta import (
    ComparisonPolicy, DefaultPolicy, DeletePolicy, IdentityPolicy, OmissionPolicy, OrderPolicy, ProfileMeta,
    RegionMeta, RegionOwnership,
)
from asset_document.mutation_intent_projection import SNAPSHOT_COVERED
from asset_document.profile_runtime import RuntimeProfileRegistration, RuntimeTargetPolicy
from asset_document.profiles import data_table_common as common
from asset_document.regions.primitive_catalog import NamedOpaqueHandleArgument, _single_handle
from asset_document.regions.target_lifecycle import generic_factory_lifecycle, lifecycle_requirements

ROUTE = "audio_profile"
PROVIDER = "audio"
SOURCE_CLASS = "/Script/MetasoundEngine.MetaSoundSource"
PATCH_CLASS = "/Script/MetasoundEngine.MetaSoundPatch"
PROFILES = {SOURCE_CLASS: "profile.metasound_source.v1", PATCH_CLASS: "profile.metasound_patch.v1"}
READ = "audio.metasound.read.v1"
CATALOG = "audio.metasound.catalog.v1"
EDIT = "audio.metasound.edit.v1"
COMPILE = "audio.metasound.compile.v1"
PREFIX = "MetaSound"
DEFAULT_PAGE = "Default"
BODY_KEYS = ("Interfaces", "Metadata", "Inputs", "Outputs", "Graphs", "Preset")
METADATA_KEYS = ("DisplayName", "Description", "Author", "Keywords", "Categories", "Referenceable", "Deprecated")
INPUT_KEYS = ("Type", "Default", "PageDefaults", "Constructor", "DisplayName", "Description", "SortOrder", "Advanced",
              "Widget", "RenamedFrom")
OUTPUT_KEYS = ("Type", "Constructor", "DisplayName", "Description", "SortOrder", "Advanced", "RenamedFrom")
PAGE_KEYS = ("Nodes", "Variables", "Comments")
NODE_KEYS = ("Class", "Member", "Version", "Id", "Position", "Hidden", "Inputs", "Defaults", "Configuration", "Comment",
             "CommentVisible", "HideUnconnectedPins")
VARIABLE_KEYS = ("Type", "Default", "DisplayName", "Description", "RenamedFrom")
COMMENT_KEYS = ("Id", "Text", "Position", "Size", "Color", "FontSize", "ColorBubble", "MoveMode")
MEMBER_CLASSES = ("Input", "Output", "Get", "GetDelayed", "Set")
REROUTE = "UE.Reroute"
TRIGGER = "Trigger"
NODE_SEED = "AssetDocument.MetaSound.Node:"
COMMENT_SEED = "AssetDocument.MetaSound.Comment:"
METADATA_DEFAULTS = {"DisplayName": "", "Description": "", "Author": "", "Keywords": [], "Categories": [],
                     "Referenceable": False, "Deprecated": False}


def _requirements(*primitives):
    return tuple(PrimitiveRequirement(item, 1, ROUTE) for item in primitives)


# A preset is created by the factory with its parent set (the Content Browser's "Create Preset"): the optional
# factory context is Body.Preset.Parent.
LIFECYCLE = dataclasses.replace(generic_factory_lifecycle(ROUTE, ("factory_context",)),
                                optional_create_context_handle_names=("factory_context",))
REQUIREMENTS = (_requirements(common.CLASS_RESOLVE, common.RESOLVE, common.REFERENCE_RESOLVE, READ, CATALOG, EDIT, COMPILE)
                + lifecycle_requirements(LIFECYCLE))
BY_ID = {item.primitive_id: item for item in REQUIREMENTS}
# The document of the MetaSound is the Body; these are never generic Properties.
BODY_OWNED = {"RootMetasoundDocument": "Body", "RootMetaSoundDocument": "Body"}
# Derived state the editor recomputes (design §2 "Derived"): refused as Properties, never extracted.
NOT_AUTHORED = ("ReferencedAssetClassKeys", "ReferencedAssetClassObjects", "ReferenceAssetClassCache",
                "QualitySettingGuid", "EditorGraph", "Graph", "AssetClassID", "RegistryInputTypes",
                "RegistryOutputTypes", "RegistryVersionMajor", "RegistryVersionMinor", "bIsPreset", "ThumbnailInfo",
                # A Source's looping and duration follow its interfaces (UMetaSoundSource::PostLoad: a Source without
                # UE.Source.OneShot loops). The editor sets them on load, so they are not the document's: extracting
                # them made BaseRevision change when the asset was loaded again.
                "bLooping", "Duration")
_DERIVED_FROM_INTERFACES = {"bLooping", "Duration"}
_KEY_CHARACTERS = re.compile(r"[^A-Za-z0-9_-]")
# Facts of the last read of each target that the Body does not carry (Diff uses them): node outputs by node ID
# (a bare "<key>" source names the node's only output) and the names of interface members.
_LIVE_FACTS = {}


def error(path, message, code):
    return common.error(path, message, code)


class AudioCalls(common.DataCalls):
    """Calls to the "audio" provider against one MetaSound (editor handles handed off)."""

    def __init__(self, context, read, mutable=None):
        self.context = context
        self.by_id = BY_ID
        self.prefix = PREFIX
        self.read = context.handoff(source_handle=read, target_provider_id=PROVIDER, access="read_only")
        self.write = (context.handoff(source_handle=mutable, target_provider_id=PROVIDER, access="mutable")
                      if mutable is not None else None)


def keyed_id(seed):
    """The node / comment ID of a document entry without ``Id``: a GUID (32 hex digits) derived from the key, so an
    AI-written document keeps matching the nodes its first apply created."""
    digest = hashlib.sha1(seed.encode("utf-8")).digest()
    return "".join(f"{int.from_bytes(digest[index:index + 4], 'little'):08X}" for index in (0, 4, 8, 12))


def node_id(key, entry):
    return entry["Id"] if type(entry.get("Id")) is str else keyed_id(NODE_SEED + key)


def comment_id(key, entry):
    return entry["Id"] if type(entry.get("Id")) is str else keyed_id(COMMENT_SEED + key)


def match_nodes(authored, live):
    """{authored key: live key}: an ``Output`` entry is its output's vertex node (by ``Member``); other entries by
    ``Id``, else by the keyed ID, else by an equal key of the same class whose node no entry names by ``Id``."""
    by_id = {entry.get("Id"): key for key, entry in live.items()}
    outputs = {entry.get("Member"): key for key, entry in live.items() if entry.get("Class") == "Output"}
    named = {entry["Id"] for entry in authored.values() if type(entry.get("Id")) is str}
    matched, used = {}, set()
    for key, entry in authored.items():
        found = outputs.get(entry.get("Member")) if entry.get("Class") == "Output" else by_id.get(node_id(key, entry))
        if found is not None and found not in used:
            matched[key] = found
            used.add(found)
    for key, entry in authored.items():
        if key in matched or entry.get("Class") == "Output" or type(entry.get("Id")) is str:
            continue
        other = live.get(key)
        if other is not None and key not in used and other.get("Id") not in named and other.get("Class") == entry.get("Class") \
                and other.get("Member") == entry.get("Member"):
            matched[key] = key
            used.add(key)
    return matched


def match_comments(authored, live):
    """{authored key: live key} by ``Id`` (or the keyed ID)."""
    by_id = {entry.get("Id"): key for key, entry in live.items()}
    return {key: by_id[comment_id(key, entry)] for key, entry in authored.items() if comment_id(key, entry) in by_id}


_MISSING = object()


def same_defaults(authored, live, class_defaults, exact):
    """Node literals: an omitted pin holds the class default (known from a read with pins); pin names compare without
    case (FNames)."""
    authored = {pin.casefold(): value for pin, value in authored.items()}
    live = {pin.casefold(): value for pin, value in live.items()}
    class_defaults = {pin.casefold(): value for pin, value in class_defaults.items()}
    for pin in set(authored) | set(live):
        left = authored.get(pin, class_defaults.get(pin, _MISSING))
        right = live.get(pin, class_defaults.get(pin, _MISSING))
        if left is _MISSING or right is _MISSING:
            if left is not right:
                return False
            continue
        if not common.same(left, right, exact):
            return False
    return True


# ---------------------------------------------------------------------------
# Document shape
# ---------------------------------------------------------------------------
def _object(value, path, what):
    if type(value) is not dict:
        raise error(path, f"{what} must be an object.", "MetaSound.InvalidBody")
    return value


def _keys(value, allowed, path, what):
    for key in value:
        if key not in allowed:
            raise error(f"{path}/{common.escape(key)}", f"Unknown {what} key '{key}'. Keys: {', '.join(allowed)}.",
                        "MetaSound.UnknownKey")


def _validate_body(body):
    path = "/Body"
    if "Interfaces" in body:
        if type(body["Interfaces"]) is not list or any(type(item) is not str or not item for item in body["Interfaces"]):
            raise error(f"{path}/Interfaces", "Interfaces is a list of interface names.", "MetaSound.InvalidBody")
    if "Metadata" in body:
        _keys(_object(body["Metadata"], f"{path}/Metadata", "Metadata"), METADATA_KEYS, f"{path}/Metadata", "Metadata")
    for region, allowed in (("Inputs", INPUT_KEYS), ("Outputs", OUTPUT_KEYS)):
        if region in body:
            for name, entry in _object(body[region], f"{path}/{region}", region).items():
                where = f"{path}/{region}/{common.escape(name)}"
                _keys(_object(entry, where, "A member"), allowed, where, "member")
    if "Graphs" in body:
        for page, graph in _object(body["Graphs"], f"{path}/Graphs", "Graphs").items():
            where = f"{path}/Graphs/{common.escape(page)}"
            _keys(_object(graph, where, "A page"), PAGE_KEYS, where, "page")
            for region, allowed in (("Nodes", NODE_KEYS), ("Variables", VARIABLE_KEYS), ("Comments", COMMENT_KEYS)):
                for key, entry in _object(graph.get(region, {}), f"{where}/{region}", region).items():
                    entry_path = f"{where}/{region}/{common.escape(key)}"
                    _keys(_object(entry, entry_path, "An entry"), allowed, entry_path, region[:-1].lower())
                    if region == "Nodes" and type(entry.get("Class")) is not str:
                        raise error(f"{entry_path}/Class", "A node names its Class.", "MetaSound.InvalidBody")
    if "Preset" in body and body["Preset"] is not None:
        preset = _object(body["Preset"], f"{path}/Preset", "Preset")
        _keys(preset, ("Parent",), f"{path}/Preset", "Preset")
        if type(preset.get("Parent")) is not str:
            raise error(f"{path}/Preset/Parent", "Preset.Parent is the parent MetaSound's object path.",
                        "MetaSound.InvalidBody")


def canonicalize(document):
    if type(document) is not dict or document.get("Class") not in PROFILES:
        raise error("/Class", f"Class must be {' or '.join(PROFILES)}.", "MetaSound.ClassMismatch")
    body = common.validate_header(document, tuple(PROFILES), PREFIX, BODY_KEYS)
    _validate_body(body)
    canonical = common.snapshot(document)
    canonical.setdefault("Definitions", {})
    canonical.setdefault("Properties", {})
    canonical.setdefault("Body", {})
    preset = canonical["Body"].get("Preset")
    if type(preset) is dict and type(preset.get("Parent")) is str:
        preset["Parent"] = object_path(preset["Parent"])
    return canonical


def object_path(path):
    """/Game/A/MSS_X -> /Game/A/MSS_X.MSS_X (an object path is kept)."""
    if not path or "." in path.rsplit("/", 1)[-1]:
        return path
    return f"{path}.{path.rsplit('/', 1)[-1]}"


PRESET_DERIVED = ("Interfaces", "Outputs", "Graphs")


def is_conversion_only(body):
    """``"Preset": null`` without the converted graph (Graphs / Interfaces / Outputs): Convert From Preset of the
    preset's overrides."""
    return "Preset" in body and body["Preset"] is None and not any(region in body for region in PRESET_DERIVED)


def check_preset_body(body, live_inputs=None):
    """A preset Body: Graphs / Interfaces / Outputs follow the parent; Inputs list overrides (Default, PageDefaults)."""
    for region in PRESET_DERIVED:
        if region in body:
            raise error(f"/Body/{region}", f"A preset's {region} follow its parent (the editor shows them read-only); "
                        "leave them out, edit the parent, or convert the preset with \"Preset\": null.",
                        "MetaSound.PresetGraphDerived")
    for name, entry in body.get("Inputs", {}).items():
        where = f"/Body/Inputs/{common.escape(name)}"
        if live_inputs is not None and name not in live_inputs:
            raise error(where, f"The parent has no input '{name}'; a preset overrides the parent's inputs "
                        f"({', '.join(sorted(live_inputs)[:40])}).", "MetaSound.UnknownMember")
        for key in entry:
            if key not in ("Default", "PageDefaults"):
                raise error(f"{where}/{common.escape(key)}", f"A preset input lists only its overriding Default and "
                            f"PageDefaults; {key} "
                            "follows the parent.", "MetaSound.PresetGraphDerived")


# ---------------------------------------------------------------------------
# Read records -> document
# ---------------------------------------------------------------------------
def read_records(calls, pins=False):
    """Every record of audio.metasound.read.v1 (header first)."""
    records = []
    offset = 0
    while True:
        page = calls.query(READ, {"offset": offset, "pins": pins} if pins else {"offset": offset})
        records += page.get("records", [])
        offset = page.get("next_offset", -1)
        if type(offset) is not int or offset < 0:
            break
    if not records or records[0].get("kind") != "header":
        raise error("/Body", f"{READ} returned no header.", "MetaSound.InvalidPrimitiveResult")
    return records


def sanitize_key(text):
    key = _KEY_CHARACTERS.sub("_", text or "")
    return key or "Node"


def _class_base(record):
    """Key base of a node that is not a member node: the class Name part (asset classes: the asset name)."""
    cls = record.get("class", "")
    if cls.startswith("/"):
        return cls.rsplit(".", 1)[-1]
    parts = record.get("class_name", cls).split(".")
    return parts[1] if len(parts) >= 2 else parts[0]


def node_keys(nodes):
    """{node id: key} for the authored nodes of one page, in node-ID order (the records' order)."""
    keys, used = {}, {}
    for record in nodes:
        base = sanitize_key(record["member"] if record["class"] in MEMBER_CLASSES and record.get("member")
                            else _class_base(record))
        count = used.get(base, 0) + 1
        used[base] = count
        key = base if count == 1 else f"{base}_{count}"
        while key in keys.values():
            count += 1
            used[base] = count
            key = f"{base}_{count}"
        keys[record["id"]] = key
    return keys


def _point(value):
    return [value[0], value[1]] if type(value) is list and len(value) == 2 else value


def _member_entry(record, interface_def, with_defaults):
    """Inputs / Outputs entry; interface members only list the fields that differ from the interface."""
    entry = {}
    pages = {item["page"]: item["value"] for item in record.get("defaults", [])} if with_defaults else {}
    if record.get("type") == TRIGGER:
        # A Trigger input has no value: a new one gets the registry's default literal (false), older content none.
        pages = {page: value for page, value in pages.items() if value is not False}
    default = pages.pop(DEFAULT_PAGE, None)
    if interface_def is not None:
        if with_defaults and "default" in interface_def and default is not None \
                and not common.same(default, interface_def["default"], exact=True):
            entry["Default"] = default
        if pages:
            entry["PageDefaults"] = pages
        if record["sort_order"] != interface_def.get("sort_order"):
            entry["SortOrder"] = record["sort_order"]
        if record["advanced"] != interface_def.get("advanced"):
            entry["Advanced"] = record["advanced"]
        if record.get("widget"):
            entry["Widget"] = record["widget"]
        return entry
    entry["Type"] = record["type"]
    if with_defaults and default is not None:
        entry["Default"] = default
    if pages:
        entry["PageDefaults"] = pages
    if record["constructor"]:
        entry["Constructor"] = True
    if record.get("display_name"):
        entry["DisplayName"] = record["display_name"]
    if record.get("description"):
        entry["Description"] = record["description"]
    entry["SortOrder"] = record["sort_order"]
    if record["advanced"]:
        entry["Advanced"] = True
    if record.get("widget"):
        entry["Widget"] = record["widget"]
    return entry


def _node_entry(record, sources):
    entry = {"Class": record["class"]}
    if record["class"] in MEMBER_CLASSES:
        entry["Member"] = record.get("member", "")
    elif record.get("major", 1) != 1:
        entry["Version"] = record["major"]
    entry["Id"] = record["id"]
    if "location" in record:
        entry["Position"] = _point(record["location"])
    else:
        entry["Hidden"] = True
    if sources:
        entry["Inputs"] = sources
    if record.get("defaults"):
        entry["Defaults"] = dict(record["defaults"])
    if record.get("configuration", {}).get("properties"):
        entry["Configuration"] = {"Type": record["configuration"]["type"],
                                  "Properties": record["configuration"]["properties"]}
    if record.get("comment"):
        entry["Comment"] = record["comment"]
    if record.get("comment_visible"):
        entry["CommentVisible"] = True
    if record.get("hide_unconnected"):
        entry["HideUnconnectedPins"] = True
    return entry


def _comment_entry(record):
    entry = {"Id": record["id"], "Text": record["text"], "Position": _point(record["position"]),
             "Size": _point(record["size"]), "Color": record["color"], "FontSize": record["font_size"]}
    if record.get("color_bubble"):
        entry["ColorBubble"] = True
    if record.get("move_mode"):
        entry["MoveMode"] = record["move_mode"]
    return entry


def assemble(records, pins=False):
    """(body, facts, keys) from the read records.  facts: {"outputs": {node id: [pins]}, "interface_inputs",
    "interface_outputs", "fixed_interfaces", "injected", "pins": {page: {key: {...}}}, "class_defaults": {node id:
    {pin: literal}} (with pins)}; keys: {page: {node id: key}}."""
    header = records[0]
    by_kind = {}
    for record in records[1:]:
        by_kind.setdefault(record["kind"], []).append(record)
    facts = {"outputs": {}, "interface_inputs": set(), "interface_outputs": set(), "injected": [], "pins": {},
             "configuration_defaults": {}, "pages": {}, "widget_defaults": {},
             "fixed_interfaces": {item["name"] for item in header.get("interfaces", []) if not item.get("modifiable")}}
    if pins:
        facts["class_defaults"] = {}
    body = {}
    preset = header.get("preset") is True
    if not preset:
        body["Interfaces"] = sorted(item["name"] for item in header.get("interfaces", []) if item.get("modifiable"))
    metadata = header.get("metadata", {})
    meta = {}
    for key, field in (("DisplayName", "display_name"), ("Description", "description"), ("Author", "author"),
                       ("Keywords", "keywords"), ("Categories", "categories")):
        if metadata.get(field):
            meta[key] = metadata[field]
    meta["Referenceable"] = metadata.get("referenceable") is True
    if metadata.get("deprecated"):
        meta["Deprecated"] = True
    body["Metadata"] = meta

    inherits = set(header.get("inherits", []))
    inputs = {}
    for record in by_kind.get("input", []):
        if record.get("interface"):
            facts["interface_inputs"].add(record["name"])
        if type(record.get("widget_defaults")) is dict:
            facts["widget_defaults"][record["name"]] = record["widget_defaults"]
        if preset:
            if record["name"] in inherits:
                continue
            values = {item["page"]: item["value"] for item in record.get("defaults", [])}
            entry = {}
            if values.get(DEFAULT_PAGE) is not None:
                entry["Default"] = values.pop(DEFAULT_PAGE)
            else:
                values.pop(DEFAULT_PAGE, None)
            if values:
                entry["PageDefaults"] = values
            inputs[record["name"]] = entry
            continue
        entry = _member_entry(record, record.get("interface_def") if record.get("interface") else None, True)
        if record.get("interface") and not entry:
            continue
        inputs[record["name"]] = entry
    body["Inputs"] = inputs
    if not preset:
        outputs = {}
        for record in by_kind.get("output", []):
            if record.get("interface"):
                facts["interface_outputs"].add(record["name"])
            entry = _member_entry(record, record.get("interface_def") if record.get("interface") else None, False)
            if record.get("interface") and not entry:
                continue
            outputs[record["name"]] = entry
        body["Outputs"] = outputs

    keys_by_page = {}
    graphs = {}
    page_names = [page["name"] for page in header.get("pages", [])]
    for page in page_names:
        nodes = [record for record in by_kind.get("node", []) if record["page"] == page]
        authored = [record for record in nodes if not record.get("derived")]
        keys = node_keys(authored)
        keys_by_page[page] = keys
        all_nodes = {record["id"]: record for record in nodes}
        # Pages copy each other's nodes (AddGraphPage duplicates a page), so node IDs repeat across pages: the facts
        # of a node are per page (page_facts); the flat maps hold the first page's (Default's) node of an ID.
        here = facts["pages"].setdefault(page, {"outputs": {}, "configuration_defaults": {}, "class_defaults": {}})
        for record in nodes:
            here["outputs"][record["id"]] = list(record.get("outputs", []))
            facts["outputs"].setdefault(record["id"], here["outputs"][record["id"]])
            if record.get("configuration", {}).get("defaults"):
                here["configuration_defaults"][record["id"]] = dict(record["configuration"]["defaults"])
                facts["configuration_defaults"].setdefault(record["id"], here["configuration_defaults"][record["id"]])
        sources = {}
        for edge in by_kind.get("edge", []):
            if edge["page"] != page or edge.get("derived") or edge["to"] not in keys:
                continue
            origin = all_nodes.get(edge["from"])
            if origin is None:
                continue
            if edge["from"] in keys:
                key = keys[edge["from"]]
                source = key if len(origin.get("outputs", [])) == 1 else f"{key}.{edge['from_pin']}"
            else:
                # An input connected without an input template node (a page the open-time injection does not touch).
                source = f"Input:{origin.get('member', '')}"
            sources.setdefault(edge["to"], {})[edge["to_pin"]] = source
        graph_nodes = {}
        for record in authored:
            graph_nodes[keys[record["id"]]] = _node_entry(record, sources.get(record["id"], {}))
            if pins and "pins" in record:
                facts["pins"].setdefault(page, {})[keys[record["id"]]] = record["pins"]
                here["class_defaults"][record["id"]] = {pin["name"]: pin["default"]
                                                        for pin in record["pins"].get("inputs", []) if "default" in pin}
                facts["class_defaults"].setdefault(record["id"], here["class_defaults"][record["id"]])
        variables = {}
        for record in by_kind.get("variable", []):
            if record["page"] != page:
                continue
            entry = {"Type": record["type"]}
            if record.get("default") is not None:
                entry["Default"] = record["default"]
            if record.get("display_name"):
                entry["DisplayName"] = record["display_name"]
            if record.get("description"):
                entry["Description"] = record["description"]
            variables[record["name"]] = entry
        comments = {}
        used = {}
        for record in by_kind.get("comment", []):
            if record["page"] != page:
                continue
            count = used.get("Comment", 0) + 1
            used["Comment"] = count
            comments["Comment" if count == 1 else f"Comment_{count}"] = _comment_entry(record)
        graphs[page] = {"Nodes": graph_nodes, "Variables": variables, "Comments": comments}
    if not preset:
        body["Graphs"] = graphs
    else:
        body["Preset"] = {"Parent": header.get("preset_parent", "")}
    injected = set(header.get("injected", []))
    for page, keys in keys_by_page.items():
        facts["injected"] += [keys[node] for node in sorted(injected) if node in keys]
    return body, facts, keys_by_page


def page_facts(facts, page):
    """The facts with the node maps (outputs, class and configuration defaults) of one page."""
    here = facts.get("pages", {}).get(page)
    if here is None:
        return facts
    merged = {**facts, "outputs": here["outputs"], "configuration_defaults": here["configuration_defaults"]}
    if "class_defaults" in facts:
        merged["class_defaults"] = here["class_defaults"]
    return merged


class _MetaSoundProperties(common.PropertiesRegion):
    """Details-panel settings of the MetaSound (generic property admission)."""

    def check_owned(self, properties):
        for name in sorted(properties):
            if name in _DERIVED_FROM_INTERFACES:
                raise error(f"/Properties/{common.escape(name)}", f"'{name}' follows the Source's Interfaces (a Source "
                            "without UE.Source.OneShot loops), not authored; leave it out and add or remove "
                            "UE.Source.OneShot in Body.Interfaces.", "MetaSound.NotAuthoredProperty")
            if name in NOT_AUTHORED:
                raise error(f"/Properties/{common.escape(name)}", f"'{name}' is derived by the MetaSound editor "
                            "(registration, versioning or the asset registry), not authored; leave it out.",
                            "MetaSound.NotAuthoredProperty")
        super().check_owned(properties)

    def skipped(self):
        # The document payload (the Body comes from the MetaSound read) and the derived state are never read.
        return super().skipped() | set(NOT_AUTHORED)


def _properties(class_path):
    return _MetaSoundProperties(class_path, BODY_OWNED, PREFIX)


def extract(target, class_path, read, context, include_defaults=False):
    """include_defaults (verification and Diff) also reads the class defaults of the node pins, so a document
    that lists a literal equal to the class default compares equal."""
    calls = AudioCalls(context, read)
    body, facts, _ = assemble(read_records(calls, pins=include_defaults), pins=include_defaults)
    _LIVE_FACTS[target] = facts
    return common.document(target, class_path, _properties(class_path).extract(read, context, include_defaults), body)


# ---------------------------------------------------------------------------
# Diff
# ---------------------------------------------------------------------------
def _change(changes, path, authored, live, authored_present=True, live_present=True):
    changes.append(Difference(path, DiffKind.REPLACE, authored_present, authored, live_present, live))


_MEMBER_DEFAULTS = {"Constructor": False, "Advanced": False, "DisplayName": "", "Description": "", "Widget": {},
                    "PageDefaults": {}}
# Omitted member fields that keep the live value (§3.5): the default value and the order.
MEMBER_KEPT = ("Default", "SortOrder", "RenamedFrom")


def _member_diff(authored, live, path, interface_names, exact, changes, widget_defaults=None):
    """``widget_defaults``: {member: every Widget setting of a new settings object} (an input's Widget is complete
    over them: an omitted setting is written as its default, so a listed default reads back omitted)."""
    live = live or {}
    widget_defaults = widget_defaults or {}
    for name in sorted(set(live) - set(authored)):
        if name not in interface_names:
            changes.append(Difference(f"{path}/{common.escape(name)}", DiffKind.REMOVE, True, None, True, live[name]))
    for name, entry in authored.items():
        where = f"{path}/{common.escape(name)}"
        if name not in live:
            if name in interface_names and all(key in ("Type",) for key in entry):
                continue
            changes.append(Difference(where, DiffKind.ADD, True, entry, False, None))
            continue
        before = live[name]
        for key in sorted(set(entry) | set(before)):
            if key in MEMBER_KEPT and key not in entry:
                continue
            if key == "RenamedFrom":
                continue
            if name in interface_names and key not in entry:
                continue
            left = entry.get(key, _MEMBER_DEFAULTS.get(key))
            right = before.get(key, _MEMBER_DEFAULTS.get(key))
            if key == "Widget" and type(left) is dict and type(right) is dict:
                settings = widget_defaults.get(name, {})
                left, right = {**settings, **left}, {**settings, **right}
            if not common.same(left, right, exact):
                if key == "PageDefaults" and type(left) is dict and type(right) is dict:
                    # A complete map: a page the document leaves out loses its value (it uses Default again), a
                    # Remove entry that validate and diff count and name.
                    _page_defaults_diff(f"{where}/PageDefaults", left, right, exact, changes)
                else:
                    _change(changes, f"{where}/{common.escape(key)}", entry.get(key), before.get(key), key in entry,
                            key in before)
                break


def _page_defaults_diff(path, authored, live, exact, changes):
    for page in sorted(set(live) - set(authored)):
        changes.append(Difference(f"{path}/{common.escape(page)}", DiffKind.REMOVE, False, None, True, live[page]))
    for page, value in authored.items():
        if page not in live:
            changes.append(Difference(f"{path}/{common.escape(page)}", DiffKind.ADD, True, value, False, None))
        elif not common.same(value, live[page], exact):
            _change(changes, f"{path}/{common.escape(page)}", value, live[page])


def _resolve_source(source, keys, outputs):
    """(node id or key, pin) of a source string; a bare key names the node's only output."""
    if type(source) is not str:
        return None
    if source.startswith("Input:"):
        return (source, None)
    key, _, pin = source.partition(".")
    node = keys.get(key, f"key:{key}")
    if not pin:
        pins = outputs.get(node, [])
        pin = pins[0] if len(pins) == 1 else None
    return (node, pin.casefold() if pin else pin)


_NODE_DEFAULTS = {"Inputs": {}, "Defaults": {}, "Comment": "", "CommentVisible": False, "HideUnconnectedPins": False,
                  "Version": 1}


def configuration_properties(entry, defaults=None):
    """The configuration a node entry asks for, as its Properties ({} = the class's default configuration); with the
    class's configuration ``defaults``, every setting (a value equal to the default is the same as leaving it out)."""
    value = entry.get("Configuration")
    properties = (value.get("Properties") or {}) if type(value) is dict else {}
    return {**defaults, **properties} if defaults else properties


def is_bare_output(entry):
    """A live output node the editor does not show: no location, no edge and no comment (an omitted Output entry)."""
    return entry.get("Class") == "Output" and "Position" not in entry and not entry.get("Inputs") \
        and not entry.get("Comment") and not entry.get("Configuration")


def _nodes_diff(authored, live, path, facts, exact, changes, removed_outputs=()):
    outputs = facts.get("outputs", {})
    class_defaults = facts.get("class_defaults", {})
    matched = match_nodes(authored, live)
    authored_keys = {key: live[matched[key]].get("Id") if key in matched else f"key:{key}" for key in authored}
    live_keys = {key: entry.get("Id") for key, entry in live.items()}
    for key in sorted(set(live) - set(matched.values())):
        entry = live[key]
        if is_bare_output(entry) or (entry.get("Class") == "Output" and entry.get("Member") in removed_outputs):
            continue
        changes.append(Difference(f"{path}/{common.escape(key)}", DiffKind.REMOVE, True, None, True, entry))
    for key, entry in authored.items():
        where = f"{path}/{common.escape(key)}"
        if key not in matched:
            changes.append(Difference(where, DiffKind.ADD, True, entry, False, None))
            continue
        before = live[matched[key]]
        for field in ("Class", "Member"):
            if entry.get(field) != before.get(field):
                _change(changes, f"{where}/{field}", entry.get(field), before.get(field), field in entry, field in before)
                break
        else:
            if entry.get("Version", 1) != before.get("Version", 1):
                _change(changes, f"{where}/Version", entry.get("Version"), before.get("Version"))
            if "Position" in entry:
                if not common.same(entry["Position"], before.get("Position"), exact):
                    _change(changes, f"{where}/Position", entry["Position"], before.get("Position"), True, "Position" in before)
            elif entry.get("Hidden") is True:
                if "Position" in before:
                    _change(changes, f"{where}/Hidden", True, None, True, False)
            elif "Position" not in before:
                # Omitting Hidden on a hidden node places it (§3.3).
                _change(changes, f"{where}/Position", None, None, False, False)
            # Pin names are FNames: they compare without case (an updated node class may spell them differently).
            mine = {pin.casefold(): (pin, source) for pin, source in entry.get("Inputs", {}).items()}
            theirs = {pin.casefold(): (pin, source) for pin, source in before.get("Inputs", {}).items()}
            for folded in sorted(set(mine) | set(theirs)):
                left = mine.get(folded, (None, None))
                right = theirs.get(folded, (None, None))
                if _resolve_source(left[1], authored_keys, outputs) != _resolve_source(right[1], live_keys, outputs):
                    _change(changes, f"{where}/Inputs/{common.escape(left[0] or right[0])}", left[1], right[1],
                            folded in mine, folded in theirs)
                    break
            if not same_defaults(entry.get("Defaults", {}), before.get("Defaults", {}),
                                 class_defaults.get(before.get("Id"), {}), exact):
                _change(changes, f"{where}/Defaults", entry.get("Defaults"), before.get("Defaults"), "Defaults" in entry,
                        "Defaults" in before)
            config_defaults = facts.get("configuration_defaults", {}).get(before.get("Id"))
            if not common.same(configuration_properties(entry, config_defaults), configuration_properties(before, config_defaults),
                               exact):
                _change(changes, f"{where}/Configuration", entry.get("Configuration"), before.get("Configuration"),
                        "Configuration" in entry, "Configuration" in before)
            for field in ("Comment", "CommentVisible", "HideUnconnectedPins"):
                left = entry.get(field, _NODE_DEFAULTS.get(field))
                right = before.get(field, _NODE_DEFAULTS.get(field))
                if not common.same(left, right, exact):
                    _change(changes, f"{where}/{field}", entry.get(field), before.get(field), field in entry, field in before)


def _entries_diff(authored, live, path, exact, changes, defaults=None, matched=None):
    defaults = defaults or {}
    matched = matched if matched is not None else {key: key for key in authored if key in live}
    for key in sorted(set(live) - set(matched.values())):
        changes.append(Difference(f"{path}/{common.escape(key)}", DiffKind.REMOVE, True, None, True, live[key]))
    for key, entry in authored.items():
        where = f"{path}/{common.escape(key)}"
        if key not in matched:
            if entry.get("RenamedFrom") in live and entry["RenamedFrom"] not in authored:
                _change(changes, f"{where}/RenamedFrom", entry["RenamedFrom"], None, True, False)
            else:
                changes.append(Difference(where, DiffKind.ADD, True, entry, False, None))
            continue
        before = live[matched[key]]
        for field in sorted(set(entry) | set(before)):
            if field in ("RenamedFrom", "Id") or (field not in entry and field not in defaults):
                continue
            left = entry.get(field, defaults.get(field))
            right = before.get(field, defaults.get(field))
            if not common.same(left, right, exact):
                _change(changes, f"{where}/{field}", entry.get(field), before.get(field), field in entry, field in before)
                break


def diff(authored, extracted, exact, facts=None):
    facts = facts if facts is not None else _LIVE_FACTS.get(extracted.get("Target"), {})
    changes = common.header_diff(authored, extracted)
    body = authored.get("Body", {})
    live = extracted.get("Body", {})
    if "Interfaces" in body:
        wanted = set(body["Interfaces"]) - set(facts.get("fixed_interfaces", ()))
        if sorted(wanted) != sorted(set(live.get("Interfaces", []))):
            _change(changes, "/Body/Interfaces", body["Interfaces"], live.get("Interfaces"), True, "Interfaces" in live)
    live_metadata = live.get("Metadata", {})
    for key, value in body.get("Metadata", {}).items():
        if not common.same(value, live_metadata.get(key, METADATA_DEFAULTS.get(key)), exact):
            _change(changes, f"/Body/Metadata/{key}", value, live_metadata.get(key), True, key in live_metadata)
    if "Inputs" in body and is_conversion_only(body) and live.get("Preset") is None:
        # Convert From Preset without the converted graph: the listed inputs' values were applied before the
        # conversion; the converted MetaSound's other members come from the parent.
        live_inputs = live.get("Inputs", {})
        for name, entry in body["Inputs"].items():
            where = f"/Body/Inputs/{common.escape(name)}"
            if name not in live_inputs:
                _change(changes, where, entry, None, True, False)
            elif "Default" in entry and not common.same(entry["Default"], live_inputs[name].get("Default"), exact):
                _change(changes, f"{where}/Default", entry["Default"], live_inputs[name].get("Default"), True,
                        "Default" in live_inputs[name])
    elif "Inputs" in body:
        # A preset lists overridden inputs only (interface inputs too): an unlisted one inherits.
        interface_inputs = set() if live.get("Preset") is not None else facts.get("interface_inputs", set())
        _member_diff(body["Inputs"], live.get("Inputs"), "/Body/Inputs", interface_inputs, exact, changes,
                     facts.get("widget_defaults"))
    removed_outputs = ()
    if "Outputs" in body:
        _member_diff(body["Outputs"], live.get("Outputs"), "/Body/Outputs", facts.get("interface_outputs", set()), exact,
                     changes)
        removed_outputs = set(live.get("Outputs", {})) - set(body["Outputs"]) - set(facts.get("interface_outputs", ()))
    if "Graphs" in body:
        live_graphs = live.get("Graphs", {})
        for page in sorted(set(live_graphs) - set(body["Graphs"]) - {DEFAULT_PAGE}):
            changes.append(Difference(f"/Body/Graphs/{common.escape(page)}", DiffKind.REMOVE, True, None, True,
                                      live_graphs[page]))
        for page, graph in body["Graphs"].items():
            where = f"/Body/Graphs/{common.escape(page)}"
            if page not in live_graphs:
                changes.append(Difference(where, DiffKind.ADD, True, graph, False, None))
                continue
            before = live_graphs[page]
            if "Nodes" in graph:
                _nodes_diff(graph["Nodes"], before.get("Nodes", {}), f"{where}/Nodes", page_facts(facts, page), exact, changes,
                            removed_outputs)
            if "Variables" in graph:
                _entries_diff(graph["Variables"], before.get("Variables", {}), f"{where}/Variables", exact, changes,
                              {"DisplayName": "", "Description": ""})
            if "Comments" in graph:
                comments = before.get("Comments", {})
                _entries_diff(graph["Comments"], comments, f"{where}/Comments", exact, changes,
                              {"ColorBubble": False}, match_comments(graph["Comments"], comments))
    if "Preset" in body and not common.same(body["Preset"], live.get("Preset"), exact=True):
        _change(changes, "/Body/Preset", body["Preset"], live.get("Preset"), True, "Preset" in live)
    changes.extend(common.properties_diff(authored.get("Properties", {}), extracted.get("Properties", {}), exact))
    return tuple(changes)


def body_changes(canonical, body, facts):
    """Differences of the authored Body from a live Body (assembled with its facts)."""
    live = {key: canonical.get(key) for key in ("SchemaVersion", "Target", "Class")}
    live["Body"] = body
    authored = {**canonical, "Properties": {}}
    return [item for item in diff(authored, {**live, "Properties": {}}, exact=False, facts=facts)
            if item.path.startswith("/Body")]


# ---------------------------------------------------------------------------
# Inspect
# ---------------------------------------------------------------------------
def _pin_text(pin, with_default):
    text = f"{pin['name']}: {pin['type']}"
    if pin.get("access") == "Value":
        text += " (constructor)"
    if with_default and pin.get("default") is not None:
        text += " = " + json.dumps(pin["default"], separators=(",", ":"), ensure_ascii=False)
    return text


def inspect_payload(document, calls, class_path):
    records = read_records(calls, pins=True)
    _, facts, _ = assemble(records, pins=True)
    header = records[0]
    pins = {}
    for page, nodes in facts["pins"].items():
        pins[page] = {key: {"Inputs": [_pin_text(pin, True) for pin in value.get("inputs", [])],
                            "Outputs": [_pin_text(pin, False) for pin in value.get("outputs", [])]}
                      for key, value in nodes.items()}
    declared = {item["name"] for item in header.get("interfaces", [])}
    available = []
    catalog = calls.query(CATALOG, {"kind": "interface", "limit": 400})
    for row in catalog.get("rows", []):
        classes = row.get("Classes", [])
        options = [item for item in classes if item.get("Class") == class_path]
        if row.get("Path") in declared or (classes and not options) or any(not item.get("Modifiable") for item in options):
            continue
        available.append(row["Path"])
    inspected = {**document, "Pins": pins,
                 "Interfaces": [{"Name": item["name"], "Version": item["version"], "Modifiable": item["modifiable"],
                                 "Registered": item["registered"]} for item in header.get("interfaces", [])],
                 "AvailableInterfaces": sorted(available),
                 "Pages": [item["name"] for item in header.get("pages", [])],
                 # Pages a document may add (Project Settings > MetaSound > Pages), and the page that plays.
                 "ConfiguredPages": list(header.get("configured_pages", [DEFAULT_PAGE])),
                 "RegisteredPage": header.get("registered_page", DEFAULT_PAGE),
                 "BuildPage": header.get("build_page", DEFAULT_PAGE)}
    if facts["injected"]:
        # Nodes the MetaSound editor adds when it opens this asset (shown, not yet saved in the asset).
        inspected["InjectedNodes"] = facts["injected"]
    return inspected


# ---------------------------------------------------------------------------
# Handler
# ---------------------------------------------------------------------------
class MetaSoundRuntimeProfile:
    runtime_entry_module = "asset_document/bootstrap.py"
    # Snapshot-covered: preflight never reads a preview.
    preflight_scratch = False

    def __init__(self, class_path, profile_id):
        self.exact_asset_class_path = class_path
        self.profile_id = profile_id

    def required_primitives(self):
        return REQUIREMENTS

    def inspect_profile(self):
        return {"Class": self.exact_asset_class_path, "BodySections": list(BODY_KEYS), "ReadOnlySections": [],
                "CreateContext": "Body.Preset.Parent"}

    def create_template(self, target):
        target = common.require_target(target, PREFIX)
        if self.exact_asset_class_path == SOURCE_CLASS:
            # A new Source has a mono output (Properties.OutputFormat Mono) and the UE.Source interface (On Play).
            out = "UE.OutputFormat.Mono.Audio:0"
            body = {"Inputs": {"Frequency": {"Type": "Float", "Default": 440.0}},
                    "Graphs": {DEFAULT_PAGE: {"Nodes": {
                        "Frequency": {"Class": "Input", "Member": "Frequency", "Position": [-500, 0]},
                        "Sine": {"Class": "UE.Sine.Audio", "Position": [-250, 0], "Inputs": {"Frequency": "Frequency"}},
                        "Out": {"Class": "Output", "Member": out, "Position": [0, 0], "Inputs": {out: "Sine.Audio"}}}}}}
        else:
            body = {"Inputs": {"In": {"Type": "Audio"}}, "Outputs": {"Out": {"Type": "Audio"}},
                    "Graphs": {DEFAULT_PAGE: {"Nodes": {
                        "In": {"Class": "Input", "Member": "In", "Position": [-300, 0]},
                        "Out": {"Class": "Output", "Member": "Out", "Position": [0, 0], "Inputs": {"Out": "In"}}}}}}
        return common.document(target, self.exact_asset_class_path, {}, body)

    def _canonical(self, document):
        canonical = canonicalize(document)
        if canonical["Class"] != self.exact_asset_class_path:
            raise error("/Class", f"Class must be {self.exact_asset_class_path}.", "MetaSound.ClassMismatch")
        return canonical

    def validate(self, document):
        self._canonical(document)

    def canonicalize(self, document):
        return self._canonical(document)

    def create_target_context(self, document, context):
        """The preset parent (Body.Preset.Parent), the MetaSound factory's context."""
        canonical = self._canonical(document)
        body = canonical.get("Body", {})
        if body.get("Preset") is None:
            return ()
        check_preset_body(body)
        parent = body["Preset"]["Parent"]
        try:
            outcome = common.invoke(context, BY_ID, common.REFERENCE_RESOLVE,
                                    {"expected_class": self.exact_asset_class_path, "path": parent},
                                    path="/Body/Preset/Parent", prefix=PREFIX)
        except AssetDocumentContractError as failure:
            raise error("/Body/Preset/Parent", f"Preset.Parent must be an existing {self.exact_asset_class_path.rsplit('.', 1)[-1]} "
                        f"(the editor makes presets of the same class); got {parent!r}. "
                        f"{failure.diagnostic.message}", "MetaSound.InvalidPresetParent") from failure
        handle = _single_handle(outcome, "asset")
        records = AudioCalls(context, handle).query(READ, {"offset": 0}).get("records", [])
        first = records[0] if records else {}
        if first.get("audio_device_required"):
            raise error("/Body/Preset/Parent", f"{parent} uses {first['audio_device_required']}; registering it without an audio "
                        "device (an editor started with -NoSound) would crash the editor (Harmonix FusionPatch). Run the editor "
                        "with audio to make a preset of it.", "MetaSound.AudioDeviceRequired")
        return (NamedOpaqueHandleArgument("factory_context", handle),)

    def _existing(self, document, context):
        """Read handle of the target, or None when it does not exist yet."""
        target = document["Target"]
        result = common.invoke(context, BY_ID, common.RESOLVE,
                               {"path": f"{target}.{target.rsplit('/', 1)[-1]}",
                                "expected_class": self.exact_asset_class_path}, prefix=PREFIX)
        value = result.value if type(result.value) is dict else {}
        if value.get("exists") is not True:
            return None
        return next((group.handles[0] for group in result.handle_results
                     if group.name == "asset_read" and group.handles), None)

    def preflight(self, document, scratch_handle, context):
        canonical = self._canonical(document)
        _properties(canonical["Class"]).validate(canonical.get("Properties", {}), context)
        read = self._existing(canonical, context)
        if read is not None:
            _apply.preflight(canonical, read, context)
        elif canonical.get("Body", {}).get("Preset") is not None:
            check_preset_body(canonical["Body"])
        return SNAPSHOT_COVERED

    def apply(self, document, read_asset_handle, mutable_asset_handle, context):
        return _apply.apply(self._canonical(document), read_asset_handle, mutable_asset_handle, context)

    def repair(self, document, asset_handle, context):
        self._canonical(document)
        return ()

    def extract(self, *, target, asset_handle, context, include_defaults=False):
        return extract(common.require_target(target, PREFIX), self.exact_asset_class_path, asset_handle, context,
                       include_defaults)

    def inspect(self, *, mode, exact_class_path, exact_asset_object_path, editor_asset_handle, context):
        if mode != "live_asset" or exact_class_path != self.exact_asset_class_path or editor_asset_handle is None:
            raise error("/Inspect", "MetaSound inspection reads a live MetaSound Source or Patch.",
                        "MetaSound.InspectNotAdmitted")
        document = self.canonicalize(self.extract(target=exact_asset_object_path.rsplit(".", 1)[0],
                                                  asset_handle=editor_asset_handle, context=context))
        return inspect_payload(document, AudioCalls(context, editor_asset_handle), self.exact_asset_class_path)

    def diff(self, authored, extracted):
        return diff(self._canonical(authored), self._canonical(extracted), exact=False)

    def diff_exact(self, authored, extracted):
        return diff(self._canonical(authored), self._canonical(extracted), exact=True)


def _meta(class_path, profile_id):
    rows = [("interfaces", "/Body/Interfaces"), ("metadata", "/Body/Metadata"), ("inputs", "/Body/Inputs"),
            ("outputs", "/Body/Outputs"), ("graphs", "/Body/Graphs"), ("preset", "/Body/Preset")]
    regions = tuple(RegionMeta(f"metasound.{name}.v1", path, RegionOwnership.MANAGED, IdentityPolicy.STABLE_KEY,
                               OrderPolicy.CANONICAL, DefaultPolicy.EXPLICIT, OmissionPolicy.PRESERVE,
                               DeletePolicy.EXPLICIT_ONLY, ComparisonPolicy.CANONICAL)
                    for name, path in rows)
    return ProfileMeta(profile_id, ROUTE, class_path, regions, REQUIREMENTS)


def _registration(class_path):
    profile_id = PROFILES[class_path]
    return RuntimeProfileRegistration(_meta(class_path, profile_id), lambda: MetaSoundRuntimeProfile(class_path, profile_id),
                                      RuntimeTargetPolicy.CREATE_OR_UPDATE, LIFECYCLE)


# Module-level on purpose (after everything metasound_apply uses from this module): the native runtime restores
# sys.modules between phase calls, so an in-call import would load a second copy of the primitive catalog and its
# PrimitiveOutcome class.
from asset_document.profiles import metasound_apply as _apply  # noqa: E402

METASOUND_SOURCE_RUNTIME_REGISTRATION = _registration(SOURCE_CLASS)
METASOUND_PATCH_RUNTIME_REGISTRATION = _registration(PATCH_CLASS)

__all__ = ("METASOUND_SOURCE_RUNTIME_REGISTRATION", "METASOUND_PATCH_RUNTIME_REGISTRATION", "MetaSoundRuntimeProfile",
           "assemble", "canonicalize", "diff", "keyed_id", "match_nodes", "node_keys", "sanitize_key")
