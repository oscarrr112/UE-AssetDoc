"""Primitive failures keep the primitive id out of the public message (it goes to context/details)."""
import json
import unittest

from asset_document.diagnostics import DiagnosticContext, RuntimeDiagnostic, describe_json, primitive_error


class PrimitiveErrorTests(unittest.TestCase):
    def test_primitive_detail_goes_to_context_not_message(self):
        source = RuntimeDiagnostic("/ValueArguments/path", "ClassReference.NonCanonicalAsset",
                                   "The asset path did not resolve to that exact canonical identity.",
                                   context=(DiagnosticContext("candidate", "/Game/X"),))
        diagnostic = primitive_error("/Body/RowStruct", source, "editor.reference.resolve_asset.v1").diagnostic
        self.assertEqual((diagnostic.path, diagnostic.code), ("/Body/RowStruct", "ClassReference.NonCanonicalAsset"))
        self.assertEqual(diagnostic.message, source.message)
        self.assertEqual({item.key: item.value for item in diagnostic.context},
                         {"primitive": "editor.reference.resolve_asset.v1",
                          "primitive_path": "/ValueArguments/path", "candidate": "/Game/X"})


class DescribeJsonTests(unittest.TestCase):
    def test_short_values_are_written_whole(self):
        value = {"Differs": {"Nodes": [{"Id": "Branch", "PinOverrides": [{"Pin": "Condition", "DefaultValue": "false"}]}]}}
        self.assertEqual(describe_json(value, 400), json.dumps(value, sort_keys=True, separators=(",", ":")))

    def test_long_values_elide_whole_entries_never_mid_value(self):
        self.assertEqual(describe_json(list(range(300)), 100), "[" + ",".join(map(str, range(32))) + ',"+268 more"]')
        text = "x" * 500
        self.assertEqual(describe_json(text, 100), json.dumps(text))
        described = describe_json({f"Key{i:03}": "v" * 20 for i in range(50)}, 120)
        entry = '"Key00{}":"' + "v" * 20 + '"'
        self.assertEqual(described, "{" + ",".join(entry.format(i) for i in range(3)) + ',"+47 more"}')


if __name__ == "__main__":
    unittest.main()
