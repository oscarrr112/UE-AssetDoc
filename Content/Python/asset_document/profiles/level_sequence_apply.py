"""Level Sequence apply (profile.level_sequence.v1): the document as Sequencer's own edits (design §4.2, §4.3).

The Body is planned against one read of the sequence and written in ``seq.edit.v1`` op batches:

  0. the Director Blueprint (created if the document has one and the sequence not; its members and graphs through the
     Blueprint components, sequencer_director): event keys and director bindings name its endpoints
  1. structure: Settings (tick-resolution migration and rates first: every later frame is in the final rates) ->
     binding deletes -> binding renames -> binding creates (parents first) -> track deletes -> track creates (their
     object names come back for the next batches)
  2. content: binding kinds and content (locators, spawnable templates, custom bindings, tags) -> binding order ->
     track order -> per track: Details, then sections (deletes from the end, in-place edits, appends; header before
     channels); tracks that reference bindings inside sub-sequences last (their sub sections exist by then)
  3. the rest of dense channels (columnar chunks) -> section groups -> folders -> markers -> node groups -> MetaData
  4. the Director removed when the document says "Director": null
  5. the top-level Properties (the generic Details write); repair compiles the Director when it is not up to date

Identity (design §3.4.2, §3.5.2): bindings by ``Id`` (GUID), else ``Name``, else ``RenamedFrom`` among their
siblings; tracks by ``Id`` (object name), else (Type, Property, Name) in order within their scope; sections by
position. The status before and after (``seq.status.v1``) decides the outcome: new errors fail the apply
(``Sequencer.ValidationFailed``, rolled back by Core); errors the sequence already had and warnings come back as
warnings. Deleting a binding that other packages reference reports ``Sequencer.DependentsAffected`` (warn only).
"""
from __future__ import annotations

import copy
import re
import uuid

from asset_document.diagnostics import AssetDocumentContractError, DiagnosticSeverity, RuntimeDiagnostic, with_diagnostics
from asset_document.profiles import data_table_common as common
from asset_document.profiles import level_sequence as seq
from asset_document.profiles import sequencer_director as director
from asset_document.regions.primitive_catalog import NamedOpaqueHandleArgument

escape = seq.escape

# Node budget of the key columns one op carries (the rest of a dense channel follows as channel.append chunks).
KEY_NODE_BUDGET = 1500
# Node budget of a section entry or a parameters.append chunk without its keys (the rest of a long Parameters list
# follows as parameters.append ops). With the keys, one op stays under common.OP_NODE_BUDGET.
ENTRY_NODE_BUDGET = 1400
# A binding's kind and content as binding.set writes it (Root / Component: the root and the component bindings of a
# Template Sequence, template_sequence.py).
ENTRY_KEYS = ("Class", "Bind", "Spawnable", "CustomBinding", "References", "Root", "Component", "Tags")
TRACK_HEADER_KEYS = ("Property", "Name", "Muted", "MutedRows", "RowConditions", "Properties")
GUID = re.compile(r"^[0-9A-Fa-f]{32}$")


def error(path, message, code):
    return seq.error(path, message, code)


# ---------------------------------------------------------------------------
# Transport
# ---------------------------------------------------------------------------
class Op:
    """One edit op with the document path its errors are reported at: ``fields`` maps an op field to a path,
    ``values`` an op field holding a document object to the path of that object (nested errors keep their tail).
    ``created`` receives the object name of the track / section the op adds."""

    def __init__(self, op, path, fields=None, values=None, created=None):
        self.op = op
        self.path = path
        self.fields = fields or {}
        self.values = values or {}
        self.created = created


def op_path(primitive_path, ops):
    """/ops/<n>/<field>/... -> the document path of op n."""
    parts = [part for part in (primitive_path or "").split("/") if part]
    if len(parts) >= 2 and parts[0] == "ops" and parts[1].isdigit() and int(parts[1]) < len(ops):
        item = ops[int(parts[1])]
        rest = parts[2:]
        for size in range(len(rest), 0, -1):
            key = "/".join(rest[:size])
            if key in item.fields:
                return item.fields[key]
            if key in item.values:
                return item.values[key] + "".join(f"/{part}" for part in rest[size:])
        return item.path
    return "/Body"


class Edits:
    """Runs op batches; collects the changed set, the created object names and the dependents report."""

    def __init__(self, calls):
        self.calls = calls
        self.changed = []
        self.dependents = []

    def run(self, ops):
        if not ops:
            return False
        for chunk in common.chunk_ops([(item.op, item) for item in ops]):
            items = [item for _, item in chunk]

            def where(primitive_path, items=items):
                return op_path(primitive_path, items)
            outcome = common.invoke(self.calls.context, seq.BY_ID, seq.EDIT,
                                    {"schema_version": 1, "ops": [op for op, _ in chunk]},
                                    (NamedOpaqueHandleArgument("asset", self.calls.write),), paths=where,
                                    prefix=seq.PREFIX)
            self.changed.extend(outcome.changed_set)
            value = outcome.value if type(outcome.value) is dict else {}
            self.dependents.extend(item for item in value.get("dependents", []) if type(item) is dict)
            for row in value.get("created", []):
                index = row.get("op") if type(row) is dict else None
                if type(index) is int and 0 <= index < len(items) and items[index].created is not None:
                    items[index].created(row.get("object", ""))
        return True


def status(calls):
    return calls.query(seq.STATUS).get("compile", {})


# ---------------------------------------------------------------------------
# Channels: dense keys split into op-sized columnar chunks
# ---------------------------------------------------------------------------
def _columns(keys):
    """Key objects -> the columnar form (Values hold Value or Event)."""
    columns = {"Frames": [key.get("Frame") for key in keys],
               "Values": [key.get("Value", key.get("Event")) for key in keys]}
    if any("Interp" in key for key in keys):
        columns["Interp"] = [key.get("Interp", "SmartAuto") for key in keys]
    for name in seq.TANGENT_KEYS:
        if any(name in key for key in keys):
            columns[name] = [key.get(name) for key in keys]
    return columns


def _key_nodes(key):
    return sum(common.count_nodes(value) for value in key.values())


def _width(keys):
    """Columns of the columnar form of these keys."""
    return len(_columns(keys)) if keys else 0


def _cost(keys, width):
    """Nodes the keys take in either written form (key objects, or ``width`` columns), an upper bound."""
    return width + sum(max(1 + _key_nodes(key), width) for key in keys)


def _chunks(keys, budget):
    """Consecutive key lists each under the node budget (at least one key each)."""
    width = _width(keys)
    chunks, current, size = [], [], width
    for key in keys:
        nodes = max(1 + _key_nodes(key), width)
        if current and size + nodes > budget:
            chunks.append(current)
            current, size = [], width
        current.append(key)
        size += nodes
    if current:
        chunks.append(current)
    return chunks


def _frame_order(key):
    frame = key.get("Frame")
    return (0, frame) if type(frame) in (int, float) else (1, 0)


def _is_stated(key):
    """A key whose tangents are kept as listed although Sequencer computes them (Arrive / Leave on a key other than
    User / Break)."""
    return key.get("Interp", "SmartAuto") not in ("User", "Break") and ("Arrive" in key or "Leave" in key)


def _runs(indices):
    """Sorted indices -> [start, count] runs."""
    runs = []
    for index in indices:
        if runs and runs[-1][0] + runs[-1][1] == index:
            runs[-1][1] += 1
        else:
            runs.append([index, 1])
    return runs


class _Split:
    """Splits the channels of a section (and of its Parameters entries) under one op budget."""

    def __init__(self, budget=KEY_NODE_BUDGET):
        self.budget = budget
        self.appends = []

    def channels(self, channels, parameter):
        written = {}
        for name, channel in channels.items():
            if type(channel) is not dict or ("Keys" not in channel and "Frames" not in channel):
                written[name] = channel
                continue
            keys = [key for key in seq.channel_keys(channel) if type(key) is dict]
            head = {key: value for key, value in channel.items() if key not in seq.COLUMNS and key != "Keys"}
            chunks = _chunks(keys, KEY_NODE_BUDGET)
            if len(chunks) > 1:
                # Written in several ops: in time order, so the keys already written are a prefix of the channel and
                # "stated" can name them by index.
                keys = sorted(keys, key=_frame_order)
                chunks = _chunks(keys, KEY_NODE_BUDGET)
            first = chunks[0] if chunks else []
            form = ({"Keys": first} if len(first) <= 64 and len(chunks) == 1 else _columns(first)) if first else {}
            cost = _cost(first, _width(keys)) if first else 0
            if cost > self.budget:
                first, form, cost = [], {}, 0
            else:
                chunks = chunks[1:]
            self.budget -= cost
            head.update(form)
            written[name] = head
            done = len(first)
            for chunk in chunks:
                stated = _runs([index for index, key in enumerate(keys[:done]) if _is_stated(key)])
                self.appends.append((name, parameter, _columns(chunk), stated))
                done += len(chunk)
        return written

    def parameters(self, parameters, path, start=0):
        if type(parameters) is not list:
            return parameters
        result = []
        for index, parameter in enumerate(parameters, start):
            if type(parameter) is dict:
                parameter = dict(parameter)
                if type(parameter.get("Channels")) is dict:
                    parameter["Channels"] = self.channels(parameter["Channels"], path + [index])
                if "Parameters" in parameter:
                    parameter["Parameters"] = self.parameters(parameter["Parameters"], path + [index])
            result.append(parameter)
        return result


def _skeleton_nodes(entry):
    """Nodes of a section or parameter entry without its channel keys."""
    split = _Split(0)
    entry = dict(entry)
    if type(entry.get("Channels")) is dict:
        entry["Channels"] = split.channels(entry["Channels"], None)
    if "Parameters" in entry:
        entry["Parameters"] = split.parameters(entry["Parameters"], [])
    return common.count_nodes(entry)


def _shrink(entry, path, later):
    """An entry within ENTRY_NODE_BUDGET: a larger one keeps the nested Parameters that fit and the rest follow in
    ``later`` (after the op that adds the entry)."""
    if type(entry) is not dict or type(entry.get("Parameters")) is not list or _skeleton_nodes(entry) <= ENTRY_NODE_BUDGET:
        return entry
    head = {key: value for key, value in entry.items() if key != "Parameters"}
    head["Parameters"] = _place(entry["Parameters"], path, ENTRY_NODE_BUDGET - _skeleton_nodes(head), later)
    return head


def _place(entries, path, room, later):
    """The entries of the owner at ``path`` that fit ``room`` (a prefix); the rest go to ``later`` as
    (path, first index, entries) parameters.append chunks, each followed by the chunks of its own entries."""
    inline, chunk, chunk_room = [], None, 0
    for index, entry in enumerate(entries):
        nested = []
        entry = _shrink(entry, path + [index], nested)
        size = _skeleton_nodes(entry) if type(entry) is dict else common.count_nodes(entry)
        if chunk is None and size <= room:
            inline.append(entry)
            room -= size
        else:
            if chunk is None or size > chunk_room:
                chunk = []
                later.append((path, index, chunk))
                chunk_room = ENTRY_NODE_BUDGET
            chunk.append(entry)
            chunk_room -= size
        later.extend(nested)
    return inline


def split_section(section):
    """(section entry for one op, [(parameter path, first index, entries)] that follow as parameters.append, [(channel,
    parameter path or None, columnar keys, stated runs)] that follow as channel.append): the entry and each
    parameters.append chunk carry their channels with as many of the keys as fit the op budget."""
    entry = seq.without_group(seq._public(section))
    later = []
    if type(entry.get("Parameters")) is list and _skeleton_nodes(entry) > ENTRY_NODE_BUDGET:
        head = {key: value for key, value in entry.items() if key != "Parameters"}
        entry["Parameters"] = _place(entry["Parameters"], [], ENTRY_NODE_BUDGET - _skeleton_nodes(head), later)
    split = _Split()
    if type(entry.get("Channels")) is dict:
        entry["Channels"] = split.channels(entry["Channels"], None)
    if "Parameters" in entry:
        entry["Parameters"] = split.parameters(entry["Parameters"], [])
    appends = split.appends
    parameters = []
    for path, start, chunk in later:
        split = _Split()
        parameters.append((path, start, split.parameters(chunk, path, start)))
        appends.extend(split.appends)
    return entry, parameters, appends


# ---------------------------------------------------------------------------
# Identity
# ---------------------------------------------------------------------------
def track_key(track):
    prop = track.get("Property") or ""
    if track.get("Type") == "Transform" and prop == "Transform":
        prop = ""
    return track.get("Type"), prop, track.get("Name") or ""


def match_tracks(authored, live):
    """[(authored, live or None)] and the live tracks no authored one matches: Id (object name) first, else the k-th
    track of the same (Type, Property, Name)."""
    used = set()
    pairs = []
    for track in authored:
        found = None
        if track.get("Id"):
            found = next((index for index, item in enumerate(live)
                          if index not in used and item.get("_Object") == track["Id"] and track_key(item) == track_key(track)),
                         None)
        if found is None:
            found = next((index for index, item in enumerate(live)
                          if index not in used and track_key(item) == track_key(track)
                          and (not track.get("Id") or item.get("Id") in (None, track["Id"]))), None)
        if found is not None:
            used.add(found)
        pairs.append((track, live[found] if found is not None else None))
    return pairs, [item for index, item in enumerate(live) if index not in used]


def match_bindings(authored, live):
    """[(authored, live or None)] and the unmatched live bindings among one sibling list: Id, else Name, else
    RenamedFrom."""
    used = set()
    pairs = []
    for binding in authored:
        found = None
        wanted = (binding.get("Id") or "").upper()
        if wanted:
            found = next((index for index, item in enumerate(live) if index not in used and item.get("_Guid", "").upper() == wanted),
                         None)
        else:
            for name in (binding.get("Name"), binding.get("RenamedFrom")):
                if found is None and name:
                    found = next((index for index, item in enumerate(live) if index not in used and item.get("Name") == name),
                                 None)
        if found is not None:
            used.add(found)
        pairs.append((binding, live[found] if found is not None else None))
    return pairs, [item for index, item in enumerate(live) if index not in used]


def _guids(bindings):
    for binding in bindings:
        yield binding.get("_Guid", "")
        yield from _guids(binding.get("Children", []))


# ---------------------------------------------------------------------------
# Plan
# ---------------------------------------------------------------------------
class TrackNode:
    def __init__(self, authored, live, path, binding):
        self.authored = authored          # normalized authored track
        self.live = live                  # live track (with _Object) or None
        self.path = path
        self.binding = binding            # binding GUID, "" for global tracks
        self.object = live.get("_Object") if live is not None else None
        self.sections = []                # SectionNode

    def set_object(self, name):
        self.object = name


class SectionNode:
    def __init__(self, track, path, live=None):
        self.track = track
        self.path = path
        self.object = live.get("_Object") if live is not None else None
        self.parameters = []
        self.appends = []

    def set_object(self, name):
        self.object = name


class BindingNode:
    def __init__(self, authored, live, guid, parent, path):
        self.authored = authored          # normalized authored binding (RenamedFrom kept)
        self.live = live
        self.guid = guid
        self.parent = parent
        self.path = path
        self.children = []
        self.tracks = []


class Plan:
    def __init__(self, body, live, root_path=None):
        """``root_path``: the document path of the single root binding (a Template Sequence's /Body/Root) instead of
        /Body/Bindings/<n>."""
        self.body = body
        self.live = live
        self.root_path = root_path
        self.bindings_path = root_path or "/Body/Bindings"
        self.warnings = []
        self.bindings = []                # root BindingNodes
        self.removed = []                 # live bindings deleted (whole subtrees)
        self.global_tracks = []           # TrackNodes
        self.removed_tracks = []          # (live track, path)
        self.track_scopes = []            # (scope path, [TrackNode], live tracks)
        self.taken = set(_guids(live.get("Bindings", [])))
        self.ids = set()

    # ---- matching ----------------------------------------------------------------------------------------------
    def _new_guid(self, binding, removed_guids):
        wanted = (binding.get("Id") or "").upper()
        if wanted and GUID.match(wanted) and wanted not in self.ids and (wanted not in self.taken or wanted in removed_guids):
            if wanted in removed_guids:
                self.warnings.append(RuntimeDiagnostic(
                    path=self.bindings_path, code="Sequencer.BindingRecreated", severity=DiagnosticSeverity.WARNING,
                    message=f"Binding {binding.get('Name', '')} ({wanted}) moved to another parent: Sequencer has no such move, so "
                            "it is deleted and created again under its new parent with the same GUID; its tracks are "
                            "rewritten from the document."))
            return wanted
        while True:
            guid = uuid.uuid4().hex.upper()
            if guid not in self.taken and guid not in self.ids:
                return guid

    def _bindings(self, authored, live, parent, base):
        pairs, unmatched = match_bindings(authored, live)
        self.removed.extend(unmatched)
        return [BindingNode(binding, match, match["_Guid"].upper() if match is not None else None, parent,
                            self.root_path if self.root_path and parent is None else f"{base}/{index}")
                for index, (binding, match) in enumerate(pairs)]

    def match(self):
        body, live = self.body, self.live
        if "Bindings" in body:
            def walk(authored, live_list, parent, base):
                nodes = self._bindings(authored, live_list, parent, base)
                for node in nodes:
                    node.children = walk(node.authored.get("Children", []),
                                         node.live.get("Children", []) if node.live is not None else [], node.guid,
                                         f"{node.path}/Children")
                return nodes
            self.bindings = walk(body["Bindings"], live.get("Bindings", []), None, "/Body/Bindings")
            # New bindings get their Id when it is free (a binding whose Id moved parent is re-created with it), else a
            # new GUID, as the editor gives one.
            removed_guids = {guid.upper() for guid in _guids(self.removed)}
            kept = {node.guid for node in self.binding_order() if node.guid is not None}
            self.taken = kept | removed_guids
            self.ids = set(kept)
            for node in self.binding_order():
                if node.guid is None:
                    node.guid = self._new_guid(node.authored, removed_guids - kept)
                    self.ids.add(node.guid)
                for child in node.children:
                    child.parent = node.guid
            for node in self.binding_order():
                tracks = seq.normalize_binding(node.authored).get("Tracks", [])
                if _keeps_missing_spawn_track(node):
                    tracks = tracks[1:]
                live_tracks = node.live.get("Tracks", []) if node.live is not None else []
                node.tracks = self._tracks(tracks, live_tracks, f"{node.path}/Tracks", node.guid)
        if "Tracks" in body:
            tracks = [seq.normalize_track(track) for track in body["Tracks"]]
            self.global_tracks = self._tracks(tracks, live.get("Tracks", []), "/Body/Tracks", "")

    def _tracks(self, authored, live, base, binding):
        pairs, unmatched = match_tracks(authored, live)
        self.removed_tracks.extend((item, base) for item in unmatched)
        nodes = [TrackNode(track, match, f"{base}/{index}", binding) for index, (track, match) in enumerate(pairs)]
        self.track_scopes.append((base, nodes, live))
        return nodes

    def binding_order(self):
        """Every planned binding, depth first in document order."""
        order = []

        def walk(nodes):
            for node in nodes:
                order.append(node)
                walk(node.children)
        walk(self.bindings)
        return order

    def all_tracks(self):
        tracks = []
        for node in self.binding_order():
            tracks.extend(node.tracks)
        return tracks + self.global_tracks

    # ---- batch 1: structure ------------------------------------------------------------------------------------
    def structure(self):
        settings = settings_op(self.body, self.live)
        ops = [settings] if settings is not None else []
        for binding in self.removed:
            ops.append(Op({"op": "binding.remove", "guid": binding["_Guid"]}, self.bindings_path))
        nodes = self.binding_order()
        for node in nodes:
            if node.live is not None and node.authored.get("Name") != node.live.get("Name"):
                ops.append(Op({"op": "binding.rename", "guid": node.guid, "name": node.authored["Name"]}, f"{node.path}/Name"))
        for node in nodes:
            if node.live is None:
                op = {"op": "binding.add", "guid": node.guid, "name": node.authored["Name"]}
                if node.parent:
                    op["parent"] = node.parent
                ops.append(Op(op, node.path, {"name": f"{node.path}/Name"}))
        # (Tracks of deleted bindings go with them: only matched scopes are planned.)
        for track, base in self.removed_tracks:
            ops.append(Op({"op": "track.remove", "track": track.get("_Object", "")}, base))
        for track in self.all_tracks():
            if track.live is None:
                op = {"op": "track.add", "type": track.authored.get("Type", "")}
                if track.binding:
                    op["binding"] = track.binding
                prop = track.authored.get("Property")
                if prop:
                    op["property"] = prop
                if track.authored.get("Name"):
                    op["name"] = track.authored["Name"]
                if track.authored.get("Id"):
                    op["object"] = track.authored["Id"]
                ops.append(Op(op, track.path, {"type": f"{track.path}/Type", "property": f"{track.path}/Property",
                                               "name": f"{track.path}/Name", "binding": track.path}, created=track.set_object))
        return ops

    # ---- batch 2: content --------------------------------------------------------------------------------------
    def content(self):
        ops = []
        for node in self.binding_order():
            wanted = {key: node.authored[key] for key in ENTRY_KEYS if key in node.authored}
            if not any(kind in wanted for kind in seq.BINDING_KINDS):
                wanted["Bind"] = []
            have = {key: node.live[key] for key in ENTRY_KEYS if node.live is not None and key in node.live}
            if node.live is None:
                have = {"Bind": []}
            if _differs(_entry_form(wanted), _entry_form(have)):
                values = {"entry": node.path}
                if "Root" in wanted:
                    values["entry/Root"] = node.path  # the root's fields are Body.Root's own
                ops.append(Op({"op": "binding.set", "guid": node.guid, "entry": wanted}, node.path, values=values))
        if "Bindings" in self.body:
            order = [node.guid for node in self.binding_order()]
            live_order = [guid.upper() for guid in _guids(self.live.get("Bindings", []))]
            kept = [guid for guid in live_order if guid in set(order)]
            if [guid for guid in order if guid in set(kept)] != kept or len(order) != len(kept):
                ops.append(Op({"op": "binding.order", "guids": order}, self.bindings_path))
        # Track order first: a binding reference into a sub-sequence names its sub section by document path.
        for base, nodes, live in self.track_scopes:
            names = [node.object for node in nodes]
            live_names = [item.get("_Object") for item in live if item.get("_Object") in set(names)]
            if any(node.live is None for node in nodes) or [name for name in names if name in set(live_names)] != live_names:
                if names and all(names):
                    ops.append(Op({"op": "track.order", "tracks": names}, base))
        for track in sorted(self.all_tracks(), key=lambda node: _has_remote_reference(node.authored)):
            ops += self._track_ops(track)
        return ops

    def _track_ops(self, track):
        ops = []
        wanted = {key: track.authored[key] for key in TRACK_HEADER_KEYS if key in track.authored}
        have = {key: track.live[key] for key in TRACK_HEADER_KEYS if track.live is not None and key in track.live}
        if track.live is None:
            have = {key: wanted[key] for key in ("Property", "Name") if key in wanted}
        if _differs(seq._drop_defaults(wanted), seq._drop_defaults(seq.normalize_track(have))):
            ops.append(Op({"op": "track.set", "track": track.object, "entry": {"Type": track.authored.get("Type", ""), **wanted}},
                          track.path, values={"entry": track.path}))
        authored = track.authored.get("Sections", [])
        live = [section for section in (track.live or {}).get("Sections", [])]
        keep = 0
        if track.live is not None:
            while keep < min(len(authored), len(live)) and authored[keep].get("Class") == live[keep].get("Class"):
                keep += 1
        for index in reversed(range(keep, len(live))):
            ops.append(Op({"op": "section.remove", "track": track.object, "section": live[index].get("_Object", "")},
                          f"{track.path}/Sections"))
        for index, section in enumerate(authored):
            path = f"{track.path}/Sections/{index}"
            if index < keep:
                node = SectionNode(track, path, live[index])
                track.sections.append(node)
                if not _differs(seq._drop_defaults(seq.without_group(seq.normalize_section(section))),
                                seq._drop_defaults(seq.without_group(seq.normalize_section(live[index])))):
                    continue
                entry, node.parameters, node.appends = split_section(section)
                ops.append(Op({"op": "section.set", "track": track.object, "section": node.object, "entry": entry}, path,
                              values={"entry": path}))
            else:
                node = SectionNode(track, path)
                track.sections.append(node)
                entry, node.parameters, node.appends = split_section(section)
                ops.append(Op({"op": "section.add", "track": track.object, "entry": entry}, path, values={"entry": path},
                              created=node.set_object))
        return ops

    # ---- batch 3: dense channels, folders, markers, metadata -------------------------------------------------------
    def finish(self):
        ops = []
        for track in self.all_tracks():
            for node in track.sections:
                for path, start, entries in node.parameters:
                    where = node.path + "".join(f"/Parameters/{index}" for index in path) + "/Parameters"
                    op = {"op": "parameters.append", "track": track.object, "section": node.object, "entries": entries}
                    if path:
                        op["parameter"] = path
                    ops.append(Op(op, where, values={f"entries/{index}": f"{where}/{start + index}" for index in range(len(entries))}))
                for channel, parameter, keys, stated in node.appends:
                    owner = node.path + "".join(f"/Parameters/{index}" for index in parameter or [])
                    where = f"{owner}/Channels/{escape(channel)}"
                    op = {"op": "channel.append", "track": track.object, "section": node.object, "channel": channel,
                          "keys": keys}
                    if parameter is not None:
                        op["parameter"] = parameter
                    if stated:
                        op["stated"] = stated
                    ops.append(Op(op, where, values={"keys": where}))
        groups = self._section_groups()
        if groups is not None:
            ops.append(groups)
        folders = self._folders()
        if folders is not None:
            ops.append(folders)
        if "Markers" in self.body:
            wanted = seq.normalize_region("Markers", self.body["Markers"])
            if _differs(wanted, seq.normalize_region("Markers", self.live.get("Markers", []))):
                ops.append(Op({"op": "markers.set", "markers": self.body["Markers"]}, "/Body/Markers",
                              values={"markers": "/Body/Markers"}))
        if "Groups" in self.body and _differs(seq.normalize_region("Groups", self.body["Groups"]),
                                              seq.normalize_region("Groups", self.live.get("Groups", []))):
            ops.append(Op({"op": "groups.set", "groups": self.body["Groups"]}, "/Body/Groups",
                          values={"groups": "/Body/Groups"}))
        metadata = self.body.get("MetaData", {})
        live_metadata = self.live.get("MetaData", {})
        changed = {name: value for name, value in metadata.items() if value != live_metadata.get(name, "")}
        if "MetaData" in self.body:
            # A listed MetaData is complete: a field it leaves out is cleared.
            changed.update(seq.omitted_values("MetaData", metadata, live_metadata))
        if changed:
            ops.append(Op({"op": "metadata.set", "values": changed}, "/Body/MetaData", values={"values": "/Body/MetaData"}))
        return ops

    def _section_groups(self):
        """section_groups.set when the planned sections are grouped otherwise than the live ones (every group, by
        track and section object names; the regions the document omits keep their live sections)."""
        if not any(key in self.body for key in seq.GROUPED):
            return None
        wanted, have = {}, {}

        def add(groups, number, track, section):
            if number is not None and track and section:
                groups.setdefault(number, []).append({"track": track, "section": section})

        def add_live(groups, tracks):
            for track in tracks:
                for section in track.get("Sections", []):
                    add(groups, section.get("Group"), track.get("_Object"), section.get("_Object"))

        def live_tracks(bindings):
            for binding in bindings:
                yield from binding.get("Tracks", [])
                yield from live_tracks(binding.get("Children", []))
        planned = (([track for node in self.binding_order() for track in node.tracks] if "Bindings" in self.body else [])
                   + (self.global_tracks if "Tracks" in self.body else []))
        for track in planned:
            for index, node in enumerate(track.sections):
                add(wanted, track.authored.get("Sections", [])[index].get("Group"), track.object, node.object)
        if "Bindings" not in self.body:
            add_live(wanted, live_tracks(self.live.get("Bindings", [])))
        if "Tracks" not in self.body:
            add_live(wanted, self.live.get("Tracks", []))
        add_live(have, live_tracks(self.live.get("Bindings", [])))
        add_live(have, self.live.get("Tracks", []))

        def partition(groups):
            return {frozenset((row["track"], row["section"]) for row in rows) for rows in groups.values()}
        if partition(wanted) == partition(have):
            return None
        return Op({"op": "section_groups.set", "groups": [wanted[number] for number in sorted(wanted)]},
                  self.bindings_path if "Bindings" in self.body and "Tracks" not in self.body else "/Body/Tracks")

    def _folders(self):
        body, live = self.body, self.live
        if not any(key in body for key in ("Folders", "Bindings", "Tracks")):
            return None
        folders = body["Folders"] if "Folders" in body else live.get("Folders", [])
        if "Bindings" in body:
            binding_rows = [{"guid": node.guid, "folder": node.authored["Folder"]} for node in self.bindings
                            if node.authored.get("Folder")]
        else:
            binding_rows = [{"guid": item["_Guid"], "folder": item["Folder"]} for item in live.get("Bindings", [])
                            if item.get("Folder")]
        if "Tracks" in body:
            track_rows = [{"track": node.object, "folder": node.authored["Folder"]} for node in self.global_tracks
                          if node.authored.get("Folder")]
        else:
            track_rows = [{"track": item["_Object"], "folder": item["Folder"]} for item in live.get("Tracks", [])
                          if item.get("Folder")]
        live_bindings = sorted((item["_Guid"].upper(), item["Folder"]) for item in live.get("Bindings", []) if item.get("Folder"))
        live_tracks = sorted((item.get("_Object"), item["Folder"]) for item in live.get("Tracks", []) if item.get("Folder"))
        same = (not _differs(folders, live.get("Folders", []))
                and sorted((row["guid"].upper(), row["folder"]) for row in binding_rows) == live_bindings
                and sorted((row["track"], row["folder"]) for row in track_rows) == live_tracks)
        if same:
            return None
        return Op({"op": "folders.set", "folders": folders, "bindings": binding_rows, "tracks": track_rows}, "/Body/Folders",
                  values={"folders": "/Body/Folders"})


def _has_remote_reference(value):
    """A binding reference into a sub-sequence ({"Binding", "Sequence"}) somewhere in this track."""
    if type(value) is dict:
        return ("Sequence" in value and "Binding" in value) or any(_has_remote_reference(item) for item in value.values())
    if type(value) is list:
        return any(_has_remote_reference(item) for item in value)
    return False


def _is_spawn_track(track):
    return track.get("Type") == "Spawn"


def _keeps_missing_spawn_track(node):
    """A spawnable that has no Spawn track and whose document lists none keeps having none (the comparison form gives
    both sides the default one, so they are equal); a new spawnable, or one converted to a spawnable, gets it."""
    return (node.live is not None and any(kind in node.live and kind in node.authored for kind in ("Spawnable", "Root"))
            and not any(_is_spawn_track(track) for track in node.live.get("Tracks", []))
            and not any(_is_spawn_track(track) for track in node.authored.get("Tracks", [])))


def _entry_form(entry):
    """A binding's kind and content as compared (Tags [] = none)."""
    entry = dict(entry)
    if entry.get("Tags") == []:
        del entry["Tags"]
    return entry


def _differs(authored, live):
    changes = []
    seq.value_diff("", authored, live, False, changes)
    return bool(changes)


# ---------------------------------------------------------------------------
# Refusals that need no write
# ---------------------------------------------------------------------------
def refuse(body, live):
    if any(key in body for key in seq.GROUPED):
        seq.check_section_groups({key: body[key] if key in body else live.get(key, []) for key in seq.GROUPED})
    folders = {item.get("Path") for item in (body["Folders"] if "Folders" in body else live.get("Folders", []))}
    for key in ("Bindings", "Tracks"):
        for index, entry in enumerate(body.get(key, [])):
            if entry.get("Folder") and entry["Folder"] not in folders:
                raise error(f"/Body/{key}/{index}/Folder", f"Folder '{entry['Folder']}' is not listed in Body.Folders.",
                            "Sequencer.InvalidFolder")
    _refuse_bindings(body.get("Bindings", []), "/Body/Bindings")


def _refuse_bindings(bindings, base):
    for index, binding in enumerate(bindings):
        path = f"{base}/{index}"
        spawnable = binding.get("Spawnable")
        if spawnable is not None and (type(spawnable) is not dict or not spawnable.get("Class")):
            raise error(f"{path}/Spawnable", "A spawnable has Class (the actor class it spawns).", "Sequencer.InvalidBody")
        if binding.get("Id") and not GUID.match(binding["Id"]):
            raise error(f"{path}/Id", "Id is a binding GUID (32 hex digits).", "Sequencer.InvalidBody")
        if binding.get("Folder") is not None and base != "/Body/Bindings":
            raise error(f"{path}/Folder", "Only root bindings have a Folder.", "Sequencer.InvalidBody")
        _refuse_bindings(binding.get("Children", []), f"{path}/Children")


# ---------------------------------------------------------------------------
# Outcome
# ---------------------------------------------------------------------------
def _messages(value):
    return [item for item in (value or {}).get("Messages", []) if type(item) is dict]


def validation_outcome(before, after):
    """(fatal error or None, warnings): new errors fail the apply; errors the sequence already had and warnings are
    reported."""
    def key(item):
        return item.get("Message"), item.get("Path", "")
    old = {key(item) for item in _messages(before) if item.get("Severity") == "Error"}
    errors = [item for item in _messages(after) if item.get("Severity") == "Error"]
    new = [item for item in errors if key(item) not in old]
    if new:
        message = "The sequence has new errors: " + " | ".join(item.get("Message", "") for item in new[:8])
        return error(new[0].get("Path") or "/Body", message, "Sequencer.ValidationFailed"), ()
    warnings = [RuntimeDiagnostic(path=item.get("Path") or "/Body", code="Sequencer.ValidationError",
                                  message=item.get("Message", ""), severity=DiagnosticSeverity.WARNING) for item in errors]
    warnings += [RuntimeDiagnostic(path=item.get("Path") or "/Body", code="Sequencer.ValidationWarning",
                                   message=item.get("Message", ""), severity=DiagnosticSeverity.WARNING)
                 for item in _messages(after) if item.get("Severity") == "Warning"]
    return None, tuple(warnings)


def dependents_warnings(dependents):
    """Sequencer.DependentsAffected: packages that reference the sequence after bindings were deleted (decision §8.8:
    reported, never edited)."""
    by_package = {}
    for item in dependents:
        rows = by_package.setdefault(item.get("package", ""), [])
        rows.extend(name for name in item.get("bindings", []) if name not in rows)
    return [RuntimeDiagnostic(
        path="/Body/Bindings", code="Sequencer.DependentsAffected", severity=DiagnosticSeverity.WARNING,
        message=f"{package} references this sequence; the deleted binding(s) {', '.join(repr(name) for name in names)} are "
                "gone, so binding references it stores (Get Sequence Binding pins, Level Sequence Actor binding overrides, "
                "camera cuts of parent sequences) no longer find them. It is not edited.")
        for package, names in sorted(by_package.items())]


# ---------------------------------------------------------------------------
# Apply
# ---------------------------------------------------------------------------
def settings_op(body, live):
    """The Settings fields that differ, or None. A listed Settings resets the fields it leaves out to a new
    sequence's (null: CustomClock / SelectionRange none), but the kept timing fields (seq.SETTINGS_KEPT)."""
    if "Settings" not in body:
        return None
    settings, have = body["Settings"], live.get("Settings", {})
    changed = {name: value for name, value in settings.items() if not common.same(value, have.get(name), False)}
    changed.update(seq.omitted_values("Settings", settings, have))
    return Op({"op": "settings", "values": changed}, "/Body/Settings", values={"values": "/Body/Settings"}) if changed else None


def apply(canonical, read, mutable, context):
    """Core calls apply only when the document differs from the asset. The Body is applied first, then the
    Properties as the Details panel writes them. A rate change runs first on its own: the document states every
    frame in the new rates, so the rest is planned against a read in those rates; the other batches are planned
    against one read."""
    body = canonical.get("Body", {})
    region = seq._properties()
    properties = canonical.get("Properties", {})
    region.check_owned(properties)
    calls = seq.SequencerCalls(context, read, mutable)
    live, facts = seq.read_body(calls, ids=True)
    changed = []
    warnings = []
    edits = Edits(calls)
    owner = seq.director_class(facts, canonical["Target"])
    if body.get("Director") is not None:
        if owner is None:
            created = []
            edits.run([Op({"op": "director.create"}, "/Body/Director", created=created.append)])
            owner = director.class_of(created[0]) if created and created[0] else director.default_class(canonical["Target"])
        written, notes = director.apply(context, read, mutable, seq.ROUTE, seq.BY_ID, owner, body["Director"])
        changed.extend(written)
        warnings.extend(notes)
        if written:
            # Event keys and director bindings read their endpoints from the Director: plan against what it is now (a
            # key whose endpoint the document deleted is then rewritten, and refused with Sequencer.UnknownEventEndpoint).
            live, facts = seq.read_body(calls, ids=True)
    if seq.body_diff(body, live, exact=False):
        refuse(body, live)
        before = status(calls)
        settings = settings_op(body, live)
        if settings is not None and any(key in settings.op["values"] for key in ("DisplayRate", "TickResolution")):
            edits.run([settings])
            live, _ = seq.read_body(calls, ids=True)
        plan = Plan(body, live)
        plan.match()
        edits.run(plan.structure())
        edits.run(plan.content())
        edits.run(plan.finish())
        failure, outcome = validation_outcome(before, status(calls))
        if failure is not None:
            raise failure
        warnings.extend(plan.warnings)
        warnings.extend(outcome)
        warnings.extend(dependents_warnings(edits.dependents))
    if "Director" in body and body["Director"] is None and owner is not None:
        edits.run([Op({"op": "director.remove"}, "/Body/Director")])
    changed.extend(edits.changed)
    changed.extend(region.apply(properties, read, mutable, context))
    return with_diagnostics(tuple(dict.fromkeys(changed)), warnings)


def repair(canonical, mutable, context):
    """Compile the Director when it is not up to date after an apply that wrote it or the tracks (event keys and
    director bindings call the compiled class). Compiler errors fail the apply when the document authors the Director;
    otherwise (a Director that was already broken) they come back as warnings."""
    body = canonical.get("Body", {})
    if not any(key in body for key in ("Director", "Bindings", "Tracks")) or body.get("Director", True) is None:
        return ()
    calls = seq.SequencerCalls(context, mutable)
    page = calls.query(seq.READ, {"offset": 0}).get("records", [])
    header = page[0] if page and page[0].get("kind") == "header" else {}
    owner = seq.director_class({"header": header}, canonical["Target"])
    if owner is None:
        return ()
    try:
        changed, notes = director.compile_if_needed(context, mutable, seq.ROUTE, seq.BY_ID, owner)
    except AssetDocumentContractError as failure:
        if "Director" in body:
            raise
        return with_diagnostics((), (RuntimeDiagnostic(
            path="/Body/Director", code="Sequencer.DirectorCompileFailed", severity=DiagnosticSeverity.WARNING,
            message=f"The Director Blueprint does not compile: {failure.diagnostic.message}"),))
    return with_diagnostics(tuple(dict.fromkeys(changed)), notes)


def preflight(canonical, read, context):
    """Refusals that need no write (an existing sequence: read; a new one: the document alone)."""
    body = canonical.get("Body", {})
    if read is None:
        refuse(body, {})
        return
    live, _ = seq.read_body(seq.SequencerCalls(context, read), ids=True)
    if seq.body_diff(body, live, exact=False):
        refuse(body, live)


__all__ = ("Edits", "Op", "Plan", "apply", "settings_op", "dependents_warnings", "match_bindings", "match_tracks", "op_path", "preflight",
           "refuse", "repair", "split_section", "validation_outcome")
