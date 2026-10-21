import random

from asset_document.dto import JsonValue


_KEYS = (
    "A",
    "B",
    "Body",
    "Key0",
    "Key1",
    "雪",
    "é",
    "Tilde~",
    "Slash/",
)
_STRINGS = ("", "ascii", "雪", "é", "~", "/", "mixed雪/é~")


def _generated_scalar(generator: random.Random) -> JsonValue:
    kind = generator.randrange(6)
    if kind == 0:
        return None
    if kind == 1:
        return bool(generator.randrange(2))
    if kind == 2:
        return generator.randint(-(2**31), 2**31 - 1)
    if kind == 3:
        return generator.randint(-100000, 100000) / 8
    if kind == 4:
        return _STRINGS[generator.randrange(len(_STRINGS))]
    return generator.choice((-(2**63), 2**63 - 1))


def _generated_value(generator: random.Random, depth: int) -> JsonValue:
    if depth >= 4:
        return _generated_scalar(generator)
    kind = generator.randrange(3)
    if kind == 0:
        return _generated_scalar(generator)
    if kind == 1:
        return [
            _generated_value(generator, depth + 1)
            for _ in range(generator.randint(0, 4))
        ]
    width = generator.randint(0, 4)
    keys = generator.sample(_KEYS, width)
    return {key: _generated_value(generator, depth + 1) for key in keys}


def generated_json_values(seed: int, count: int) -> tuple[JsonValue, ...]:
    generator = random.Random(seed)
    return tuple(_generated_value(generator, 0) for _ in range(count))
