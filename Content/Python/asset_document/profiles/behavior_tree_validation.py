"""BehaviorTree binding policy over phase-local, typed native evidence."""
from __future__ import annotations

from asset_document.diagnostics import contract_error
from asset_document.dto import PrimitiveRequirement
from asset_document.profiles import behavior_tree_execution as native
from asset_document.profiles.data_table_common import describe_candidates
from asset_document.regions.primitive_catalog import NamedOpaqueHandleArgument


BINDING_REQUIREMENT = PrimitiveRequirement("ai.behavior_tree.binding.inspect.v1", 1, native.ROUTE_ID)
_VALUE_OR_TYPES = frozenset("/Script/AIModule.ValueOrBBKey_" + name for name in (
    "Bool", "Class", "Enum", "Float", "Int32", "Name", "String", "Object", "Rotator", "Struct", "Vector"))


def _fail(path, code, message):
    raise contract_error(path, code, message)


def _none(value):
    return value in ("", "None")


def _token(value):
    return str(value).replace("~", "~0").replace("/", "~1")


def validate_tree_bindings(tree, *, context, blackboard, class_evidence,
                           target=None, read_handle=None, retained=False, authored_tree=None):
    """Full replacement uses class defaults; an overlay reads exact live nodes.

    Tree values are already lowered through the inspected property schema. No
    native address, handle or evidence is retained after this phase returns.
    """
    asset = native.ai_asset(context, read_handle) if retained else None
    bound_node_paths = {}

    def authored_paths(node, path):
        for index, decorator in enumerate(node.get("Decorators", [])):
            graph = decorator.get("BoundGraph")
            if graph is not None:
                graph_path = path + "/Decorators/" + str(index) + "/BoundGraph/Nodes/"
                for bound_index, bound_node in enumerate(graph["Nodes"]):
                    bound_node_paths[bound_node["Id"].upper()] = graph_path + str(bound_index)
        for index, child in enumerate(node.get("Children", [])):
            authored_paths(child, path + "/Children/" + str(index))

    if authored_tree is not None and authored_tree.get("Root"):
        authored_paths(authored_tree["Root"], "/Body/Tree/Root")

    def inspect_node(node, path, class_key="node_class_path", families=None):
        class_path = node[class_key]
        evidence = class_evidence(class_path)
        if families is not None and evidence.get("node_family") is not None and evidence["node_family"] not in families:
            _fail(path + "/Class", "InvalidBehaviorTreeNodeClass", "The exact node class does not belong to this graph role.")
        properties = node["authored_properties"]
        queries, policies = [], []

        def scan(value, shape, locator, wire_path, structs):
            kind = shape["kind"]
            if kind == "selector":
                queries.append({"kind": "selector", "locator": locator, "selected_key": value["Key"]})
                policies.append(("selector", wire_path, value, properties))
                return
            binding_kind = shape.get("binding_kind")
            if binding_kind in ("static_subtree", "dynamic_subtree"):
                queries.append({"kind": binding_kind, "locator": locator,
                                "subtree_path_or_null": None if value is None else value["Path"]})
                policies.append((binding_kind, wire_path, value, properties))
                return
            if kind == "struct":
                if shape.get("struct_path") in _VALUE_OR_TYPES:
                    queries.append({"kind": "value_or", "locator": locator,
                                    "selected_key": value["Key"],
                                    "authored_value_or_null": None if retained else value})
                    policies.append(("value_or", wire_path, value, properties))
                for name, child_shape in shape["fields"].items():
                    scan(value[name], child_shape, locator + [{"kind": "field", "name": name}],
                         wire_path + "/" + _token(name), structs)
            elif kind in ("array", "set"):
                for index, item in enumerate(value):
                    step = {"kind": "array_element", "index": index} if kind == "array" else {"kind": "set_element", "value": item}
                    scan(item, shape["element"], locator + [step], wire_path + "/" + str(index), structs)
            elif kind == "map":
                rows = value.items() if type(value) is dict else ((row["key"], row["value"]) for row in value)
                for key, item in rows:
                    scan(item, shape["value"], locator + [{"kind": "map_value", "key": key}],
                         wire_path + "/" + _token(key), structs)
            elif kind == "instanced_struct" and value is not None:
                struct_path = value["Struct"]
                scan(value["Properties"], {"kind": "struct", "struct_path": struct_path,
                                           "fields": structs[struct_path]},
                     locator + [{"kind": "instanced_struct", "struct_path": struct_path}],
                     wire_path + "/Properties", structs)
            elif kind == "instanced_object" and value is not None:
                inline = class_evidence(value["Class"], shape["declared_class_path"])
                scan(value["Properties"], {"kind": "struct", "fields": inline["property_schema"]},
                     locator + [{"kind": "instanced_object", "class_path": value["Class"]}],
                     wire_path + "/Properties", inline.get("struct_schemas", {}))

        for name, shape in evidence["property_schema"].items():
            scan(properties[name], shape, [{"kind": "field", "name": name}],
                 path + "/Properties/" + name, evidence.get("struct_schemas", {}))
        if not queries:
            return evidence
        source = ({"kind": "live_node", "behavior_tree_path": native.object_path(target),
                   "stable_node_id": node["stable_node_id"]} if retained else
                  {"kind": "class_defaults", "node_class_path": class_path})
        handles = (NamedOpaqueHandleArgument("asset", asset),) if retained else ()
        result = native.invoke(context, BINDING_REQUIREMENT,
            {"blackboard_path_or_null": blackboard, "queries": queries, "source": source}, handles).value
        observed = result.get("evidence") if type(result) is dict else None
        if type(observed) is not list or len(observed) != len(policies):
            _fail(path, "BehaviorTree.InvalidPrimitiveResult", "Binding evidence must preserve the exact query count.")
        # The effective Blackboard's key names, present when a queried key does not exist (the close ones name the fix).
        keys = result.get("blackboard_keys") if type(result.get("blackboard_keys")) is list else []
        for (kind, wire_path, value, node_properties), item in zip(policies, observed):
            def flag(name):
                if type(item) is not dict or type(item.get(name)) is not bool:
                    _fail(wire_path, "BehaviorTree.InvalidPrimitiveResult", "Binding evidence omitted a typed policy result.")
                return item[name]
            if kind == "selector":
                key = value["Key"]
                if _none(key):
                    if not retained and not flag("none_allowed"):
                        _fail(wire_path + "/Key", "MissingBlackboardKey", "The node requires an authored Blackboard key.")
                    continue
                if blackboard is None:
                    _fail("/Body/BlackboardAsset", "MissingBehaviorTreeBlackboard", "A bound selector requires an effective BlackboardAsset.")
                if not flag("key_exists"):
                    _fail(wire_path + "/Key", "UnknownBlackboardKey", f"The key '{key}' does not exist in the effective "
                          f"BlackboardAsset.{describe_candidates(key, keys, 'Keys')}")
                if not flag("filter_passes"):
                    _fail(wire_path + "/Key", "IncompatibleBlackboardKeyType", "The key does not satisfy the node's actual selector filter.")
                enum_values = item.get("enum_values_or_null")
                # Exact Blackboard decorator policy, including subclasses, is
                # identified by the declared selector metadata from the provider.
                if item.get("blackboard_decorator_selector", False) and enum_values is not None and node_properties.get("StringValue"):
                    token = node_properties["StringValue"]
                    if type(enum_values) is not dict or token not in enum_values:
                        _fail(path + "/Properties/StringValue", "UnknownBehaviorTreeEnumDecoratorValue", "The retained enum token is absent from the effective Blackboard enum.")
                    if node_properties["IntValue"] != enum_values[token]:
                        _fail(path + "/Properties/IntValue", "BehaviorTreeEnumDecoratorValueOutOfSync", "The authored enum integer disagrees with its token in the effective Blackboard enum.")
            elif kind == "value_or":
                if _none(value["Key"]):
                    continue
                if blackboard is None:
                    _fail("/Body/BlackboardAsset", "MissingBehaviorTreeBlackboard", "A bound ValueOr value requires an effective BlackboardAsset.")
                if not flag("key_exists"):
                    _fail(wire_path + "/Key", "UnknownValueOrBlackboardKey", f"The ValueOr key '{value['Key']}' does not exist in "
                          f"the effective BlackboardAsset.{describe_candidates(value['Key'], keys, 'Keys')}")
                if not flag("compatible"):
                    _fail(wire_path + "/Key", "IncompatibleValueOrBlackboardKeyType", "The key is incompatible with the actual authored ValueOr parameters.")
            else:
                if value is None:
                    if kind == "static_subtree":
                        _fail(wire_path, "IncompatibleBehaviorTreeBlackboard", "A static subtree requires a BehaviorAsset with a BlackboardAsset.")
                    continue
                if blackboard is None:
                    _fail("/Body/BlackboardAsset", "MissingBehaviorTreeBlackboard", "A subtree reference requires an effective BlackboardAsset.")
                if not flag("exists") or not flag("effective_blackboard_compatible"):
                    _fail(wire_path, "IncompatibleBehaviorTreeBlackboard", "The effective Blackboard must equal or derive from the subtree Blackboard.")
                if kind == "dynamic_subtree" and flag("has_root_decorators"):
                    _fail(wire_path, "DynamicSubtreeRootDecoratorsUnsupported", "Dynamic subtree defaults cannot contain root decorators.")
        return evidence

    def bound(graph, path):
        for index, node in enumerate(graph["nodes"]):
            if node["kind"] == "test":
                node_path = bound_node_paths.get(node["stable_node_id"], path + "/Nodes/" + str(index))
                inspect_node(node, node_path, "node_class_path_or_null", {"decorator"})

    def main(node, path):
        evidence = inspect_node(node, path, families={"composite"} if path == "/Body/Tree/Root" else {"composite", "task"})
        if evidence.get("node_family") == "task" and node["children"]:
            _fail(path + "/Children", "InvalidBehaviorTreeParentClass", "A task cannot own graph children.")
        if not evidence.get("pin_semantic_children", False):
            for index in range(1, len(node["children"])):
                before = node["children"][index - 1]["editor"]["position"]["x"]
                after = node["children"][index]["editor"]["position"]["x"]
                if after <= before:
                    _fail(path + "/Children/" + str(index) + "/Editor/Position/X",
                          "DuplicateBehaviorTreeSiblingCoordinate" if after == before else "BehaviorTreeLayoutOrderConflict",
                          "Children must follow strictly increasing editor X order.")
        for index, decorator in enumerate(node["decorators"]):
            child_path = path + "/Decorators/" + str(index)
            if decorator["kind"] == "composite":
                bound(decorator["bound_graph"], child_path + "/BoundGraph")
            else:
                inspect_node(decorator, child_path, families={"decorator"})
        for index, service in enumerate(node["services"]):
            inspect_node(service, path + "/Services/" + str(index), families={"service"})
        for index, child in enumerate(node["children"]):
            main(child, path + "/Children/" + str(index))

    if tree["root"] is not None:
        main(tree["root"], "/Body/Tree/Root")
