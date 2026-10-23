#!/usr/bin/env python3
# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Render docs/architecture/asset-document-coverage-audit.md from the audit JSON.

Requires ENGINE_DIR (the engine's Engine folder), which maps modules to plugins.
Usage: build_report.py <coverage-audit.json> <modal-scan.json> <out.md> [<previous-coverage-audit.json> [<base-coverage-audit.json>]]

Inputs are produced by the UEAssetDoc.Coverage.GenericRouteAudit automation test
(coverage-audit.json) and scan_modal_factories.py (modal-scan.json). All numbers in the document are computed
here; the curated "non-property structure" column and the shortlist are JUDGEMENT and are labelled as such.
With a previous audit JSON the report gains a per-class "changes since the previous run" section; a base audit JSON
(the same audit re-run at the commit a change started from) adds the change's own before/after headline numbers.
Which classes own a Profile is read from MCP/schemas/assetdocument/*.md ("Classes:" lines), the documentation MCP
serves, so runs made before a Profile existed are compared with the same set.
"""
import collections
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(__file__))
import module_plugin_map  # noqa: E402

audit = json.load(open(sys.argv[1]))
modal = json.load(open(sys.argv[2]))
out_path = sys.argv[3]
previous = json.load(open(sys.argv[4])) if len(sys.argv) > 4 else None
base_run = json.load(open(sys.argv[5])) if len(sys.argv) > 5 else None
modmap = module_plugin_map.build()

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
SCHEMA_DIR = os.path.join(REPO, "MCP", "schemas", "assetdocument")


def profile_classes():
    """Short class names of every Profile section (MCP/schemas/assetdocument/<Name>.md "Classes:" line), except Generic."""
    found = {}
    for file in sorted(os.listdir(SCHEMA_DIR)):
        if not file.endswith(".md") or file in ("Generic.md", "PolicyMetadata.md"):
            continue
        m = re.search(r"^Classes: (.+)$", open(os.path.join(SCHEMA_DIR, file)).read(), re.M)
        for entry in (m.group(1).split(",") if m else []):
            entry = entry.strip()
            if entry.startswith("/"):
                found[entry.split(".")[-1]] = file[:-3]
    return found


PROFILES = profile_classes()
# Classes with their own Profile: the audit flags them (CreateTemplate returns the Profile's template), and the MCP
# Profile sections list them. Their create uses the Profile template's placeholders and their edit step is skipped, so
# they are counted apart from the generic route's own gaps.
SPECIALIZED = {r["class"].split(".")[-1] for r in audit["classes"] if r.get("specialized_profile")} | set(PROFILES)
# Shortlist rows covered by tools other than an asset Profile.
OTHER_TOOLS = {"World": "level tools (`AssetDocumentLevel`: query/spawn/edit actors in a level)"}

# JUDGEMENT: non-property structure that humans edit in the UE editor for each class (not derived from data).
STRUCTURE = {
    "Material": "expression graph (nodes, pins, links, comments, reroutes), material attributes pins, custom HLSL",
    "ValidationMaterial": "expression graph (see Material)",
    "PreviewMaterial": "expression graph (see Material)",
    "MaterialFunction": "expression graph with function inputs/outputs",
    "MaterialFunctionMaterialLayer": "expression graph (layer function)",
    "MaterialFunctionMaterialLayerBlend": "expression graph (blend function)",
    "MaterialInstanceConstant": "parameter override lists (scalar/vector/texture/static switches/layers) edited in the instance editor",
    "LandscapeMaterialInstanceConstant": "parameter override lists (see MaterialInstanceConstant)",
    "MaterialFunctionInstance": "parameter override lists",
    "MaterialFunctionMaterialLayerInstance": "parameter override lists",
    "MaterialFunctionMaterialLayerBlendInstance": "parameter override lists",
    "MaterialParameterCollection": "scalar and vector parameter arrays (these are the content)",
    "NiagaraSystem": "emitter handles, system/emitter module stacks, scratch pad scripts, renderers, user parameters",
    "NiagaraEmitter": "module stacks (spawn/update/event/simulation stages), renderers, scratch pad scripts, versions",
    "NiagaraScript": "node graph (module/function/dynamic-input scripts)",
    "NiagaraParameterCollection": "parameter list",
    "NiagaraParameterCollectionInstance": "parameter overrides",
    "NiagaraParameterDefinitions": "parameter definition list",
    "NiagaraDataChannelAsset": "channel layout / variables",
    "NiagaraStatelessEmitterTemplate": "template modules",
    "ParticleSystem": "Cascade emitters, modules, LOD levels, curves",
    "SoundCue": "sound node graph (mixers, randomizers, attenuation, modulators; per-node properties)",
    "MetaSoundSource": "MetaSound node graph (inputs, outputs, nodes, connections)",
    "MetaSoundPatch": "MetaSound node graph",
    "SoundClass": "class hierarchy (parent/child) and per-class properties",
    "SoundMix": "class adjuster list",
    "SoundSubmix": "submix hierarchy, effect chain, sends",
    "SoundfieldSubmix": "submix hierarchy, effect chain",
    "EndpointSubmix": "submix hierarchy, effect chain",
    "SoundfieldEndpointSubmix": "submix hierarchy, effect chain",
    "SoundAttenuation": "attenuation settings struct (shape, curves, spatialization)",
    "SoundEffectSourcePresetChain": "ordered effect chain entries",
    "AudioBus": "none beyond properties",
    "DialogueWave": "context mappings (speaker/target voice -> sound wave)",
    "SoundSourceBus": "sends and effect chain",
    "ModularSynthPresetBank": "preset list",
    "WaveTableBank": "wavetable entries",
    "StateTree": "state hierarchy, transitions, tasks, conditions, evaluators, global tasks, parameter bag",
    "DataTable": "rows (typed by RowStruct); cells are the content",
    "CompositeDataTable": "parent table list and merged rows",
    "MirrorDataTable": "bone/curve/notify mirror pairs",
    "CurveTable": "row curves (keys per row)",
    "CompositeCurveTable": "parent table list",
    "StringTable": "namespace and key->text entries (the content)",
    "UserDefinedStruct": "member variable list (names, types, defaults, tooltips)",
    "UserDefinedEnum": "enumerator list and display names",
    "CurveFloat": "curve keys (specialized profile covers them)",
    "CurveVector": "three float curves with keys",
    "CurveLinearColor": "four float curves with keys / gradient",
    "CurveLinearColorAtlas": "gradient curve list",
    "EaseCurve": "easing curve keys/tangents",
    "EaseCurveLibrary": "preset list",
    "InputAction": "trigger and modifier instances (instanced objects)",
    "InputMappingContext": "key mappings with per-mapping triggers and modifiers",
    "PlayerMappableInputConfig": "context list",
    "BlendSpace": "sample points (animation, X/Y) and axis settings",
    "BlendSpace1D": "sample points and axis settings",
    "AimOffsetBlendSpace": "sample points (aim poses)",
    "AimOffsetBlendSpace1D": "sample points (aim poses)",
    "AnimComposite": "section/segment track of animation sequences",
    "AnimMontage": "sections, slots, segments, notifies, branching points (specialized profile)",
    "AnimSequence": "tracks, notifies, curves, sync markers (specialized profile)",
    "PoseAsset": "poses and pose curves",
    "AnimBank": "banked sequence list",
    "AnimBankData": "baked data",
    "AnimCurveCompressionSettings": "none beyond properties",
    "AnimBoneCompressionSettings": "codec list",
    "PhysicsAsset": "bodies, shapes, constraints, profiles (Physics Asset Editor)",
    "SkeletalMesh": "LOD settings, materials/slots, morph targets, sockets, clothing (import-oriented)",
    "Skeleton": "bone tree, sockets, virtual bones, blend profiles, retarget modes",
    "PreviewMeshCollection": "preview skeletal mesh list",
    "IKRigDefinition": "goals, solver stack (instanced structs), retarget chains, skeleton bindings",
    "IKRetargeter": "chain mappings, retarget ops stack, pose editing",
    "ControlRigShapeLibrary": "shape list",
    "OptimusDeformer": "compute graph nodes/resources/variables",
    "Dataflow": "dataflow node graph",
    "LevelSequence": "movie scene: bindings, tracks, sections, channels/keys, camera cuts, markers",
    "TemplateSequence": "movie scene tracks and bindings",
    "CameraAnimationSequence": "movie scene tracks and keys",
    "CameraAsset": "camera rigs, director, transitions, parameter definitions",
    "CameraRigAsset": "node tree, interface parameters",
    "CameraRigProxyAsset": "proxy tables",
    "CameraShakeAsset": "shake pattern nodes",
    "CameraVariableCollection": "variable list",
    "LevelVariantSets": "variant sets, variants, bindings, captured properties",
    "World": "actors, levels, world partition, data layers (map editing)",
    "DataLayerAsset": "none beyond properties",
    "ExternalDataLayerAsset": "none beyond properties",
    "HLODLayer": "none beyond properties",
    "FoliageType_Actor": "none beyond properties",
    "FoliageType_InstancedStaticMesh": "none beyond properties",
    "LandscapeGrassType": "grass varieties list",
    "ProceduralFoliageSpawner": "foliage type entries",
    "PaperSprite": "source region/pivot, collision polygons, render geometry",
    "PaperFlipbook": "key frame list (sprite, duration)",
    "PaperTileMap": "tile layers and tile painting",
    "PaperTileSet": "per-tile metadata and collision",
    "PaperTerrainMaterial": "terrain material rules",
    "Font": "composite font typefaces and glyph data",
    "SlateBrushAsset": "none beyond properties (brush struct)",
    "SlateVectorArtData": "vector art data",
    "SlateWidgetStyleAsset": "style container object (chosen class)",
    "TouchInterface": "touch control list",
    "GameplayCueSet": "cue notify list",
    "GameplayCueManager": "none beyond properties",
    "GameplayAbilitySet": "ability binding list",
    "GameplayTagReponseTable": "tag response entries",
    "DataRegistry": "item source list",
    "EnvQuery": "query option/test tree (specialized AI tooling needed)",
    "BehaviorTree": "node tree (specialized profile)",
    "BlackboardData": "keys (specialized profile)",
    "RuntimeVirtualTexture": "none beyond properties",
    "VirtualTextureBuilder": "built data (not human-edited)",
    "Texture2D": "pixel data (import), mip/compression settings, LOD groups",
    "Texture2DArray": "slice list (import)",
    "TextureCubeArray": "slice list (import)",
    "TextureRenderTarget2D": "none beyond properties",
    "VolumeTexture": "volume data (import)",
    "MediaTexture": "none beyond properties",
    "MediaPlaylist": "media source list",
    "GroomAsset": "hair groups, LOD and interpolation settings, dataflow template (import-oriented)",
    "GroomBindingAsset": "binding target groups",
    "Skeleton ": "",
    "ChaosSolver": "none beyond properties",
    "ChaosCacheCollection": "cache list",
    "GeometryCollection": "hierarchy of geometry pieces / clustering",
    "ComputeGraphFromText": "compute shader source text and bindings",
    "OptimusSource": "HLSL source",
    "WidgetPreview": "preview widget configuration",
    "InteractiveToolsPresetCollectionAsset": "preset store",
    "TaggedAssetBrowserConfiguration": "tag filter / grouping entries",
    "PhysicalMaterial": "none beyond properties",
    "PhysicalMaterialMask": "mask texture",
    "NeuralProfile": "none beyond properties",
    "ObjectLibrary": "asset/class lists",
    "AnimationSharingSetup": "per-skeleton setup list",
    "DialogueVoice": "none beyond properties",
}

LABELS = {"pass": "ok", "fail": "FAIL", "partial": "partial", "n/a": "-", "skipped": "-", None: "-"}


def short(name):
    return name.split(".")[-1]


def cell(step):
    if not isinstance(step, dict):
        return "-"
    return LABELS.get(step.get("status"), str(step.get("status")))


def md(text):
    return str(text).replace("|", "\\|").replace("\n", " ")


rows = audit["classes"]
skipped = [r for r in rows if "skipped" in r]
audited = [r for r in rows if "skipped" not in r]
created = [r for r in audited if r["create"]["status"] == "pass"]
by_name = {short(r["class"]): r for r in rows}


def plugin_of(module):
    if module in modmap:
        return "%s (plugin %s)" % (module, modmap[module]) if module != modmap[module] else "%s (plugin)" % module
    return module if module == "Engine" else "%s (Engine)" % module


def codes(step):
    return (step or {}).get("codes") or []


# --------------------------------------------------------------------------------------- statistics
def step_counts(step):
    return collections.Counter((r.get(step) or {}).get("status") for r in audited)


STEP_ROWS = ["template", "create", "extract", "edit", "reload", "noop_diff", "noop_full"]
steps = {s: step_counts(s) for s in STEP_ROWS}

props_vis = sum(r["properties"]["details_visible"] for r in created)
props_emit = sum(r["properties"]["emitted"] for r in created)
props_drop = sum(r["properties"]["dropped"] for r in created)
props_const = sum(r["properties"].get("edit_const_not_authorable", 0) for r in created)


def drop_category(f):
    text = f["kind"] + " " + f["reason"]
    if "instanced_subobject" in text:
        return "instanced subobject (Instanced/EditInlineNew UObject, direct or in a container)"
    if "InstancedStruct" in text or "PropertyBag" in text:
        return "FInstancedStruct / instanced property bag"
    if "object_ref" in text:
        return "hard UObject reference (direct, in array/set/map, or inside a struct)"
    if "class_ref" in text:
        return "hard class reference (UClass*, TSubclassOf)"
    if re.search(r"\btext\b", text):
        return "FText (direct, in container or in struct)"
    if "transient" in f["reason"]:
        return "edit-visible but Transient / DuplicateTransient"
    return "struct or container with an unadmitted member (long tail; see below)"


gap_classes = collections.defaultdict(set)
gap_props = collections.Counter()
long_tail = collections.defaultdict(set)
for r in created:
    for f in r["properties"]["dropped_list"]:
        c = drop_category(f)
        gap_classes[c].add(short(r["class"]))
        gap_props[c] += 1
        if c.startswith("struct or container"):
            long_tail[f["kind"]].add(short(r["class"]))

TRIVIAL_INNER = {"AssetImportData"}
hidden = [r for r in created if r["properties"].get("hidden_structure_candidates") or r["properties"].get("default_inner_objects")]
hidden_strong = [r for r in created if set(r["properties"].get("default_inner_objects", {})) - TRIVIAL_INNER]
emit_nothing = [r for r in created if r["properties"]["emitted"] == 0 and short(r["class"]) not in SPECIALIZED]
noop_fail = [r for r in audited if r["noop_diff"]["status"] == "fail"]
noop_float = [r for r in noop_fail if r.get("noop_full_ok_with_float_literals")]
create_failed = [r for r in audited if r["create"]["status"] != "pass"]
create_by_code = collections.defaultdict(list)
for r in create_failed:
    create_by_code[(codes(r["create"]) or ["(no code)"])[0]].append(r)
incomplete = [r for r in audited if short(r["class"]) not in SPECIALIZED and (codes(r["create"]) or [""])[0] in (
    "FactoryCreate.IncompleteCreation", "FactoryCreate.SubclassUnsupported", "FactoryCreate.MissingFactoryContext")]
resolve_fail = create_by_code.get("ClassResolve.Unsupported", [])
scratch_fail = create_by_code.get("ScratchCreate.InvalidIdentity", [])
clone_fail = [r for r in audited if "Scratch.CloneFailed" in codes(r.get("edit"))]
edit_bad = [r for r in audited if r["edit"]["status"] in ("fail", "partial")]
edit_by_code = collections.defaultdict(list)
for r in edit_bad:
    edit_by_code[(codes(r["edit"]) or ["(no code)"])[0]].append(r)
reload_fail = [r for r in audited if r["reload"]["status"] == "fail"]
generic_group = [r for r in audited if short(r["class"]) not in SPECIALIZED]
generic_create_failed = [r for r in create_failed if short(r["class"]) not in SPECIALIZED]


def generic_headline(run):
    """Headline counts over a run's audited classes without a Profile (the generic route's own result)."""
    rs = [r for r in run["classes"] if "skipped" not in r and short(r["class"]) not in SPECIALIZED]
    count = lambda step, st: sum(1 for r in rs if (r.get(step) or {}).get("status") == st)
    made = [r for r in rs if r["create"]["status"] == "pass"]
    return collections.OrderedDict([
        ("audited", len(rs)), ("create pass", count("create", "pass")), ("create fail", count("create", "fail")),
        ("edit pass", count("edit", "pass")), ("edit partial", count("edit", "partial")), ("edit fail", count("edit", "fail")),
        ("reload pass", count("reload", "pass")), ("reload fail", count("reload", "fail")),
        ("no-op pass", count("noop_diff", "pass")),
        ("properties emitted", sum(r["properties"]["emitted"] for r in made)),
        ("properties dropped", sum(r["properties"]["dropped"] for r in made)),
        ("refused classes", len(run["refused_classes"]))])

crash_rows = [r for r in skipped if r["skipped"].startswith(("EDITOR CRASH", "MODAL"))]
fixture_rows = [r for r in skipped if r not in crash_rows]

refused = audit["refused_classes"]
refused_by_code = collections.Counter(r["code"] for r in refused)
factories = audit["factories"]
fac_verdict = collections.Counter(
    ("admitted" if f["verdict"] == "admitted" else f["verdict"].split("(")[0].strip()[:60]) for f in factories)

# --------------------------------------------------------------------------------------- writing
W = []


def w(line=""):
    W.append(line)


def pct(n, d):
    return "%d/%d" % (n, d)


w("# AssetDocument generic-route coverage audit")
w()
w("Generated by `Scripts/AssetDocument/coverage_audit/build_report.py` from `asset-document-coverage-audit.json`")
w("(raw per-class results of `UEAssetDoc.Coverage.GenericRouteAudit`). Engine %s, audit run %s." % (
    audit["engine_version"], audit["generated_utc"]))
w()
w("Convention: statements marked **Evidence** come straight from the measured run (numbers are computed from the JSON);")
w("statements marked **Judgement** are the author's reading of Engine source or UE usage and were not measured.")
w()
w("## 1. What was measured")
w()
w("Every class that the generic factory lifecycle (`factory_generic.v1`) can address in the sandbox editor (Engine plus all plugins "
  "enabled in the sandbox project) was enumerated with the same selection rules the lifecycle uses "
  "(concrete, non-deprecated factory whose CDO says `CanCreateNew`, default workflow, `SupportedClass` set; an ancestor factory is only "
  "trusted when it shows in the New menu; Blueprint classes and abstract classes are refused). %d classes are creatable, %d are refused "
  "(section 3.3). For each creatable class the test drives the production service "
  "(`FAssetDocumentModule::Get().GetService()`) through: CreateTemplate, ApplyFile create under a test-owned `/Game` path, Extract, "
  "an edit of up to ~5 properties of varied kinds (Apply, Extract, compare), save + unload + reload + Extract compare, an unchanged re-apply "
  "(expected no-op: `bSavedAsset == false`), and cleanup (undo history reset before the forced delete). Details-visible properties "
  "(`CPF_Edit`) are compared with what Extract emits to find silently dropped kinds." % (
      audit["creatable_class_count"], len(refused)))
w()
w("Scope notes. %d of the %d creatable classes were not run: %d AssetDocument plugin test fixtures and %d classes that crash or hang the editor "
  "(section 3.2). The %d classes with a specialized Python profile (%s) are included, but their edit step is skipped "
  "because their documents use profile-specific forms rather than the generic property syntax; their create step uses the generic template, "
  "which lacks the explicit inputs some profiles require. The audit runs with `-unattended -NoPause` and no sound device." % (
      len(skipped), audit["creatable_class_count"], len(fixture_rows), len(crash_rows), len(SPECIALIZED), ", ".join(sorted(SPECIALIZED))))
w()
w("## 2. Headline numbers (Evidence)")
w()
w("Audited classes: **%d** (of %d creatable; %d skipped). Rows below count audited classes only." % (
    len(audited), audit["creatable_class_count"], len(skipped)))
w()
w("| Step | pass | fail | partial | n/a or not reached |")
w("|---|---|---|---|---|")
labels = {"template": "CreateTemplate", "create": "ApplyFile create", "extract": "Extract", "edit": "Edit (~5 properties)",
          "reload": "Save, unload, reload, Extract equal", "noop_diff": "Unchanged re-apply, diff-only extract",
          "noop_full": "Unchanged re-apply, full extract"}
for s in STEP_ROWS:
    c = steps[s]
    other = len(audited) - c.get("pass", 0) - c.get("fail", 0) - c.get("partial", 0)
    w("| %s | %d | %d | %d | %d |" % (labels[s], c.get("pass", 0), c.get("fail", 0), c.get("partial", 0), other))
w()
noop_other = [r for r in noop_fail if r not in noop_float]
cleanup_failed = [short(r["class"]) for r in audited if r.get("cleanup_ok") is False]
noop_text = ("The unchanged re-apply is a no-op for every created class." if not noop_fail else
             "The unchanged re-apply step fails for %d classes (%d of them pass when the same document is re-written with `.0` float literals; "
             "the others: %s)." % (len(noop_fail), len(noop_float), ", ".join("%s %s" % (short(r["class"]), "/".join(codes(r["noop_diff"]))) for r in noop_other) or "none"))
w("Notes on the table. Create passed for %d of %d audited classes; every later step is only attempted for those, so \"n/a or not reached\" "
  "for later steps is dominated by the %d create failures (section 3.4). The edit step is \"n/a\" for %d classes because Extract emits no authorable "
  "property at all (section 4) or because the class is specialized (edit skipped). %s Cleanup failed for %s. "
  "Total audit time %.0f s (about %.1f s per class)." % (
      len(created), len(audited), len(create_failed),
      sum(1 for r in audited if r["edit"]["status"] == "n/a"), noop_text,
      ", ".join(cleanup_failed) if cleanup_failed else "no class", sum(r.get("seconds", 0) for r in audited),
      sum(r.get("seconds", 0) for r in audited) / max(1, len(audited))))
w()
gh = generic_headline(audit)
w("Generic route alone (the %d audited classes without a Profile; Profile classes are created from their Profile template, whose "
  "placeholders a real document replaces, and their edit step is skipped): create **%d pass / %d fail**, edit %d pass / %d partial / %d fail, "
  "reload %d pass / %d fail, unchanged re-apply %d pass." % (
      gh["audited"], gh["create pass"], gh["create fail"], gh["edit pass"], gh["edit partial"], gh["edit fail"],
      gh["reload pass"], gh["reload fail"], gh["no-op pass"]))
w()
w("Property visibility over the %d classes that were created: %d Details-visible (`CPF_Edit`) properties, **%d emitted** by Extract, "
  "**%d dropped**, %d `EditConst` (visible but not authorable by design)." % (
      len(created), props_vis, props_emit, props_drop, props_const))
w()

# -------------------------------------------------------------------------------------- findings
def first_create_code(name):
    r = by_name.get(name)
    if not r or "skipped" in r:
        return None
    return (codes(r["create"]) or [None])[0] if r["create"]["status"] != "pass" else "creates"


w("## 3. Findings that need a product decision")
w()
w("### 3.1 Factories that open modal UI (Evidence + Engine source)")
w()
w("The first audit run hung for 13+ minutes on a Slate modal \"Select a Groom Dataflow Template\" that appeared on the user's screen "
  "while `GroomAsset` was being created; `-unattended` does not stop it. An MCP tool must never block on, or show, UI. "
  "The generic lifecycle now cancels a factory that opens a modal window while creating and refuses the class: in this run "
  "GroomAsset reports `%s`." % (first_create_code("GroomAsset") or "not audited"))
w()
w("- Call path (Evidence, read from Engine source): `GroomFactory::FactoryCreateNew` -> `UE::Groom::BuildGroomDataflowAsset` "
  "(`HairStrandsFactory.cpp`) -> `SMessageDialog::ShowModal()`. The modal is shown from `FactoryCreateNew` itself, which is the call "
  "the generic lifecycle makes, so the generic route reaches it with no `ConfigureProperties` involved. The same helper is called from "
  "`GroomActions.cpp` for the asset-type action.")
w("- `-unattended` is passed to the test run. It does **not** suppress this dialog. Engine source: `FMessageDialog::Open` returns its "
  "default reply when `FApp::IsUnattended()`; `SMessageDialog::ShowModal` and `FSlateApplication::AddModalWindow` have no such check. "
  "Any factory or hook that uses the Slate widgets directly can therefore block the editor game thread forever, even unattended.")
w("- Why it matters for the product: the MCP path goes through `editor.scratch.create_by_factory` / the factory lifecycle, runs on the game thread, "
  "and has no timeout. A single hostile or merely interactive factory freezes the editor and the MCP session.")
w("- Other classes where modal UI is reachable (Evidence for the call sites; **Judgement** for whether the generic path reaches them, because the run was "
  "protected by the skip list and the stall watchdog and `ConfigureProperties` is never called by the generic route):")
w()
w("| Admitted factory | Class | Function containing the UI call | Call(s) | Reached by generic route? |")
w("|---|---|---|---|---|")
w("| GroomFactory | GroomAsset | FactoryCreateNew (via BuildGroomDataflowAsset) | SMessageDialog::ShowModal | Reached (first run hung); now refused with `%s` |" % (
    first_create_code("GroomAsset") or "not audited"))
fc_rows = []
cp_rows = []
for fac, entries in sorted(modal.items()):
    for e in entries:
        (cp_rows if e["function"] == "ConfigureProperties" else fc_rows).append((fac, e))
refused_names = {short(r["class"]) for r in refused}
for fac, e in fc_rows:
    reach = ("Only on an error branch; `FMessageDialog::Open` returns its default under `-unattended`"
             if e["tokens"] == ["FMessageDialog::Open"] else "Possible: check")
    if short(e["supported_class"]) in refused_names:
        reach += "; class is refused by the generic lifecycle anyway"
    w("| %s | %s | %s | %s | %s |" % (fac, short(e["supported_class"]), e["function"], ", ".join(e["tokens"]), reach))
w()
w("Modal UI inside `ConfigureProperties` (the interactive configuration step: class pickers via `SClassPickerDialog`/`PickClass`, "
  "`AddModalWindow` option dialogs, a file dialog for fonts): %d admitted factories. **Judgement**: the generic route builds a transient factory "
  "and never calls `ConfigureProperties` (none of these factories blocked the audit run), so these dialogs are not reached; "
  "the consequence is the opposite problem, namely that the factory then runs with class defaults and often cannot create the requested class "
  "(section 3.4). Factories: %s." % (len(cp_rows), ", ".join("%s (%s)" % (f, short(e["supported_class"])) for f, e in cp_rows)))
w()
w("The static scan (`coverage_audit/scan_modal_factories.py`) only sees UI calls written directly inside the factory functions; it does "
  "not follow helper calls (it does not even find the Groom case). Other plugins using `SMessageDialog` that are not loaded in the sandbox and "
  "therefore were not exercised: `ChaosOutfitAsset/OutfitAssetFactory.cpp`. **Judgement**: expect more in third-party plugins; the "
  "reliable defence is not a deny list but refusing or isolating interactive factories on the MCP path (for example a modal-window guard around "
  "factory invocation plus a per-call timeout), which is a separate task.")
w()
w("Audit hardening: the test keeps a skip list with reasons (empty now; recheck an entry with `ASSETDOC_COVERAGE_IGNORE_SKIPLIST=1`). "
  "It writes `current.txt` before each class and appends to `results.jsonl` after it. A class that opens a modal window or crashes the "
  "editor stops both files advancing: when neither has changed for several minutes (a cold derived-data cache can legitimately block a "
  "class for minutes, so allow at least 240 s, and longer while the editor starts), kill the editor and start it again with "
  "`ASSETDOC_COVERAGE_RESUME=1`. The next run records the in-flight class as hung/crashed and continues with the next class.")
w()
w("### 3.2 Editor crashes and hangs reached through the generic route (Evidence)")
w()
if crash_rows:
    w("These classes are skip-listed after crashing or hanging the editor. Each is a way an MCP request can take the editor down.")
    w()
    w("| Class | Reason |")
    w("|---|---|")
    for r in crash_rows:
        w("| %s | %s |" % (short(r["class"]), md(r["skipped"])))
else:
    w("None in this run: the skip list holds only plugin test fixtures. Classes that crashed or hung earlier runs are now refused with a "
      "stable diagnostic (code observed in this run):")
    w()
    w("| Class | Earlier failure | Now |")
    w("|---|---|---|")
    for name, before, step in [
            ("GroomAsset", "modal dialog from FactoryCreateNew (hang)", "create"),
            ("LightWeightInstanceManager", "factory needs dialog context (crash)", "create"),
            ("Skeleton", "factory needs dialog context (crash)", "create"),
            ("SlateWidgetStyleAsset", "factory needs dialog context (crash)", "create"),
            ("SoundEffectSourcePresetChain", "property write notifies a missing audio device manager (crash)", "edit"),
            ("MetaSoundSource", "transient scratch clone of a MetaSound document (crash)", "edit"),
            ("MetaSoundPatch", "transient scratch clone of a MetaSound document (crash)", "edit")]:
        r = by_name.get(name)
        refused_row = next((x for x in refused if short(x["class"]) == name), None)
        if refused_row:
            now = "refused by the lifecycle: %s (section 3.3)" % refused_row["code"]
        elif not r or "skipped" in r:
            now = "not audited"
        else:
            step_result = r.get(step) or {}
            now = "%s %s" % (step, "/".join(codes(step_result)) or step_result.get("status", "-"))
        w("| %s | %s | %s |" % (name, before, md(now)))
w()
w("Fixture classes skipped (not product assets): %s." % ", ".join(short(r["class"]) for r in fixture_rows))
w()
w("### 3.3 What the generic lifecycle refuses (Evidence)")
w()
w("Refused classes: %d. By code: %s." % (len(refused), ", ".join("%s x%d" % kv for kv in refused_by_code.most_common())))
w()
w("| Class | Code | Reason |")
w("|---|---|---|")
for r in sorted(refused, key=lambda r: (r["code"], r["class"])):
    w("| %s | %s | %s |" % (short(r["class"]), r["code"], md(r["reason"])))
w()
w("Factories examined: %d (%s). Factories refused are import/open-only (`bCreateNew == false`) or have no `SupportedClass`, so they never "
  "produce an asset from nothing; they are not a gap of the create lifecycle." % (
      len(factories), ", ".join("%s: %d" % kv for kv in fac_verdict.most_common())))
w()
w("Admitted DataAsset subclasses (the generic `DataAssetFactory` is trusted as an ancestor factory): %d classes were audited via "
  "`DataAssetFactory` and all reached the create step. This is the route project-specific `UDataAsset`/`UPrimaryDataAsset` subclasses take (Evidence for the Engine/plugin ones listed in the table; **Judgement** that "
  "project classes behave the same)." % sum(1 for r in audited if "DataAssetFactory" in r["factory"]))
w()
CREATE_CAUSES = {
    "FactoryCreate.SubclassUnsupported": "No factory creates the class directly; the nearest New-menu factory creates its base class and picks the subclass only in `ConfigureProperties` (class picker), so the lifecycle refuses rather than create the wrong class",
    "FactoryCreate.MissingFactoryContext": "The exact factory needs what its New-asset dialog asks for (skeleton, mesh, schema, style class, ...); a document names it in `FactoryContext`. The audit retries with an existing registry asset or a picker class (`factory_context` in the JSON); these classes had none available in the sandbox",
    "FactoryCreate.BlueprintClassUnsupported": "Not a standalone asset: an Actor class, or a class whose factory makes a Blueprint of it; created through the Blueprint profile",
    "FactoryCreate.FactoryDisabled": "The class's own factory is switched off in this editor (`CanCreateNew()` false, e.g. an experimental setting); the Content Browser does not offer it either",
    "FactoryCreate.DialogFactoryInitFailed": "The factory's dialog would not offer this class (e.g. a class picker that hides it)",
    "FactoryCreate.IncompleteCreation": "The factory did not create an exact instance of the class (its dialog configures it in a way a document cannot supply)",
    "FactoryCreate.InteractiveFactory": "The factory opens a modal dialog while creating; cancelled and refused (section 3.1)",
    "ClassResolve.Unsupported": "Sidecar v1 explicitly does not support the class (Material, World)",
    "ScratchCreate.InvalidIdentity": "The factory returns a scratch that is not in the transient package (factories that add asset flags to a transient-package scratch are accepted)",
    "DataTable.RowStructNotFound": "Specialized profile: the template's placeholder RowStruct does not exist; a real document names one",
    "MirrorDataTable.CreateUnsupported": "Specialized profile: creating a MirrorDataTable needs a Skeleton and a row struct together; refused (update works)",
    "AnimMontage.InvalidIntent": "Specialized profile: create needs an explicit Skeleton the template does not provide",
    "AnimSequence.SkeletonRequired": "Specialized profile: create needs an explicit Skeleton the template does not provide",
}
w("### 3.4 Admitted by the lifecycle but not created (%d of %d audited, Evidence)" % (len(create_failed), len(audited)))
w()
w("%d of them have no Profile (the generic route's own failures); the other %d are Profile classes whose Profile template has placeholders "
  "(a skeleton, a row struct, a parent material) that a real document fills (`Profile` column)." % (
      len(generic_create_failed), len(create_failed) - len(generic_create_failed)))
w()
w("| Code | Cause | Classes | Count | Profile |")
w("|---|---|---|---|---|")
for code, rs in sorted(create_by_code.items(), key=lambda kv: (-len(kv[1]), kv[0])):
    w("| `%s` | %s | %s | %d | %s |" % (code, md(CREATE_CAUSES.get(code, (rs[0]["create"].get("message") or "")[:150])),
                                       ", ".join(sorted(short(r["class"]) for r in rs)), len(rs),
                                       "yes" if all(short(r["class"]) in SPECIALIZED for r in rs) else
                                       ("no" if not any(short(r["class"]) in SPECIALIZED for r in rs) else "mixed")))
w()

# --------------------------------------------------------------------------------------- summary
w("## 4. Summary (i): generic gaps ranked by classes affected")
w()
w("Counts are classes among the %d audited unless stated. A class can appear under several gaps." % len(audited))
w()
w("| Rank | Gap | Classes | Evidence | Where it bites |")
w("|---|---|---|---|---|")
gaps = []
CAT_REF = "hard UObject reference (direct, in array/set/map, or inside a struct)"
CAT_INST = "instanced subobject (Instanced/EditInlineNew UObject, direct or in a container)"
CAT_TEXT = "FText (direct, in container or in struct)"
CAT_TRANSIENT = "edit-visible but Transient / DuplicateTransient"
CAT_CLASS = "hard class reference (UClass*, TSubclassOf)"


def names(rs, limit=8):
    ns = sorted(short(r["class"]) for r in rs)
    return ", ".join(ns[:limit]) + (", ..." if len(ns) > limit else "")


gaps.append((len(incomplete), "Factory configuration the generic route cannot supply (class picker or context object normally chosen in `ConfigureProperties`), classes without a Profile",
             "%d classes fail create with SubclassUnsupported / MissingFactoryContext / IncompleteCreation (section 3.4): %s" % (len(incomplete), names(incomplete, 12)),
             "`FactoryContext` covers a dialog's context object or picker class when the caller names it"))
gaps.append((len(gap_classes[CAT_REF]), "Hard `UObject*` references dropped from Extract", "%d properties" % gap_props[CAT_REF],
             "Hard references are authorable in general; the remaining drops are listed per class in section 6"))
gaps.append((len(hidden_strong), "Non-property structure is invisible: classes whose real content lives in subobjects/graphs that are not CPF_Edit properties",
             "%d created classes own default inner objects other than AssetImportData (upper bound for editable structure: %d classes have any inner object or non-edit UPROPERTY content; many are editor bookkeeping)" % (len(hidden_strong), len(hidden)),
             "Niagara, SoundCue, MetaSound, Dataflow, Optimus, LevelSequence, IKRig, camera assets"))
gaps.append((len(emit_nothing), "Extract emits no authorable property at all (generic document is empty)",
             "%d created classes emit 0 properties (excluding the specialized ones); the edit step is n/a for them: %s" % (len(emit_nothing), names(emit_nothing)),
             "Container-like or settings-free assets"))
gaps.append((len(noop_float), "Integral float values written by Extract are rejected on re-apply",
             "%d classes fail the unchanged re-apply but pass with `.0` literals" % len(noop_float), "Any class with an integral float value"))
gaps.append((len(gap_classes[CAT_INST]), "Instanced subobjects dropped from Extract", "%d properties" % gap_props[CAT_INST],
             "Instanced subobjects are authorable as {Class, Properties}; the remaining drops are transient or unsupported containers"))
gaps.append((len(gap_classes[CAT_TEXT]), "`FText` dropped from Extract", "%d properties" % gap_props[CAT_TEXT], "Display names and descriptions"))
gaps.append((len(gap_classes[CAT_TRANSIENT]), "Edit-visible properties marked Transient are not authored",
             "%d properties" % gap_props[CAT_TRANSIENT], "Preview-only fields; arguably correct, listed for completeness"))
gaps.append((len(gap_classes[CAT_CLASS]), "Hard class references (`UClass*`, `TSubclassOf`) dropped from Extract",
             "%d properties" % gap_props[CAT_CLASS], "Class-valued settings"))
EDIT_CAUSES = {
    "Generic.PropertyValueNotStoredExactly": "The class's PostEditChange rewrites the synthetic value (clamped, regenerated or normalized); the write is refused with the value the asset would keep",
    "PropertyPreflight.DecodeFailed": "The synthetic value is not valid for the property (wrong class for a class/object picker, null instanced element)",
    "Scratch.UnsupportedSource": "Refused: MetaSound documents cannot be duplicated into a transient preview",
    "PropertyWrite.EditorServiceUnavailable": "Refused: property writes notify the audio device manager, and the audit editor runs without audio",
    "Scratch.CloneFailed": "The transient scratch duplicate failed the identity check",
}
for code, rs in sorted(edit_by_code.items(), key=lambda kv: -len(kv[1])):
    gaps.append((len(rs), "Edit step: `%s`" % code, names(rs), EDIT_CAUSES.get(code, "see section 6")))
gaps.append((len(scratch_fail), "Scratch creation returns an object outside the transient package (`ScratchCreate.InvalidIdentity`)",
             names(scratch_fail), "Factories that build the asset somewhere else"))
gaps.append((len(reload_fail), "Values changed by `PostLoad`/`PostEditChange` differ after reload",
             "; ".join("%s: %s" % (short(r["class"]), (r.get("reload_differences") or [r["reload"]["message"]])[0][:90]) for r in reload_fail), "Evidence only; the class normalizes the value"))
gaps.append((len(set().union(*long_tail.values())) if long_tail else 0, "Struct/array/map properties with a member the codec does not admit (%d distinct kinds)" % len(long_tail),
             "e.g. " + ", ".join(sorted(long_tail)[:6]), "Long tail; each kind affects 1 to 5 classes"))
gaps.append((len(resolve_fail), "Class explicitly unsupported by the sidecar (`ClassResolve.Unsupported`)",
             names(resolve_fail), "Material and World can neither be created nor templated"))
gaps.append((len(crash_rows), "Editor crashes/hangs on create or write (section 3.2)", "%d classes" % len(crash_rows),
             "Robustness gap, not a representation gap"))
gaps = [g for g in gaps if g[0] > 0]
for rank, g in enumerate(sorted(gaps, key=lambda g: -g[0]), 1):
    w("| %d | %s | %d | %s | %s |" % (rank, g[1], g[0], md(g[2]), md(g[3])))
w()
if long_tail:
    w("Long tail of unadmitted kinds (class counts): %s." % ", ".join(
        "%s x%d" % (k, len(v)) for k, v in sorted(long_tail.items(), key=lambda kv: (-len(kv[1]), kv[0]))[:20]))
    w()
w("Rules that explain rows which are **not** gaps: integral floats are accepted (`1` decodes as `1.0` and compares numerically); a float "
  "property stores the nearest float32 value and preflight compares in that precision, so `0.51` is accepted; hard object references, "
  "class references, instanced subobjects and `FText` are authorable (value rules in `MCP/schemas/assetdocument/Generic.md`).")
w()
derived = sum(r["properties"].get("editor_derived_not_authorable", 0) for r in created)
w("Editor-derived references: a plain hard reference (not instanced, no `EditInline` or `ShowInnerProperties` metadata) that holds an object "
  "inside the edited asset was created by the asset's own code, and the Details panel asset picker cannot select it. Extract omits such "
  "properties, Inspect lists them as `editor-derived-reference`, and a document may not name such an object (`Property.EditorDerivedReference`). "
  "This run counts %d such properties (`editor_derived_not_authorable`), e.g. `PaperSprite.BodySetup` and the `Settings` default subobject "
  "of the audio-analysis NRT assets." % derived)
w()
w("## 5. Summary (ii): asset types that need specialized profiles")
w()
w("**Judgement.** Ranked by (how common the type is in typical UE projects) x (how much of its human-edited content the generic route misses). "
  "\"Evidence\" lists what this audit measured for the same class; commonness is the author's estimate, not data from this run. "
  "Classes with a Profile at the audited commit (from the MCP Profile sections): %s (Blueprint-based ones such as Blueprint, WidgetBlueprint, AnimBlueprint "
  "and ControlRigBlueprint are refused by the generic lifecycle and so are not in the audited set); "
  "a row whose classes all have one, or that another tool covers, is marked done." % ", ".join(sorted(SPECIALIZED)))
w()


def ev(name):
    r = by_name.get(name)
    if not r and name in SPECIALIZED:
        return "specialized profile (a Blueprint-based asset: not in the generic route's set)"
    if not r:
        return "not in the creatable set (no factory in the New menu or refused)"
    if "skipped" in r:
        return "skipped: " + r["skipped"][:60]
    if name in OTHER_TOOLS:
        return "covered by " + OTHER_TOOLS[name].split(" (")[0]
    if name in SPECIALIZED:
        return "specialized profile"
    if r["create"]["status"] != "pass":
        return "create FAILS (%s)" % (", ".join(codes(r["create"])[:1]) or r["create"]["message"][:60])
    p = r["properties"]
    return "creates; %d of %d Details-visible properties emitted, %d dropped; inner objects %s" % (
        p["emitted"], p["details_visible"], p["dropped"],
        ", ".join("%s x%d" % kv for kv in list(p.get("default_inner_objects", {}).items())[:3]) or "none")


SHORTLIST = [
    ("Material (expression graph) + Material Function / Layer / Blend",
     ["Material", "MaterialFunction", "MaterialFunctionMaterialLayer", "MaterialFunctionMaterialLayerBlend"],
     "Used in every project with custom visuals; the whole asset is a node graph. The generic route refuses Material (ClassResolve.Unsupported); the Material profile authors the graph."),
    ("DataTable family (DataTable, CompositeDataTable, MirrorDataTable, CurveTable, StringTable)",
     ["DataTable", "CompositeDataTable", "CurveTable", "StringTable"],
     "Ubiquitous in gameplay projects; the content is rows, which are not properties, and create needs a row struct."),
    ("Material Instance (Constant)",
     ["MaterialInstanceConstant"],
     "Most-created material asset in production; parameter overrides are authored by parameter name. Material Function Instances stay on the generic route."),
    ("Niagara System / Emitter / Script / Parameter Collection",
     ["NiagaraSystem", "NiagaraEmitter", "NiagaraScript", "NiagaraParameterCollection"],
     "Standard VFX path; emitters, module stacks and scripts live in subobject graphs."),
    ("StateTree",
     ["StateTree"],
     "Growing replacement for behavior logic; the state hierarchy is the content."),
    ("Sound graphs: SoundCue, MetaSound Source/Patch",
     ["SoundCue", "MetaSoundSource", "MetaSoundPatch"],
     "Every shipped game has audio; SoundCue and MetaSound are node graphs."),
    ("Sound settings: Sound Class / Mix / Submix / Attenuation",
     ["SoundClass", "SoundMix", "SoundSubmix", "SoundAttenuation"],
     "Class/submix hierarchies are edited as trees (parent/child links on both sides); their properties edit through the generic route."),
    ("Enhanced Input: InputAction + InputMappingContext",
     ["InputAction", "InputMappingContext"],
     "Default input stack of UE5; triggers, modifiers and mapping lists are authorable as instanced subobjects and structs, so the generic route covers them; a profile would only add convenience."),
    ("Level Sequence / Template Sequence / Camera animation (Sequencer)",
     ["LevelSequence", "TemplateSequence", "CameraAnimationSequence"],
     "Cinematics and gameplay camera work; bindings, tracks and keys are all inside MovieScene subobjects."),
    ("User Defined Struct / Enum",
     ["UserDefinedStruct", "UserDefinedEnum"],
     "Very common in Blueprint-heavy projects and a prerequisite for DataTables; members and enumerators are editor data objects."),
    ("Animation asset family: BlendSpace, AimOffset, AnimComposite, PoseAsset",
     ["BlendSpace", "BlendSpace1D", "AimOffsetBlendSpace", "AimOffsetBlendSpace1D", "AnimComposite", "PoseAsset"],
     "Next to AnimMontage/AnimSequence in daily animation work; sample lists are the content."),
    ("PhysicsAsset / Skeleton",
     ["PhysicsAsset", "Skeleton"],
     "Bodies, constraints, bones, sockets and virtual bones are structure rather than properties."),
    ("Skeletal Mesh structure",
     ["SkeletalMesh"],
     "Common but imported; LOD and material slots edit as properties. Create from a static mesh needs `FactoryContext`."),
    ("IK Rig / IK Retargeter / Control Rig",
     ["IKRigDefinition", "IKRetargeter", "ControlRigBlueprint"],
     "Animation retargeting and rigging; goals, solver stack, chain mappings and the rig graph are hidden structure."),
    ("World / Level",
     ["World"],
     "Universal but a different problem (actor and level editing): the generic route refuses World (ClassResolve.Unsupported)."),
]


def covered_by(names_):
    """'profile' when every class has a Profile, the tool text when another tool covers the row, else None."""
    if all(n in SPECIALIZED for n in names_):
        return "specialized profile (%s)" % ", ".join(sorted({PROFILES[n] for n in names_ if n in PROFILES})) if any(
            n in PROFILES for n in names_) else "specialized profile"
    if all(n in OTHER_TOOLS for n in names_):
        return OTHER_TOOLS[names_[0]]
    return None


w("| Rank | Asset type | Evidence from this audit | Why (judgement) |")
w("|---|---|---|---|")
for rank, (title, names_, why) in enumerate(SHORTLIST, 1):
    done = covered_by(names_)
    w("| %d | %s | %s | %s |" % (rank, md(title) + (" (**done**: %s)" % done if done else ""),
                                 md("; ".join("%s: %s" % (n, ev(n)) for n in names_)), md(why)))
w()
w("Cheaper wins that are **not** new profiles (generic-route strengthening, from section 4): factory configuration for the %d classes "
  "whose factory needs a class picker or a dialog-chosen context object, and the remaining scratch-identity failures (%d classes)." % (
      len(incomplete), len(scratch_fail)))
w()

# --------------------------------------------------------------------------------------- table
w("## 6. Per-class results")
w()
w("Legend: ok = passed, FAIL = failed, partial = some of the edited properties failed, `-` = not applicable or not reached (earlier step failed, "
  "or no authorable property). No-op = unchanged re-apply returns `bSavedAsset == false` (the `.0` mark means it only passes with `.0` float literals). "
  "Dropped = coarse kinds of Details-visible properties that Extract does not emit (count of properties). Non-property structure is "
  "**Judgement** (curated); where there is no curated entry and the audit found inner objects or non-edit UPROPERTYs these are listed as `observed:`.")
w()
w("| Class | Module (plugin) | Specialized? | Create | Extract | Edit | Reload | No-op | Dropped property kinds | Non-property structure humans edit | Notes |")
w("|---|---|---|---|---|---|---|---|---|---|---|")


def notes_for(r):
    n = []
    if "skipped" in r:
        return md(r["skipped"][:200])
    if r["create"]["status"] != "pass":
        msg = r["create"]["message"].split("(at ")[0].split(" | ")[0]
        first = (codes(r["create"]) or [""])[0]
        if first and msg.startswith(first + ": "):
            msg = msg[len(first) + 2:]
        n.append(md((first + ": " + msg)[:170]))
        return "; ".join(n)
    if r["template"]["status"] != "pass":
        n.append("template: " + md(r["template"]["message"][:80]))
    if r["edit"]["status"] in ("fail", "partial"):
        n.append("edit: " + md((", ".join(codes(r["edit"])[:1]) + " " + (r["edit"]["message"].split(" (at ")[-1].split(")")[0] if " (at " in r["edit"]["message"] else ""))[:100]))
    if r["reload"]["status"] == "fail":
        n.append("reload: " + md(r["reload"]["message"][:100]))
    if r["noop_diff"]["status"] == "fail":
        if r.get("noop_full_ok_with_float_literals"):
            n.append("no-op fails on integral float (%s); passes with .0 literals" % ", ".join(f["name"] for f in r.get("noop_failing_properties", [])[:3]))
        else:
            n.append("no-op: " + md(", ".join(codes(r["noop_diff"]))))
    if r.get("cleanup_ok") is False:
        n.append("cleanup failed")
    if short(r["class"]) in SPECIALIZED:
        n.append("specialized profile; synthetic edit skipped")
    if not r["factory_exact"]:
        n.append("via ancestor factory %s" % short(r["factory"]))
    return "; ".join(n)


def drop_cell(r):
    if "skipped" in r or r["create"]["status"] != "pass":
        return "-"
    kinds = r["properties"].get("dropped_by_kind", {})
    return ", ".join("%s x%d" % kv for kv in sorted(kinds.items(), key=lambda kv: -kv[1])) or "none"


def struct_cell(r):
    name = short(r["class"])
    if name in STRUCTURE and STRUCTURE[name]:
        return md(STRUCTURE[name])
    p = r.get("properties") or {}
    seen = ["%s" % f["name"] for f in p.get("hidden_structure_candidates", [])[:3]]
    inner = [k for k in p.get("default_inner_objects", {}) if k not in TRIVIAL_INNER][:2]
    if seen or inner:
        return md("observed: " + ", ".join(seen + inner))
    return "none identified"


def noop_cell(r):
    if "skipped" in r or r["create"]["status"] != "pass":
        return "-"
    base = cell(r["noop_diff"])
    if r["noop_diff"]["status"] == "fail" and r.get("noop_full_ok_with_float_literals"):
        return "FAIL (.0 fixes)"
    return base


for r in sorted(rows, key=lambda r: short(r["class"]).lower()):
    name = short(r["class"])
    if "skipped" in r:
        cells = ["skip", "-", "-", "-", "-"]
    else:
        cells = [cell(r["create"]), cell(r["extract"]), cell(r["edit"]), cell(r["reload"]), noop_cell(r)]
    spec = "yes" if name in SPECIALIZED else "no"
    w("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
        name, md(plugin_of(r["module"])), spec, *cells, md(drop_cell(r)), struct_cell(r), notes_for(r)))
w()
w("## 7. Reproducing")
w()
w("```")
w("# Build the plugin into a project, then run the automation test in an editor process of its own")
w("# (add ASSETDOC_COVERAGE_RESUME=1 to the environment to continue an interrupted run from results.jsonl):")
w("<UE_ROOT>/Engine/Binaries/<Platform>/UnrealEditor-Cmd <Project>.uproject -nosplash -NoSound -unattended -NoPause \\")
w("  \"-ExecCmds=Automation RunTests UEAssetDoc.Coverage.GenericRouteAudit;Quit\" \"-TestExit=Automation Test Queue Empty\" -log")
w("")
w("export ENGINE_DIR=<UE_ROOT>/Engine")
w("python3 Scripts/AssetDocument/coverage_audit/scan_modal_factories.py <Project>/Saved/AssetDocumentCoverage/coverage-audit.json > modal-scan.json")
w("python3 Scripts/AssetDocument/coverage_audit/build_report.py <coverage-audit.json> modal-scan.json docs/architecture/asset-document-coverage-audit.md [<previous coverage-audit.json>]")
w("```")
w()
w("The automation test itself (`UEAssetDoc.Coverage.GenericRouteAudit`) asserts only that it ran; it writes "
  "`Saved/AssetDocumentCoverage/{coverage-audit.json,results.jsonl,current.txt,extracts/*.json}` in the project. "
  "Never run it in the same editor process as the rest of the AssetDocument suite: leave `Coverage` out of the default test list and run it explicitly with the automation filter `UEAssetDoc.Coverage`.")

if previous:
    w()
    w("## 8. Changes since the previous run (Evidence)")
    w()
    prev_rows = {short(r["class"]): r for r in previous["classes"]}
    w("Previous run %s (%d creatable, %d refused); this run %s (%d creatable, %d refused)." % (
        previous["generated_utc"], previous["creatable_class_count"], len(previous["refused_classes"]),
        audit["generated_utc"], audit["creatable_class_count"], len(refused)))
    w()

    def status_of(r, step):
        if r is None:
            return "absent"
        if "skipped" in r:
            return "skip"
        st = (r.get(step) or {}).get("status")
        return "pass" if st == "pass" else ("fail" if st in ("fail", "partial") else "-")

    w("| Step | pass before | pass now | newly passing | newly failing |")
    w("|---|---|---|---|---|")
    for step in ["create", "edit", "reload", "noop_diff"]:
        common = sorted(set(prev_rows) & set(by_name))
        up = [n for n in common if status_of(prev_rows[n], step) != "pass" and status_of(by_name[n], step) == "pass"]
        down = [n for n in common if status_of(by_name[n], step) == "fail" and status_of(prev_rows[n], step) != "fail"]
        before = sum(1 for r in prev_rows.values() if status_of(r, step) == "pass")
        now = sum(1 for r in by_name.values() if status_of(r, step) == "pass")
        w("| %s | %d | %d | %d | %s |" % (labels.get(step, step), before, now, len(up), ", ".join(down) or "none"))
    w()
    prev_created = [r for r in previous["classes"] if "skipped" not in r and r["create"]["status"] == "pass"]
    w("Properties over created classes: emitted %d -> %d, dropped %d -> %d. Classes only in this run: %s. Classes only in the previous run: %s." % (
        sum(r["properties"]["emitted"] for r in prev_created), props_emit,
        sum(r["properties"]["dropped"] for r in prev_created), props_drop,
        ", ".join(sorted(set(by_name) - set(prev_rows))) or "none", ", ".join(sorted(set(prev_rows) - set(by_name))) or "none"))
    w()
    w("A newly failing step is usually a class whose create or extract now passes, so the audit reaches a step it skipped before; "
      "section 6 has the per-class reason.")

if base_run:
    w()
    w("### Change in this run's commit (Evidence)")
    w()
    w("The same audit re-run at the commit this change started from (%s), then after it (%s); counts over the audited classes "
      "without a Profile (the same Profile set for both runs):" % (base_run["generated_utc"], audit["generated_utc"]))
    w()
    before_h, after_h = generic_headline(base_run), generic_headline(audit)
    w("| Measure | before | after |")
    w("|---|---|---|")
    for key in before_h:
        w("| %s | %d | %d |" % (key, before_h[key], after_h[key]))
    w()
    base_rows = {short(r["class"]): r for r in base_run["classes"]}

    base_refused = {short(r["class"]): r["code"] for r in base_run["refused_classes"]}
    now_refused = {short(r["class"]): r["code"] for r in refused}

    def create_code(r, name, refused_codes):
        if name in refused_codes:
            return "refused " + refused_codes[name]
        if r is None or "skipped" in r:
            return "absent"
        return "pass" if r["create"]["status"] == "pass" else ((codes(r["create"]) or ["fail"])[0])

    moved = []
    for n in sorted(set(base_rows) | set(by_name)):
        was = create_code(base_rows.get(n), n, base_refused)
        now = create_code(by_name.get(n), n, now_refused)
        if n not in SPECIALIZED and was != now:
            moved.append((n, was, now))
    if moved:
        w("Create outcome changes (classes without a Profile): %s." % "; ".join("%s %s -> %s" % m for m in moved))
        w()

open(out_path, "w").write("\n".join(W) + "\n")
print("wrote", out_path, len(W), "lines")
