import math
import unittest

from asset_document.canonical.document_codec import (
    MAXIMUM_DEPTH,
    MAXIMUM_INTEGER,
    MAXIMUM_NODES,
    MINIMUM_INTEGER,
    DocumentCodec,
    integral_numbers_as_integers,
)
from asset_document.diagnostics import AssetDocumentContractError
from asset_document.tests.property_cases import generated_json_values


def assert_codec_error(test: unittest.TestCase, code: str, path: str, callback) -> None:
    with test.assertRaises(AssetDocumentContractError) as caught:
        callback()
    test.assertEqual((caught.exception.diagnostic.code, caught.exception.diagnostic.path), (code, path))


def nested_array(depth: int):
    value = None
    for _ in range(depth):
        value = [value]
    return value


class DocumentCodecGoldenTests(unittest.TestCase):
    def setUp(self) -> None:
        self.codec = DocumentCodec()

    def test_exact_utf8_bytes_and_shape_roundtrip(self) -> None:
        value = {
            "Z": [None, True, 1, 1.0, "雪"],
            "A": {"EmptyArray": [], "EmptyObject": {}, "EmptyString": ""},
        }
        expected = (
            b'{"A":{"EmptyArray":[],"EmptyObject":{},"EmptyString":""},'
            b'"Z":[null,true,1,1.0,"\xe9\x9b\xaa"]}'
        )
        self.assertEqual(self.codec.encode(value), expected)
        self.assertEqual(self.codec.decode(expected), value)
        self.assertIs(type(self.codec.decode(b"1")), int)
        self.assertIs(type(self.codec.decode(b"1.0")), float)

    def test_rejects_nonfinite_python_numbers(self) -> None:
        for value in (math.nan, math.inf, -math.inf):
            with self.subTest(value=value):
                assert_codec_error(
                    self, "Codec.NonFiniteNumber", "/Body/0",
                    lambda value=value: self.codec.encode({"Body": [value]}),
                )

    def test_rejects_nonfinite_json_constants(self) -> None:
        for payload in (b"NaN", b"Infinity", b"-Infinity"):
            with self.subTest(payload=payload):
                assert_codec_error(
                    self, "Codec.NonFiniteNumber", "/",
                    lambda payload=payload: self.codec.decode(payload),
                )

    def test_rejects_unsupported_codec_version(self) -> None:
        for version in (True, 0, 2, "1"):
            with self.subTest(version=version):
                assert_codec_error(
                    self, "Codec.UnsupportedVersion", "/CodecVersion",
                    lambda version=version: DocumentCodec(version),
                )

    def test_rejects_integer_below_int64(self) -> None:
        for value in (MINIMUM_INTEGER - 1, MAXIMUM_INTEGER + 1):
            with self.subTest(value=value):
                assert_codec_error(
                    self, "Codec.IntegerOutOfRange", "/Value",
                    lambda value=value: self.codec.encode({"Value": value}),
                )

    def test_accepts_signed_int64_boundaries(self) -> None:
        value = [MINIMUM_INTEGER, MAXIMUM_INTEGER]
        self.assertEqual(self.codec.decode(self.codec.encode(value)), value)

    def test_rejects_non_string_key(self) -> None:
        assert_codec_error(
            self, "Codec.InvalidType", "/", lambda: self.codec.encode({1: "value"})
        )

    def test_rejects_bytes_tuple_and_custom_object(self) -> None:
        class Custom:
            pass

        for value in (b"bytes", (1,), Custom()):
            with self.subTest(value=type(value).__name__):
                assert_codec_error(
                    self, "Codec.InvalidType", "/Body",
                    lambda value=value: self.codec.encode({"Body": value}),
                )

    def test_generated_roundtrip_determinism_and_idempotence(self) -> None:
        for value in generated_json_values(seed=0xAD0C, count=512):
            with self.subTest(value=value):
                first = self.codec.encode(value)
                decoded = self.codec.decode(first)
                second = self.codec.encode(decoded)
                self.assertEqual(second, first)
                self.assertEqual(self.codec.decode(second), decoded)

    def test_rejects_invalid_unicode(self) -> None:
        assert_codec_error(
            self, "Codec.InvalidUnicode", "/Text",
            lambda: self.codec.encode({"Text": "\ud800"}),
        )
        assert_codec_error(
            self, "Codec.InvalidUnicode", "/Key",
            lambda: self.codec.encode({"\ud800": 1}),
        )

    def test_rejects_invalid_utf8(self) -> None:
        assert_codec_error(
            self, "Codec.InvalidUtf8", "/", lambda: self.codec.decode(b"\xff")
        )

    def test_decode_requires_exact_bytes_and_rejects_str_bytearray_memoryview(self) -> None:
        for payload in ("{}", bytearray(b"{}"), memoryview(b"{}")):
            with self.subTest(payload=type(payload).__name__):
                assert_codec_error(
                    self, "Codec.InvalidType", "/",
                    lambda payload=payload: self.codec.decode(payload),
                )

    def test_rejects_duplicate_decoded_key(self) -> None:
        assert_codec_error(
            self, "Codec.DuplicateObjectKey", "/",
            lambda: self.codec.decode(b'{"A":1,"A":2}'),
        )

    def test_rejects_malformed_json(self) -> None:
        assert_codec_error(
            self, "Codec.MalformedJson", "/", lambda: self.codec.decode(b'{"A":')
        )

    def test_rejects_depth_65_and_accepts_depth_64(self) -> None:
        accepted = nested_array(MAXIMUM_DEPTH)
        self.assertEqual(self.codec.decode(self.codec.encode(accepted)), accepted)
        expected_path = "/0" * (MAXIMUM_DEPTH + 1)
        assert_codec_error(
            self, "Codec.DepthExceeded", expected_path,
            lambda: self.codec.encode(nested_array(MAXIMUM_DEPTH + 1)),
        )

    def test_rejects_one_node_over_the_document_limit(self) -> None:
        accepted = [None] * (MAXIMUM_NODES - 1)
        self.assertEqual(len(self.codec.decode(self.codec.encode(accepted))), MAXIMUM_NODES - 1)
        rejected = [None] * MAXIMUM_NODES
        assert_codec_error(
            self, "Codec.NodeLimitExceeded", f"/{MAXIMUM_NODES - 1}",
            lambda: self.codec.encode(rejected),
        )

    def test_rejects_active_container_cycle_but_allows_shared_acyclic_value(self) -> None:
        cycle = []
        cycle.append(cycle)
        assert_codec_error(
            self, "Codec.CycleDetected", "/0", lambda: self.codec.encode(cycle)
        )
        shared = {"Value": [1, 2]}
        value = [shared, shared]
        self.assertEqual(self.codec.decode(self.codec.encode(value)), value)

    def test_rejects_builtin_container_and_scalar_subclasses(self) -> None:
        class ListSubclass(list):
            pass

        class DictSubclass(dict):
            pass

        class IntSubclass(int):
            pass

        class FloatSubclass(float):
            pass

        class StringSubclass(str):
            pass

        values = (
            ListSubclass(), DictSubclass(), IntSubclass(1),
            FloatSubclass(1.0), StringSubclass("value"),
        )
        for value in values:
            with self.subTest(value=type(value).__name__):
                assert_codec_error(
                    self, "Codec.InvalidType", "/Body",
                    lambda value=value: self.codec.encode({"Body": value}),
                )


class IntegralNumberFormTests(unittest.TestCase):
    def test_integral_floats_become_integers_recursively(self) -> None:
        value = {"A": 1.0, "B": [0.0, -0.0, 2.5, {"C": -3.0}], "D": "1.0", "E": True, "F": 7}
        normalized = integral_numbers_as_integers(value)
        self.assertEqual(normalized, {"A": 1, "B": [0, 0, 2.5, {"C": -3}], "D": "1.0", "E": True, "F": 7})
        self.assertIs(type(normalized["A"]), int)
        self.assertIs(type(normalized["B"][2]), float)
        self.assertIs(type(normalized["E"]), bool)
        self.assertEqual(value["A"], 1.0)
        self.assertIs(type(value["A"]), float)

    def test_out_of_int64_range_and_non_integral_floats_stay_exact(self) -> None:
        self.assertEqual(integral_numbers_as_integers(9.3e18), 9.3e18)
        self.assertIs(type(integral_numbers_as_integers(9.3e18)), float)
        self.assertIs(type(integral_numbers_as_integers(-9.2233720368547758e18)), int)
        self.assertEqual(integral_numbers_as_integers(0.1).hex(), (0.1).hex())
        self.assertEqual(integral_numbers_as_integers(1e16), 10**16)

    def test_normalized_values_encode_like_ue_writer_output(self) -> None:
        # UE's JSON writer prints the double 1.0 as "1"; both spellings decode to one form.
        codec = DocumentCodec()
        for text in (b'{"V":1}', b'{"V":1.0}'):
            with self.subTest(text=text):
                self.assertEqual(codec.encode(integral_numbers_as_integers(codec.decode(text))), b'{"V":1}')


class SidecarLayoutTests(unittest.TestCase):
    """The sidecar an apply writes back is indented, so that a person can edit it again."""

    def test_indented_layout_with_short_scalar_arrays_inline(self) -> None:
        value = {"Z": {"Position": [-400, 0], "Name": "雪", "Empty": [], "None": {}},
                 "A": [{"K": 1.5}, True], "Long": ["x" * 60, "y" * 60]}
        expected = (
            '{\n'
            '  "A": [\n'
            '    {\n'
            '      "K": 1.5\n'
            '    },\n'
            '    true\n'
            '  ],\n'
            '  "Long": [\n'
            f'    "{"x" * 60}",\n'
            f'    "{"y" * 60}"\n'
            '  ],\n'
            '  "Z": {\n'
            '    "Empty": [],\n'
            '    "Name": "雪",\n'
            '    "None": {},\n'
            '    "Position": [-400, 0]\n'
            '  }\n'
            '}\n'
        ).encode("utf-8")
        self.assertEqual(DocumentCodec().encode_sidecar(value), expected)

    def test_sidecar_bytes_decode_to_the_document_and_leave_the_hash_alone(self) -> None:
        from asset_document.canonical.semantic_hash import SemanticHashScope, compute_semantic_hash
        codec = DocumentCodec()
        for value in generated_json_values(seed=73, count=200):
            with self.subTest(value=value):
                self.assertEqual(codec.decode(codec.encode_sidecar(value)), codec.decode(codec.encode(value)))
        document = {"SchemaVersion": 1, "Body": {"Nodes": [{"Id": "A", "Position": [0, 0]}]}}
        compact = compute_semantic_hash(document, scope=SemanticHashScope.DOCUMENT, codec=codec)
        self.assertEqual(compute_semantic_hash(codec.decode(codec.encode_sidecar(document)),
                                               scope=SemanticHashScope.DOCUMENT, codec=codec), compact)


if __name__ == "__main__":
    unittest.main()
