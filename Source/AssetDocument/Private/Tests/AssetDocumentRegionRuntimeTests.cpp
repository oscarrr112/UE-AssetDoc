// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentJsonRegionUtils.h"
#include "AssetDocumentRegionRuntime.h"
#include "AssetDocumentRegionRuntimeTestFixture.h"
#include "Regions/AssetDocumentDeferredRegionAdapter.h"
#include "Regions/AssetDocumentGraphRegionWrapperAdapter.h"
#include "Regions/AssetDocumentIdentityArrayDiffHelper.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

namespace
{

TSharedPtr<FJsonValue> MakeIdentityValue(const FString& Name, const int32 Count)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("Name"), Name);
	Object->SetNumberField(TEXT("Count"), Count);
	return MakeShared<FJsonValueObject>(Object);
}

TSharedRef<FJsonObject> MakeTestFragment(const TCHAR* Kind)
{
	TSharedRef<FJsonObject> Fragment = MakeShared<FJsonObject>();
	Fragment->SetStringField(TEXT("Kind"), Kind);
	return Fragment;
}

TSharedRef<FJsonObject> MakeTreeNode(const FString& Id)
{
	TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("Id"), Id);
	Node->SetStringField(TEXT("Class"), TEXT("/Script/Test.TreeNode"));
	return Node;
}

TSharedPtr<FJsonValue> MakeTreeValue(const TSharedRef<FJsonObject>& Root)
{
	TSharedRef<FJsonObject> Tree = MakeShared<FJsonObject>();
	Tree->SetObjectField(TEXT("Root"), Root);
	return FAssetDocumentRegionRuntimeTestFixture::MakeObjectValue(Tree);
}

int32 GetIdentityCount(const TSharedPtr<FJsonValue>& Value)
{
	const TSharedPtr<FJsonObject> Object = Value.IsValid() ? Value->AsObject() : nullptr;
	return Object.IsValid() ? FMath::RoundToInt(Object->GetNumberField(TEXT("Count"))) : INDEX_NONE;
}

struct FTestRegionAdapter : IAssetDocumentRegionAdapter
{
	explicit FTestRegionAdapter(FName InName)
		: Name(InName)
	{
	}

	FName Name;
	bool bSupportsRegion = true;
	bool bApplyChanged = true;
	bool bValidateSucceeds = true;
	bool bApplySucceeds = true;
	TSharedPtr<FJsonValue> CurrentValue = MakeShared<FJsonValueString>(TEXT("Current"));
	TArray<FName>* ApplyLog = nullptr;
	mutable int32 ValidateCalls = 0;
	mutable int32 PreflightCalls = 0;
	mutable int32 ApplyCalls = 0;
	mutable int32 ExtractCalls = 0;
	mutable int32 DiffCalls = 0;
	mutable FString LastJsonPointer;

	FName GetName() const override
	{
		return Name;
	}

	bool SupportsRegion(const FAssetDocumentRegionContext& Context) const override
	{
		LastJsonPointer = Context.JsonPointer;
		return bSupportsRegion;
	}

	TSharedRef<FJsonObject> GetSchemaHint(const FAssetDocumentRegionContext&) const override
	{
		return MakeShared<FJsonObject>();
	}

	FAssetDocumentCapabilityResult ValidateRegion(
		const FAssetDocumentRegionContext& Context,
		const TSharedPtr<FJsonValue>&) const override
	{
		++ValidateCalls;
		LastJsonPointer = Context.JsonPointer;
		if (!bValidateSucceeds)
		{
			return FAssetDocumentCapabilityResult::Failure(TEXT("validate failed"), Context.JsonPointer, TEXT("AdapterValidateFailed"));
		}
		return FAssetDocumentCapabilityResult::Success(TEXT("validated"));
	}

	FAssetDocumentCapabilityResult PreflightRegion(
		FAssetDocumentRegionContext& Context,
		const TSharedPtr<FJsonValue>& DesiredValue) const override
	{
		++PreflightCalls;
		return IAssetDocumentRegionAdapter::PreflightRegion(Context, DesiredValue);
	}

	FAssetDocumentCapabilityResult ApplyRegion(
		FAssetDocumentRegionContext& Context,
		const TSharedPtr<FJsonValue>&,
		bool& bOutChanged) override
	{
		++ApplyCalls;
		LastJsonPointer = Context.JsonPointer;
		if (ApplyLog)
		{
			ApplyLog->Add(Context.RegionId);
		}
		if (!bApplySucceeds)
		{
			return FAssetDocumentCapabilityResult::Failure(TEXT("apply failed"), Context.JsonPointer, TEXT("AdapterApplyFailed"));
		}
		bOutChanged = bApplyChanged;
		return FAssetDocumentCapabilityResult::Success(TEXT("applied"));
	}

	FAssetDocumentCapabilityResult ExtractRegion(
		const FAssetDocumentRegionContext& Context,
		TSharedPtr<FJsonValue>& OutCurrentValue) const override
	{
		++ExtractCalls;
		LastJsonPointer = Context.JsonPointer;
		OutCurrentValue = CurrentValue;
		return FAssetDocumentCapabilityResult::Success(TEXT("extracted"));
	}

	FAssetDocumentCapabilityResult DiffRegion(
		const FAssetDocumentRegionContext& Context,
		const TSharedPtr<FJsonValue>&,
		TArray<TSharedPtr<FJsonValue>>&) const override
	{
		++DiffCalls;
		LastJsonPointer = Context.JsonPointer;
		return FAssetDocumentCapabilityResult::Success(TEXT("diffed"));
	}
};

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentIdentityArrayDiffHelperBasicTest,
	"UEAssetDoc.RegionRuntime.IdentityArrayDiff.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentIdentityArrayDiffHelperBasicTest::RunTest(const FString&)
{
	TArray<FAssetDocumentIdentityArrayDiffElement> Current;
	Current.Add({TEXT("Alpha"), TEXT("Alpha"), MakeIdentityValue(TEXT("Alpha"), 1)});
	Current.Add({TEXT("Beta"), TEXT("Beta"), MakeIdentityValue(TEXT("Beta"), 2)});
	Current.Add({TEXT("CurrentOnly"), TEXT("CurrentOnly"), MakeIdentityValue(TEXT("CurrentOnly"), 3)});

	TArray<FAssetDocumentIdentityArrayDiffElement> Desired;
	Desired.Add({TEXT("Alpha"), TEXT("Alpha"), MakeIdentityValue(TEXT("Alpha"), 1)});
	Desired.Add({TEXT("Beta"), TEXT("Beta"), MakeIdentityValue(TEXT("Beta"), 20)});
	Desired.Add({TEXT("DesiredOnly"), TEXT("Desired/Only~Escaped"), MakeIdentityValue(TEXT("DesiredOnly"), 4)});

	FAssetDocumentIdentityArrayDiffOptions Options;
	Options.RegionPath = TEXT("/Body/TestArray");

	TArray<TSharedPtr<FJsonValue>> Entries;
	const FAssetDocumentCapabilityResult Result =
		FAssetDocumentIdentityArrayDiffHelper::Diff(Options, Current, Desired, {}, Entries);

	TestTrue(TEXT("identity array diff succeeds"), Result.bSuccess);
	TestEqual(TEXT("diff emits current-first plus desired-only entries"), Entries.Num(), 4);
	const TArray<FString> ExpectedOrder = {
		TEXT("/Body/TestArray/Alpha"),
		TEXT("/Body/TestArray/Beta"),
		TEXT("/Body/TestArray/CurrentOnly"),
		TEXT("/Body/TestArray/Desired~1Only~0Escaped"),
	};
	for (int32 Index = 0; Index < ExpectedOrder.Num(); ++Index)
	{
		TestEqual(
			FString::Printf(TEXT("diff entry %d keeps exact traversal order"), Index),
			FAssetDocumentRegionRuntimeTestFixture::GetDiffEntryPath(Entries, Index),
			ExpectedOrder[Index]);
	}

	const TSharedPtr<FJsonObject> Alpha = FAssetDocumentRegionRuntimeTestFixture::FindDiffEntryByPath(Entries, TEXT("/Body/TestArray/Alpha"));
	TestTrue(TEXT("Alpha entry exists"), Alpha.IsValid());
	if (Alpha.IsValid())
	{
		TestEqual(TEXT("Alpha unchanged"), Alpha->GetStringField(TEXT("status")), FString(TEXT("unchanged")));
		TestFalse(TEXT("Alpha has no change"), Alpha->HasField(TEXT("change")));
	}

	const TSharedPtr<FJsonObject> Beta = FAssetDocumentRegionRuntimeTestFixture::FindDiffEntryByPath(Entries, TEXT("/Body/TestArray/Beta"));
	TestTrue(TEXT("Beta entry exists"), Beta.IsValid());
	if (Beta.IsValid())
	{
		TestEqual(TEXT("Beta changed"), Beta->GetStringField(TEXT("status")), FString(TEXT("changed")));
		TestEqual(TEXT("Beta changed change"), Beta->GetStringField(TEXT("change")), FString(TEXT("changed")));
		TestEqual(TEXT("Beta current value shape"), GetIdentityCount(Beta->TryGetField(TEXT("current"))), 2);
		TestEqual(TEXT("Beta desired value shape"), GetIdentityCount(Beta->TryGetField(TEXT("desired"))), 20);
	}

	const TSharedPtr<FJsonObject> CurrentOnly = FAssetDocumentRegionRuntimeTestFixture::FindDiffEntryByPath(Entries, TEXT("/Body/TestArray/CurrentOnly"));
	TestTrue(TEXT("current-only entry exists"), CurrentOnly.IsValid());
	if (CurrentOnly.IsValid())
	{
		TestEqual(TEXT("current-only status"), CurrentOnly->GetStringField(TEXT("status")), FString(TEXT("changed")));
		TestEqual(TEXT("current-only change"), CurrentOnly->GetStringField(TEXT("change")), FString(TEXT("extra")));
		const TSharedPtr<FJsonValue> DesiredValue = CurrentOnly->TryGetField(TEXT("desired"));
		TestTrue(TEXT("current-only desired is null"), DesiredValue.IsValid() && DesiredValue->IsNull());
	}

	const TSharedPtr<FJsonObject> DesiredOnly = FAssetDocumentRegionRuntimeTestFixture::FindDiffEntryByPath(Entries, TEXT("/Body/TestArray/Desired~1Only~0Escaped"));
	TestTrue(TEXT("desired-only entry exists"), DesiredOnly.IsValid());
	if (DesiredOnly.IsValid())
	{
		TestEqual(TEXT("desired-only status"), DesiredOnly->GetStringField(TEXT("status")), FString(TEXT("changed")));
		TestEqual(TEXT("desired-only change"), DesiredOnly->GetStringField(TEXT("change")), FString(TEXT("missing")));
		const TSharedPtr<FJsonValue> CurrentValue = DesiredOnly->TryGetField(TEXT("current"));
		TestTrue(TEXT("desired-only current is null"), CurrentValue.IsValid() && CurrentValue->IsNull());
	}

	FAssetDocumentIdentityArrayDiffOptions SuppressUnchangedOptions = Options;
	SuppressUnchangedOptions.bEmitUnchanged = false;
	TArray<TSharedPtr<FJsonValue>> SuppressedEntries;
	const FAssetDocumentCapabilityResult SuppressedResult =
		FAssetDocumentIdentityArrayDiffHelper::Diff(SuppressUnchangedOptions, Current, Desired, {}, SuppressedEntries);
	TestTrue(TEXT("suppressed unchanged diff succeeds"), SuppressedResult.bSuccess);
	TestEqual(TEXT("suppressed unchanged emits only changed/current-only/desired-only entries"), SuppressedEntries.Num(), 3);
	TestEqual(
		TEXT("suppressed unchanged does not re-emit matched desired as desired-only"),
		FAssetDocumentRegionRuntimeTestFixture::FindDiffEntryByPath(SuppressedEntries, TEXT("/Body/TestArray/Alpha")).IsValid(),
		false);
	TestEqual(TEXT("suppressed entry 0 order"), FAssetDocumentRegionRuntimeTestFixture::GetDiffEntryPath(SuppressedEntries, 0), FString(TEXT("/Body/TestArray/Beta")));
	TestEqual(TEXT("suppressed entry 1 order"), FAssetDocumentRegionRuntimeTestFixture::GetDiffEntryPath(SuppressedEntries, 1), FString(TEXT("/Body/TestArray/CurrentOnly")));
	TestEqual(TEXT("suppressed entry 2 order"), FAssetDocumentRegionRuntimeTestFixture::GetDiffEntryPath(SuppressedEntries, 2), FString(TEXT("/Body/TestArray/Desired~1Only~0Escaped")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentIdentityArrayDiffHelperHooksAndDuplicateTest,
	"UEAssetDoc.RegionRuntime.IdentityArrayDiff.HooksAndDuplicate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentIdentityArrayDiffHelperHooksAndDuplicateTest::RunTest(const FString&)
{
	FAssetDocumentIdentityArrayDiffOptions Options;
	Options.RegionPath = TEXT("/Body/TestArray");

	TArray<FAssetDocumentIdentityArrayDiffElement> Current;
	Current.Add({TEXT("Equal"), TEXT("Equal"), MakeIdentityValue(TEXT("Equal"), 1)});
	Current.Add({TEXT("Changed"), TEXT("Changed"), MakeIdentityValue(TEXT("Changed"), 2)});

	TArray<FAssetDocumentIdentityArrayDiffElement> Desired;
	Desired.Add({TEXT("Equal"), TEXT("Equal"), MakeIdentityValue(TEXT("Equal"), 99)});
	Desired.Add({TEXT("Changed"), TEXT("Changed"), MakeIdentityValue(TEXT("Changed"), 20)});

	bool bPathHookSawEqualCurrent = false;
	bool bPathHookSawEqualDesired = false;
	int32 PathHookEqualCurrentCount = INDEX_NONE;
	int32 PathHookEqualDesiredCount = INDEX_NONE;
	bool bPathHookSawChangedCurrent = false;
	bool bPathHookSawChangedDesired = false;
	int32 PathHookChangedCurrentCount = INDEX_NONE;
	int32 PathHookChangedDesiredCount = INDEX_NONE;

	FAssetDocumentIdentityArrayDiffHooks Hooks;
	Hooks.AreElementsEqual = [](const FAssetDocumentIdentityArrayDiffEntryContext& Entry)
	{
		return Entry.Identity == TEXT("Equal");
	};
	Hooks.MakePath = [
		&bPathHookSawEqualCurrent,
		&bPathHookSawEqualDesired,
		&PathHookEqualCurrentCount,
		&PathHookEqualDesiredCount,
		&bPathHookSawChangedCurrent,
		&bPathHookSawChangedDesired,
		&PathHookChangedCurrentCount,
		&PathHookChangedDesiredCount](const FAssetDocumentIdentityArrayDiffEntryContext& Entry)
	{
		if (Entry.Identity == TEXT("Equal"))
		{
			bPathHookSawEqualCurrent = Entry.bHasCurrent;
			bPathHookSawEqualDesired = Entry.bHasDesired;
			PathHookEqualCurrentCount = GetIdentityCount(Entry.CurrentValue);
			PathHookEqualDesiredCount = GetIdentityCount(Entry.DesiredValue);
		}
		else if (Entry.Identity == TEXT("Changed"))
		{
			bPathHookSawChangedCurrent = Entry.bHasCurrent;
			bPathHookSawChangedDesired = Entry.bHasDesired;
			PathHookChangedCurrentCount = GetIdentityCount(Entry.CurrentValue);
			PathHookChangedDesiredCount = GetIdentityCount(Entry.DesiredValue);
		}
		return FString::Printf(TEXT("/Custom/%s"), *Entry.Identity);
	};
	Hooks.MakeChange = [](const FAssetDocumentIdentityArrayDiffEntryContext&)
	{
		return FString(TEXT("semantic"));
	};

	TArray<TSharedPtr<FJsonValue>> Entries;
	const FAssetDocumentCapabilityResult Result =
		FAssetDocumentIdentityArrayDiffHelper::Diff(Options, Current, Desired, Hooks, Entries);
	TestTrue(TEXT("custom hook diff succeeds"), Result.bSuccess);
	TestEqual(TEXT("custom hook emits two entries"), Entries.Num(), 2);
	TestTrue(TEXT("path hook sees matched current flag for Equal"), bPathHookSawEqualCurrent);
	TestTrue(TEXT("path hook sees matched desired flag for Equal"), bPathHookSawEqualDesired);
	TestEqual(TEXT("path hook sees Equal current value"), PathHookEqualCurrentCount, 1);
	TestEqual(TEXT("path hook sees Equal desired value"), PathHookEqualDesiredCount, 99);
	TestTrue(TEXT("path hook sees matched current flag for Changed"), bPathHookSawChangedCurrent);
	TestTrue(TEXT("path hook sees matched desired flag for Changed"), bPathHookSawChangedDesired);
	TestEqual(TEXT("path hook sees Changed current value"), PathHookChangedCurrentCount, 2);
	TestEqual(TEXT("path hook sees Changed desired value"), PathHookChangedDesiredCount, 20);

	const TSharedPtr<FJsonObject> EqualEntry = FAssetDocumentRegionRuntimeTestFixture::FindDiffEntryByPath(Entries, TEXT("/Custom/Equal"));
	TestTrue(TEXT("custom equality entry exists"), EqualEntry.IsValid());
	if (EqualEntry.IsValid())
	{
		TestEqual(TEXT("custom path used"), EqualEntry->GetStringField(TEXT("path")), FString(TEXT("/Custom/Equal")));
		TestEqual(TEXT("custom equality marks unchanged"), EqualEntry->GetStringField(TEXT("status")), FString(TEXT("unchanged")));
		TestFalse(TEXT("custom equality unchanged has no change"), EqualEntry->HasField(TEXT("change")));
	}

	const TSharedPtr<FJsonObject> ChangedEntry = FAssetDocumentRegionRuntimeTestFixture::FindDiffEntryByPath(Entries, TEXT("/Custom/Changed"));
	TestTrue(TEXT("custom change entry exists"), ChangedEntry.IsValid());
	if (ChangedEntry.IsValid())
	{
		TestEqual(TEXT("custom change marks changed"), ChangedEntry->GetStringField(TEXT("status")), FString(TEXT("changed")));
		TestEqual(TEXT("custom change is used"), ChangedEntry->GetStringField(TEXT("change")), FString(TEXT("semantic")));
	}

	Current.Add({TEXT("Equal"), TEXT("Duplicate"), MakeIdentityValue(TEXT("Duplicate"), 2)});
	TArray<TSharedPtr<FJsonValue>> DuplicateEntries;
	const FAssetDocumentCapabilityResult DuplicateResult =
		FAssetDocumentIdentityArrayDiffHelper::Diff(Options, Current, Desired, Hooks, DuplicateEntries);
	TestFalse(TEXT("duplicate identity fails"), DuplicateResult.bSuccess);
	TestTrue(
		TEXT("duplicate diagnostic path is region path"),
		DuplicateResult.Diagnostics.Num() > 0 && DuplicateResult.Diagnostics[0].Path == TEXT("/Body/TestArray"));
	TestTrue(
		TEXT("duplicate diagnostic code is stable"),
		DuplicateResult.Diagnostics.Num() > 0 && DuplicateResult.Diagnostics[0].Code == TEXT("DuplicateIdentityArrayDiffIdentity"));

	TArray<FAssetDocumentIdentityArrayDiffElement> DesiredWithDuplicate = Desired;
	DesiredWithDuplicate.Add({TEXT("Equal"), TEXT("DuplicateDesired"), MakeIdentityValue(TEXT("DuplicateDesired"), 3)});
	TArray<TSharedPtr<FJsonValue>> DesiredDuplicateEntries;
	const FAssetDocumentCapabilityResult DesiredDuplicateResult =
		FAssetDocumentIdentityArrayDiffHelper::Diff(Options, Desired, DesiredWithDuplicate, Hooks, DesiredDuplicateEntries);
	TestFalse(TEXT("desired duplicate identity fails"), DesiredDuplicateResult.bSuccess);
	TestTrue(
		TEXT("desired duplicate diagnostic path is region path"),
		DesiredDuplicateResult.Diagnostics.Num() > 0 && DesiredDuplicateResult.Diagnostics[0].Path == TEXT("/Body/TestArray"));
	TestTrue(
		TEXT("desired duplicate diagnostic code is stable"),
		DesiredDuplicateResult.Diagnostics.Num() > 0 && DesiredDuplicateResult.Diagnostics[0].Code == TEXT("DuplicateIdentityArrayDiffIdentity"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentRegionRuntimeGraphWrapperSupportsSyntheticBodyTest,
	"UEAssetDoc.RegionRuntime.GraphWrapper.SupportsSyntheticBody",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRegionRuntimeGraphWrapperSupportsSyntheticBodyTest::RunTest(const FString& Parameters)
{
	FAssetDocumentGraphRegionWrapperConfig Config;
	Config.AdapterName = TEXT("TestGraphWrapper");
	Config.RegionId = TEXT("Body.TestGraphRegions");
	Config.BodyPath = TEXT("Body.TestGraphRegions");
	Config.JsonPointer = TEXT("/Body");
	Config.SchemaLabel = TEXT("TestGraphRegions");
	FAssetDocumentGraphRegionWrapperHooks Hooks;
	FAssetDocumentGraphRegionWrapperAdapter Adapter(MoveTemp(Config), MoveTemp(Hooks));

	FAssetDocumentRegionContext Context;
	Context.RegionId = TEXT("Body.TestGraphRegions");
	Context.BodyPath = TEXT("Body.TestGraphRegions");
	Context.JsonPointer = TEXT("/Body");
	TestTrue(TEXT("Region id is supported"), Adapter.SupportsRegion(Context));

	Context.RegionId = TEXT("Different.Region");
	TestTrue(TEXT("Body path is supported"), Adapter.SupportsRegion(Context));

	Context.JsonPointer = TEXT("/Body/UbergraphPages");
	TestFalse(TEXT("Non-body pointer is rejected"), Adapter.SupportsRegion(Context));

	Context.JsonPointer = TEXT("/Body");
	const TSharedRef<FJsonObject> SchemaHint = Adapter.GetSchemaHint(Context);
	TestEqual(TEXT("Schema hint reports adapter"), SchemaHint->GetStringField(TEXT("Adapter")), FString(TEXT("TestGraphWrapper")));
	TestEqual(TEXT("Schema hint reports label"), SchemaHint->GetStringField(TEXT("Label")), FString(TEXT("TestGraphRegions")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentRegionRuntimeGraphWrapperDispatchesHooksTest,
	"UEAssetDoc.RegionRuntime.GraphWrapper.DispatchesHooks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRegionRuntimeGraphWrapperDispatchesHooksTest::RunTest(const FString& Parameters)
{
	int32 ValidateCalls = 0;
	int32 PreflightCalls = 0;
	int32 ApplyCalls = 0;
	int32 ExtractCalls = 0;
	int32 DiffCalls = 0;

	FAssetDocumentGraphRegionWrapperConfig Config;
	Config.AdapterName = TEXT("TestGraphWrapper");
	Config.RegionId = TEXT("Body.TestGraphRegions");
	Config.BodyPath = TEXT("Body.TestGraphRegions");
	Config.JsonPointer = TEXT("/Body");

	FAssetDocumentGraphRegionWrapperHooks Hooks;
	Hooks.Validate = [&ValidateCalls](
		const FAssetDocumentCapabilityContext& Context,
		const TSharedRef<FJsonObject>& BodyObject)
	{
		++ValidateCalls;
		return BodyObject->HasField(TEXT("Graphs")) && Context.bIsDryRun
			? FAssetDocumentCapabilityResult::Success(TEXT("validated"))
			: FAssetDocumentCapabilityResult::Failure(TEXT("missing graphs"), TEXT("/Body/Graphs"), TEXT("MissingGraphs"));
	};
	Hooks.Preflight = [&PreflightCalls](
		FAssetDocumentCapabilityContext& Context,
		const TSharedRef<FJsonObject>& BodyObject)
	{
		++PreflightCalls;
		return BodyObject->HasField(TEXT("Graphs")) && Context.bIsDryRun
			? FAssetDocumentCapabilityResult::Success(TEXT("preflighted"))
			: FAssetDocumentCapabilityResult::Failure(TEXT("missing preflight context"), TEXT("/Body"), TEXT("MissingPreflightContext"));
	};
	Hooks.Apply = [&ApplyCalls](
		FAssetDocumentCapabilityContext& Context,
		const TSharedRef<FJsonObject>& BodyObject,
		bool& bOutChanged)
	{
		++ApplyCalls;
		bOutChanged = BodyObject->HasField(TEXT("Graphs")) && Context.bIsDryRun;
		return bOutChanged
			? FAssetDocumentCapabilityResult::Success(TEXT("applied"))
			: FAssetDocumentCapabilityResult::Failure(TEXT("missing apply context"), TEXT("/Body"), TEXT("MissingApplyContext"));
	};
	Hooks.Extract = [&ExtractCalls](
		const FAssetDocumentCapabilityContext& Context,
		TSharedRef<FJsonObject>& OutBodyObject)
	{
		++ExtractCalls;
		if (!Context.bIsDryRun)
		{
			return FAssetDocumentCapabilityResult::Failure(TEXT("missing extract context"), TEXT("/Body"), TEXT("MissingExtractContext"));
		}
		OutBodyObject->SetStringField(TEXT("Graphs"), TEXT("current"));
		return FAssetDocumentCapabilityResult::Success(TEXT("extracted"));
	};
	Hooks.Diff = [&DiffCalls](
		const FAssetDocumentCapabilityContext& Context,
		const TSharedRef<FJsonObject>& BodyObject,
		TArray<TSharedPtr<FJsonValue>>& OutDiffEntries)
	{
		++DiffCalls;
		if (!BodyObject->HasField(TEXT("Graphs")) || !Context.bIsDryRun)
		{
			return FAssetDocumentCapabilityResult::Failure(TEXT("missing diff context"), TEXT("/Body"), TEXT("MissingDiffContext"));
		}
		FAssetDocumentJsonRegionUtils::AddDiffEntry(OutDiffEntries, TEXT("/Body/Graphs"), TEXT("changed"), nullptr, nullptr);
		return FAssetDocumentCapabilityResult::Success(TEXT("diffed"));
	};

	FAssetDocumentGraphRegionWrapperAdapter Adapter(MoveTemp(Config), MoveTemp(Hooks));
	FAssetDocumentRegionContext RegionContext;
	RegionContext.RegionId = TEXT("Body.TestGraphRegions");
	RegionContext.BodyPath = TEXT("Body.TestGraphRegions");
	RegionContext.JsonPointer = TEXT("/Body");
	RegionContext.bIsDryRun = true;

	TSharedRef<FJsonObject> BodyObject = MakeShared<FJsonObject>();
	BodyObject->SetArrayField(TEXT("Graphs"), {});
	const TSharedRef<FJsonValueObject> BodyValue = MakeShared<FJsonValueObject>(BodyObject);

	TestTrue(TEXT("Validate succeeds"), Adapter.ValidateRegion(RegionContext, BodyValue).bSuccess);
	FAssetDocumentRegionContext PreflightContext = RegionContext;
	TestTrue(TEXT("Preflight succeeds"), Adapter.PreflightRegion(PreflightContext, BodyValue).bSuccess);
	bool bChanged = false;
	TestTrue(TEXT("Apply succeeds"), Adapter.ApplyRegion(RegionContext, BodyValue, bChanged).bSuccess);
	TestTrue(TEXT("Apply propagates changed"), bChanged);
	TSharedPtr<FJsonValue> ExtractedValue;
	TestTrue(TEXT("Extract succeeds"), Adapter.ExtractRegion(RegionContext, ExtractedValue).bSuccess);
	TestTrue(TEXT("Extract returns object"), ExtractedValue.IsValid() && ExtractedValue->Type == EJson::Object);
	TArray<TSharedPtr<FJsonValue>> DiffEntries;
	TestTrue(TEXT("Diff succeeds"), Adapter.DiffRegion(RegionContext, BodyValue, DiffEntries).bSuccess);

	TestEqual(TEXT("Validate calls"), ValidateCalls, 1);
	TestEqual(TEXT("Preflight calls"), PreflightCalls, 1);
	TestEqual(TEXT("Apply calls"), ApplyCalls, 1);
	TestEqual(TEXT("Extract calls"), ExtractCalls, 1);
	TestEqual(TEXT("Diff calls"), DiffCalls, 1);
	TestEqual(TEXT("One diff entry"), DiffEntries.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentRegionRuntimeGraphWrapperPropagatesHookFailuresTest,
	"UEAssetDoc.RegionRuntime.GraphWrapper.PropagatesHookFailures",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRegionRuntimeGraphWrapperPropagatesHookFailuresTest::RunTest(const FString& Parameters)
{
	FAssetDocumentGraphRegionWrapperConfig Config;
	Config.AdapterName = TEXT("TestGraphWrapper");
	Config.RegionId = TEXT("Body.TestGraphRegions");
	Config.BodyPath = TEXT("Body.TestGraphRegions");
	Config.JsonPointer = TEXT("/Body");

	FAssetDocumentGraphRegionWrapperHooks Hooks;
	Hooks.Validate = [](
		const FAssetDocumentCapabilityContext&,
		const TSharedRef<FJsonObject>&)
	{
		return FAssetDocumentCapabilityResult::Failure(
			TEXT("validate failed"),
			TEXT("/Body/Graphs/0"),
			TEXT("GraphValidateFailed"));
	};

	FAssetDocumentGraphRegionWrapperAdapter Adapter(MoveTemp(Config), MoveTemp(Hooks));
	FAssetDocumentRegionContext RegionContext;
	RegionContext.RegionId = TEXT("Body.TestGraphRegions");
	RegionContext.BodyPath = TEXT("Body.TestGraphRegions");
	RegionContext.JsonPointer = TEXT("/Body");

	TSharedRef<FJsonObject> BodyObject = MakeShared<FJsonObject>();
	BodyObject->SetArrayField(TEXT("Graphs"), {});
	const FAssetDocumentCapabilityResult Result =
		Adapter.ValidateRegion(RegionContext, MakeShared<FJsonValueObject>(BodyObject));
	TestFalse(TEXT("Validate hook failure propagates"), Result.bSuccess);
	TestEqual(TEXT("Failure reports one diagnostic"), Result.Diagnostics.Num(), 1);
	if (Result.Diagnostics.Num() < 1)
	{
		return false;
	}
	TestEqual(TEXT("Failure path is preserved"), Result.Diagnostics[0].Path, FString(TEXT("/Body/Graphs/0")));
	TestEqual(TEXT("Failure code is preserved"), Result.Diagnostics[0].Code, FString(TEXT("GraphValidateFailed")));
	TestEqual(TEXT("Failure message is preserved"), Result.Diagnostics[0].Message, FString(TEXT("validate failed")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentRegionRuntimeGraphWrapperRejectsInvalidBodyAndMissingHooksTest,
	"UEAssetDoc.RegionRuntime.GraphWrapper.RejectsInvalidBodyAndMissingHooks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRegionRuntimeGraphWrapperRejectsInvalidBodyAndMissingHooksTest::RunTest(const FString& Parameters)
{
	FAssetDocumentGraphRegionWrapperConfig Config;
	Config.AdapterName = TEXT("TestGraphWrapper");
	Config.RegionId = TEXT("Body.TestGraphRegions");
	Config.BodyPath = TEXT("Body.TestGraphRegions");
	Config.JsonPointer = TEXT("/Body");
	FAssetDocumentGraphRegionWrapperHooks Hooks;
	FAssetDocumentGraphRegionWrapperAdapter Adapter(MoveTemp(Config), MoveTemp(Hooks));

	FAssetDocumentRegionContext RegionContext;
	RegionContext.RegionId = TEXT("Body.TestGraphRegions");
	RegionContext.BodyPath = TEXT("Body.TestGraphRegions");
	RegionContext.JsonPointer = TEXT("/Body");

	const FAssetDocumentCapabilityResult InvalidBodyResult =
		Adapter.ValidateRegion(RegionContext, MakeShared<FJsonValueString>(TEXT("not object")));
	TestFalse(TEXT("Invalid body fails"), InvalidBodyResult.bSuccess);
	TestEqual(
		TEXT("Invalid body code"),
		InvalidBodyResult.Diagnostics.Num() > 0 ? InvalidBodyResult.Diagnostics[0].Code : FString(),
		FString(TEXT("InvalidBodySectionType")));
	TestEqual(
		TEXT("Invalid body path"),
		InvalidBodyResult.Diagnostics.Num() > 0 ? InvalidBodyResult.Diagnostics[0].Path : FString(),
		FString(TEXT("/Body")));

	TSharedRef<FJsonObject> BodyObject = MakeShared<FJsonObject>();
	const FAssetDocumentCapabilityResult MissingHookResult =
		Adapter.ValidateRegion(RegionContext, MakeShared<FJsonValueObject>(BodyObject));
	TestFalse(TEXT("Missing hook fails"), MissingHookResult.bSuccess);
	TestEqual(
		TEXT("Missing hook code"),
		MissingHookResult.Diagnostics.Num() > 0 ? MissingHookResult.Diagnostics[0].Code : FString(),
		FString(TEXT("UnsupportedGraphRegionLifecycle")));

	FAssetDocumentRegionContext PreflightContext = RegionContext;
	const FAssetDocumentCapabilityResult MissingPreflightResult =
		Adapter.PreflightRegion(PreflightContext, MakeShared<FJsonValueObject>(BodyObject));
	TestFalse(TEXT("Missing preflight hook fails"), MissingPreflightResult.bSuccess);
	TestEqual(
		TEXT("Missing preflight hook code"),
		MissingPreflightResult.Diagnostics.Num() > 0 ? MissingPreflightResult.Diagnostics[0].Code : FString(),
		FString(TEXT("UnsupportedGraphRegionLifecycle")));

	bool bMissingApplyChanged = false;
	const FAssetDocumentCapabilityResult MissingApplyResult =
		Adapter.ApplyRegion(RegionContext, MakeShared<FJsonValueObject>(BodyObject), bMissingApplyChanged);
	TestFalse(TEXT("Missing apply hook fails"), MissingApplyResult.bSuccess);
	TestEqual(
		TEXT("Missing apply hook code"),
		MissingApplyResult.Diagnostics.Num() > 0 ? MissingApplyResult.Diagnostics[0].Code : FString(),
		FString(TEXT("UnsupportedGraphRegionLifecycle")));

	TSharedPtr<FJsonValue> MissingExtractValue;
	const FAssetDocumentCapabilityResult MissingExtractResult = Adapter.ExtractRegion(RegionContext, MissingExtractValue);
	TestFalse(TEXT("Missing extract hook fails"), MissingExtractResult.bSuccess);
	TestEqual(
		TEXT("Missing extract hook code"),
		MissingExtractResult.Diagnostics.Num() > 0 ? MissingExtractResult.Diagnostics[0].Code : FString(),
		FString(TEXT("UnsupportedGraphRegionLifecycle")));

	TArray<TSharedPtr<FJsonValue>> MissingDiffEntries;
	const FAssetDocumentCapabilityResult MissingDiffResult =
		Adapter.DiffRegion(RegionContext, MakeShared<FJsonValueObject>(BodyObject), MissingDiffEntries);
	TestFalse(TEXT("Missing diff hook fails"), MissingDiffResult.bSuccess);
	TestEqual(
		TEXT("Missing diff hook code"),
		MissingDiffResult.Diagnostics.Num() > 0 ? MissingDiffResult.Diagnostics[0].Code : FString(),
		FString(TEXT("UnsupportedGraphRegionLifecycle")));

	bool bChanged = true;
	const FAssetDocumentCapabilityResult InvalidApplyBodyResult =
		Adapter.ApplyRegion(RegionContext, MakeShared<FJsonValueString>(TEXT("not object")), bChanged);
	TestFalse(TEXT("Invalid apply body fails"), InvalidApplyBodyResult.bSuccess);
	TestFalse(TEXT("Invalid apply resets changed"), bChanged);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentRegionRuntimeJsonUtilsRequireObjectTest,
	"UEAssetDoc.RegionRuntime.JsonUtils.RequireObject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRegionRuntimeJsonUtilsRequireObjectTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> OutObject;
	TSharedRef<FJsonObject> SourceObject = MakeShared<FJsonObject>();
	SourceObject->SetStringField(TEXT("Name"), TEXT("Preview"));

	FAssetDocumentCapabilityResult Result =
		FAssetDocumentJsonRegionUtils::RequireObjectValue(FAssetDocumentRegionRuntimeTestFixture::MakeObjectValue(SourceObject), TEXT("/Body/Preview"), OutObject);
	TestTrue(TEXT("Object value succeeds"), Result.bSuccess);
	TestTrue(TEXT("Object value is returned"), OutObject == SourceObject);

	Result = FAssetDocumentJsonRegionUtils::RequireObjectValue(
		MakeShared<FJsonValueString>(TEXT("not-object")),
		TEXT("/Body/Preview"),
		OutObject);
	TestFalse(TEXT("String value fails object requirement"), Result.bSuccess);
	TestEqual(TEXT("Failure reports one diagnostic"), Result.Diagnostics.Num(), 1);
	if (Result.Diagnostics.Num() < 1)
	{
		return false;
	}
	TestEqual(TEXT("Failure path uses supplied JSON pointer"), Result.Diagnostics[0].Path, FString(TEXT("/Body/Preview")));
	TestEqual(TEXT("Failure code marks invalid section type"), Result.Diagnostics[0].Code, FString(TEXT("InvalidBodySectionType")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentRegionRuntimeJsonUtilsRequireArrayTest,
	"UEAssetDoc.RegionRuntime.JsonUtils.RequireArray",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRegionRuntimeJsonUtilsRequireArrayTest::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FJsonValue>> OutArray;
	FAssetDocumentCapabilityResult Result = FAssetDocumentJsonRegionUtils::RequireArrayValue(
		FAssetDocumentRegionRuntimeTestFixture::MakeArrayValue({MakeShared<FJsonValueString>(TEXT("A"))}),
		TEXT("/Body/Curves"),
		OutArray);
	TestTrue(TEXT("Array value succeeds"), Result.bSuccess);
	TestEqual(TEXT("Array values are copied to caller-owned storage"), OutArray.Num(), 1);
	if (OutArray.Num() < 1)
	{
		return false;
	}
	TestEqual(TEXT("Array value remains safe after temporary input"), OutArray[0]->AsString(), FString(TEXT("A")));

	Result = FAssetDocumentJsonRegionUtils::RequireArrayValue(
		FAssetDocumentRegionRuntimeTestFixture::MakeObjectValue(MakeShared<FJsonObject>()),
		TEXT("/Body/Curves"),
		OutArray);
	TestFalse(TEXT("Object value fails array requirement"), Result.bSuccess);
	TestEqual(TEXT("Failure reports one diagnostic"), Result.Diagnostics.Num(), 1);
	if (Result.Diagnostics.Num() < 1)
	{
		return false;
	}
	TestEqual(TEXT("Failure path uses supplied JSON pointer"), Result.Diagnostics[0].Path, FString(TEXT("/Body/Curves")));
	TestEqual(TEXT("Failure message describes array requirement"), Result.Message, FString(TEXT("Expected a JSON array")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentRegionRuntimeJsonUtilsJsonPointerEscapingTest,
	"UEAssetDoc.RegionRuntime.JsonUtils.JsonPointerEscaping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRegionRuntimeJsonUtilsJsonPointerEscapingTest::RunTest(const FString& Parameters)
{
	TestEqual(
		TEXT("Escapes JSON pointer slash and tilde tokens"),
		FAssetDocumentJsonRegionUtils::EscapeJsonPointerToken(TEXT("Curve/Name~LOD")),
		FString(TEXT("Curve~1Name~0LOD")));
	TestEqual(
		TEXT("Body path escapes body key token"),
		FAssetDocumentJsonRegionUtils::MakeBodyPath(TEXT("Graph/With~Token")),
		FString(TEXT("/Body/Graph~1With~0Token")));
	TestEqual(
		TEXT("Body array item path appends numeric token"),
		FAssetDocumentJsonRegionUtils::MakeBodyArrayItemPath(TEXT("Curves"), 7),
		FString(TEXT("/Body/Curves/7")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentRegionRuntimeJsonUtilsDiffEntryShapeTest,
	"UEAssetDoc.RegionRuntime.JsonUtils.DiffEntryShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRegionRuntimeJsonUtilsDiffEntryShapeTest::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FJsonValue>> Entries;
	FAssetDocumentJsonRegionUtils::AddDiffEntry(
		Entries,
		TEXT("/Body/Preview"),
		TEXT("changed"),
		MakeShared<FJsonValueString>(TEXT("Before")),
		MakeShared<FJsonValueString>(TEXT("After")));

	TestEqual(TEXT("One diff entry is added"), Entries.Num(), 1);
	if (Entries.Num() < 1)
	{
		return false;
	}
	TestTrue(TEXT("Entry is a JSON object"), Entries[0].IsValid() && Entries[0]->Type == EJson::Object);
	const TSharedPtr<FJsonObject> EntryObject = Entries[0]->AsObject();
	if (!EntryObject.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("Entry path field"), EntryObject->GetStringField(TEXT("path")), FString(TEXT("/Body/Preview")));
	TestEqual(TEXT("Entry status field"), EntryObject->GetStringField(TEXT("status")), FString(TEXT("changed")));
	TestEqual(TEXT("Entry current field"), EntryObject->GetStringField(TEXT("current")), FString(TEXT("Before")));
	TestEqual(TEXT("Entry desired field"), EntryObject->GetStringField(TEXT("desired")), FString(TEXT("After")));

	TSharedRef<FJsonObject> FirstObject = MakeShared<FJsonObject>();
	FirstObject->SetNumberField(TEXT("B"), 2.0);
	FirstObject->SetNumberField(TEXT("A"), 1.0);
	TSharedRef<FJsonObject> SecondObject = MakeShared<FJsonObject>();
	SecondObject->SetNumberField(TEXT("A"), 1.0);
	SecondObject->SetNumberField(TEXT("B"), 2.0);
	TestEqual(
		TEXT("Comparable JSON ignores object field insertion order"),
		FAssetDocumentJsonRegionUtils::JsonValueToComparableString(FAssetDocumentRegionRuntimeTestFixture::MakeObjectValue(FirstObject)),
		FAssetDocumentJsonRegionUtils::JsonValueToComparableString(FAssetDocumentRegionRuntimeTestFixture::MakeObjectValue(SecondObject)));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentRegionRuntimeJsonUtilsFieldFailureTest,
	"UEAssetDoc.RegionRuntime.JsonUtils.FieldFailures",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRegionRuntimeJsonUtilsFieldFailureTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("Name"), TEXT("   "));
	Object->SetStringField(TEXT("Enabled"), TEXT("true"));

	FString OutString;
	FAssetDocumentCapabilityResult Result = FAssetDocumentJsonRegionUtils::RequireStringField(
		Object,
		TEXT("Name"),
		TEXT("/Body/Variables/0/Name"),
		OutString);
	TestFalse(TEXT("Whitespace-only string is rejected"), Result.bSuccess);
	TestEqual(TEXT("Whitespace string failure diagnostic count"), Result.Diagnostics.Num(), 1);
	if (Result.Diagnostics.Num() < 1)
	{
		return false;
	}
	TestEqual(TEXT("Whitespace string failure path"), Result.Diagnostics[0].Path, FString(TEXT("/Body/Variables/0/Name")));

	double OutNumber = 0.0;
	Result = FAssetDocumentJsonRegionUtils::RequireNumberField(
		Object,
		TEXT("Time"),
		TEXT("/Body/Curves/0/Keys/0/Time"),
		OutNumber);
	TestFalse(TEXT("Missing number is rejected"), Result.bSuccess);
	TestEqual(TEXT("Missing number diagnostic count"), Result.Diagnostics.Num(), 1);
	if (Result.Diagnostics.Num() < 1)
	{
		return false;
	}
	TestEqual(TEXT("Missing number diagnostic code"), Result.Diagnostics[0].Code, FString(TEXT("InvalidNumericField")));

	bool bOutBool = false;
	Result = FAssetDocumentJsonRegionUtils::RequireBoolField(
		Object,
		TEXT("Enabled"),
		TEXT("/Body/Options/Enabled"),
		bOutBool);
	TestFalse(TEXT("Wrong bool type is rejected"), Result.bSuccess);
	TestEqual(TEXT("Wrong bool diagnostic count"), Result.Diagnostics.Num(), 1);
	if (Result.Diagnostics.Num() < 1)
	{
		return false;
	}
	TestEqual(TEXT("Wrong bool diagnostic code"), Result.Diagnostics[0].Code, FString(TEXT("InvalidBooleanField")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentRegionRuntimeDispatchValidateCallsAdapterTest,
	"UEAssetDoc.RegionRuntime.Dispatch.ValidateCallsAdapter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRegionRuntimeDispatchValidateCallsAdapterTest::RunTest(const FString& Parameters)
{
	FTestRegionAdapter Adapter(TEXT("Fake"));
	const FAssetDocumentRegionPolicy Policy = FAssetDocumentRegionRuntimeTestFixture::MakePolicy(TEXT("Preview"), TEXT("Body.Preview"));
	const FAssetDocumentRegionContext Context = FAssetDocumentRegionRuntimeTestContextBuilder()
		.WithRegionId(TEXT("Preview"))
		.WithJsonPointer(TEXT("/Body/Preview"))
		.WithPolicy(&Policy)
		.Build();

	const FAssetDocumentCapabilityResult Result = FAssetDocumentRegionRuntime::Validate(
		Context,
		MakeShared<FJsonValueString>(TEXT("Desired")),
		Adapter);

	TestTrue(TEXT("Runtime validate succeeds"), Result.bSuccess);
	TestEqual(TEXT("Adapter validate is called once"), Adapter.ValidateCalls, 1);
	TestEqual(TEXT("Adapter receives JSON pointer"), Adapter.LastJsonPointer, FString(TEXT("/Body/Preview")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentRegionRuntimeDispatchPreflightUsesValidateDefaultTest,
	"UEAssetDoc.RegionRuntime.Dispatch.PreflightUsesValidateDefault",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRegionRuntimeDispatchPreflightUsesValidateDefaultTest::RunTest(const FString& Parameters)
{
	FTestRegionAdapter Adapter(TEXT("Fake"));
	const FAssetDocumentRegionPolicy Policy = FAssetDocumentRegionRuntimeTestFixture::MakePolicy(TEXT("Preview"), TEXT("Body.Preview"));
	FAssetDocumentRegionContext Context = FAssetDocumentRegionRuntimeTestFixture::MakeRuntimeContext(TEXT("Preview"), TEXT("/Body/Preview"), &Policy);

	const FAssetDocumentCapabilityResult Result = FAssetDocumentRegionRuntime::Preflight(
		Context,
		MakeShared<FJsonValueString>(TEXT("Desired")),
		Adapter);

	TestTrue(TEXT("Runtime preflight succeeds"), Result.bSuccess);
	TestEqual(TEXT("Adapter preflight override is called once"), Adapter.PreflightCalls, 1);
	TestEqual(TEXT("Default preflight routes to validate"), Adapter.ValidateCalls, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentRegionRuntimeDispatchApplyReturnsChangedFlagTest,
	"UEAssetDoc.RegionRuntime.Dispatch.ApplyReturnsChangedFlag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRegionRuntimeDispatchApplyReturnsChangedFlagTest::RunTest(const FString& Parameters)
{
	FTestRegionAdapter Adapter(TEXT("Fake"));
	Adapter.bApplyChanged = true;
	const FAssetDocumentRegionPolicy Policy = FAssetDocumentRegionRuntimeTestFixture::MakePolicy(TEXT("Preview"), TEXT("Body.Preview"));
	FAssetDocumentRegionContext Context = FAssetDocumentRegionRuntimeTestFixture::MakeRuntimeContext(TEXT("Preview"), TEXT("/Body/Preview"), &Policy);

	bool bChanged = false;
	const FAssetDocumentCapabilityResult Result = FAssetDocumentRegionRuntime::Apply(
		Context,
		MakeShared<FJsonValueString>(TEXT("Desired")),
		Adapter,
		bChanged);

	TestTrue(TEXT("Runtime apply succeeds"), Result.bSuccess);
	TestTrue(TEXT("Changed flag is returned from adapter"), bChanged);
	TestEqual(TEXT("Adapter apply is called once"), Adapter.ApplyCalls, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentRegionRuntimeDispatchDiffAllowsAdapterHandledEmptyResultTest,
	"UEAssetDoc.RegionRuntime.Dispatch.DiffAllowsAdapterHandledEmptyResult",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRegionRuntimeDispatchDiffAllowsAdapterHandledEmptyResultTest::RunTest(const FString& Parameters)
{
	FTestRegionAdapter Adapter(TEXT("Fake"));
	Adapter.CurrentValue = MakeShared<FJsonValueString>(TEXT("Current"));
	const FAssetDocumentRegionPolicy Policy = FAssetDocumentRegionRuntimeTestFixture::MakePolicy(TEXT("Display/Name"), TEXT("Body.Display/Name"));
	const FAssetDocumentRegionContext Context = FAssetDocumentRegionRuntimeTestFixture::MakeRuntimeContext(TEXT("Display/Name"), TEXT("/Body/Display~1Name"), &Policy);

	TArray<TSharedPtr<FJsonValue>> DiffEntries;
	const FAssetDocumentCapabilityResult Result = FAssetDocumentRegionRuntime::Diff(
		Context,
		MakeShared<FJsonValueString>(TEXT("Desired")),
		Adapter,
		DiffEntries);

	TestTrue(TEXT("Runtime diff succeeds"), Result.bSuccess);
	TestEqual(TEXT("Adapter-handled empty diff stays empty"), DiffEntries.Num(), 0);
	TestEqual(TEXT("Runtime does not infer fallback extraction"), Adapter.ExtractCalls, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentRegionRuntimeDispatchDiffEntryUsesJsonPointerTest,
	"UEAssetDoc.RegionRuntime.Dispatch.DiffEntryUsesJsonPointer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRegionRuntimeDispatchDiffEntryUsesJsonPointerTest::RunTest(const FString& Parameters)
{
	FTestRegionAdapter Adapter(TEXT("Fake"));
	Adapter.CurrentValue = MakeShared<FJsonValueString>(TEXT("Current"));
	const FAssetDocumentRegionPolicy Policy = FAssetDocumentRegionRuntimeTestFixture::MakePolicy(TEXT("Display/Name"), TEXT("Body.Display/Name"));
	const FAssetDocumentRegionContext Context = FAssetDocumentRegionRuntimeTestFixture::MakeRuntimeContext(TEXT("Display/Name"), TEXT("/Body/Display~1Name"), &Policy);

	TArray<TSharedPtr<FJsonValue>> DiffEntries;
	const FAssetDocumentCapabilityResult Result = FAssetDocumentRegionRuntime::DiffByCanonicalExtract(
		Context,
		MakeShared<FJsonValueString>(TEXT("Desired")),
		Adapter,
		DiffEntries);

	TestTrue(TEXT("Default runtime diff succeeds"), Result.bSuccess);
	TestEqual(TEXT("Default diff emits one changed entry"), DiffEntries.Num(), 1);
	if (DiffEntries.Num() < 1 || !DiffEntries[0].IsValid() || DiffEntries[0]->Type != EJson::Object)
	{
		return false;
	}
	TestEqual(TEXT("Diff entry path uses region JSON pointer"), DiffEntries[0]->AsObject()->GetStringField(TEXT("path")), FString(TEXT("/Body/Display~1Name")));
	TestEqual(TEXT("Default diff does not call adapter semantic diff hook"), Adapter.DiffCalls, 0);
	TestEqual(TEXT("Runtime extracts current value for default diff"), Adapter.ExtractCalls, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentRegionRuntimeExplicitEmptyRejectsUnexpectedNullTest,
	"UEAssetDoc.RegionRuntime.ExplicitEmpty.RejectsUnexpectedNull",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRegionRuntimeExplicitEmptyRejectsUnexpectedNullTest::RunTest(const FString& Parameters)
{
	FTestRegionAdapter Adapter(TEXT("Fake"));
	FAssetDocumentRegionPolicy Policy = FAssetDocumentRegionRuntimeTestFixture::MakePolicy(TEXT("Preview"), TEXT("Body.Preview"));
	FAssetDocumentRegionContext Context = FAssetDocumentRegionRuntimeTestFixture::MakeRuntimeContext(TEXT("Preview"), TEXT("/Body/Preview"), &Policy);

	const FAssetDocumentCapabilityResult Result = FAssetDocumentRegionRuntime::Validate(
		Context,
		MakeShared<FJsonValueNull>(),
		Adapter);

	TestFalse(TEXT("Unexpected null fails before adapter validation"), Result.bSuccess);
	TestEqual(TEXT("Adapter validate is not called"), Adapter.ValidateCalls, 0);
	TestEqual(TEXT("Unexpected null diagnostic path"), Result.Diagnostics.Num() > 0 ? Result.Diagnostics[0].Path : FString(), FString(TEXT("/Body/Preview")));
	TestEqual(TEXT("Unexpected null diagnostic code"), Result.Diagnostics.Num() > 0 ? Result.Diagnostics[0].Code : FString(), FString(TEXT("UnexpectedNullBodySection")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentRegionRuntimeDeferredRegionAcceptsEmptyArrayTest,
	"UEAssetDoc.RegionRuntime.DeferredRegion.AcceptsEmptyArray",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRegionRuntimeDeferredRegionAcceptsEmptyArrayTest::RunTest(const FString& Parameters)
{
	FAssetDocumentDeferredRegionAdapter Adapter;
	const FAssetDocumentRegionPolicy Policy =
		FAssetDocumentRegionRuntimeTestFixture::MakeDeferredPolicy(TEXT("Body.FunctionGraphs"), TEXT("Body.FunctionGraphs"), EAssetDocumentRegionKind::Graph);
	const FAssetDocumentRegionContext Context =
		FAssetDocumentRegionRuntimeTestFixture::MakeRuntimeContext(TEXT("Body.FunctionGraphs"), TEXT("/Body/FunctionGraphs"), &Policy);

	const FAssetDocumentCapabilityResult Result =
		FAssetDocumentRegionRuntime::Validate(Context, FAssetDocumentRegionRuntimeTestFixture::MakeArrayValue({}), Adapter);

	TestTrue(TEXT("Deferred empty array succeeds"), Result.bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentRegionRuntimeDeferredRegionAcceptsEmptyObjectTest,
	"UEAssetDoc.RegionRuntime.DeferredRegion.AcceptsEmptyObject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRegionRuntimeDeferredRegionAcceptsEmptyObjectTest::RunTest(const FString& Parameters)
{
	FAssetDocumentDeferredRegionAdapter Adapter;
	const FAssetDocumentRegionPolicy Policy =
		FAssetDocumentRegionRuntimeTestFixture::MakeDeferredPolicy(TEXT("Body.Preview"), TEXT("Body.Preview"), EAssetDocumentRegionKind::Object);
	const FAssetDocumentRegionContext Context =
		FAssetDocumentRegionRuntimeTestFixture::MakeRuntimeContext(TEXT("Body.Preview"), TEXT("/Body/Preview"), &Policy);

	const FAssetDocumentCapabilityResult Result =
		FAssetDocumentRegionRuntime::Validate(Context, FAssetDocumentRegionRuntimeTestFixture::MakeObjectValue(MakeShared<FJsonObject>()), Adapter);

	TestTrue(TEXT("Deferred empty object succeeds"), Result.bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentRegionRuntimeDeferredRegionRejectsNonEmptyArrayTest,
	"UEAssetDoc.RegionRuntime.DeferredRegion.RejectsNonEmptyArray",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRegionRuntimeDeferredRegionRejectsNonEmptyArrayTest::RunTest(const FString& Parameters)
{
	FAssetDocumentDeferredRegionAdapter Adapter;
	const FAssetDocumentRegionPolicy Policy =
		FAssetDocumentRegionRuntimeTestFixture::MakeDeferredPolicy(TEXT("Body.FunctionGraphs"), TEXT("Body.FunctionGraphs"), EAssetDocumentRegionKind::Graph);
	const FAssetDocumentRegionContext Context =
		FAssetDocumentRegionRuntimeTestFixture::MakeRuntimeContext(TEXT("Body.FunctionGraphs"), TEXT("/Body/FunctionGraphs"), &Policy);

	const FAssetDocumentCapabilityResult Result = FAssetDocumentRegionRuntime::Validate(
		Context,
		FAssetDocumentRegionRuntimeTestFixture::MakeArrayValue({MakeShared<FJsonValueString>(TEXT("Graph"))}),
		Adapter);

	TestFalse(TEXT("Deferred non-empty array fails"), Result.bSuccess);
	FAssetDocumentRegionRuntimeTestFixture::TestDiagnostic(this, TEXT("Non-empty array"), Result, TEXT("/Body/FunctionGraphs"), TEXT("UnsupportedRegion"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentRegionRuntimeDeferredRegionRejectsNonEmptyObjectTest,
	"UEAssetDoc.RegionRuntime.DeferredRegion.RejectsNonEmptyObject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRegionRuntimeDeferredRegionRejectsNonEmptyObjectTest::RunTest(const FString& Parameters)
{
	FAssetDocumentDeferredRegionAdapter Adapter;
	const FAssetDocumentRegionPolicy Policy =
		FAssetDocumentRegionRuntimeTestFixture::MakeDeferredPolicy(TEXT("Body.Preview"), TEXT("Body.Preview"), EAssetDocumentRegionKind::Object);
	const FAssetDocumentRegionContext Context =
		FAssetDocumentRegionRuntimeTestFixture::MakeRuntimeContext(TEXT("Body.Preview"), TEXT("/Body/Preview"), &Policy);
	TSharedRef<FJsonObject> NonEmptyObject = MakeShared<FJsonObject>();
	NonEmptyObject->SetStringField(TEXT("Name"), TEXT("Preview"));

	const FAssetDocumentCapabilityResult Result =
		FAssetDocumentRegionRuntime::Validate(Context, FAssetDocumentRegionRuntimeTestFixture::MakeObjectValue(NonEmptyObject), Adapter);

	TestFalse(TEXT("Deferred non-empty object fails"), Result.bSuccess);
	FAssetDocumentRegionRuntimeTestFixture::TestDiagnostic(this, TEXT("Non-empty object"), Result, TEXT("/Body/Preview"), TEXT("UnsupportedRegion"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentRegionRuntimeDeferredRegionExtractsDeclaredDefaultTest,
	"UEAssetDoc.RegionRuntime.DeferredRegion.ExtractsDeclaredDefault",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRegionRuntimeDeferredRegionExtractsDeclaredDefaultTest::RunTest(const FString& Parameters)
{
	FAssetDocumentDeferredRegionAdapter Adapter;
	const FAssetDocumentRegionPolicy Policy =
		FAssetDocumentRegionRuntimeTestFixture::MakeDeferredPolicy(TEXT("Body.FunctionGraphs"), TEXT("Body.FunctionGraphs"), EAssetDocumentRegionKind::Graph);
	const FAssetDocumentRegionContext Context =
		FAssetDocumentRegionRuntimeTestFixture::MakeRuntimeContext(TEXT("Body.FunctionGraphs"), TEXT("/Body/FunctionGraphs"), &Policy);

	TSharedPtr<FJsonValue> ExtractedValue;
	const FAssetDocumentCapabilityResult Result =
		FAssetDocumentRegionRuntime::Extract(Context, Adapter, ExtractedValue);

	TestTrue(TEXT("Deferred extract succeeds"), Result.bSuccess);
	TestTrue(TEXT("Deferred graph default is array"), ExtractedValue.IsValid() && ExtractedValue->Type == EJson::Array);
	TestEqual(TEXT("Deferred graph default is empty"), ExtractedValue.IsValid() ? ExtractedValue->AsArray().Num() : -1, 0);
	return true;
}

#endif
