# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Unregistered BehaviorTree Profile candidate for A-BT."""

import re

from asset_document.canonical.document_codec import DocumentCodec
from asset_document.diagnostics import AssetDocumentContractError, contract_error
from asset_document.diff.canonical_diff import canonical_diff
from asset_document.dto import PrimitiveRequirement, RuntimePhase
from asset_document.meta import DocumentAction
from asset_document.regions.primitive_catalog import (
    GenericPrimitiveContext,
    NamedOpaqueHandleArgument,
    OpaqueHandleProxy,
    PrimitiveOutcome,
    PrimitiveOutcomeStatus,
)
from asset_document.read_only_extract import admits_read_only


BEHAVIOR_TREE_CLASS = "/Script/AIModule.BehaviorTree"
BEHAVIOR_TREE_ROUTE_ID = "ai_profile"
CANONICAL_BODY_KEYS = ("BlackboardAsset", "Tree")
REQUIRED_PRIMITIVES = (
    PrimitiveRequirement("ai.behavior_tree.graph.create.v1", 1, BEHAVIOR_TREE_ROUTE_ID),
    PrimitiveRequirement("ai.behavior_tree.graph.create_defaults.v1", 1, BEHAVIOR_TREE_ROUTE_ID),
    PrimitiveRequirement("ai.behavior_tree.node.create_action.v1", 1, BEHAVIOR_TREE_ROUTE_ID),
    PrimitiveRequirement("ai.behavior_tree.subnode.create_action.v1", 1, BEHAVIOR_TREE_ROUTE_ID),
    PrimitiveRequirement("ai.behavior_tree.comment.create_action.v1", 1, BEHAVIOR_TREE_ROUTE_ID),
    PrimitiveRequirement("ai.behavior_tree.graph.connect.v1", 1, BEHAVIOR_TREE_ROUTE_ID),
    PrimitiveRequirement("ai.behavior_tree.blackboard.set.v1", 1, BEHAVIOR_TREE_ROUTE_ID),
    PrimitiveRequirement("ai.behavior_tree.runtime_mirror.rebuild.v1", 1, BEHAVIOR_TREE_ROUTE_ID),
)
_REQUIREMENT_BY_ID = {requirement.primitive_id: requirement for requirement in REQUIRED_PRIMITIVES}
_TREE_BLOCKERS = {
    "InstallGraph": "AD.BLOCK.AI.GRAPH_INSTALL_AUTHORITY",
    "EnsureDecoratorInputCount": "AD.BLOCK.AI.DECORATOR_INPUT_AUTHORITY",
    "RefreshValueOrKey": "AD.BLOCK.AI.VALUE_OR_KEY_REFRESH_AUTHORITY",
    "RefreshDecorators": "AD.BLOCK.AI.DECORATOR_REFRESH_AUTHORITY",
}
_TREE_KEYS = frozenset(("Nodes", "Comments", *_TREE_BLOCKERS))
_NODE_KEYS = frozenset(("Id", "Kind", "Class", "Position", "Children", "Decorators", "Services"))
_SUBNODE_KEYS = frozenset(("Id", "Kind", "Class"))
_COMMENT_KEYS = frozenset(("Id", "Bounds", "Text"))


def _snapshot(value: object):
    codec = DocumentCodec()
    return codec.decode(codec.encode(value))


def _require_target(value: object) -> str:
    if admits_read_only(value):
        return value
    if type(value) is not str or re.fullmatch(r"/Game/[A-Za-z0-9_/-]+", value) is None:
        raise contract_error(
            "/Target",
            "BehaviorTree.NonCanonicalTarget",
            "Target must be a canonical /Game package path",
        )
    return value


def _require_nonempty_string(value: object, path: str, code: str) -> str:
    if type(value) is not str or not value:
        raise contract_error(path, code, "value must be a non-empty exact string")
    return value


class BehaviorTreeProfileCandidate:
    """Canonical A-BT semantics that remain unregistered from production dispatch."""

    def inspect_profile(self) -> dict:
        return {"Class": BEHAVIOR_TREE_CLASS, "BodySections": list(CANONICAL_BODY_KEYS)}

    def required_primitives(self) -> tuple[PrimitiveRequirement, ...]:
        return REQUIRED_PRIMITIVES

    def create_template(self, target: str) -> dict:
        return _snapshot(
            {
                "SchemaVersion": 1,
                "Target": _require_target(target),
                "Class": BEHAVIOR_TREE_CLASS,
                "Action": DocumentAction.CREATE_OR_UPDATE.value,
                "Definitions": {},
                "Properties": {},
                "Body": {"BlackboardAsset": None, "Tree": {"Nodes": [], "Comments": []}},
            }
        )

    def validate(self, document: object) -> None:
        if type(document) is not dict:
            raise contract_error("/", "BehaviorTree.InvalidDocumentType", "document must be an exact object")
        self._validate_header(document)
        body = document.get("Body")
        if type(body) is not dict:
            raise contract_error("/Body", "BehaviorTree.InvalidBodyType", "Body must be an exact object")
        for key in body:
            if key not in CANONICAL_BODY_KEYS:
                raise contract_error(f"/Body/{key}", "BehaviorTree.UnknownBodyKey", "Body key is not canonical")
        if "BlackboardAsset" in body:
            blackboard = body["BlackboardAsset"]
            if blackboard is not None:
                _require_target(blackboard)
        if "Tree" in body:
            self._validate_tree(body["Tree"])
        DocumentCodec().validate(document)

    def canonicalize(self, document: object) -> dict:
        self.validate(document)
        return _snapshot(document)

    def diff(self, authored: object, extracted: object) -> tuple:
        return canonical_diff(self.canonicalize(authored), self.canonicalize(extracted))

    def apply(
        self,
        document: object,
        *,
        context: GenericPrimitiveContext,
        asset_handle: OpaqueHandleProxy,
    ) -> None:
        canonical = self.canonicalize(document)
        body = canonical["Body"]
        tree = body.get("Tree", {"Nodes": [], "Comments": []})
        self._reject_requested_blocker(tree)
        target = canonical["Target"]
        if "BlackboardAsset" in body:
            self._invoke(
                context,
                "ai.behavior_tree.blackboard.set.v1",
                {"behavior_tree_path": target, "blackboard_path_or_null": body["BlackboardAsset"]},
                (NamedOpaqueHandleArgument("asset", asset_handle),),
            )
        graph = self._one_handle(
            self._invoke(
                context,
                "ai.behavior_tree.graph.create.v1",
                {"behavior_tree_path": target, "graph_name": "A-BT-Candidate"},
                (),
            ),
            "graph",
        )
        self._one_handle(
            self._invoke(
                context,
                "ai.behavior_tree.graph.create_defaults.v1",
                {},
                (NamedOpaqueHandleArgument("graph", graph),),
            ),
            "root_node",
        )
        nodes = self._create_nodes(context, tree["Nodes"], graph)
        self._create_comments(context, tree["Comments"], graph)
        self._connect_children(context, tree["Nodes"], nodes, graph)

    def rebuild_runtime_mirror(
        self,
        document: object,
        *,
        context: GenericPrimitiveContext,
        asset_handle: OpaqueHandleProxy,
        live_graph_handle: OpaqueHandleProxy,
    ) -> None:
        canonical = self.canonicalize(document)
        if context.phase is not RuntimePhase.REPAIR:
            raise contract_error(
                "/Phase",
                "BehaviorTree.InvalidRuntimeMirrorPhase",
                "runtime-mirror rebuild requires the bound REPAIR context",
            )
        self._invoke(
            context,
            "ai.behavior_tree.runtime_mirror.rebuild.v1",
            {"behavior_tree_path": canonical["Target"], "update_flags": ["KeepRebuildCounter"]},
            (
                NamedOpaqueHandleArgument("asset", asset_handle),
                NamedOpaqueHandleArgument("graph", live_graph_handle),
            ),
        )

    @staticmethod
    def _validate_header(document: dict) -> None:
        if type(document.get("SchemaVersion")) is not int or document["SchemaVersion"] != 1:
            raise contract_error("/SchemaVersion", "Meta.InvalidSchemaVersion", "SchemaVersion must be exact integer 1")
        _require_target(document.get("Target"))
        if document.get("Class") != BEHAVIOR_TREE_CLASS:
            raise contract_error("/Class", "BehaviorTree.ClassMismatch", "Class must be BehaviorTree")
        if document.get("Action") not in {item.value for item in DocumentAction}:
            raise contract_error("/Action", "BehaviorTree.InvalidAction", "Action is invalid")
        for key in ("Definitions", "Properties"):
            if type(document.get(key, {})) is not dict:
                raise contract_error(f"/{key}", f"BehaviorTree.Invalid{key}Type", f"{key} must be an object")

    @staticmethod
    def _validate_tree(tree: object) -> None:
        if type(tree) is not dict:
            raise contract_error("/Body/Tree", "BehaviorTree.InvalidTreeType", "Tree must be an exact object")
        for key in tree:
            if key not in _TREE_KEYS:
                raise contract_error(f"/Body/Tree/{key}", "BehaviorTree.UnknownTreeKey", "Tree key is not canonical")
        for flag in _TREE_BLOCKERS:
            if flag in tree and type(tree[flag]) is not bool:
                raise contract_error(f"/Body/Tree/{flag}", "BehaviorTree.InvalidBlockerRequest", "blocker request must be boolean")
        nodes = tree.get("Nodes")
        comments = tree.get("Comments")
        if type(nodes) is not list or type(comments) is not list:
            raise contract_error("/Body/Tree", "BehaviorTree.InvalidTreeCollections", "Nodes and Comments must be arrays")
        node_ids: set[str] = set()
        for index, node in enumerate(nodes):
            node_path = f"/Body/Tree/Nodes/{index}"
            if type(node) is not dict or set(node) != _NODE_KEYS:
                raise contract_error(node_path, "BehaviorTree.InvalidTreeNode", "node has the wrong canonical shape")
            node_id = _require_nonempty_string(node["Id"], f"{node_path}/Id", "BehaviorTree.InvalidNodeId")
            if node_id in node_ids:
                raise contract_error(f"{node_path}/Id", "BehaviorTree.DuplicateNodeId", "node IDs must be unique")
            node_ids.add(node_id)
            if node["Kind"] != "Main":
                raise contract_error(f"{node_path}/Kind", "BehaviorTree.InvalidNodeKind", "main node kind is required")
            _require_nonempty_string(node["Class"], f"{node_path}/Class", "BehaviorTree.InvalidNodeClass")
            BehaviorTreeProfileCandidate._validate_position(node["Position"], f"{node_path}/Position")
            if type(node["Children"]) is not list or any(type(child) is not str for child in node["Children"]):
                raise contract_error(f"{node_path}/Children", "BehaviorTree.InvalidChildren", "children must be string IDs")
            for subnode_key, kind in (("Decorators", "Decorator"), ("Services", "Service")):
                subnodes = node[subnode_key]
                if type(subnodes) is not list:
                    raise contract_error(f"{node_path}/{subnode_key}", "BehaviorTree.InvalidSubnodes", "subnodes must be arrays")
                for subindex, subnode in enumerate(subnodes):
                    subpath = f"{node_path}/{subnode_key}/{subindex}"
                    if type(subnode) is not dict or set(subnode) != _SUBNODE_KEYS or subnode["Kind"] != kind:
                        raise contract_error(subpath, "BehaviorTree.InvalidSubnode", "subnode has the wrong canonical shape")
                    _require_nonempty_string(subnode["Id"], f"{subpath}/Id", "BehaviorTree.InvalidSubnodeId")
                    _require_nonempty_string(subnode["Class"], f"{subpath}/Class", "BehaviorTree.InvalidSubnodeClass")
        for index, comment in enumerate(comments):
            path = f"/Body/Tree/Comments/{index}"
            if type(comment) is not dict or set(comment) != _COMMENT_KEYS:
                raise contract_error(path, "BehaviorTree.InvalidComment", "comment has the wrong canonical shape")
            _require_nonempty_string(comment["Id"], f"{path}/Id", "BehaviorTree.InvalidCommentId")
            _require_nonempty_string(comment["Text"], f"{path}/Text", "BehaviorTree.InvalidCommentText")
            BehaviorTreeProfileCandidate._validate_bounds(comment["Bounds"], f"{path}/Bounds")
        for node in nodes:
            for child in node["Children"]:
                if child not in node_ids:
                    raise contract_error("/Body/Tree/Nodes", "BehaviorTree.UnknownChild", "child must name a declared node")

    @staticmethod
    def _validate_position(value: object, path: str) -> None:
        if type(value) is not dict or set(value) != {"x", "y"} or any(type(item) is not int for item in value.values()):
            raise contract_error(path, "BehaviorTree.InvalidPosition", "position requires exact integer x/y")

    @staticmethod
    def _validate_bounds(value: object, path: str) -> None:
        if type(value) is not dict or set(value) != {"x", "y", "w", "h"} or any(type(item) is not int for item in value.values()):
            raise contract_error(path, "BehaviorTree.InvalidBounds", "bounds require exact integer x/y/w/h")

    @staticmethod
    def _reject_requested_blocker(tree: dict) -> None:
        for field, code in _TREE_BLOCKERS.items():
            if tree.get(field) is True:
                raise contract_error(f"/Body/Tree/{field}", code, "requested BehaviorTree capability is a frozen blocker")

    def _create_nodes(self, context: GenericPrimitiveContext, specs: list, graph: OpaqueHandleProxy) -> dict[str, OpaqueHandleProxy]:
        created: dict[str, OpaqueHandleProxy] = {}
        for spec in specs:
            node = self._one_handle(
                self._invoke(
                    context,
                    "ai.behavior_tree.node.create_action.v1",
                    {"node_class_path": spec["Class"], "position": spec["Position"]},
                    (NamedOpaqueHandleArgument("graph", graph),),
                ),
                "node",
            )
            created[spec["Id"]] = node
            for key, kind in (("Decorators", "Decorator"), ("Services", "Service")):
                for subnode in spec[key]:
                    self._one_handle(
                        self._invoke(
                            context,
                            "ai.behavior_tree.subnode.create_action.v1",
                            {"node_class_path": subnode["Class"], "subnode_kind": kind},
                            (NamedOpaqueHandleArgument("graph", graph), NamedOpaqueHandleArgument("parent_node", node)),
                        ),
                        "node",
                    )
        return created

    def _create_comments(self, context: GenericPrimitiveContext, comments: list, graph: OpaqueHandleProxy) -> None:
        for comment in comments:
            self._one_handle(
                self._invoke(
                    context,
                    "ai.behavior_tree.comment.create_action.v1",
                    {"bounds": comment["Bounds"], "text": comment["Text"]},
                    (NamedOpaqueHandleArgument("graph", graph),),
                ),
                "node",
            )

    def _connect_children(self, context: GenericPrimitiveContext, specs: list, nodes: dict[str, OpaqueHandleProxy], graph: OpaqueHandleProxy) -> None:
        for spec in specs:
            for child_id in spec["Children"]:
                self._invoke(
                    context,
                    "ai.behavior_tree.graph.connect.v1",
                    {"from_pin_name": "Out", "to_pin_name": "In"},
                    (NamedOpaqueHandleArgument("graph", graph), NamedOpaqueHandleArgument("from_node", nodes[spec["Id"]]), NamedOpaqueHandleArgument("to_node", nodes[child_id])),
                )

    @staticmethod
    def _invoke(context: GenericPrimitiveContext, primitive_id: str, value: dict, handles: tuple[NamedOpaqueHandleArgument, ...]) -> PrimitiveOutcome:
        outcome = context.invoke(requirement=_REQUIREMENT_BY_ID[primitive_id], value_arguments=_snapshot(value), handle_arguments=handles)
        if outcome.status is PrimitiveOutcomeStatus.SUCCEEDED:
            return outcome
        if outcome.diagnostics:
            raise AssetDocumentContractError(outcome.diagnostics[0])
        raise contract_error("/PrimitiveOutcome", "BehaviorTree.PrimitiveFailure", "primitive omitted a failure diagnostic")

    @staticmethod
    def _one_handle(outcome: PrimitiveOutcome, name: str) -> OpaqueHandleProxy:
        matches = [result for result in outcome.handle_results if result.name == name]
        if len(matches) != 1 or len(matches[0].handles) != 1:
            raise contract_error("/PrimitiveOutcome/HandleResults", "BehaviorTree.InvalidHandleResult", "primitive did not return its exact opaque handle")
        return matches[0].handles[0]
