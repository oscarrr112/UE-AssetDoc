# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""EnvQuery Apply: the Environment Query editor's own operations through ai.env_query.edit.v1.

Order (docs/architecture/asset-document-eqs-design.md §3): checks (classes, properties, option order, open editor),
new options (palette action from Root, placed and renamed to their key), tests added / moved per option in order,
removals (tests, then options and comment boxes), renames of keys that were held, connections, Details of generators
and tests (Details panel admission, EditDefaultsOnly included), test toggles, layout (option positions, Root, node
comments), comment boxes, then one ai.env_query.finalize.v1 (the editor's graph rebuild of the runtime options).
Errors the query did not have before fail the apply (``EnvQuery.ValidationFailed``; Core rolls the package back);
errors it already had come back as ``EnvQuery.ValidationError`` warnings, warnings as ``EnvQuery.ValidationWarning``.
A document the query already holds changes nothing (no finalize either).
"""
from __future__ import annotations

from asset_document.diagnostics import (
    AssetDocumentContractError, DiagnosticContext, DiagnosticSeverity, RuntimeDiagnostic, with_diagnostics,
)
from asset_document.profiles import data_table_common as common
from asset_document.profiles import env_query_model as model
from asset_document.profiles.env_query_model import PREFIX, error, escape

_MISSING = object()
COLUMN = 300  # horizontal step between options, as the editor lays out missing options
ROW = 100     # options sit this far below Root


def _option_path(index):
    return f"/Body/Options/{index}"


def _test_path(index, position):
    return f"/Body/Options/{index}/Tests/{position}"


def _comment_path(key):
    return f"/Body/Comments/{escape(key)}"


def _warning(path, code, message):
    return RuntimeDiagnostic(path=path, code=code, message=message, severity=DiagnosticSeverity.WARNING)


class _Apply:
    def __init__(self, document, read, mutable, context):
        self.document = document
        self.read_handle = read
        self.mutable_handle = mutable
        self.body = document.get("Body", {})
        self.context = context
        self.calls = model.Calls(context, read, mutable)
        self.diagnostics = []
        self.ops = []
        self.changed = False

    # -- ops -------------------------------------------------------------
    def op(self, request, path, **fields):
        """Queues an edit op (``fields``: op field -> document path of its errors); returns its index in the next
        flush's result."""
        self.ops.append((request, path, fields))
        return len(self.ops) - 1

    def flush(self):
        """Runs the queued ops; the object name each one created (or "")."""
        if not self.ops:
            return []
        ops, self.ops = self.ops, []
        self.changed = True
        return self.calls.edit(ops)

    # -- checks ----------------------------------------------------------
    def check(self, live):
        options = self.body.get("Options", ())
        for index, option in enumerate(options):
            path = _option_path(index)
            self.check_properties(live, "generator", option["Generator"], f"{path}/Generator")
            for position, test in enumerate(option.get("Tests", ())):
                self.check_properties(live, "test", test, _test_path(index, position))

    def check_properties(self, live, kind, entry, path):
        defaults = live.defaults.get((kind, entry["Class"]))
        if defaults is None:
            raise error(f"{path}/Class", f"'{entry['Class']}' is not a {kind} class of the Environment Query editor.",
                        f"{PREFIX}.UnknownClass")
        for name in entry.get("Properties", {}):
            if name not in defaults:
                raise error(f"{path}/Properties/{escape(name)}", f"{entry['Class']} has no Details property '{name}'. "
                            f"Properties: {', '.join(sorted(defaults)) or 'none'}.", f"{PREFIX}.UnknownProperty")

    # -- layout ------------------------------------------------------------
    def positions(self, live, matched):
        """Final position of every authored option: as written, else the live one when it keeps the runtime order,
        else derived (evenly between its neighbours, or a column right of the previous option)."""
        options = self.body["Options"]
        root = self.body.get("Root", {}).get("Position", live.root)
        out = [None] * len(options)
        connected = [index for index, option in enumerate(options) if option.get("Connected", True)]
        fixed = {index: options[index]["Position"] for index in connected if "Position" in options[index]}
        previous = None
        for index in connected:
            if index in fixed:
                where = fixed[index]
                if previous is not None and where[0] < previous[1][0]:
                    raise error(f"{_option_path(index)}/Position", (
                        f"Option {index} sits left of option {previous[0]} (x {where[0]} < {previous[1][0]}): the editor "
                        "runs connected options left to right, so this Position contradicts the Options order. Move it "
                        "right or reorder Options."), f"{PREFIX}.OptionOrder")
                if previous is not None and where[0] == previous[1][0]:
                    self.diagnostics.append(_warning(f"{_option_path(index)}/Position", f"{PREFIX}.OptionOrderTie", (
                        f"Options {previous[0]} and {index} share x {where[0]}; the editor orders them by x, so their "
                        "order may change on the next edit. Give them different x.")))
                previous = (index, where)
        # Runs of options without Position between two fixed ones (or the ends).
        run = []
        last = None  # (x, y) of the previous placed connected option
        for index in connected + [None]:
            if index is not None and index not in fixed:
                run.append(index)
                continue
            following = fixed[index] if index is not None else None
            placed = []
            low = last[0] if last is not None else None
            for member in run:
                name = matched[member]
                current = live.option_by_name[name].position if name in live.option_by_name else None
                if current is not None and (low is None or current[0] > low) \
                        and (following is None or current[0] < following[0]):
                    placed.append(list(current))
                    low = current[0]
                    continue
                placed.append(None)
            # Fill the gaps evenly between the kept neighbours.
            position = 0
            while position < len(placed):
                if placed[position] is not None:
                    position += 1
                    continue
                end = position
                while end < len(placed) and placed[end] is None:
                    end += 1
                left = placed[position - 1] if position > 0 else last
                right = placed[end] if end < len(placed) else following
                count = end - position
                for step in range(count):
                    if left is None and right is None:
                        x, y = root[0] + COLUMN * step, root[1] + ROW
                    elif left is None:
                        x, y = right[0] - COLUMN * (count - step), right[1]
                    elif right is None:
                        x, y = left[0] + COLUMN * (step + 1), left[1]
                    else:
                        x = left[0] + (right[0] - left[0]) * (step + 1) // (count + 1)
                        y = left[1]
                    placed[position + step] = [int(x), int(y)]
                position = end
            for member, where in zip(run, placed):
                out[member] = where
            run = []
            if index is not None:
                out[index] = list(following)
                last = following
            elif placed:
                last = placed[-1]
        # Unconnected options: as written, else where they are, else a column right of everything.
        right_most = max([where[0] for where in out if where is not None]
                         + [option.position[0] for option in live.options] + [root[0] - COLUMN])
        for index, option in enumerate(options):
            if out[index] is not None:
                continue
            if "Position" in option:
                out[index] = list(option["Position"])
                continue
            name = matched[index]
            if name in live.option_by_name:
                out[index] = list(live.option_by_name[name].position)
                continue
            right_most += COLUMN
            out[index] = [right_most, root[1] + ROW]
        # Equal x of derived connected options (no room between fixed neighbours).
        for before, after in zip(connected, connected[1:]):
            if out[before][0] >= out[after][0] and not (before in fixed and after in fixed):
                self.diagnostics.append(_warning(f"{_option_path(after)}/Position", f"{PREFIX}.OptionOrderTie", (
                    f"No room between the options around option {after} to keep the Options order; give it a Position.")))
        return out

    # -- structure -------------------------------------------------------
    def live_names(self, live):
        return {name.casefold() for name in list(live.option_by_name) + list(live.tests) + list(live.comments)}

    def run_options(self, live):
        """Options and tests (the Options section is present): adds, moves, removals and key renames.  Returns the
        object name of each authored option and test."""
        options = self.body["Options"]
        matched = model.match_options(options, live.options)
        replaced = set()
        for index, option in enumerate(options):
            name = matched[index]
            if name is not None and live.option_by_name[name].label != option["Generator"]["Class"]:
                replaced.add(index)  # a generator class change: the editor deletes the node and places a new one
        tests = model.match_tests(options, matched, live)
        swapped = set()  # tests whose class changes: the editor deletes the node and adds a new one
        for index, option in enumerate(options):
            for position, test in enumerate(option.get("Tests", ())):
                name = tests[index][position]
                if name is not None and live.tests[name].label != test["Class"]:
                    swapped.add(name)
                    tests[index][position] = None
        self.option_positions = self.positions(live, matched)
        held = self.live_names(live)
        renames = []  # (current name, key, path, (option index, test position or None))

        # New options (and the replacements of class changes), placed and wired as the palette does.
        targets = list(matched)
        added = {}
        for index, option in enumerate(options):
            if matched[index] is not None and index not in replaced:
                continue
            key = option.get("Key", "")
            path = _option_path(index)
            added[index] = self.op({"op": "add_option", "class": option["Generator"]["Class"],
                                    "name": key if key and key.casefold() not in held else "",
                                    "position": self.option_positions[index], "connect": option.get("Connected", True)},
                                   path, **{"class": f"{path}/Generator/Class", "name": f"{path}/Key"})
        created = self.flush()
        for index, slot in added.items():
            targets[index] = created[slot]
            key = options[index].get("Key")
            if key and created[slot].casefold() != key.casefold():
                renames.append((created[slot], key, f"{_option_path(index)}/Key", (index, None)))

        # Tests: per option, in order (an existing test is moved, a new one placed by Add Test).
        layout = {name: list(option.tests) for name, option in live.option_by_name.items()}
        for name in targets:
            layout.setdefault(name, [])
        where = {test: option for option, names in layout.items() for test in names}
        claimed = {name for row in tests for name in row if name is not None}
        new_tests = {}
        for index, option in enumerate(options):
            target = targets[index]
            if "Tests" in option:
                plan = [(tests[index][position], test, _test_path(index, position))
                        for position, test in enumerate(option["Tests"])]
            elif index in replaced:
                # A replaced option without Tests keeps the tests it had.
                plan = [(name, None, _option_path(index)) for name in layout.get(matched[index], []) if name not in claimed]
            else:
                continue
            for position, (name, test, path) in enumerate(plan):
                if name is not None:
                    if where.get(name) == target and layout[target].index(name) == position:
                        continue
                    self.op({"op": "move_test", "name": name, "option": target, "index": position}, path)
                    layout[where[name]].remove(name)
                    layout[target].insert(min(position, len(layout[target])), name)
                    where[name] = target
                    continue
                key = test.get("Key", "")
                slot = self.op({"op": "add_test", "option": target, "class": test["Class"],
                                "name": key if key and key.casefold() not in held else "", "index": position},
                               path, **{"class": f"{path}/Class", "name": f"{path}/Key"})
                marker = ("new", index, position)
                layout[target].insert(min(position, len(layout[target])), marker)
                where[marker] = target
                new_tests[(index, position)] = slot
        created = self.flush()
        for (index, position), slot in new_tests.items():
            name = created[slot]
            tests[index][position] = name
            target = where.pop(("new", index, position))
            layout[target][layout[target].index(("new", index, position))] = name
            where[name] = target
            key = options[index]["Tests"][position].get("Key")
            if key and name.casefold() != key.casefold():
                renames.append((name, key, f"{_test_path(index, position)}/Key", (index, position)))

        # Removals: tests no option keeps, then the options the document drops or replaces.
        kept = set(targets)
        for index, option in enumerate(options):
            if "Tests" not in option:
                continue
            wanted = set(tests[index])
            for name in list(layout[targets[index]]):
                if name not in claimed and name not in wanted:
                    self.op({"op": "remove", "name": name}, f"{_option_path(index)}/Tests")
                    layout[targets[index]].remove(name)
        for name in swapped:
            if name in where and name in layout[where[name]]:
                self.op({"op": "remove", "name": name}, "/Body/Options")
                layout[where[name]].remove(name)
        for name in live.option_by_name:
            if name not in kept:
                path = next((_option_path(index) for index in replaced if matched[index] == name), "/Body/Options")
                self.op({"op": "remove", "name": name}, path)
        self.flush()

        # Keys that were held by removed objects.
        for name, key, path, (index, position) in renames:
            slot = self.op({"op": "rename", "name": name, "key": key}, path, key=path)
            renamed = self.flush()[slot]
            if position is None:
                targets[index] = renamed
            else:
                tests[index][position] = renamed
        return targets, tests

    def details(self, live, kind, name, entry, current, path):
        defaults = live.defaults.get((kind, entry["Class"]), {})
        have = {**defaults, **(current or {})}
        for property_name, value in {**defaults, **entry.get("Properties", {})}.items():
            if property_name in have and common.same(value, common.struct_as_authored(value, have[property_name]), False):
                continue
            where = f"{path}/Properties/{escape(property_name)}"
            self.op({"op": "set_property", "name": name, "property": property_name, "value": value}, where,
                    value=where, property=where)

    def node_layout(self, name, entry, current, path, position=None):
        layout = {}
        if position is not None and (current is None or list(position) != list(current.position)):
            layout["position"] = list(position)
        if entry.get("Comment", "") != (current.comment if current is not None else ""):
            layout["comment"] = entry.get("Comment", "")
        if entry.get("CommentVisible", False) != (current.comment_visible if current is not None else False):
            layout["comment_visible"] = entry.get("CommentVisible", False)
        if layout:
            self.op({"op": "layout", "name": name, **layout}, path)

    def run_values(self, live, targets, tests):
        """Connections, Details, test toggles and node layout of the authored options."""
        for index, option in enumerate(self.body["Options"]):
            name = targets[index]
            current = live.option_by_name.get(name)
            path = _option_path(index)
            if current is not None and current.connected != option.get("Connected", True):
                self.op({"op": "connect", "name": name, "connected": option.get("Connected", True)}, f"{path}/Connected")
            self.details(live, "generator", name, option["Generator"], current.properties if current else None,
                         f"{path}/Generator")
            self.node_layout(name, option, current, path, self.option_positions[index])
            for position, test in enumerate(option.get("Tests", ())):
                test_name = tests[index][position]
                live_test = live.tests.get(test_name)
                test_path = _test_path(index, position)
                self.details(live, "test", test_name, test, live_test.properties if live_test else None, test_path)
                if test.get("Enabled", True) != (live_test.enabled if live_test is not None else True):
                    self.op({"op": "set_enabled", "name": test_name, "enabled": test.get("Enabled", True)},
                            f"{test_path}/Enabled")
                self.node_layout(test_name, test, live_test, test_path)

    def run_root(self, live):
        root = self.body.get("Root", {})
        if "Position" in root and list(root["Position"]) != list(live.root):
            self.op({"op": "layout", "name": "", "position": root["Position"]}, "/Body/Root/Position")

    def run_comments(self, live):
        """Comment boxes (the Comments section is present): removals, Create Comment, then text / size / Details /
        position of each."""
        comments = self.body.get("Comments")
        if comments is None:
            return
        for key in live.comments:
            if key not in comments:
                self.op({"op": "remove_comment", "name": key}, _comment_path(key))
        self.flush()
        held = self.live_names(live) - {key.casefold() for key in live.comments if key not in comments}
        added = {}
        for key, comment in comments.items():
            if key in live.comments:
                continue
            path = _comment_path(key)
            value = {"op": "add_comment", "name": key if key.casefold() not in held else "", "text": comment["Text"],
                     "position": comment.get("Position", [0, 0])}
            if "Size" in comment:
                value["size"] = comment["Size"]
            added[key] = self.op(value, path, text=f"{path}/Text", position=f"{path}/Position", size=f"{path}/Size")
        created = self.flush()
        names = {key: key for key in comments}
        for key, slot in added.items():
            if created[slot].casefold() != key.casefold():
                path = _comment_path(key)
                slot = self.op({"op": "rename", "name": created[slot], "key": key}, path, key=path)
                names[key] = self.flush()[slot]
        defaults = live.comment_defaults
        for key, comment in comments.items():
            current = live.comments.get(key)
            path = _comment_path(key)
            if current is not None:
                have = model.comment_entry(live, current)
            else:  # as Create Comment made it
                have = {"Text": comment["Text"], "Position": comment.get("Position", [0, 0]),
                        "Size": comment.get("Size", [400, 100])}
            values = {"op": "comment", "name": names[key]}
            if comment["Text"] != have.get("Text"):
                values["text"] = comment["Text"]
            if "Size" in comment and comment["Size"] != have.get("Size"):
                values["size"] = comment["Size"]
            properties = {}
            fields = {}
            for field, name in model.COMMENT_PROPERTIES.items():
                want = comment.get(field, defaults.get(name, _MISSING))
                current_value = have.get(field, defaults.get(name, _MISSING))
                if want is _MISSING or (current_value is not _MISSING and common.same(want, current_value, False)):
                    continue
                properties[name] = want
                fields[name] = f"{path}/{field}"
            if properties:
                values["properties"] = properties
            if len(values) > 2:
                self.op(values, path, text=f"{path}/Text", size=f"{path}/Size", properties=fields)
            if "Position" in comment and comment["Position"] != have.get("Position"):
                self.op({"op": "layout", "name": names[key], "position": comment["Position"]}, f"{path}/Position")


    # -- finalize --------------------------------------------------------
    def path_of(self, message, names):
        return names.get(message.get("test") or "", names.get(message.get("option") or "", "/Body"))

    def finalize(self, before, names):
        result = self.calls.finalize()
        messages = [item for item in result.get("messages", []) if type(item) is dict]
        existing = {(item.get("message"), item.get("option", ""), item.get("test", ""))
                    for item in before.messages if item.get("severity") == "error"}
        new = [item for item in messages if item.get("severity") == "error"
               and (item.get("message"), item.get("option", ""), item.get("test", "")) not in existing]
        if new:
            raise AssetDocumentContractError(RuntimeDiagnostic(
                path=self.path_of(new[0], names), code=f"{PREFIX}.ValidationFailed",
                message="The query is not valid: " + " | ".join(str(item.get("message", "")) for item in new[:8]),
                severity=DiagnosticSeverity.ERROR,
                context=(DiagnosticContext("messages", [{"path": self.path_of(item, names),
                                                         "message": str(item.get("message", ""))} for item in new]),)))
        self.diagnostics += model.message_diagnostics(
            messages, {"error": f"{PREFIX}.ValidationError", "warning": f"{PREFIX}.ValidationWarning"},
            lambda item: self.path_of(item, names))
        params = [dict(item) for item in result.get("params", []) if type(item) is dict]
        if params != before.params:
            dependents = self.calls.live(dependents=True).dependents
            if dependents:
                names_list = ", ".join(item.get("asset") or item.get("package", "") for item in dependents[:8])
                self.diagnostics.append(_warning("/Body", f"{PREFIX}.DependentParams", (
                    f"The query's parameters changed ({_params_text(before.params)} -> {_params_text(params)}); "
                    f"assets that use it may bind the old ones: {names_list}.")))

    # -- run ---------------------------------------------------------------
    def run(self, properties_region):
        live, _ = model.read_live(self.calls, self.body)
        self.calls.changes.extend(properties_region.apply(self.document.get("Properties", {}), self.read_handle,
                                                          self.mutable_handle, self.context))
        self.body = model.relabel(self.body, live.labels)
        self.check(live)
        before = live
        names = {}
        if "Options" in self.body:
            targets, tests = self.run_options(live)
            if self.changed:
                live, _ = model.read_live(self.calls, self.body)
            self.run_values(live, targets, tests)
            for index, name in enumerate(targets):
                names[name] = _option_path(index)
                for position, test in enumerate(tests[index]):
                    names[test] = _test_path(index, position)
        self.run_root(live)
        self.flush()
        self.run_comments(live)
        self.flush()
        if self.changed:
            self.finalize(before, names)
        return with_diagnostics(self.calls.changes, self.diagnostics)


def _params_text(params):
    return ", ".join(f"{item.get('name')}: {item.get('type')}" for item in params) or "none"


def apply(document, read, mutable, context, properties_region):
    return _Apply(document, read, mutable, context).run(properties_region)


__all__ = ("apply",)
