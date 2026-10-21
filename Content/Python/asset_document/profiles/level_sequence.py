"""Registered Level Sequence Profile (profile.level_sequence.v1, class /Script/LevelSequence.LevelSequence).

Document (MCP/schemas/assetdocument/Sequencer.md)::

    "Body": {
      "Settings": {"DisplayRate": {"Numerator": 30, "Denominator": 1},
                   "TickResolution": {"Numerator": 24000, "Denominator": 1},
                   "Evaluation": "WithSubFrames", "PlaybackRange": {"Start": 0, "End": 150}},
      "Bindings": [{"Name": "SM_Door", "Class": "/Script/Engine.StaticMeshActor",
                    "Bind": ["uobj://actor?payload0=/Game/Maps/Lobby.Lobby:PersistentLevel.SM_Door_3"],
                    "Tracks": [{"Type": "Transform", "Sections": [{"Start": 60, "End": 90, "Channels": {
                      "Rotation.Z": {"Keys": [{"Frame": 60, "Value": 0}, {"Frame": 90, "Value": -95}]}}}]}]}],
      "Tracks": [{"Type": "Fade", "Sections": [...]}],
      "Folders": [{"Path": "Set"}], "Markers": [{"Frame": 60, "Label": "Door opens"}],
      "Groups": [{"Name": "Hero", "Members": [{"Binding": "Hero"}, {"Track": "/Body/Tracks/0"}]}],
      "Director": {"Variables": [...], "FunctionGraphs": [...], "UbergraphPages": [...]}, "MetaData": {"Notes": "..."}
    }

* Create and Apply write every region as Sequencer's own edits (``seq.edit.v1``, planned in ``level_sequence_apply``).
* ``Director`` is the hosted Director Blueprint in the Blueprint profile's keys (``sequencer_director``); event keys
  and director bindings name its functions / custom events as ``Endpoint``.
* A section's ``Group`` is a number shared by the sections of one section group; groups compare by which sections they
  group, not by their numbers. ``Groups`` (the Group Manager's node groups) compare their ``Members`` as a set.
* Times are display frames of ``Settings.DisplayRate`` (shortest decimal that converts back to the same tick);
  sub / shot section ``Properties`` use the inner sequence's frames.
* A channel with more than 64 keys is extracted in the columnar form ``{"Frames", "Values", "Interp"?, ...}``
  (``Interp`` one token when every key has it; omitted when every key is ``SmartAuto``).
"""
from __future__ import annotations

import copy
import dataclasses
import json

from asset_document.diagnostics import DiagnosticSeverity, RuntimeDiagnostic, with_diagnostics
from asset_document.diff.model import Difference, DiffKind
from asset_document.dto import PrimitiveRequirement
from asset_document.meta import (
    ComparisonPolicy, DefaultPolicy, DeletePolicy, IdentityPolicy, OmissionPolicy, OrderPolicy, ProfileMeta,
    RegionMeta, RegionOwnership,
)
from asset_document.mutation_intent_projection import SNAPSHOT_COVERED
from asset_document.profile_runtime import RuntimeProfileRegistration, RuntimeTargetPolicy
from asset_document.profiles import data_table_common as common
from asset_document.profiles import ik_common as ik
from asset_document.profiles import sequencer_director as director
from asset_document.profiles.ublueprint_runtime import PENDING_RELAYOUT, relayout_differences
from asset_document.regions.target_lifecycle import generic_factory_lifecycle, lifecycle_requirements

PROFILE_ID = "profile.level_sequence.v1"
CLASS = "/Script/LevelSequence.LevelSequence"
ROUTE = "seq_profile"
PROVIDER = "seq"
PREFIX = "Sequencer"
READ = "seq.read.v1"
INSPECT = "seq.inspect.v1"
CATALOG = "seq.catalog.v1"
EDIT = "seq.edit.v1"
STATUS = "seq.status.v1"

BODY_KEYS = ("Settings", "Bindings", "Tracks", "Folders", "Markers", "Groups", "Director", "MetaData")
SETTINGS_KEYS = ("DisplayRate", "TickResolution", "Evaluation", "Clock", "CustomClock", "PlaybackRange",
                 "SelectionRange", "PlaybackRangeLocked", "ReadOnly", "MarkersLocked", "ShowMarkers")
BINDING_KEYS = ("Name", "RenamedFrom", "Id", "Class", "Bind", "Spawnable", "CustomBinding", "References", "Tags",
                "Folder", "Tracks", "Children")
TRACK_KEYS = ("Type", "Property", "Name", "Id", "Folder", "Muted", "MutedRows", "RowConditions", "Properties", "Sections")
SECTION_KEYS = ("Class", "Start", "End", "Row", "OverlapPriority", "Active", "Locked", "Group", "Blend", "Easing",
                "PreRoll", "PostRoll", "WhenFinished", "Properties", "Event", "Unsupported", "Channels", "Parameters")
# A parameter of a parameter section (material, MPC, custom primitive data, particle parameters, DMX patches).
PARAMETER_KEYS = ("Type", "Name", "Properties", "Channels", "Parameters")
CHANNEL_KEYS = ("Keys", "Default", "PreInfinity", "PostInfinity", "Unsupported", "Frames", "Values", "Interp",
                "Arrive", "Leave", "ArriveWeight", "LeaveWeight", "WeightMode")
FOLDER_KEYS = ("Path", "Color")
MARKER_KEYS = ("Frame", "Label", "Comment", "Color", "DeterminismFence", "Inclusive")
GROUP_KEYS = ("Name", "Members")
METADATA_KEYS = ("Author", "Notes")
# A listed Settings / MetaData: an omitted field takes a new sequence's value. The timing fields extract always
# writes are kept when omitted (a Settings that lists only PlaybackRange must not reset the rates); Diff says so
# (Sequencer.OmittedKept, Info).
SETTINGS_KEPT = ("DisplayRate", "TickResolution", "Evaluation", "PlaybackRange")
# Omitted = this value; CustomClock and SelectionRange omitted = none (null in the settings op: the defaults).
SETTINGS_DEFAULTS = {"Clock": "Tick", "PlaybackRangeLocked": False, "ReadOnly": False, "MarkersLocked": False,
                     "ShowMarkers": False}
METADATA_DEFAULTS = {"Author": "", "Notes": ""}
# Columnar channel arrays (document key -> seq.read.v1 "columns" record key).
COLUMNS = {"Frames": "frames", "Values": "values", "Interp": "interp", "Arrive": "arrive", "Leave": "leave",
           "ArriveWeight": "arrive_weight", "LeaveWeight": "leave_weight", "WeightMode": "weight_mode"}
# Omitted = this value (dropped when canonical).
SECTION_DEFAULTS = {"Row": 0, "Active": True, "Locked": False, "PreRoll": 0, "PostRoll": 0}
EMPTY_DROPPED = ("Tracks", "Children", "Sections", "Keys", "Tags", "MutedRows", "RowConditions", "Channels", "Properties",
                 "Easing", "Parameters")
# (Root / Component: a Template Sequence's root spawnable and component bindings, template_sequence.py.)
BINDING_KINDS = ("Bind", "Spawnable", "CustomBinding", "References", "Root", "Component")
# Tangents and weights of a curve key: listed for User / Break keys, and for other keys whose stored tangents differ from
# the ones AutoSetTangents computes (stated tangents); the others' are computed.
TANGENT_KEYS = ("Arrive", "Leave", "ArriveWeight", "LeaveWeight", "WeightMode")
# The Spawn track the editor creates with a spawnable (spawned for the whole sequence): a spawnable without a listed
# Spawn track has this one.
SPAWN_TRACK = {"Type": "Spawn", "Sections": [{"Channels": {"Value": {"Default": True}}}]}

BODY_OWNED = {"MovieScene": "the Body regions (Settings, Bindings, Tracks, Folders, Markers)",
              "BindingReferences": "Body/Bindings (Bind, Spawnable, CustomBinding)",
              "DirectorBlueprint": "Body/Director", "DirectorClass": "Body/Director",
              "MetaDataObjects": "Body/MetaData"}
NOT_AUTHORED = ("ObjectReferences", "PossessedObjects", "ThumbnailInfo")


def _requirements(*primitives):
    return tuple(PrimitiveRequirement(item, 1, ROUTE) for item in primitives)


LIFECYCLE = generic_factory_lifecycle(ROUTE)
REQUIREMENTS = (_requirements(common.CLASS_RESOLVE, common.RESOLVE, READ, INSPECT, CATALOG, EDIT, STATUS)
                + lifecycle_requirements(LIFECYCLE) + director.requirements(ROUTE))
BY_ID = {item.primitive_id: item for item in REQUIREMENTS}


def error(path, message, code):
    return common.error(path, message, code)


escape = common.escape


class SequencerCalls(common.DataCalls):
    """Calls to the "seq" provider against one Level Sequence (editor handles handed off)."""

    def __init__(self, context, read, mutable=None):
        self.context = context
        self.by_id = BY_ID
        self.prefix = PREFIX
        self.read = context.handoff(source_handle=read, target_provider_id=PROVIDER, access="read_only")
        self.write = (context.handoff(source_handle=mutable, target_provider_id=PROVIDER, access="mutable")
                      if mutable is not None else None)


def _properties():
    return ik.RigProperties(CLASS, BODY_OWNED, NOT_AUTHORED, PREFIX)


# ---------------------------------------------------------------------------
# Document shape
# ---------------------------------------------------------------------------
def _object(value, path, what):
    return ik.object_value(value, path, what, PREFIX)


def _list(value, path, what):
    if type(value) is not list:
        raise error(path, f"{what} must be an array.", "Sequencer.InvalidBody")
    return value


def _keys(value, allowed, path, what):
    ik.check_keys(value, allowed, path, what, PREFIX)


def _validate_channels(channels, path):
    for name, channel in _object(channels, path, "Channels").items():
        where = f"{path}/{escape(name)}"
        _keys(_object(channel, where, "A channel"), CHANNEL_KEYS, where, "channel")
        if "Keys" in channel and "Frames" in channel:
            raise error(where, "A channel lists its keys either as Keys or in the columnar form (Frames, Values), "
                        "not both.", "Sequencer.InvalidBody")
        if "Keys" in channel:
            for index, key in enumerate(_list(channel["Keys"], f"{where}/Keys", "Keys")):
                _object(key, f"{where}/Keys/{index}", "A key")
        if "Frames" in channel:
            frames = _list(channel["Frames"], f"{where}/Frames", "Frames")
            for key in ("Values", "Arrive", "Leave", "ArriveWeight", "LeaveWeight", "WeightMode"):
                if key in channel and len(_list(channel[key], f"{where}/{key}", key)) != len(frames):
                    raise error(f"{where}/{key}", f"{key} has one entry per frame in Frames.", "Sequencer.InvalidBody")
            if type(channel.get("Interp")) is list and len(channel["Interp"]) != len(frames):
                raise error(f"{where}/Interp", "Interp is one token or one per frame in Frames.", "Sequencer.InvalidBody")


def _validate_parameters(parameters, path):
    for index, parameter in enumerate(_list(parameters, path, "Parameters")):
        where = f"{path}/{index}"
        _keys(_object(parameter, where, "A parameter"), PARAMETER_KEYS, where, "parameter")
        if "Channels" in parameter:
            _validate_channels(parameter["Channels"], f"{where}/Channels")
        if "Parameters" in parameter:
            _validate_parameters(parameter["Parameters"], f"{where}/Parameters")


def _validate_tracks(tracks, path):
    for index, track in enumerate(_list(tracks, path, "Tracks")):
        where = f"{path}/{index}"
        _keys(_object(track, where, "A track"), TRACK_KEYS, where, "track")
        if type(track.get("Type")) is not str or not track["Type"]:
            raise error(f"{where}/Type", "A track has a Type (a token such as Transform or a track class path).",
                        "Sequencer.InvalidBody")
        for row, condition in _object(track.get("RowConditions", {}), f"{where}/RowConditions", "RowConditions").items():
            if not row.isdigit():
                raise error(f"{where}/RowConditions/{escape(row)}", "RowConditions maps a row index (\"0\", \"1\", ...) to "
                            "its condition {Class, Properties?}.", "Sequencer.InvalidBody")
        for number, section in enumerate(_list(track.get("Sections", []), f"{where}/Sections", "Sections")):
            at = f"{where}/Sections/{number}"
            _keys(_object(section, at, "A section"), SECTION_KEYS, at, "section")
            group = section.get("Group")
            if group is not None and (type(group) not in (int, float) or not float(group).is_integer() or group < 1):
                raise error(f"{at}/Group", "Group is a section group number (1, 2, ...): the sections of one group share it.",
                            "Sequencer.InvalidBody")
            if "Channels" in section:
                _validate_channels(section["Channels"], f"{at}/Channels")
            if "Parameters" in section:
                _validate_parameters(section["Parameters"], f"{at}/Parameters")


def _validate_bindings(bindings, path):
    for index, binding in enumerate(_list(bindings, path, "Bindings")):
        where = f"{path}/{index}"
        _keys(_object(binding, where, "A binding"), BINDING_KEYS, where, "binding")
        ik.name_value(binding.get("Name"), f"{where}/Name", "A binding's Name", PREFIX)
        kinds = [key for key in ("Bind", "Spawnable", "CustomBinding", "References") if key in binding]
        if len(kinds) > 1:
            raise error(where, f"A binding is one of Bind, Spawnable or CustomBinding (got {', '.join(kinds)}).",
                        "Sequencer.InvalidBody")
        if "Tracks" in binding:
            _validate_tracks(binding["Tracks"], f"{where}/Tracks")
        if "Children" in binding:
            _validate_bindings(binding["Children"], f"{where}/Children")


def validate_body(body):
    path = "/Body"
    if "Settings" in body:
        _keys(_object(body["Settings"], f"{path}/Settings", "Settings"), SETTINGS_KEYS, f"{path}/Settings", "Settings")
    if "Bindings" in body:
        _validate_bindings(body["Bindings"], f"{path}/Bindings")
    if "Tracks" in body:
        _validate_tracks(body["Tracks"], f"{path}/Tracks")
    for key, allowed in (("Folders", FOLDER_KEYS), ("Markers", MARKER_KEYS), ("Groups", GROUP_KEYS)):
        if key in body:
            for index, entry in enumerate(_list(body[key], f"{path}/{key}", key)):
                _keys(_object(entry, f"{path}/{key}/{index}", f"A {key[:-1].lower()}"), allowed,
                      f"{path}/{key}/{index}", key[:-1].lower())
    for index, group in enumerate(body.get("Groups") or []):
        if type(group.get("Name")) is not str or not group["Name"]:
            raise error(f"{path}/Groups/{index}/Name", "A group has a Name.", "Sequencer.InvalidBody")
        for number, member in enumerate(_list(group.get("Members", []), f"{path}/Groups/{index}/Members", "Members")):
            _object(member, f"{path}/Groups/{index}/Members/{number}", "A group member")
    if "MetaData" in body:
        _keys(_object(body["MetaData"], f"{path}/MetaData", "MetaData"), METADATA_KEYS, f"{path}/MetaData", "MetaData")


def _drop_defaults(value):
    """Canonical form: values equal to their omitted meaning are dropped (empty lists / objects of complete
    sub-regions, section header defaults, unmuted tracks, empty channels)."""
    if type(value) is list:
        return [_drop_defaults(item) for item in value]
    if type(value) is not dict:
        return value
    result = {}
    for key, item in value.items():
        item = _drop_defaults(item)
        if key in EMPTY_DROPPED and item in ([], {}):
            continue
        if key in SECTION_DEFAULTS and item == SECTION_DEFAULTS[key] and type(item) is type(SECTION_DEFAULTS[key]):
            continue
        if key == "Muted" and item is False:
            continue
        result[key] = item
    if "Channels" in result:
        result["Channels"] = {name: channel for name, channel in result["Channels"].items() if channel != {}}
        if not result["Channels"]:
            del result["Channels"]
    return result


CAMERA_CUT_TYPES = ("CameraCut", "/Script/MovieSceneTracks.MovieSceneCameraCutTrack")


def _retarget_track_path(path, moved):
    """A document path under /Body/Tracks/<i> with <i> renumbered by ``moved`` (old index -> new index)."""
    prefix = "/Body/Tracks/"
    if type(path) is not str or not path.startswith(prefix):
        return path
    head, _, rest = path[len(prefix):].partition("/")
    if not head.isdigit() or int(head) not in moved:
        return path
    return f"{prefix}{moved[int(head)]}" + (f"/{rest}" if rest else "")


def _retarget(value, moved):
    """Remote binding references ({"Binding", "Sequence"}) and node group members ({"Track"}) renumbered."""
    if type(value) is list:
        return [_retarget(item, moved) for item in value]
    if type(value) is not dict:
        return value
    out = {key: _retarget(item, moved) for key, item in value.items()}
    if "Binding" in out and "Sequence" in out:
        sequence = value["Sequence"]
        out["Sequence"] = ([_retarget_track_path(item, moved) for item in sequence] if type(sequence) is list
                           else _retarget_track_path(sequence, moved))
    if set(out) <= {"Track", "Node"} and "Track" in out:
        out["Track"] = _retarget_track_path(value["Track"], moved)
    return out


def camera_cut_first(body):
    """Sequencer pins the camera cut track at the top of the global tracks (the read lists it first): a document that
    lists it elsewhere has it moved first, and its /Body/Tracks/<i> references renumbered to match."""
    tracks = body.get("Tracks")
    if type(tracks) is not list:
        return body
    index = next((number for number, track in enumerate(tracks)
                  if type(track) is dict and track.get("Type") in CAMERA_CUT_TYPES), None)
    if not index:
        return body
    moved = {number: (0 if number == index else number + 1 if number < index else number) for number in range(len(tracks))}
    order = [tracks[index]] + [track for number, track in enumerate(tracks) if number != index]
    out = {key: (_retarget(value, moved) if key != "Tracks" else value) for key, value in body.items()}
    out["Tracks"] = _retarget(order, moved)
    return out


def canonicalize(document):
    body = common.validate_header(document, (CLASS,), PREFIX, BODY_KEYS)
    validate_body(body)
    canonical = common.snapshot(document)
    canonical.setdefault("Definitions", {})
    canonical.setdefault("Properties", {})
    if "Body" in canonical:
        canonical["Body"] = camera_cut_first(canonical["Body"])
    canonical["Body"] = {key: (_drop_defaults(value) if key in ("Bindings", "Tracks") else value)
                         for key, value in canonical.get("Body", {}).items()}
    if "Director" in canonical["Body"]:
        canonical["Body"]["Director"] = director.canonical(canonical["Body"]["Director"],
                                                           director.default_class(document["Target"]))
    return canonical


# ---------------------------------------------------------------------------
# Comparison form (Diff and the apply plan)
# ---------------------------------------------------------------------------
def channel_keys(channel):
    """The keys of a channel as key objects (the columnar form expanded; a column value is the key's Value)."""
    if "Frames" not in channel:
        return [dict(key) for key in channel.get("Keys", [])]
    interp = channel.get("Interp")
    keys = []
    for index, frame in enumerate(channel.get("Frames", [])):
        key = {"Frame": frame}
        if "Values" in channel:
            key["Value"] = channel["Values"][index]
        token = interp[index] if type(interp) is list else interp
        if token is not None:
            key["Interp"] = token
        for name in TANGENT_KEYS:
            column = channel.get(name)
            if type(column) is list and column[index] is not None:
                key[name] = column[index]
        keys.append(key)
    return keys


def canonical_key(key):
    """SmartAuto is the omitted Interp. A key other than User / Break states its tangents with Arrive / Leave (stored
    tangents Sequencer would compute differently); without them its tangents are computed and the weights are moot."""
    key = dict(key)
    if key.get("Interp") == "SmartAuto":
        del key["Interp"]
    if key.get("Interp") not in ("User", "Break") and "Arrive" not in key and "Leave" not in key:
        for name in TANGENT_KEYS:
            key.pop(name, None)
    return key


def canonical_channel(channel):
    """One form for both key forms (key objects), Constant extrapolation omitted."""
    if type(channel) is not dict:
        return channel
    result = {name: value for name, value in channel.items() if name not in COLUMNS and name != "Keys"}
    keys = [canonical_key(key) for key in channel_keys(channel) if type(key) is dict]
    if keys:
        result["Keys"] = keys
    for name in ("PreInfinity", "PostInfinity"):
        if result.get(name) == "Constant":
            del result[name]
    return result


def _public(entry):
    """Without the plan's private ids (_Guid, _Object)."""
    return {key: value for key, value in entry.items() if not key.startswith("_")}


def _normalize_parameters(parameters):
    if type(parameters) is not list:
        return parameters
    result = []
    for parameter in parameters:
        if type(parameter) is dict:
            parameter = dict(parameter)
            if type(parameter.get("Channels")) is dict:
                parameter["Channels"] = {name: canonical_channel(channel) for name, channel in parameter["Channels"].items()}
            if "Parameters" in parameter:
                parameter["Parameters"] = _normalize_parameters(parameter["Parameters"])
        result.append(parameter)
    return result


def normalize_section(section):
    section = _public(section)
    if type(section.get("Channels")) is dict:
        section["Channels"] = {name: canonical_channel(channel) for name, channel in section["Channels"].items()}
    if "Parameters" in section:
        section["Parameters"] = _normalize_parameters(section["Parameters"])
    return section


def normalize_track(track):
    track = _public(track)
    if track.get("Type") == "Transform" and track.get("Property") == "Transform":
        del track["Property"]
    if "Sections" in track:
        track["Sections"] = [normalize_section(section) for section in track["Sections"]]
    return track


def normalize_binding(binding):
    """A binding as compared: no RenamedFrom, a kind (Bind: [] when none is listed), the default Spawn track of a
    spawnable that lists none."""
    binding = {key: value for key, value in _public(binding).items() if key != "RenamedFrom"}
    if not any(kind in binding for kind in BINDING_KINDS):
        binding["Bind"] = []
    tracks = [normalize_track(track) for track in binding.get("Tracks", [])]
    if ("Spawnable" in binding or "Root" in binding) and not any(track.get("Type") == "Spawn" for track in tracks):
        tracks.insert(0, copy.deepcopy(SPAWN_TRACK))
    binding["Tracks"] = tracks
    if "Children" in binding:
        binding["Children"] = [normalize_binding(child) for child in binding["Children"]]
    return binding


def without_group(section):
    """A section without its "Group" (section groups compare as a partition: section_groups / group_diff)."""
    return {key: value for key, value in section.items() if key != "Group"} if type(section) is dict else section


def strip_groups(value):
    """A region with the sections' "Group" fields removed."""
    if type(value) is list:
        return [strip_groups(item) for item in value]
    if type(value) is not dict:
        return value
    return {key: ([without_group(strip_groups(item)) for item in item_list] if key == "Sections" and type(item_list) is list
                  else strip_groups(item_list))
            for key, item_list in value.items()}


def section_groups(regions, base="/Body"):
    """{document path of a section: its Group} over these regions ({Body key: value})."""
    out = {}

    def walk(value, path):
        if type(value) is dict:
            for key, item in value.items():
                if key == "Sections" and type(item) is list:
                    for index, section in enumerate(item):
                        if type(section) is dict and section.get("Group") is not None:
                            out[f"{path}/Sections/{index}"] = section["Group"]
                else:
                    walk(item, f"{path}/{escape(key)}")
        elif type(value) is list:
            for index, item in enumerate(value):
                walk(item, f"{path}/{index}")
    for key, value in regions.items():
        walk(value, f"{base}/{key}")
    return out


def _mates(groups):
    members = {}
    for path, number in groups.items():
        members.setdefault(number, set()).add(path)
    return {path: frozenset(members[number]) for path, number in groups.items()}


def group_diff(authored_regions, live_regions):
    """Sections whose section group (the set of sections grouped with them) differs; the numbers themselves are
    names only."""
    mine, theirs = section_groups(authored_regions), section_groups(live_regions)
    wanted, have = _mates(mine), _mates(theirs)
    changes = []
    for path in sorted(set(wanted) | set(have)):
        if wanted.get(path) == have.get(path):
            continue
        kind = DiffKind.ADD if path not in theirs else DiffKind.REMOVE if path not in mine else DiffKind.REPLACE
        changes.append(Difference(f"{path}/Group", kind, path in mine, mine.get(path), path in theirs, theirs.get(path)))
    return changes


def check_section_groups(regions):
    """Every section group has two or more sections (Sequencer groups a selection of sections)."""
    groups = section_groups(regions)
    for path, mates in _mates(groups).items():
        if len(mates) < 2:
            raise error(f"{path}/Group", f"Section group {groups[path]} has only this section: a section group has two or "
                        "more sections (give the others the same Group, or leave Group out).", "Sequencer.InvalidBody")


def _frame_order(marker):
    frame = marker.get("Frame") if type(marker) is dict else None
    return frame if type(frame) in (int, float) else 0


def normalize_region(key, value):
    """A complete Body region in its comparison form."""
    if key == "Bindings":
        return _drop_defaults([normalize_binding(binding) for binding in value])
    if key == "Tracks":
        return _drop_defaults([normalize_track(track) for track in value])
    if key == "Markers":
        return sorted(value, key=_frame_order)
    if key == "Groups":
        return [{**group, "Members": sorted(group.get("Members", []), key=lambda member: json.dumps(member, sort_keys=True))}
                if type(group) is dict and type(group.get("Members")) is list else group for group in value]
    return value


# ---------------------------------------------------------------------------
# Read records -> document
# ---------------------------------------------------------------------------
def read_records(calls, primitive=READ, header=True, args=None):
    records = []
    offset = 0
    while True:
        page = calls.query(primitive, {**(args or {}), "offset": offset})
        records += page.get("records", [])
        offset = page.get("next_offset", -1)
        if type(offset) is not int or offset < 0:
            break
    if header and (not records or records[0].get("kind") != "header"):
        raise error("/Body", f"{primitive} returned no header.", "Sequencer.InvalidPrimitiveResult")
    return records


def _join_columns(channel, record):
    for key, field in COLUMNS.items():
        if field in record:
            channel.setdefault(key, []).extend(record[field])


def _collapse_columns(channel):
    """Interp one token when uniform, omitted when every key is SmartAuto."""
    interp = channel.get("Interp")
    if type(interp) is list and interp and all(item == interp[0] for item in interp):
        if interp[0] == "SmartAuto":
            del channel["Interp"]
        else:
            channel["Interp"] = interp[0]


def _record_channels(section, record):
    """The Channels of a key record's owner: the section, or the Parameters entry at the record's "parameter" path."""
    owner = section
    for index in record.get("parameter") or []:
        owner = owner["Parameters"][index]
    return owner.setdefault("Channels", {})


def assemble(records, ids=False):
    """Body and read warnings of seq.read.v1 records (header, bindings, tracks each followed by its sections, each
    section followed by its key records, warnings). ``ids``: entries also carry the private ``_Guid`` (bindings) and
    ``_Object`` (tracks, sections) the apply plan addresses them by."""
    header = records[0]
    warnings = []
    entries, roots, order = {}, [], []
    for record in records[1:]:
        if record.get("kind") == "binding":
            entries[record["id"]] = dict(record["entry"])
            if ids:
                entries[record["id"]]["_Guid"] = record["id"]
            order.append(record)
    for record in order:
        entry = entries[record["id"]]
        parent = record.get("parent") or ""
        if parent and parent in entries:
            entries[parent].setdefault("Children", []).append(entry)
        else:
            roots.append(entry)
    tracks = []
    track = section = None
    columned = []
    for record in records[1:]:
        kind = record.get("kind")
        if kind == "track":
            track = dict(record["entry"])
            track["Sections"] = []
            if ids:
                track["_Object"] = record.get("object", "")
            owner = record.get("binding") or ""
            if owner:
                entries[owner].setdefault("Tracks", []).append(track)
            else:
                tracks.append(track)
            section = None
        elif kind == "section":
            section = dict(record["entry"])
            section["Channels"] = dict(section.get("Channels", {}))
            if ids:
                section["_Object"] = record.get("object", "")
            track["Sections"].append(section)
        elif kind == "keys":
            _record_channels(section, record).setdefault(record["channel"], {})["Keys"] = record["keys"]
        elif kind == "columns":
            channel = _record_channels(section, record).setdefault(record["channel"], {})
            _join_columns(channel, record)
            columned.append(channel)
        elif kind == "warning":
            warnings.append({"Code": record.get("code", ""), "Message": record.get("message", ""),
                             "Path": record.get("path", "")})
    for channel in columned:
        _collapse_columns(channel)
    body = {"Settings": header.get("settings", {}), "Bindings": _drop_defaults(roots), "Tracks": _drop_defaults(tracks)}
    if header.get("folders"):
        body["Folders"] = header["folders"]
    if header.get("markers"):
        body["Markers"] = header["markers"]
    if header.get("groups"):
        body["Groups"] = header["groups"]
    if header.get("metadata"):
        body["MetaData"] = header["metadata"]
    return body, {"header": header, "warnings": warnings}


def _positive_zero(value):
    """-0.0 reads as 0.0: a document number "-0" parses back as the integer 0, and both evaluate the same."""
    if type(value) is dict:
        for key, item in value.items():
            value[key] = _positive_zero(item)
    elif type(value) is list:
        for index, item in enumerate(value):
            value[index] = _positive_zero(item)
    elif type(value) is float and value == 0.0:
        return 0.0
    return value


def read_body(calls, ids=False):
    body, facts = assemble(read_records(calls), ids)
    return _positive_zero(body), facts


def director_class(facts, target):
    """The live Director Blueprint's generated class (None: the sequence has no Director)."""
    header = (facts.get("header") or {}).get("director")
    if type(header) is not dict:
        return None
    return header.get("Class") or director.default_class(target)


def document_body(calls, read, context, target):
    """The extracted Body: the read regions and the Director region."""
    body, facts = read_body(calls)
    owner = director_class(facts, target)
    if owner is not None:
        body["Director"] = director.extract(context, read, ROUTE, BY_ID, owner)
    return {key: body[key] for key in BODY_KEYS if key in body}, facts


def extract(target, read, context, include_defaults=True):
    body, _ = document_body(SequencerCalls(context, read), read, context, target)
    return common.document(target, CLASS, _properties().extract(read, context, include_defaults), body)


# ---------------------------------------------------------------------------
# Diff
# ---------------------------------------------------------------------------
def value_diff(path, authored, live, exact, changes):
    """Structural differences: objects key by key, arrays index by index, numbers as float32 unless exact."""
    if type(authored) is dict and type(live) is dict:
        for key in list(authored) + [key for key in live if key not in authored]:
            where = f"{path}/{escape(key)}"
            if key not in live:
                changes.append(Difference(where, DiffKind.ADD, True, authored[key], False, None))
            elif key not in authored:
                changes.append(Difference(where, DiffKind.REMOVE, False, None, True, live[key]))
            else:
                value_diff(where, authored[key], live[key], exact, changes)
        return
    if type(authored) is list and type(live) is list:
        for index in range(max(len(authored), len(live))):
            where = f"{path}/{index}"
            if index >= len(live):
                changes.append(Difference(where, DiffKind.ADD, True, authored[index], False, None))
            elif index >= len(authored):
                changes.append(Difference(where, DiffKind.REMOVE, False, None, True, live[index]))
            else:
                value_diff(where, authored[index], live[index], exact, changes)
        return
    if not common.same(authored, live, exact):
        changes.append(Difference(path, DiffKind.REPLACE, True, authored, True, live))


GROUPED = ("Bindings", "Tracks")


def body_diff(authored, live, exact, groups=True):
    """Differences of the authored Body regions (omitted region = keep) from the live Body, without the Director
    (director_diff). Settings and MetaData are sparse (only listed fields compare); the other regions are complete.
    ``groups``: also the section groups of Bindings and Tracks (group_diff)."""
    changes = []
    for key in BODY_KEYS:
        if key not in authored or key == "Director":
            continue
        path = f"/Body/{key}"
        if key in ("Settings", "MetaData"):
            _sparse_diff(path, authored[key], live.get(key, {}), key, exact, changes)
        else:
            value_diff(path, strip_groups(normalize_region(key, authored[key])),
                       strip_groups(normalize_region(key, live.get(key, []))), exact, changes)
    if groups and any(key in authored for key in GROUPED):
        changes.extend(group_diff({key: normalize_region(key, authored[key] if key in authored else live.get(key, []))
                                   for key in GROUPED},
                                  {key: normalize_region(key, live.get(key, [])) for key in GROUPED}))
    return changes


def omitted_values(region, authored, live):
    """{name: value} that a listed Settings / MetaData resets: the live fields it leaves out (Settings: but the kept
    timing fields), each at a new sequence's value (None: CustomClock / SelectionRange, none)."""
    defaults = SETTINGS_DEFAULTS if region == "Settings" else METADATA_DEFAULTS
    kept = SETTINGS_KEPT if region == "Settings" else ()
    if region == "Settings" and authored.get("Clock") == "Custom":
        # A custom clock source has its clock object (the editor has no "none" for it): omitted keeps it.
        kept = kept + ("CustomClock",)
    return {name: defaults.get(name) for name in live if name not in authored and name not in kept}


def _sparse_diff(path, mine, theirs, region, exact, changes):
    """Listed fields compare with the read (a field the read leaves out is at its default); a live field the document
    leaves out is reset (REMOVE), but the kept timing fields."""
    defaults = SETTINGS_DEFAULTS if region == "Settings" else METADATA_DEFAULTS
    for name in mine:
        if name not in theirs and name in defaults and common.same(mine[name], defaults[name], exact):
            continue
        value_diff(f"{path}/{escape(name)}", mine[name], theirs.get(name), exact, changes)
    for name in omitted_values(region, mine, theirs):
        changes.append(Difference(f"{path}/{escape(name)}", DiffKind.REMOVE, False, None, True, theirs[name]))


def omitted_kept(body, prefix=PREFIX):
    """Info diagnostics of the kept Settings fields a listed Settings leaves out (Diff says they keep their value)."""
    settings = body.get("Settings") if type(body) is dict else None
    if type(settings) is not dict:
        return ()
    omitted = [name for name in SETTINGS_KEPT if name not in settings]
    if not omitted:
        return ()
    return (RuntimeDiagnostic(path="/Body/Settings", code=f"{prefix}.OmittedKept", severity=DiagnosticSeverity.INFO,
                              message=f"{', '.join(omitted)} omitted from Settings: kept (the timing fields keep their "
                                      "value; the other Settings fields omitted take a new sequence's value)."),)


def director_diff(authored, live, target, exact):
    if "Director" not in authored:
        return []
    return director.diff(authored["Director"], live.get("Director"), director.default_class(target), strict=exact)


def diff(authored, extracted, exact):
    changes = common.header_diff(authored, extracted)
    changes.extend(body_diff(authored.get("Body", {}), extracted.get("Body", {}), exact))
    changes.extend(director_diff(authored.get("Body", {}), extracted.get("Body", {}), authored["Target"], exact))
    changes.extend(common.properties_diff(authored.get("Properties", {}), extracted.get("Properties", {}), exact))
    return with_diagnostics(changes, omitted_kept(authored.get("Body", {})))


# ---------------------------------------------------------------------------
# Verify / Diff: authored defaults (the sparse objects leave out a field at its default)
# ---------------------------------------------------------------------------
# Paths asked of one read (the request's value budget); a document listing more defaults than DEFAULTS_AT_LIMIT
# compares without them.
DEFAULTS_AT_CHUNK = 1000
DEFAULTS_AT_LIMIT = 5000


def _leaves(path, value, out):
    """{document path: value} of the scalars and arrays under value (objects compare field by field)."""
    if type(value) is dict and value:
        for key, item in value.items():
            _leaves(f"{path}/{escape(key)}", item, out)
    else:
        out[path] = value
    return out


def absent_paths(changes):
    """Document paths of the values the document lists and the read leaves out (the ADD differences of the Body):
    the candidates for a default the read leaves out."""
    paths = {}
    for change in changes:
        if change.kind is DiffKind.ADD and change.before_present and change.path.startswith("/Body/"):
            _leaves(change.path, change.before, paths)
    return paths


def read_defaults_at(calls, paths):
    """{path: default} for the listed document paths whose field a sparse object of the read leaves out at its
    default (seq.read.v1 defaults_at)."""
    defaults = {}
    paths = sorted(paths)
    for start in range(0, len(paths), DEFAULTS_AT_CHUNK):
        for record in read_records(calls, args={"defaults_at": paths[start:start + DEFAULTS_AT_CHUNK]}):
            if record.get("kind") == "default" and type(record.get("path")) is str:
                defaults[record["path"]] = record.get("value")
    return defaults


def defaults_at(context, read, paths):
    if not paths or len(paths) > DEFAULTS_AT_LIMIT:
        return {}
    return read_defaults_at(SequencerCalls(context, read), paths)


def extract_with_defaults(target, read, context, authored):
    """The document and the defaults of the Body values ``authored`` lists that the read leaves out; with the
    relayout the authored Director graphs' "Layout": "Auto" would make (PENDING_RELAYOUT, for the Diff operation)."""
    document = extract(target, read, context)
    if type(authored) is not dict:
        return document, {}
    mine, theirs = canonicalize(authored), canonicalize(document)
    changes = body_diff(mine.get("Body", {}), theirs.get("Body", {}), False, groups=False)
    defaults = defaults_at(context, read, absent_paths(changes))
    if type(mine.get("Body", {}).get("Director")) is dict and type(theirs.get("Body", {}).get("Director")) is dict:
        pending = director.pending_relayout(context, read, ROUTE, director.default_class(target), mine["Body"]["Director"])
        if pending:
            defaults = {**defaults, PENDING_RELAYOUT: pending}
    return document, defaults


def explained_by_defaults(change, defaults, exact):
    """An ADD difference whose listed values are all defaults the read left out."""
    if change.kind is not DiffKind.ADD or not change.before_present or not defaults:
        return False
    leaves = _leaves(change.path, change.before, {})
    return all(path in defaults and common.same(value, defaults[path], exact) for path, value in leaves.items())


def diff_with_defaults(authored, extracted, defaults, exact):
    changes = diff(authored, extracted, exact)
    kept = tuple(change for change in changes if not explained_by_defaults(change, defaults, exact))
    if exact:
        kept += tuple(relayout_differences(defaults))
    return with_diagnostics(kept, getattr(changes, "diagnostics", ()))


# ---------------------------------------------------------------------------
# Diff paths: the comparison form -> the authored document
# ---------------------------------------------------------------------------
# The comparison form differs from the document in three places: a spawnable (or a template's root) that lists no
# Spawn track gets the default one first (normalize_binding), the camera cut track is moved first (camera_cut_first)
# and Markers are sorted by frame. Diff, validate's changes and the verify check report paths into the document.
def _items(value):
    return value if type(value) is list else []


def _spawn_track_inserted(binding, spawnable):
    return type(binding) is dict and spawnable and not any(
        type(track) is dict and track.get("Type") == "Spawn" for track in _items(binding.get("Tracks")))


def _binding_index_maps(binding, path, maps):
    """Index maps of the Tracks of this binding and its children ({comparison index: document index}, None: the
    default Spawn track the document leaves out)."""
    if type(binding) is not dict:
        return
    if _spawn_track_inserted(binding, "Spawnable" in binding or "Root" in binding):
        maps[f"{path}/Tracks"] = lambda index: None if index == 0 else index - 1
    for number, child in enumerate(_items(binding.get("Children"))):
        _binding_index_maps(child, f"{path}/Children/{number}", maps)


def _permutation(order):
    """{comparison index: document index} of a list the comparison form reorders (order: document indices in
    comparison order)."""
    return lambda index: order[index] if index < len(order) else index


def body_index_maps(body, root_path=None):
    """{document path of a list: index map} of the authored Body. ``root_path``: a template's Body.Root (always a
    spawnable)."""
    maps = {}
    if type(body) is not dict:
        return maps
    for number, binding in enumerate(_items(body.get("Bindings"))):
        _binding_index_maps(binding, f"/Body/Bindings/{number}", maps)
    if root_path is not None and _spawn_track_inserted(body.get("Root"), True):
        maps[f"{root_path}/Tracks"] = lambda index: None if index == 0 else index - 1
    tracks = _items(body.get("Tracks"))
    cut = next((number for number, track in enumerate(tracks)
                if type(track) is dict and track.get("Type") in CAMERA_CUT_TYPES), None) if root_path is None else None
    if cut:
        maps["/Body/Tracks"] = _permutation([cut] + [number for number in range(len(tracks)) if number != cut])
    markers = _items(body.get("Markers"))
    order = sorted(range(len(markers)), key=lambda number: _frame_order(markers[number]))
    if order != list(range(len(markers))):
        maps["/Body/Markers"] = _permutation(order)
    return maps


def document_path(path, maps):
    """A comparison-form path as a path into the authored document."""
    for prefix in sorted(maps, key=len, reverse=True):
        if not path.startswith(prefix + "/"):
            continue
        index, _, rest = path[len(prefix) + 1:].partition("/")
        if not index.isdigit():
            return path
        mapped = maps[prefix](int(index))
        if mapped is None:
            return prefix
        return f"{prefix}/{mapped}" + (f"/{rest}" if rest else "")
    return path


def document_paths(changes, document, root_path=None):
    """The differences with paths into the authored ``document`` (see body_index_maps)."""
    notes = getattr(changes, "diagnostics", ())
    maps = body_index_maps(document.get("Body") if type(document) is dict else None, root_path)
    if not maps:
        return with_diagnostics(changes, notes)
    return with_diagnostics((dataclasses.replace(change, path=document_path(change.path, maps)) for change in changes),
                            notes)


def existing(context, document):
    return ik.existing(context, document, CLASS, PREFIX)


# ---------------------------------------------------------------------------
# Inspect
# ---------------------------------------------------------------------------
def inspect_payload(document, warnings, calls):
    bindings, properties, track_classes = [], {}, []
    director, references, status = None, [], {}
    for record in read_records(calls, INSPECT, header=False):
        kind = record.get("kind")
        if kind == "binding":
            row = {"Path": record.get("path", ""), "DocumentPath": record.get("document_path", ""),
                   "Guid": record.get("guid", ""), "Kind": record.get("binding_kind", ""),
                   "Locators": record.get("locators", [])}
            if record.get("class"):
                row["Class"] = record["class"]
            bindings.append(row)
        elif kind == "properties":
            properties[record["class"]] = record.get("properties", [])
        elif kind == "track_class":
            track_classes.append({"Class": record.get("class", ""), "Type": record.get("type", ""),
                                  "Sections": record.get("sections", [])})
        elif kind == "director":
            director = record.get("director")
        elif kind == "references":
            references = record.get("assets", [])
        elif kind == "status":
            status = record.get("compile", {})
    return {**document, "BindingFacts": bindings, "AnimatableProperties": properties, "TrackClasses": track_classes,
            "DirectorEndpoints": director, "References": references, "ReadWarnings": warnings, "Status": status}


# ---------------------------------------------------------------------------
# Handler
# ---------------------------------------------------------------------------
class LevelSequenceRuntimeProfile:
    runtime_entry_module = "asset_document/bootstrap.py"
    # Snapshot-covered: preflight never reads a preview.
    preflight_scratch = False
    exact_asset_class_path = CLASS
    profile_id = PROFILE_ID

    def required_primitives(self):
        return REQUIREMENTS

    def inspect_profile(self):
        return {"Class": CLASS, "BodySections": list(BODY_KEYS), "ReadOnlySections": []}

    def create_template(self, target):
        return common.document(common.require_target(target, PREFIX), CLASS, {}, {})

    def validate(self, document):
        canonicalize(document)

    def canonicalize(self, document):
        return canonicalize(document)

    def create_target_context(self, document, context):
        canonicalize(document)
        return ()

    def preflight(self, document, scratch_handle, context):
        canonical = canonicalize(document)
        _properties().validate(canonical.get("Properties", {}), context)
        _apply.preflight(canonical, existing(context, canonical), context)
        return SNAPSHOT_COVERED

    def apply(self, document, read_asset_handle, mutable_asset_handle, context):
        return _apply.apply(canonicalize(document), read_asset_handle, mutable_asset_handle, context)

    def repair(self, document, asset_handle, context):
        return _apply.repair(canonicalize(document), asset_handle, context)

    def extract(self, *, target, asset_handle, context, include_defaults=False):
        return extract(common.require_target(target, PREFIX), asset_handle, context, include_defaults)

    def inspect(self, *, mode, exact_class_path, exact_asset_object_path, editor_asset_handle, context):
        if mode != "live_asset" or exact_class_path != CLASS or editor_asset_handle is None:
            raise error("/Inspect", "Level Sequence inspection reads a live Level Sequence.",
                        "Sequencer.InspectNotAdmitted")
        target = common.require_target(exact_asset_object_path.rsplit(".", 1)[0], PREFIX)
        calls = SequencerCalls(context, editor_asset_handle)
        body, facts = document_body(calls, editor_asset_handle, context, target)
        document = common.document(target, CLASS, _properties().extract(editor_asset_handle, context), body)
        return inspect_payload(document, facts["warnings"], calls)

    def diff(self, authored, extracted):
        return document_paths(diff(canonicalize(authored), canonicalize(extracted), exact=False), authored)

    def diff_exact(self, authored, extracted):
        return document_paths(diff(canonicalize(authored), canonicalize(extracted), exact=True), authored)

    def extract_with_defaults(self, *, target, asset_handle, context, authored=None):
        return extract_with_defaults(common.require_target(target, PREFIX), asset_handle, context, authored)

    def diff_with_defaults(self, authored, extracted, live_defaults, exact):
        return document_paths(diff_with_defaults(canonicalize(authored), canonicalize(extracted), live_defaults, exact),
                              authored)


def _meta():
    rows = [("settings", "/Body/Settings"), ("bindings", "/Body/Bindings"), ("tracks", "/Body/Tracks"),
            ("folders", "/Body/Folders"), ("markers", "/Body/Markers"), ("groups", "/Body/Groups"),
            ("director", "/Body/Director"),
            ("metadata", "/Body/MetaData")]
    regions = tuple(RegionMeta(f"level_sequence.{name}.v1", path, RegionOwnership.MANAGED, IdentityPolicy.STABLE_KEY,
                               OrderPolicy.CANONICAL, DefaultPolicy.EXPLICIT, OmissionPolicy.PRESERVE,
                               DeletePolicy.EXPLICIT_ONLY, ComparisonPolicy.CANONICAL)
                    for name, path in rows)
    return ProfileMeta(PROFILE_ID, ROUTE, CLASS, regions, REQUIREMENTS)


LEVEL_SEQUENCE_RUNTIME_REGISTRATION = RuntimeProfileRegistration(_meta(), LevelSequenceRuntimeProfile,
                                                                 RuntimeTargetPolicy.CREATE_OR_UPDATE, LIFECYCLE)

__all__ = ("LEVEL_SEQUENCE_RUNTIME_REGISTRATION", "LevelSequenceRuntimeProfile", "assemble", "body_diff",
           "canonicalize", "check_section_groups", "diff", "director_diff", "group_diff", "normalize_binding",
           "normalize_region", "normalize_track", "strip_groups", "without_group")

# Module-level on purpose (after everything level_sequence_apply uses from this module): the native runtime restores
# sys.modules between phase calls, so an in-call import would load a second copy of the primitive catalog.
from asset_document.profiles import level_sequence_apply as _apply  # noqa: E402
