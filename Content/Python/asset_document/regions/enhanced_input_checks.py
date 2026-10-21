"""Enhanced Input checks of the generic profile (Input Action / Input Mapping Context).

The editor accepts all of these, so they are warnings, never refusals: a modifier
that does not fit the action's value type (a Swizzle on a Boolean / Axis1D action),
a mapping without an action, and Blueprint input-action nodes whose value pin no
longer matches the action's ValueType. The native check reads the preflight
scratch after the property preflight wrote the document's values into it.
"""

from __future__ import annotations

from asset_document.diagnostics import AssetDocumentContractError, DiagnosticSeverity, RuntimeDiagnostic
from asset_document.regions.primitive_catalog import (
    GenericPrimitiveContext,
    NamedOpaqueHandleArgument,
    OpaqueHandleProxy,
    PrimitiveOutcomeStatus,
    requirement,
)


CHECK_ID = "editor.enhanced_input.check_domain.v1"
CHECKED_CLASSES = frozenset({
    "/Script/EnhancedInput.InputAction",
    "/Script/EnhancedInput.InputMappingContext",
})


def enhanced_input_warnings(
    exact_asset_class_path: str,
    target: str,
    scratch_handle: OpaqueHandleProxy,
    context: GenericPrimitiveContext,
) -> tuple[RuntimeDiagnostic, ...]:
    if exact_asset_class_path not in CHECKED_CLASSES:
        return ()
    outcome = context.invoke(
        requirement=requirement(CHECK_ID),
        value_arguments={"target": target},
        handle_arguments=(NamedOpaqueHandleArgument("scratch", scratch_handle),),
    )
    if outcome.status is not PrimitiveOutcomeStatus.SUCCEEDED:
        if outcome.diagnostics:
            raise AssetDocumentContractError(outcome.diagnostics[0])
        return ()
    return tuple(item for item in outcome.diagnostics if item.severity is DiagnosticSeverity.WARNING)
