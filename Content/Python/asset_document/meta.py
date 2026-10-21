from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
import re

from asset_document.diagnostics import contract_error
from asset_document.dto import PrimitiveRequirement


class DocumentAction(str, Enum):
    CREATE = "Create"
    UPDATE = "Update"
    CREATE_OR_UPDATE = "CreateOrUpdate"


class RegionOwnership(str, Enum):
    MANAGED = "Managed"
    PRESERVED = "Preserved"
    OBSERVED = "Observed"


class IdentityPolicy(str, Enum):
    POSITIONAL = "Positional"
    STABLE_ID = "StableId"
    STABLE_KEY = "StableKey"


class OrderPolicy(str, Enum):
    PRESERVE = "Preserve"
    CANONICAL = "Canonical"
    IDENTITY_KEYED = "IdentityKeyed"


class DefaultPolicy(str, Enum):
    PRESERVE = "Preserve"
    ENGINE_DEFAULT = "EngineDefault"
    EXPLICIT = "Explicit"


class OmissionPolicy(str, Enum):
    PRESERVE = "Preserve"
    OMIT_DEFAULT = "OmitDefault"
    ABSENT_BY_POLICY = "AbsentByPolicy"


class DeletePolicy(str, Enum):
    FORBID = "Forbid"
    EXPLICIT_ONLY = "ExplicitOnly"
    OMISSION_DELETES = "OmissionDeletes"


class ComparisonPolicy(str, Enum):
    EXACT = "Exact"
    CANONICAL = "Canonical"
    ORDERED = "Ordered"


def _invalid_type(path: str, message: str):
    return contract_error(path, "Meta.InvalidDeclarationType", message)


def _require_string(value: object, path: str, label: str) -> None:
    if type(value) is not str:
        raise _invalid_type(path, f"{label} must be an exact string.")


def _require_enum(value: object, expected: type[Enum], path: str, label: str) -> None:
    if type(value) is not expected:
        raise _invalid_type(path, f"{label} must be an exact {expected.__name__}.")


def _valid_body_path(path: str) -> bool:
    return (
        (path == "/Body" or path.startswith("/Body/"))
        and re.fullmatch(r"(?:/(?:[^~/]|~[01])*)*", path) is not None
    )


def _require_declaration_tuple(value: object, expected: type, path: str, label: str) -> None:
    if type(value) is not tuple:
        raise _invalid_type(path, f"{label} must be an exact tuple.")
    if any(type(item) is not expected for item in value):
        raise _invalid_type(path, f"Every {label} item must be exact {expected.__name__}.")


def _validate_region_uniqueness(regions: tuple[RegionMeta, ...]) -> None:
    region_ids = [region.region_id for region in regions]
    if len(region_ids) != len(set(region_ids)):
        raise contract_error(
            "/Regions", "Meta.DuplicateRegionId", "Region IDs must be unique."
        )
    body_paths = [region.body_path for region in regions]
    if len(body_paths) != len(set(body_paths)):
        raise contract_error(
            "/Regions", "Meta.DuplicateBodyPath", "Body paths must have one owner."
        )


def _validate_requirements(profile: ProfileMeta) -> None:
    if len(profile.required_primitives) != len(set(profile.required_primitives)):
        raise contract_error(
            "/RequiredPrimitives",
            "Meta.DuplicatePrimitiveRequirement",
            "Primitive requirement triples must be unique within a Profile.",
        )
    if any(
        requirement.required_by_route_id != profile.route_id
        for requirement in profile.required_primitives
    ):
        raise contract_error(
            "/RequiredPrimitives",
            "Meta.RequirementRouteMismatch",
            "Every requirement must name its owning Profile route.",
        )


@dataclass(frozen=True, slots=True)
class DocumentMeta:
    schema_version: int
    target: str
    exact_asset_class_path: str
    action: DocumentAction

    def __post_init__(self) -> None:
        if type(self.schema_version) is not int:
            raise _invalid_type("/SchemaVersion", "schema_version must be an exact int.")
        if self.schema_version <= 0:
            raise contract_error(
                "/SchemaVersion", "Meta.InvalidSchemaVersion", "Schema version must be positive."
            )
        _require_string(self.target, "/Target", "target")
        if not self.target:
            raise contract_error("/Target", "Meta.EmptyTarget", "Target must not be empty.")
        _require_string(self.exact_asset_class_path, "/Class", "exact_asset_class_path")
        if not self.exact_asset_class_path:
            raise contract_error(
                "/Class", "Meta.EmptyExactClassPath", "Exact asset class path must not be empty."
            )
        _require_enum(self.action, DocumentAction, "/Action", "action")


@dataclass(frozen=True, slots=True)
class RegionMeta:
    region_id: str
    body_path: str
    ownership: RegionOwnership
    identity: IdentityPolicy
    order: OrderPolicy
    default: DefaultPolicy
    omission: OmissionPolicy
    deletion: DeletePolicy
    comparison: ComparisonPolicy

    def __post_init__(self) -> None:
        _require_string(self.region_id, "/Regions", "region_id")
        if not self.region_id:
            raise contract_error(
                "/Regions", "Meta.EmptyRegionId", "Region ID must not be empty."
            )
        body_path_error = f"/Regions/{self.region_id}/BodyPath"
        _require_string(self.body_path, body_path_error, "body_path")
        if not _valid_body_path(self.body_path):
            raise contract_error(
                body_path_error,
                "Meta.InvalidBodyPath",
                "Body path must be a valid RFC 6901 pointer rooted at /Body.",
            )
        for value, expected, suffix, label in (
            (self.ownership, RegionOwnership, "Ownership", "ownership"),
            (self.identity, IdentityPolicy, "Identity", "identity"),
            (self.order, OrderPolicy, "Order", "order"),
            (self.default, DefaultPolicy, "Default", "default"),
            (self.omission, OmissionPolicy, "Omission", "omission"),
            (self.deletion, DeletePolicy, "Deletion", "deletion"),
            (self.comparison, ComparisonPolicy, "Comparison", "comparison"),
        ):
            _require_enum(
                value, expected, f"/Regions/{self.region_id}/{suffix}", label
            )


@dataclass(frozen=True, slots=True)
class ProfileMeta:
    profile_id: str
    route_id: str
    exact_asset_class_path: str
    regions: tuple[RegionMeta, ...]
    required_primitives: tuple[PrimitiveRequirement, ...]

    def __post_init__(self) -> None:
        _require_string(self.profile_id, "/ProfileId", "profile_id")
        if not self.profile_id:
            raise contract_error(
                "/ProfileId", "Meta.EmptyProfileId", "Profile ID must not be empty."
            )
        _require_string(self.route_id, "/RouteId", "route_id")
        if not self.route_id:
            raise contract_error(
                "/RouteId", "Meta.EmptyRouteId", "Route ID must not be empty."
            )
        _require_string(self.exact_asset_class_path, "/Class", "exact_asset_class_path")
        if not self.exact_asset_class_path:
            raise contract_error(
                "/Class", "Meta.EmptyExactClassPath", "Exact asset class path must not be empty."
            )
        _require_declaration_tuple(self.regions, RegionMeta, "/Regions", "regions")
        _require_declaration_tuple(
            self.required_primitives,
            PrimitiveRequirement,
            "/RequiredPrimitives",
            "required primitives",
        )
        _validate_region_uniqueness(self.regions)
        _validate_requirements(self)
