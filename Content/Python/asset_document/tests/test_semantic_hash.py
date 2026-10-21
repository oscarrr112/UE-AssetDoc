import math
import unittest

from asset_document.canonical.document_codec import DocumentCodec
from asset_document.canonical.semantic_hash import SemanticHashScope, compute_semantic_hash
from asset_document.diagnostics import AssetDocumentContractError
from asset_document.tests.property_cases import generated_json_values


def assert_hash_error(test: unittest.TestCase, code: str, path: str, callback) -> None:
    with test.assertRaises(AssetDocumentContractError) as caught:
        callback()
    test.assertEqual((caught.exception.diagnostic.code, caught.exception.diagnostic.path), (code, path))


class SemanticHashTests(unittest.TestCase):
    def test_golden_document_and_region_hashes(self) -> None:
        value = {"Body": {"Count": 1, "Enabled": True}, "SchemaVersion": 1}
        self.assertEqual(
            compute_semantic_hash(value, scope=SemanticHashScope.DOCUMENT),
            "adsh:v1:document:da0e68b5f5b0a01b06aee1407bef2fe5ce2df4c1d0210ec64b0cb4beaa96a846",
        )
        self.assertEqual(
            compute_semantic_hash(value, scope=SemanticHashScope.REGION),
            "adsh:v1:region:fddf12a057d02b32d2e185037df9a2fc64f90ddecec322ff9d055576dd67c7d5",
        )

    def test_domain_shape_and_key_order_rules(self) -> None:
        left = {"A": 1, "B": [True, None]}
        reordered = {"B": [True, None], "A": 1}
        self.assertEqual(
            compute_semantic_hash(left, scope=SemanticHashScope.DOCUMENT),
            compute_semantic_hash(reordered, scope=SemanticHashScope.DOCUMENT),
        )
        self.assertNotEqual(
            compute_semantic_hash(1, scope=SemanticHashScope.DOCUMENT),
            compute_semantic_hash(1.0, scope=SemanticHashScope.DOCUMENT),
        )
        self.assertNotEqual(
            compute_semantic_hash(left, scope=SemanticHashScope.DOCUMENT),
            compute_semantic_hash(left, scope=SemanticHashScope.REGION),
        )
        self.assertTrue(
            compute_semantic_hash(left, scope=SemanticHashScope.DOCUMENT).startswith(
                "adsh:v1:document:"
            )
        )

    def test_generated_values_are_stable_across_codec_roundtrip(self) -> None:
        codec = DocumentCodec()
        for value in generated_json_values(seed=0xAD5A, count=512):
            with self.subTest(value=value):
                first = compute_semantic_hash(value, scope=SemanticHashScope.DOCUMENT)
                second = compute_semantic_hash(value, scope=SemanticHashScope.DOCUMENT)
                roundtrip = codec.decode(codec.encode(value))
                self.assertEqual(first, second)
                self.assertEqual(
                    first,
                    compute_semantic_hash(roundtrip, scope=SemanticHashScope.DOCUMENT),
                )

    def test_scope_requires_exact_enum(self) -> None:
        for scope in ("document", object()):
            with self.subTest(scope=scope):
                assert_hash_error(
                    self, "Hash.InvalidScope", "/SemanticHash/Scope",
                    lambda scope=scope: compute_semantic_hash({}, scope=scope),
                )

    def test_signed_zero_hashes_are_distinct_and_stable(self) -> None:
        negative = compute_semantic_hash(-0.0, scope=SemanticHashScope.DOCUMENT)
        positive = compute_semantic_hash(0.0, scope=SemanticHashScope.DOCUMENT)
        self.assertEqual(
            negative,
            "adsh:v1:document:91abb1897de37f68a98d589925b739f8b2d56ec8e4fb9e01df723a4dcfe5f584",
        )
        self.assertEqual(
            positive,
            "adsh:v1:document:3e18c65c6da2f4be99753c9750fdd5d5ab39b589ff162c9fa370c836cee59727",
        )
        self.assertNotEqual(negative, positive)
        self.assertEqual(DocumentCodec().encode(-0.0), b"-0.0")
        self.assertEqual(DocumentCodec().encode(0.0), b"0.0")

    def test_invalid_values_preserve_codec_diagnostic(self) -> None:
        assert_hash_error(
            self, "Codec.NonFiniteNumber", "/Body",
            lambda: compute_semantic_hash(
                {"Body": math.nan}, scope=SemanticHashScope.DOCUMENT
            ),
        )


if __name__ == "__main__":
    unittest.main()
