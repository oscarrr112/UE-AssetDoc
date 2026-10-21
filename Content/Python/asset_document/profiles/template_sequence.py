"""Registered Template Sequence Profiles (design §3.2, §3.4.3, §7.2):

* ``profile.template_sequence.v1``          class /Script/TemplateSequence.TemplateSequence
* ``profile.camera_animation_sequence.v1``  class /Script/TemplateSequence.CameraAnimationSequence (a Template
  Sequence whose root is a camera; played as a camera shake or by the camera animation subsystem)

Document (MCP/schemas/assetdocument/Sequencer.md, "Template Sequence")::

    "Body": {
      "Settings": {...},                                  (as a Level Sequence)
      "Root": {"Name": "CineCameraActor", "Class": "/Script/CinematicCamera.CineCameraActor",
               "Template": {...}?, "Ownership"?, "ContinuouslyRespawn"?, "NetAddressableName"?, "LevelName"?,
               "SpawnTransform"?, "Tags"?, "Folder"?,
               "Tracks": [{"Type": "Transform", ...}],     (a Spawn track is the default, as the factory makes it)
               "Children": [{"Name": "CameraComponent", "Class": "/Script/CinematicCamera.CineCameraComponent",
                             "Component"?: "CameraComponent", "Tags"?, "Tracks": [...]}]},
      "Tracks": [...], "Folders": [...], "Markers": [...], "Groups": [...]
    }

* ``Root`` is the root spawnable (UTemplateSequence's first spawnable): ``Class`` its bound actor class
  (``BoundActorClass``; changing it is the Details panel's class picker: a new root of that class gets the old root's
  tracks and component bindings), ``Template`` the spawned actor's saved default state, the other fields the
  spawnable's Details. ``Name`` omitted keeps the root's name.
* ``Children`` are component bindings: ``Component`` names the component of the spawned actor the binding animates
  (``BoundActorComponents``; omitted = the binding's Name, ``""`` = none); ``Class`` the binding's object class.
* Creating a Template Sequence needs ``Root.Class`` (the class the New Template Sequence dialog asks for); a Camera
  Animation Sequence is created with a Cine Camera Actor root (its factory) and takes Camera Actor classes only.
* Everything else (Settings, tracks, sections and their groups, channels, folders, markers, node groups) is the Level
  Sequence profile's (a Template Sequence has no Director Blueprint); the plan is
  ``level_sequence_apply.Plan`` over the root as a binding tree.
"""
from __future__ import annotations

from asset_document.diagnostics import AssetDocumentContractError, with_diagnostics
from asset_document.meta import (
    ComparisonPolicy, DefaultPolicy, DeletePolicy, IdentityPolicy, OmissionPolicy, OrderPolicy, ProfileMeta,
    RegionMeta, RegionOwnership,
)
from asset_document.mutation_intent_projection import SNAPSHOT_COVERED
from asset_document.profile_runtime import RuntimeProfileRegistration, RuntimeTargetPolicy
from asset_document.profiles import data_table_common as common
from asset_document.profiles import ik_common as ik
from asset_document.profiles import level_sequence as seq
from asset_document.regions.primitive_catalog import NamedOpaqueHandleArgument, _single_handle
from asset_document.regions.target_lifecycle import generic_factory_lifecycle, lifecycle_requirements

TEMPLATE_CLASS = "/Script/TemplateSequence.TemplateSequence"
CAMERA_CLASS = "/Script/TemplateSequence.CameraAnimationSequence"
TEMPLATE_PROFILE_ID = "profile.template_sequence.v1"
CAMERA_PROFILE_ID = "profile.camera_animation_sequence.v1"
PREFIX = seq.PREFIX
ACTOR_BASE = "/Script/Engine.Actor"
CAMERA_BASE = "/Script/Engine.CameraActor"
CLASS_CLASS = "/Script/CoreUObject.Class"

BODY_KEYS = ("Settings", "Root", "Tracks", "Folders", "Markers", "Groups")
ROOT_PATH = "/Body/Root"
GROUPED = ("Root", "Tracks")
# The root spawnable's own fields (seq.read.v1 entry "Root").
SPAWNABLE_KEYS = ("Class", "Template", "Ownership", "ContinuouslyRespawn", "NetAddressableName", "LevelName", "SpawnTransform")
ROOT_KEYS = ("Name",) + SPAWNABLE_KEYS + ("Tags", "Folder", "Tracks", "Children")
CHILD_KEYS = ("Name", "RenamedFrom", "Id", "Class", "Component", "Tags", "Tracks")

BODY_OWNED = {"MovieScene": "the Body regions (Settings, Root, Tracks, Folders, Markers)",
              "BoundActorClass": "Body/Root/Class", "BoundActorComponents": "Body/Root/Children (Component)"}
NOT_AUTHORED = ("ThumbnailInfo",)


def _requirements(*primitives):
    return tuple(seq._requirements(*primitives))


TEMPLATE_LIFECYCLE = generic_factory_lifecycle(seq.ROUTE, ("factory_context",))
CAMERA_LIFECYCLE = generic_factory_lifecycle(seq.ROUTE)
_PRIMITIVES = (common.CLASS_RESOLVE, common.RESOLVE, common.REFERENCE_RESOLVE, seq.READ, seq.INSPECT, seq.CATALOG, seq.EDIT,
               seq.STATUS)
TEMPLATE_REQUIREMENTS = _requirements(*_PRIMITIVES) + lifecycle_requirements(TEMPLATE_LIFECYCLE)
CAMERA_REQUIREMENTS = _requirements(*_PRIMITIVES) + lifecycle_requirements(CAMERA_LIFECYCLE)
BY_ID = {item.primitive_id: item for item in TEMPLATE_REQUIREMENTS + CAMERA_REQUIREMENTS}

error = seq.error
escape = seq.escape


def _properties(class_path):
    return ik.RigProperties(class_path, BODY_OWNED, NOT_AUTHORED, PREFIX)


def _base(class_path):
    return CAMERA_BASE if class_path == CAMERA_CLASS else ACTOR_BASE


# ---------------------------------------------------------------------------
# Document shape
# ---------------------------------------------------------------------------
def validate_body(body):
    if "Root" in body:
        root = ik.object_value(body["Root"], "/Body/Root", "Root", PREFIX)
        ik.check_keys(root, ROOT_KEYS, "/Body/Root", "Root", PREFIX)
        if "Name" in root:
            ik.name_value(root["Name"], "/Body/Root/Name", "The root's Name", PREFIX)
        if "Class" in root and (type(root["Class"]) is not str or not root["Class"]):
            raise error("/Body/Root/Class", "Root.Class is the bound actor class path, e.g. "
                        "/Script/CinematicCamera.CineCameraActor.", "Sequencer.InvalidBody")
        if "Tracks" in root:
            seq._validate_tracks(root["Tracks"], "/Body/Root/Tracks")
        for index, child in enumerate(seq._list(root.get("Children", []), "/Body/Root/Children", "Children")):
            where = f"/Body/Root/Children/{index}"
            ik.check_keys(ik.object_value(child, where, "A component binding", PREFIX), CHILD_KEYS, where, "component binding", PREFIX)
            ik.name_value(child.get("Name"), f"{where}/Name", "A component binding's Name", PREFIX)
            for key in ("Class", "Component"):
                if key in child and type(child[key]) is not str:
                    raise error(f"{where}/{key}", f"{key} is a string.", "Sequencer.InvalidBody")
            if "Tracks" in child:
                seq._validate_tracks(child["Tracks"], f"{where}/Tracks")
    level = {key: value for key, value in body.items() if key in ("Settings", "Tracks", "Folders", "Markers", "Groups")}
    seq.validate_body(level)


def canonicalize(document, class_path):
    body = common.validate_header(document, (class_path,), PREFIX, BODY_KEYS)
    validate_body(body)
    canonical = common.snapshot(document)
    canonical.setdefault("Definitions", {})
    canonical.setdefault("Properties", {})
    canonical["Body"] = {key: (seq._drop_defaults(value) if key in ("Root", "Tracks") else value)
                         for key, value in canonical.get("Body", {}).items()}
    return canonical


# ---------------------------------------------------------------------------
# Body.Root <-> the Level Sequence profile's binding form (one root binding: kind Root, children: kind Component)
# ---------------------------------------------------------------------------
def to_internal(root, reference=None):
    """Body.Root as a level binding. ``reference``: the live Body.Root (template form) that omitted Name / Class
    keep; the root is matched to it (RenamedFrom)."""
    reference = reference or {}
    entry = {"Name": root.get("Name") or reference.get("Name") or ""}
    if reference.get("Name") and reference["Name"] != entry["Name"]:
        entry["RenamedFrom"] = reference["Name"]
    spawnable = {key: root[key] for key in SPAWNABLE_KEYS if key in root}
    if "Class" not in spawnable and reference.get("Class"):
        spawnable["Class"] = reference["Class"]
    entry["Root"] = spawnable
    for key in ("Tags", "Folder", "Tracks"):
        if key in root:
            entry[key] = root[key]
    children = []
    for child in root.get("Children", []):
        item = {key: child[key] for key in ("Name", "RenamedFrom", "Id", "Class") if key in child}
        item["Component"] = child.get("Component", child.get("Name", ""))
        for key in ("Tags", "Tracks"):
            if key in child:
                item[key] = child[key]
        children.append(item)
    if children:
        entry["Children"] = children
    return entry


def from_internal(binding):
    """A level root binding (seq.read.v1 entry, assembled) as Body.Root."""
    binding = seq._public(binding)
    root = {"Name": binding.get("Name", "")}
    root.update(binding.get("Root", {}))
    for key in ("Tags", "Folder", "Tracks"):
        if key in binding:
            root[key] = binding[key]
    children = []
    for child in binding.get("Children", []):
        child = seq._public(child)
        item = {key: child[key] for key in ("Name", "Id", "Class") if key in child}
        if child.get("Component", child.get("Name")) != child.get("Name"):
            item["Component"] = child.get("Component", "")
        for key in ("Tags", "Tracks"):
            if key in child:
                item[key] = child[key]
        children.append(item)
    if children:
        root["Children"] = children
    return root


def template_body(level_body):
    """The template document Body of a level-form body (seq.read.v1 assembled)."""
    body = {"Settings": level_body.get("Settings", {})}
    bindings = level_body.get("Bindings", [])
    if bindings:
        body["Root"] = from_internal(bindings[0])
    body["Tracks"] = level_body.get("Tracks", [])
    for key in ("Folders", "Markers", "Groups"):
        if key in level_body:
            body[key] = level_body[key]
    return body


def normalize_root(root, reference):
    """Body.Root as compared: Name / Class from the live root when omitted, the default Spawn track, no
    RenamedFrom."""
    return from_internal(seq.normalize_region("Bindings", [to_internal(root, reference)])[0])


def body_diff(authored, live, exact):
    """Differences of the authored Body regions (omitted = keep) from the live Body (both template form)."""
    changes = []
    for key in BODY_KEYS:
        if key not in authored:
            continue
        if key == "Root":
            reference = live.get("Root")
            seq.value_diff("/Body/Root", seq.strip_groups(normalize_root(authored["Root"], reference)),
                           seq.strip_groups(normalize_root(reference, reference)) if reference is not None else None,
                           exact, changes)
        else:
            changes.extend(seq.body_diff({key: authored[key]}, live, exact, groups=False))
    if any(key in authored for key in GROUPED):
        changes.extend(seq.group_diff(_grouped(authored, live), _grouped(live, live)))
    return changes


def _grouped(body, live):
    """Root and Tracks (the body's, else the live ones) in their comparison form, for the section groups."""
    reference = live.get("Root")
    root = body.get("Root", reference)
    return {"Root": normalize_root(root, reference) if root is not None else None,
            "Tracks": seq.normalize_region("Tracks", body["Tracks"] if "Tracks" in body else live.get("Tracks", []))}


def diff(authored, extracted, exact):
    changes = common.header_diff(authored, extracted)
    changes.extend(body_diff(authored.get("Body", {}), extracted.get("Body", {}), exact))
    changes.extend(common.properties_diff(authored.get("Properties", {}), extracted.get("Properties", {}), exact))
    return with_diagnostics(changes, seq.omitted_kept(authored.get("Body", {})))


def read_body(calls):
    level, facts = seq.read_body(calls)
    return template_body(level), facts


def extract(target, read, context, class_path, include_defaults=True):
    body, _ = read_body(seq.SequencerCalls(context, read))
    return common.document(target, class_path, _properties(class_path).extract(read, context, include_defaults), body)


# ---------------------------------------------------------------------------
# Refusals that need no write
# ---------------------------------------------------------------------------
def refuse(body, live, class_path):
    root = body.get("Root")
    live_root = live.get("Root")
    if root is not None and not root.get("Class") and live_root is None:
        raise error("/Body/Root/Class", "The template has no root yet: Root.Class names the actor class it animates (the "
                    "Bound Actor Class), e.g. /Script/CinematicCamera.CineCameraActor.", "Sequencer.RootClassRequired")
    if any(key in body for key in GROUPED):
        seq.check_section_groups({key: body[key] if key in body else live.get(key) for key in GROUPED})
    folders = {item.get("Path") for item in (body["Folders"] if "Folders" in body else live.get("Folders", []))}
    entries = [("/Body/Root", root)] if root is not None else []
    entries += [(f"/Body/Tracks/{index}", track) for index, track in enumerate(body.get("Tracks", []))]
    for path, entry in entries:
        if entry.get("Folder") and entry["Folder"] not in folders:
            raise error(f"{path}/Folder", f"Folder '{entry['Folder']}' is not listed in Body.Folders.", "Sequencer.InvalidFolder")
    for index, child in enumerate((root or {}).get("Children", [])):
        if child.get("Id") and not _apply.GUID.match(child["Id"]):
            raise error(f"/Body/Root/Children/{index}/Id", "Id is a binding GUID (32 hex digits).", "Sequencer.InvalidBody")


# ---------------------------------------------------------------------------
# Apply
# ---------------------------------------------------------------------------
def _live(calls):
    level, _ = seq.read_body(calls, ids=True)
    return level, template_body(level)


def apply(canonical, read, mutable, context, class_path):
    """As the Level Sequence apply (rate change first, then structure, content, the rest), with the root's class
    first: a root of another class (or the first root) is made by template.root alone and the rest is planned
    against a read of the new root."""
    body = canonical.get("Body", {})
    region = _properties(class_path)
    properties = canonical.get("Properties", {})
    region.check_owned(properties)
    calls = seq.SequencerCalls(context, read, mutable)
    level, live = _live(calls)
    changed = []
    warnings = []
    if body_diff(body, live, exact=False):
        refuse(body, live, class_path)
        before = _apply.status(calls)
        edits = _apply.Edits(calls)
        settings = _apply.settings_op(body, live)
        if settings is not None and any(key in settings.op["values"] for key in ("DisplayRate", "TickResolution")):
            edits.run([settings])
            level, live = _live(calls)
        root = body.get("Root")
        if root is not None and root.get("Class") and root["Class"] != (live.get("Root") or {}).get("Class"):
            edits.run([_apply.Op({"op": "template.root", "class": root["Class"]}, "/Body/Root", {"class": "/Body/Root/Class"})])
            level, live = _live(calls)
        internal = {key: value for key, value in body.items() if key != "Root"}
        if root is not None:
            internal["Bindings"] = [to_internal(root, live.get("Root"))]
        plan = _apply.Plan(internal, level, root_path="/Body/Root")
        plan.match()
        edits.run(plan.structure())
        edits.run(plan.content())
        edits.run(plan.finish())
        changed.extend(edits.changed)
        failure, outcome = _apply.validation_outcome(before, _apply.status(calls))
        if failure is not None:
            raise failure
        warnings.extend(plan.warnings)
        warnings.extend(outcome)
    changed.extend(region.apply(properties, read, mutable, context))
    return with_diagnostics(changed, warnings)


def preflight(canonical, read, context, class_path):
    body = canonical.get("Body", {})
    if read is None:
        refuse(body, {}, class_path)
        return
    _, live = _live(seq.SequencerCalls(context, read))
    if body_diff(body, live, exact=False):
        refuse(body, live, class_path)


def create_target_context(document, context, class_path):
    """A Template Sequence's factory takes the bound actor class (the New Template Sequence dialog's class picker);
    a Camera Animation Sequence's factory always makes a Cine Camera Actor root."""
    if class_path != TEMPLATE_CLASS:
        return ()
    root = document.get("Body", {}).get("Root") or {}
    if not root.get("Class"):
        raise error("/Body/Root/Class", "Creating a Template Sequence requires Body.Root.Class (the actor class the New "
                    "Template Sequence dialog asks for), e.g. /Script/CinematicCamera.CineCameraActor. list_types (kind "
                    f"class, base {ACTOR_BASE}) lists actor classes.", "Sequencer.RootClassRequired")
    path = root["Class"]
    try:
        common.invoke(context, BY_ID, common.CLASS_RESOLVE, {"class": path, "expected_base": ACTOR_BASE},
                      path="/Body/Root/Class", prefix=PREFIX)
        outcome = common.invoke(context, BY_ID, common.REFERENCE_RESOLVE, {"expected_class": CLASS_CLASS, "path": path},
                                path="/Body/Root/Class", prefix=PREFIX)
    except AssetDocumentContractError as failure:
        raise error("/Body/Root/Class", f"Root class '{path}' is not a loadable actor class: {failure.diagnostic.message} "
                    f"list_types (kind class, base {ACTOR_BASE}) lists actor classes.", "Sequencer.RootClassInvalid") from None
    return (NamedOpaqueHandleArgument("factory_context", _single_handle(outcome, "asset")),)


# ---------------------------------------------------------------------------
# Handlers
# ---------------------------------------------------------------------------
class _TemplateSequenceProfile:
    runtime_entry_module = "asset_document/bootstrap.py"
    preflight_scratch = False
    exact_asset_class_path = TEMPLATE_CLASS
    profile_id = TEMPLATE_PROFILE_ID
    requirements = TEMPLATE_REQUIREMENTS
    label = "Template Sequence"

    def required_primitives(self):
        return self.requirements

    def inspect_profile(self):
        return {"Class": self.exact_asset_class_path, "BodySections": list(BODY_KEYS), "ReadOnlySections": []}

    def create_template(self, target):
        return common.document(common.require_target(target, PREFIX), self.exact_asset_class_path, {}, {})

    def validate(self, document):
        canonicalize(document, self.exact_asset_class_path)

    def canonicalize(self, document):
        return canonicalize(document, self.exact_asset_class_path)

    def create_target_context(self, document, context):
        return create_target_context(canonicalize(document, self.exact_asset_class_path), context, self.exact_asset_class_path)

    def preflight(self, document, scratch_handle, context):
        canonical = canonicalize(document, self.exact_asset_class_path)
        _properties(self.exact_asset_class_path).validate(canonical.get("Properties", {}), context)
        preflight(canonical, ik.existing(context, canonical, self.exact_asset_class_path, PREFIX), context,
                  self.exact_asset_class_path)
        return SNAPSHOT_COVERED

    def apply(self, document, read_asset_handle, mutable_asset_handle, context):
        return apply(canonicalize(document, self.exact_asset_class_path), read_asset_handle, mutable_asset_handle, context,
                     self.exact_asset_class_path)

    def repair(self, document, asset_handle, context):
        canonicalize(document, self.exact_asset_class_path)
        return ()

    def extract(self, *, target, asset_handle, context, include_defaults=False):
        return extract(common.require_target(target, PREFIX), asset_handle, context, self.exact_asset_class_path,
                       include_defaults)

    def inspect(self, *, mode, exact_class_path, exact_asset_object_path, editor_asset_handle, context):
        if mode != "live_asset" or exact_class_path != self.exact_asset_class_path or editor_asset_handle is None:
            raise error("/Inspect", f"{self.label} inspection reads a live {self.label}.", "Sequencer.InspectNotAdmitted")
        target = common.require_target(exact_asset_object_path.rsplit(".", 1)[0], PREFIX)
        calls = seq.SequencerCalls(context, editor_asset_handle)
        body, facts = read_body(calls)
        document = common.document(target, self.exact_asset_class_path,
                                   _properties(self.exact_asset_class_path).extract(editor_asset_handle, context), body)
        return seq.inspect_payload(document, facts["warnings"], calls)

    def diff(self, authored, extracted):
        return seq.document_paths(diff(canonicalize(authored, self.exact_asset_class_path),
                                       canonicalize(extracted, self.exact_asset_class_path), exact=False),
                                  authored, ROOT_PATH)

    def diff_exact(self, authored, extracted):
        return seq.document_paths(diff(canonicalize(authored, self.exact_asset_class_path),
                                       canonicalize(extracted, self.exact_asset_class_path), exact=True),
                                  authored, ROOT_PATH)

    def extract_with_defaults(self, *, target, asset_handle, context, authored=None):
        """The document and the defaults of the Body values ``authored`` lists that the read leaves out (see
        level_sequence.extract_with_defaults)."""
        document = self.extract(target=target, asset_handle=asset_handle, context=context)
        if type(authored) is not dict:
            return document, {}
        mine = canonicalize(authored, self.exact_asset_class_path)
        theirs = canonicalize(document, self.exact_asset_class_path)
        changes = body_diff(mine.get("Body", {}), theirs.get("Body", {}), False)
        return document, seq.defaults_at(context, asset_handle, seq.absent_paths(changes))

    def diff_with_defaults(self, authored, extracted, live_defaults, exact):
        changes = diff(canonicalize(authored, self.exact_asset_class_path),
                       canonicalize(extracted, self.exact_asset_class_path), exact)
        return seq.document_paths(with_diagnostics((change for change in changes
                                                    if not seq.explained_by_defaults(change, live_defaults, exact)),
                                                   getattr(changes, "diagnostics", ())), authored, ROOT_PATH)


class TemplateSequenceRuntimeProfile(_TemplateSequenceProfile):
    pass


class CameraAnimationSequenceRuntimeProfile(_TemplateSequenceProfile):
    exact_asset_class_path = CAMERA_CLASS
    profile_id = CAMERA_PROFILE_ID
    requirements = CAMERA_REQUIREMENTS
    label = "Camera Animation Sequence"


def _meta(profile_id, class_path, requirements, name):
    rows = [("settings", "/Body/Settings"), ("root", "/Body/Root"), ("tracks", "/Body/Tracks"), ("folders", "/Body/Folders"),
            ("markers", "/Body/Markers")]
    regions = tuple(RegionMeta(f"{name}.{row}.v1", path, RegionOwnership.MANAGED, IdentityPolicy.STABLE_KEY,
                               OrderPolicy.CANONICAL, DefaultPolicy.EXPLICIT, OmissionPolicy.PRESERVE,
                               DeletePolicy.EXPLICIT_ONLY, ComparisonPolicy.CANONICAL)
                    for row, path in rows)
    return ProfileMeta(profile_id, seq.ROUTE, class_path, regions, requirements)


TEMPLATE_SEQUENCE_RUNTIME_REGISTRATION = RuntimeProfileRegistration(
    _meta(TEMPLATE_PROFILE_ID, TEMPLATE_CLASS, TEMPLATE_REQUIREMENTS, "template_sequence"), TemplateSequenceRuntimeProfile,
    RuntimeTargetPolicy.CREATE_OR_UPDATE, TEMPLATE_LIFECYCLE)
CAMERA_ANIMATION_SEQUENCE_RUNTIME_REGISTRATION = RuntimeProfileRegistration(
    _meta(CAMERA_PROFILE_ID, CAMERA_CLASS, CAMERA_REQUIREMENTS, "camera_animation_sequence"),
    CameraAnimationSequenceRuntimeProfile, RuntimeTargetPolicy.CREATE_OR_UPDATE, CAMERA_LIFECYCLE)

__all__ = ("CAMERA_ANIMATION_SEQUENCE_RUNTIME_REGISTRATION", "CameraAnimationSequenceRuntimeProfile",
           "TEMPLATE_SEQUENCE_RUNTIME_REGISTRATION", "TemplateSequenceRuntimeProfile", "body_diff", "canonicalize", "diff",
           "from_internal", "normalize_root", "template_body", "to_internal")

# Module-level on purpose (see level_sequence.py).
from asset_document.profiles import level_sequence_apply as _apply  # noqa: E402
