# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Pure legacy Tree.Root semantics and public authored-readback V2 projection.

The lowered records are an internal semantic value, not a native call contract.
Class-dependent property conversion belongs to an explicit caller-supplied mapper
using public type inspection. This module never infers enum types from integers.
"""
from __future__ import annotations

from copy import deepcopy
from dataclasses import replace
import hashlib
import math
import re
import struct
from typing import Callable

from asset_document.canonical.document_codec import DocumentCodec
from asset_document.diagnostics import AssetDocumentContractError, contract_error

PropertyMapper = Callable[[str, dict], dict]
ClassEvidence = Callable[[str], dict]
ObjectEvidence = Callable[[str, str], dict]
ReferenceResolver = Callable[[str, str, str, str], None]
_MISSING = object()
_GUID = re.compile(r"[0-9a-fA-F]{32}\Z")
_EDITOR = {"Position": "position", "NodeComment": "node_comment",
           "bCommentBubblePinned": "comment_bubble_pinned", "bCommentBubbleVisible": "comment_bubble_visible"}
_COMMENT = {"Id": "stable_node_id", "Text": "text", "Position": "position", "Size": "size",
            "Color": "color", "CommentDepth": "comment_depth", "FontSize": "font_size",
            "MoveMode": "move_mode", "NodeDetails": "node_details",
            "bCommentBubblePinned": "comment_bubble_pinned", "bCommentBubbleVisible": "comment_bubble_visible",
            "bCommentBubbleVisible_InDetailsPanel": "comment_bubble_visible_in_details",
            "bColorCommentBubble": "color_comment_bubble"}


def _fail(path, message, code="BehaviorTree.InvalidCanonicalShape"):
    raise contract_error(path, code, message)


def _path(path, token):
    return path + "/" + str(token).replace("~", "~0").replace("/", "~1")


def _object(value, keys, path, required=(), *, code="BehaviorTree.InvalidCanonicalShape",
            unknown_code="UnknownField"):
    if type(value) is not dict:
        _fail(path, "Expected an object", code)
    for name in value:
        if name not in keys:
            _fail(_path(path, name), "Unknown authored field", unknown_code)
    for name in required:
        if name not in value:
            _fail(_path(path, name), "Required authored field is missing", code)
    return value


def _guid(value, path, code="InvalidBehaviorTreeNodeGuid"):
    if type(value) is not str or not _GUID.fullmatch(value) or int(value, 16) == 0:
        _fail(path, "Expected a nonzero 32-hex persistent GUID", code)
    # Stock FGuid::ToString(Digits) supplies the native authored identity.
    return value.upper()


def _text(value, path, code="BehaviorTree.InvalidCanonicalShape"):
    if type(value) is not str:
        _fail(path, "Expected a string", code)
    return value


def _int(value, path, code="BehaviorTree.InvalidCanonicalShape"):
    if type(value) not in (int, float) or not math.isfinite(value) or int(value) != value or not -(2**31) <= value < 2**31:
        _fail(path, "Expected a finite int32", code)
    return int(value)


def _array(value, path, code="BehaviorTree.InvalidCanonicalShape"):
    if type(value) is not list:
        _fail(path, "Expected an array", code)
    return value


def _editor(value, ordinal, depth, path):
    value = _object(value, _EDITOR, path, code="InvalidBehaviorTreeObject")
    result = {"position": {"x": ordinal * 300, "y": depth * 300},
              "node_comment": "", "comment_bubble_pinned": False, "comment_bubble_visible": False}
    for name, item in value.items():
        if name == "Position":
            _object(item, ("X", "Y"), path + "/Position", code="InvalidBehaviorTreeObject")
            result["position"] = {key.lower(): _int(item.get(key), path + "/Position/" + key,
                                                   "InvalidBehaviorTreeCoordinate") for key in ("X", "Y")}
        elif name == "NodeComment":
            result[_EDITOR[name]] = _text(item, path + "/" + name, "InvalidBehaviorTreeEditorField")
        elif type(item) is not bool:
            _fail(path + "/" + name, "Expected a boolean", "InvalidBehaviorTreeEditorField")
        else:
            result[_EDITOR[name]] = item
    return result


def _map_properties(node, mapper, path):
    value = node.get("Properties", {})
    if type(value) is not dict:
        _fail(path + "/Properties", "Properties must be an object", "InvalidTreeProperties")
    if mapper is None:
        return deepcopy(value)
    try:
        result = mapper(node["Class"].strip(), deepcopy(value))
    except AssetDocumentContractError as error:
        relative = error.diagnostic.path
        if relative == "/Class" or relative == "/Properties" or relative.startswith("/Properties/"):
            raise AssetDocumentContractError(replace(error.diagnostic, path=path + relative)) from error
        raise
    if type(result) is not dict:
        _fail(path, "Property mapper must return an object")
    DocumentCodec().validate(result)
    return result


class _Lower:
    def __init__(self, mapper, existing_comments=()):
        self.mapper = mapper
        self.existing_comments = {_guid(item["Id"], "/Existing/Comments/Id"): item for item in existing_comments}
        self.ids = set()
        self.ordinal = 0
        self.positions = set()

    def identity(self, node, path):
        value = _guid(node.get("Id"), path + "/Id")
        if value in self.ids:
            _fail(path + "/Id", "Persistent node IDs must be unique", "DuplicateBehaviorTreeNodeGuid")
        self.ids.add(value)
        return value

    def editor(self, node, depth, path):
        result = _editor(node.get("Editor", {}), self.ordinal, depth, path + "/Editor")
        self.ordinal += 1
        return result

    def simple(self, node, path, depth=0):
        identity = self.identity(node, path)
        class_path = _text(node.get("Class"), path + "/Class", "InvalidBehaviorTreeNodeClass").strip()
        if not class_path:
            _fail(path + "/Class", "Class is required", "InvalidBehaviorTreeNodeClass")
        return {"stable_node_id": identity, "node_class_path": class_path,
                "authored_properties": _map_properties(node, self.mapper, path),
                "editor": self.editor(node, depth, path)}

    def main(self, node, path, depth=0):
        _object(node, ("Id", "Class", "Properties", "Editor", "Children", "Decorators", "Services"), path,
                code="InvalidBehaviorTreeNode")
        result = self.simple(node, path, depth)
        position = tuple(result["editor"]["position"].values())
        if position in self.positions:
            _fail(path + "/Editor/Position/X", "Main nodes cannot share a coordinate",
                  "DuplicateBehaviorTreeSiblingCoordinate")
        self.positions.add(position)
        result["decorators"] = [self.decorator(item, f"{path}/Decorators/{i}")
                                for i, item in enumerate(_array(node.get("Decorators", []), path + "/Decorators",
                                                               "InvalidBehaviorTreeArray"))]
        result["services"] = []
        for i, item in enumerate(_array(node.get("Services", []), path + "/Services", "InvalidBehaviorTreeArray")):
            subpath = f"{path}/Services/{i}"
            _object(item, ("Id", "Class", "Properties", "Editor"), subpath, code="InvalidBehaviorTreeService")
            result["services"].append(self.simple(item, subpath))
        result["children"] = [self.main(item, f"{path}/Children/{i}", depth + 1)
                              for i, item in enumerate(_array(node.get("Children", []), path + "/Children",
                                                             "InvalidBehaviorTreeArray"))]
        return result

    def decorator(self, node, path):
        if type(node) is not dict:
            _fail(path, "Decorator must be an object", "InvalidBehaviorTreeDecorator")
        if "Kind" in node and node["Kind"] != "Composite":
            _fail(path + "/Kind", "Decorator Kind must be Composite when present", "InvalidBehaviorTreeDecoratorKind")
        if "Kind" not in node:
            _object(node, ("Id", "Class", "Properties", "Editor"), path)
            return dict(self.simple(node, path), kind="simple", bound_graph=None)
        _object(node, ("Id", "Kind", "Properties", "Editor", "BoundGraph"), path)
        properties = _object(node.get("Properties", {}), ("CompositeName", "bShowOperations"), path + "/Properties",
                             code="InvalidTreeProperties")
        name = _text(properties.get("CompositeName", ""), path + "/Properties/CompositeName",
                     "InvalidBehaviorTreeDecoratorProperty")
        show = properties.get("bShowOperations", True)
        if type(show) is not bool:
            _fail(path + "/Properties/bShowOperations", "bShowOperations must be boolean",
                  "InvalidBehaviorTreeDecoratorProperty")
        identity = self.identity(node, path)
        editor = self.editor(node, 0, path)
        bound = self.bound(node.get("BoundGraph"), path + "/BoundGraph")
        return {"stable_node_id": identity, "node_class_path": None, "kind": "composite",
                "authored_properties": {"CompositeName": name, "bShowOperations": show},
                "editor": editor, "bound_graph": bound}

    def bound(self, graph, path):
        _object(graph, ("GraphGuid", "Nodes", "Links"), path, code="InvalidBehaviorTreeObject")
        graph_guid = _guid(graph.get("GraphGuid"), path + "/GraphGuid", "InvalidBehaviorTreeGraphGuid")
        nodes, node_paths = [], {}
        for i, node in enumerate(_array(graph.get("Nodes"), path + "/Nodes", "InvalidBehaviorTreeArray")):
            subpath = f"{path}/Nodes/{i}"
            _object(node, ("Id", "Kind", "Class", "Properties", "Editor"), subpath,
                    code="InvalidBehaviorTreeDecoratorNode")
            identity = self.identity(node, subpath)
            kind = node.get("Kind")
            if kind not in ("Sink", "Test", "And", "Or", "Not"):
                _fail(subpath + "/Kind", "Unknown bound node kind", "InvalidBehaviorTreeDecoratorKind")
            if kind == "Test":
                class_path = _text(node.get("Class"), subpath + "/Class", "InvalidBehaviorTreeNodeClass").strip()
                if not class_path:
                    _fail(subpath + "/Class", "Class is required", "InvalidBehaviorTreeNodeClass")
                entry = {"stable_node_id": identity, "node_class_path": class_path,
                         "authored_properties": _map_properties(node, self.mapper, subpath),
                         "editor": self.editor(node, 0, subpath)}
            else:
                if "Class" in node or "Properties" in node:
                    field = "Class" if "Class" in node else "Properties"
                    _fail(subpath + "/" + field, "Only Test bound nodes declare Class or Properties",
                          "InvalidBehaviorTreeDecoratorNode")
                entry = {"stable_node_id": identity, "node_class_path": None,
                         "authored_properties": {}, "editor": self.editor(node, 0, subpath)}
            entry["node_class_path_or_null"] = entry.pop("node_class_path")
            entry["kind"] = kind.lower()
            nodes.append(entry)
            node_paths[identity] = subpath
        by_id = {node["stable_node_id"]: node for node in nodes}
        links, outgoing, inputs = [], {}, {}
        for i, link in enumerate(_array(graph.get("Links"), path + "/Links", "InvalidBehaviorTreeArray")):
            subpath = f"{path}/Links/{i}"
            _object(link, ("From", "To", "ToInput"), subpath, code="InvalidBehaviorTreeDecoratorLink")
            source = _guid(link.get("From"), subpath + "/From")
            target = _guid(link.get("To"), subpath + "/To")
            slot = _int(link.get("ToInput"), subpath + "/ToInput", "InvalidBehaviorTreeDecoratorInput")
            if slot < 0:
                _fail(subpath + "/ToInput", "ToInput must be non-negative", "InvalidBehaviorTreeDecoratorInput")
            for field, identity in (("From", source), ("To", target)):
                if identity not in by_id:
                    _fail(subpath + "/" + field, "Link endpoint does not reference a node", "DanglingBehaviorTreeDecoratorLink")
            if source in outgoing:
                _fail(subpath + "/From", "Node has more than one outgoing link", "MultipleBehaviorTreeDecoratorLinks")
            if (target, slot) in inputs:
                _fail(subpath + "/ToInput", "Input has more than one incoming link", "MultipleBehaviorTreeDecoratorLinks")
            outgoing[source] = (target, subpath)
            inputs[target, slot] = source
            links.append({"from_stable_node_id": source, "to_stable_node_id": target, "to_input_index": slot})
        # Match the old parser: the closing link wins over a resulting arity error.
        visited = set()
        for node in nodes:
            active, current = set(), node["stable_node_id"]
            while current not in visited:
                active.add(current)
                edge = outgoing.get(current)
                if edge is None:
                    break
                target, link_path = edge
                if target in active:
                    _fail(link_path, "Bound graph contains a cycle", "BehaviorTreeDecoratorGraphCycle")
                current = target
            visited.update(active)
        sinks = []
        for node in nodes:
            identity, kind = node["stable_node_id"], node["kind"]
            count = sum(target == identity for target, _ in inputs)
            expected = count if kind in ("and", "or") else (0 if kind == "test" else 1)
            if count != expected or (kind in ("and", "or") and count < 2) or any((identity, i) not in inputs for i in range(expected)):
                _fail(node_paths[identity], "Bound graph requires contiguous complete input arity",
                      "InvalidBehaviorTreeDecoratorArity")
            node["input_count"] = expected
            if kind == "sink":
                sinks.append(identity)
                if identity in outgoing:
                    _fail(node_paths[identity], "Sink cannot have an outgoing link", "InvalidBehaviorTreeDecoratorArity")
            elif identity not in outgoing:
                _fail(node_paths[identity], "Node does not reach the Sink", "UnreachableBehaviorTreeDecoratorNode")
        if len(sinks) != 1:
            _fail(path + "/Nodes", "Bound graph requires exactly one Sink", "InvalidBehaviorTreeDecoratorSink")
        for node in nodes:
            identity = node["stable_node_id"]
            seen, current = set(), identity
            while current != sinks[0]:
                if current in seen or current not in outgoing:
                    _fail(node_paths[identity], "Node does not reach the Sink", "UnreachableBehaviorTreeDecoratorNode")
                seen.add(current)
                current = outgoing[current][0]
        return {"graph_guid": graph_guid, "comments": [],
                "nodes": sorted(nodes, key=lambda item: item["stable_node_id"]),
                "links": sorted(links, key=lambda item: (item["from_stable_node_id"], item["to_stable_node_id"], item["to_input_index"]))}

    def comment(self, node, path):
        _object(node, _COMMENT, path, code="InvalidBehaviorTreeComment")
        # Existing comment omission means retain, unlike defaults for new IDs.
        original = self.existing_comments.get(_guid(node.get("Id"), path + "/Id"))
        if original:
            merged = deepcopy(original)
            for name, value in node.items():
                if name in ("Position", "Size", "Color") and type(value) is dict:
                    merged[name] = dict(merged.get(name, {}), **value)
                else:
                    merged[name] = value
            node = merged
        result = {"stable_node_id": self.identity(node, path), "text": "", "position": {"x": 0, "y": 0},
                  "size": {"width": 400, "height": 100}, "color": {"r": 1.0, "g": 1.0, "b": 1.0, "a": 1.0},
                  "comment_depth": -1, "font_size": 18, "move_mode": "GroupMovement", "node_details": "",
                  "comment_bubble_pinned": True, "comment_bubble_visible": True,
                  "comment_bubble_visible_in_details": True, "color_comment_bubble": False}
        for name, value in node.items():
            if name == "Id":
                continue
            key = _COMMENT[name]
            if name in ("Position", "Size", "Color"):
                names = {"Position": ("X", "Y"), "Size": ("Width", "Height"), "Color": ("R", "G", "B", "A")}[name]
                _object(value, names, path + "/" + name, code="InvalidBehaviorTreeObject")
                for k, v in value.items():
                    field_path = path + "/" + name + "/" + k
                    if name == "Color":
                        if type(v) not in (int, float) or not math.isfinite(v) or abs(v) > 3.4028234663852886e38:
                            _fail(field_path, "Color component must be a finite float32", "InvalidBehaviorTreeNumber")
                        result[key][k.lower()] = struct.unpack(">f", struct.pack(">f", v))[0]
                    else:
                        result[key][k.lower()] = _int(v, field_path, "InvalidBehaviorTreeCoordinate")
            elif type(result[key]) is bool:
                if type(value) is not bool:
                    _fail(path + "/" + name, "Comment flag must be boolean", "InvalidBehaviorTreeCommentField")
                result[key] = value
            elif type(result[key]) is int:
                result[key] = _int(value, path + "/" + name, "InvalidBehaviorTreeCoordinate")
            else:
                result[key] = _text(value, path + "/" + name, "InvalidBehaviorTreeCommentField")
        if result["move_mode"] not in ("GroupMovement", "NoGroupMovement"):
            _fail(path + "/MoveMode", "Unknown comment movement mode", "InvalidBehaviorTreeCommentField")
        if min(result["size"].values()) <= 0:
            _fail(path + "/Size", "Comment size must be positive", "InvalidBehaviorTreeCommentSize")
        if result["font_size"] < 1:
            _fail(path + "/FontSize", "Comment font size must be positive", "InvalidBehaviorTreeCommentField")
        return result


def tree_to_semantic_projection(tree: dict, *, property_mapper: PropertyMapper | None = None,
                                existing_tree: dict | None = None) -> dict:
    """Lower legacy authored intent; reuse an observed GUID or keep it unresolved.

    Properties remain canonical unless an explicit schema-backed mapper is supplied.
    Native class/family/property admission remains the provider's responsibility.
    existing_tree is a caller-supplied observation; it supplies graph identity and
    omitted fields of comments with an existing Id, without ambient state access.
    """
    DocumentCodec().validate(tree)
    _object(tree, ("GraphGuid", "Root", "Comments"), "/Body/Tree", ("Root",),
            code="InvalidBehaviorTreeObject")
    if existing_tree is not None:
        DocumentCodec().validate(existing_tree)
    existing = existing_tree or {}
    graph_guid = tree.get("GraphGuid", existing.get("GraphGuid"))
    if "GraphGuid" in tree or graph_guid is not None:
        graph_guid = _guid(graph_guid, "/Body/Tree/GraphGuid", "InvalidBehaviorTreeGraphGuid")
    if "GraphGuid" in existing and graph_guid != _guid(existing["GraphGuid"], "/Existing/GraphGuid", "InvalidBehaviorTreeGraphGuid"):
        _fail("/Body/Tree/GraphGuid", "Existing graph identity cannot be replaced",
              "BehaviorTreeGraphGuidReplacementRejected")
    lower = _Lower(property_mapper, existing.get("Comments", []))
    comments = [lower.comment(item, f"/Body/Tree/Comments/{i}")
                for i, item in enumerate(_array(tree.get("Comments", []), "/Body/Tree/Comments", "InvalidBehaviorTreeArray"))]
    root = tree["Root"]
    if type(root) is not dict:
        _fail("/Body/Tree/Root", "Root must be an object; empty object denotes an empty tree", "InvalidBehaviorTreeObject")
    return {"graph_state": "present", "graph_guid": graph_guid,
            "comments": sorted(comments, key=lambda item: item["stable_node_id"]),
            "root": lower.main(root, "/Body/Tree/Root") if root else None}


def canonicalize_body(body: dict) -> dict:
    """Validate structure while preserving omitted fields and explicit null exactly."""
    DocumentCodec().validate(body)
    _object(body, ("BlackboardAsset", "Tree"), "/Body", unknown_code="UnknownBodyKey")
    if "BlackboardAsset" in body and body["BlackboardAsset"] is not None:
        ref = body["BlackboardAsset"]
        ref_path = "/Body/BlackboardAsset"
        if type(ref) is dict:
            _object(ref, ("Kind", "Path"), ref_path)
            if ref.get("Kind") != "AssetRef":
                _fail(ref_path + "/Kind", "Expected AssetRef", "InvalidBehaviorTreeBlackboardRef")
            ref, ref_path = ref.get("Path"), ref_path + "/Path"
        elif type(ref) is not str:
            _fail(ref_path, "Expected a string or Blackboard AssetRef object", "InvalidBodySectionType")
        if type(ref) is not str or not ref.strip():
            _fail(ref_path, "Blackboard asset path must be a non-empty string", "MissingBehaviorTreeBlackboardPath")
        if not ref.startswith("/"):
            _fail(ref_path, "Expected an absolute Blackboard asset path")
    if "Tree" in body:
        tree_to_semantic_projection(body["Tree"])
    return deepcopy(body)


def _raise_editor(value):
    return {name: ({key.upper(): item for key, item in value[field].items()} if name == "Position"
                   else deepcopy(value[field])) for name, field in _EDITOR.items()}


def _raise_comment(value):
    result = {}
    for name, field in _COMMENT.items():
        item = deepcopy(value[field])
        if name in ("Position", "Color"):
            item = {key.upper(): child for key, child in item.items()}
        elif name == "Size":
            item = {key.capitalize(): child for key, child in item.items()}
        result[name] = item
    return result


def readback_v2_to_tree(value: dict, *, property_mapper: PropertyMapper | None = None) -> dict:
    """Map full public D2 Value to legacy authored shape without guessing types.

    Bound comments and unconnected input slots have no legacy wire field. Reject
    that unrepresentable extension explicitly instead of dropping authored data.
    The caller may retain the original V2 value for overlay equivalence/digests.
    """
    DocumentCodec().validate(value)
    _object(value, ("comments", "graph_guid", "graph_state", "root"), "/Readback", ("comments", "graph_guid", "graph_state", "root"))
    if value["graph_state"] not in ("absent", "present"):
        _fail("/Readback", "Unknown graph state")

    def properties(node, class_key="node_class_path"):
        props = deepcopy(node["authored_properties"])
        return property_mapper(node[class_key], props) if property_mapper else props

    def simple(node, class_key="node_class_path"):
        return {"Id": node["stable_node_id"], "Class": node[class_key],
                "Properties": properties(node, class_key), "Editor": _raise_editor(node["editor"])}

    def bound(graph):
        if graph["comments"]:
            _fail("/Readback/BoundGraph/comments", "Legacy BoundGraph has no Comments field", "BehaviorTree.UnrepresentableReadback")
        nodes = []
        for node in graph["nodes"]:
            linked = sum(link["to_stable_node_id"] == node["stable_node_id"] for link in graph["links"])
            if linked != node["input_count"]:
                _fail("/Readback/BoundGraph/input_count", "Legacy BoundGraph cannot encode unconnected input slots", "BehaviorTree.UnrepresentableReadback")
            entry = simple(node, "node_class_path_or_null") if node["kind"] == "test" else {
                "Id": node["stable_node_id"], "Editor": _raise_editor(node["editor"])}
            entry["Kind"] = node["kind"].capitalize()
            nodes.append(entry)
        return {"GraphGuid": graph["graph_guid"], "Nodes": nodes,
                "Links": [{"From": item["from_stable_node_id"], "To": item["to_stable_node_id"],
                           "ToInput": item["to_input_index"]} for item in graph["links"]]}

    def decorator(node):
        if node["kind"] == "simple":
            return simple(node)
        if node["kind"] != "composite" or set(node["authored_properties"]) != {"CompositeName", "bShowOperations"}:
            _fail("/Readback/Decorators", "Composite readback must include its complete authored wrapper fields", "BehaviorTree.IncompleteReadback")
        return {"Id": node["stable_node_id"], "Kind": "Composite", "Properties": deepcopy(node["authored_properties"]),
                "Editor": _raise_editor(node["editor"]), "BoundGraph": bound(node["bound_graph"])}

    def main(node):
        return dict(simple(node), Children=[main(child) for child in node["children"]],
                    Decorators=[decorator(child) for child in node["decorators"]],
                    Services=[simple(child) for child in node["services"]])

    if value["graph_state"] == "absent":
        if value["graph_guid"] is not None or value["root"] is not None or value["comments"]:
            _fail("/Readback", "Absent graph must have no authored content")
        return {"Root": {}, "Comments": []}
    result = {"GraphGuid": _guid(value["graph_guid"], "/Readback/graph_guid"),
              "Root": main(value["root"]) if value["root"] is not None else {},
              "Comments": [_raise_comment(item) for item in value["comments"]]}
    # Reuse the legacy topology validator, including global identity and arity.
    tree_to_semantic_projection(result)
    return result


def encode_runtime_value(value: object, *, preserve_object_order=False) -> bytes:
    """D2 RuntimeValue canonical bytes. Keep Integer distinct from Number."""
    DocumentCodec().validate(value)
    def encode(item):
        if item is None:
            return b"\x00"
        if type(item) is bool:
            return b"\x01" + bytes([item])
        if type(item) is int:
            return b"\x02" + struct.pack(">q", item)
        if type(item) is float:
            return b"\x03" + struct.pack(">d", 0.0 if item == 0 else item)
        if type(item) is str:
            raw = item.encode("utf-8")
            return b"\x04" + struct.pack(">Q", len(raw)) + raw
        if type(item) is list:
            return b"\x06" + struct.pack(">Q", len(item)) + b"".join(encode(child) for child in item)
        keys = item if preserve_object_order else sorted(item)
        return b"\x07" + struct.pack(">Q", len(item)) + b"".join(encode(key) + encode(item[key]) for key in keys)
    return encode(value)


def authored_digest_v2(tree_path: str, readback_value: dict) -> str:
    """Hash the complete public V2 Value, never the authored document projection."""
    _text(tree_path, "/behavior_tree_path")
    envelope = ["assetdocument.ai.bt.value_or.authored.v2", tree_path, readback_value]
    # Native readback already owns canonical field ordering. FString ordering
    # differs from Python's ordering for mixed-case authored property names.
    return hashlib.sha256(encode_runtime_value(envelope, preserve_object_order=True).hex().encode("ascii")).hexdigest()


def _enum_value(value, values, path, to_native, aliases=None):
    if type(values) is not dict or not values or any(type(k) is not str or type(v) is not int for k, v in values.items()):
        _fail(path, "Public enum evidence requires declared labels and integer values", "BehaviorTree.InvalidClassEvidence")
    if to_native:
        if aliases is not None:
            if type(aliases) is not dict or any(type(k) is not str or type(v) is not int or v not in values.values() for k, v in aliases.items()):
                _fail(path, "Enum aliases must name declared canonical values", "BehaviorTree.InvalidClassEvidence")
            if type(value) is str and value in aliases:
                return aliases[value]
        if type(value) is str and value in values:
            return values[value]
        if type(value) is int and value in values.values():
            return value
        _fail(path, "Enum value is not declared by the inspected type", "InvalidPropertyValue")
    if type(value) is not int:
        _fail(path, "Enum readback must retain its Integer kind")
    labels = [label for label, number in values.items() if number == value]
    if len(labels) != 1:
        _fail(path, "Enum value requires an unambiguous canonical label from public evidence")
    return labels[0]


def _typed_leaf_value(value, schema, path, to_native, reference_resolver):
    value_type = schema.get("value_type")
    if value_type is None:
        return deepcopy(value)
    if value_type == "bool":
        if type(value) is not bool:
            _fail(path, "Expected a boolean", "InvalidPropertyValue")
    elif value_type == "integer":
        bits, signed = schema.get("integer_bits"), schema.get("integer_signed")
        if type(bits) is not int or bits not in (8, 16, 32, 64) or type(signed) is not bool:
            _fail(path, "Integer evidence requires its actual width and signedness", "BehaviorTree.InvalidClassEvidence")
        minimum = -(2 ** (bits - 1)) if signed else 0
        maximum = 2 ** (bits - int(signed)) - 1
        if to_native and type(value) is float and math.isfinite(value) and value.is_integer():
            value = int(value)
        # The existing runtime Integer carrier is signed int64 even when the
        # reflected property is uint64. Preserve that input domain while
        # accepting the integral JSON Numbers handled by the old setter.
        if type(value) is not int or not minimum <= value <= min(maximum, 2 ** 63 - 1):
            _fail(path, "Integer is outside the inspected property type", "InvalidPropertyValue")
    elif value_type in ("string", "text"):
        _text(value, path, "InvalidPropertyValue")
    elif value_type == "reference":
        kind, declared = schema.get("reference_kind"), schema.get("declared_class_path")
        constraint = schema.get("reference_constraint", "represented_class" if kind == "ClassRef" else "object_class")
        soft = schema.get("reference_soft", False)
        if kind not in ("ClassRef", "AssetRef") or type(declared) is not str or not declared or constraint not in ("represented_class", "object_class") or type(soft) is not bool:
            _fail(path, "Reference evidence requires its exact declared class and constraint", "BehaviorTree.InvalidClassEvidence")
        if value is None or value == "":
            return None
        if type(value) is str:
            reference_path = value
        else:
            _object(value, ("Kind", "Path"), path, ("Kind", "Path"), code="InvalidPropertyValue")
            if value["Kind"] != kind:
                _fail(path + "/Kind", "Reference Kind disagrees with the inspected property", "InvalidPropertyValue")
            reference_path = _text(value["Path"], path + "/Path", "InvalidPropertyValue").strip()
            if not reference_path:
                return None
        if to_native and not soft and reference_resolver is not None:
            reference_resolver(constraint, reference_path, declared, path)
        return {"Kind": kind, "Path": reference_path}
    else:
        _fail(path, "Unsupported scalar type evidence", "BehaviorTree.InvalidClassEvidence")
    return deepcopy(value)


def _property_value(value, schema, default, path, to_native, struct_schemas, object_evidence, reference_resolver):
    """Convert only distinctions supported by explicit BT class evidence."""
    if type(schema) is not dict:
        _fail(path, "Missing property type evidence", "BehaviorTree.InvalidClassEvidence")
    kind = schema.get("kind")
    if kind == "instanced_object":
        if value is None:
            return None
        _object(value, ("Class", "Properties"), path, ("Class", "Properties"))
        declared = _text(schema.get("declared_class_path"), path)
        class_path = _text(value["Class"], path + "/Class")
        if object_evidence is None:
            _fail(path, "Inline object requires explicit native class/base evidence", "BehaviorTree.IncompleteClassEvidence")
        observed = object_evidence(class_path, declared)
        defaults, fields, structs = _class_evidence(class_path, lambda _: observed)
        if type(default) is dict and default.get("Class") == class_path:
            defaults = default["Properties"]
        return {"Class": class_path, "Properties": _property_value(value["Properties"],
                {"kind": "struct", "fields": fields}, defaults, path + "/Properties", to_native, structs, object_evidence, reference_resolver)}
    if kind == "selector":
        if type(value) is not dict:
            _fail(path, "BlackboardKeySelector must be an object", "InvalidPropertyValue")
        derived = {"SelectedKeyName", "AllowedTypes", "SelectedKeyType", "SelectedKeyID", "bNoneIsAllowedValue"}
        for name, item in value.items():
            if name in derived:
                _fail(_path(path, name), "Selector field is derived class policy or runtime cache", "NonAuthoredProperty")
            if name != "Key":
                _fail(_path(path, name), "Unknown BlackboardKeySelector field", "UnknownProperty")
            _text(item, path + "/Key", "InvalidPropertyValue")
        if "Key" not in value:
            _fail(path + "/Key", "BlackboardKeySelector requires authored Key", "MissingProperty")
        key = value["Key"]
        return {"Key": "None" if not key or key.casefold() == "none" else key}
    if kind == "name":
        name = _text(value, path, "InvalidPropertyValue")
        return "None" if not name or name.casefold() == "none" else name
    if kind == "instanced_struct":
        if value is None:
            return None
        _object(value, ("Struct", "Properties"), path, code="InvalidPropertyValue", unknown_code="UnknownProperty")
        struct_path = _text(value.get("Struct"), path + "/Struct", "InvalidStructRef")
        if not struct_path.strip():
            _fail(path + "/Struct", "Struct must be a non-empty script struct path", "InvalidStructRef")
        if type(value.get("Properties")) is not dict:
            _fail(path + "/Properties", "FInstancedStruct Properties must be an object", "InvalidPropertyValue")
        if struct_path not in struct_schemas:
            _fail(path, "Dynamic struct requires explicitly requested public type evidence", "BehaviorTree.IncompleteClassEvidence")
        base = default.get("Properties", {}) if type(default) is dict and default.get("Struct") == struct_path else {}
        return {"Struct": struct_path, "Properties": _property_value(value["Properties"],
                {"kind": "struct", "fields": struct_schemas[struct_path]}, base, path + "/Properties", to_native, struct_schemas, object_evidence, reference_resolver)}
    if kind == "enum":
        return _enum_value(value, schema.get("values"), path, to_native, schema.get("aliases"))
    if kind == "number":
        if type(value) not in (int, float) or not math.isfinite(value):
            _fail(path, "Expected a finite authored number", "InvalidPropertyValue")
        if schema.get("precision") == "float32":
            if abs(value) > 3.4028234663852886e38:
                _fail(path, "Number exceeds the inspected float32 range", "InvalidPropertyValue")
            return struct.unpack(">f", struct.pack(">f", value))[0]
        if schema.get("precision") != "float64":
            _fail(path, "Number evidence must specify float32 or float64 precision", "BehaviorTree.InvalidClassEvidence")
        return float(value)
    if kind == "struct":
        fields = schema.get("fields")
        if type(fields) is not dict:
            _fail(path, "Struct requires inspected field evidence", "BehaviorTree.InvalidClassEvidence")
        if type(value) is not dict:
            _fail(path, "Struct value must be an object", "InvalidPropertyValue")
        for name in value:
            if name not in fields:
                _fail(_path(path, name), "Struct field is absent from inspected authored type", "UnknownProperty")
        if not to_native and set(value) != set(fields):
            _fail(path, "Readback must cover the complete inspected struct", "BehaviorTree.IncompleteReadback")
        base = default if type(default) is dict else schema.get("default", {})
        result = {}
        names = fields if to_native else value
        for name in names:
            child_schema = fields[name]
            child_default = base.get(name, child_schema.get("default", _MISSING))
            child = value.get(name, child_default)
            if child is _MISSING:
                _fail(_path(path, name), "Omitted struct field requires observed native default", "BehaviorTree.IncompleteClassEvidence")
            result[name] = _property_value(child, child_schema, child_default, _path(path, name), to_native, struct_schemas, object_evidence, reference_resolver)
        return result
    if kind in ("array", "set"):
        _array(value, path, "InvalidPropertyValue")
        element = schema.get("element")
        if type(element) is not dict:
            _fail(path, "Container element type is missing", "BehaviorTree.InvalidClassEvidence")
        converted = [_property_value(item, element, element.get("default", _MISSING), f"{path}/{i}", to_native, struct_schemas, object_evidence, reference_resolver)
                     for i, item in enumerate(value)]
        if kind == "set":
            key = encode_runtime_value if to_native else DocumentCodec().encode
            encoded = [key(item) for item in converted]
            if len(set(encoded)) != len(encoded):
                _fail(path, "Duplicate set element after typed canonical conversion")
            converted = [item for _, item in sorted(zip(encoded, converted), key=lambda pair: pair[0])]
        return converted
    if kind == "map":
        if not to_native and type(value) is list:
            # V2 extends only the previously unrepresentable empty-key domain;
            # ordinary maps retain their historical object projection and digest.
            rows = value
            if any(type(row) is not dict or set(row) != {"key", "value"}
                   or type(row["key"]) is not str for row in rows):
                _fail(path, "Map entries require exact string key/value rows")
            keys = [row["key"] for row in rows]
            if "" not in keys or keys != sorted(set(keys), key=lambda key: key.encode("utf-16-be")) or "key_enum" in schema or schema.get("key_kind") == "name":
                _fail(path, "Extended map entries require unique ordered keys including empty string")
            value = {row["key"]: row["value"] for row in rows}
        if type(schema.get("value")) is not dict:
            _fail(path, "Map requires inspected value evidence", "BehaviorTree.InvalidClassEvidence")
        if type(value) is not dict:
            _fail(path, "Map value must be an object", "InvalidPropertyValue")
        result = {}
        for name, item in value.items():
            mapped_name = name
            if schema.get("key_kind") == "name" and name == "":
                mapped_name = "None"
            if "key_enum" in schema:
                if to_native:
                    mapped_name = str(_enum_value(name, schema["key_enum"], path, True, schema.get("key_enum_aliases")))
                else:
                    try:
                        numeric_key = int(name)
                    except ValueError:
                        _fail(path, "Enum map readback key must be a decimal integer")
                    if str(numeric_key) != name:
                        _fail(path, "Enum map key is not canonical decimal")
                    mapped_name = _enum_value(numeric_key, schema["key_enum"], path, False)
            if mapped_name in result:
                _fail(path, "Duplicate map key after typed conversion")
            result[mapped_name] = _property_value(item, schema["value"], schema["value"].get("default", _MISSING), _path(path, name), to_native, struct_schemas, object_evidence, reference_resolver)
        if to_native and "" in result:
            return [{"key": key, "value": result[key]} for key in sorted(result, key=lambda key: key.encode("utf-16-be"))]
        return result
    if kind != "value":
        _fail(path, "Unsupported property evidence kind", "BehaviorTree.InvalidClassEvidence")
    return _typed_leaf_value(value, schema, path, to_native, reference_resolver)


def _class_evidence(class_path, evidence):
    observed = evidence(class_path)
    if type(observed) is not dict or observed.get("node_class_path") != class_path:
        _fail("/Class", "Public class evidence identity does not match", "BehaviorTree.InvalidClassEvidence")
    defaults, schema = observed.get("authored_defaults"), observed.get("property_schema")
    if type(defaults) is not dict or type(schema) is not dict or set(defaults) != set(schema):
        _fail("/Properties", "Complete CDO defaults and property schema must cover the same fields", "BehaviorTree.IncompleteClassEvidence")
    DocumentCodec().validate(defaults)
    DocumentCodec().validate(schema)
    structs = observed.get("struct_schemas", {})
    if type(structs) is not dict:
        _fail("/Properties", "Dynamic struct evidence must be an object", "BehaviorTree.InvalidClassEvidence")
    DocumentCodec().validate(structs)
    return defaults, schema, structs


def lower_authored_properties(class_path: str, authored: dict, *, evidence: ClassEvidence,
                               object_evidence: ObjectEvidence | None = None,
                               reference_resolver: ReferenceResolver | None = None) -> dict:
    """Complete requested fields with observed CDO defaults and normalize types.

    Containers replace their entire value; structs merge omitted fields with
    observed defaults. Full default completion makes unexpected native drift
    visible during Verify, including properties omitted from the document.
    """
    defaults, schema, structs = _class_evidence(class_path, evidence)
    if type(authored) is not dict:
        _fail("/Properties", "Authored properties must be an object", "InvalidTreeProperties")
    for name in authored:
        if name not in schema:
            _fail(_path("/Properties", name), "Authored property is absent from public class evidence", "UnknownProperty")
    DocumentCodec().validate(authored)
    result = {}
    for name, shape in schema.items():
        if name in authored:
            result[name] = _property_value(authored[name], shape, defaults[name], _path("/Properties", name), True, structs, object_evidence, reference_resolver)
        else:
            result[name] = deepcopy(defaults[name])
    return result


def extract_authored_properties(class_path: str, observed: dict, *, evidence: ClassEvidence,
                                omit_defaults: bool = False, object_evidence: ObjectEvidence | None = None,
                                reference_resolver: ReferenceResolver | None = None) -> dict:
    """Recover legacy enum/set/map representation; optional whole-property omission.

    Legacy Extract includes defaults. Omission is explicit, and only removes an
    entire property equal to its complete native default, never nested content.
    """
    defaults, schema, structs = _class_evidence(class_path, evidence)
    if type(observed) is not dict or set(observed) != set(schema):
        _fail("/Properties", "Readback must cover the complete inspected authored inventory", "BehaviorTree.IncompleteReadback")
    DocumentCodec().validate(observed)
    return {name: _property_value(value, schema[name], defaults[name], _path("/Properties", name), False, structs, object_evidence, reference_resolver)
            for name, value in observed.items()
            if not omit_defaults or encode_runtime_value(value) != encode_runtime_value(defaults[name])}


def authored_properties_match(class_path: str, authored: dict, observed: dict, *, evidence: ClassEvidence,
                               object_evidence: ObjectEvidence | None = None,
                               reference_resolver: ReferenceResolver | None = None) -> bool:
    """Compare the complete expected typed value, including omitted CDO fields."""
    expected = lower_authored_properties(class_path, authored, evidence=evidence, object_evidence=object_evidence,
                                        reference_resolver=reference_resolver)
    # Validate completeness before comparing; missing native fields are not defaults.
    extract_authored_properties(class_path, observed, evidence=evidence, object_evidence=object_evidence,
                               reference_resolver=reference_resolver)
    return encode_runtime_value(expected) == encode_runtime_value(observed)


def selector_inventory(readback_value: dict, *, evidence: ClassEvidence,
                       object_evidence: ObjectEvidence | None = None) -> list[dict]:
    """Discover selectors by inspected type, with exact PropertyEditor locators.

    D2 sorts map/set values and does not expose PropertyEditor slot identities.
    Those paths, inline-object owners and dynamic struct wrappers require native
    locator evidence; fail explicitly when they actually contain a selector.
    Ordinary structs and arrays of structs retain their observable property path.
    """
    DocumentCodec().validate(readback_value)
    result, identities = [], set()

    def scan(value, shape, path, identity, structs, unsupported=False, inner=False):
        kind = shape.get("kind")
        if kind == "selector":
            if unsupported or inner:
                _fail(path, "Selector requires an observed native owner/property locator", "BehaviorTree.SelectorLocatorEvidenceRequired")
            _object(value, ("Key",), path, ("Key",))
            result.append({"stable_node_id": identity, "property_path": path,
                           "selected_key_name": _text(value["Key"], path + "/Key")})
        elif kind == "struct":
            if type(value) is not dict or set(value) != set(shape["fields"]):
                _fail(path, "Selector discovery requires complete typed fields", "BehaviorTree.IncompleteReadback")
            for name, child in shape["fields"].items():
                scan(value[name], child, path + "." + name, identity, structs, unsupported)
        elif kind == "array":
            for index, item in enumerate(_array(value, path)):
                # FPropertyNode includes both the container property and its
                # same-named inner property, whose ArrayIndex carries the slot.
                inner_name = path.rsplit(".", 1)[-1].split("[", 1)[0]
                scan(item, shape["element"], f"{path}.{inner_name}[{index}]", identity, structs, unsupported, True)
        elif kind in ("map", "set"):
            child = shape["value"] if kind == "map" else shape["element"]
            values = (value.values() if type(value) is dict else
                      [row["value"] for row in value]) if kind == "map" else value
            for item in values:
                scan(item, child, path, identity, structs, True, True)
        elif kind == "instanced_struct" and value is not None:
            type_path = value["Struct"]
            if type_path not in structs:
                _fail(path, "Dynamic struct selector discovery requires public type evidence", "BehaviorTree.IncompleteClassEvidence")
            scan(value["Properties"], {"kind": "struct", "fields": structs[type_path]}, path, identity, structs, True)
        elif kind == "instanced_object" and value is not None:
            if object_evidence is None:
                _fail(path, "Inline object discovery requires explicit native class evidence", "BehaviorTree.IncompleteClassEvidence")
            observed = object_evidence(value["Class"], shape["declared_class_path"])
            _, fields, child_structs = _class_evidence(value["Class"], lambda _: observed)
            scan(value["Properties"], {"kind": "struct", "fields": fields}, path, identity, child_structs, True)

    def node(value, class_key="node_class_path"):
        identity = _guid(value["stable_node_id"], "/Readback/stable_node_id")
        if identity in identities:
            _fail("/Readback", "Duplicate node identity in selector inventory")
        identities.add(identity)
        _, fields, structs = _class_evidence(value[class_key], evidence)
        properties = value["authored_properties"]
        if set(properties) != set(fields):
            _fail("/Readback", "Selector discovery requires the complete class property inventory", "BehaviorTree.IncompleteReadback")
        for name, shape in fields.items():
            scan(properties[name], shape, name, identity, structs)

    def main(value):
        node(value)
        for service in value["services"]:
            node(service)
        for decorator in value["decorators"]:
            if decorator["kind"] == "simple":
                node(decorator)
            elif decorator["bound_graph"] is not None:
                for bound in decorator["bound_graph"]["nodes"]:
                    if bound["kind"] == "test":
                        node(bound, "node_class_path_or_null")
        for child in value["children"]:
            main(child)

    if readback_value["root"] is not None:
        main(readback_value["root"])
    return sorted(result, key=lambda item: (item["stable_node_id"], item["property_path"]))
