// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

namespace UE::AssetDocumentServer
{
/** The plugin's modules the editor never loads on its own (LoadingPhase "None"): the automation-test modules. */
TArray<FName> GetTestOnlyModuleNames();

#if WITH_DEV_AUTOMATION_TESTS
/** Remembers which test-only modules were loaded once the editor finished starting. */
void RecordTestOnlyModulesAtStartup();

/**
 * Lets the first automation test listing of the session load the test-only modules. The automation worker loads the
 * configured test modules (AutomationTestSettings EditorTestModules) before it lists the tests, but only when the
 * requested test filter is not smoke-only, and the filter it checks is the one of the previous listing: smoke-only
 * for the first listing of a session. Starting from a non-smoke filter makes the first listing load them as well.
 * Every listing sets its own filter, so test selection is unchanged.
 */
void AllowFirstAutomationListingToLoadTestOnlyModules();

/** False when the editor has not finished starting yet. OutLoaded: the test-only modules loaded at that point. */
bool GetTestOnlyModulesLoadedAtStartup(TArray<FName>& OutLoaded);
#endif
}
