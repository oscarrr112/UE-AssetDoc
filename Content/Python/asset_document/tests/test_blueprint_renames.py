from __future__ import annotations

import unittest

from asset_document.profiles import blueprint_dispatchers as dispatchers
from asset_document.profiles import blueprint_renames as renames
from asset_document.profiles import blueprint_timelines as timelines
from asset_document.profiles import ublueprint_members as members

FLOAT = {"category": "real", "sub_category": "double"}


def query():
    return {
        "variables": [{"name": "health", "type": FLOAT}],
        "dispatchers": [{"name": "ondied", "inputs": []}],
        "components": [{"name": "mesh", "scope": "owned", "class": "/Script/Engine.StaticMeshComponent"},
                       {"name": "CharacterMesh0", "scope": "native"}],
        "timelines": [{"name": "fade", "length": 1.0, "tracks": []}],
    }


class BlueprintRenamePlanTests(unittest.TestCase):
    def test_case_only_changes_become_renames_of_the_same_member(self):
        body = {"Variables": [{"Name": "Health", "Type": "float"}],
                "EventDispatchers": [{"Name": "OnDied"}],
                "Components": [{"Name": "Mesh", "Class": "/Script/Engine.StaticMeshComponent"},
                               {"Name": "charactermesh0", "Scope": "Native"}],
                "Timelines": [{"Name": "Fade"}]}
        self.assertEqual(renames.plan_renames(body, query()), [
            ("blueprint.member.rename.v1", {"kind": "variable", "name": "health", "new_name": "Health"}, "/Body/Variables/0/Name"),
            ("blueprint.member.rename.v1", {"kind": "dispatcher", "name": "ondied", "new_name": "OnDied"}, "/Body/EventDispatchers/0/Name"),
            ("blueprint.member.rename.v1", {"kind": "component", "name": "mesh", "new_name": "Mesh"}, "/Body/Components/0/Name"),
            ("blueprint.member.rename.v1", {"kind": "timeline", "name": "fade", "new_name": "Fade"}, "/Body/Timelines/0/Name"),
        ])

    def test_same_casing_other_names_and_omitted_regions_plan_nothing(self):
        body = {"Variables": [{"Name": "health", "Type": "float"}, {"Name": "Other", "Type": "float"}]}
        self.assertEqual(renames.plan_renames(body, query()), [])
        self.assertEqual(renames.plan_renames({}, query()), [])

    def test_diff_sees_a_case_only_change(self):
        variables = [{"Name": "health", "Type": "float"}]
        self.assertTrue(members._variables_match(variables, variables))
        self.assertFalse(members._variables_match([{"Name": "Health", "Type": "float"}], variables))
        component = {"Name": "mesh", "Class": "/Script/Engine.StaticMeshComponent"}
        self.assertTrue(members._components_match([component], [component]))
        self.assertFalse(members._components_match([{**component, "Name": "Mesh"}], [component]))
        self.assertTrue(dispatchers.rows_match([{"Name": "ondied"}], [{"Name": "ondied"}]))
        self.assertFalse(dispatchers.rows_match([{"Name": "OnDied"}], [{"Name": "ondied"}]))
        self.assertTrue(timelines.rows_match([{"Name": "fade"}], [{"Name": "fade"}]))
        self.assertFalse(timelines.rows_match([{"Name": "Fade"}], [{"Name": "fade"}]))


if __name__ == "__main__":
    unittest.main()
