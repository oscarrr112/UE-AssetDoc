"""Readable automatic layout of graph nodes (pure).

Nodes the document does not place (no ``Position``) and graphs that ask for a relayout
(``"Layout": "Auto"``) are arranged the way people arrange Blueprint graphs:

- Exec flow runs left to right: a node's column is its longest exec path from its band's root.
- Every event or entry (a node with exec pins and no exec link into it) starts its own
  horizontal band; bands are stacked top to bottom in document order, so events never interleave.
- Inside a band a node's first exec output continues its row; each further output (Branch's
  else, Sequence's Then 1, a cast's Cast Failed) opens a new row below what the band uses so far,
  in pin order.
- Pure (data) nodes sit left of the node that first consumes them, one sub-column per step
  upstream, aligned with their consumer and stacked in input pin order.
- A graph without exec flow (an AnimGraph) treats its sinks (nodes whose outputs go nowhere) as
  the band roots.
- Node rectangles use the nodes' real sizes (the editor's widget size, or an estimate from the
  title and pins), so laid-out nodes never overlap.

``place`` puts the nodes to move into a graph that may hold nodes that stay: a cluster of moved
nodes in a band that has a staying node keeps its offset from the nearest staying node (and moves
down until it overlaps nothing); bands with no staying node go below everything as one block (for
a relayout, the block keeps the graph's first node where it is).  Results are deterministic: the same graph gives the same positions.
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Iterable, Mapping, Sequence

GRID = 16
COLUMN_GAP = 80
PURE_GAP = 48
ROW_GAP = 48
PURE_ROW_GAP = 24
BAND_GAP = 160
CLEARANCE = 32
MISC_ROW_WIDTH = 1600


@dataclass(frozen=True, slots=True)
class Box:
    x: float
    y: float
    width: float
    height: float

    @property
    def right(self) -> float:
        return self.x + self.width

    @property
    def bottom(self) -> float:
        return self.y + self.height

    def overlaps(self, other: "Box", margin: float = 0.0) -> bool:
        return (self.x < other.right + margin and other.x < self.right + margin
                and self.y < other.bottom + margin and other.y < self.bottom + margin)

    def moved(self, dx: float, dy: float) -> "Box":
        return Box(self.x + dx, self.y + dy, self.width, self.height)


@dataclass(frozen=True, slots=True)
class LayoutNode:
    id: str
    width: float
    height: float
    has_exec: bool = False      # has exec pins: part of the exec flow
    comment_box: bool = False   # a comment box: laid out like a lone node, never an obstacle


@dataclass(frozen=True, slots=True)
class LayoutEdge:
    """A link from an output pin to an input pin; ``*_pin`` are the pins' indices on their nodes."""
    source: str
    target: str
    exec: bool = False
    source_pin: int = 0
    target_pin: int = 0


def snap(value: float) -> int:
    return int(round(value / GRID)) * GRID


def _snap_up(value: float) -> int:
    """``value`` rounded up to the grid (a shift that must clear what it measured)."""
    return -int(-value // GRID) * GRID


# --------------------------------------------------------------------------
# Ideal layout: bands with local coordinates
# --------------------------------------------------------------------------
@dataclass(slots=True)
class _Band:
    positions: dict          # node id -> (x, y), band-local, top-left at (0, 0)
    width: float = 0.0
    height: float = 0.0


def ideal_bands(nodes: Sequence[LayoutNode], edges: Iterable[LayoutEdge]) -> list[_Band]:
    """The graph laid out as bands (document order of their roots); the last band holds what no
    root reaches (lone data nodes, comment boxes)."""
    order = {node.id: index for index, node in enumerate(nodes)}
    by_id = {node.id: node for node in nodes}
    edges = [edge for edge in edges if edge.source in by_id and edge.target in by_id and edge.source != edge.target]
    exec_flow = any(edge.exec for edge in edges) or any(node.has_exec for node in nodes)
    if exec_flow:
        skeleton = {node.id for node in nodes if node.has_exec and not node.comment_box}
        flow = [edge for edge in edges if edge.exec and edge.source in skeleton and edge.target in skeleton]
    else:
        sources = {edge.source for edge in edges}
        targets = {edge.target for edge in edges}
        skeleton = {node.id for node in nodes if node.id in targets and node.id not in sources and not node.comment_box}
        flow = []
    successors: dict[str, list[LayoutEdge]] = {node_id: [] for node_id in skeleton}
    entered = set()
    for edge in flow:
        successors[edge.source].append(edge)
        entered.add(edge.target)
    for items in successors.values():
        items.sort(key=lambda edge: (edge.source_pin, order[edge.target], edge.target_pin))
    roots = [node.id for node in nodes if node.id in skeleton and node.id not in entered]
    roots += [node.id for node in nodes if node.id in skeleton and node.id in entered]

    band_of: dict[str, int] = {}
    members: list[list[str]] = []
    for root in roots:
        if root in band_of:
            continue
        index = len(members)
        found: list[str] = []
        stack = [root]
        while stack:
            node_id = stack.pop()
            if node_id in band_of:
                continue
            band_of[node_id] = index
            found.append(node_id)
            stack.extend(edge.target for edge in reversed(successors[node_id]) if edge.target not in band_of)
        members.append(found)

    data_in: dict[str, list[LayoutEdge]] = {node.id: [] for node in nodes}
    for edge in edges:
        if not edge.exec:
            data_in[edge.target].append(edge)
    for items in data_in.values():
        items.sort(key=lambda edge: (edge.target_pin, order[edge.source], edge.source_pin))
    claimed: set[str] = set(skeleton)

    bands = []
    for index, band_members in enumerate(members):
        bands.append(_lay_band(band_members[0], index, band_of, successors, data_in, claimed, by_id, order))
    rest = [node for node in nodes if node.id not in claimed]
    if rest:
        bands.append(_lay_rest(rest))
    return bands


def _lay_band(root: str, index: int, band_of, successors, data_in, claimed, by_id, order) -> _Band:
    # Rows (depth-first, first output continues the row) and back edges.
    row = {root: 0}
    visit = [root]
    state = {root: 1}  # 1 on the stack, 2 done
    back: set[tuple[str, str]] = set()
    continued: set[str] = set()
    lowest = 0
    stack = [(root, iter(successors[root]))]
    while stack:
        node_id, items = stack[-1]
        for edge in items:
            target = edge.target
            if band_of.get(target) != index:
                continue
            if state.get(target) == 1:
                back.add((node_id, target))
                continue
            first = node_id not in continued
            continued.add(node_id)
            if state.get(target) == 2:
                continue
            if first:
                row[target] = row[node_id]
            else:
                lowest += 1
                row[target] = lowest
            state[target] = 1
            visit.append(target)
            stack.append((target, iter(successors[target])))
            break
        else:
            state[node_id] = 2
            stack.pop()
    # Columns: longest exec path over the band's forward edges.
    column = {node_id: 0 for node_id in visit}
    incoming: dict[str, int] = {node_id: 0 for node_id in visit}
    forward: dict[str, list[str]] = {node_id: [] for node_id in visit}
    for node_id in visit:
        for edge in successors[node_id]:
            if edge.target in incoming and (node_id, edge.target) not in back:
                forward[node_id].append(edge.target)
                incoming[edge.target] += 1
    ready = [node_id for node_id in visit if incoming[node_id] == 0]
    while ready:
        node_id = ready.pop(0)
        for target in forward[node_id]:
            column[target] = max(column[target], column[node_id] + 1)
            incoming[target] -= 1
            if incoming[target] == 0:
                ready.append(target)

    # Pure trees: each data node left of the node that first consumes it.
    tree: dict[str, list[tuple[str, str, int]]] = {}
    for owner in visit:
        items: list[tuple[str, str, int]] = []
        pending = [(edge.source, owner, 1) for edge in reversed(data_in[owner])]
        while pending:
            source, consumer, depth = pending.pop()
            if source in claimed or by_id[source].comment_box:
                continue
            claimed.add(source)
            items.append((source, consumer, depth))
            pending.extend((edge.source, source, depth + 1) for edge in reversed(data_in[source]))
        tree[owner] = items

    columns = max(column.values()) + 1
    exec_width = [0.0] * columns
    pure_width: list[dict[int, float]] = [{} for _ in range(columns)]
    for node_id in visit:
        c = column[node_id]
        exec_width[c] = max(exec_width[c], by_id[node_id].width)
        for source, _, depth in tree[node_id]:
            pure_width[c][depth] = max(pure_width[c].get(depth, 0.0), by_id[source].width)
    exec_x = [0.0] * columns
    left = 0.0
    for c in range(columns):
        area = sum(width + PURE_GAP for width in pure_width[c].values())
        exec_x[c] = left + area
        left = exec_x[c] + exec_width[c] + COLUMN_GAP

    # Band-relative boxes per row, rows placed on a per-column skyline.
    relative: dict[str, tuple[float, float]] = {}
    rows: dict[int, list[str]] = {}
    for node_id in visit:
        rows.setdefault(row[node_id], []).append(node_id)
    height_of_row: dict[int, float] = {}
    for r, row_nodes in rows.items():
        bottom = 0.0
        for owner in row_nodes:
            c = column[owner]
            relative[owner] = (exec_x[c], 0.0)
            bottom = max(bottom, by_id[owner].height)
            cursor: dict[int, float] = {}
            for source, consumer, depth in tree[owner]:
                right = exec_x[c] - PURE_GAP - sum(pure_width[c][k] + PURE_GAP for k in range(1, depth))
                wanted = relative[consumer][1]
                y = max(wanted, cursor.get(depth, 0.0))
                cursor[depth] = y + by_id[source].height + PURE_ROW_GAP
                relative[source] = (right - by_id[source].width, y)
                bottom = max(bottom, y + by_id[source].height)
        height_of_row[r] = bottom
    skyline = [0.0] * columns
    positions: dict[str, tuple[float, float]] = {}
    for r in sorted(rows):
        span = [column[node_id] for node_id in rows[r]]
        low, high = min(span), max(span)
        top = max(skyline[low:high + 1])
        for c in range(low, high + 1):
            skyline[c] = top + height_of_row[r] + ROW_GAP
        for owner in rows[r]:
            for node_id in [owner] + [source for source, _, _ in tree[owner]]:
                x, y = relative[node_id]
                positions[node_id] = (x, top + y)
    return _normalized(positions, by_id)


def _lay_rest(rest: Sequence[LayoutNode]) -> _Band:
    """Nodes no band holds, in document order, wrapped into rows."""
    positions = {}
    x = y = row_height = 0.0
    for node in rest:
        if x > 0 and x + node.width > MISC_ROW_WIDTH:
            x, y, row_height = 0.0, y + row_height + ROW_GAP, 0.0
        positions[node.id] = (x, y)
        x += node.width + COLUMN_GAP
        row_height = max(row_height, node.height)
    return _normalized(positions, {node.id: node for node in rest})


def _normalized(positions: dict, by_id) -> _Band:
    low_x = min(x for x, _ in positions.values())
    low_y = min(y for _, y in positions.values())
    moved = {node_id: (x - low_x, y - low_y) for node_id, (x, y) in positions.items()}
    width = max(x + by_id[node_id].width for node_id, (x, _) in moved.items())
    height = max(y + by_id[node_id].height for node_id, (_, y) in moved.items())
    return _Band(moved, width, height)


# --------------------------------------------------------------------------
# Placement among nodes that stay
# --------------------------------------------------------------------------
def _settle(boxes: list[Box], occupied: list[Box]) -> float:
    """How far ``boxes`` move down (together) so none overlaps ``occupied`` (with clearance)."""
    shift = 0.0
    for _ in range(10000):
        needed = 0.0
        for box in boxes:
            moved = box.moved(0.0, shift)
            for other in occupied:
                if moved.overlaps(other, CLEARANCE):
                    needed = max(needed, other.bottom + CLEARANCE - moved.y)
        if needed <= 0.0:
            return shift
        shift += needed
    return shift


def place(nodes: Sequence[LayoutNode], edges: Iterable[LayoutEdge], *, movable: Iterable[str],
          fixed: Mapping[str, tuple[float, float]], obstacles: Sequence[Box] = (),
          keep: tuple[str, tuple[float, float]] | None = None) -> dict[str, tuple[int, int]]:
    """Positions (grid-snapped top-left corners) for the ``movable`` nodes.

    ``fixed``: where the other nodes are (they stay); ``obstacles``: further areas to keep clear
    (nodes outside the layout, such as the editor's disabled default events).  The bands that hold
    no fixed node go below everything as one block; ``keep`` (a movable node and its position)
    places that block so the node stays where it is (a relayout) instead.
    """
    edges = list(edges)
    wanted = set(movable)
    movable = {node.id for node in nodes if node.id in wanted}
    if not movable:
        return {}
    by_id = {node.id: node for node in nodes}
    order = {node.id: index for index, node in enumerate(nodes)}
    # Band-local positions on the grid, and every offset below too: a node then lands exactly where the
    # same layout put it last time (a relayout of a laid-out graph moves nothing).
    bands = ideal_bands(nodes, edges)
    for band in bands:
        band.positions = {node_id: (snap(x), snap(y)) for node_id, (x, y) in band.positions.items()}
    occupied: list[Box] = list(obstacles)
    for node_id, (x, y) in fixed.items():
        node = by_id.get(node_id)
        if node is not None and node_id not in movable and not node.comment_box:
            occupied.append(Box(x, y, node.width, node.height))
    neighbours: dict[str, list[str]] = {node.id: [] for node in nodes}
    for edge in edges:
        if edge.source in neighbours and edge.target in neighbours:
            neighbours[edge.source].append(edge.target)
            neighbours[edge.target].append(edge.source)
    for items in neighbours.values():
        items.sort(key=order.__getitem__)
    output: dict[str, tuple[int, int]] = {}

    def commit(ids, dx, dy, band):
        for node_id in ids:
            x, y = band.positions[node_id]
            output[node_id] = (snap(x + dx), snap(y + dy))
            node = by_id[node_id]
            if not node.comment_box:
                occupied.append(Box(output[node_id][0], output[node_id][1], node.width, node.height))

    free = []
    for band in bands:
        moving = [node_id for node_id in band.positions if node_id in movable]
        staying = [node_id for node_id in band.positions if node_id not in movable and node_id in fixed]
        if not moving:
            continue
        if not staying:
            free.append(band)
            continue
        for cluster in _clusters(moving, neighbours, movable):
            anchor = _nearest(cluster, neighbours, set(staying), list(band.positions))
            ax, ay = band.positions[anchor]
            fx, fy = fixed[anchor]
            dx, dy = fx - ax, fy - ay
            boxes = [Box(band.positions[node_id][0] + dx, band.positions[node_id][1] + dy,
                         by_id[node_id].width, by_id[node_id].height)
                     for node_id in cluster if not by_id[node_id].comment_box]
            commit(cluster, dx, dy + _snap_up(_settle(boxes, occupied)), band)
    if free:
        stacked: list[tuple[_Band, float]] = []
        cursor = 0.0
        for band in free:
            stacked.append((band, cursor))
            cursor = _snap_up(cursor + band.height + BAND_GAP)
        origin = None
        if keep is not None:
            for band, offset in stacked:
                if keep[0] in band.positions:
                    x, y = band.positions[keep[0]]
                    origin = (keep[1][0] - x, keep[1][1] - offset - y)
        if origin is None:
            if occupied:
                origin = (min(box.x for box in occupied), max(box.bottom for box in occupied) + BAND_GAP)
            else:
                origin = (0.0, 0.0)
        origin = (snap(origin[0]), snap(origin[1]))
        boxes = [Box(band.positions[node_id][0] + origin[0], band.positions[node_id][1] + offset + origin[1],
                     by_id[node_id].width, by_id[node_id].height)
                 for band, offset in stacked for node_id in band.positions if not by_id[node_id].comment_box]
        shift = _snap_up(_settle(boxes, occupied))
        for band, offset in stacked:
            commit([node_id for node_id in band.positions if node_id in movable], origin[0],
                   origin[1] + offset + shift, band)
    return output


def _clusters(moving: list[str], neighbours, movable: set[str]) -> list[list[str]]:
    seen: set[str] = set()
    output = []
    for start in moving:
        if start in seen:
            continue
        seen.add(start)
        cluster, pending = [], [start]
        while pending:
            node_id = pending.pop(0)
            cluster.append(node_id)
            for other in neighbours[node_id]:
                if other in movable and other not in seen:
                    seen.add(other)
                    pending.append(other)
        output.append(cluster)
    return output


def _nearest(cluster: list[str], neighbours, staying: set[str], band: list[str]) -> str:
    """The staying node of the band closest (in links) to the cluster; else the band's first staying node."""
    members = set(band)
    seen = set(cluster)
    pending = list(cluster)
    while pending:
        node_id = pending.pop(0)
        for other in neighbours[node_id]:
            if other in seen or other not in members:
                continue
            if other in staying:
                return other
            seen.add(other)
            pending.append(other)
    return next(node_id for node_id in band if node_id in staying)


# --------------------------------------------------------------------------
# Quality metrics
# --------------------------------------------------------------------------
def overlap_count(boxes: Mapping[str, Box]) -> int:
    items = list(boxes.values())
    return sum(1 for i, a in enumerate(items) for b in items[i + 1:] if a.overlaps(b))


def crossing_count(boxes: Mapping[str, Box], edges: Iterable[LayoutEdge]) -> int:
    """Straight wires (output: right edge, input: left edge, both at the node's middle) that cross."""
    segments = []
    for edge in edges:
        a, b = boxes.get(edge.source), boxes.get(edge.target)
        if a is None or b is None or edge.source == edge.target:
            continue
        segments.append((edge.source, edge.target, (a.right, a.y + a.height / 2), (b.x, b.y + b.height / 2)))
    count = 0
    for i, (s1, t1, p1, q1) in enumerate(segments):
        for s2, t2, p2, q2 in segments[i + 1:]:
            if {s1, t1} & {s2, t2}:
                continue
            if _cross(p1, q1, p2, q2):
                count += 1
    return count


def _cross(p1, q1, p2, q2) -> bool:
    def side(a, b, c):
        value = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
        return (value > 0) - (value < 0)
    return side(p1, q1, p2) * side(p1, q1, q2) < 0 and side(p2, q2, p1) * side(p2, q2, q1) < 0


# --------------------------------------------------------------------------
# Node sizes
# --------------------------------------------------------------------------
def estimate_size(title: str, inputs: Sequence[str], outputs: Sequence[str]) -> tuple[float, float]:
    """A generous estimate of a graph node's drawn size when the editor cannot measure it."""
    lines = (title or "").splitlines() or [""]
    widest_title = max(len(line) for line in lines)
    widest_in = max((len(name) for name in inputs), default=0)
    widest_out = max((len(name) for name in outputs), default=0)
    width = max(120.0, 64.0 + 8.0 * widest_title,
                (36.0 + 7.5 * widest_in + 96.0 if inputs else 0.0) + (36.0 + 7.5 * widest_out if outputs else 0.0) + 24.0)
    height = 40.0 + 20.0 * (len(lines) - 1) + 26.0 * max(len(inputs), len(outputs), 1) + 12.0
    return float(min(width, 640.0)), float(height)


# --------------------------------------------------------------------------
# Live graphs
# --------------------------------------------------------------------------
def live_node_size(node) -> tuple[float, float]:
    """A live node's drawn size: the editor's measure, a comment box's own size, else an estimate."""
    if node.box_size is not None:
        return float(node.box_size[0]), float(node.box_size[1])
    if node.widget_size is not None:
        return float(node.widget_size[0]), float(node.widget_size[1])
    visible = [pin for pin in node.pins if not pin.hidden]
    return estimate_size(node.title, [pin.display_name or pin.name for pin in visible if pin.direction == "input"],
                         [pin.display_name or pin.name for pin in visible if pin.direction == "output"])


def live_layout(graph, order: Sequence[str]) -> tuple[list[LayoutNode], list[LayoutEdge]]:
    """Layout nodes (by GUID, in ``order``) and the links between them of a live graph."""
    by_guid = {node.guid: node for node in graph.nodes}
    nodes = []
    for guid in order:
        node = by_guid.get(guid)
        if node is None:
            continue
        width, height = live_node_size(node)
        nodes.append(LayoutNode(guid, width, height, any(pin.type.category == "exec" for pin in node.pins),
                                node.box_size is not None))
    included = {node.id for node in nodes}
    edges = []
    for link in graph.links:
        source, target = by_guid.get(link.from_.node_guid), by_guid.get(link.to.node_guid)
        if source is None or target is None or source.guid not in included or target.guid not in included:
            continue
        out_index = next((index for index, pin in enumerate(source.pins)
                          if pin.name == link.from_.pin_name and pin.direction == "output"), 0)
        in_index = next((index for index, pin in enumerate(target.pins)
                         if pin.name == link.to.pin_name and pin.direction == "input"), 0)
        exec_pin = source.pins[out_index].type.category == "exec" if source.pins else False
        edges.append(LayoutEdge(source.guid, target.guid, exec_pin, out_index, in_index))
    return nodes, edges


def live_positions(graph, *, order: Sequence[str], movable: Iterable[str],
                   positions: Mapping[str, tuple[float, float]] | None = None, obstacles: Iterable[str] = (),
                   keep: str | None = None) -> dict[str, tuple[int, int]]:
    """New top-left corners for the ``movable`` live nodes (GUIDs) of ``graph``.

    ``order``: the GUIDs laid out together, in document order; ``positions``: where a staying node
    will be when that is not where it is now (a stated Position not applied yet); ``obstacles``:
    GUIDs of other live nodes to keep clear of; ``keep``: a movable node that stays where it is now
    (a relayout keeps the graph's first node in place; default: new bands go below everything).
    """
    by_guid = {node.guid: node for node in graph.nodes}
    nodes, edges = live_layout(graph, order)
    movable = {guid for guid in movable if guid in by_guid}
    fixed = {}
    for node in nodes:
        if node.id in movable:
            continue
        position = (positions or {}).get(node.id)
        if position is None and by_guid[node.id].position is not None:
            position = (by_guid[node.id].position.x, by_guid[node.id].position.y)
        if position is not None:
            fixed[node.id] = position
    blocked = []
    for guid in obstacles:
        node = by_guid.get(guid)
        if node is not None and node.position is not None and node.box_size is None:
            width, height = live_node_size(node)
            blocked.append(Box(node.position.x, node.position.y, width, height))
    kept = by_guid.get(keep) if keep is not None else None
    anchor = (keep, (kept.position.x, kept.position.y)) if kept is not None and kept.position is not None else None
    return place(nodes, edges, movable=movable, fixed=fixed, obstacles=blocked, keep=anchor)
