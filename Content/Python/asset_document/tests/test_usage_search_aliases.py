"""find_assets `uses` normalizes action-key aliases in C++ (AssetDocumentGraphUsageScanner.cpp); its table
mirrors asset_document.graphs.actions.ALIASES and must stay in step with it."""
from __future__ import annotations

import re
import unittest
from pathlib import Path

from asset_document.graphs.actions import ALIASES

_SCANNER = (Path(__file__).resolve().parents[4] / "Source" / "AssetDocumentGraph" / "Private"
            / "AssetDocumentGraphUsageScanner.cpp")
_ENTRY = re.compile(r'\{TEXT\("(K2:[^"]+)"\), TEXT\("([^"]+)"\), (nullptr|\w+|TEXT\("[^"]*"\)), (nullptr|TEXT\("[^"]*"\))\}')
_CONSTANT = re.compile(r'const TCHAR\* const (\w+) = TEXT\("([^"]+)"\);')


def _text(token: str, constants: dict[str, str]) -> str | None:
    if token == "nullptr":
        return None
    if token.startswith('TEXT("'):
        return token[len('TEXT("'):-2]
    return constants[token]


class UsageSearchAliasTests(unittest.TestCase):
    def test_usage_search_aliases_match_python(self):
        if not _SCANNER.is_file():
            self.skipTest("plugin sources are not next to the Python package")
        source = _SCANNER.read_text(encoding="utf-8")
        constants = dict(_CONSTANT.findall(source))
        table = {}
        for alias, kind, owner, field in _ENTRY.findall(source):
            owner_text, field_text = _text(owner, constants), _text(field, constants)
            if owner_text is None:
                table[alias] = kind
            else:
                table[alias] = ":".join(part for part in ("K2", kind, owner_text, field_text) if part)
        self.assertEqual(table, ALIASES)


if __name__ == "__main__":
    unittest.main()
