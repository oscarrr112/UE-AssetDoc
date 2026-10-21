"""Phase-local native operations used by the exact BehaviorTree Profile."""
from __future__ import annotations

import re

from asset_document.diagnostics import AssetDocumentContractError, DiagnosticContext, RuntimeDiagnostic, contract_error
from asset_document.dto import PrimitiveRequirement, RuntimePhase
from asset_document.regions.primitive_catalog import (
    NamedOpaqueHandleArgument, PrimitiveOutcomeStatus, _invoke_required,
    _optional_handle, _single_handle,
)
from asset_document.regions.property_semantics import PropertySemanticAdapter


CLASS_PATH = "/Script/AIModule.BehaviorTree"
ROUTE_ID = "ai_profile"
READBACK_ID = "ai.behavior_tree.authored_graph.readback.v2"
SELECTOR_REQUIREMENT = PrimitiveRequirement(
    "ai.behavior_tree.selector.resolve_batch.v2", 2, ROUTE_ID)
REFRESH_REQUIREMENT = PrimitiveRequirement(
    "ai.behavior_tree.value_or_key.refresh.v3", 3, ROUTE_ID)


def _native_field_order(name):
    # Stock GenericPlatformStricmp folds ASCII A-Z only and compares TCHAR
    # UTF-16 code units. Unicode upper/casefold would change authored keys.
    folded = "".join(chr(ord(char) + 32) if "A" <= char <= "Z" else char for char in name)
    return folded.encode("utf-16-be")


def _canonical_native_value(value):
    # The native DTO preserves Python insertion order; AI contracts require
    # canonical object ordering at every depth, including authored properties.
    if type(value) is dict:
        return {key: _canonical_native_value(value[key]) for key in sorted(value, key=_native_field_order)}
    if type(value) is list:
        return [_canonical_native_value(item) for item in value]
    return value


def invoke(context, requirement, values, handles=()):
    outcome = context.invoke(requirement=requirement, value_arguments=_canonical_native_value(values),
                             handle_arguments=handles)
    if outcome.status is not PrimitiveOutcomeStatus.SUCCEEDED:
        if outcome.diagnostics:
            raise AssetDocumentContractError(outcome.diagnostics[0])
        raise contract_error("/PrimitiveOutcome", "BehaviorTree.InvalidPrimitiveResult",
                             "A failed native operation omitted its diagnostic.")
    return outcome


def object_path(target):
    """Canonical document targets are package paths; native identity is object path."""
    return target + "." + target.rsplit("/", 1)[1]


def resolve(context, target):
    outcome = _invoke_required(context, "editor.asset.resolve_existing.v1",
                               values={"path": object_path(target), "expected_class": CLASS_PATH})
    if type(outcome.value) is not dict or type(outcome.value.get("exists")) is not bool:
        raise contract_error("/Target", "BehaviorTree.InvalidPrimitiveResult",
                             "Target resolution omitted exact existence evidence.")
    read = _optional_handle(outcome, "asset_read")
    # Verify intentionally has no mutable handle lane.
    mutable = (None if context.phase is RuntimePhase.VERIFY else
               _optional_handle(outcome, "asset_mutable"))
    exists = outcome.value["exists"]
    if exists != (read is not None) or (context.phase is not RuntimePhase.VERIFY
                                      and exists != (mutable is not None)):
        raise contract_error("/Target", "BehaviorTree.InvalidPrimitiveResult",
                             "Target identity handles disagree with existence.")
    return exists, read, mutable


def ai_asset(context, handle, *, mutable=False):
    return context.handoff(source_handle=handle, target_provider_id="ai",
                           access="mutable" if mutable else "read_only")


def _public_graph_readback_diagnostic(diagnostic):
    if (diagnostic.code != "AIProvider.BehaviorTree.GraphReadbackTopologyRejected"
            or diagnostic.path != "/Primitive"):
        return diagnostic
    evidence = [item.value for item in diagnostic.context if item.key == "graph_validation"]
    if len(evidence) != 1 or type(evidence[0]) is not dict or set(evidence[0]) != {"fault", "path"}:
        return diagnostic
    fault, path = evidence[0]["fault"], evidence[0]["path"]
    if type(fault) is not str or type(path) is not str:
        return diagnostic
    codes = {
        "multiple_roots": "MultipleBehaviorTreeRoots",
        "unreachable": "UnreachableBehaviorTreeGraphNode",
        "cycle": "BehaviorTreeGraphCycle",
        "invalid_connection": "InvalidBehaviorTreeGraphConnection",
    }
    if fault not in codes:
        return diagnostic
    if fault == "multiple_roots":
        valid_path = path == "/Body/Tree/Root"
    elif fault == "unreachable":
        valid_path = path == "/Body/Tree/Nodes"
    else:
        valid_path = re.fullmatch(r"/Body/Tree/Root(?:/Children/(?:0|[1-9][0-9]*)){0,64}", path) is not None
    if not valid_path:
        return diagnostic
    return RuntimeDiagnostic(path, codes[fault], diagnostic.message, diagnostic.severity, diagnostic.context)


def readback(context, target, editor_read):
    asset = ai_asset(context, editor_read)
    try:
        return invoke(context, PrimitiveRequirement(READBACK_ID, 2, ROUTE_ID),
                      {"behavior_tree_path": object_path(target)},
                      (NamedOpaqueHandleArgument("asset", asset),)).value
    except AssetDocumentContractError as error:
        diagnostic = _public_graph_readback_diagnostic(error.diagnostic)
        if diagnostic is not error.diagnostic:
            raise AssetDocumentContractError(diagnostic) from error
        raise


def create_target(context, target):
    if context.phase is not RuntimePhase.APPLY:
        raise contract_error("/Phase", "BehaviorTree.InvalidMutationPhase",
                             "Factory creation requires Apply.")
    resolved = _invoke_required(context, "editor.class.resolve_canonical.v1",
                               values={"class": CLASS_PATH, "expected_base": None})
    package, name = target.rsplit("/", 1)
    created = _invoke_required(context, "editor.asset.create_by_factory.v1",
                              values={"factory_id": "behavior_tree_factory.v1",
                                      "package_path": package, "asset_name": name},
                              handles=(NamedOpaqueHandleArgument(
                                  "asset_class", _single_handle(resolved, "class")),))
    # Factory result leases are lifecycle payloads, not ordinary AI asset leases.
    exists, read, mutable = resolve(context, target)
    if not exists:
        raise contract_error("/Target", "BehaviorTree.CreatedTargetMissing",
                             "Created target must resolve before AI handoff.")
    return read, mutable, created.changed_set


def apply_properties(context, document, read, mutable):
    properties = document.get("Properties", {})
    if not properties:
        return ()
    adapter = PropertySemanticAdapter()
    admitted = adapter.enumerate(CLASS_PATH, context)
    return adapter.apply(read, mutable, properties, admitted, context, phase=RuntimePhase.APPLY)


def extract_properties(context, read, *, include_defaults=False):
    adapter = PropertySemanticAdapter()
    admitted = adapter.enumerate(CLASS_PATH, context)
    return adapter.extract(read, admitted, context, include_defaults=include_defaults)


def header(context, target, editor_read):
    return invoke(context, PrimitiveRequirement(
        "ai.behavior_tree.blackboard.readback.v1", 1, ROUTE_ID),
        {"behavior_tree_path": object_path(target)},
        (NamedOpaqueHandleArgument("asset", ai_asset(context, editor_read)),)
    ).value["blackboard_path_or_null"]


def class_evidence(context, class_path, *, expected_base="/Script/AIModule.BTNode",
                   struct_paths=()):
    # Repair/Verify inspect classes already present in the actual loaded tree.
    # The loading class resolver is only admitted in Validate/Preflight/Apply.
    try:
        if context.phase in (RuntimePhase.VALIDATE, RuntimePhase.PREFLIGHT, RuntimePhase.APPLY):
            _invoke_required(context, "editor.class.resolve_canonical.v1",
                             values={"class": class_path, "expected_base": expected_base})
        return invoke(context, PrimitiveRequirement(
            "ai.behavior_tree.node_class.inspect.v2", 2, ROUTE_ID),
            {"node_class_path": class_path, "expected_base_class_path": expected_base,
             "struct_type_paths": sorted(set(struct_paths))}).value
    except AssetDocumentContractError as error:
        codes = {"ClassReference.NonCanonicalClass": "InvalidBehaviorTreeNodeClass",
                 "ClassReference.ExpectedBaseMismatch": "InvalidBehaviorTreeNodeClass",
                 "AIProvider.BehaviorTree.NodeClassUnavailable": "InvalidBehaviorTreeNodeClass",
                 "AIProvider.BehaviorTree.NodeClassRejected": "InvalidBehaviorTreeNodeClass",
                 "AIProvider.BehaviorTree.NodeClassAbstract": "AbstractBehaviorTreeNodeClass",
                 "AIProvider.BehaviorTree.NodeClassDeprecated": "DeprecatedBehaviorTreeNodeClass"}
        if error.diagnostic.code in codes:
            raise contract_error("/Class", codes[error.diagnostic.code], error.diagnostic.message) from error
        raise


def validate_properties(context, document):
    properties = document.get("Properties", {})
    owners = {
        "BlackboardAsset": "Body.BlackboardAsset",
        "BTGraph": "Body.Tree",
        "RootNode": "Body.Tree",
        "RootDecorators": "Body.Tree",
        "RootDecoratorOps": "Body.Tree",
    }
    if type(properties) is dict:
        for name in sorted(properties):
            owner = next((path for managed, path in owners.items()
                          if _native_field_order(name) == _native_field_order(managed)), None)
            if owner is not None:
                raise contract_error(f"/Properties/{name}", "BodyOwnedProperty",
                                     f"Property '{name}' is owned by {owner} and cannot be authored through top-level Properties")
    adapter = PropertySemanticAdapter()
    adapter.validate(properties, adapter.enumerate(CLASS_PATH, context))


def validate_reference(context, constraint, path, declared, wire_path):
    if context.phase not in (RuntimePhase.VALIDATE, RuntimePhase.PREFLIGHT):
        return
    try:
        if constraint == "represented_class":
            _invoke_required(context, "editor.class.resolve_canonical.v1",
                             values={"class": path, "expected_base": declared})
        else:
            _invoke_required(context, "editor.reference.resolve_asset.v1",
                             values={"expected_class": declared, "path": path})
    except AssetDocumentContractError as error:
        if error.diagnostic.code in ("ClassReference.NonCanonicalClass", "ClassReference.ExpectedBaseMismatch",
                                     "ClassReference.NonCanonicalAsset", "ClassReference.AssetTypeMismatch"):
            raise contract_error(wire_path, "InvalidPropertyValue", error.diagnostic.message) from error
        raise


def resolve_binding_dependencies(context, blackboard, tree):
    if context.phase not in (RuntimePhase.VALIDATE, RuntimePhase.PREFLIGHT):
        return
    # Reuse the existing readonly loaders before the AI leaf inspects exact
    # loaded identities. This does not construct a preview node or graph.
    seen = set()
    def asset(path, expected=None):
        if path not in seen:
            seen.add(path)
            _invoke_required(context, "editor.reference.resolve_asset.v1",
                             values={"expected_class": expected, "path": path})
    def scan(value):
        if type(value) is dict:
            if set(value) == {"Kind", "Path"}:
                if value["Kind"] == "AssetRef":
                    asset(value["Path"])
                elif value["Kind"] == "ClassRef" and value["Path"] not in seen:
                    seen.add(value["Path"])
                    _invoke_required(context, "editor.class.resolve_canonical.v1",
                                     values={"class": value["Path"], "expected_base": None})
            for child in value.values():
                scan(child)
        elif type(value) is list:
            for child in value:
                scan(child)
    if blackboard is not None:
        try:
            asset(blackboard, "/Script/AIModule.BlackboardData")
        except AssetDocumentContractError as error:
            if not error.diagnostic.code.startswith("ClassReference."):
                raise
            # Name the asset the document points at, not the primitive argument.
            raise AssetDocumentContractError(RuntimeDiagnostic(
                path="/Body/BlackboardAsset", code="BehaviorTree.BlackboardNotFound",
                message=f"BlackboardAsset '{blackboard}' is not an existing BlackboardData asset "
                        f"({error.diagnostic.message}) Create the Blackboard first, or use find_assets "
                        "(class /Script/AIModule.BlackboardData) to list existing ones.",
                context=(DiagnosticContext("primitive_code", error.diagnostic.code),) + error.diagnostic.context,
            )) from None
    scan(tree)


def selector_inventory(context, target, editor_read):
    return invoke(context, PrimitiveRequirement(
        "ai.behavior_tree.selector.inventory.v1", 1, ROUTE_ID),
        {"behavior_tree_path": object_path(target)},
        (NamedOpaqueHandleArgument("asset", ai_asset(context, editor_read)),)
    ).value["selectors"]
