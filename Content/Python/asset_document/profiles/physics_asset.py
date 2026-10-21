"""Registered Physics Asset Profile (profile.physics_asset.v1, class /Script/Engine.PhysicsAsset).

Document (MCP/schemas/assetdocument/PhysicsAsset.md)::

    "Properties": {"ConstraintProfiles": ["Ragdoll"], "PhysicalAnimationProfiles": ["HitReact"]},
    "Body": {
      "PreviewMesh": "/Game/Characters/SKM_Hero.SKM_Hero",
      "Bodies": {
        "upperarm_l": {"Properties": {"DefaultInstance": {"bOverrideMass": true, "MassInKgOverride": 4.0}},
                       "Shapes": {"Capsules": [{"Center": {"X": 14, "Y": 0, "Z": 0}, "Radius": 6.5, "Length": 18}]},
                       "PhysicalAnimation": {"HitReact": {"OrientationStrength": 500}}},
        "lowerarm_l": {"Shapes": {"Boxes": [{"X": 12, "Y": 4, "Z": 8}]}}
      },
      "Constraints": {
        "lowerarm_l": {"Child": "lowerarm_l", "Parent": "upperarm_l",
                       "Properties": {"ProfileInstance": {"TwistLimit": {"TwistMotion": "ACM_Locked"}}},
                       "Profiles": {"Ragdoll": {"ConeLimit": {"Swing2LimitDegrees": 120}}}}
      },
      "CollisionDisabled": [["upperarm_l", "lowerarm_l"]]
    }

* Body regions are complete state when present and kept when omitted. Bodies are keyed by bone, constraints by
  joint name (``RenamedFrom`` renames). Inside an entry every key is sparse: an omitted ``Properties`` /
  ``PhysicalAnimation`` / ``Profiles`` field has the value of a new body / constraint, omitted ``Frames`` are the
  editor's "Snap" frames; ``Shapes`` omitted keeps a body's shapes.
* Extract reads the asset as stored (constraint profile values from the default profile a save writes).
* Apply (``physics_asset_apply``) makes the Physics Asset Editor's edits.
* Create is the editor's "Create > Physics Asset" on a skeletal mesh without its dialog (design §4.3): the mesh is
  the top-level ``FactoryContext`` (else ``Body.PreviewMesh``); the new asset is never assigned to the mesh. With
  ``Bodies`` the bodies are the document's; without, the editor's generation runs with the project's generation
  settings and the create-only ``Body.Generation`` over them. A new body's create-only ``Fit`` fits its shapes from
  the mesh as the editor's Add Body does. ``FactoryContext``, ``Generation`` and ``Fit`` are never extracted.
"""
from __future__ import annotations

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
from asset_document.profiles import ik_common as ik
from asset_document.regions.primitive_catalog import NamedOpaqueHandleArgument, _single_handle
from asset_document.regions.target_lifecycle import generic_factory_lifecycle, lifecycle_requirements

PROFILE_ID = "profile.physics_asset.v1"
CLASS = "/Script/Engine.PhysicsAsset"
ROUTE = "physics_profile"
PROVIDER = "physics"
PREFIX = "PhysicsAsset"
READ = "physics.asset.read.v1"
EDIT = "physics.asset.edit.v1"
STATUS = "physics.status.v1"

BODY_KEYS = ("PreviewMesh", "Bodies", "Constraints", "CollisionDisabled")
# Create-time helpers, never extracted: Body.Generation (the editor's generation of a new asset without Bodies),
# a new body's Fit (the editor's Add Body fit), the document's FactoryContext (the mesh the asset is made for).
CREATE_BODY_KEYS = ("Generation",)
SKELETAL_MESH_CLASS = "/Script/Engine.SkeletalMesh"
BODY_ENTRY_KEYS = ("Properties", "Shapes", "PhysicalAnimation", "Fit")
CONSTRAINT_KEYS = ("Child", "Parent", "Frames", "Properties", "Profiles", "RenamedFrom")
FRAME_KEYS = ("Position", "PriAxis", "SecAxis")
# Authored shape types (document key -> the editor's auto name suffix); the experimental level set / skinned
# triangle mesh types are read only (Inspect lists them, apply keeps them).
SHAPE_TYPES = {"Spheres": "sphere", "Boxes": "box", "Capsules": "capsule", "TaperedCapsules": "tapered_capsule",
               "Convexes": "convex"}
READ_ONLY_SHAPE_TYPES = ("LevelSets", "SkinnedLevelSets", "MLLevelSets", "SkinnedTriangleMeshes")
# Complete values the sparse entries are read against (header "defaults" of physics.asset.read.v1: body, constraint,
# physical_animation). Engine constants: the last read's are kept so a diff compares an explicitly authored default as
# equal to an omitted one. Empty until a read (a missing key is then an add / remove).
_VALUE_DEFAULTS = {}
PROFILE_LISTS = {"ConstraintProfiles": "constraint", "PhysicalAnimationProfiles": "physical_animation"}

BODY_OWNED = {"SkeletalBodySetups": "Body/Bodies", "ConstraintSetup": "Body/Constraints",
              "PreviewSkeletalMesh": "Body/PreviewMesh"}
NOT_AUTHORED = ("BoundsBodies", "ThumbnailInfo", "SolverIterations", "EditorBodyFlags", "CurrentPhysicalAnimationProfileName",
                "CurrentConstraintProfileName", "DefaultSkelMesh_DEPRECATED", "BodySetup_DEPRECATED")


def _requirements(*primitives):
    return tuple(PrimitiveRequirement(item, 1, ROUTE) for item in primitives)


# UPhysicsAssetFactory's TargetSkeletalMesh is the factory context (the mesh its picker asks for); the Editor runs
# what the factory does after its modal generation dialog (registered by the AssetDocumentPhysics module).
LIFECYCLE = generic_factory_lifecycle(ROUTE, ("factory_context",))
REQUIREMENTS = (_requirements(common.CLASS_RESOLVE, common.RESOLVE, common.REFERENCE_RESOLVE, READ, EDIT, STATUS)
                + lifecycle_requirements(LIFECYCLE))
BY_ID = {item.primitive_id: item for item in REQUIREMENTS}


def error(path, message, code):
    return common.error(path, message, code)


escape = common.escape


class PhysicsCalls(common.DataCalls):
    """Calls to the "physics" provider against one physics asset (editor handles handed off)."""

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


def _name(value, path, what):
    return ik.name_value(value, path, what, PREFIX)


def _keys(value, allowed, path, what):
    ik.check_keys(value, allowed, path, what, PREFIX)


def _vector(value, path, what):
    if type(value) is not dict or set(value) != {"X", "Y", "Z"} \
            or any(type(item) not in (int, float) or type(item) is bool for item in value.values()):
        raise error(path, f"{what} is a vector {{X, Y, Z}}.", "PhysicsAsset.InvalidBody")


def validate_body(body):
    path = "/Body"
    if "Generation" in body:
        _object(body["Generation"], f"{path}/Generation", "Generation (generation settings: GeomType, MinBoneSize, ...)")
        if "Bodies" in body:
            raise error(f"{path}/Generation", "Generation makes the bodies of a new asset whose document has no Bodies; "
                        "with Bodies the bodies are the document's (a new body can still be fitted with its Fit).",
                        "PhysicsAsset.GenerationWithBodies")
    if "PreviewMesh" in body:
        ik.path_or_null(body["PreviewMesh"], f"{path}/PreviewMesh", "PreviewMesh", PREFIX)
    for name, entry in _object(body.get("Bodies", {}), f"{path}/Bodies", "Bodies").items():
        where = f"{path}/Bodies/{escape(name)}"
        _name(name, where, "A body key (its bone)")
        _keys(_object(entry, where, "A body"), BODY_ENTRY_KEYS, where, "body")
        for key in ("Properties", "PhysicalAnimation"):
            if key in entry:
                region = _object(entry[key], f"{where}/{key}", key)
                if key == "PhysicalAnimation":
                    for profile, data in region.items():
                        _object(data, f"{where}/{key}/{escape(profile)}", "Physical animation data")
        if "Fit" in entry:
            _object(entry["Fit"], f"{where}/Fit", "Fit (generation settings: GeomType, VertWeight, ...)")
        if "Shapes" in entry:
            shapes = _object(entry["Shapes"], f"{where}/Shapes", "Shapes")
            for kind, elements in shapes.items():
                at = f"{where}/Shapes/{escape(kind)}"
                if kind in READ_ONLY_SHAPE_TYPES:
                    raise error(at, f"{kind} are experimental shapes the editor only makes through its fit tools; they "
                                "are kept as they are and cannot be written.", "PhysicsAsset.ShapeTypeNotAuthored")
                if kind not in SHAPE_TYPES:
                    raise error(at, f"Unknown shape type '{kind}'. Types: {', '.join(SHAPE_TYPES)}.",
                                "PhysicsAsset.UnknownKey")
                if type(elements) is not list:
                    raise error(at, f"{kind} is an array of shapes.", "PhysicsAsset.InvalidBody")
                for index, element in enumerate(elements):
                    _object(element, f"{at}/{index}", "A shape")
    for name, entry in _object(body.get("Constraints", {}), f"{path}/Constraints", "Constraints").items():
        where = f"{path}/Constraints/{escape(name)}"
        if not name or "~" in name:
            raise error(where, "A constraint key is its joint name (no '~'; extract keys repeated joint names <name>~N: "
                        "rename one with RenamedFrom).", "PhysicsAsset.InvalidBody")
        _keys(_object(entry, where, "A constraint"), CONSTRAINT_KEYS, where, "constraint")
        for key in ("Child", "Parent"):
            _name(entry.get(key), f"{where}/{key}", f"A constraint's {key} (a bone with a body)")
        if entry["Child"] == entry["Parent"]:
            raise error(f"{where}/Parent", "A constraint joins two different bodies.", "PhysicsAsset.InvalidBody")
        if "RenamedFrom" in entry:
            _name(entry["RenamedFrom"], f"{where}/RenamedFrom", "RenamedFrom")
        if "Frames" in entry:
            frames = _object(entry["Frames"], f"{where}/Frames", "Frames")
            _keys(frames, ("Child", "Parent"), f"{where}/Frames", "Frames")
            for side in ("Child", "Parent"):
                frame = _object(frames.get(side), f"{where}/Frames/{side}", f"Frames.{side}")
                _keys(frame, FRAME_KEYS, f"{where}/Frames/{side}", "frame")
                for key in FRAME_KEYS:
                    _vector(frame.get(key), f"{where}/Frames/{side}/{key}", key)
        for key in ("Properties", "Profiles"):
            if key in entry:
                region = _object(entry[key], f"{where}/{key}", key)
                if key == "Profiles":
                    for profile, data in region.items():
                        _object(data, f"{where}/{key}/{escape(profile)}", "Constraint profile data")
    if "CollisionDisabled" in body:
        pairs = body["CollisionDisabled"]
        if type(pairs) is not list:
            raise error(f"{path}/CollisionDisabled", "CollisionDisabled is a list of [boneA, boneB] pairs.",
                        "PhysicsAsset.InvalidBody")
        seen = set()
        for index, pair in enumerate(pairs):
            at = f"{path}/CollisionDisabled/{index}"
            if type(pair) is not list or len(pair) != 2 or any(type(item) is not str or not item for item in pair) \
                    or pair[0] == pair[1]:
                raise error(at, "A collision pair is [boneA, boneB] of two different bodies.", "PhysicsAsset.InvalidBody")
            key = frozenset(pair)
            if key in seen:
                raise error(at, f"The pair {pair} is listed twice.", "PhysicsAsset.InvalidBody")
            seen.add(key)


def _normalize(body):
    """Canonical form: shape Names equal to the editor's auto name are dropped (the auto name is the default)."""
    for bone, entry in body.get("Bodies", {}).items():
        for kind, elements in entry.get("Shapes", {}).items():
            suffix = SHAPE_TYPES.get(kind)
            for element in elements:
                if suffix is not None and element.get("Name") == f"{bone}_{suffix}":
                    del element["Name"]


def canonicalize(document):
    body = common.validate_header(document, (CLASS,), PREFIX, BODY_KEYS + CREATE_BODY_KEYS, ("FactoryContext",))
    if "FactoryContext" in document:
        value = document["FactoryContext"]
        if type(value) is not str or not value.startswith("/") or "." not in value:
            raise error("/FactoryContext", "FactoryContext is the object path of the Skeletal Mesh the physics asset is made "
                        "for, e.g. /Game/Characters/SKM_Hero.SKM_Hero.", "PhysicsAsset.InvalidFactoryContext")
    validate_body(body)
    canonical = common.snapshot(document)
    canonical.setdefault("Definitions", {})
    canonical.setdefault("Properties", {})
    canonical.setdefault("Body", {})
    _normalize(canonical["Body"])
    return canonical


# ---------------------------------------------------------------------------
# Read records -> document
# ---------------------------------------------------------------------------
def read_records(calls, detail="document"):
    records = []
    offset = 0
    while True:
        values = {"offset": offset}
        if detail != "document":
            values["detail"] = detail
        page = calls.query(READ, values)
        records += page.get("records", [])
        offset = page.get("next_offset", -1)
        if type(offset) is not int or offset < 0:
            break
    if not records or records[0].get("kind") != "header":
        raise error("/Body", f"{READ} returned no header.", "PhysicsAsset.InvalidPrimitiveResult")
    return records


def _positive_zero(value):
    """-0.0 reads as 0 (a rotator / axis component the engine stores negative-signed is the same value)."""
    if type(value) is dict:
        return {key: _positive_zero(item) for key, item in value.items()}
    if type(value) is list:
        return [_positive_zero(item) for item in value]
    if type(value) is float and value == 0.0:
        return 0.0
    return value


def assemble(records):
    """Body and Inspect facts of physics.asset.read.v1 records."""
    records = _positive_zero(records)
    header = records[0]
    if type(header.get("defaults")) is dict:
        _VALUE_DEFAULTS.clear()
        _VALUE_DEFAULTS.update(header["defaults"])
    body = {}
    facts = {"header": header, "skeleton": [], "bodies": {}, "mesh_users": [], "warnings": []}
    if header.get("preview_mesh"):
        body["PreviewMesh"] = header["preview_mesh"]
    bodies, constraints, pairs = {}, {}, []
    for record in records[1:]:
        kind = record.get("kind")
        if kind == "body":
            bodies[record["bone"]] = record["entry"]
        elif kind == "convex_vertices":
            element = bodies[record["bone"]]["Shapes"]["Convexes"][record["index"]]
            element.setdefault("VertexData", []).extend(record["vertices"])
        elif kind == "constraint":
            constraints[record["key"]] = record["entry"]
        elif kind == "collision":
            pairs += [list(pair) for pair in record["pairs"]]
        elif kind == "warning":
            facts["warnings"].append(record)
        elif kind == "skeleton":
            facts["skeleton"] += record["bones"]
        elif kind == "body_facts":
            facts["bodies"][record["bone"]] = record
        elif kind == "mesh_users":
            facts["mesh_users"] = record["meshes"]
    body["Bodies"] = bodies
    body["Constraints"] = constraints
    body["CollisionDisabled"] = pairs
    return body, facts


def read_body(calls, detail="document"):
    return assemble(read_records(calls, detail))


def extract(target, read, context, include_defaults=True):
    body, _ = read_body(PhysicsCalls(context, read))
    return common.document(target, CLASS, _properties().extract(read, context, include_defaults), body)


# ---------------------------------------------------------------------------
# Diff
# ---------------------------------------------------------------------------
def _pair_set(pairs):
    return {frozenset(pair) for pair in pairs}


def shape_defaults(kind, bone):
    """A new element of a shape type (header defaults "shapes"): Name is the editor's auto name, which the read
    omits. Empty until a read."""
    element = _VALUE_DEFAULTS.get("shapes", {}).get(kind)
    if type(element) is not dict:
        return {}
    return {**element, "Name": f"{bone}_{SHAPE_TYPES[kind]}"}


def shapes_diff(path, authored, live, exact, changes, bone=None):
    """A present Shapes object is complete over the authored types (a type it does not name is empty). Elements are
    sparse over a new element of their type (``bone``: the body's, for the auto name)."""
    for kind in SHAPE_TYPES:
        mine, theirs = authored.get(kind, []), live.get(kind, [])
        where = f"{path}/{kind}"
        for index in range(max(len(mine), len(theirs))):
            at = f"{where}/{index}"
            if index >= len(theirs):
                changes.append(Difference(at, DiffKind.ADD, True, mine[index], False, None))
            elif index >= len(mine):
                changes.append(Difference(at, DiffKind.REMOVE, False, None, True, theirs[index]))
            else:
                sparse_diff(at, mine[index], theirs[index], shape_defaults(kind, bone) if bone is not None else {},
                            exact, changes)


class Each:
    """Defaults of a name-keyed map: an entry present on both sides compares against ``value``; an entry on one
    side only is an add / remove."""

    def __init__(self, value):
        self.value = value


def sparse_diff(path, authored, live, defaults, exact, changes):
    """``ik.value_diff`` with nested defaults: a key missing on one side takes its value from ``defaults`` (at any
    depth) when it has one there, else it is an add / remove."""
    if type(authored) is not dict or type(live) is not dict:
        if not common.same(authored, live, exact):
            changes.append(Difference(path, DiffKind.REPLACE, True, authored, True, live))
        return
    for key in list(authored) + [key for key in live if key not in authored]:
        where = f"{path}/{escape(key)}"
        if isinstance(defaults, Each):
            known, default = False, defaults.value
        else:
            known = type(defaults) is dict and key in defaults
            default = defaults.get(key) if known else None
        if key in authored and key in live:
            sparse_diff(where, authored[key], live[key], default, exact, changes)
        elif known:
            omitted = {} if isinstance(default, Each) else default  # an omitted map has no entries
            sparse_diff(where, authored.get(key, omitted), live.get(key, omitted), default, exact, changes)
        elif key in authored:
            changes.append(Difference(where, DiffKind.ADD, True, authored[key], False, None))
        else:
            changes.append(Difference(where, DiffKind.REMOVE, False, None, True, live[key]))


def _merged(base, over):
    if type(base) is not dict or type(over) is not dict:
        return over
    merged = dict(base)
    for key, value in over.items():
        merged[key] = _merged(base.get(key), value)
    return merged


def body_defaults():
    return {"Properties": _VALUE_DEFAULTS.get("body", {}),
            "PhysicalAnimation": Each(_VALUE_DEFAULTS.get("physical_animation", {}))}


def constraint_defaults(live):
    """A constraint entry's defaults: profile entries are sparse against its own default profile (its Properties'
    ProfileInstance over a new constraint's)."""
    instance = _VALUE_DEFAULTS.get("constraint", {})
    profile = _merged(instance.get("ProfileInstance", {}), live.get("Properties", {}).get("ProfileInstance", {}))
    return {"Frames": {}, "Properties": instance, "Profiles": Each(profile)}


def body_entry_diff(path, authored, live, exact, changes, bone=None):
    entry = {key: value for key, value in authored.items() if key not in ("Shapes", "Fit")}
    current = {key: value for key, value in live.items() if key != "Shapes"}
    sparse_diff(path, entry, current, body_defaults(), exact, changes)
    if "Shapes" in authored:
        shapes_diff(f"{path}/Shapes", authored["Shapes"], live.get("Shapes", {}), exact, changes, bone)


def body_diff(authored, live, exact):
    """Differences of the authored Body regions (omitted region = keep) from the live Body."""
    changes = []
    path = "/Body"
    if "PreviewMesh" in authored:
        ik.value_diff(f"{path}/PreviewMesh", authored["PreviewMesh"] or "", live.get("PreviewMesh") or "", exact, changes)
    if "Bodies" in authored:
        mine, theirs = authored["Bodies"], live.get("Bodies", {})
        for bone in list(mine) + [bone for bone in theirs if bone not in mine]:
            where = f"{path}/Bodies/{escape(bone)}"
            if bone not in theirs:
                changes.append(Difference(where, DiffKind.ADD, True, mine[bone], False, None))
            elif bone not in mine:
                changes.append(Difference(where, DiffKind.REMOVE, False, None, True, theirs[bone]))
            else:
                body_entry_diff(where, mine[bone], theirs[bone], exact, changes, bone)
    if "Constraints" in authored:
        mine, theirs = authored["Constraints"], live.get("Constraints", {})
        for name in list(mine) + [name for name in theirs if name not in mine]:
            where = f"{path}/Constraints/{escape(name)}"
            if name not in theirs:
                changes.append(Difference(where, DiffKind.ADD, True, mine[name], False, None))
            elif name not in mine:
                changes.append(Difference(where, DiffKind.REMOVE, False, None, True, theirs[name]))
            else:
                # RenamedFrom is an instruction, not state: it is not compared.
                entry = {key: value for key, value in mine[name].items() if key != "RenamedFrom"}
                sparse_diff(where, entry, theirs[name], constraint_defaults(theirs[name]), exact, changes)
    if "CollisionDisabled" in authored:
        if _pair_set(authored["CollisionDisabled"]) != _pair_set(live.get("CollisionDisabled", [])):
            changes.append(Difference(f"{path}/CollisionDisabled", DiffKind.REPLACE, True, authored["CollisionDisabled"],
                                      True, live.get("CollisionDisabled", [])))
    return changes


def diff(authored, extracted, exact):
    changes = common.header_diff(authored, extracted)
    changes.extend(body_diff(authored.get("Body", {}), extracted.get("Body", {}), exact))
    changes.extend(common.properties_diff(authored.get("Properties", {}), extracted.get("Properties", {}), exact))
    return tuple(changes)


def existing(context, document):
    """Read handle of the document's target, or None when it does not exist yet."""
    target = document["Target"]
    result = common.invoke(context, BY_ID, common.RESOLVE,
                           {"path": f"{target}.{target.rsplit('/', 1)[-1]}", "expected_class": CLASS}, prefix=PREFIX)
    value = result.value if type(result.value) is dict else {}
    if value.get("exists") is not True:
        return None
    return next((group.handles[0] for group in result.handle_results
                 if group.name == "asset_read" and group.handles), None)


# Documents whose target this request creates: create_target_context runs before preflight / apply in the same
# call with the same document object (Apply, and Validate's trial apply), so apply knows the asset is the factory's
# new one (a document without Bodies then gets the editor's generation). Held by identity: an entry can never match
# another document, and every create path takes it again.
_CREATING = {}


def mark_creating(document):
    _CREATING[id(document)] = document


def take_creating(document):
    return _CREATING.pop(id(document), None) is document


def factory_mesh(document):
    """(path, document path) of the skeletal mesh a new asset is made for: FactoryContext, else Body.PreviewMesh."""
    if document.get("FactoryContext"):
        return document["FactoryContext"], "/FactoryContext"
    if document.get("Body", {}).get("PreviewMesh"):
        return document["Body"]["PreviewMesh"], "/Body/PreviewMesh"
    return None, "/FactoryContext"


def create_target_context(document, context):
    """The New Physics Asset factory's skeletal mesh (its TargetSkeletalMesh, which its picker asks for)."""
    canonical = canonicalize(document)
    mesh, path = factory_mesh(canonical)
    if mesh is None:
        raise error("/FactoryContext", "Creating a Physics Asset needs the Skeletal Mesh it is made for: FactoryContext "
                    "(or Body.PreviewMesh), e.g. /Game/Characters/SKM_Hero.SKM_Hero.", "PhysicsAsset.MeshRequired")
    try:
        outcome = common.invoke(context, BY_ID, common.REFERENCE_RESOLVE,
                                {"expected_class": SKELETAL_MESH_CLASS, "path": mesh}, path=path, prefix=PREFIX)
    except AssetDocumentContractError as failure:
        raise error(path, f"'{mesh}' is not a loadable Skeletal Mesh ({failure.diagnostic.message}).",
                    "PhysicsAsset.UnknownMesh") from None
    mark_creating(document)
    return (NamedOpaqueHandleArgument("factory_context", _single_handle(outcome, "asset")),)


# ---------------------------------------------------------------------------
# Inspect
# ---------------------------------------------------------------------------
def status(calls):
    """Data validation and structural checks of the asset ({Status, Messages})."""
    return calls.query(STATUS).get("compile", {})


def read_generation(calls):
    """Inspect's Generation suggestion: what the editor's generation with the project's settings makes for the preview
    mesh ({Settings, Body}), or None without a preview mesh."""
    records = []
    offset = 0
    while True:
        page = calls.query(READ, {"offset": offset, "detail": "generation"})
        records += page.get("records", [])
        offset = page.get("next_offset", -1)
        if type(offset) is not int or offset < 0:
            break
    if not records or records[0].get("kind") != "generation" or records[0].get("available") is not True:
        return None
    body, _ = assemble(records[1:])
    body.pop("PreviewMesh", None)
    return {"Settings": _positive_zero(records[0].get("settings", {})), "Body": body}


def inspect_payload(document, calls):
    body, facts = read_body(calls, "inspect")
    bodies = {}
    for bone, record in facts["bodies"].items():
        row = {"Mass": record.get("mass", 0), "InPreviewMesh": record.get("in_mesh", False)}
        if record.get("read_only_shapes"):
            row["ReadOnlyShapes"] = record["read_only_shapes"]
        bodies[bone] = row
    warnings = [{"Code": item.get("code", ""), "Message": item.get("message", ""), "Path": item.get("path", "")}
                for item in facts["warnings"]]
    payload = {**document, "Skeleton": facts["skeleton"], "BodyFacts": bodies, "MeshUsers": facts["mesh_users"],
               "ReadWarnings": warnings, "Status": status(calls)}
    generation = read_generation(calls)
    if generation is not None:
        payload["Generation"] = generation
    return payload


# ---------------------------------------------------------------------------
# Handler
# ---------------------------------------------------------------------------
class PhysicsAssetRuntimeProfile:
    runtime_entry_module = "asset_document/bootstrap.py"
    # Snapshot-covered: preflight never reads a preview.
    preflight_scratch = False
    exact_asset_class_path = CLASS
    profile_id = PROFILE_ID

    def required_primitives(self):
        return REQUIREMENTS

    def inspect_profile(self):
        return {"Class": CLASS, "BodySections": list(BODY_KEYS), "ReadOnlySections": [],
                "CreateContext": "FactoryContext: the Skeletal Mesh (else Body.PreviewMesh); create-only Body.Generation "
                                 "and a new body's Fit"}

    def create_template(self, target):
        return common.document(common.require_target(target, PREFIX), CLASS, {}, {})

    def validate(self, document):
        canonicalize(document)

    def canonicalize(self, document):
        return canonicalize(document)

    def create_target_context(self, document, context):
        return create_target_context(document, context)

    def preflight(self, document, scratch_handle, context):
        created = take_creating(document)
        canonical = canonicalize(document)
        _properties().validate(_apply.generic_properties(canonical.get("Properties", {})), context)
        read = None if created else existing(context, canonical)
        if read is None:
            _apply.preflight_create(canonical)
        else:
            _apply.preflight(canonical, read, context)
        return SNAPSHOT_COVERED

    def apply(self, document, read_asset_handle, mutable_asset_handle, context):
        created = take_creating(document)
        return _apply.apply(canonicalize(document), read_asset_handle, mutable_asset_handle, context, created)

    def repair(self, document, asset_handle, context):
        canonicalize(document)
        return ()

    def extract(self, *, target, asset_handle, context, include_defaults=False):
        return extract(common.require_target(target, PREFIX), asset_handle, context)

    def inspect(self, *, mode, exact_class_path, exact_asset_object_path, editor_asset_handle, context):
        if mode != "live_asset" or exact_class_path != CLASS or editor_asset_handle is None:
            raise error("/Inspect", "Physics Asset inspection reads a live Physics Asset.", "PhysicsAsset.InspectNotAdmitted")
        document = canonicalize(self.extract(target=exact_asset_object_path.rsplit(".", 1)[0],
                                             asset_handle=editor_asset_handle, context=context))
        return inspect_payload(document, PhysicsCalls(context, editor_asset_handle))

    def diff(self, authored, extracted):
        return diff(canonicalize(authored), canonicalize(extracted), exact=False)

    def diff_exact(self, authored, extracted):
        return diff(canonicalize(authored), canonicalize(extracted), exact=True)


def _meta():
    rows = [("preview_mesh", "/Body/PreviewMesh"), ("bodies", "/Body/Bodies"), ("constraints", "/Body/Constraints"),
            ("collision_disabled", "/Body/CollisionDisabled")]
    regions = tuple(RegionMeta(f"physics_asset.{name}.v1", path, RegionOwnership.MANAGED, IdentityPolicy.STABLE_KEY,
                               OrderPolicy.CANONICAL, DefaultPolicy.EXPLICIT, OmissionPolicy.PRESERVE,
                               DeletePolicy.EXPLICIT_ONLY, ComparisonPolicy.CANONICAL)
                    for name, path in rows)
    return ProfileMeta(PROFILE_ID, ROUTE, CLASS, regions, REQUIREMENTS)


PHYSICS_ASSET_RUNTIME_REGISTRATION = RuntimeProfileRegistration(_meta(), PhysicsAssetRuntimeProfile,
                                                                RuntimeTargetPolicy.CREATE_OR_UPDATE, LIFECYCLE)

__all__ = ("PHYSICS_ASSET_RUNTIME_REGISTRATION", "PhysicsAssetRuntimeProfile", "assemble", "body_diff", "canonicalize",
           "diff")

# Module-level on purpose (after everything physics_asset_apply uses from this module): the native runtime restores
# sys.modules between phase calls, so an in-call import would load a second copy of the primitive catalog.
from asset_document.profiles import physics_asset_apply as _apply  # noqa: E402
