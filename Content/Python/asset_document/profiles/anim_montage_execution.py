"""Scoped Montage primitive orchestration; no retained request or native state."""
from asset_document.diagnostics import AssetDocumentContractError, contract_error
from asset_document.dto import PrimitiveRequirement
from asset_document.regions.primitive_catalog import (
    NamedOpaqueHandleArgument, PrimitiveOutcome, PrimitiveOutcomeStatus,
    _single_handle, _optional_handle,
)
from asset_document.profiles.anim_montage_codec import lower_body, snapshot_to_body
from asset_document.profiles import anim_montage_settings as settings_codec
from asset_document.regions.target_lifecycle import generic_factory_lifecycle, lifecycle_requirements

ROUTE = "animation_profile"
CLASS = "/Script/Engine.AnimMontage"
CLASS_RESOLVE = "editor.class.resolve_canonical.v1"
RESOLVE = "editor.asset.resolve_existing.v1"
# The shared editor-factory lifecycle; the Skeleton is the factory context
# (UAnimMontageFactory::TargetSkeleton), exactly as the New Montage dialog sets it.
LIFECYCLE = generic_factory_lifecycle(ROUTE, ("factory_context",))
CREATE = LIFECYCLE.create_asset.primitive_id
CLASS_ENUMERATE = "editor.property.enumerate_admitted.v1"
# Montage's own native default time-stretch curve name (UAnimMontage ctor).
NATIVE_TIME_STRETCH_CURVE = "MontageTimeStretchCurve"
SNAPSHOT = "animation.montage.profile.snapshot.v1"
CURVES = "animation.curve.float.replace.v2"
PREVIEW = "animation.asset.preview_mesh.set.v1"
SKELETON = "animation.asset.skeleton.set.v1"
SLOTS = "animation.montage.slot_tracks.replace.v1"
SECTIONS = "animation.montage.sections.mutate.v1"
NOTIFIES = "animation.montage.notifies.replace.v1"
STATES = "animation.montage.notify_states.replace.v1"
# The read-only preparation of the same timeline edit (slots, sections, notifies, notify tracks) on the live target:
# Validate reports what Apply would (segment gaps, correspondence, sections, notify tracks and lengths).
CHECK = "animation.montage.structure.check.v1"
EDITOR_ENUMERATE = settings_codec.EDITOR_ENUMERATE
EDITOR_WRITE = settings_codec.EDITOR_WRITE
SETTINGS = settings_codec.SETTINGS
# The Skeleton's slots (provider "animation_asset"), to explain a slot the Skeleton lacks.
SKELETON_READ = "animation_asset.skeleton.read.v1"
# UAnimMontage's constructor adds one track on FAnimSlotGroup::DefaultSlotName.
NATIVE_DEFAULT_SLOT = "DefaultSlot"
REQUIREMENTS = (*(PrimitiveRequirement(id, 2 if id == CURVES else 1, ROUTE)
    for id in (CLASS_RESOLVE, RESOLVE, SNAPSHOT, CURVES, PREVIEW, SKELETON, SLOTS, SECTIONS,
               NOTIFIES, STATES, CLASS_ENUMERATE, EDITOR_ENUMERATE, EDITOR_WRITE, SETTINGS, SKELETON_READ, CHECK)),
    *lifecycle_requirements(LIFECYCLE))
BY_ID = {row.primitive_id: row for row in REQUIREMENTS}
SUPPORTED = ("Skeleton", "PreviewMesh", "Curves", "SlotAnimTracks", "CompositeSections", "Notifies", "NotifyStates",
             "NotifyTracks")
# The timeline keys Apply replaces in one prepared edit (the check primitive's input, in its order).
TIMELINE_KEYS = ("slots", "sections", "notifies", "notify_states", "notify_tracks")
SETTINGS_KEYS = settings_codec.SETTINGS_BODY_KEYS


def lower_document(document):
    """Lower one canonical document to (structural values, editor fields, settings intent, settings).

    Editor fields merge the Body-owned plain reflected settings with the
    authored Properties region; both are written by one generic Editor batch.
    """
    body = document.get("Body", {})
    lowered = lower_body({key: value for key, value in body.items() if key in SUPPORTED})
    settings = settings_codec.normalize_settings(body)
    settings_codec.validate_settings_against_body(settings, body)
    fields, intent = settings_codec.lower_settings(settings)
    fields.update(settings_codec.validate_properties(document.get("Properties", {})))
    return lowered, fields, intent, settings


def _admitted_batch(context, read_asset_handle):
    """Admitted Montage fields in native order, plus the batch token handle."""
    result = invoke(context, EDITOR_ENUMERATE, {}, (NamedOpaqueHandleArgument("asset", read_asset_handle),))
    rows = result.value.get("properties") if type(result.value) is dict else None
    groups = [group for group in result.handle_results if getattr(group, "name", None) == "property_batch"]
    if type(rows) is not list or len(groups) != 1 or len(groups[0].handles) != 1:
        raise error("Property batch enumeration must return rows and one property_batch handle.", "/PrimitiveOutcome")
    return rows, groups[0].handles[0]


# The Montage Properties region carries plain scalar fields only (the native
# settings snapshot projects the same set); references, text and structs are
# owned by Body regions or not authored through this profile.
_PLAIN_PROPERTY_CODECS = frozenset({
    "bool.v1", "integer.v1", "number.v1", "string.v1", "name.v1", "enum.v1",
    "soft_object_path.v1", "soft_class_path.v1",
})


def editor_rows(fields, admitted, codecs=None):
    """Order authored values like native admission and coerce integral numbers."""
    by_name = {row.get("name"): row for row in admitted if type(row) is dict}
    rows = []
    for name in fields:
        row = by_name.get(name)
        if row is None or row.get("writable") is not True or (
                codecs is not None and row.get("codec_id") not in codecs):
            raise error(f"{name} is not an admitted writable AnimMontage property.",
                        "/Properties/" + name.replace("~", "~0").replace("/", "~1"), "AnimMontage.UnknownProperty")
    for row in admitted:
        name = row.get("name") if type(row) is dict else None
        if name in fields:
            value = fields[name]
            if row.get("codec_id") == "number.v1" and type(value) is int:
                value = float(value)
            rows.append({"name": name, "value": value})
    return rows


def error(message, path="/Body", code="AnimMontage.InvalidIntent"):
    return contract_error(path, code, message)


def invoke(context, primitive, values, handles=()):
    outcome = context.invoke(requirement=BY_ID[primitive], value_arguments=values,
                             handle_arguments=handles)
    if type(outcome) is not PrimitiveOutcome:
        raise error("A typed primitive outcome is required.", "/PrimitiveOutcome")
    if outcome.status is not PrimitiveOutcomeStatus.SUCCEEDED:
        if outcome.diagnostics:
            raise AssetDocumentContractError(outcome.diagnostics[0])
        raise error("The primitive did not succeed.", "/PrimitiveOutcome")
    return outcome


def reference_path(path):
    if type(path) is not str or not path.startswith("/") or ":" in path:
        raise error("A canonical top-level asset reference is required.")
    return path if "." in path.rsplit("/", 1)[-1] else path + "." + path.rsplit("/", 1)[-1]


def resolve(context, path, expected_class=None):
    result = invoke(context, RESOLVE, {"path": reference_path(path), "expected_class": expected_class})
    if type(result.value) is not dict or type(result.value.get("exists")) is not bool:
        raise error("Resolver existence evidence is missing.", "/PrimitiveOutcome")
    return result.value["exists"], _optional_handle(result, "asset_read"), _optional_handle(result, "asset_mutable")


def animation_handle(context, handle, access="read_only"):
    return context.handoff(source_handle=handle, target_provider_id="animation", access=access)


def read_snapshot(context, editor_asset):
    result = invoke(context, SNAPSHOT, {}, (NamedOpaqueHandleArgument(
        "asset", animation_handle(context, editor_asset)),))
    if result.changed_set or result.handle_results:
        raise error("Snapshot must be handle-free and read-only.", "/PrimitiveOutcome")
    # Validate the complete native observation before publishing any projection.
    snapshot_to_body(result.value)
    return result.value


def _require_reference(context, path, expected_class):
    exists, read, _ = resolve(context, path, expected_class)
    if not exists or read is None:
        raise error("An authored reference is unavailable.", "/Body", "AnimationProvider.Montage.DependencyUnavailable")
    return read


def load_references(context, lowered):
    for key, cls in (("skeleton", "/Script/Engine.Skeleton"), ("preview_mesh", "/Script/Engine.SkeletalMesh")):
        if lowered.get(key) is not None:
            _require_reference(context, lowered[key], cls)
    for slot in lowered.get("slots", []):
        for segment in slot["segments"]:
            _require_reference(context, segment["animation"], None)
    for key, base in (("notifies", "/Script/Engine.AnimNotify"), ("notify_states", "/Script/Engine.AnimNotifyState")):
        for row in lowered.get(key, []):
            result = invoke(context, CLASS_RESOLVE, {"class": row["object"]["class"], "expected_base": base})
            _single_handle(result, "class")


def skeleton_slots(context, skeleton_path):
    """Slot names of a Skeleton, as the Anim Slot Manager lists them."""
    read = _require_reference(context, skeleton_path, "/Script/Engine.Skeleton")
    handle = context.handoff(source_handle=read, target_provider_id="animation_asset", access="read_only")
    result = invoke(context, SKELETON_READ, {}, (NamedOpaqueHandleArgument("asset", handle),))
    groups = result.value.get("slot_groups") if type(result.value) is dict else None
    if type(groups) is not list:
        raise error("Skeleton slot groups are missing.", "/PrimitiveOutcome")
    return [slot for group in groups for slot in group["slots"]]


def check_slots(lowered, snapshot):
    """(skeleton path, [(slot name, document path)]) the montage will need, or None when unchanged.

    Authored slot tracks name their slots; otherwise a new montage keeps its native DefaultSlot track
    and an existing one keeps its tracks (checked when the Skeleton changes).
    """
    skeleton = lowered.get("skeleton") if "skeleton" in lowered else (snapshot or {}).get("skeleton")
    if not skeleton or ("slots" not in lowered and "skeleton" not in lowered):
        return None
    if "slots" in lowered:
        names = [(slot["name"], f"/Body/SlotAnimTracks/{index}/SlotName")
                 for index, slot in enumerate(lowered["slots"])]
    elif snapshot is None:
        names = [(NATIVE_DEFAULT_SLOT, "/Body/SlotAnimTracks")]
    else:
        names = [(slot["name"], "/Body/Skeleton") for slot in snapshot.get("slots", [])]
    return skeleton, names


# A Skeleton outside /Game (engine or plugin content) cannot take a Skeleton document.
READ_ONLY_SKELETON_HINT = (
    " This Skeleton is engine or plugin content (read-only): copy it into /Game with duplicate_asset, add the slot "
    "to the copy (list the original in its Properties.CompatibleSkeletons to keep using the original's animations) "
    "and use the copy as the montage's Skeleton.")


def require_slots(context, lowered, snapshot):
    """Every slot track names a slot of the montage's Skeleton (AnimMontage.UnknownSlot otherwise)."""
    needed = check_slots(lowered, snapshot)
    if needed is None:
        return
    skeleton, names = needed
    slots = skeleton_slots(context, skeleton)
    for name, path in names:
        if name not in slots:
            new_track = path == "/Body/SlotAnimTracks"
            raise error(
                f"Skeleton {skeleton} has no slot '{name}'"
                + (" (a new montage starts with one DefaultSlot track when SlotAnimTracks is omitted)" if new_track else "")
                + f". Its slots: {', '.join(slots) if slots else 'none'}. Use one of them in SlotAnimTracks, or add the "
                "slot to the Skeleton first with a Skeleton document (Class /Script/Engine.Skeleton, Body.SlotGroups)."
                + ("" if str(skeleton).startswith("/Game/") else READ_ONLY_SKELETON_HINT),
                path, "AnimMontage.UnknownSlot")


def check_timeline(context, lowered, read):
    """The read-only preparation of the document's timeline edit on the existing target (CHECK): its first diagnostic,
    exactly as Apply reports it. A new Montage is checked by Apply itself."""
    values = {key: lowered[key] for key in TIMELINE_KEYS if key in lowered}
    if "notify_tracks" in values:
        values["notify_tracks"] = [row["name"] for row in values["notify_tracks"]]
    if not values:
        return
    invoke(context, CHECK, values, (NamedOpaqueHandleArgument("asset", animation_handle(context, read)),))


def plan(lowered, editor_rows=(), settings_intent=None):
    """Compile one deterministic contribution per native owned surface group.

    Structural surfaces come first; plain reflected settings (one generic Editor
    batch) and the Montage settings primitive run last so their derived refresh
    (sync markers, time stretch bake) sees the final timeline.
    """
    entries = []
    for key, primitive in (("curves", CURVES), ("preview_mesh", PREVIEW)):
        if key in lowered:
            values = ({"curves": lowered[key]} if key == "curves" else
                      {"operation": "clear" if lowered[key] is None else "set", "preview_mesh_path": lowered[key]})
            entries.append((primitive, values))
    structure = {key: lowered[key] for key in ("slots", "sections") if key in lowered}
    notification = {key: lowered[key] for key in ("notifies", "notify_states") if key in lowered}
    if notification and "notify_tracks" in lowered:
        # The whole notify track list, published with the notifies on it (validate_notify_tracks).
        notification["notify_tracks"] = [row["name"] for row in lowered["notify_tracks"]]
    skeleton = None
    if "skeleton" in lowered:
        skeleton = (SKELETON, {"operation": "clear" if lowered["skeleton"] is None else "set",
                                "skeleton_path": lowered["skeleton"]})
    if skeleton and not structure:
        entries.append(skeleton)
    if notification:
        if structure:
            notification["structure"] = structure
        entries.append((NOTIFIES if "notifies" in notification else STATES, notification))
    elif structure:
        entries.append((SLOTS if "slots" in structure else SECTIONS, structure))
    if skeleton and structure:
        entries.append(skeleton)
    if editor_rows:
        entries.append((EDITOR_WRITE, {"properties": list(editor_rows)}))
    if editor_rows or settings_intent:
        # Always last: an empty intent still runs the editor's derived refresh
        # (sync markers, time stretch bake) after plain field writes, and binds
        # the Montage request owner whose staged snapshot verifies the result.
        entries.append((SETTINGS, settings_intent or {}))
    return [{"Index": index, "PrimitiveId": primitive, "Version": BY_ID[primitive].version,
             "Phase": "Apply", "InputSchemaId": primitive + (".input.v2" if primitive == CURVES else ".input.v1"),
             "Intent": values} for index, (primitive, values) in enumerate(entries)]


def load_setting_references(context, settings):
    for path, cls in settings_codec.references(settings):
        _require_reference(context, path, cls)


def _admitted_class(context):
    """Admitted Montage fields of the class (create: no target to enumerate yet)."""
    result = invoke(context, CLASS_ENUMERATE, {"exact_asset_class_path": CLASS})
    rows = result.value.get("properties") if type(result.value) is dict else None
    if type(rows) is not list:
        raise error("Class property enumeration must return rows.", "/PrimitiveOutcome")
    return rows


def created_body(body):
    """A new Montage owns only the curves its document authors (omitted means none)."""
    return body if "Curves" in body else {**body, "Curves": []}


def preflight(document, context):
    lowered, fields, intent, settings = lower_document(document)
    load_references(context, lowered)
    load_setting_references(context, settings)
    exists, read, _ = resolve(context, document["Target"], CLASS)
    rows = []
    # A new Montage starts with the native default curve name.
    native_curve = NATIVE_TIME_STRETCH_CURVE
    snapshot = None
    if exists:
        snapshot = read_snapshot(context, read)
        native = snapshot.get("settings") if type(snapshot) is dict else None
        native_curve = native["time_stretch"]["curve_name"] if type(native) is dict else None
    require_slots(context, lowered, snapshot)
    if exists:
        check_timeline(context, lowered, read)
    body = document.get("Body", {})
    settings_codec.validate_time_stretch_curve(settings, body if exists else created_body(body), native_curve)
    if fields:
        admitted = _admitted_batch(context, read)[0] if exists else _admitted_class(context)
        rows = editor_rows(fields, admitted, _PLAIN_PROPERTY_CODECS)
    steps = plan(lowered, rows, intent)
    if not exists:
        if lowered.get("skeleton") is None:
            raise error("Create requires an explicit effective Skeleton.", "/Body/Skeleton")
        # One apply: the shared factory create is step 0, every Body/Properties
        # step follows on the new asset (created-asset rollback covers them all).
        package_path, asset_name = document["Target"].rsplit("/", 1)
        steps.insert(0, {"Index": 0, "PrimitiveId": CREATE, "Version": LIFECYCLE.create_asset.version,
                         "Phase": "Apply", "InputSchemaId": CREATE + ".input.v1", "Intent": {
                             "asset_class_path": CLASS, "factory_context_path": lowered["skeleton"],
                             "package_path": package_path, "asset_name": asset_name}})
        for index, step in enumerate(steps):
            step["Index"] = index
    return steps


def apply(document, read_asset_handle, mutable_asset_handle, context):
    lowered, fields, intent, settings = lower_document(document)
    load_references(context, lowered)
    load_setting_references(context, settings)
    target = animation_handle(context, mutable_asset_handle, "mutable")
    rows, batch = [], None
    if fields:
        admitted, batch = _admitted_batch(context, read_asset_handle)
        rows = editor_rows(fields, admitted, _PLAIN_PROPERTY_CODECS)
    changes = []
    for step in plan(lowered, rows, intent):
        primitive, values = step["PrimitiveId"], step["Intent"]
        if primitive == EDITOR_WRITE:
            changes.extend(invoke(context, primitive, values, (
                NamedOpaqueHandleArgument("asset", mutable_asset_handle),
                NamedOpaqueHandleArgument("property_batch", batch))).changed_set)
            continue
        handles = [NamedOpaqueHandleArgument("asset", target)]
        if primitive in (SKELETON, PREVIEW):
            reference, cls = (("skeleton", "/Script/Engine.Skeleton") if primitive == SKELETON else
                              ("preview_mesh", "/Script/Engine.SkeletalMesh"))
            if values["operation"] == "set":
                read = _require_reference(context, values[reference + "_path"], cls)
                handles.append(NamedOpaqueHandleArgument(reference, animation_handle(context, read)))
            values = {"operation": values["operation"]}
        changes.extend(invoke(context, primitive, values, tuple(handles)).changed_set)
    return tuple(changes)


def create_target_context(document, context):
    body = lower_body({key: value for key, value in document.get("Body", {}).items() if key in SUPPORTED})
    if body.get("skeleton") is None:
        raise error("Create requires an explicit effective Skeleton.", "/Body/Skeleton")
    read = _require_reference(context, body["skeleton"], "/Script/Engine.Skeleton")
    return (NamedOpaqueHandleArgument("factory_context", read),)
