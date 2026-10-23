# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Top-level Properties of the exact profiles (data_table_common.PropertiesRegion): names a profile owns
(Body-owned, editor state, not extracted, derived) are never read through the generic property codec, so
Extract, Diff and the save's fresh verification read the same set and a Body-owned value larger than one
runtime value (a MetaSound's recreated paged frontend document, a Niagara script's version data) does not
fail the read (TypedValueCodec.ConversionFailed -> Persistence.CanonicalMismatch)."""
from __future__ import annotations

import unittest

from asset_document.profiles import data_table_common, material, metasound, niagara


class _Item:
    def __init__(self, name):
        self.name = name


class _RecordingAdapter:
    """Stands in for PropertySemanticAdapter: enumerates names, records which ones were read."""

    def __init__(self, names):
        self.names = names
        self.read = None

    def enumerate(self, class_path, context):
        return tuple(_Item(name) for name in self.names)

    def extract(self, read, admitted, context, include_defaults=True):
        self.read = sorted(item.name for item in admitted)
        return {item.name: 1 for item in admitted}


def _extract(region, names):
    adapter = _RecordingAdapter(names)
    region.adapter = adapter
    return region.extract(read=None, context=None), adapter.read


class PropertiesRegionTests(unittest.TestCase):
    def test_owned_names_are_not_read(self):
        region = data_table_common.PropertiesRegion("/Script/Test.Asset", {"Payload": "Body.Payload"}, "Test",
                                                    ("ThumbnailInfo",))
        values, read = _extract(region, ["Payload", "ThumbnailInfo", "Setting"])
        self.assertEqual(read, ["Setting"])
        self.assertEqual(values, {"Setting": 1})

    def test_metasound_document_and_derived_state_are_not_read(self):
        names = ["RootMetasoundDocument", "ReferencedAssetClassKeys", "Graph", "ThumbnailInfo", "OutputFormat"]
        values, read = _extract(metasound._properties(metasound.SOURCE_CLASS), names)
        self.assertEqual(read, ["OutputFormat"])
        self.assertEqual(values, {"OutputFormat": 1})

    def test_niagara_script_version_data_is_not_read(self):
        values, read = _extract(niagara.script_properties(), ["VersionData", "Usage", "PreviewMovie"])
        self.assertEqual(read, ["PreviewMovie"])

    def test_material_graph_containers_are_not_read(self):
        values, read = _extract(material._properties("/Script/Engine.Material"), ["EditorOnlyData", "ParameterGroupData", "ThumbnailInfo", "TwoSided"])
        self.assertEqual(read, ["TwoSided"])


if __name__ == "__main__":
    unittest.main()
