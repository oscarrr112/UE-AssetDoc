"""Registered Material / Material Function Profiles (profile.material.v1, profile.material_function.v1,
profile.material_function_material_layer.v1, profile.material_function_material_layer_blend.v1).

Document (docs/architecture/asset-document-material-matrix.md)::

    "Body": {
      "Expressions": {                                    # every expression and comment, by key (object name)
        "Albedo": {"Class": "TextureSampleParameter2D",  # engine classes by short name; others by class path
                   "Position": [-600, 0],
                   "Properties": {"ParameterName": "Albedo", "Group": "Surface"},   # differences from a new node
                   "Inputs": {"Coordinates": "Tiling"}},  # input pin -> "<key>" or "<key>.<output>"
        "Tiling": {"Class": "TextureCoordinate", "Position": [-900, 0], "Properties": {"UTiling": 4, "VTiling": 4}},
        "Rough": {"Class": "ScalarParameter", "Position": [-600, 300],
                  "Properties": {"ParameterName": "Roughness", "DefaultValue": 0.5}}
      },
      "Outputs": {"BaseColor": "Albedo.RGB", "Roughness": "Rough"},   # material inputs (materials only)
      "ParameterGroups": {"Surface": 0}                              # parameter group sort priorities (materials only)
    },
    "Properties": {"MaterialDomain": "MD_Surface", "BlendMode": "BLEND_Opaque", "TwoSided": false, ...}

* ``Expressions`` is complete when present: an expression that is not listed is deleted (its links are broken
  as the Material Editor's delete does), a new key creates an expression, a different ``Class`` on a key
  replaces the expression.  ``Properties`` and ``Inputs`` of an expression are complete too: an omitted property
  has the value of a new node of that class, an omitted input is not connected.  ``Position`` omitted keeps the
  node where it is (a new node is placed left of the material node).
* ``Outputs`` (the main material node's pins, by attribute name: BaseColor, Metallic, Specular, Roughness,
  Anisotropy, EmissiveColor, Opacity, OpacityMask, Normal, Tangent, WorldPositionOffset, Displacement,
  SubsurfaceColor, CustomData0, CustomData1, AmbientOcclusion, Refraction, PixelDepthOffset, ShadingModel,
  SurfaceThickness, FrontMaterial, MaterialAttributes, ...) is complete when present.  A number
  (``"Roughness": 0.15``) is a constant: the Material Editor wires a Constant node to the pin for that, so the
  apply makes (or updates) the Constant expression ``Constant_<pin>`` and wires it; extract writes such a node
  (a Constant with only its value, feeding only that pin) back as the number.
* Keys are object names, which UE compares without case: two keys that differ only by case are refused
  (``Material.DuplicateKey``).
* ``ParameterGroups`` is the material's parameter group sort list (Details: Parameter Group Data) and is
  complete when present.
* A section that is omitted is left as it is.
* Sources: ``"<key>"`` is output 0 of an expression whose first output has no name; ``"<key>.<output>"``
  names an output (``"<key>.#<n>"`` for an unnamed or duplicate output).  Inputs without a unique name are
  ``"#<n>"``.  Expression references in properties (a named reroute usage's ``Declaration``, a composite's
  pin bases) are expression keys.
* Apply edits the graph the Material Editor's way (create / delete / connect / Details edit) and then
  recompiles once (the editor's Apply).  The apply fails (and is rolled back) when the recompile reports
  errors the material did not have before; errors it already had come back as warnings.  Shader compilation
  is not awaited.
* Material-level settings (Details panel) go through the generic property admission (``Properties``).

Writes are covered by Core's package snapshot (existing target) or the factory rollback (created target);
preflight therefore returns ``SNAPSHOT_COVERED``.
"""
from __future__ import annotations

import dataclasses
import math

from asset_document.diagnostics import DiagnosticSeverity, RuntimeDiagnostic, with_diagnostics
from asset_document.diff.model import Difference, DiffKind
from asset_document.meta import (
    ComparisonPolicy, DefaultPolicy, DeletePolicy, IdentityPolicy, OmissionPolicy, OrderPolicy, ProfileMeta,
    RegionMeta, RegionOwnership,
)
from asset_document.mutation_intent_projection import SNAPSHOT_COVERED
from asset_document.profile_runtime import RuntimeProfileRegistration, RuntimeTargetPolicy
from asset_document.profiles import data_table_common as common
from asset_document.regions.primitive_catalog import NamedOpaqueHandleArgument
from asset_document.regions.target_lifecycle import generic_factory_lifecycle, lifecycle_requirements

MATERIAL_CLASS = "/Script/Engine.Material"
MATERIAL_FUNCTION_CLASS = "/Script/Engine.MaterialFunction"
MATERIAL_LAYER_CLASS = "/Script/Engine.MaterialFunctionMaterialLayer"
MATERIAL_LAYER_BLEND_CLASS = "/Script/Engine.MaterialFunctionMaterialLayerBlend"
PROFILES = {
    MATERIAL_CLASS: "profile.material.v1",
    MATERIAL_FUNCTION_CLASS: "profile.material_function.v1",
    MATERIAL_LAYER_CLASS: "profile.material_function_material_layer.v1",
    MATERIAL_LAYER_BLEND_CLASS: "profile.material_function_material_layer_blend.v1",
}
READ = "data.material.read.v1"
EDIT = "data.material.edit.v1"
COMPILE = "data.material.compile.v1"
PREFIX = "Material"
MATERIAL_BODY_KEYS = ("Expressions", "Outputs", "ParameterGroups")
FUNCTION_BODY_KEYS = ("Expressions",)
EXPRESSION_KEYS = ("Class", "Position", "Properties", "Inputs")
LIFECYCLE = generic_factory_lifecycle(common.ROUTE)
REQUIREMENTS = (common.requirements(common.CLASS_RESOLVE, common.RESOLVE, READ, EDIT, COMPILE)
                + lifecycle_requirements(LIFECYCLE))
BY_ID = {item.primitive_id: item for item in REQUIREMENTS}
# The graph is authored in the Body, never as generic Properties.
BODY_OWNED = {name: "Body.Expressions" for name in (
    "Expressions", "EditorComments", "ExpressionCollection", "FunctionExpressions", "FunctionEditorComments",
    "ExpressionExecBegin", "ExpressionExecEnd")}
BODY_OWNED["ParameterGroupData"] = "Body.ParameterGroups"
# UMaterial's editor-only container of the graph: material inputs (Outputs) and the expression collection.
BODY_OWNED["EditorOnlyData"] = "Body.Expressions"
# Editor state that is not material content: the Content Browser thumbnail (preview shape / camera).
NOT_AUTHORED = ("ThumbnailInfo",)
# UE object names cannot contain these (INVALID_OBJECTNAME_CHARACTERS); '#' marks pin indices.
_INVALID_KEY_CHARACTERS = set("\"' ,/.:|&!~\n\r\t@#(){}[]=;^%$`")
# Placement of new expressions authored without Position: columns left of the material node.
_NEW_COLUMN_X = -400
_NEW_COLUMN_STEP = -300
_NEW_ROW_STEP = 150
_NEW_PER_COLUMN = 8
# A number in Outputs is the Constant expression "Constant_<pin>" wired to that pin (the Material Editor has no
# constant on a material input pin; one places a Constant node).
IMPLICIT_CONSTANT_PREFIX = "Constant_"
IMPLICIT_CONSTANT_CLASS = "Constant"
IMPLICIT_CONSTANT_VALUE = "R"
# Class label -> {"properties": every authored property of a new node, "pins": ...}; filled by reads
# (the values an omitted property means; constant for a class within an editor session).
_CLASS_DEFAULTS = {}


def is_material(class_path):
    return class_path == MATERIAL_CLASS


def body_keys(class_path):
    return MATERIAL_BODY_KEYS if is_material(class_path) else FUNCTION_BODY_KEYS


def class_label(value):
    """Canonical class label: native expression classes (/Script/<Module>.MaterialExpression<Name>) by short
    name <Name>; other classes by class path."""
    marker = ".MaterialExpression"
    if value.startswith("/Script/") and marker in value and value.index(marker) + len(marker) < len(value):
        return value[value.index(marker) + len(marker):]
    if not value.startswith("/") and value.startswith("MaterialExpression") and len(value) > len("MaterialExpression"):
        return value[len("MaterialExpression"):]
    return value


# ---------------------------------------------------------------------------
# Document shape
# ---------------------------------------------------------------------------
def _key_problem(key):
    if type(key) is not str or not key or len(key) >= 1024:
        return "A key is a non-empty object name."
    if key.lower() == "none" or any(char in _INVALID_KEY_CHARACTERS for char in key):
        return (f"'{key}' is not a valid key: an expression key is its object name (letters, digits, '_', '-'; "
                "no spaces or '.', '/', ':', '#').")
    return None


def _source(value, path, keys):
    if type(value) is not str or not value:
        raise common.error(path, "A source is \"<key>\" or \"<key>.<output>\" (null or omitted: not connected).",
                           "Material.InvalidSource")
    key = value.split(".", 1)[0]
    if keys is not None and key not in keys:
        raise common.error(path, f"No expression '{key}' in Body.Expressions."
                           f"{common.describe_candidates(key, sorted(keys), 'Expressions')}", "Material.ExpressionNotFound")
    return value


def _integral(item):
    return type(item) is int or (type(item) is float and item.is_integer())


def _number(value):
    return type(value) in (int, float) and math.isfinite(value)


def implicit_constant_key(output):
    return IMPLICIT_CONSTANT_PREFIX + output


def _output_source(name, value, path, keys):
    """An output's source, or a number (a constant: the expression Constant_<pin>)."""
    if type(value) is bool or (type(value) in (int, float) and not _number(value)):
        raise common.error(path, "An output is \"<key>\", \"<key>.<output>\" or a finite number (a constant).",
                           "Material.InvalidSource")
    if not _number(value):
        return _source(value, path, keys)
    key = implicit_constant_key(name)
    problem = _key_problem(key)
    if problem is not None:
        raise common.error(path, f"A constant on '{name}' is the expression '{key}': {problem}", "Material.InvalidKey")
    clash = next((other for other in (keys or ()) if other.lower() == key.lower()), None)
    if clash is not None:
        raise common.error(path, f"A constant on '{name}' is the expression '{key}', and Body.Expressions has "
                           f"'{clash}' already. Wire that expression (\"{name}\": \"{clash}\") or remove it.",
                           "Material.DuplicateKey")
    return float(value)


def _check_duplicate_keys(expressions):
    """UE object names (the keys) are compared without case: 'DropS' and 'Drops' would be one expression."""
    seen = {}
    for key in expressions:
        if type(key) is not str:
            continue
        other = seen.setdefault(key.lower(), key)
        if other != key:
            raise common.error(f"/Body/Expressions/{common.escape(key)}",
                               f"Expression keys '{other}' and '{key}' differ only by case. A key is the expression's "
                               "object name, and UE object names ignore case: rename one of them.",
                               "Material.DuplicateKey")


def _position(value, path):
    # JSON writers may emit integral coordinates as floats (-1300.0); both mean the same node position.
    if type(value) not in (list, tuple) or len(value) != 2 or not all(_integral(item) for item in value):
        raise common.error(path, "Position is [x, y] (two integers).", "Material.InvalidPosition")
    return [int(item) for item in value]


def _expression(key, value, path, keys):
    if type(value) is not dict:
        raise common.error(path, "An expression is {\"Class\", \"Position\"?, \"Properties\"?, \"Inputs\"?}.",
                           "Material.InvalidExpression")
    for field in value:
        if field not in EXPRESSION_KEYS:
            raise common.error(f"{path}/{common.escape(field)}", f"Unknown expression field '{field}'. Fields: "
                               f"{', '.join(EXPRESSION_KEYS)} (pins are Inputs).", "Material.UnknownExpressionField")
    if type(value.get("Class")) is not str or not value["Class"].strip():
        raise common.error(f"{path}/Class", "Class is the expression class: an engine class by short name "
                           "(\"Multiply\", \"TextureSampleParameter2D\") or a class path.", "Material.UnknownExpressionClass")
    out = {"Class": class_label(value["Class"].strip())}
    if "Position" in value:
        out["Position"] = _position(value["Position"], f"{path}/Position")
    properties = value.get("Properties", {})
    if type(properties) is not dict:
        raise common.error(f"{path}/Properties", "Properties is an object of property name -> value.",
                           "Material.InvalidProperties")
    out["Properties"] = dict(properties)
    inputs = value.get("Inputs", {})
    if type(inputs) is not dict:
        raise common.error(f"{path}/Inputs", "Inputs is an object of input pin -> source.", "Material.InvalidInputs")
    out["Inputs"] = {name: _source(source, f"{path}/Inputs/{common.escape(name)}", keys)
                     for name, source in inputs.items() if source is not None}
    return out


def canonical_body(body, class_path):
    out = {}
    keys = None
    if "Expressions" in body:
        expressions = body["Expressions"]
        if type(expressions) is not dict:
            raise common.error("/Body/Expressions", "Expressions is an object of key -> expression.",
                               "Material.InvalidExpressions")
        keys = set(expressions)
        _check_duplicate_keys(expressions)
        canonical = {}
        for key, value in expressions.items():
            path = f"/Body/Expressions/{common.escape(key)}"
            problem = _key_problem(key)
            if problem is not None:
                raise common.error(path, problem, "Material.InvalidKey")
            canonical[key] = _expression(key, value, path, keys)
        out["Expressions"] = canonical
        _check_cycles(canonical)
    if "Outputs" in body:
        outputs = body["Outputs"]
        if type(outputs) is not dict:
            raise common.error("/Body/Outputs", "Outputs is an object of material input (BaseColor, Roughness, ...) "
                               "-> source.", "Material.InvalidOutputs")
        out["Outputs"] = {name: _output_source(name, source, f"/Body/Outputs/{common.escape(name)}", keys)
                          for name, source in outputs.items() if source is not None}
    if "ParameterGroups" in body:
        groups = body["ParameterGroups"]
        if type(groups) is not dict or not all(_integral(value) for value in groups.values()) \
                or not all(name.strip() for name in groups):
            raise common.error("/Body/ParameterGroups", "ParameterGroups is an object of group name -> sort priority "
                               "(integer).", "Material.InvalidParameterGroups")
        out["ParameterGroups"] = {name: int(value) for name, value in groups.items()}
    return out


def _check_cycles(expressions):
    """A material graph is acyclic (the translator rejects a loop); report the first cycle at its input."""
    state = {}

    def visit(key, trail):
        state[key] = 1
        for name, source in sorted(expressions[key]["Inputs"].items()):
            upstream = source.split(".", 1)[0]
            if upstream not in expressions:
                continue
            if state.get(upstream) == 1:
                loop = trail[trail.index(upstream):] + [upstream] if upstream in trail else [key, upstream]
                raise common.error(f"/Body/Expressions/{common.escape(key)}/Inputs/{common.escape(name)}",
                                   f"Connection cycle: {' -> '.join(reversed(loop))}. A material graph cannot feed an "
                                   "expression from its own output.", "Material.ConnectionCycle")
            if upstream not in state:
                visit(upstream, trail + [upstream])
        state[key] = 2

    for key in sorted(expressions):
        if key not in state:
            visit(key, [key])


def _depths(expressions):
    """Longest path from a source (an expression without authored inputs) to each expression."""
    depth = {}
    for root in expressions:
        stack = [(root, False)]
        while stack:
            key, ready = stack.pop()
            if key in depth:
                continue
            upstream = [source.split(".", 1)[0] for source in expressions[key]["Inputs"].values()]
            upstream = [other for other in upstream if other in expressions and other != key]
            if ready:
                depth[key] = 1 + max((depth.get(other, 0) for other in upstream), default=-1)
            else:
                stack.append((key, True))
                stack.extend((other, False) for other in upstream if other not in depth)
    return depth


def _link_counts(expressions, outputs):
    """Expression key -> number of links from its outputs (expression inputs and material outputs)."""
    counts = {}
    sources = [source for row in expressions.values() for source in (row.get("Inputs") or {}).values()]
    for source in sources + list(outputs.values()):
        if type(source) is str:
            key = source.split(".", 1)[0]
            counts[key] = counts.get(key, 0) + 1
    return counts


def _implicit_value(key, expressions, links):
    """The value of an implicit constant (Constant_<pin>: a Constant with only its value, linked only to its pin),
    else None."""
    row = expressions.get(key)
    if type(row) is not dict or class_label(row.get("Class", "")) != IMPLICIT_CONSTANT_CLASS or links.get(key) != 1:
        return None
    properties = row.get("Properties") or {}
    if row.get("Inputs") or set(properties) - {IMPLICIT_CONSTANT_VALUE}:
        return None
    value = properties.get(IMPLICIT_CONSTANT_VALUE, 0.0)
    return float(value) if _number(value) else None


def collapse_constants(body):
    """Document form of the material outputs: an output wired from its implicit constant is that number, and the
    Constant expression is not listed."""
    expressions, outputs = body.get("Expressions"), body.get("Outputs")
    if type(expressions) is not dict or type(outputs) is not dict:
        return body
    links = _link_counts(expressions, outputs)
    values = {}
    for name, source in outputs.items():
        key = implicit_constant_key(name)
        if source == key:
            value = _implicit_value(key, expressions, links)
            if value is not None:
                values[name] = value
    if not values:
        return body
    removed = {implicit_constant_key(name) for name in values}
    return {**body, "Outputs": {name: values.get(name, source) for name, source in outputs.items()},
            "Expressions": {key: row for key, row in expressions.items() if key not in removed}}


def expand_constants(body, live):
    """Apply form of the material outputs: a number is the Constant expression Constant_<pin> wired to the pin.
    Without Body.Expressions (left as it is) the live graph stands for it, less the implicit constants the
    document no longer has."""
    outputs = body.get("Outputs")
    if type(outputs) is not dict:
        return body
    constants = {name: value for name, value in outputs.items() if _number(value)}
    if "Expressions" in body:
        expressions = dict(body["Expressions"])
    else:
        rows = {key: {"Class": row["Class"], "Properties": dict(row["Properties"]), "Inputs": dict(row["Inputs"])}
                for key, row in live["expressions"].items()}
        stale = set(rows) - set(collapse_constants({"Expressions": rows, "Outputs": live["outputs"]})["Expressions"])
        if not constants and not stale:
            return body
        expressions = {key: row for key, row in rows.items() if key not in stale}
    for name, value in constants.items():
        expressions[implicit_constant_key(name)] = {"Class": IMPLICIT_CONSTANT_CLASS,
                                                    "Properties": {IMPLICIT_CONSTANT_VALUE: value}, "Inputs": {}}
    return {**body, "Expressions": expressions,
            "Outputs": {name: implicit_constant_key(name) if name in constants else source
                        for name, source in outputs.items()}}


def canonicalize(document):
    class_path = document.get("Class") if type(document) is dict else None
    common.validate_header(document, tuple(PROFILES), PREFIX,
                           body_keys(class_path) if class_path in PROFILES else MATERIAL_BODY_KEYS)
    canonical_body(document.get("Body", {}), document["Class"])
    return common.snapshot(document)


# ---------------------------------------------------------------------------
# Live state
# ---------------------------------------------------------------------------
def read_live(calls):
    first, rows = calls.query_all(READ, "expressions")
    expressions = {}
    for row in rows:
        expressions[row["key"]] = {"Class": row["class"], "Position": list(row["position"]),
                                   "Properties": dict(row.get("properties", {})), "Inputs": dict(row.get("inputs", {})),
                                   "outputs": list(row.get("outputs", []))}
    return {"host": first.get("host"), "expressions": expressions, "outputs": dict(first.get("outputs", {})),
            "parameter_groups": dict(first.get("parameter_groups", {})),
            "compile_errors": list(first.get("compile_errors", []))}


def class_defaults(calls, labels):
    """New-node values (and pins) of the classes; cached for the session."""
    missing = sorted({label for label in labels if label not in _CLASS_DEFAULTS})
    for start in range(0, len(missing), 8):
        page = calls.query(READ, {"classes": missing[start:start + 8]})
        for label, row in page.get("classes", {}).items():
            _CLASS_DEFAULTS[label] = row
    return {label: _CLASS_DEFAULTS[label] for label in labels if label in _CLASS_DEFAULTS}


def _body(live, class_path):
    expressions = {}
    # Graph order (the material compiles its expressions in it: texture and vertex interpolator numbering).
    for key, row in live["expressions"].items():
        entry = {"Class": row["Class"], "Position": row["Position"]}
        if row["Properties"]:
            entry["Properties"] = row["Properties"]
        if row["Inputs"]:
            entry["Inputs"] = row["Inputs"]
        expressions[key] = entry
    body = {"Expressions": expressions}
    if is_material(class_path):
        body["Outputs"] = live["outputs"]
        body["ParameterGroups"] = live["parameter_groups"]
        body = collapse_constants(body)
    return body


# ---------------------------------------------------------------------------
# Apply
# ---------------------------------------------------------------------------
def _same_properties(authored, live, defaults, exact):
    """Authored (complete) properties against the live non-default ones; None when equal, else the first
    differing property name."""
    for name in sorted(set(authored) | set(live)):
        if name in authored and name in live:
            if not common.same(authored[name], live[name], exact):
                return name
        elif name in live:
            # (A property whose new-node value is random, read always: omitted keeps the node's value.)
            if name in ((defaults or {}).get("volatile") or ()):
                continue
            return name
        else:
            known = defaults.get("properties", {}) if defaults else {}
            if name not in known or not common.same(authored[name], known[name], exact):
                return name
    return None


def _output_zero(class_label_, defaults):
    """Name of output 0 on a new node of the class (from the cached pins), or None."""
    outputs = ((defaults or {}).get(class_label_) or {}).get("pins", {}).get("outputs", [])
    name = outputs[0].get("name") if outputs and type(outputs[0]) is dict else None
    return name if type(name) is str and name and not name.startswith("#") else None


def same_source(wanted, current, classes, defaults):
    """Whether two sources name the same output. "<key>" is output 0; a node saved by an older engine version
    can keep an unnamed output 0 where a new node of its class names it ("RGB"), so "<key>" and
    "<key>.<that name>" are the same link."""
    if wanted == current:
        return True
    if type(wanted) is not str or type(current) is not str:
        return False
    for bare, named in ((wanted, current), (current, wanted)):
        if "." not in bare and named.startswith(bare + "."):
            zero = _output_zero(classes.get(bare), defaults)
            if zero is not None and named[len(bare) + 1:] == zero:
                return True
    return False


def _same_links(wanted, current, classes, defaults, exact=False):
    """Material outputs: sources name the same output, constants (numbers) are the same value."""
    def same(left, right):
        if _number(left) or _number(right):
            return common.same(left, right, exact)
        return same_source(left, right, classes, defaults)
    return set(wanted) == set(current) and all(same(wanted[name], current[name]) for name in wanted)


def graph_ops(body, live, defaults=None):
    """Edit ops (with document paths): deletes, broken links, creates, property / position sets, connections,
    material outputs, parameter groups (in that order: links are broken while their inputs exist, creates precede
    the references to them, properties precede the connections because they can change the pins). An expression
    whose Class changes is deleted and created again, and every link to it is made again."""
    defaults = defaults or {}
    current = live["expressions"]
    classes = {key: row["Class"] for key, row in current.items()}
    classes.update({key: row["Class"] for key, row in body.get("Expressions", {}).items()})
    deletes, disconnects, creates, sets, connects = [], [], [], [], []
    placed = 0
    if "Expressions" in body:
        authored = body["Expressions"]
        for key in sorted(current):
            if key not in authored or class_label(authored[key]["Class"]) != class_label(current[key]["Class"]):
                deletes.append(({"op": "delete", "key": key}, f"/Body/Expressions/{common.escape(key)}"))
        replaced = {op["key"] for op, _ in deletes}
        for key, expression in authored.items():
            path = f"/Body/Expressions/{common.escape(key)}"
            new = key not in current or key in replaced
            before = None if new else current[key]
            if new:
                position = expression.get("Position")
                if position is None:
                    position = [_NEW_COLUMN_X + _NEW_COLUMN_STEP * (placed // _NEW_PER_COLUMN),
                                _NEW_ROW_STEP * (placed % _NEW_PER_COLUMN)]
                    placed += 1
                creates.append(({"op": "create", "key": key, "class": expression["Class"], "position": position},
                                f"{path}/Class"))
            op = {"op": "set", "key": key}
            if not new and "Position" in expression and expression["Position"] != before["Position"]:
                op["position"] = expression["Position"]
            # A new node is always set: the factory may have made a name unique that the document leaves at the
            # new-node value.
            if new or _same_properties(expression["Properties"], before["Properties"],
                                       defaults.get(expression["Class"]), True) is not None:
                op["properties"] = expression["Properties"]
            if len(op) > 2:
                sets.append((op, f"{path}/Properties" if "properties" in op else f"{path}/Position"))
            # A link from a replaced expression went with it: the new expression is wired again.
            live_inputs = {} if new else _surviving(before["Inputs"], replaced)
            for name in sorted(set(expression["Inputs"]) | set(live_inputs)):
                wanted = expression["Inputs"].get(name)
                if not same_source(wanted, live_inputs.get(name), classes, defaults):
                    op = ({"op": "connect", "key": key, "input": name, "from": wanted}, f"{path}/Inputs/{common.escape(name)}")
                    # A link is broken while its input still exists: a property set (a Custom node's Inputs) can
                    # remove the input.
                    (disconnects if wanted is None else connects).append(op)
    outputs = []
    if "Outputs" in body:
        live_outputs = _surviving(live["outputs"], {op["key"] for op, _ in deletes})
        for name in sorted(set(body["Outputs"]) | set(live_outputs)):
            wanted = body["Outputs"].get(name)
            if not same_source(wanted, live_outputs.get(name), classes, defaults):
                outputs.append(({"op": "output", "name": name, "from": wanted}, f"/Body/Outputs/{common.escape(name)}"))
    groups = []
    if "ParameterGroups" in body and body["ParameterGroups"] != live["parameter_groups"]:
        groups.append(({"op": "parameter_groups", "groups": body["ParameterGroups"]}, "/Body/ParameterGroups"))
    # Wire from the sources forward, as one wires a graph left to right: an expression whose output type
    # follows its inputs (a switch, a reroute) has its inputs before the connection type check of its output.
    depth = _depths(body.get("Expressions", {}))
    connects.sort(key=lambda item: depth.get(item[0]["key"], 0))
    return deletes + disconnects + creates + sets + connects + outputs + groups


def _surviving(links, removed):
    """The links whose source expression is not deleted ("<key>" or "<key>.<output>")."""
    return {name: source for name, source in links.items()
            if not (type(source) is str and source.split(".", 1)[0] in removed)}


def _op_path(primitive_path, op_paths):
    """/ops/<n>/<field>/... -> the document path of op n (a property: .../Properties/<name>)."""
    parts = [part for part in (primitive_path or "").split("/") if part]
    if len(parts) >= 2 and parts[0] == "ops" and parts[1].isdigit() and int(parts[1]) < len(op_paths):
        base = op_paths[int(parts[1])]
        if len(parts) >= 4 and parts[2] == "properties":
            root = base[:-len("/Properties")] if base.endswith("/Properties") else base.rsplit("/", 1)[0]
            return f"{root}/Properties/{parts[3]}"
        if len(parts) >= 3 and parts[2] == "position" and base.endswith("/Properties"):
            return base[:-len("/Properties")] + "/Position"
        return base
    return "/Body"


def _edit(calls, ops):
    changed = []
    for chunk in common.chunk_ops(ops):
        op_paths = [path for _, path in chunk]

        def where(primitive_path, op_paths=op_paths):
            return _op_path(primitive_path, op_paths)
        outcome = common.invoke(calls.context, BY_ID, EDIT, {"schema_version": 1, "ops": [op for op, _ in chunk]},
                                (NamedOpaqueHandleArgument("asset", calls.write),), paths=where, prefix=PREFIX)
        changed.extend(outcome.changed_set)
    return changed


def compile_outcome(before, result):
    """(fatal diagnostic or None, warnings) of a recompile: new errors fail the apply, errors the material
    already had are warnings (compile state not worse)."""
    errors = list(result.get("errors", []))
    new = [error for error in errors if error not in before]
    if new:
        nodes = result.get("error_expressions", [])
        where = f"/Body/Expressions/{common.escape(nodes[0])}" if len(nodes) == 1 else "/Body"
        message = "The material does not compile: " + " | ".join(new[:8])
        if nodes:
            message += f" (expressions: {', '.join(nodes[:20])})"
        return common.error(where, message, "Material.CompileFailed"), ()
    warnings = tuple(RuntimeDiagnostic(path="/Body", code="Material.CompileError", message=error,
                                       severity=DiagnosticSeverity.WARNING) for error in errors)
    return None, warnings


# Properties of a parameter expression that hold its default value (what a material instance overrides).
PARAMETER_DEFAULT_PROPERTIES = frozenset({"DefaultValue", "Texture", "TextureCollection", "DefaultR", "DefaultG", "DefaultB",
                                          "DefaultA", "Font", "FontTexturePage", "VirtualTexture", "SparseVolumeTexture",
                                          "Curve", "Atlas"})
MAX_NAMED_INSTANCES = 10


def changed_parameter_defaults(body, live, defaults):
    """Names (as the live material has them) of the existing parameter expressions whose default value the body
    changes. A new or re-classed expression is not a changed default."""
    names = set()
    for key, expression in body.get("Expressions", {}).items():
        before = live["expressions"].get(key)
        if before is None or class_label(before["Class"]) != class_label(expression["Class"]):
            continue
        known = ((defaults or {}).get(before["Class"]) or {}).get("properties", {})
        authored = expression.get("Properties", {})

        def value(properties, name):
            return properties[name] if name in properties else known.get(name)
        name = value(before["Properties"], "ParameterName")
        if type(name) is not str or not name or name == "None":
            continue
        if any(not common.same(value(authored, prop), value(before["Properties"], prop), True)
               for prop in PARAMETER_DEFAULT_PROPERTIES if prop in authored or prop in before["Properties"]):
            names.add(name)
    return sorted(names)


def overriding_instances_info(calls, names):
    """Info diagnostic naming the material instances that override a changed parameter default (they keep their own
    value), or ()."""
    if not names:
        return ()
    found = calls.query(READ, {"instance_overrides": list(names)}).get("instance_overrides") or {}
    count = found.get("count", 0)
    rows = [row for row in found.get("instances", []) if type(row) is dict]
    if type(count) is not int or count <= 0 or not rows:
        return ()
    listed = ", ".join(f"{row.get('path', '?').rsplit('.', 1)[0]} ({', '.join(row.get('parameters', []))})"
                       for row in rows[:MAX_NAMED_INSTANCES])
    more = f" and {count - len(rows[:MAX_NAMED_INSTANCES])} more" if count > len(rows[:MAX_NAMED_INSTANCES]) else ""
    message = (f"{count} material instance(s) override the changed default of {', '.join(names)} and keep their own "
               f"value: {listed}{more}. Edit or clear their overrides to use the new default.")
    return (RuntimeDiagnostic(path="/Body/Expressions", code="Material.DefaultOverriddenByInstances", message=message,
                              severity=DiagnosticSeverity.INFO),)


def apply(document, read, mutable, context):
    class_path = document["Class"]
    body = canonical_body(document.get("Body", {}), class_path)
    properties = document.get("Properties", {})
    region = _properties(class_path)
    region.check_owned(properties)
    calls = common.DataCalls(context, BY_ID, read, mutable, PREFIX)
    live = read_live(calls)
    body = expand_constants(body, live)
    # Material settings first (the domain decides which material inputs apply), then the graph.
    changes = list(region.apply(properties, read, mutable, context))
    # New-node facts of the classes in the graph: omitted property values, and output names for links. (An
    # authored class that is not in the graph yet is created; its class is checked by the edit.)
    defaults = class_defaults(calls, {row["Class"] for row in live["expressions"].values()})
    ops = graph_ops(body, live, defaults)
    warnings = ()
    if ops or changes:
        if ops:
            changes.extend(_edit(calls, ops))
        outcome = common.invoke(context, BY_ID, COMPILE, {"schema_version": 1},
                                (NamedOpaqueHandleArgument("asset", calls.write),), prefix=PREFIX)
        changes.extend(outcome.changed_set)
        failure, warnings = compile_outcome(live["compile_errors"],
                                            outcome.value if type(outcome.value) is dict else {})
        if failure is not None:
            raise failure
        if is_material(class_path):
            warnings = tuple(warnings) + overriding_instances_info(calls, changed_parameter_defaults(body, live, defaults))
    return with_diagnostics(changes, warnings)


def _properties(class_path):
    return common.PropertiesRegion(class_path, BODY_OWNED, PREFIX, NOT_AUTHORED)


# ---------------------------------------------------------------------------
# Extract / Diff / Inspect
# ---------------------------------------------------------------------------
def extract(target, class_path, read, context, include_defaults=True):
    calls = common.DataCalls(context, BY_ID, read, prefix=PREFIX)
    live = read_live(calls)
    class_defaults(calls, {row["Class"] for row in live["expressions"].values()})
    return common.document(target, class_path, _properties(class_path).extract(read, context, include_defaults), _body(live, class_path))


def diff(authored, extracted, exact, defaults=None):
    defaults = _CLASS_DEFAULTS if defaults is None else defaults
    changes = common.header_diff(authored, extracted)
    class_path = authored["Class"]
    body = collapse_constants(canonical_body(authored.get("Body", {}), class_path))
    live = collapse_constants(canonical_body(extracted.get("Body", {}), class_path))
    classes = {key: row["Class"] for key, row in live.get("Expressions", {}).items()}
    classes.update({key: row["Class"] for key, row in body.get("Expressions", {}).items()})
    if "Expressions" in body:
        current = live.get("Expressions", {})
        for key in sorted(set(current) - set(body["Expressions"])):
            changes.append(Difference(f"/Body/Expressions/{common.escape(key)}", DiffKind.REMOVE, True, None, True,
                                      current[key]))
        for key, expression in body["Expressions"].items():
            path = f"/Body/Expressions/{common.escape(key)}"
            if key not in current:
                changes.append(Difference(path, DiffKind.ADD, True, expression, False, None))
                continue
            before = current[key]
            if expression["Class"] != before["Class"]:
                changes.append(Difference(f"{path}/Class", DiffKind.REPLACE, True, expression["Class"], True,
                                          before["Class"]))
                continue
            if "Position" in expression and expression["Position"] != before.get("Position"):
                changes.append(Difference(f"{path}/Position", DiffKind.REPLACE, True, expression["Position"], True,
                                          before.get("Position")))
            name = _same_properties(expression["Properties"], before["Properties"], defaults.get(expression["Class"]),
                                    exact)
            if name is not None:
                changes.append(Difference(f"{path}/Properties/{common.escape(name)}", DiffKind.REPLACE,
                                          name in expression["Properties"], expression["Properties"].get(name),
                                          name in before["Properties"], before["Properties"].get(name)))
            for name in sorted(set(expression["Inputs"]) | set(before["Inputs"])):
                if not same_source(expression["Inputs"].get(name), before["Inputs"].get(name), classes, defaults):
                    changes.append(Difference(f"{path}/Inputs/{common.escape(name)}", DiffKind.REPLACE,
                                              name in expression["Inputs"], expression["Inputs"].get(name),
                                              name in before["Inputs"], before["Inputs"].get(name)))
                    break
    if "Outputs" in body and not _same_links(body["Outputs"], live.get("Outputs", {}), classes, defaults, exact):
        changes.append(Difference("/Body/Outputs", DiffKind.REPLACE, True, body["Outputs"], "Outputs" in live,
                                  live.get("Outputs")))
    if "ParameterGroups" in body and body["ParameterGroups"] != live.get("ParameterGroups"):
        changes.append(Difference("/Body/ParameterGroups", DiffKind.REPLACE, True, body["ParameterGroups"],
                                  "ParameterGroups" in live, live.get("ParameterGroups")))
    changes.extend(common.properties_diff(authored.get("Properties", {}), extracted.get("Properties", {}), exact))
    return tuple(changes)


def _pin_list(pins):
    return [f"{pin['name']}: {pin['type']}" for pin in pins]


def inspect_payload(document, calls):
    """Inspect: the document plus each expression's pins (name: type) and the material inputs."""
    first = calls.query(READ, {"offset": 0, "pins": True})
    rows = list(first.get("expressions", []))
    offset = first.get("next_offset", -1)
    while type(offset) is int and offset >= 0:
        page = calls.query(READ, {"offset": offset, "pins": True})
        rows += page.get("expressions", [])
        offset = page.get("next_offset", -1)
    pins = {}
    for row in rows:
        entry = {}
        if row.get("pins", {}).get("inputs"):
            entry["Inputs"] = _pin_list(row["pins"]["inputs"])
        if row.get("pins", {}).get("outputs"):
            entry["Outputs"] = _pin_list(row["pins"]["outputs"])
        pins[row["key"]] = entry
    inspected = {**document, "Pins": pins}
    if "material_inputs" in first:
        inspected["MaterialInputs"] = _pin_list(first["material_inputs"])
    if first.get("compile_errors"):
        inspected["CompileErrors"] = list(first["compile_errors"])
    return inspected


# ---------------------------------------------------------------------------
# Handler
# ---------------------------------------------------------------------------
class MaterialRuntimeProfile:
    runtime_entry_module = "asset_document/bootstrap.py"
    # Snapshot-covered: preflight never reads a preview.
    preflight_scratch = False

    def __init__(self, class_path, profile_id):
        self.exact_asset_class_path = class_path
        self.profile_id = profile_id

    def required_primitives(self):
        return REQUIREMENTS

    def inspect_profile(self):
        return {"Class": self.exact_asset_class_path, "BodySections": list(body_keys(self.exact_asset_class_path)),
                "ReadOnlySections": []}

    def create_template(self, target):
        target = common.require_target(target, PREFIX)
        if not is_material(self.exact_asset_class_path):
            body = {"Expressions": {
                "In": {"Class": "FunctionInput", "Position": [-400, 0], "Properties": {"InputName": "In"}},
                "Out": {"Class": "FunctionOutput", "Position": [0, 0], "Properties": {"OutputName": "Result"},
                        "Inputs": {"A": "In"}}}}
            return common.document(target, self.exact_asset_class_path, {}, body)
        body = {"Expressions": {
            "Color": {"Class": "VectorParameter", "Position": [-400, 0],
                      "Properties": {"ParameterName": "BaseColor", "DefaultValue": {"R": 1, "G": 1, "B": 1, "A": 1}}},
            "Rough": {"Class": "ScalarParameter", "Position": [-400, 200],
                      "Properties": {"ParameterName": "Roughness", "DefaultValue": 0.5}}},
            "Outputs": {"BaseColor": "Color", "Roughness": "Rough"}, "ParameterGroups": {}}
        return common.document(target, self.exact_asset_class_path, {}, body)

    def _canonical(self, document):
        canonical = canonicalize(document)
        if canonical["Class"] != self.exact_asset_class_path:
            raise common.error("/Class", f"Class must be {self.exact_asset_class_path}.", "Material.ClassMismatch")
        return canonical

    def validate(self, document):
        self._canonical(document)

    def canonicalize(self, document):
        return self._canonical(document)

    def create_target_context(self, document, context):
        self._canonical(document)
        return ()

    def preflight(self, document, scratch_handle, context):
        canonical = self._canonical(document)
        _properties(canonical["Class"]).validate(canonical.get("Properties", {}), context)
        return SNAPSHOT_COVERED

    def apply(self, document, read_asset_handle, mutable_asset_handle, context):
        return apply(self._canonical(document), read_asset_handle, mutable_asset_handle, context)

    def repair(self, document, asset_handle, context):
        self._canonical(document)
        return ()

    def extract(self, *, target, asset_handle, context, include_defaults=False):
        return extract(common.require_target(target, PREFIX), self.exact_asset_class_path, asset_handle, context,
                       include_defaults)

    def inspect(self, *, mode, exact_class_path, exact_asset_object_path, editor_asset_handle, context):
        if mode != "live_asset" or exact_class_path != self.exact_asset_class_path or editor_asset_handle is None:
            raise common.error("/Inspect", "Material inspection reads a live material or material function.",
                               "Material.InspectNotAdmitted")
        document = self.canonicalize(self.extract(target=exact_asset_object_path.rsplit(".", 1)[0],
                                                  asset_handle=editor_asset_handle, context=context, include_defaults=True))
        return inspect_payload(document, common.DataCalls(context, BY_ID, editor_asset_handle, prefix=PREFIX))

    def diff(self, authored, extracted):
        return diff(self._canonical(authored), self._canonical(extracted), exact=False)

    def diff_exact(self, authored, extracted):
        return diff(self._canonical(authored), self._canonical(extracted), exact=True)


def _meta(class_path, profile_id):
    rows = [("expressions", "/Body/Expressions")]
    if is_material(class_path):
        rows += [("outputs", "/Body/Outputs"), ("parameter_groups", "/Body/ParameterGroups")]
    regions = tuple(RegionMeta(f"material.{name}.v1", path, RegionOwnership.MANAGED, IdentityPolicy.STABLE_KEY,
                               OrderPolicy.CANONICAL, DefaultPolicy.EXPLICIT, OmissionPolicy.PRESERVE,
                               DeletePolicy.EXPLICIT_ONLY, ComparisonPolicy.CANONICAL)
                    for name, path in rows)
    return ProfileMeta(profile_id, common.ROUTE, class_path, regions, REQUIREMENTS)


def _registration(class_path):
    profile_id = PROFILES[class_path]
    return RuntimeProfileRegistration(_meta(class_path, profile_id), lambda: MaterialRuntimeProfile(class_path, profile_id),
                                      RuntimeTargetPolicy.CREATE_OR_UPDATE, LIFECYCLE)


MATERIAL_RUNTIME_REGISTRATION = _registration(MATERIAL_CLASS)
MATERIAL_FUNCTION_RUNTIME_REGISTRATION = _registration(MATERIAL_FUNCTION_CLASS)
MATERIAL_LAYER_RUNTIME_REGISTRATION = _registration(MATERIAL_LAYER_CLASS)
MATERIAL_LAYER_BLEND_RUNTIME_REGISTRATION = _registration(MATERIAL_LAYER_BLEND_CLASS)

__all__ = ("MATERIAL_RUNTIME_REGISTRATION", "MATERIAL_FUNCTION_RUNTIME_REGISTRATION", "MATERIAL_LAYER_RUNTIME_REGISTRATION",
           "MATERIAL_LAYER_BLEND_RUNTIME_REGISTRATION", "MaterialRuntimeProfile", "canonical_body", "graph_ops", "diff",
           "compile_outcome", "class_label", "collapse_constants", "expand_constants")
