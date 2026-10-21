"""Case-only renames of Blueprint members (variables, event dispatchers, own components, timelines), pure.

Names are matched case-insensitively (FName), so ``ondied`` -> ``OnDied`` looks like "the same member" to
every planner.  The editor still applies it (``SBlueprintPaletteItem::OnNameTextCommitted`` ->
``FBlueprintEditorUtils::RenameMemberVariable`` / ``RenameTimeline``; the component Details name box ->
``RenameComponentMemberVariable``), and the member stays the same object, so references to it follow:
``blueprint.member.rename.v1`` performs that rename and these planners emit it when the authored name
differs from the live name only by case.  Graph names (functions, macros, event graphs) go through the
graph executor's ``graph.rename``.  Any other new name is still a different member (remove + add).
"""
from __future__ import annotations

PRIMITIVES = ("rename",)
_KINDS = (
    # (kind, Body key, query key, live name field)
    ("variable", "Variables", "variables", "name"),
    ("dispatcher", "EventDispatchers", "dispatchers", "name"),
    ("component", "Components", "components", "name"),
    ("timeline", "Timelines", "timelines", "name"),
)


def case_only(wanted: str, live: str) -> bool:
    return wanted != live and wanted.casefold() == live.casefold()


def plan_renames(body: dict, query: dict) -> list:
    """``blueprint.member.rename.v1`` ops for every listed member whose casing differs from the live member."""
    ops = []
    for kind, key, query_key, field in _KINDS:
        rows = body.get(key)
        if rows is None:
            continue
        live = {}
        for item in query.get(query_key, []):
            if kind != "component" or item.get("scope") == "owned":
                live[item[field].casefold()] = item[field]
        for index, row in enumerate(rows):
            if kind == "component" and "Scope" in row:
                continue
            have = live.get(row["Name"].casefold())
            if have is not None and case_only(row["Name"], have):
                ops.append(("blueprint.member.rename.v1", {"kind": kind, "name": have, "new_name": row["Name"]},
                            f"/Body/{key}/{index}/Name"))
    return ops

