"""Legacy Tree.Root semantics implemented through finite AI provider operations."""
from __future__ import annotations

from copy import deepcopy
from functools import partial
from uuid import uuid4

from asset_document.canonical.document_codec import DocumentCodec
from asset_document.diagnostics import contract_error
from asset_document.diff.canonical_diff import canonical_diff, canonical_equal
from asset_document.diff.model import Difference, DiffKind
from asset_document.dto import PrimitiveRequirement, RuntimePhase
from asset_document.meta import (
    ComparisonPolicy, DefaultPolicy, DeletePolicy, DocumentAction, IdentityPolicy,
    OmissionPolicy, OrderPolicy, ProfileMeta, RegionMeta, RegionOwnership,
)
from asset_document.profiles.generic import GenericProfileCandidate, omit_inspected_properties
from asset_document.profiles import behavior_tree_execution as native
from asset_document.profiles.behavior_tree_codec import (
    authored_digest_v2, canonicalize_body, encode_runtime_value,
    extract_authored_properties, lower_authored_properties,
    readback_v2_to_tree, tree_to_semantic_projection, _raise_comment, _raise_editor,
)
from asset_document.profiles.behavior_tree_validation import validate_tree_bindings
from asset_document.regions.primitive_catalog import NamedOpaqueHandleArgument, _single_handle


BEHAVIOR_TREE_CLASS = native.CLASS_PATH
BEHAVIOR_TREE_ROUTE_ID = native.ROUTE_ID
_VERSIONS = {
    "ai.behavior_tree.blackboard.readback.v1": 1,
    "ai.behavior_tree.authored_graph.readback.v2": 2,
    "ai.behavior_tree.node_class.inspect.v2": 2,
    "ai.behavior_tree.binding.inspect.v1": 1,
    "ai.behavior_tree.selector.inventory.v1": 1,
    "ai.behavior_tree.selector.resolve_batch.v2": 2,
    "ai.behavior_tree.graph.create.v1": 1,
    "ai.behavior_tree.graph.author_scratch.v1": 1,
    "ai.behavior_tree.graph.install.v1": 1,
    "ai.behavior_tree.blackboard.set.v1": 1,
    "ai.behavior_tree.value_or_key.refresh.v3": 3,
}
REQUIRED_PRIMITIVES = tuple(PrimitiveRequirement(name, version, BEHAVIOR_TREE_ROUTE_ID)
                            for name, version in _VERSIONS.items())


def _invoke(context, primitive, values, handles=()):
    return native.invoke(context, PrimitiveRequirement(primitive, _VERSIONS[primitive],
                         BEHAVIOR_TREE_ROUTE_ID), values, handles)


def _blackboard_path(value):
    path = value.get("Path") if type(value) is dict else value
    if path is not None and "." not in path.rsplit("/", 1)[-1]:
        path = native.object_path(path)
    return path


def _struct_paths(value):
    found = set()
    if type(value) is dict:
        if set(value) == {"Struct", "Properties"} and type(value["Struct"]) is str:
            found.add(value["Struct"])
        for child in value.values():
            found.update(_struct_paths(child))
    elif type(value) is list:
        for child in value:
            found.update(_struct_paths(child))
    return found


def _class_evidences(context, source):
    # Evidence and caches are confined to this phase; no native leases survive it.
    paths, cache = _struct_paths(source), {}
    def evidence(class_path, base="/Script/AIModule.BTNode"):
        key = (class_path, base)
        if key not in cache:
            while True:
                observed = native.class_evidence(context, class_path,
                                                 expected_base=base, struct_paths=paths)
                # Omitted instanced values still contribute their actual CDO
                # bindings. Request those observed types through the same v2
                # inspection before lowering or scanning their default values.
                missing = _struct_paths(observed) - paths
                if not missing:
                    cache[key] = observed
                    break
                paths.update(missing)
        return cache[key]
    return evidence


def _mapper(context, source, *, extract=False, evidence=None, omit_defaults=False):
    evidence = evidence or _class_evidences(context, source)
    convert = (partial(extract_authored_properties, omit_defaults=omit_defaults) if extract
               else lower_authored_properties)
    return lambda class_path, properties: convert(class_path, properties, evidence=evidence, object_evidence=evidence,
        reference_resolver=lambda constraint, path, declared, wire_path:
            native.validate_reference(context, constraint, path, declared, wire_path))


def _existing_identity(value):
    # Only identity and retained comment fields are needed for an authored replacement.
    # Do not require old wire representability of unrelated old bound graphs.
    projection = deepcopy(value)
    projection["root"] = None
    return readback_v2_to_tree(projection)


def _diff_path(path, name):
    return path + "/" + str(name).replace("~", "~0").replace("/", "~1")


def _flatten_diff_object(value, path, output):
    # Legacy Tree diff recurses through objects and compares arrays as a whole.
    for name, item in value.items():
        child_path = _diff_path(path, name)
        if type(item) is dict:
            _flatten_diff_object(item, child_path, output)
        else:
            output[child_path] = deepcopy(item)


def _flatten_diff_tree(tree, property_mapper):
    """Project typed observations to the existing GUID-addressed public paths."""
    path, output = "/Body/Tree", {}
    # Presence distinguishes an absent graph from an unresolved new empty graph.
    if tree["graph_state"] == "present":
        output[path + "/GraphGuid"] = tree["graph_guid"]
    output[path + "/Root"] = tree["root"]["stable_node_id"] if tree["root"] is not None else None
    output[path + "/Comments"] = sorted(comment["stable_node_id"] for comment in tree["comments"])
    for comment in tree["comments"]:
        fields = _raise_comment(comment)
        fields.pop("Id")
        _flatten_diff_object(fields, path + "/Comments/" + comment["stable_node_id"], output)

    def fields(node, node_path, class_key="node_class_path"):
        output[node_path + "/Class"] = node[class_key]
        _flatten_diff_object(property_mapper(node[class_key], node["authored_properties"]),
                             node_path + "/Properties", output)
        _flatten_diff_object(_raise_editor(node["editor"]), node_path + "/Editor", output)

    def attachment(node, collection):
        node_path = path + "/" + collection + "/" + node["stable_node_id"]
        if node.get("kind") != "composite":
            fields(node, node_path)
            return
        output[node_path + "/Kind"] = "Composite"
        _flatten_diff_object(node["authored_properties"], node_path + "/Properties", output)
        _flatten_diff_object(_raise_editor(node["editor"]), node_path + "/Editor", output)
        graph, bound_path = node["bound_graph"], node_path + "/BoundGraph"
        output[bound_path + "/GraphGuid"] = graph["graph_guid"]
        output[bound_path + "/Nodes"] = sorted(item["stable_node_id"] for item in graph["nodes"])
        output[bound_path + "/Links"] = [
            {"From": item["from_stable_node_id"], "To": item["to_stable_node_id"], "ToInput": item["to_input_index"]}
            for item in sorted(graph["links"], key=lambda item:
                (item["from_stable_node_id"], item["to_stable_node_id"], item["to_input_index"]))]
        for item in graph["nodes"]:
            item_path = bound_path + "/Nodes/" + item["stable_node_id"]
            output[item_path + "/Kind"] = item["kind"].capitalize()
            if item["kind"] == "test":
                fields(item, item_path, "node_class_path_or_null")
            else:
                _flatten_diff_object(_raise_editor(item["editor"]), item_path + "/Editor", output)

    def main(node):
        node_path = path + "/Nodes/" + node["stable_node_id"]
        fields(node, node_path)
        # Projection order already carries ordinary child order and SimpleParallel pins.
        for field, collection in (("children", "Children"), ("decorators", "Decorators"), ("services", "Services")):
            output[node_path + "/" + collection] = [item["stable_node_id"] for item in node[field]]
        for item in node["decorators"]:
            attachment(item, "Decorators")
        for item in node["services"]:
            attachment(item, "Services")
        for item in node["children"]:
            main(item)

    if tree["root"] is not None:
        main(tree["root"])
    return output


def _tree_pointers(tree, output):
    """GUID-addressed diff prefix -> JSON pointer of that node in a document Tree."""
    if type(tree) is not dict:
        return output

    def key(value):
        return str(value).upper()

    def attachments(node, pointer):
        for collection in ("Decorators", "Services"):
            for index, item in enumerate(node.get(collection) or ()):
                if type(item) is not dict or "Id" not in item:
                    continue
                item_pointer = f"{pointer}/{collection}/{index}"
                prefix = f"/Body/Tree/{collection}/{key(item['Id'])}"
                output.setdefault(prefix, item_pointer)
                graph = item.get("BoundGraph")
                for inner, bound in enumerate((graph or {}).get("Nodes") or ()):
                    if type(bound) is dict and "Id" in bound:
                        output.setdefault(f"{prefix}/BoundGraph/Nodes/{key(bound['Id'])}",
                                          f"{item_pointer}/BoundGraph/Nodes/{inner}")

    def main(node, pointer):
        if type(node) is not dict or "Id" not in node:
            return
        output.setdefault(f"/Body/Tree/Nodes/{key(node['Id'])}", pointer)
        attachments(node, pointer)
        for index, child in enumerate(node.get("Children") or ()):
            main(child, f"{pointer}/Children/{index}")

    main(tree.get("Root"), "/Body/Tree/Root")
    for index, comment in enumerate(tree.get("Comments") or ()):
        if type(comment) is dict and "Id" in comment:
            output.setdefault(f"/Body/Tree/Comments/{key(comment['Id'])}", f"/Body/Tree/Comments/{index}")
    return output


def _document_pointer(path, pointers):
    """Rewrite a GUID-addressed tree diff path to the JSON pointer of that node in the document
    (the authored document, or the extracted one for a node the document removes)."""
    parts = path.split("/")
    # Longest prefix first: a bound-graph node lives under its decorator.
    for length in (8, 5):
        prefix = "/".join(parts[:length])
        if len(parts) >= length and prefix in pointers:
            return pointers[prefix] + "".join("/" + part for part in parts[length:])
    return path


def _first_tree_mismatch(expected, actual, path=""):
    if type(expected) is not type(actual):
        return f"{path}: expected {type(expected).__name__} {expected!r}, observed {type(actual).__name__} {actual!r}"
    if type(expected) is dict:
        if expected.keys() != actual.keys():
            return f"{path}: missing {sorted(expected.keys() - actual.keys())}, unexpected {sorted(actual.keys() - expected.keys())}"
        pairs = ((name, expected[name], actual[name]) for name in sorted(expected))
    elif type(expected) is list:
        if len(expected) != len(actual):
            return f"{path}: expected {len(expected)} items, observed {len(actual)}"
        pairs = ((str(index), left, right) for index, (left, right) in enumerate(zip(expected, actual)))
    else:
        return None if expected == actual else f"{path}: expected {expected!r}, observed {actual!r}"
    for name, left, right in pairs:
        mismatch = _first_tree_mismatch(left, right, path + "/" + name)
        if mismatch:
            return mismatch
    return None


class BehaviorTreeProfile:
    def required_primitives(self):
        return REQUIRED_PRIMITIVES

    def inspect_profile(self):
        result = GenericProfileCandidate(BEHAVIOR_TREE_CLASS).inspect_profile()
        result["BodySections"] = ["BlackboardAsset", "Tree"]
        result["DocumentShape"]["Body"] = self.schema()["Body"]
        result["RegionPolicies"] = [
            {"RegionId": "Body." + region.region_id, "BodyPath": region.body_path,
             "RegionKind": region.ownership.value, "DefaultSource": region.default.value,
             "ApplyMode": "ManagedRegion", "OmissionPolicy": region.omission.value,
             "DeletePolicy": region.deletion.value, "OrderPolicy": region.order.value}
            for region in BEHAVIOR_TREE_PROFILE_META.regions]
        return result

    def inspect(self, **kwargs):
        # Body owns these; listing them as skipped "non-editable" rows would misdirect authors.
        return omit_inspected_properties(GenericProfileCandidate(BEHAVIOR_TREE_CLASS).inspect(**kwargs),
                                         ("BlackboardAsset", "BTGraph", "RootNode", "RootDecorators", "RootDecoratorOps"))

    def schema(self):
        return {"SchemaVersion": 1, "Class": BEHAVIOR_TREE_CLASS,
                "Body": {"BlackboardAsset": "object path | AssetRef | null",
                         "Tree": {"GraphGuid": "optional persistent GUID", "Root": "recursive node", "Comments": "array"}}}

    def create_template(self, target):
        result = GenericProfileCandidate(BEHAVIOR_TREE_CLASS).create_template(target)
        result["Body"] = {"BlackboardAsset": None, "Tree": {"GraphGuid": uuid4().hex.upper(), "Root": {}, "Comments": []}}
        return result

    def canonicalize(self, document):
        if type(document) is not dict:
            raise contract_error("/", "BehaviorTree.InvalidDocumentType", "Document must be an object.")
        header = deepcopy(document)
        body = header.pop("Body", {})
        result = GenericProfileCandidate(BEHAVIOR_TREE_CLASS).canonicalize(header)
        result["Body"] = canonicalize_body(body)
        return result

    def _validate_regions(self, canonical, *, context, exists, read, current=None, evidence=None):
        body, target = canonical["Body"], canonical["Target"]
        if "Tree" not in body and "BlackboardAsset" not in body:
            return
        evidence = evidence or _class_evidences(context, [canonical, current])
        blackboard = (_blackboard_path(body["BlackboardAsset"]) if "BlackboardAsset" in body else
                      native.header(context, target, read) if exists else None)
        native.resolve_binding_dependencies(context, blackboard, None)
        if "Tree" in body:
            root = body["Tree"]["Root"]
            if root and evidence(root["Class"].strip()).get("node_family") not in (None, "composite"):
                raise contract_error("/Body/Tree/Root/Class", "InvalidBehaviorTreeNodeClass",
                                     "The exact node class does not belong to this graph role.")
            tree = tree_to_semantic_projection(body["Tree"], property_mapper=_mapper(context, canonical, evidence=evidence),
                existing_tree=_existing_identity(current) if current else None)
            validate_tree_bindings(tree, context=context, blackboard=blackboard, class_evidence=evidence,
                                   authored_tree=body["Tree"])
        elif exists:
            if current is None:
                current = native.readback(context, target, read)
            validate_tree_bindings(current, context=context, blackboard=blackboard, class_evidence=evidence,
                                   target=target, read_handle=read, retained=True)

    def validate(self, document, *, context=None):
        canonical = self.canonicalize(document)
        if context is not None:
            native.validate_properties(context, canonical)
            exists, read, _ = native.resolve(context, canonical["Target"])
            current = native.readback(context, canonical["Target"], read) if exists else None
            self._validate_regions(canonical, context=context, exists=exists, read=read, current=current)

    def _diff_result(self, authored, extracted, *, context=None, validated=False):
        desired, current = self.canonicalize(authored), self.canonicalize(extracted)
        if context is not None and not validated:
            self.validate(desired, context=context)
        # Action describes the requested operation, not the asset's stored state.
        desired.pop("Action", None)
        current.pop("Action", None)
        for view in (desired, current):
            view.setdefault("AssetType", "GenericAsset")
            view.setdefault("Definitions", {})
        declared = desired.get("Properties", {})
        desired["Properties"] = declared
        current["Properties"] = {key: value for key, value in current.get("Properties", {}).items()
                                 if key in declared}
        wanted_body, actual_body = desired["Body"], current["Body"]
        before, after, pointers = {}, {}, {}
        # An omitted existing Tree is not compared: an Update without Tree asks
        # the provider to complete derived selector state on the loaded graph.
        # Report it as skipped so callers cannot certify the request a no-op.
        tree_omitted = "Tree" not in wanted_body and actual_body.get("Tree") is not None
        for region in ("Tree", "BlackboardAsset"):
            if region not in wanted_body:
                actual_body.pop(region, None)
        if "BlackboardAsset" in wanted_body:
            def reference(value):
                value = _blackboard_path(value)
                return {"Kind": "AssetRef", "Path": value} if value is not None else None
            before["/Body/BlackboardAsset"] = reference(actual_body.pop("BlackboardAsset", None))
            after["/Body/BlackboardAsset"] = reference(wanted_body.pop("BlackboardAsset"))
        if "Tree" in wanted_body:
            if context is None:
                raise contract_error("/Body/Tree", "BehaviorTree.DiffContextRequired",
                                     "Semantic tree diff requires readonly class evidence.")
            evidence = _class_evidences(context, [authored, extracted])
            mapper = _mapper(context, [authored, extracted], evidence=evidence)
            wire_mapper = _mapper(context, [authored, extracted], extract=True, evidence=evidence)
            existing = actual_body.pop("Tree", None)
            # Public diff paths point into the document: the authored Tree first, then the
            # extracted one for nodes only the asset has.
            pointers = _tree_pointers(existing, _tree_pointers(wanted_body.get("Tree"), {}))
            wanted = tree_to_semantic_projection(wanted_body.pop("Tree"),
                property_mapper=mapper, existing_tree=existing)
            observed = {"graph_state": "absent", "graph_guid": None, "root": None, "comments": []}
            if existing is not None:
                observed = tree_to_semantic_projection(existing, property_mapper=mapper)
                # The public extractor represents an absent graph without GraphGuid.
                if "GraphGuid" not in existing and not existing["Root"]:
                    observed["graph_state"] = "absent"
            before.update(_flatten_diff_tree(observed, wire_mapper))
            after.update(_flatten_diff_tree(wanted, wire_mapper))
        differences = list(canonical_diff(current, desired))
        payload = {name: [] for name in ("changed", "added", "removed", "unchanged", "skipped", "failed")}
        for path in sorted(before.keys() | after.keys()):
            had, has = path in before, path in after
            same = canonical_equal(before.get(path), after.get(path))
            # Legacy flattened fields compare an absent value as JSON null.
            # GraphGuid presence also carries absent versus new empty graph.
            if same and not (path == "/Body/Tree/GraphGuid" and had != has):
                if had and has and path in ("/Body/BlackboardAsset", "/Body/Tree/GraphGuid"):
                    payload["unchanged"].append({"path": path, "status": "unchanged",
                        "current": deepcopy(before[path]), "desired": deepcopy(after[path])})
                continue
            kind = DiffKind.ADD if not had else DiffKind.REMOVE if not has else DiffKind.REPLACE
            differences.append(Difference(_document_pointer(path, pointers), kind, had, before.get(path), has, after.get(path)))
        differences.sort(key=lambda item: item.path)
        for item in differences:
            if item.path.startswith("/Body/"):
                payload["changed"].append({"path": item.path, "status": "changed",
                    "current": deepcopy(item.before), "desired": deepcopy(item.after)})
            else:
                bucket = {DiffKind.ADD: "added", DiffKind.REMOVE: "removed", DiffKind.REPLACE: "changed"}[item.kind]
                payload[bucket].append({"path": item.path, "status": bucket,
                    "before": deepcopy(item.before), "after": deepcopy(item.after)})
        for name, value in declared.items():
            if name in current["Properties"] and canonical_equal(current["Properties"][name], value):
                payload["unchanged"].append({"name": name, "path": _diff_path("/Properties", name),
                    "status": "unchanged", "value": deepcopy(value)})
        if tree_omitted:
            payload["skipped"].append({"path": "/Body/Tree", "status": "skipped",
                "reason": "Omitted Tree is not compared; Update completes derived selector state."})
        return tuple(differences), payload

    def diff(self, authored, extracted, *, context=None):
        return self._diff_result(authored, extracted, context=context)[0]

    def diff_payload(self, authored, extracted, *, context=None, validated=False):
        """Build the BT public receipt once; runtime may supply prior Verify validation."""
        return self._diff_result(authored, extracted, context=context, validated=validated)[1]

    def preflight(self, document, *, context):
        canonical = self.canonicalize(document)
        native.validate_properties(context, canonical)
        exists, read, _ = native.resolve(context, canonical["Target"])
        action = canonical["Action"]
        if (action == DocumentAction.CREATE.value and exists) or (action == DocumentAction.UPDATE.value and not exists):
            raise contract_error("/Action", "BehaviorTree.TargetActionMismatch", "Action disagrees with target existence.")
        current = native.readback(context, canonical["Target"], read) if exists else None
        self._validate_regions(canonical, context=context, exists=exists, read=read, current=current)
        return ()

    def apply(self, document, *, context, expected_sink=None):
        canonical = self.canonicalize(document)
        target, body = canonical["Target"], canonical["Body"]
        exists, read, mutable = native.resolve(context, target)
        changes = []
        if not exists:
            read, mutable, created = native.create_target(context, target)
            changes.extend(created)
        current = native.readback(context, target, read)
        expected = {"tree": deepcopy(current),
                    "blackboard": native.header(context, target, read),
                    "properties": deepcopy(canonical.get("Properties", {}))}
        if "Tree" in body:
            expected["tree"] = tree_to_semantic_projection(body["Tree"],
                property_mapper=_mapper(context, canonical), existing_tree=_existing_identity(current))
        if "BlackboardAsset" in body:
            expected["blackboard"] = _blackboard_path(body["BlackboardAsset"])
        changes.extend(native.apply_properties(context, canonical, read, mutable))
        asset = native.ai_asset(context, mutable, mutable=True)
        if "Tree" in body:
            desired = expected["tree"]
            graph = _single_handle(_invoke(context, "ai.behavior_tree.graph.create.v1",
                {"behavior_tree_path": native.object_path(target), "graph_name": "AssetDocumentAuthored"}), "graph")
            authored = _invoke(context, "ai.behavior_tree.graph.author_scratch.v1", {"tree": desired},
                              (NamedOpaqueHandleArgument("graph", graph),)).value
            # Only a newly generated graph identity may be supplied by the result.
            if desired["graph_guid"] is None:
                desired["graph_guid"] = authored["graph_guid"]
            if encode_runtime_value(desired) != encode_runtime_value(authored):
                raise contract_error("/Body/Tree", "BehaviorTree.VerificationMismatch",
                                     "Scratch differs from the pre-mutation authored expectation: " + str(_first_tree_mismatch(desired, authored)))
            installed = _invoke(context, "ai.behavior_tree.graph.install.v1",
                {"behavior_tree_path": native.object_path(target),
                 "expected_current_graph_guid_or_null": current["graph_guid"],
                 "rollback_completion_mode": "ValueOrDecoratorMirror"},
                (NamedOpaqueHandleArgument("asset", asset), NamedOpaqueHandleArgument("graph", graph)))
            changes.extend(installed.changed_set)
        if "BlackboardAsset" in body:
            changed = _invoke(context, "ai.behavior_tree.blackboard.set.v1",
                {"behavior_tree_path": native.object_path(target),
                 "blackboard_path_or_null": _blackboard_path(body["BlackboardAsset"])},
                (NamedOpaqueHandleArgument("asset", asset),))
            changes.extend(changed.changed_set)
        if expected_sink is not None:
            self._verify_tree_header(expected, native.readback(context, target, read), native.header(context, target, read))
            expected_sink(deepcopy(expected))
        return tuple(changes)

    def repair(self, document, *, context, expected=None):
        canonical = self.canonicalize(document)
        target = canonical["Target"]
        exists, read, mutable = native.resolve(context, target)
        if not exists:
            raise contract_error("/Target", "BehaviorTree.TargetMissing", "Repair requires the applied target.")
        asset = native.ai_asset(context, mutable, mutable=True)
        blackboard = native.header(context, target, read)
        if expected is not None:
            self._verify_tree_header(expected, native.readback(context, target, read), blackboard)
        selectors = native.selector_inventory(context, target, read)
        if "Tree" not in canonical["Body"]:
            # An overlay leaves existing unbound selectors unauthored. Resolving
            # those rows would apply the full-tree required-None policy to them.
            selectors = [row for row in selectors if row["selected_key_name"] != "None"]
        selected = native.invoke(context, native.SELECTOR_REQUIREMENT,
            {"behavior_tree_path": native.object_path(target), "blackboard_path": blackboard,
             "selectors": selectors},
            (NamedOpaqueHandleArgument("asset", asset),))
        observed = native.readback(context, target, read)
        completed = native.invoke(context, native.REFRESH_REQUIREMENT,
            {"behavior_tree_path": native.object_path(target), "expected_blackboard_path": blackboard,
             "completion_registration": "installed_replacement" if "Tree" in canonical["Body"] else "current_graph",
             "expected_authored_inventory_digest": authored_digest_v2(native.object_path(target), observed)},
            (NamedOpaqueHandleArgument("asset", asset),))
        if expected is not None:
            self._verify_tree_header(expected, native.readback(context, target, read), native.header(context, target, read))
        return tuple(selected.changed_set) + tuple(completed.changed_set)

    def extract(self, *, target, context, asset_handle=None, include_defaults=False):
        if asset_handle is None:
            exists, asset_handle, _ = native.resolve(context, target)
            if not exists:
                raise contract_error("/Target", "BehaviorTree.TargetMissing", "Extraction requires an existing target.")
        observed = native.readback(context, target, asset_handle)
        blackboard = native.header(context, target, asset_handle)
        result = self.create_template(target)
        result["Properties"] = native.extract_properties(context, asset_handle, include_defaults=include_defaults)
        result["Body"] = {"BlackboardAsset": None if blackboard is None else {"Kind": "AssetRef", "Path": blackboard},
                          "Tree": readback_v2_to_tree(observed, property_mapper=_mapper(
                              context, observed, extract=True, omit_defaults=not include_defaults))}
        return self.canonicalize(result)

    @staticmethod
    def _verify_tree_header(expected, tree, blackboard):
        if encode_runtime_value(expected["tree"]) != encode_runtime_value(tree):
            raise contract_error("/Body/Tree", "BehaviorTree.VerificationMismatch", "Complete tree differs from the pre-mutation expectation.")
        if expected["blackboard"] != blackboard:
            raise contract_error("/Body/BlackboardAsset", "BehaviorTree.VerificationMismatch",
                                 f"Blackboard differs from the pre-mutation expectation: expected {expected['blackboard']!r}, observed {blackboard!r}.")

    def verify(self, document, *, context, expected=None):
        canonical = self.canonicalize(document)
        target, body = canonical["Target"], canonical["Body"]
        exists, read, _ = native.resolve(context, target)
        if not exists:
            raise contract_error("/Target", "BehaviorTree.TargetMissing", "Verify requires the applied target.")
        if expected is not None:
            self._verify_tree_header(expected, native.readback(context, target, read), native.header(context, target, read))
        actual_properties = native.extract_properties(context, read, include_defaults=True)
        if any(name not in actual_properties or DocumentCodec().encode(value) != DocumentCodec().encode(actual_properties[name])
               for name, value in canonical.get("Properties", {}).items()):
            raise contract_error("/Properties", "BehaviorTree.VerificationMismatch", "Authored asset properties differ.")
        if "BlackboardAsset" in body and _blackboard_path(body["BlackboardAsset"]) != native.header(context, target, read):
            raise contract_error("/Body/BlackboardAsset", "BehaviorTree.VerificationMismatch", "Authored Blackboard differs.")
        if "Tree" in body:
            observed = native.readback(context, target, read)
            expected = tree_to_semantic_projection(body["Tree"], property_mapper=_mapper(context, canonical),
                                                  existing_tree=_existing_identity(observed))
            if encode_runtime_value(expected) != encode_runtime_value(observed):
                raise contract_error("/Body/Tree", "BehaviorTree.VerificationMismatch", "Complete authored tree projection differs.")
        self._validate_regions(canonical, context=context, exists=True, read=read,
                               current=native.readback(context, target, read))
        return canonical


BEHAVIOR_TREE_PROFILE_META = ProfileMeta("profile.behavior_tree.v1", BEHAVIOR_TREE_ROUTE_ID,
    BEHAVIOR_TREE_CLASS, tuple(RegionMeta(name, "/Body/" + name, RegionOwnership.MANAGED,
        IdentityPolicy.STABLE_ID, OrderPolicy.PRESERVE, DefaultPolicy.ENGINE_DEFAULT,
        OmissionPolicy.PRESERVE, DeletePolicy.EXPLICIT_ONLY, ComparisonPolicy.EXACT)
        for name in ("BlackboardAsset", "Tree")), REQUIRED_PRIMITIVES)


def create_behavior_tree_profile():
    return BehaviorTreeProfile()
