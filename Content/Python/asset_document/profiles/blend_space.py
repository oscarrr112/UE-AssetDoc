"""Registered blend space Profiles: BlendSpace, BlendSpace1D, AimOffsetBlendSpace and
AimOffsetBlendSpace1D (profile.blend_space.v1, profile.blend_space_1d.v1, profile.aim_offset.v1,
profile.aim_offset_1d.v1): the Blend Space editor's axes and samples.

Document (docs/architecture/asset-document-animation-assets-matrix.md)::

    "Body": {
      "Skeleton": "/Game/Characters/SK_Hero_Skeleton.SK_Hero_Skeleton",   # create: the New Asset dialog's
      "PreviewMesh": "/Game/Characters/SK_Hero.SK_Hero",                   #   skeleton; immutable after
      "Axes": {
        "X": {"Name": "Direction", "Min": -180, "Max": 180, "GridDivisions": 4, "SnapToGrid": true,
              "WrapInput": false},
        "Y": {"Name": "Speed", "Min": 0, "Max": 600, "GridDivisions": 4}   # 1D classes: X only
      },
      "Samples": [                                                         # the full sample list
        {"Animation": "/Game/Anims/Idle.Idle", "X": 0, "Y": 0},
        {"Animation": "/Game/Anims/Walk.Walk", "X": 0, "Y": 150, "RateScale": 1.2}
      ]
    },
    "Properties": {"InterpolationParam": [...], "bLoop": true, "NotifyTriggerMode": "AllAnimations", ...}

* ``Skeleton`` is required to create (BlendSpaceFactoryNew / BlendSpaceFactory1D TargetSkeleton) and
  cannot change afterwards (``BlendSpace.SkeletonImmutable``).
* ``Axes``: omitted fields keep their value.  Min < Max; GridDivisions 1..1000.
* ``Samples`` are identified by their position (X, Y; 1D: X), which the editor keeps unique: a moved
  sample is a removed sample plus a new one.  Positions must lie inside the axis range
  (``BlendSpace.SampleOutOfRange``; the grid only offers positions inside it) and, on axes with
  SnapToGrid, on a grid point (``BlendSpace.SampleOffGrid``; the editor would move them).
  ``RateScale`` (default 1), ``SingleFrame`` (frame index or null = whole animation, default null)
  and ``IncludeInAnalyseAll`` (default true) omitted on a new sample = default, on an existing sample
  = keep.  Animations must use a compatible skeleton and the additive type the class accepts
  (Aim Offsets: mesh-space or local-space rotation offsets) and all samples share one additive type.
* Other Details-panel settings (sample smoothing, interpolation, notifies, ...) are Properties; after
  they change the asset is re-validated and resampled as the editor does.

Writes are covered by Core's package snapshot (existing target) or the factory rollback (created
target); preflight returns ``SNAPSHOT_COVERED``.
"""
from __future__ import annotations

from asset_document.diff.model import Difference, DiffKind
from asset_document.meta import (
    ComparisonPolicy, DefaultPolicy, DeletePolicy, IdentityPolicy, OmissionPolicy, OrderPolicy, ProfileMeta,
    RegionMeta, RegionOwnership,
)
from asset_document.mutation_intent_projection import SNAPSHOT_COVERED
from asset_document.profile_runtime import RuntimeProfileRegistration, RuntimeTargetPolicy
from asset_document.profiles import anim_asset_common as anim
from asset_document.profiles import data_table_common as common
from asset_document.regions.target_lifecycle import generic_factory_lifecycle, lifecycle_requirements

BLEND_SPACE_CLASS = "/Script/Engine.BlendSpace"
BLEND_SPACE_1D_CLASS = "/Script/Engine.BlendSpace1D"
AIM_OFFSET_CLASS = "/Script/Engine.AimOffsetBlendSpace"
AIM_OFFSET_1D_CLASS = "/Script/Engine.AimOffsetBlendSpace1D"
CLASSES = {BLEND_SPACE_CLASS: "profile.blend_space.v1", BLEND_SPACE_1D_CLASS: "profile.blend_space_1d.v1",
           AIM_OFFSET_CLASS: "profile.aim_offset.v1", AIM_OFFSET_1D_CLASS: "profile.aim_offset_1d.v1"}
PREFIX = "BlendSpace"
BODY_KEYS = ("Skeleton", "PreviewMesh", "Axes", "Samples")
AXIS_KEYS = {"Name": "name", "Min": "min", "Max": "max", "GridDivisions": "grid", "SnapToGrid": "snap",
             "WrapInput": "wrap"}
SAMPLE_KEYS = ("Animation", "X", "Y", "RateScale", "SingleFrame", "IncludeInAnalyseAll")
SAMPLE_DEFAULTS = {"RateScale": 1, "SingleFrame": None, "IncludeInAnalyseAll": True}
SAMPLE_FIELDS = {"Animation": "animation", "RateScale": "rate_scale", "SingleFrame": "single_frame",
                 "IncludeInAnalyseAll": "analyse_all"}
GRID_TOLERANCE = 1e-3
LIFECYCLE = generic_factory_lifecycle(anim.ROUTE, ("factory_context",))
REQUIREMENTS = (anim.requirements(anim.CLASS_RESOLVE, anim.RESOLVE, anim.REFERENCE_RESOLVE, anim.BLEND_SPACE_READ,
                                  anim.BLEND_SPACE_EDIT)
                + lifecycle_requirements(LIFECYCLE))
BY_ID = {item.primitive_id: item for item in REQUIREMENTS}
BODY_OWNED = {"BlendParameters": "Body.Axes", "SampleData": "Body.Samples", "Skeleton": "Body.Skeleton",
              "PreviewSkeletalMesh": "Body.PreviewMesh"}


def _error(path, message, code):
    return common.error(path, message, code)


def axis_names(class_path):
    return ("X",) if class_path in (BLEND_SPACE_1D_CLASS, AIM_OFFSET_1D_CLASS) else ("X", "Y")


# ---------------------------------------------------------------------------
# Document shape
# ---------------------------------------------------------------------------
def _axis(value, path):
    if type(value) is not dict:
        raise _error(path, "An axis is an object with Name, Min, Max, GridDivisions, SnapToGrid, WrapInput.",
                     "BlendSpace.InvalidAxis")
    out = {}
    for key, item in value.items():
        where = f"{path}/{common.escape(key)}"
        if key not in AXIS_KEYS:
            raise _error(where, f"Unknown axis key '{key}'. Keys: {', '.join(AXIS_KEYS)}.", "BlendSpace.InvalidAxis")
        if key == "Name":
            if type(item) is not str:
                raise _error(where, "Name (the axis label) is a string.", "BlendSpace.InvalidAxis")
        elif key in ("Min", "Max"):
            anim.number(item, where, "BlendSpace.InvalidAxis", key)
        elif key == "GridDivisions":
            item = anim.integer(item, 1, 1000)
            if item is None:
                raise _error(where, "GridDivisions is an integer within 1..1000.", "BlendSpace.InvalidAxis")
        elif type(item) is not bool:
            raise _error(where, f"{key} is true or false.", "BlendSpace.InvalidAxis")
        out[key] = item
    if "Min" in out and "Max" in out and not out["Min"] < out["Max"]:
        raise _error(f"{path}/Min", "Min must be below Max.", "BlendSpace.InvalidAxis")
    return out


def _sample(row, path, axes):
    if type(row) is not dict:
        raise _error(path, "A sample is an object with Animation and its position.", "BlendSpace.InvalidSample")
    for key in row:
        if key not in SAMPLE_KEYS or (key == "Y" and "Y" not in axes):
            raise _error(f"{path}/{common.escape(key)}", f"Unknown sample key '{key}'. Keys: Animation, "
                         f"{', '.join(axes)}, RateScale, SingleFrame, IncludeInAnalyseAll.", "BlendSpace.InvalidSample")
    item = {"Animation": anim.object_path(row.get("Animation"), f"{path}/Animation", "BlendSpace.InvalidSample",
                                          "Animation")}
    for axis in axes:
        if axis not in row:
            raise _error(f"{path}/{axis}", f"A sample needs its {axis} position.", "BlendSpace.InvalidSample")
        item[axis] = anim.number(row[axis], f"{path}/{axis}", "BlendSpace.InvalidSample", axis)
    if "RateScale" in row:
        rate = anim.number(row["RateScale"], f"{path}/RateScale", "BlendSpace.InvalidSample", "RateScale")
        if not 0.01 <= rate <= 64:
            raise _error(f"{path}/RateScale", "RateScale is within 0.01..64.", "BlendSpace.InvalidSample")
        item["RateScale"] = rate
    if "SingleFrame" in row:
        frame = row["SingleFrame"] if row["SingleFrame"] is None else anim.integer(row["SingleFrame"], 0)
        if row["SingleFrame"] is not None and frame is None:
            raise _error(f"{path}/SingleFrame", "SingleFrame is a frame index (>= 0) or null (the whole animation).",
                         "BlendSpace.InvalidSample")
        item["SingleFrame"] = frame
    if "IncludeInAnalyseAll" in row:
        if type(row["IncludeInAnalyseAll"]) is not bool:
            raise _error(f"{path}/IncludeInAnalyseAll", "IncludeInAnalyseAll is true or false.", "BlendSpace.InvalidSample")
        item["IncludeInAnalyseAll"] = row["IncludeInAnalyseAll"]
    return item


def position(item, axes):
    return tuple(common._float32(float(item[axis])) for axis in axes)


def canonical_body(body, class_path):
    axes = axis_names(class_path)
    out = {}
    if "Skeleton" in body:
        out["Skeleton"] = anim.object_path(body["Skeleton"], "/Body/Skeleton", "BlendSpace.InvalidSkeleton", "Skeleton")
    if "PreviewMesh" in body:
        out["PreviewMesh"] = anim.optional_path(body["PreviewMesh"], "/Body/PreviewMesh", "BlendSpace.InvalidPreviewMesh",
                                                "PreviewMesh")
    if "Axes" in body:
        value = body["Axes"]
        if type(value) is not dict:
            raise _error("/Body/Axes", f"Axes is an object with {' and '.join(axes)}.", "BlendSpace.InvalidAxis")
        for key in value:
            if key not in axes:
                raise _error(f"/Body/Axes/{common.escape(key)}", f"This blend space has the axes {', '.join(axes)}.",
                             "BlendSpace.InvalidAxis")
        out["Axes"] = {key: _axis(value[key], f"/Body/Axes/{key}") for key in axes if key in value}
    if "Samples" in body:
        rows = body["Samples"]
        if type(rows) is not list:
            raise _error("/Body/Samples", "Samples is the list of samples.", "BlendSpace.InvalidSample")
        samples, seen = [], {}
        for index, row in enumerate(rows):
            item = _sample(row, f"/Body/Samples/{index}", axes)
            key = position(item, axes)
            if key in seen:
                raise _error(f"/Body/Samples/{index}", f"Samples {seen[key]} and {index} share one position (the editor "
                             "keeps sample positions unique).", "BlendSpace.DuplicateSample")
            seen[key] = index
            samples.append(item)
        out["Samples"] = samples
    return out


def canonicalize(document, class_path=None):
    body = common.validate_header(document, tuple(CLASSES), PREFIX, BODY_KEYS)
    if class_path is not None and document["Class"] != class_path:
        raise _error("/Class", f"Class must be {class_path}.", "BlendSpace.ClassMismatch")
    canonical_body(body, document["Class"])
    _properties(document["Class"]).check_owned(document.get("Properties", {}))
    return common.snapshot(document)


def _properties(class_path):
    return common.PropertiesRegion(class_path, BODY_OWNED, PREFIX)


# ---------------------------------------------------------------------------
# Live state
# ---------------------------------------------------------------------------
def read_live(calls):
    return calls.query(anim.BLEND_SPACE_READ)


def live_axes(live, class_path):
    return {name: {key: row[field] for key, field in AXIS_KEYS.items()}
            for name, row in zip(axis_names(class_path), live.get("axes", []))}


def live_samples(live, class_path):
    axes = axis_names(class_path)
    samples = []
    for row in live.get("samples", []):
        item = {"Animation": row["animation"], "X": row["x"]}
        if "Y" in axes:
            item["Y"] = row["y"]
        item.update({"RateScale": row["rate_scale"], "SingleFrame": row["single_frame"],
                     "IncludeInAnalyseAll": row["analyse_all"]})
        samples.append(item)
    return sorted(samples, key=lambda item: tuple(reversed(position(item, axes))))


def body_from_live(live, class_path):
    return {"Skeleton": live.get("skeleton", ""), "PreviewMesh": live.get("preview_mesh", ""),
            "Axes": live_axes(live, class_path), "Samples": live_samples(live, class_path)}


# ---------------------------------------------------------------------------
# Apply
# ---------------------------------------------------------------------------
def effective_axes(body, live, class_path):
    axes = live_axes(live, class_path)
    for name, value in body.get("Axes", {}).items():
        axes[name] = {**axes[name], **value}
    return axes


def check_samples(body, axes, class_path):
    """Positions inside the axis range and, when snapping, on a grid point (before any edit)."""
    names = axis_names(class_path)
    for name in names:
        if not axes[name]["Min"] < axes[name]["Max"]:
            raise _error(f"/Body/Axes/{name}/Min", f"Axis {name}: Min ({axes[name]['Min']}) must be below Max "
                         f"({axes[name]['Max']}).", "BlendSpace.InvalidAxis")
    snapping = all(axes[name]["SnapToGrid"] for name in names)
    for index, sample in enumerate(body.get("Samples", [])):
        for name in names:
            axis, value = axes[name], sample[name]
            where = f"/Body/Samples/{index}/{name}"
            if not axis["Min"] - GRID_TOLERANCE <= value <= axis["Max"] + GRID_TOLERANCE:
                raise _error(where, f"Sample {name}={value} is outside axis {name} ({axis['Min']}..{axis['Max']}); widen "
                             "the axis in Body.Axes or move the sample.", "BlendSpace.SampleOutOfRange")
            if snapping:
                step = (axis["Max"] - axis["Min"]) / axis["GridDivisions"]
                cells = (value - axis["Min"]) / step
                if abs(cells - round(cells)) > GRID_TOLERANCE:
                    raise _error(where, f"Axis {name} snaps to its grid (step {step:g} from {axis['Min']}); "
                                 f"{name}={value} is not a grid point, so the editor would move the sample. Use a grid "
                                 "point or set SnapToGrid false.", "BlendSpace.SampleOffGrid")


def _axis_ops(body, live, class_path):
    ops = []
    current = live_axes(live, class_path)
    for index, name in enumerate(axis_names(class_path)):
        wanted = body.get("Axes", {}).get(name, {})
        change = {AXIS_KEYS[key]: value for key, value in wanted.items()
                  if not common.same(value, current[name][key], exact=False)}
        if change:
            ops.append(({"op": "axis", "axis": index, **change}, f"/Body/Axes/{name}"))
    return ops


def _position_args(item, axes):
    return {axis.lower(): item[axis] for axis in axes}


def sample_ops(body, live, class_path):
    axes = axis_names(class_path)
    ops = []
    if "Samples" not in body:
        return ops, ops, ops
    current = {position(item, axes): item for item in live_samples(live, class_path)}
    wanted = {position(item, axes): (index, item) for index, item in enumerate(body["Samples"])}
    removes = [({"op": "sample_remove", **_position_args(item, axes)}, "/Body/Samples")
               for key, item in current.items() if key not in wanted]
    adds, sets = [], []
    for key, (index, item) in sorted(wanted.items(), key=lambda pair: pair[1][0]):
        path = f"/Body/Samples/{index}"
        have = current.get(key)
        if have is None:
            adds.append(({"op": "sample_add", "animation": item["Animation"], **_position_args(item, axes)}, path))
            have = {"Animation": item["Animation"], **SAMPLE_DEFAULTS}
        change = {}
        for field, arg in SAMPLE_FIELDS.items():
            if field in item and not common.same(item[field], have[field], exact=False):
                change[arg] = item[field]
        if change:
            sets.append(({"op": "sample_set", **_position_args(item, axes), **change}, path))
    return removes, adds, sets


def plan(body, live, class_path):
    """Ordered ops: removed samples, axes, new samples, sample settings, preview mesh."""
    removes, adds, sets = sample_ops(body, live, class_path)
    ops = removes + _axis_ops(body, live, class_path) + adds + sets
    if "PreviewMesh" in body and body["PreviewMesh"] != live.get("preview_mesh", ""):
        ops.append(({"op": "preview_mesh", "path": body["PreviewMesh"]}, "/Body/PreviewMesh"))
    return ops


FIELD_PATHS = {"animation": "/Animation", "rate_scale": "/RateScale", "single_frame": "/SingleFrame",
               "analyse_all": "/IncludeInAnalyseAll", "name": "/Name", "min": "/Min", "max": "/Max",
               "grid": "/GridDivisions", "snap": "/SnapToGrid", "wrap": "/WrapInput", "x": "/X", "y": "/Y"}


def map_path(primitive_path, op_paths):
    parts = [part for part in (primitive_path or "").split("/") if part]
    if len(parts) >= 2 and parts[0] == "ops" and parts[1].isdigit() and int(parts[1]) < len(op_paths):
        path = op_paths[int(parts[1])]
        if len(parts) > 2 and parts[2] in FIELD_PATHS and path not in ("/Body/Samples", "/Body/PreviewMesh"):
            path += FIELD_PATHS[parts[2]]
        return path
    return "/Body"


def edit(calls, ops, path_map=map_path):
    changed = []
    for chunk in common.chunk_ops(ops):
        op_paths = [path for _, path in chunk]
        outcome = common.invoke(calls.context, calls.by_id, anim.BLEND_SPACE_EDIT,
                                {"schema_version": 1, "ops": [op for op, _ in chunk]},
                                (common.NamedOpaqueHandleArgument("asset", calls.write),),
                                paths=lambda where, op_paths=op_paths: path_map(where, op_paths), prefix=PREFIX)
        changed.extend(outcome.changed_set)
    return changed


def apply(document, read, mutable, context):
    class_path = document["Class"]
    body = canonical_body(document.get("Body", {}), class_path)
    properties = document.get("Properties", {})
    region = _properties(class_path)
    region.check_owned(properties)
    calls = anim.AnimCalls(context, BY_ID, read, mutable, PREFIX)
    live = read_live(calls)
    anim.require_unchanged(body, "Skeleton", live.get("skeleton", ""), PREFIX,
                           "A blend space's Skeleton is chosen when it is created and the editor offers no change")
    check_samples(body, effective_axes(body, live, class_path), class_path)
    ops = plan(body, live, class_path)
    changes = edit(calls, ops) if ops else []
    written = list(region.apply(properties, read, mutable, context))
    if written:
        # SBlendSpaceEditor::NotifyPostChange after Details edits: validate and resample.
        written += edit(calls, [({"op": "refresh"}, "/Properties")])
    return tuple(changes + written)


def create_target_context(document, context):
    body = canonical_body(document.get("Body", {}), document["Class"])
    if "Skeleton" not in body:
        raise _error("/Body/Skeleton", "Creating a blend space requires Body.Skeleton (the skeleton the New Blend Space "
                     "dialog asks for).", "BlendSpace.SkeletonRequired")
    return anim.resolve_context(context, BY_ID, body["Skeleton"], "/Body/Skeleton", anim.SKELETON_CLASS, PREFIX,
                                "Skeleton")


# ---------------------------------------------------------------------------
# Extract / Diff
# ---------------------------------------------------------------------------
def extract(target, class_path, read, context, include_defaults=True):
    live = read_live(anim.AnimCalls(context, BY_ID, read, prefix=PREFIX))
    return common.document(target, class_path, _properties(class_path).extract(read, context, include_defaults),
                           body_from_live(live, class_path))


def diff(authored, extracted, exact):
    changes = common.header_diff(authored, extracted)
    class_path = authored["Class"]
    axes = axis_names(class_path)
    body = canonical_body(authored.get("Body", {}), class_path)
    live = canonical_body(extracted.get("Body", {}), extracted.get("Class", class_path))
    for key in ("Skeleton", "PreviewMesh"):
        if key in body and body[key] != live.get(key, ""):
            changes.append(Difference(f"/Body/{key}", DiffKind.REPLACE, True, body[key], key in live, live.get(key)))
    for name, value in body.get("Axes", {}).items():
        observed = live.get("Axes", {}).get(name, {})
        for key, item in value.items():
            anim.value_diff(changes, f"/Body/Axes/{name}/{key}", item, observed.get(key), exact)
    if "Samples" in body:
        have = {position(item, axes): item for item in live.get("Samples", [])}
        if set(have) != {position(item, axes) for item in body["Samples"]}:
            changes.append(Difference("/Body/Samples", DiffKind.REPLACE, True, body["Samples"], True, live.get("Samples")))
        else:
            for index, item in enumerate(body["Samples"]):
                observed = have[position(item, axes)]
                for key, value in item.items():
                    if key not in axes:
                        anim.value_diff(changes, f"/Body/Samples/{index}/{key}", value, observed.get(key), exact)
    changes.extend(common.properties_diff(authored.get("Properties", {}), extracted.get("Properties", {}), exact))
    return tuple(changes)


# ---------------------------------------------------------------------------
# Handler
# ---------------------------------------------------------------------------
class BlendSpaceRuntimeProfile:
    runtime_entry_module = "asset_document/bootstrap.py"
    preflight_scratch = False

    def __init__(self, class_path, profile_id):
        self.exact_asset_class_path = class_path
        self.profile_id = profile_id

    def required_primitives(self):
        return REQUIREMENTS

    def inspect_profile(self):
        return {"Class": self.exact_asset_class_path, "BodySections": list(BODY_KEYS), "ReadOnlySections": [],
                "CreateContext": "Body.Skeleton", "Axes": list(axis_names(self.exact_asset_class_path))}

    def create_template(self, target):
        axes = {name: {"Name": "None", "Min": 0, "Max": 100, "GridDivisions": 4}
                for name in axis_names(self.exact_asset_class_path)}
        return common.document(common.require_target(target, PREFIX), self.exact_asset_class_path, {},
                               {"Skeleton": "/Game/Characters/SK_Hero_Skeleton.SK_Hero_Skeleton", "Axes": axes,
                                "Samples": []})

    def _canonical(self, document):
        return canonicalize(document, self.exact_asset_class_path)

    def validate(self, document):
        self._canonical(document)

    def canonicalize(self, document):
        return self._canonical(document)

    def create_target_context(self, document, context):
        return create_target_context(self._canonical(document), context)

    def preflight(self, document, scratch_handle, context):
        canonical = self._canonical(document)
        _properties(canonical["Class"]).validate(canonical.get("Properties", {}), context)
        return SNAPSHOT_COVERED

    def apply(self, document, read_asset_handle, mutable_asset_handle, context):
        return apply(self._canonical(document), read_asset_handle, mutable_asset_handle, context)

    def repair(self, document, asset_handle, context):
        self._canonical(document)
        return ()

    def extract(self, *, target, asset_handle, context, include_defaults=False):
        return extract(common.require_target(target, PREFIX), self.exact_asset_class_path, asset_handle, context,
                       include_defaults)

    def inspect(self, *, mode, exact_class_path, exact_asset_object_path, editor_asset_handle, context):
        if mode != "live_asset" or exact_class_path != self.exact_asset_class_path or editor_asset_handle is None:
            raise _error("/Inspect", "Blend space inspection reads a live blend space.", "BlendSpace.InspectNotAdmitted")
        document = self.extract(target=exact_asset_object_path.rsplit(".", 1)[0], asset_handle=editor_asset_handle,
                                context=context, include_defaults=True)
        live = read_live(anim.AnimCalls(context, BY_ID, editor_asset_handle, prefix=PREFIX))
        axes = axis_names(self.exact_asset_class_path)
        invalid = [{axis: row[axis.lower()] for axis in axes} for row in live.get("samples", []) if not row.get("valid")]
        details = {"Axes": list(axes), "SampleCount": len(live.get("samples", [])),
                   "Note": "Samples are identified by position; moving one is remove + add. Animations must use a "
                           "skeleton compatible with Body.Skeleton."}
        if invalid:
            details["InvalidSamples"] = invalid
        return {**self._canonical(document), "BlendSpaceDetails": details}

    def diff(self, authored, extracted):
        return diff(self._canonical(authored), self._canonical(extracted), exact=False)

    def diff_exact(self, authored, extracted):
        return diff(self._canonical(authored), self._canonical(extracted), exact=True)


def _meta(class_path, profile_id):
    rows = (("skeleton", "Skeleton", IdentityPolicy.POSITIONAL, ComparisonPolicy.CANONICAL),
            ("preview_mesh", "PreviewMesh", IdentityPolicy.POSITIONAL, ComparisonPolicy.CANONICAL),
            ("axes", "Axes", IdentityPolicy.STABLE_KEY, ComparisonPolicy.CANONICAL),
            ("samples", "Samples", IdentityPolicy.STABLE_KEY, ComparisonPolicy.CANONICAL))
    regions = tuple(RegionMeta(f"blend_space.{name}.v1", f"/Body/{key}", RegionOwnership.MANAGED, identity,
                               OrderPolicy.CANONICAL, DefaultPolicy.EXPLICIT, OmissionPolicy.PRESERVE,
                               DeletePolicy.EXPLICIT_ONLY, comparison)
                    for name, key, identity, comparison in rows)
    return ProfileMeta(profile_id, anim.ROUTE, class_path, regions, REQUIREMENTS)


def _registration(class_path, profile_id):
    return RuntimeProfileRegistration(_meta(class_path, profile_id), lambda: BlendSpaceRuntimeProfile(class_path, profile_id),
                                      RuntimeTargetPolicy.CREATE_OR_UPDATE, LIFECYCLE)


BLEND_SPACE_RUNTIME_REGISTRATIONS = tuple(_registration(class_path, profile_id) for class_path, profile_id in CLASSES.items())

__all__ = ("BLEND_SPACE_RUNTIME_REGISTRATIONS", "BlendSpaceRuntimeProfile", "check_samples", "plan")
