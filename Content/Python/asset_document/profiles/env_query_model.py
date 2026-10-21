"""Environment Query document model: shape checks, the live query (ai.env_query.read.v1), Extract and Diff.

Document Body (profile.env_query.v1, docs/architecture/asset-document-eqs-design.md)::

    "Body": {
      "Root": {"Position": [0, 0]},
      "Options": [                                         # connected options in runtime order (left to right)
        {"Key": "Grid", "Generator": {"Class": "SimpleGrid", "Properties": {"GridSize": {"DefaultValue": 800.0}}},
         "Position": [0, 150],
         "Tests": [{"Key": "Dist", "Class": "Distance", "Properties": {"TestPurpose": "Score"}},
                   {"Key": "Vis", "Class": "Trace", "Enabled": false}]},
        {"Key": "Spare", "Generator": {"Class": "ActorsOfClass"}, "Connected": false, "Position": [600, 150],
         "Tests": []}],
      "Comments": {"Note": {"Text": "fallbacks", "Position": [-40, -80], "Size": [700, 300]}}
    }

* ``Options`` / an option's ``Tests`` / ``Comments`` omitted = keep, present = complete.  ``Root`` omitted = keep.
* Keys are object names (option / test: its graph node once keyed, else its UEnvQueryOption / UEnvQueryTest; comment:
  its comment node); the editor's Undo renames instances, not keyed graph nodes.  A keyless
  entry matches the first unclaimed live entry of the same class (in order); otherwise it is new.
* ``Properties`` are the Details values, sparse over a new node of the class (``defaults``).
"""
from __future__ import annotations

import math
from types import MappingProxyType

from asset_document.diagnostics import AssetDocumentContractError, DiagnosticSeverity, RuntimeDiagnostic
from asset_document.diff.model import Difference, DiffKind
from asset_document.dto import PrimitiveRequirement
from asset_document.profiles import data_table_common as common
from asset_document.regions.primitive_catalog import NamedOpaqueHandleArgument
from asset_document.regions.target_lifecycle import generic_factory_lifecycle, lifecycle_requirements

ROUTE = "env_query_profile"
PREFIX = "EnvQuery"
CLASS = "/Script/AIModule.EnvQuery"
PROFILE_ID = "profile.env_query.v1"
PROVIDER = "ai"
READ = "ai.env_query.read.v1"
EDIT = "ai.env_query.edit.v1"
FINALIZE = "ai.env_query.finalize.v1"
LIFECYCLE = generic_factory_lifecycle(ROUTE)
REQUIREMENTS = (tuple(PrimitiveRequirement(item, 1, ROUTE) for item in (common.CLASS_RESOLVE, common.RESOLVE, READ, EDIT,
                                                                       FINALIZE))
                + lifecycle_requirements(LIFECYCLE))
BY_ID = {item.primitive_id: item for item in REQUIREMENTS}

BODY_KEYS = ("Root", "Options", "Comments")
ROOT_FIELDS = ("Position",)
OPTION_FIELDS = ("Key", "Generator", "Tests", "Position", "Connected", "Comment", "CommentVisible")
GENERATOR_FIELDS = ("Class", "Properties")
TEST_FIELDS = ("Key", "Class", "Enabled", "Properties", "Comment", "CommentVisible")
COMMENT_FIELDS = ("Text", "Position", "Size", "Color", "FontSize", "BubbleVisible", "ColorBubble", "MoveMode", "Details")
COMMENT_PROPERTIES = {"Color": "CommentColor", "FontSize": "FontSize",
                      "BubbleVisible": "bCommentBubbleVisible_InDetailsPanel", "ColorBubble": "bColorCommentBubble",
                      "MoveMode": "MoveMode", "Details": "NodeDetails"}
ENGINE_PREFIX = "/Script/AIModule."
CLASS_PREFIXES = {"generator": "EnvQueryGenerator_", "test": "EnvQueryTest_"}
_INVALID_KEY_CHARACTERS = set("\"' ,/.:|&!~\n\r\t@#(){}[]=;^%$`*?<>\\+")
MAX_DEFAULT_ROWS = 64


def error(path, message, code):
    return common.error(path, message, code)


def escape(name):
    return common.escape(name)


# ---------------------------------------------------------------------------
# Shape
# ---------------------------------------------------------------------------
def class_label(value, kind):
    """Canonical class label: AIModule generators / tests by short name, others by class path."""
    prefix = CLASS_PREFIXES[kind]
    if value.startswith(ENGINE_PREFIX + prefix) and len(value) > len(ENGINE_PREFIX + prefix):
        return value[len(ENGINE_PREFIX + prefix):]
    if "/" not in value and "." not in value and value.startswith(prefix) and len(value) > len(prefix):
        return value[len(prefix):]
    return value


def check_key(key, path, what):
    if type(key) is not str or not key or len(key) >= 1024 or key.lower() == "none" \
            or any(char in _INVALID_KEY_CHARACTERS for char in key):
        raise error(path, f"A {what} key is an object name: letters, digits, '_' and '-' (got {key!r}).",
                    f"{PREFIX}.InvalidKey")
    return key


def _number(value):
    return type(value) in (int, float) and math.isfinite(value)


def _pair(value, path, what, positive=False):
    if type(value) not in (list, tuple) or len(value) != 2 or not all(_number(item) for item in value) \
            or (positive and not all(item > 0 for item in value)):
        raise error(path, f"{what} is [{'width, height' if positive else 'x, y'}]"
                          f"{' (positive numbers)' if positive else ' (numbers)'}.", f"{PREFIX}.Invalid{what}")
    return [int(round(item)) for item in value]


def _fields(entry, path, allowed, what):
    if type(entry) is not dict:
        raise error(path, f"A {what} is an object.", f"{PREFIX}.Invalid{what.title()}")
    for field in entry:
        if field not in allowed:
            raise error(f"{path}/{escape(field)}", f"Unknown {what} field '{field}'. Fields: {', '.join(allowed)}.",
                        f"{PREFIX}.UnknownField")


def _bool(entry, field, path):
    if field in entry and type(entry[field]) is not bool:
        raise error(f"{path}/{field}", f"{field} is a boolean.", f"{PREFIX}.Invalid{field}")


def _string(entry, field, path):
    if field in entry and type(entry[field]) is not str:
        raise error(f"{path}/{field}", f"{field} is a string.", f"{PREFIX}.Invalid{field}")


def _properties(entry, path):
    if "Properties" not in entry:
        return None
    if type(entry["Properties"]) is not dict:
        raise error(f"{path}/Properties", "Properties is an object of Details values.", f"{PREFIX}.InvalidProperties")
    return tidy(thaw(entry["Properties"]))


def _check_test(entry, path, keys):
    _fields(entry, path, TEST_FIELDS, "test")
    if type(entry.get("Class")) is not str or not entry["Class"]:
        raise error(f"{path}/Class", "Class is required: a test class (Distance, Dot, Trace, ... or a class path; "
                    "list_types kind env_query_test).", f"{PREFIX}.MissingField")
    test = {}
    if "Key" in entry:
        check_key(entry["Key"], f"{path}/Key", "test")
        if entry["Key"] in keys:
            raise error(f"{path}/Key", f"'{entry['Key']}' is used twice; a key names one object.", f"{PREFIX}.DuplicateKey")
        keys.add(entry["Key"])
        test["Key"] = entry["Key"]
    test["Class"] = class_label(entry["Class"], "test")
    for field in ("Enabled", "CommentVisible"):
        _bool(entry, field, path)
    _string(entry, "Comment", path)
    properties = _properties(entry, path)
    if properties is not None:
        test["Properties"] = properties
    for field in ("Enabled", "Comment", "CommentVisible"):
        if field in entry:
            test[field] = entry[field]
    return test


def _check_option(entry, path, keys):
    _fields(entry, path, OPTION_FIELDS, "option")
    option = {}
    if "Key" in entry:
        check_key(entry["Key"], f"{path}/Key", "option")
        if entry["Key"] in keys:
            raise error(f"{path}/Key", f"'{entry['Key']}' is used twice; a key names one object.", f"{PREFIX}.DuplicateKey")
        keys.add(entry["Key"])
        option["Key"] = entry["Key"]
    generator = entry.get("Generator")
    if type(generator) is not dict:
        raise error(f"{path}/Generator", "Generator is required: {\"Class\": ..., \"Properties\": {...}} (list_types kind "
                    "env_query_generator lists the classes).", f"{PREFIX}.MissingField")
    _fields(generator, f"{path}/Generator", GENERATOR_FIELDS, "generator")
    if type(generator.get("Class")) is not str or not generator["Class"]:
        raise error(f"{path}/Generator/Class", "Generator.Class is required: a generator class (SimpleGrid, "
                    "ActorsOfClass, Donut, ... or a class path).", f"{PREFIX}.MissingField")
    option["Generator"] = {"Class": class_label(generator["Class"], "generator")}
    properties = _properties(generator, f"{path}/Generator")
    if properties is not None:
        option["Generator"]["Properties"] = properties
    if "Tests" in entry:
        if type(entry["Tests"]) not in (list, tuple):
            raise error(f"{path}/Tests", "Tests is an array of tests in run order.", f"{PREFIX}.InvalidTests")
        option["Tests"] = [_check_test(test, f"{path}/Tests/{index}", keys) for index, test in enumerate(entry["Tests"])]
    if "Position" in entry:
        option["Position"] = _pair(entry["Position"], f"{path}/Position", "Position")
    for field in ("Connected", "CommentVisible"):
        _bool(entry, field, path)
    _string(entry, "Comment", path)
    for field in ("Connected", "Comment", "CommentVisible"):
        if field in entry:
            option[field] = entry[field]
    return option


def check_body(body):
    """Shape of the Body; returns the canonical Body (class labels, integer positions)."""
    out = {}
    keys = set()
    if "Root" in body:
        _fields(body["Root"], "/Body/Root", ROOT_FIELDS, "root")
        out["Root"] = {}
        if "Position" in body["Root"]:
            out["Root"]["Position"] = _pair(body["Root"]["Position"], "/Body/Root/Position", "Position")
    if "Options" in body:
        if type(body["Options"]) not in (list, tuple):
            raise error("/Body/Options", "Options is an array of options (generator + tests).", f"{PREFIX}.InvalidOptions")
        out["Options"] = [_check_option(entry, f"/Body/Options/{index}", keys)
                          for index, entry in enumerate(body["Options"])]
    if "Comments" in body:
        comments = body["Comments"]
        if type(comments) is not dict:
            raise error("/Body/Comments", "Comments is an object of comment key -> comment box.", f"{PREFIX}.InvalidComments")
        out["Comments"] = {}
        for key, entry in comments.items():
            path = f"/Body/Comments/{escape(key)}"
            check_key(key, path, "comment")
            _fields(entry, path, COMMENT_FIELDS, "comment")
            if type(entry.get("Text")) is not str:
                raise error(f"{path}/Text", "Text is required (a string).", f"{PREFIX}.MissingField")
            comment = {field: (value if field in ("Text", "Position", "Size") else tidy(thaw(value)))
                       for field, value in entry.items()}
            if "Position" in entry:
                comment["Position"] = _pair(entry["Position"], f"{path}/Position", "Position")
            if "Size" in entry:
                comment["Size"] = _pair(entry["Size"], f"{path}/Size", "Size", positive=True)
            out["Comments"][key] = comment
    return out


# ---------------------------------------------------------------------------
# Values
# ---------------------------------------------------------------------------
def thaw(value):
    if isinstance(value, MappingProxyType) or isinstance(value, dict):
        return {key: thaw(item) for key, item in value.items()}
    if type(value) in (list, tuple):
        return [thaw(item) for item in value]
    return value


def tidy(value):
    """Numbers stored as float32 (EQS values are floats) as the shortest decimal that round-trips to the same float32."""
    if type(value) is float and math.isfinite(value) and abs(value) < 3.4e38:
        stored = common._float32(value)
        if stored == value:
            for digits in range(1, 10):
                candidate = float(f"{stored:.{digits}g}")
                if common._float32(candidate) == stored:
                    return candidate
        return value
    if type(value) is dict:
        return {key: tidy(item) for key, item in value.items()}
    if type(value) is list:
        return [tidy(item) for item in value]
    return value


# ---------------------------------------------------------------------------
# Live query
# ---------------------------------------------------------------------------
class Calls:
    """ai.env_query.* against one query (editor handles handed off to the AI provider)."""

    def __init__(self, context, read, mutable=None):
        self.context = context
        self.read_handle = context.handoff(source_handle=read, target_provider_id=PROVIDER, access="read_only")
        self.write_handle = (context.handoff(source_handle=mutable, target_provider_id=PROVIDER, access="mutable")
                             if mutable is not None else None)
        self.changes = []

    def _invoke(self, primitive, values, handle, path="/Body", paths=None):
        outcome = common.invoke(self.context, BY_ID, primitive, {"schema_version": 1, **values},
                                (NamedOpaqueHandleArgument("asset", handle),), path=path, paths=paths, prefix=PREFIX)
        if handle is self.write_handle:
            self.changes.extend(outcome.changed_set)
        return outcome.value if type(outcome.value) is dict else {}

    def live(self, defaults=(), dependents=False, paths=None):
        """The live query; ``defaults``: (kind, class label) pairs whose new-node values to read (``paths`` maps one to
        the document path its class came from)."""
        rows = [{"kind": kind, "class": label} for kind, label in dict.fromkeys(defaults)]
        records = []
        batches = [rows[start:start + MAX_DEFAULT_ROWS] for start in range(0, len(rows), MAX_DEFAULT_ROWS)] or [[]]
        for number, batch in enumerate(batches):
            offset = 0

            def where(primitive_path, batch=batch):
                parts = [part for part in (primitive_path or "").split("/") if part]
                if len(parts) >= 2 and parts[0] == "defaults" and parts[1].isdigit() and int(parts[1]) < len(batch):
                    row = batch[int(parts[1])]
                    return (paths or {}).get((row["kind"], row["class"]), "/Body")
                return "/Body"
            while True:
                values = {"offset": offset, "defaults": batch}
                if dependents and number == 0:
                    values["dependents"] = True
                page = self._invoke(READ, values, self.read_handle, paths=where)
                page_records = page.get("records", [])
                # Only the defaults / dependents of later batches: the query itself was read with the first.
                records += page_records if number == 0 else [item for item in page_records
                                                             if item.get("kind") in ("default",)]
                offset = page.get("next_offset", -1)
                if type(offset) is not int or offset < 0:
                    break
        return Live(records)

    def edit(self, ops):
        """ops: [(op, document path, field paths)] -> the created object names, one per op."""
        created = []
        for chunk in common.chunk_ops([(op, (path, fields)) for op, path, fields in ops]):
            maps = [where for _, where in chunk]

            def where(primitive_path, maps=maps):
                parts = [part for part in (primitive_path or "").split("/") if part]
                if len(parts) >= 2 and parts[0] == "ops" and parts[1].isdigit() and int(parts[1]) < len(maps):
                    base, fields = maps[int(parts[1])]
                    rest = parts[2:]
                    if rest and rest[0] in fields:
                        mapped = fields[rest[0]]
                        if type(mapped) is dict:  # per member (e.g. a comment's Details name -> its document field)
                            if len(rest) > 1 and rest[1] in mapped:
                                return mapped[rest[1]] + "".join(f"/{part}" for part in rest[2:])
                            return base
                        return mapped + "".join(f"/{part}" for part in rest[1:])
                    return base
                return "/Body"
            value = self._invoke(EDIT, {"ops": [op for op, _ in chunk]}, self.write_handle, paths=where)
            created += list(value.get("created", []))
        return created

    def finalize(self):
        return self._invoke(FINALIZE, {}, self.write_handle)


class LiveOption:
    def __init__(self, record):
        self.name = record.get("name", "")
        self.label = class_label(record.get("class", ""), "generator")
        self.properties = tidy(thaw(record.get("properties", {})))
        self.position = list(record.get("position", [0, 0]))
        self.connected = record.get("connected") is True
        self.runtime_index = record.get("runtime_index", -1)
        self.comment = record.get("comment", "")
        self.comment_visible = record.get("comment_visible") is True
        self.tests = list(record.get("tests", []))
        self.error = record.get("error", "")
        self.item_type = record.get("item_type", "")


class LiveTest:
    def __init__(self, record):
        self.name = record.get("name", "")
        self.option = record.get("option", "")
        self.index = record.get("index", 0)
        self.label = class_label(record.get("class", ""), "test")
        self.enabled = record.get("enabled") is not False
        self.properties = tidy(thaw(record.get("properties", {})))
        self.comment = record.get("comment", "")
        self.comment_visible = record.get("comment_visible") is True
        self.supported = record.get("supported") is not False
        self.weight = record.get("weight_pct", -1)


class Live:
    """One read of the query: options (graph order), tests, comments, defaults."""

    def __init__(self, records):
        self.graph = False
        self.open_in_editor = False
        self.root = [0, 0]
        self.params = []
        self.messages = []
        self.runtime_order = []
        self.options = []
        self.option_by_name = {}
        self.tests = {}
        self.comments = {}
        self.defaults = {}
        self.labels = {}
        self.comment_defaults = {}
        self.dependents = []
        for record in records:
            if type(record) is not dict:
                continue
            kind = record.get("kind")
            if kind == "query":
                self.graph = record.get("graph") is True
                self.open_in_editor = record.get("open_in_editor") is True
                self.root = list(record.get("root", [0, 0]))
                self.params = [dict(item) for item in record.get("params", [])]
                self.messages = [dict(item) for item in record.get("messages", []) if type(item) is not str]
                self.runtime_order = list(record.get("runtime_order", []))
            elif kind == "option":
                option = LiveOption(record)
                self.options.append(option)
                self.option_by_name[option.name] = option
            elif kind == "test":
                test = LiveTest(record)
                self.tests[test.name] = test
            elif kind == "comment":
                self.comments[record.get("name", "")] = {
                    "text": record.get("text", ""), "position": list(record.get("position", [0, 0])),
                    "size": list(record.get("size", [0, 0])), "properties": tidy(thaw(record.get("properties", {})))}
            elif kind == "default":
                node = record.get("node", "")
                properties = tidy(thaw(record.get("properties", {})))
                label = class_label(record.get("class", ""), node)
                self.defaults[(node, label)] = properties
                self.labels[(node, label)] = label
                self.labels[(node, class_label(record.get("request", ""), node))] = label
            elif kind == "comment_default":
                self.comment_defaults = tidy(thaw(record.get("properties", {})))
            elif kind == "dependent":
                self.dependents.append(dict(record))

    def find(self, name):
        """The live option / test / comment a name addresses (object names compare case-insensitively)."""
        folded = name.casefold()
        for table in (self.option_by_name, self.tests, self.comments):
            for key, value in table.items():
                if key.casefold() == folded:
                    return key, value
        return None, None


# ---------------------------------------------------------------------------
# Extract
# ---------------------------------------------------------------------------
def runtime_sorted(live):
    """Connected options in runtime order, then the others left to right."""
    connected = [option for option in live.options if option.connected]
    others = [option for option in live.options if not option.connected]
    connected.sort(key=lambda option: (option.runtime_index if option.runtime_index >= 0 else 1 << 30, option.position[0]))
    others.sort(key=lambda option: option.position[0])
    return connected + others


def test_entry(test):
    entry = {"Key": test.name, "Class": test.label}
    if test.properties:
        entry["Properties"] = test.properties
    if not test.enabled:
        entry["Enabled"] = False
    if test.comment:
        entry["Comment"] = test.comment
    if test.comment_visible:
        entry["CommentVisible"] = True
    return entry


def option_entry(live, option):
    generator = {"Class": option.label}
    if option.properties:
        generator["Properties"] = option.properties
    entry = {"Key": option.name, "Generator": generator, "Position": list(option.position),
             "Tests": [test_entry(live.tests[name]) for name in option.tests if name in live.tests]}
    if not option.connected:
        entry["Connected"] = False
    if option.comment:
        entry["Comment"] = option.comment
    if option.comment_visible:
        entry["CommentVisible"] = True
    return entry


def comment_entry(live, comment):
    entry = {"Text": comment["text"], "Position": list(comment["position"]), "Size": list(comment["size"])}
    defaults = live.comment_defaults
    for field, name in COMMENT_PROPERTIES.items():
        value = comment["properties"].get(name)
        if name in comment["properties"] and (name not in defaults or not common.same(value, defaults[name], True)):
            entry[field] = value
    return entry


def extract_body(live):
    return {"Root": {"Position": list(live.root)},
            "Options": [option_entry(live, option) for option in runtime_sorted(live)],
            "Comments": {name: comment_entry(live, comment) for name, comment in live.comments.items()}}


def with_defaults(body, defaults):
    """The Body with every Details value (new-node defaults filled in) of each generator and test."""
    options = []
    for option in body.get("Options", ()):
        option = dict(option)
        generator = dict(option["Generator"])
        full = {**defaults.get(("generator", generator["Class"]), {}), **generator.get("Properties", {})}
        if full:
            generator["Properties"] = full
        option["Generator"] = generator
        tests = []
        for test in option.get("Tests", ()):
            full = {**defaults.get(("test", test["Class"]), {}), **test.get("Properties", {})}
            test = dict(test)
            if full:
                test["Properties"] = full
            tests.append(test)
        option["Tests"] = tests
        options.append(option)
    return {**body, "Options": options}


def classes_of(body):
    """(kind, class label) pairs a Body names."""
    out = []
    for option in body.get("Options", ()):
        out.append(("generator", option["Generator"]["Class"]))
        for test in option.get("Tests", ()):
            out.append(("test", test["Class"]))
    return out


def classes_of_live(live):
    return [("generator", option.label) for option in live.options] + [("test", test.label) for test in live.tests.values()]


def class_paths(body):
    """(kind, class label) -> the document path of its first Class (unknown-class errors point there)."""
    out = {}
    for index, option in enumerate(body.get("Options", ())):
        out.setdefault(("generator", option["Generator"]["Class"]), f"/Body/Options/{index}/Generator/Class")
        for position, test in enumerate(option.get("Tests", ())):
            out.setdefault(("test", test["Class"]), f"/Body/Options/{index}/Tests/{position}/Class")
    return out


def read_live(calls, authored=None, dependents=False):
    """(live query, extracted Body) with the defaults of every class the query and the ``authored`` Body name."""
    probe = calls.live(defaults=())
    wanted = classes_of_live(probe) + (classes_of(authored) if authored else [])
    live = (calls.live(defaults=wanted, dependents=dependents, paths=class_paths(authored or {}))
            if wanted or dependents else probe)
    return live, extract_body(live)


def relabel(body, labels):
    """The Body with each class label as the editor names it (a class given by path or another spelling)."""
    if "Options" not in body:
        return body
    options = []
    for option in body["Options"]:
        option = dict(option)
        generator = dict(option["Generator"])
        generator["Class"] = labels.get(("generator", generator["Class"]), generator["Class"])
        option["Generator"] = generator
        if "Tests" in option:
            option["Tests"] = [{**test, "Class": labels.get(("test", test["Class"]), test["Class"])}
                               for test in option["Tests"]]
        options.append(option)
    return {**body, "Options": options}


# ---------------------------------------------------------------------------
# Matching (Diff and Apply)
# ---------------------------------------------------------------------------
def match_options(options, live_options):
    """Per authored option, the name of the live option it addresses (or None: new).  Keyed entries by name; keyless
    ones take the first unclaimed live option of the same generator class (in runtime order)."""
    by_fold = {option.name.casefold(): option for option in live_options}
    claimed = set()
    out = [None] * len(options)
    for index, option in enumerate(options):
        if "Key" in option and option["Key"].casefold() in by_fold:
            name = by_fold[option["Key"].casefold()].name
            out[index] = name
            claimed.add(name)
    for index, option in enumerate(options):
        if "Key" in option:
            continue
        for candidate in live_options:
            if candidate.name not in claimed and candidate.label == option["Generator"]["Class"]:
                out[index] = candidate.name
                claimed.add(candidate.name)
                break
    return out


def match_tests(options, matched, live):
    """Per authored option, per test: the live test it addresses (or None).  Keyed by name anywhere in the query (a
    test listed under another option is moved there); keyless: the first unclaimed test of the same class in the
    option it matched."""
    by_fold = {name.casefold(): name for name in live.tests}
    claimed = set()
    out = [[None] * len(option.get("Tests", ())) for option in options]
    for index, option in enumerate(options):
        for position, test in enumerate(option.get("Tests", ())):
            if "Key" in test and test["Key"].casefold() in by_fold:
                name = by_fold[test["Key"].casefold()]
                out[index][position] = name
                claimed.add(name)
    for index, option in enumerate(options):
        source = live.option_by_name.get(matched[index]) if matched[index] else None
        for position, test in enumerate(option.get("Tests", ())):
            if "Key" in test or source is None:
                continue
            for name in source.tests:
                candidate = live.tests.get(name)
                if candidate is not None and name not in claimed and candidate.label == test["Class"]:
                    out[index][position] = name
                    claimed.add(name)
                    break
    return out


# ---------------------------------------------------------------------------
# Diff
# ---------------------------------------------------------------------------
_MISSING = object()


def _difference(changes, path, authored, live, authored_present=True, live_present=True):
    changes.append(Difference(path, DiffKind.REPLACE, authored_present, authored if authored_present else None,
                              live_present, live if live_present else None))


def properties_diff(changes, path, authored, live, defaults, exact):
    """Properties sparse over ``defaults`` (an omitted property is its default); a struct value is an edit of the current
    one (members it omits keep their value), so it compares with the live value in its shape."""
    for name in list(authored) + [name for name in live if name not in authored]:
        default = defaults.get(name, _MISSING) if defaults is not None else _MISSING
        left = authored.get(name, default)
        right = live.get(name, default)
        if name in authored and right is not _MISSING:
            right = common.struct_as_authored(left, right)
        if left is _MISSING or right is _MISSING or not common.same(left, right, exact):
            _difference(changes, f"{path}/{escape(name)}", None if left is _MISSING else left,
                        None if right is _MISSING else right, left is not _MISSING, right is not _MISSING)


class _ExtractedLive:
    """A Live built from an extracted Body (Diff compares two documents)."""

    def __init__(self, body):
        self.options = []
        self.option_by_name = {}
        self.tests = {}
        self.runtime_order = []
        for entry in body.get("Options", ()):
            option = LiveOption({"name": entry.get("Key", ""), "class": entry["Generator"]["Class"],
                                 "properties": entry["Generator"].get("Properties", {}),
                                 "position": entry.get("Position", [0, 0]),
                                 "connected": entry.get("Connected", True), "comment": entry.get("Comment", ""),
                                 "comment_visible": entry.get("CommentVisible", False),
                                 "tests": [test.get("Key", "") for test in entry.get("Tests", ())]})
            option.label = entry["Generator"]["Class"]
            self.options.append(option)
            self.option_by_name[option.name] = option
            if option.connected:
                self.runtime_order.append(option.name)
            for index, test_value in enumerate(entry.get("Tests", ())):
                test = LiveTest({"name": test_value.get("Key", ""), "option": option.name, "index": index,
                                 "class": test_value["Class"], "enabled": test_value.get("Enabled", True),
                                 "properties": test_value.get("Properties", {}), "comment": test_value.get("Comment", ""),
                                 "comment_visible": test_value.get("CommentVisible", False)})
                test.label = test_value["Class"]
                self.tests[test.name] = test


def _node_fields_diff(changes, path, authored, live_entry, fields):
    for field, default in fields:
        if authored.get(field, default) != live_entry.get(field, default):
            _difference(changes, f"{path}/{field}", authored.get(field, default), live_entry.get(field, default))


def body_diff(authored, extracted, exact, defaults=None):
    """Does the query (its extracted Body) hold what the document Body authors?  ``defaults``: {(kind, label): new-node
    Details} the sparse Properties compare over (None: both sides compare as written)."""
    changes = []
    live_root = extracted.get("Root", {}).get("Position")
    if "Root" in authored and "Position" in authored["Root"] and authored["Root"]["Position"] != live_root:
        _difference(changes, "/Body/Root/Position", authored["Root"]["Position"], live_root)
    if "Options" in authored:
        live = _ExtractedLive(extracted)
        options = authored["Options"]
        matched = match_options(options, live.options)
        tests = match_tests(options, matched, live)
        entries = {entry.get("Key"): entry for entry in extracted.get("Options", ())}
        for index, option in enumerate(options):
            path = f"/Body/Options/{index}"
            name = matched[index]
            if name is None:
                changes.append(Difference(path, DiffKind.ADD, True, option, False, None))
                continue
            live_entry = entries.get(name, {})
            label = option["Generator"]["Class"]
            if label != live_entry.get("Generator", {}).get("Class"):
                _difference(changes, f"{path}/Generator/Class", label, live_entry.get("Generator", {}).get("Class"))
            else:
                properties_diff(changes, f"{path}/Generator/Properties", option["Generator"].get("Properties", {}),
                                live_entry.get("Generator", {}).get("Properties", {}),
                                None if defaults is None else defaults.get(("generator", label)), exact)
            if "Position" in option and option["Position"] != live_entry.get("Position"):
                _difference(changes, f"{path}/Position", option["Position"], live_entry.get("Position"))
            _node_fields_diff(changes, path, option, live_entry, (("Connected", True), ("Comment", ""),
                                                                   ("CommentVisible", False)))
            if "Tests" not in option:
                continue
            live_tests = {test.get("Key"): test for entry in extracted.get("Options", ()) for test in entry.get("Tests", ())}
            wanted = []
            for position, test in enumerate(option["Tests"]):
                test_path = f"{path}/Tests/{position}"
                test_name = tests[index][position]
                if test_name is None:
                    changes.append(Difference(test_path, DiffKind.ADD, True, test, False, None))
                    continue
                wanted.append(test_name)
                live_test = live_tests.get(test_name, {})
                if test["Class"] != live_test.get("Class"):
                    _difference(changes, f"{test_path}/Class", test["Class"], live_test.get("Class"))
                    continue
                properties_diff(changes, f"{test_path}/Properties", test.get("Properties", {}),
                                live_test.get("Properties", {}),
                                None if defaults is None else defaults.get(("test", test["Class"])), exact)
                _node_fields_diff(changes, test_path, test, live_test, (("Enabled", True), ("Comment", ""),
                                                                        ("CommentVisible", False)))
            have = [test.get("Key") for test in live_entry.get("Tests", ())]
            if wanted != have:
                _difference(changes, f"{path}/Tests", wanted, have)
        claimed = {name for name in matched if name is not None}
        for entry in extracted.get("Options", ()):
            if entry.get("Key") not in claimed:
                changes.append(Difference(f"/Body/Options/{escape(entry.get('Key', ''))}", DiffKind.REMOVE, True, None,
                                          True, entry))
        order = [matched[index] for index, option in enumerate(options)
                 if matched[index] is not None and option.get("Connected", True)]
        live_order = [name for name in live.runtime_order if name in set(order)]
        if order != live_order:
            _difference(changes, "/Body/Options", order, live_order)
    if "Comments" in authored:
        current = extracted.get("Comments", {})
        for key, entry in authored["Comments"].items():
            path = f"/Body/Comments/{escape(key)}"
            if key not in current:
                changes.append(Difference(path, DiffKind.ADD, True, entry, False, None))
                continue
            for field in COMMENT_FIELDS:
                if field in entry and not common.same(entry[field], current[key].get(field), exact):
                    _difference(changes, f"{path}/{field}", entry[field], current[key].get(field))
        for key in current:
            if key not in authored["Comments"]:
                changes.append(Difference(f"/Body/Comments/{escape(key)}", DiffKind.REMOVE, True, None, True, current[key]))
    return changes


# ---------------------------------------------------------------------------
# Validation messages
# ---------------------------------------------------------------------------
def message_diagnostics(messages, code_by_severity, path_of):
    out = []
    for message in messages:
        severity = str(message.get("severity", "")).casefold()
        code = code_by_severity.get(severity)
        if code is None:
            continue
        out.append(RuntimeDiagnostic(path=path_of(message), code=code, message=str(message.get("message", "")),
                                     severity=DiagnosticSeverity.WARNING))
    return out


__all__ = ("AssetDocumentContractError", "BODY_KEYS", "BY_ID", "CLASS", "Calls", "Live", "PREFIX", "PROFILE_ID",
           "READ", "EDIT", "FINALIZE", "REQUIREMENTS", "ROUTE", "LIFECYCLE", "body_diff", "check_body", "class_label",
           "extract_body", "match_options", "match_tests", "read_live")
