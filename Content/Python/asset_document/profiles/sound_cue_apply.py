# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""SoundCue Apply: the Sound Cue editor's own operations through the shared graph primitives.

Order (design §4.2): cue Details (generic property admission), deletes, spawns (each renamed to its key), input
pin counts (Add Input / Delete Input), the links that change (breaks first, then connects: every intermediate graph
is part of the final one, so only a cycle the document itself has is refused), node Details (container
``sound_node``), comment boxes, layout, then one ``graph.finalize.v1`` (compile + validation).  Errors the cue did
not have before fail the apply (``SoundCue.ValidationFailed``; Core rolls the package back); errors it already had
come back as ``SoundCue.ValidationError`` warnings, warnings as ``SoundCue.ValidationWarning``.  A document the cue
already holds changes nothing (no finalize either).
"""
from __future__ import annotations

from asset_document.diagnostics import (
    AssetDocumentContractError, DiagnosticContext, DiagnosticSeverity, RuntimeDiagnostic, with_diagnostics,
)
from asset_document.profiles import data_table_common as common
from asset_document.profiles import sound_cue_model as model
from asset_document.profiles.sound_cue_model import PREFIX, error, escape

_MISSING = object()
# Column layout of a new node without Position: left of its first parent, one row per input slot.
COLUMN = 300
ROW = 150


def _node_path(key):
    return f"/Body/Nodes/{escape(key)}"


def _comment_path(key):
    return f"/Body/Comments/{escape(key)}"


def _remap(failure, path, code=None, message=None):
    source = failure.diagnostic
    return AssetDocumentContractError(RuntimeDiagnostic(path=path, code=code or source.code,
                                                        message=message or source.message,
                                                        severity=source.severity, context=source.context))


def _slots_to_remove(linked, wanted):
    """Indices of `linked` to delete so that the kept slots, in order, match `wanted` in the most positions."""
    count, keep = len(linked), len(wanted)
    best = [[0] * (keep + 1) for _ in range(count + 1)]  # best[i][j]: matches of linked[i:] kept as wanted[j:]
    for i in range(count - 1, -1, -1):
        for j in range(keep, -1, -1):
            if count - i < keep - j:
                best[i][j] = -1
                continue
            options = []
            if j < keep and best[i + 1][j + 1] >= 0:
                options.append(best[i + 1][j + 1] + (1 if linked[i] == wanted[j] and linked[i] is not None else 0))
            if count - i > keep - j:
                options.append(best[i + 1][j])
            best[i][j] = max(options) if options else 0
    removed, j = [], 0
    for i in range(count):
        take = j < keep and best[i + 1][j + 1] >= 0 and (
            best[i + 1][j + 1] + (1 if linked[i] == wanted[j] and linked[i] is not None else 0)) == best[i][j]
        if take:
            j += 1
        else:
            removed.append(i)
    return removed


def _point(value):
    return {"x": value[0], "y": value[1]}


class _Apply:
    def __init__(self, document, read, mutable, context):
        self.document = document
        self.body = document.get("Body", {})
        self.context = context
        self.read_handle = read
        self.mutable_handle = mutable
        self.calls = model.Calls(context, read, mutable)
        self.diagnostics = []

    # -- checks before any change ---------------------------------------
    def check(self, live):
        body = self.body
        model.check_references(body, live.nodes)
        paths = {}
        wanted = []
        for key, node in body.get("Nodes", {}).items():
            pair = (node["Class"], len(node["Inputs"]) if "Inputs" in node else None)
            wanted.append(pair)
            paths.setdefault(pair, f"{_node_path(key)}/Class")
        model.ensure_defaults(self.calls, wanted, paths)
        levels = model.quality_levels()
        for key, node in body.get("Nodes", {}).items():
            path = _node_path(key)
            facts = model.class_facts(node["Class"])
            inputs = model.desired_inputs(node)
            count = len(inputs)
            if count < facts["min"] or count > facts["max"]:
                allowed = (f"exactly {facts['min']}" if facts["min"] == facts["max"]
                           else f"{facts['min']} to {facts['max']}")
                extra = (f" (one per project quality level: {', '.join(levels)})"
                         if node["Class"] == "QualityLevel" and levels else "")
                raise error(f"{path}/Inputs", f"{node['Class']} takes {allowed} input(s){extra}; the document gives "
                            f"{count}.", f"{PREFIX}.ChildCount")
            existing = live.nodes.get(key)
            already = (existing is not None and existing.extras.get("class") == "Delay" and not live.inputs(key))
            if node["Class"] == "Delay" and count == 0 and not already:
                # USoundNodeDelay::GetDuration reads input 0 unchecked: the cue's next compile would crash the editor,
                # so a Delay losing its slot is refused before any edit (one that already has none is finalize's
                # pre-existing error).
                raise error(path, "A Delay needs its input slot (its duration reads input 0); give it one input.",
                            f"{PREFIX}.ValidationFailed")
            defaults = model.node_defaults(node["Class"], count) or {}
            for name, value in node.get("Properties", {}).items():
                if name not in defaults:
                    raise error(f"{path}/Properties/{escape(name)}", f"{node['Class']} has no editable property "
                                f"'{name}'. Properties: {', '.join(sorted(defaults)) or 'none'}.",
                                f"{PREFIX}.UnknownProperty")
            per_child = model.PER_CHILD.get(node["Class"])
            if per_child is not None and per_child[0] in node.get("Properties", {}):
                name, offset = per_child
                value = node["Properties"][name]
                expected = max(count + offset, 0)
                if type(value) is not list or len(value) != expected:
                    raise error(f"{path}/Properties/{name}", f"{name} has one entry per input"
                                f"{' but the last' if offset else ''}: {expected} for {count} input(s) (got "
                                f"{len(value) if type(value) is list else type(value).__name__}).",
                                f"{PREFIX}.ChildArrayLength")

    # -- writes ----------------------------------------------------------
    def write(self, primitive, values, path):
        return self.calls.call(primitive, values, path=path, write=True)

    def remove(self, node, path):
        self.write("graph.node.remove.v1", {"node_guid": node.guid}, path)

    def spawn(self, action_key, key, where, path):
        values = {"graph": {"graph_guid": self.calls.graph_info().guid}, "action_key": action_key}
        if where is not None:
            values["position"] = _point(where)
        try:
            result = self.write("graph.node.spawn.v1", values, path)
        except AssetDocumentContractError as failure:
            if failure.diagnostic.code == "Graph.ActionNotFound":
                raise _remap(failure, f"{path}/Class", f"{PREFIX}.UnknownNodeClass") from None
            raise
        guid = str(result.get("node_guid", "")).upper()
        self.write("graph.node.rename.v1", {"node_guid": guid, "name": key}, path)
        return guid

    def new_positions(self, live):
        """Position of each new node without one: left of its first parent (Root first, then the document order)."""
        nodes = self.body.get("Nodes", {})
        parents = {}
        if "Root" in self.body and self.body["Root"]["Input"] is not None:
            parents.setdefault(self.body["Root"]["Input"], (None, 0))
        for key, node in nodes.items():
            for index, child in enumerate(node.get("Inputs", ())):
                if child is not None:
                    parents.setdefault(child, (key, index))
        placed = {}
        orphans = [0]

        def where(key, depth=0):
            if key in placed:
                return placed[key]
            node = nodes.get(key)
            if node is not None and "Position" in node:
                placed[key] = node["Position"]
            elif key in live.nodes and model.position(live.nodes[key]) is not None and node is not None \
                    and node["Class"] == live.nodes[key].extras.get("class"):
                placed[key] = model.position(live.nodes[key])
            elif key in parents and depth < 64:
                parent, index = parents[key]
                if parent is None:
                    base = model.position(live.root) if live.root is not None else None
                    base = self.body["Root"].get("Position", base) or [0, 0]
                else:
                    base = where(parent, depth + 1)
                placed[key] = [base[0] - COLUMN, base[1] + index * ROW]
            else:
                placed[key] = [-COLUMN, ROW * (len(nodes) + orphans[0])]
                orphans[0] += 1
            return placed[key]
        return where

    def structure(self, live):
        """Deletes, spawns and renames; True when anything changed."""
        body = self.body
        changed = False
        nodes = body.get("Nodes")
        comments = body.get("Comments")
        if nodes is not None:
            for key, node in live.nodes.items():
                if key not in nodes or nodes[key]["Class"] != node.extras.get("class"):
                    self.remove(node, _node_path(key))
                    changed = True
        if comments is not None:
            for key, node in live.comments.items():
                if key not in comments:
                    self.remove(node, _comment_path(key))
                    changed = True
        where = self.new_positions(live)
        for key, node in (nodes or {}).items():
            if key in live.nodes and live.nodes[key].extras.get("class") == node["Class"]:
                continue
            facts = model.class_facts(node["Class"])
            self.spawn(model.NODE_ACTION_PREFIX + facts["class_path"], key, where(key), _node_path(key))
            changed = True
        for key, comment in (comments or {}).items():
            if key in live.comments:
                continue
            self.spawn(model.COMMENT_ACTION, key, comment.get("Position", [0, 0]), _comment_path(key))
            changed = True
        return changed

    def pin_counts(self, live):
        changed = False
        for key, node in self.body.get("Nodes", {}).items():
            current = live.nodes[key]
            wanted = len(model.desired_inputs(node))
            pins = [pin for pin in current.pins if pin.direction == "input"]
            path = f"{_node_path(key)}/Inputs"
            for _ in range(wanted - len(pins)):
                self.write("graph.node.dynamic_pin.add.v1", {"node_guid": current.guid}, path)
                changed = True
            if len(pins) > wanted:
                # Delete Input on the slots whose removal keeps the most links (a slot deleted in the middle shifts
                # the later ones, as the editor's Delete Input does); ties remove the last slots.
                linked = live.inputs(key)
                for index in sorted(_slots_to_remove(linked, model.desired_inputs(node)), reverse=True):
                    self.write("graph.node.dynamic_pin.remove.v1", {"node_guid": current.guid,
                                                                    "pin_name": pins[index].name}, path)
                    changed = True
        return changed

    def links(self, live):
        """Breaks every slot whose link changes, then connects the new links."""
        body = self.body
        slots = []  # (parent node, pin address, linked child key, wanted child key, document path)
        if "Root" in body and live.root is not None:
            pin = next((pin for pin in live.root.pins if pin.direction == "input"), None)
            if pin is not None and live.root_input() != body["Root"]["Input"]:
                slots.append((live.root, pin.name, live.root_input(), body["Root"]["Input"], "/Body/Root/Input"))
        for key, node in body.get("Nodes", {}).items():
            current = live.inputs(key)
            for index, child in enumerate(model.desired_inputs(node)):
                if index < len(current) and current[index] != child:
                    slots.append((live.nodes[key], f"#{index}", current[index], child, f"{_node_path(key)}/Inputs/{index}"))
        for parent, pin, linked, _, path in slots:
            if linked is not None:
                self.write("graph.pin.break.v1", {"from": {"node_guid": parent.guid, "pin_name": pin,
                                                           "direction": "input"}}, path)
        for parent, pin, _, child, path in slots:
            if child is None:
                continue
            source = live.nodes[child]
            output = next((p for p in source.pins if p.direction == "output"), None)
            if output is None:
                raise error(path, f"'{child}' has no output (only the Output node plays a cue).", f"{PREFIX}.InvalidInput")
            try:
                self.write("graph.pin.connect.v1", {
                    "from": {"node_guid": source.guid, "pin_name": output.name, "direction": "output"},
                    "to": {"node_guid": parent.guid, "pin_name": pin, "direction": "input"}}, path)
            except AssetDocumentContractError as failure:
                reason = " ".join(str(item.value) for item in failure.diagnostic.context if item.key == "reason")
                if failure.diagnostic.code == "Graph.ConnectDisallowed" and "loop" in (reason + failure.diagnostic.message).lower():
                    raise _remap(failure, path, f"{PREFIX}.ConnectionCycle", (
                        f"Connecting '{child}' here makes a loop (a node would play itself); the Sound Cue editor "
                        "refuses it too.")) from None
                raise _remap(failure, path) from None
        return bool(slots)

    def node_details(self, live):
        changed = False
        for key, node in self.body.get("Nodes", {}).items():
            current = live.nodes[key]
            path = _node_path(key)
            count = len(model.desired_inputs(node))
            defaults = model.node_defaults(node["Class"], count) or {}
            have = {**defaults, **model.thaw(current.extras.get("properties", {}))}
            want = {**defaults, **node.get("Properties", {})}
            for name, value in want.items():
                # A struct value is an edit (the members it lists): the same when those members are.
                if name in have and common.same(value, common.struct_as_authored(value, have[name]), False):
                    continue
                where = f"{path}/Properties/{escape(name)}"
                try:
                    self.write("graph.node.property.set.v1", {"node_guid": current.guid, "container": "sound_node",
                                                              "property_path": name, "value": value}, where)
                except AssetDocumentContractError as failure:
                    raise _remap(failure, where) from None
                changed = True
            layout = {}
            if "Position" in node and node["Position"] != model.position(current):
                layout["position"] = _point(node["Position"])
            if node.get("Comment", "") != current.comment:
                layout["comment"] = node.get("Comment", "")
            if node.get("CommentVisible", False) != (current.extras.get("comment_bubble_visible") is True):
                layout["comment_bubble_visible"] = node.get("CommentVisible", False)
            if layout:
                self.write("graph.node.layout.set.v1", {"node_guid": current.guid, **layout}, path)
                changed = True
        root = self.body.get("Root", {})
        if "Position" in root and live.root is not None and root["Position"] != model.position(live.root):
            self.write("graph.node.layout.set.v1", {"node_guid": live.root.guid, "position": _point(root["Position"])},
                       "/Body/Root/Position")
            changed = True
        return changed

    def comments(self, live):
        changed = False
        defaults = model.comment_defaults()
        for key, comment in self.body.get("Comments", {}).items():
            current = live.comments[key]
            path = _comment_path(key)
            have = model.comment_entry(current)
            for field, name in model.COMMENT_PROPERTIES.items():
                want = comment.get(field, defaults.get(name, _MISSING))
                if want is _MISSING:
                    continue
                current_value = have.get(field, defaults.get(name, _MISSING))
                if current_value is not _MISSING and common.same(want, current_value, False):
                    continue
                where = f"{path}/{field}"
                try:
                    self.write("graph.node.property.set.v1", {"node_guid": current.guid, "container": "node",
                                                              "property_path": name, "value": want}, where)
                except AssetDocumentContractError as failure:
                    raise _remap(failure, where) from None
                changed = True
            if "Size" in comment and comment["Size"] != have.get("Size"):
                self.write("graph.node.property.set.v1", {"node_guid": current.guid, "container": "node",
                                                          "property_path": "Size", "value": comment["Size"]},
                           f"{path}/Size")
                changed = True
            layout = {}
            if comment["Text"] != current.comment:
                layout["comment"] = comment["Text"]
            if "Position" in comment and comment["Position"] != model.position(current):
                layout["position"] = _point(comment["Position"])
            if layout:
                self.write("graph.node.layout.set.v1", {"node_guid": current.guid, **layout}, path)
                changed = True
        return changed

    def finalize(self, before, live):
        """graph.finalize.v1; new errors fail the apply at the node's path."""
        try:
            result = self.write("graph.finalize.v1", {}, "/Body")
            messages = result.get("messages", [])
        except AssetDocumentContractError as failure:
            if failure.diagnostic.code != "Graph.FinalizeFailed":
                raise
            messages = next((item.value for item in failure.diagnostic.context if item.key == "messages"), [])
        messages = [item for item in messages if type(item) is dict]

        def path_of(message):
            return live.path_of(str(message.get("node_guid", "")).upper())

        def name_of(message):
            key = live.key_of(str(message.get("node_guid", "")).upper())
            return key or ""
        existing = {(item.get("message"), item.get("node", "")) for item in before if item.get("severity") == "error"}
        new = [item for item in messages if str(item.get("severity", "")).casefold() == "error"
               and (item.get("message"), name_of(item)) not in existing]
        if new:
            raise AssetDocumentContractError(RuntimeDiagnostic(
                path=path_of(new[0]), code=f"{PREFIX}.ValidationFailed",
                message="The cue is not valid: " + " | ".join(str(item.get("message", "")) for item in new[:8]),
                severity=DiagnosticSeverity.ERROR,
                context=(DiagnosticContext("messages", [{"path": path_of(item), "message": str(item.get("message", ""))}
                                                        for item in new]),)))
        self.diagnostics += model.message_diagnostics(
            messages, {"error": f"{PREFIX}.ValidationError", "warning": f"{PREFIX}.ValidationWarning"}, path_of)

    def run(self, properties_region):
        state = self.calls.read()
        if state.get("editable") is False:
            raise error("/Class", "This cue is made from a Sound Cue Template (its graph is derived from the template's "
                        "settings) and is not edited as a graph.", f"{PREFIX}.UnsupportedAsset")
        before = [item for item in state.get("messages", []) if type(item) is dict]
        properties = self.document.get("Properties", {})
        self.calls.changes.extend(properties_region.apply(properties, self.read_handle, self.mutable_handle,
                                                          self.context))
        live = self.calls.live()
        self.check(live)
        changed = self.structure(live)
        if changed:
            live = self.calls.live()
        if self.pin_counts(live):
            changed = True
            live = self.calls.live()
        if self.links(live):
            changed = True
            live = self.calls.live()
        changed = self.node_details(live) | changed
        changed = self.comments(live) | changed
        if changed:
            self.finalize(before, self.calls.live())
        return with_diagnostics(self.calls.changes, self.diagnostics)


def apply(document, read, mutable, context, properties_region):
    return _Apply(document, read, mutable, context).run(properties_region)


__all__ = ("apply",)
