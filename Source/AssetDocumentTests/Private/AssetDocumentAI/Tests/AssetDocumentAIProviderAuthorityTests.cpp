// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#if WITH_DEV_AUTOMATION_TESTS

#include "Tests/Fixtures/AssetDocumentAITestFixture.h"

#include "AssetDocumentAIPrimitiveProvider.h"
#include "AssetDocumentEditorNativeState.h"
#include "BehaviorTree/AssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollback.h"
#include "BehaviorTree/BehaviorTreeTypes.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTreeEditorModule.h"
#include "BehaviorTreeFactory.h"
#include "EdGraph/EdGraphSchema.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "IBehaviorTreeEditor.h"
#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PropertyHandle.h"
#include "Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardStructuralPublication.h"
#include "Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardKeyTypeStructCanonicalRead.h"
#include "Runtime/AssetDocumentRequestCoordinator.h"
#include "TypedPreimage/AssetDocumentAIBlackboardTypedPreimage.h"
#include "TypedPreimage/AssetDocumentAISelectorTypedPreimage.h"

#include <type_traits>
#include "AssetDocumentPluginPaths.h"

namespace AssetDocumentAIProviderAuthorityTests
{
struct FStructuralPublicationSource
{
	FString RelativePath;
	FString Text;
};

int32 CountOccurrences(FStringView Text, FStringView Needle)
{
	int32 Count = 0;
	int32 Offset = 0;
	while (Offset < Text.Len())
	{
		const int32 Index = Text.Find(Needle, Offset, ESearchCase::CaseSensitive);
		if (Index == INDEX_NONE) break;
		++Count;
		Offset = Index + Needle.Len();
	}
	return Count;
}

int32 CountCodeOccurrences(FStringView Text, FStringView Needle)
{
	FString Code(Text);
	bool bInLineComment = false;
	bool bInBlockComment = false;
	TCHAR Quote = 0;
	bool bEscaped = false;
	for (int32 Index = 0; Index < Code.Len(); ++Index)
	{
		const TCHAR Current = Code[Index];
		const TCHAR Next = Index + 1 < Code.Len() ? Code[Index + 1] : 0;
		if (bInLineComment)
		{
			if (Current == TEXT('\n')) bInLineComment = false;
			else Code[Index] = TEXT(' ');
			continue;
		}
		if (bInBlockComment)
		{
			Code[Index] = TEXT(' ');
			if (Current == TEXT('*') && Next == TEXT('/'))
			{
				Code[++Index] = TEXT(' ');
				bInBlockComment = false;
			}
			continue;
		}
		if (Quote != 0)
		{
			Code[Index] = TEXT(' ');
			if (!bEscaped && Current == Quote) Quote = 0;
			bEscaped = !bEscaped && Current == TEXT('\\');
			continue;
		}
		if (Current == TEXT('/') && Next == TEXT('/'))
		{
			Code[Index] = Code[++Index] = TEXT(' ');
			bInLineComment = true;
		}
		else if (Current == TEXT('/') && Next == TEXT('*'))
		{
			Code[Index] = Code[++Index] = TEXT(' ');
			bInBlockComment = true;
		}
		else if (Current == TEXT('"') || Current == TEXT('\''))
		{
			Code[Index] = TEXT(' ');
			Quote = Current;
			bEscaped = false;
		}
	}
	return CountOccurrences(Code, Needle);
}

bool LoadStructuralPublicationSources(
	FAutomationTestBase& Test,
	TArray<FStructuralPublicationSource>& OutSources)
{
	const FString SourceRoot = FPaths::Combine(GetAssetDocumentPluginBaseDir(), TEXT("Source/AssetDocumentAI"));
	TArray<FString> Paths;
	IFileManager::Get().FindFilesRecursive(Paths, *SourceRoot, TEXT("*"), true, false);
	for (const FString& Path : Paths)
	{
		const FString Extension = FPaths::GetExtension(Path);
		if (Extension != TEXT("h") && Extension != TEXT("cpp")) continue;
		FString RelativePath = Path;
		FPaths::MakePathRelativeTo(RelativePath, *(SourceRoot + TEXT("/")));
		FPaths::NormalizeFilename(RelativePath);
		if (RelativePath.StartsWith(TEXT("Private/Tests/"))) continue;
		FStructuralPublicationSource& Source = OutSources.AddDefaulted_GetRef();
		Source.RelativePath = MoveTemp(RelativePath);
		if (!Test.TestTrue(
			*FString::Printf(TEXT("structural production source is readable: %s"), *Source.RelativePath),
			FFileHelper::LoadFileToString(Source.Text, *Path))) return false;
	}
	OutSources.Sort([](const auto& Left, const auto& Right)
	{
		return Left.RelativePath < Right.RelativePath;
	});
	return true;
}

const FStructuralPublicationSource* FindStructuralPublicationSource(
	TConstArrayView<FStructuralPublicationSource> Sources,
	FStringView RelativePath)
{
	for (const FStructuralPublicationSource& Source : Sources)
	{
		if (Source.RelativePath == RelativePath) return &Source;
	}
	return nullptr;
}

int32 CountAcrossStructuralPublicationSources(
	TConstArrayView<FStructuralPublicationSource> Sources,
	FStringView Needle)
{
	int32 Count = 0;
	for (const FStructuralPublicationSource& Source : Sources)
	{
		Count += CountOccurrences(Source.Text, Needle);
	}
	return Count;
}

void RequireOrderedStructuralTokens(
	FAutomationTestBase& Test,
	FStringView Text,
	TConstArrayView<const TCHAR*> Tokens)
{
	int32 Previous = INDEX_NONE;
	for (const TCHAR* Token : Tokens)
	{
		const int32 Position = Text.Find(Token, Previous + 1, ESearchCase::CaseSensitive);
		Test.TestTrue(*FString::Printf(TEXT("structural packet contains ordered token: %s"), Token),
			Position != INDEX_NONE && Position > Previous);
		Previous = Position;
	}
}

bool ExtractExactFunctionPacket(FStringView Source, FStringView Symbol, FString& OutPacket)
{
	int32 SearchFrom = 0;
	for (;;)
	{
		const int32 SymbolStart = Source.Find(Symbol, SearchFrom, ESearchCase::CaseSensitive);
		if (SymbolStart == INDEX_NONE) return false;
		const int32 OpenBrace = Source.Find(TEXTVIEW("{"), SymbolStart, ESearchCase::CaseSensitive);
		const int32 Semicolon = Source.Find(TEXTVIEW(";"), SymbolStart, ESearchCase::CaseSensitive);
		if (OpenBrace == INDEX_NONE || (Semicolon != INDEX_NONE && Semicolon < OpenBrace))
		{
			SearchFrom = SymbolStart + Symbol.Len();
			continue;
		}
		int32 Depth = 0;
		for (int32 Index = OpenBrace; Index < Source.Len(); ++Index)
		{
			if (Source[Index] == TEXT('{')) ++Depth;
			else if (Source[Index] == TEXT('}') && --Depth == 0)
			{
				OutPacket = FString(Source.Mid(SymbolStart, Index - SymbolStart + 1));
				return true;
			}
		}
		return false;
	}
}

void RequireBlackboardRowTopologySourceCheck(FAutomationTestBase& Test)
{
	TArray<FStructuralPublicationSource> Sources;
	if (!LoadStructuralPublicationSources(Test, Sources)) return;
	const FStructuralPublicationSource* Internal = FindStructuralPublicationSource(Sources,
		TEXT("Private/Provider/Blackboard/AssetDocumentAIBlackboardMutationInternal.h"));
	const FStructuralPublicationSource* Mutation = FindStructuralPublicationSource(Sources,
		TEXT("Private/Provider/Blackboard/AssetDocumentAIBlackboardMutation.cpp"));
	const FStructuralPublicationSource* Update = FindStructuralPublicationSource(Sources,
		TEXT("Private/Provider/Blackboard/AssetDocumentAIBlackboardKeyTypePropertyMutation.cpp"));
	const FStructuralPublicationSource* Add = FindStructuralPublicationSource(Sources,
		TEXT("Private/Provider/Blackboard/AssetDocumentAIBlackboardEntryAdd.cpp"));
	const FStructuralPublicationSource* Replace = FindStructuralPublicationSource(Sources,
		TEXT("Private/Provider/Blackboard/AssetDocumentAIBlackboardEntryReplaceClass.cpp"));
	const FStructuralPublicationSource* Remove = FindStructuralPublicationSource(Sources,
		TEXT("Private/Provider/Blackboard/AssetDocumentAIBlackboardEntryRemove.cpp"));
	const FStructuralPublicationSource* Reorder = FindStructuralPublicationSource(Sources,
		TEXT("Private/Provider/Blackboard/AssetDocumentAIBlackboardLocalKeysReorder.cpp"));
	const FStructuralPublicationSource* ReorderDescriptor = FindStructuralPublicationSource(Sources,
		TEXT("Private/Provider/Blackboard/AssetDocumentAIBlackboardLocalKeysReorderDescriptor.cpp"));
	const FStructuralPublicationSource* ReorderParser = FindStructuralPublicationSource(Sources,
		TEXT("Private/Provider/AssetDocumentAIProviderContractsLocalKeysReorder.cpp"));
	if (!Test.TestNotNull(TEXT("row-topology private declarations exist"), Internal)
		|| !Test.TestNotNull(TEXT("row-topology mutation owner exists"), Mutation)
		|| !Test.TestNotNull(TEXT("row-topology update owner exists"), Update)
		|| !Test.TestNotNull(TEXT("row-topology ADD owner exists"), Add)
		|| !Test.TestNotNull(TEXT("row-topology replace owner exists"), Replace)
		|| !Test.TestNotNull(TEXT("row-topology REMOVE owner exists"), Remove)
		|| !Test.TestNotNull(TEXT("SC-REORDER admission handler exists"), Reorder)
		|| !Test.TestNotNull(TEXT("SC-REORDER admission descriptor exists"), ReorderDescriptor)
		|| !Test.TestNotNull(TEXT("SC-REORDER sole parser exists"), ReorderParser)) return;

	Test.TestEqual(TEXT("RuntimeValueEquals has one private declaration"),
		CountOccurrences(Internal->Text, TEXT("RuntimeValueEquals(")), 1);
	Test.TestEqual(TEXT("RuntimeValueEquals has one definition"),
		CountOccurrences(Mutation->Text, TEXT("bool RuntimeValueEquals(")), 1);
	Test.TestTrue(TEXT("RuntimeValueEquals preserves BytesValue equality"),
		Mutation->Text.Contains(TEXT("Left.BytesValue != Right.BytesValue")));
	Test.TestEqual(TEXT("RuntimeValuesEqual plural is fully removed"),
		CountAcrossStructuralPublicationSources(Sources, TEXT("RuntimeValuesEqual")), 0);
	Test.TestEqual(TEXT("ADD row has one exact external definition"),
		CountOccurrences(Add->Text,
			TEXT("FAssetDocumentPrimitiveOutcome ApplyAssetDocumentAIBlackboardEntryAddWithIdentity(")), 1);
	Test.TestEqual(TEXT("ADD handler has one exact row call"),
		CountOccurrences(Mutation->Text,
			TEXT("ApplyAssetDocumentAIBlackboardEntryAddWithIdentity(")), 1);
	Test.TestEqual(TEXT("replace row has one exact external definition"),
		CountOccurrences(Replace->Text,
			TEXT("FAssetDocumentPrimitiveOutcome ApplyAssetDocumentAIBlackboardEntryReplaceClassWithIdentity(")), 1);
	Test.TestEqual(TEXT("replace handler has one exact row call"),
		CountOccurrences(Update->Text,
			TEXT("ApplyAssetDocumentAIBlackboardEntryReplaceClassWithIdentity(")), 1);
	Test.TestEqual(TEXT("former ADD body symbol is absent"),
		CountAcrossStructuralPublicationSources(Sources, TEXT("ApplyEntryAddWithIdentity(")), 0);
	Test.TestEqual(TEXT("former replace body symbol is absent"),
		CountAcrossStructuralPublicationSources(Sources, TEXT("ApplyEntryReplaceClassWithIdentity(")), 0);
	Test.TestEqual(TEXT("new ADD row cannot load request-selected references"),
		CountOccurrences(Add->Text, TEXT("TryLoad")), 0);
	Test.TestEqual(TEXT("new replace row cannot load request-selected references"),
		CountOccurrences(Replace->Text, TEXT("TryLoad")), 0);
	Test.TestEqual(TEXT("SC-REMOVE handler has one exact external definition"),
		CountOccurrences(Remove->Text,
			TEXT("FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBlackboardEntryRemove(")), 1);
	for (const TCHAR* Forbidden : {TEXT("StageKeyType"), TEXT("CanonicalWrite"),
		TEXT("FPreparedWrite"), TEXT("ResolveObject"), TEXT("TryLoad"), TEXT("LoadObject"),
		TEXT("AssetRegistry"), TEXT("TObjectIterator"), TEXT("ForEachObject"), TEXT("GUObjectArray"),
		TEXT("TFieldIterator"), TEXT("Locator"), TEXT("GetChildHandle"), TEXT("CreateSingleProperty"),
		TEXT("IDetailTreeNode"), TEXT("EnumerateRawData"), TEXT("ExportText"),
		TEXT("ImportText"), TEXT("FJsonObjectConverter"), TEXT("NewObject"),
		TEXT("StaticConstructObject"), TEXT("DuplicateObject")})
	{
			Test.TestEqual(*FString::Printf(TEXT("SC-REMOVE row forbids %s"), Forbidden),
				CountOccurrences(Remove->Text, Forbidden), 0);
		}
	Test.TestEqual(TEXT("SC-REORDER handler has one exact external definition"),
		CountOccurrences(Reorder->Text,
			TEXT("FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBlackboardLocalKeysReorder(")), 1);
	Test.TestEqual(TEXT("SC-REORDER parser has one exact private definition"),
		CountOccurrences(ReorderParser->Text,
			TEXT("bool ParseAssetDocumentAIBlackboardLocalKeysReorderRequest(")), 1);
	Test.TestEqual(TEXT("SC-REORDER descriptor has one exact factory"),
		CountOccurrences(ReorderDescriptor->Text,
			TEXT("FAssetDocumentPrimitiveDescriptor MakeAssetDocumentAIBlackboardLocalKeysReorderV2Descriptor(")), 1);
	for (const TCHAR* Required : {TEXT("AcquireAssetDocumentAIMutableAssetLease"),
		TEXT("IsAssetDocumentAITransactionValid"), TEXT("WithAssetDocumentEditorAssetIdentity"),
		TEXT("CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage"),
		TEXT("EnlistFootprint"), TEXT("PublishLocalKeysSnapshot"),
		TEXT("CompletePublishedLoadedConsumers"),
		TEXT("BuildAssetDocumentAIBlackboardCanonicalKeyTypeProperties")})
	{
		Test.TestEqual(*FString::Printf(TEXT("SC-REORDER handler retains %s"), Required),
			CountOccurrences(Reorder->Text, Required), 1);
	}
	for (const TCHAR* Forbidden : {TEXT("ResolveObject"), TEXT("TryLoad"), TEXT("LoadObject"),
		TEXT("FPreparedWrite"), TEXT("CanonicalWrite"), TEXT("StageKeyType"), TEXT("NewObject"),
		TEXT("StaticConstructObject"), TEXT("DuplicateObject"), TEXT("Locator"),
		TEXT("GetChildHandle"), TEXT("CreateSingleProperty"), TEXT("IDetailTreeNode"),
		TEXT("EnumerateRawData"), TEXT("ExportText"), TEXT("ImportText"),
		TEXT("FJsonObjectConverter")})
	{
		Test.TestEqual(*FString::Printf(TEXT("SC-REORDER handler forbids %s"), Forbidden),
			CountOccurrences(Reorder->Text, Forbidden), 0);
	}
	Test.TestEqual(TEXT("SC-REORDER fingerprint has one canonical READ governance reference"),
		CountOccurrences(ReorderDescriptor->Text,
			TEXT("AD.AI.BLACKBOARD.KEYTYPE-STRUCT-CANONICAL-READ-v1")), 1);
	Test.TestEqual(TEXT("SC-REORDER descriptor excludes canonical WRITE governance"),
		CountOccurrences(ReorderDescriptor->Text,
			TEXT("AD.AI.BLACKBOARD.KEYTYPE-STRUCT-CANONICAL-WRITE-v1")), 0);

	for (const TCHAR* Symbol : {TEXT("ApplyAssetDocumentAIBlackboardEntryAddWithIdentity("),
		TEXT("ApplyAssetDocumentAIBlackboardEntryReplaceClassWithIdentity(")})
	{
		const int32 Start = Internal->Text.Find(Symbol, ESearchCase::CaseSensitive);
		const int32 End = Internal->Text.Find(TEXT(";"), ESearchCase::CaseSensitive,
			ESearchDir::FromStart, Start);
		const FString Declaration = Start == INDEX_NONE || End == INDEX_NONE
			? FString() : Internal->Text.Mid(Start, End - Start + 1);
		Test.TestTrue(*FString::Printf(TEXT("row seam declaration exists: %s"), Symbol),
			!Declaration.IsEmpty());
		for (const TCHAR* Forbidden : {TEXT("IPropertyHandle"), TEXT("IPropertyRowGenerator"),
			TEXT("IDetailTreeNode"), TEXT("FPreparedWrite"), TEXT("TUniquePtr"), TEXT("TSharedPtr")})
		{
			Test.TestFalse(*FString::Printf(TEXT("row seam excludes handle-bearing type %s"), Forbidden),
				Declaration.Contains(Forbidden));
		}
	}
}

void RequireStructCanonicalWriteAuthority(FAutomationTestBase& Test)
{
	TArray<FStructuralPublicationSource> Sources;
	if (!LoadStructuralPublicationSources(Test, Sources)) return;
	const auto* Header = FindStructuralPublicationSource(Sources, TEXT(
		"Private/Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite.h"));
	const auto* Implementation = FindStructuralPublicationSource(Sources, TEXT(
		"Private/Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite.cpp"));
	const auto* Decode = FindStructuralPublicationSource(Sources, TEXT(
		"Private/Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardKeyTypeStructCanonicalWriteDecode.cpp"));
	const auto* Mutation = FindStructuralPublicationSource(Sources, TEXT(
		"Private/Provider/Blackboard/AssetDocumentAIBlackboardMutation.cpp"));
	const auto* Update = FindStructuralPublicationSource(Sources, TEXT(
		"Private/Provider/Blackboard/AssetDocumentAIBlackboardKeyTypePropertyMutation.cpp"));
	const auto* Add = FindStructuralPublicationSource(Sources, TEXT(
		"Private/Provider/Blackboard/AssetDocumentAIBlackboardEntryAdd.cpp"));
	const auto* Replace = FindStructuralPublicationSource(Sources, TEXT(
		"Private/Provider/Blackboard/AssetDocumentAIBlackboardEntryReplaceClass.cpp"));
	if (!Test.TestNotNull(TEXT("STATIC-SC-WRITE-AUTHORITY-001 private WRITE header exists"), Header)
		|| !Test.TestNotNull(TEXT("STATIC-SC-WRITE-AUTHORITY-001 private WRITE implementation exists"), Implementation)
		|| !Test.TestNotNull(TEXT("STATIC-SC-WRITE-AUTHORITY-001 private WRITE decode exists"), Decode)
		|| !Test.TestNotNull(TEXT("STATIC-SC-WRITE-AUTHORITY-001 shared Runtime equality owner exists"), Mutation)
		|| !Test.TestNotNull(TEXT("STATIC-SC-WRITE-AUTHORITY-001 update caller exists"), Update)
		|| !Test.TestNotNull(TEXT("STATIC-SC-WRITE-AUTHORITY-001 add caller exists"), Add)
		|| !Test.TestNotNull(TEXT("STATIC-SC-WRITE-AUTHORITY-001 replace caller exists"), Replace)) return;

	constexpr TCHAR WriteId[] = TEXT("AD.AI.BLACKBOARD.KEYTYPE-STRUCT-CANONICAL-WRITE-v1");
	Test.TestEqual(TEXT("STATIC-SC-WRITE-AUTHORITY-001 WRITE governance ID has one private source occurrence"),
		CountAcrossStructuralPublicationSources(Sources, WriteId), 1);
	Test.TestEqual(TEXT("STATIC-SC-WRITE-AUTHORITY-001 WRITE governance ID is owned by its private header"),
		CountOccurrences(Header->Text, WriteId), 1);
	Test.TestEqual(TEXT("STATIC-SC-WRITE-AUTHORITY-001 freezes exactly three WRITE callers"),
		CountOccurrences(Header->Text,
			TEXT("enum class ECaller : uint8 { UpdateExistingV2, AddV2, ReplaceClassV2 };")), 1);
	Test.TestEqual(TEXT("STATIC-SC-WRITE-AUTHORITY-001 has no readback WRITE caller"),
		CountAcrossStructuralPublicationSources(Sources, TEXT("ECaller::H0")), 0);
	for (const TCHAR* ForbiddenCaller : {TEXT("ECaller::ApplyPropertiesV1"),
		TEXT("ECaller::Remove"), TEXT("ECaller::Reorder")})
	{
		Test.TestEqual(*FString::Printf(TEXT("STATIC-SC-WRITE-AUTHORITY-001 forbids %s"), ForbiddenCaller),
			CountAcrossStructuralPublicationSources(Sources, ForbiddenCaller), 0);
	}

	Test.TestEqual(TEXT("STATIC-SC-WRITE-AUTHORITY-001 Prepare declaration is unique"),
		CountCodeOccurrences(Header->Text, TEXT("FPrepareResult PrepareCanonicalWrite(ECaller Caller,")), 1);
	Test.TestEqual(TEXT("STATIC-SC-WRITE-AUTHORITY-001 Prepare definition is Decode-owned"),
		CountCodeOccurrences(Decode->Text, TEXT("FPrepareResult PrepareCanonicalWrite(")), 1);
	Test.TestEqual(TEXT("STATIC-SC-WRITE-AUTHORITY-001 Apply declaration is unique"),
		CountCodeOccurrences(Header->Text, TEXT("FApplyResult ApplyPreparedCanonicalWrite(")), 1);
	Test.TestEqual(TEXT("STATIC-SC-WRITE-AUTHORITY-001 Apply definition is main-owner-owned"),
		CountCodeOccurrences(Implementation->Text, TEXT("FApplyResult ApplyPreparedCanonicalWrite(")), 1);
	Test.TestEqual(TEXT("STATIC-SC-WRITE-AUTHORITY-001 Prepare has only declaration definition three callers and three catalog fingerprints"),
		CountAcrossStructuralPublicationSources(Sources, TEXT("PrepareCanonicalWrite(")), 9);
	Test.TestEqual(TEXT("STATIC-SC-WRITE-AUTHORITY-001 Apply has only definition three callers and three catalog fingerprints"),
		CountAcrossStructuralPublicationSources(Sources, TEXT("ApplyPreparedCanonicalWrite(")), 8);

	struct FCallerPackets
	{
		const FStructuralPublicationSource* Source;
		const TCHAR* PrepareSymbol;
		const TCHAR* ApplySymbol;
		const TCHAR* Caller;
		int32 CallerOccurrencesPerPacket;
	};
	const FCallerPackets Callers[] = {
		{Update, TEXT("bool PrepareEntryMetadataForwardWrite("),
			TEXT("FAssetDocumentPrimitiveOutcome ApplyEntryMetadataForwardWriteChanges("), TEXT("UpdateExistingV2"), 1},
		{Add, TEXT("FAssetDocumentPrimitiveOutcome ApplyAssetDocumentAIBlackboardEntryAddWithIdentity("),
			TEXT("FAssetDocumentPrimitiveOutcome ApplyAssetDocumentAIBlackboardEntryAddWithIdentity("), TEXT("AddV2"), 2},
		{Replace, TEXT("TOptional<FAssetDocumentPrimitiveOutcome> PreflightEntryReplaceClass("),
			TEXT("FAssetDocumentPrimitiveOutcome ApplyAssetDocumentAIBlackboardEntryReplaceClassWithIdentity("), TEXT("ReplaceClassV2"), 1},
	};
	for (const FCallerPackets& Caller : Callers)
	{
		FString PreparePacket;
		FString ApplyPacket;
		if (Test.TestTrue(*FString::Printf(TEXT("STATIC-SC-WRITE-AUTHORITY-001 Prepare packet exists: %s"), Caller.Caller),
			ExtractExactFunctionPacket(Caller.Source->Text, Caller.PrepareSymbol, PreparePacket)))
		{
			Test.TestEqual(*FString::Printf(TEXT("STATIC-SC-WRITE-AUTHORITY-001 one Prepare caller: %s"), Caller.Caller),
				CountCodeOccurrences(PreparePacket, TEXT("PrepareCanonicalWrite(")), 1);
			Test.TestEqual(*FString::Printf(TEXT("STATIC-SC-WRITE-AUTHORITY-001 Prepare uses exact caller: %s"), Caller.Caller),
				CountCodeOccurrences(PreparePacket, Caller.Caller), Caller.CallerOccurrencesPerPacket);
		}
		if (Test.TestTrue(*FString::Printf(TEXT("STATIC-SC-WRITE-AUTHORITY-001 Apply packet exists: %s"), Caller.Caller),
			ExtractExactFunctionPacket(Caller.Source->Text, Caller.ApplySymbol, ApplyPacket)))
		{
			Test.TestEqual(*FString::Printf(TEXT("STATIC-SC-WRITE-AUTHORITY-001 at most one Apply caller: %s"), Caller.Caller),
				CountCodeOccurrences(ApplyPacket, TEXT("ApplyPreparedCanonicalWrite(")), 1);
			Test.TestEqual(*FString::Printf(TEXT("STATIC-SC-WRITE-AUTHORITY-001 Apply uses exact caller: %s"), Caller.Caller),
				CountCodeOccurrences(ApplyPacket, Caller.Caller), Caller.CallerOccurrencesPerPacket);
		}
	}

	FString LifecyclePacket;
	if (Test.TestTrue(TEXT("STATIC-SC-WRITE-AUTHORITY-001 main lifecycle packet exists"),
		ExtractExactFunctionPacket(Implementation->Text, TEXT("FApplyResult ApplyPreparedCanonicalWrite("), LifecyclePacket)))
	{
		for (const TCHAR* Token : {TEXT("Target.PreEditChange("), TEXT("Target.DefaultValue ="),
			TEXT("Target.PostEditChangeProperty("), TEXT("CanonicalRead::ReadCanonicalValue(Target.DefaultValue,")})
		{
			Test.TestEqual(*FString::Printf(TEXT("STATIC-SC-WRITE-AUTHORITY-001 one lifecycle token: %s"), Token),
				CountCodeOccurrences(LifecyclePacket, Token), 1);
		}
		RequireOrderedStructuralTokens(Test, LifecyclePacket, {TEXT("Target.PreEditChange("),
			TEXT("Target.DefaultValue ="), TEXT("Target.PostEditChangeProperty("),
			TEXT("CanonicalRead::ReadCanonicalValue(Target.DefaultValue,")});
	}

	for (const FStructuralPublicationSource* Owner : {Header, Implementation, Decode})
	{
		for (const TCHAR* Forbidden : {TEXT("IPropertyHandle"), TEXT("IPropertyRowGenerator"),
			TEXT("IDetailTreeNode"), TEXT("EnumerateRawData"), TEXT("PropertyEditor"),
			TEXT("ExportText"), TEXT("ImportText"), TEXT("FJsonObjectConverter"),
			TEXT("LocatorCodec"), TEXT("AssetRegistry"), TEXT("TObjectIterator"),
			TEXT("ForEachObject"), TEXT("GUObjectArray"), TEXT("FScopedTransaction")})
		{
			Test.TestEqual(*FString::Printf(TEXT("STATIC-SC-WRITE-AUTHORITY-001 WRITE owner forbids %s"), Forbidden),
				CountOccurrences(Owner->Text, Forbidden), 0);
		}
	}
	for (const FStructuralPublicationSource* StateOwner : {Header, Decode})
	{
		FString PreparedPacket;
		const TCHAR* Symbol = StateOwner == Header ? TEXT("class ASSETDOCUMENTAI_API FPreparedWrite final") : TEXT("class FPreparedWriteState");
		if (!Test.TestTrue(*FString::Printf(TEXT("STATIC-SC-WRITE-AUTHORITY-001 prepared-state packet exists: %s"), Symbol),
			ExtractExactFunctionPacket(StateOwner->Text, Symbol, PreparedPacket))) continue;
		for (const TCHAR* Forbidden : {TEXT("IPropertyHandle"), TEXT("IPropertyRowGenerator"),
			TEXT("IDetailTreeNode"), TEXT("FAssetDocumentMutationFootprint"), TEXT("FAssetDocumentAtomicAction")})
		{
			Test.TestEqual(*FString::Printf(TEXT("STATIC-SC-WRITE-AUTHORITY-001 prepared state forbids %s"), Forbidden),
				CountOccurrences(PreparedPacket, Forbidden), 0);
		}
	}

	FString EqualityPacket;
	if (Test.TestTrue(TEXT("STATIC-SC-WRITE-AUTHORITY-001 sole shared equality packet exists"),
		ExtractExactFunctionPacket(Mutation->Text, TEXT("bool RuntimeValueEquals("), EqualityPacket)))
	{
		Test.TestTrue(TEXT("STATIC-SC-WRITE-AUTHORITY-001 shared DTO equality compares BytesValue"),
			EqualityPacket.Contains(TEXT("Left.BytesValue != Right.BytesValue")));
	}
	Test.TestEqual(TEXT("STATIC-SC-WRITE-AUTHORITY-001 main WRITE owner has no local Equals body"),
		CountCodeOccurrences(Implementation->Text, TEXT("bool Equals(")), 0);
	Test.TestEqual(TEXT("STATIC-SC-WRITE-AUTHORITY-001 Decode WRITE owner has no local Equals body"),
		CountCodeOccurrences(Decode->Text, TEXT("bool Equals(")), 0);
	Test.TestEqual(TEXT("STATIC-SC-WRITE-AUTHORITY-001 main WRITE owner uses shared equality twice"),
		CountCodeOccurrences(Implementation->Text,
			TEXT("AssetDocumentAIBlackboardMutation::RuntimeValueEquals(")), 2);
	Test.TestEqual(TEXT("STATIC-SC-WRITE-AUTHORITY-001 Decode WRITE owner uses shared equality once"),
		CountCodeOccurrences(Decode->Text,
			TEXT("AssetDocumentAIBlackboardMutation::RuntimeValueEquals(")), 1);
}

void RequireStructuralPublicationSourceCheck(FAutomationTestBase& Test)
{
	TArray<FStructuralPublicationSource> Sources;
	if (!LoadStructuralPublicationSources(Test, Sources)) return;
	const FString HeaderPath = TEXT("Private/Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardStructuralPublication.h");
	const FString ImplementationPath = TEXT("Private/Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardStructuralPublication.cpp");
	const FString LocalKeysPath = TEXT("Private/Provider/Blackboard/AssetDocumentAIBlackboardLocalKeysMutation.cpp");
	const FString AddPath = TEXT("Private/Provider/Blackboard/AssetDocumentAIBlackboardEntryAdd.cpp");
	const FString ReplaceClassPath = TEXT("Private/Provider/Blackboard/AssetDocumentAIBlackboardEntryReplaceClass.cpp");
	const FString RemovePath = TEXT("Private/Provider/Blackboard/AssetDocumentAIBlackboardEntryRemove.cpp");
	const FString RollbackPath = TEXT("Private/TypedPreimage/AssetDocumentAIBlackboardTypedPreimage.cpp");
	const FStructuralPublicationSource* Header = FindStructuralPublicationSource(Sources, HeaderPath);
	const FStructuralPublicationSource* Implementation = FindStructuralPublicationSource(Sources, ImplementationPath);
	const FStructuralPublicationSource* LocalKeys = FindStructuralPublicationSource(Sources, LocalKeysPath);
	const FStructuralPublicationSource* Add = FindStructuralPublicationSource(Sources, AddPath);
	const FStructuralPublicationSource* ReplaceClass = FindStructuralPublicationSource(Sources, ReplaceClassPath);
	const FStructuralPublicationSource* Remove = FindStructuralPublicationSource(Sources, RemovePath);
	const FStructuralPublicationSource* Rollback = FindStructuralPublicationSource(Sources, RollbackPath);
	Test.TestNotNull(TEXT("private structural publication header exists"), Header);
	Test.TestNotNull(TEXT("private structural publication implementation exists"), Implementation);
	if (!Test.TestNotNull(TEXT("immutable v1 local-keys implementation exists"), LocalKeys)
		|| !Test.TestNotNull(TEXT("v2 entry-add implementation exists"), Add)
		|| !Test.TestNotNull(TEXT("v2 replace-class implementation exists"), ReplaceClass)
		|| !Test.TestNotNull(TEXT("v2 entry-remove implementation exists"), Remove)
		|| !Test.TestNotNull(TEXT("typed-preimage rollback implementation exists"), Rollback)) return;
	Test.TestEqual(TEXT("StageKeyType has one production raw construction site"),
		CountAcrossStructuralPublicationSources(Sources, TEXT("NewObject<UBlackboardKeyType>")), 1);
	if (Implementation != nullptr)
	{
		Test.TestEqual(TEXT("forward publication has one quarantine assignment"),
			CountOccurrences(Implementation->Text, TEXT("Blackboard.Keys = Snapshot;")), 1);
	}
	Test.TestEqual(TEXT("v1 no longer owns a second forward assignment"),
		CountOccurrences(LocalKeys->Text, TEXT("Blackboard.Keys = Desired;")), 0);
	Test.TestEqual(TEXT("typed-preimage keeps one rollback-only assignment"),
		CountOccurrences(Rollback->Text, TEXT("CurrentRoot->Keys = Keys;")), 1);
	Test.TestEqual(TEXT("production has exactly two Blackboard Keys assignments"),
		CountAcrossStructuralPublicationSources(Sources, TEXT(".Keys ="))
			+ CountAcrossStructuralPublicationSources(Sources, TEXT("->Keys =")), 2);
	if (Header != nullptr)
	{
		Test.TestTrue(TEXT("staging lifetime is represented by an Atomic transaction binding"),
			Header->Text.Contains(TEXT("FStructuralPublicationTransactionBinding")));
		Test.TestFalse(TEXT("staging/publication authority has no forgeable context booleans"),
			Header->Text.Contains(TEXT("bTypedPreimageCaptured"))
				|| Header->Text.Contains(TEXT("bAtomicFootprintEnlisted"))
				|| Header->Text.Contains(TEXT("bSnapshotPreflighted")));
		Test.TestEqual(TEXT("StageKeyType private symbol is declared once"),
			CountOccurrences(Header->Text, TEXT("StageKeyType(")), 1);
		Test.TestEqual(TEXT("publisher private symbol is declared once"),
			CountOccurrences(Header->Text, TEXT("PublishLocalKeysSnapshot(")), 1);
		Test.TestTrue(TEXT("binding is the exact private Atomic transaction decorator"),
			Header->Text.Contains(TEXT(": public IAssetDocumentAtomicTransaction"))
				&& Header->Text.Contains(TEXT("TWeakObjectPtr<UBlackboardData> ExactTarget;"))
				&& Header->Text.Contains(TEXT("FAssetDocumentMutationToken InitialToken;")));
	}
	if (Implementation != nullptr)
	{
		Test.TestEqual(TEXT("StageKeyType private symbol is defined once"),
			CountOccurrences(Implementation->Text, TEXT("StageKeyType(")), 1);
		Test.TestEqual(TEXT("publisher private symbol is defined once"),
			CountOccurrences(Implementation->Text, TEXT("PublishLocalKeysSnapshot(")), 1);
		Test.TestEqual(TEXT("staging derives one collision-free internal name"),
			CountOccurrences(Implementation->Text,
				TEXT("MakeUniqueObjectName(&Blackboard, KeyTypeClass,")), 1);
		Test.TestEqual(TEXT("staging uses the frozen internal name prefix once"),
			CountOccurrences(Implementation->Text,
				TEXT("TEXT(\"AssetDocumentAIBlackboardStagedKeyType\")")), 1);
		Test.TestEqual(TEXT("staging passes RF_Transactional at the raw site"),
			CountOccurrences(Implementation->Text, TEXT("RF_Transactional);")), 1);
		Test.TestEqual(TEXT("transaction staging state owns one exact strong pointer field"),
			CountOccurrences(Implementation->Text,
				TEXT("TStrongObjectPtr<UBlackboardKeyType> OwnedKeyType;")), 1);
		Test.TestTrue(TEXT("typed-preimage action is wrapped, not duplicated"),
			Implementation->Text.Contains(TEXT("FTransactionOwnedStagingRollbackAction"))
				&& Implementation->Text.Contains(TEXT("Inner->Rollback(StableFailureCode)"))
				&& Implementation->Text.Contains(TEXT("CompleteRollback(Result.bValid)")));
		Test.TestTrue(TEXT("exact Keys handle binds owner class, stable property and unique outer"),
			Implementation->Text.Contains(TEXT("Actual == Expected"))
				&& Implementation->Text.Contains(TEXT("KeysHandle->GeneratePathToProperty()"))
				&& Implementation->Text.Contains(TEXT("Actual->GetOwnerClass() == UBlackboardData::StaticClass()"))
				&& Implementation->Text.Contains(TEXT("Actual->GetPathName() == Expected->GetPathName()"))
				&& Implementation->Text.Contains(TEXT("OuterObjects.Num() == 1")));
		RequireOrderedStructuralTokens(Test, Implementation->Text, {
			TEXT("KeysHandle->NotifyPreChange();"),
			TEXT("Blackboard.Keys = Snapshot;"),
			TEXT("KeysHandle->NotifyPostChange(EPropertyChangeType::ValueSet);"),
			TEXT("KeysHandle->NotifyFinishedChangingProperties();"),
			TEXT("Blackboard.UpdateParentKeys();"),
			TEXT("Blackboard.UpdateKeyIDs();")});
		for (const TCHAR* Forbidden : {TEXT("MarkAsGarbage"), TEXT("Slate"), TEXT("Toolkit"),
			TEXT("Menu"), TEXT("Selection"), TEXT("ProcessEvent"), TEXT("PostEditChangeProperty")})
		{
			Test.TestFalse(*FString::Printf(TEXT("quarantine forbids private/UI path %s"), Forbidden),
				Implementation->Text.Contains(Forbidden));
		}
	}
	Test.TestEqual(TEXT("ADD and replace-class each own one StageKeyType caller plus its definition"),
		CountAcrossStructuralPublicationSources(Sources, TEXT("::StageKeyType(")), 3);
	Test.TestEqual(TEXT("publisher has only its definition, immutable v1, ADD, replace-class, REMOVE and REORDER callers"),
		CountAcrossStructuralPublicationSources(Sources, TEXT("::PublishLocalKeysSnapshot(")), 6);
	Test.TestEqual(TEXT("immutable v1 caller forwards one prepared snapshot"),
		CountOccurrences(LocalKeys->Text, TEXT("::PublishLocalKeysSnapshot(")), 1);
	RequireOrderedStructuralTokens(Test, LocalKeys->Text, {
		TEXT("FStructuralPublicationTransactionBinding"),
		TEXT("if (!EnlistLocalKeyWrite("),
		TEXT("if (!WriteExistingKeyProperties("),
		TEXT("if (!ReplaceLocalKeysThroughPropertyLifecycle(")});
	Test.TestTrue(TEXT("v1 capture and footprint use the same exact transaction binding"),
		CountOccurrences(LocalKeys->Text, TEXT("TransactionBinding")) >= 6
			&& LocalKeys->Text.Contains(TEXT("Lease, Blackboard, {ComponentId}, TransactionBinding"))
			&& LocalKeys->Text.Contains(TEXT("TransactionBinding.EnlistFootprint")));
	if (Add != nullptr)
	{
		Test.TestEqual(TEXT("ADD owns one production staging call"),
			CountOccurrences(Add->Text, TEXT("::StageKeyType(")), 1);
		Test.TestEqual(TEXT("ADD owns one production publication call"),
			CountOccurrences(Add->Text, TEXT("::PublishLocalKeysSnapshot(")), 1);
		const int32 AddStart = Add->Text.Find(
			TEXT("ApplyAssetDocumentAIBlackboardEntryAddWithIdentity("),
			ESearchCase::CaseSensitive);
		Test.TestTrue(TEXT("ADD row body is physically present"), AddStart != INDEX_NONE);
		if (AddStart != INDEX_NONE)
		{
			const FString AddPacket = Add->Text.Mid(AddStart);
			RequireOrderedStructuralTokens(Test, AddPacket, {
				TEXT("CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage("),
				TEXT("TransactionBinding.EnlistFootprint("),
				TEXT("::StageKeyType("),
				TEXT("BuildCompletePropertyWrites("),
				TEXT("Desired.Insert("),
				TEXT("::PublishLocalKeysSnapshot("),
				TEXT("UpdateIfHasSynchronizedKeys("),
				TEXT("PropagateKeyChangesToDerivedBlackboardAssets("),
				TEXT("BuildAssetDocumentAIBlackboardEntryMetadataReadback(")});
		}
	}
	if (ReplaceClass != nullptr)
	{
		Test.TestEqual(TEXT("replace-class owns one production staging call"),
			CountOccurrences(ReplaceClass->Text, TEXT("::StageKeyType(")), 1);
		Test.TestEqual(TEXT("replace-class owns one production publication call"),
			CountOccurrences(ReplaceClass->Text, TEXT("::PublishLocalKeysSnapshot(")), 1);
		const int32 ReplaceStart = ReplaceClass->Text.Find(
			TEXT("ApplyAssetDocumentAIBlackboardEntryReplaceClassWithIdentity("), ESearchCase::CaseSensitive);
		Test.TestTrue(TEXT("replace-class row body is physically present"), ReplaceStart != INDEX_NONE);
		if (ReplaceStart != INDEX_NONE)
		{
			const FString ReplacePacket = ReplaceClass->Text.Mid(ReplaceStart);
			RequireOrderedStructuralTokens(Test, ReplacePacket, {
				TEXT("CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage("),
				TEXT("TransactionBinding.EnlistFootprint("),
				TEXT("::StageKeyType("),
				TEXT("BuildCompletePropertyWrites("),
				TEXT("Desired[TargetIndex]"),
				TEXT("::PublishLocalKeysSnapshot("),
				TEXT("UpdateIfHasSynchronizedKeys("),
				TEXT("PropagateKeyChangesToDerivedBlackboardAssets("),
				TEXT("BuildAssetDocumentAIBlackboardEntryMetadataReadback(")});
		}
	}
	if (Remove != nullptr)
	{
		Test.TestEqual(TEXT("SC-REMOVE owns one production publication call"),
			CountOccurrences(Remove->Text, TEXT("::PublishLocalKeysSnapshot(")), 1);
		Test.TestEqual(TEXT("SC-REMOVE never stages or materializes a KeyType"),
			CountOccurrences(Remove->Text, TEXT("::StageKeyType(")), 0);
	}
	using namespace AssetDocumentAIBlackboardStructuralPublication;
	Test.TestEqual(TEXT("structural PublicHeader prefix is indivisible and exact"),
		FString(PublicHeaderPrefix), FString(TEXT(
			"AuthorityException/AssetDocumentAIBlackboardStructuralPublication.h+"
			"BehaviorTree/BlackboardData.h+PropertyHandle.h")));
	Test.TestEqual(TEXT("staging ExportedAuthoritativeApi is full and exact"),
		FString(StagingExportedAuthoritativeApi), FString(TEXT(
			"AssetDocumentAIBlackboardStructuralPublication::StageKeyType")));
	Test.TestEqual(TEXT("staging AuthorityFingerprint suffix is full and exact"),
		FString(StagingAuthorityFingerprintSuffix), FString(TEXT(
			"|AD.AI.BLACKBOARD.KEYTYPE-STAGING-v1|"
			"AssetDocumentAIBlackboardStructuralPublication::StageKeyType")));
	Test.TestEqual(TEXT("publication lifecycle packet is indivisible and exact"),
		FString(PublicationLifecyclePacket), FString(TEXT(
			"PublishLocalKeysSnapshot>IPropertyHandle::NotifyPreChange>"
			"UBlackboardData::Keys assignment>"
			"IPropertyHandle::NotifyPostChange(EPropertyChangeType::ValueSet)>"
			"IPropertyHandle::NotifyFinishedChangingProperties>"
			"UBlackboardData::UpdateParentKeys()>UBlackboardData::UpdateKeyIDs()")));
	Test.TestEqual(TEXT("publication ExportedAuthoritativeApi is full and exact"),
		FString(PublicationExportedAuthoritativeApi), FString(TEXT(
			"AssetDocumentAIBlackboardStructuralPublication::"
			"PublishLocalKeysSnapshot>IPropertyHandle::NotifyPreChange>"
			"UBlackboardData::Keys assignment>"
			"IPropertyHandle::NotifyPostChange(EPropertyChangeType::ValueSet)>"
			"IPropertyHandle::NotifyFinishedChangingProperties>"
			"UBlackboardData::UpdateParentKeys()>UBlackboardData::UpdateKeyIDs()")));
	Test.TestEqual(TEXT("publication AuthorityFingerprint suffix is full and exact"),
		FString(PublicationAuthorityFingerprintSuffix), FString(TEXT(
			"|AD.AI.BLACKBOARD.LOCAL-KEYS-PUBLICATION-v1|"
			"AssetDocumentAIBlackboardStructuralPublication::"
			"PublishLocalKeysSnapshot>IPropertyHandle::NotifyPreChange>"
			"UBlackboardData::Keys assignment>"
			"IPropertyHandle::NotifyPostChange(EPropertyChangeType::ValueSet)>"
			"IPropertyHandle::NotifyFinishedChangingProperties>"
			"UBlackboardData::UpdateParentKeys()>UBlackboardData::UpdateKeyIDs()")));
	Test.TestEqual(TEXT("production quarantine never marks staged objects as garbage"),
		CountAcrossStructuralPublicationSources(Sources, TEXT("MarkAsGarbage")), 0);
	Test.TestFalse(TEXT("quarantine header is not exported from Public"),
		IFileManager::Get().FileExists(*FPaths::Combine(GetAssetDocumentPluginBaseDir(), TEXT("Source/AssetDocumentAI/Public"),
			TEXT("AssetDocumentAIBlackboardStructuralPublication.h"))));
}

using FFactoryCreateNewSignature = UObject* (UBehaviorTreeFactory::*)(
	UClass*, UObject*, FName, EObjectFlags, UObject*, FFeedbackContext*);
using FCreateEditorSignature = TSharedRef<IBehaviorTreeEditor> (FBehaviorTreeEditorModule::*)(
	EToolkitMode::Type, const TSharedPtr<IToolkitHost>&, UObject*);
using FResolveSelectorSignature = void (FBlackboardKeySelector::*)(const UBlackboardData&);
using FConstObjectReferenceGetSignature = FPropertyAccess::Result (
	IPropertyHandle::*)(const UObject*&) const;
using FConstObjectReferenceSetSignature = FPropertyAccess::Result (
	IPropertyHandle::*)(const UObject* const&, EPropertyValueSetFlags::Type);

static_assert(std::is_same_v<
	decltype(&UBehaviorTreeFactory::FactoryCreateNew), FFactoryCreateNewSignature>);
static_assert(std::is_same_v<
	decltype(&FBehaviorTreeEditorModule::CreateBehaviorTreeEditor), FCreateEditorSignature>);
static_assert(std::is_same_v<
	decltype(&FBlackboardKeySelector::ResolveSelectedKey), FResolveSelectorSignature>);
static_assert(std::is_same_v<
	decltype(static_cast<FConstObjectReferenceGetSignature>(&IPropertyHandle::GetValue)),
	FConstObjectReferenceGetSignature>);
static_assert(std::is_same_v<
	decltype(static_cast<FConstObjectReferenceSetSignature>(&IPropertyHandle::SetValue)),
	FConstObjectReferenceSetSignature>);

void RequireSharedPublicAuthority(FAutomationTestBase& Test)
{
	auto* WithIdentity = &WithAssetDocumentEditorAssetIdentity;
	auto* CaptureKeyType = &CaptureAndEnlistAssetDocumentAIBlackboardKeyTypePropertiesPreimage;
	auto* CaptureLocalKeys = &CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage;
	auto* CaptureObjectReference =
		&CaptureAndEnlistAssetDocumentEditorObjectReferencePreimage;
	auto* CaptureSelector = &CaptureAndEnlistAssetDocumentAIBehaviorTreeSelectorPreimage;
	auto* RebuildMirror = &RebuildAssetDocumentBehaviorTreeRuntimeMirror;
	Test.TestTrue(TEXT("public Editor asset identity API is linked"), WithIdentity != nullptr);
	Test.TestTrue(TEXT("public key-type typed preimage API is linked"), CaptureKeyType != nullptr);
	Test.TestTrue(TEXT("public local-keys typed preimage API is linked"), CaptureLocalKeys != nullptr);
	Test.TestTrue(TEXT("public object-reference typed preimage API is linked"),
		CaptureObjectReference != nullptr);
	Test.TestTrue(TEXT("public selector typed preimage API is linked"), CaptureSelector != nullptr);
	Test.TestTrue(TEXT("public BehaviorTree runtime-mirror API is linked"), RebuildMirror != nullptr);
}

void RequireParentSetV2Authority(FAutomationTestBase& Test)
{
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentAIPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* ParentSet = nullptr;
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Provider->DescribePrimitives())
	{
		if (Descriptor.PrimitiveId == TEXT("ai.blackboard.parent.set.v2")
			&& Descriptor.Version == 2)
		{
			ParentSet = &Descriptor;
			break;
		}
	}
	if (!Test.TestNotNull(TEXT("Parent set v2 descriptor publishes exact public authority"),
		ParentSet)) return;
	const FString ExactApi = TEXT("WithAssetDocumentEditorAssetIdentity>exact /Script/AIModule.BlackboardData target/desired-Parent class validation>UBlackboardData::IsChildOf(const UBlackboardData&) const>FPropertyEditorModule::CreatePropertyRowGenerator>IPropertyRowGenerator::SetPropertyGenerationAllowListPaths>IPropertyRowGenerator::SetObjects({&ExactBlackboard})>IDetailTreeNode::CreatePropertyHandle>exact UBlackboardData::Parent IPropertyHandle>IPropertyHandle::GetValue(const UObject*&)>CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage>CaptureAndEnlistAssetDocumentEditorObjectReferencePreimage{public capture signature; rollback IPropertyHandle::SetValue(const UObject* const&, EPropertyValueSetFlags::NotTransactable)>IPropertyHandle::GetValue(const UObject*&)}>EnlistFootprint(UObjectState[LocalKeysDerivedClosure,ParentReference])>IPropertyHandle::SetValue(const UObject* const&, EPropertyValueSetFlags::NotTransactable)>UBlackboardData::PropagateKeyChangesToDerivedBlackboardAssets()>IPropertyHandle::GetValue(const UObject*&)");
	const FString ExactPrefix = TEXT("ue-5.7.4-launcher-cl-51494982|ai|ai.blackboard.parent.set.v2|AssetDocumentAI+AssetDocumentEditor+AIModule+PropertyEditor|");
	Test.TestEqual(TEXT("Parent set v2 public module is exact"),
		ParentSet->Admission.PublicModule,
		FString(TEXT("AssetDocumentAI+AssetDocumentEditor+AIModule+PropertyEditor")));
	Test.TestEqual(TEXT("Parent set v2 public header chain is exact"),
		ParentSet->Admission.PublicHeader,
		FString(TEXT("TypedPreimage/AssetDocumentAIBlackboardTypedPreimage.h+AssetDocumentEditorNativeState.h+BehaviorTree/BlackboardData.h+PropertyEditorModule.h+IPropertyRowGenerator.h+IDetailTreeNode.h+PropertyHandle.h")));
	Test.TestEqual(TEXT("Parent set v2 exported API chain is exact"),
		ParentSet->Admission.ExportedAuthoritativeApi, ExactApi);
	Test.TestEqual(TEXT("Parent set v2 authority fingerprint is complete and exact"),
		ParentSet->Admission.AuthorityFingerprint, ExactPrefix + ExactApi);
	for (const TCHAR* Forbidden : {TEXT("->Parent"), TEXT("Parent->"),
		TEXT("NotifyPreChange"), TEXT("NotifyPostChange"),
		TEXT("NotifyFinishedChangingProperties"), TEXT("PostEditChangeProperty"),
		TEXT("UpdateParentKeys"), TEXT("UpdateKeyIDs"), TEXT("ProcessEvent"),
		TEXT("reflection"), TEXT("fallback")})
	{
		Test.TestFalse(*FString::Printf(TEXT("Parent set v2 fingerprint forbids %s"), Forbidden),
			ParentSet->Admission.AuthorityFingerprint.Contains(Forbidden));
	}
}

void RequireUpdateExistingAuthority(FAutomationTestBase& Test)
{
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentAIPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* Update = nullptr;
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Provider->DescribePrimitives())
	{
		if (Descriptor.PrimitiveId == TEXT("ai.blackboard.entry.update_existing.v2")
			&& Descriptor.Version == 2)
		{
			Update = &Descriptor;
			break;
		}
	}
	if (!Test.TestNotNull(TEXT("UPDATE descriptor publishes the exact public authority"), Update)) return;
	const FString ExactApi = TEXT("FPropertyEditorModule::CreatePropertyRowGenerator>IPropertyRowGenerator::SetPropertyGenerationAllowListPaths>IPropertyRowGenerator::SetObjects({&ExactKeyType})>exact admitted KeyType leaf IPropertyHandles>FPropertyEditorModule::CreatePropertyRowGenerator>IPropertyRowGenerator::SetPropertyGenerationAllowListPaths>IPropertyRowGenerator::SetObjects({&ExactBlackboard})>outer Keys IPropertyHandle>{exact existing entry child handle|local-generator-miss-only UBlackboardData::GetKeyID(const FName&)>UBlackboardData::GetKey(FBlackboard::FKey)>read-only parent-aware KeyInherited-or-KeyMissing classification}>CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage>CaptureAndEnlistAssetDocumentAIBlackboardKeyTypePropertiesPreimage>EnlistFootprint(UObjectState[LocalKeysMetadataAggregate,KeyTypeProperties])>exact admitted KeyType leaf IPropertyHandles>IPropertyHandle::SetValue(..., NotTransactable)>EntryDescription leaf IPropertyHandle>IPropertyHandle::SetValue(..., NotTransactable)>EntryCategory leaf IPropertyHandle>IPropertyHandle::SetValue(..., NotTransactable)>bInstanceSynced leaf IPropertyHandle>IPropertyHandle::SetValue(..., NotTransactable)");
	const FString ExactPrefix = TEXT("ue-5.7.4-launcher-cl-51494982|ai|ai.blackboard.entry.update_existing.v2|AssetDocumentAI+AIModule+PropertyEditor|");
	const FString ExactStructWrite = TEXT("|AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::PrepareCanonicalWrite(UpdateExistingV2)>CaptureAndEnlistAssetDocumentAIBlackboardKeyTypePropertiesPreimage>EnlistFootprint(UObjectState[LocalKeysMetadataAggregate,KeyTypeProperties])>AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::ApplyPreparedCanonicalWrite(UpdateExistingV2)>full canonical entry readback");
	Test.TestEqual(TEXT("UPDATE authority public module is exact"), Update->Admission.PublicModule,
		FString(TEXT("AssetDocumentAI+AIModule+PropertyEditor")));
	Test.TestEqual(TEXT("UPDATE authority public header chain is exact"), Update->Admission.PublicHeader,
		FString(TEXT("TypedPreimage/AssetDocumentAIBlackboardTypedPreimage.h+BehaviorTree/BlackboardData.h+PropertyEditorModule.h+IPropertyRowGenerator.h+IDetailTreeNode.h+PropertyHandle.h")));
	Test.TestEqual(TEXT("UPDATE authority exported API chain is exact"),
		Update->Admission.ExportedAuthoritativeApi, ExactApi);
	Test.TestEqual(TEXT("UPDATE authority fingerprint is complete and exact"),
		Update->Admission.AuthorityFingerprint, ExactPrefix + ExactApi + ExactStructWrite);
	for (const TCHAR* Forbidden : {TEXT("Parent->Keys"), TEXT("ParentKeys"), TEXT("GetKeys"),
		TEXT("NewObject"), TEXT("NotifyPreChange"), TEXT("NotifyPostChange"),
		TEXT("UpdateParentKeys"), TEXT("UpdateKeyIDs"), TEXT("reflection"), TEXT("CDO"), TEXT("v1 fallback")})
	{
		Test.TestFalse(*FString::Printf(TEXT("UPDATE authority fingerprint forbids %s"), Forbidden),
			Update->Admission.AuthorityFingerprint.Contains(Forbidden));
	}
}

void RequireParentReadbackAuthority(FAutomationTestBase& Test)
{
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentAIPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* ParentReadback = nullptr;
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Provider->DescribePrimitives())
	{
		if (Descriptor.PrimitiveId == TEXT("ai.blackboard.parent.readback.v2")
			&& Descriptor.Version == 2)
		{
			ParentReadback = &Descriptor;
			break;
		}
	}
	if (!Test.TestNotNull(TEXT("Parent readback descriptor publishes exact public authority"), ParentReadback)) return;
	const FString ExactApi = TEXT("exact /Script/AIModule.BlackboardData target class>FPropertyEditorModule::CreatePropertyRowGenerator>IPropertyRowGenerator::SetPropertyGenerationAllowListPaths>IPropertyRowGenerator::SetObjects({&ExactBlackboard})>IDetailTreeNode::CreatePropertyHandle>exact UBlackboardData::Parent IPropertyHandle>IPropertyHandle::GetValue(const UObject*&)");
	const FString ExactPrefix = TEXT("ue-5.7.4-launcher-cl-51494982|ai|ai.blackboard.parent.readback.v2|AssetDocumentAI+AIModule+PropertyEditor|");
	Test.TestEqual(TEXT("Parent readback public module is exact"), ParentReadback->Admission.PublicModule,
		FString(TEXT("AssetDocumentAI+AIModule+PropertyEditor")));
	Test.TestEqual(TEXT("Parent readback public header chain is exact"), ParentReadback->Admission.PublicHeader,
		FString(TEXT("BehaviorTree/BlackboardData.h+PropertyEditorModule.h+IPropertyRowGenerator.h+IDetailTreeNode.h+PropertyHandle.h")));
	Test.TestEqual(TEXT("Parent readback exported API chain is exact"),
		ParentReadback->Admission.ExportedAuthoritativeApi, ExactApi);
	Test.TestEqual(TEXT("Parent readback authority fingerprint is complete and exact"),
		ParentReadback->Admission.AuthorityFingerprint, ExactPrefix + ExactApi);
	for (const TCHAR* Forbidden : {TEXT("->Parent"), TEXT("ParentKeys"),
		TEXT("GetKeys"), TEXT("SetValue"), TEXT("Notify"), TEXT("UpdateParentKeys"),
		TEXT("reflection"), TEXT("fallback"), TEXT("v1")})
	{
		Test.TestFalse(*FString::Printf(TEXT("Parent readback fingerprint forbids %s"), Forbidden),
			ParentReadback->Admission.AuthorityFingerprint.Contains(Forbidden));
	}
}

void RequireLocalKeysReadbackAuthority(FAutomationTestBase& Test)
{
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentAIPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* LocalKeysReadback = nullptr;
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Provider->DescribePrimitives())
	{
		if (Descriptor.PrimitiveId == TEXT("ai.blackboard.local_keys.readback.v2")
			&& Descriptor.Version == 2)
		{
			LocalKeysReadback = &Descriptor;
			break;
		}
	}
	if (!Test.TestNotNull(TEXT("local-keys readback descriptor publishes exact public authority"), LocalKeysReadback)) return;
	const FString ExactApi = TEXT("exact /Script/AIModule.BlackboardData target class>UBlackboardData::GetKeys()>FBlackboardEntry::EntryName");
	const FString ExactPrefix = TEXT("ue-5.7.4-launcher-cl-51494982|ai|ai.blackboard.local_keys.readback.v2|AssetDocumentAI+AIModule|");
	Test.TestEqual(TEXT("local-keys readback public module is exact"), LocalKeysReadback->Admission.PublicModule,
		FString(TEXT("AssetDocumentAI+AIModule")));
	Test.TestEqual(TEXT("local-keys readback public header chain is exact"), LocalKeysReadback->Admission.PublicHeader,
		FString(TEXT("BehaviorTree/BlackboardData.h")));
	Test.TestEqual(TEXT("local-keys readback exported API chain is exact"),
		LocalKeysReadback->Admission.ExportedAuthoritativeApi, ExactApi);
	Test.TestEqual(TEXT("local-keys readback authority fingerprint is complete and exact"),
		LocalKeysReadback->Admission.AuthorityFingerprint, ExactPrefix + ExactApi);
	for (const TCHAR* Forbidden : {TEXT("->Keys"), TEXT("ParentKeys"), TEXT("GetKeyID"),
		TEXT("GetKey("), TEXT("Sort"), TEXT("PropertyEditor"), TEXT("reflection"), TEXT("fallback"), TEXT("v1")})
	{
		Test.TestFalse(*FString::Printf(TEXT("local-keys authority fingerprint forbids %s"), Forbidden),
			LocalKeysReadback->Admission.AuthorityFingerprint.Contains(Forbidden));
	}
}

#if PLATFORM_WINDOWS
// FPlatformMisc::GetSHA256Signature is unimplemented on Windows and the engine ships no
// other SHA-256 in Core, so the Windows branch of the stock-header fingerprint check
// carries its own compact implementation (FIPS 180-4).
FString Sha256HexLower(TConstArrayView<uint8> Data)
{
	static const uint32 K[64] = {
		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
		0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
		0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
		0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
		0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
		0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
		0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
		0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
	uint32 H[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	TArray<uint8> Padded(Data.GetData(), Data.Num());
	Padded.Add(0x80);
	while (Padded.Num() % 64 != 56) Padded.Add(0);
	const uint64 BitLength = static_cast<uint64>(Data.Num()) * 8;
	for (int32 Shift = 56; Shift >= 0; Shift -= 8) Padded.Add(static_cast<uint8>(BitLength >> Shift));
	auto Rotr = [](uint32 V, uint32 N) { return (V >> N) | (V << (32 - N)); };
	for (int32 Offset = 0; Offset < Padded.Num(); Offset += 64)
	{
		uint32 W[64];
		for (int32 I = 0; I < 16; ++I)
		{
			W[I] = (uint32(Padded[Offset + 4 * I]) << 24) | (uint32(Padded[Offset + 4 * I + 1]) << 16)
				| (uint32(Padded[Offset + 4 * I + 2]) << 8) | uint32(Padded[Offset + 4 * I + 3]);
		}
		for (int32 I = 16; I < 64; ++I)
		{
			const uint32 S0 = Rotr(W[I - 15], 7) ^ Rotr(W[I - 15], 18) ^ (W[I - 15] >> 3);
			const uint32 S1 = Rotr(W[I - 2], 17) ^ Rotr(W[I - 2], 19) ^ (W[I - 2] >> 10);
			W[I] = W[I - 16] + S0 + W[I - 7] + S1;
		}
		uint32 A = H[0], B = H[1], C = H[2], D = H[3], E = H[4], F = H[5], G = H[6], HH = H[7];
		for (int32 I = 0; I < 64; ++I)
		{
			const uint32 S1 = Rotr(E, 6) ^ Rotr(E, 11) ^ Rotr(E, 25);
			const uint32 Ch = (E & F) ^ (~E & G);
			const uint32 T1 = HH + S1 + Ch + K[I] + W[I];
			const uint32 S0 = Rotr(A, 2) ^ Rotr(A, 13) ^ Rotr(A, 22);
			const uint32 Maj = (A & B) ^ (A & C) ^ (B & C);
			const uint32 T2 = S0 + Maj;
			HH = G; G = F; F = E; E = D + T1; D = C; C = B; B = A; A = T1 + T2;
		}
		H[0] += A; H[1] += B; H[2] += C; H[3] += D; H[4] += E; H[5] += F; H[6] += G; H[7] += HH;
	}
	FString Hex;
	for (uint32 Word : H) Hex += FString::Printf(TEXT("%08x"), Word);
	return Hex;
}
#endif

void RequireStructCanonicalReadAuthority(FAutomationTestBase& Test)
{
	using namespace AssetDocumentAIBlackboardKeyTypeStructCanonicalRead;
	Test.TestEqual(TEXT("Struct canonical READ governance ID is exact"), FString(GovernanceId),
		FString(TEXT("AD.AI.BLACKBOARD.KEYTYPE-STRUCT-CANONICAL-READ-v1")));
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider = CreateAssetDocumentAIPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* Readback = nullptr;
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Provider->DescribePrimitives())
	{
		if (Descriptor.PrimitiveId == TEXT("ai.blackboard.entry.readback.v2") && Descriptor.Version == 2)
		{
			Readback = &Descriptor; break;
		}
	}
	if (!Test.TestNotNull(TEXT("readback descriptor binds canonical Struct READ"), Readback)) return;
	Test.TestTrue(TEXT("readback fingerprint binds the private READ ID and exact live source"),
		Readback->Admission.AuthorityFingerprint.EndsWith(TEXT(
			"|AD.AI.BLACKBOARD.KEYTYPE-STRUCT-CANONICAL-READ-v1|"
			"AssetDocumentAIBlackboardKeyTypeStructCanonicalRead::ReadCanonicalValue(ExactLiveDefaultValue)")));

	TArray<FStructuralPublicationSource> Sources;
	if (!LoadStructuralPublicationSources(Test, Sources)) return;
	const auto* Header = FindStructuralPublicationSource(Sources, TEXT(
		"Private/Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardKeyTypeStructCanonicalRead.h"));
	const auto* Implementation = FindStructuralPublicationSource(Sources, TEXT(
		"Private/Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardKeyTypeStructCanonicalRead.cpp"));
	const auto* SchemaImplementation = FindStructuralPublicationSource(Sources, TEXT(
		"Private/Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardKeyTypeStructCanonicalReadSchema.cpp"));
	const auto* Inspection = FindStructuralPublicationSource(Sources, TEXT(
		"Private/Provider/Blackboard/AssetDocumentAIBlackboardInspection.cpp"));
	const auto* WriteImplementation = FindStructuralPublicationSource(Sources, TEXT(
		"Private/Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite.cpp"));
	const auto* WriteDecode = FindStructuralPublicationSource(Sources, TEXT(
		"Private/Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardKeyTypeStructCanonicalWriteDecode.cpp"));
	if (!Test.TestNotNull(TEXT("Struct canonical READ private header exists"), Header)
		|| !Test.TestNotNull(TEXT("Struct canonical READ private implementation exists"), Implementation)
		|| !Test.TestNotNull(TEXT("Struct canonical READ private schema implementation exists"), SchemaImplementation)
		|| !Test.TestNotNull(TEXT("Struct canonical READ live caller exists"), Inspection)
		|| !Test.TestNotNull(TEXT("Struct canonical WRITE lifecycle caller exists"), WriteImplementation)
		|| !Test.TestNotNull(TEXT("Struct canonical WRITE decode caller exists"), WriteDecode)) return;
	Test.TestEqual(TEXT("READ header has one private declaration"),
		CountCodeOccurrences(Header->Text, TEXT("ReadCanonicalValue(")), 1);
	Test.TestEqual(TEXT("READ implementation has one private definition"),
		CountCodeOccurrences(Implementation->Text, TEXT("ReadCanonicalValue(")), 1);
	Test.TestEqual(TEXT("READ schema implementation has no entry definition"),
		CountCodeOccurrences(SchemaImplementation->Text, TEXT("ReadCanonicalValue(")), 0);
	Test.TestEqual(TEXT("Inspection has exactly one live direct READ call"),
		CountCodeOccurrences(Inspection->Text, TEXT("ReadCanonicalValue(")), 1);
	Test.TestEqual(TEXT("WRITE lifecycle has exact scratch and live READ calls"),
		CountCodeOccurrences(WriteImplementation->Text, TEXT("ReadCanonicalValue(")), 2);
	Test.TestEqual(TEXT("WRITE decode has exactly one detached scratch READ call"),
		CountCodeOccurrences(WriteDecode->Text, TEXT("ReadCanonicalValue(")), 1);
	Test.TestEqual(TEXT("WRITE lifecycle scratch READ source is exact"),
		CountCodeOccurrences(WriteImplementation->Text, TEXT(
			"ReadCanonicalValue(Scratch, CanonicalRead::ESource::DetachedScratch, nullptr)")), 1);
	Test.TestEqual(TEXT("WRITE lifecycle live READ source is exact"),
		CountCodeOccurrences(WriteImplementation->Text, TEXT(
			"ReadCanonicalValue(Target.DefaultValue, CanonicalRead::ESource::ExactLiveDefaultValue, &Target)")), 1);
	Test.TestEqual(TEXT("WRITE decode scratch READ source is exact"),
		CountCodeOccurrences(WriteDecode->Text, TEXT(
			"ReadCanonicalValue(State.Scratch, CanonicalRead::ESource::DetachedScratch, nullptr)")), 1);
	for (const FStructuralPublicationSource& Source : Sources)
	{
		int32 ExpectedCalls = 0;
		if (&Source == Header || &Source == Implementation || &Source == Inspection) ExpectedCalls = 1;
		else if (&Source == WriteImplementation) ExpectedCalls = 2;
		else if (&Source == WriteDecode) ExpectedCalls = 1;
		Test.TestEqual(*FString::Printf(TEXT("READ entry callgraph is closed in %s"), *Source.RelativePath),
			CountCodeOccurrences(Source.Text, TEXT("ReadCanonicalValue(")), ExpectedCalls);
	}
	for (const FStructuralPublicationSource* OwnerSource : {Implementation, SchemaImplementation})
	{
		for (const TCHAR* Forbidden : {TEXT("ResolveObject"), TEXT("TryLoad"), TEXT("LoadObject"),
			TEXT("ExportText"), TEXT("ImportText"), TEXT("FJsonObjectConverter"),
			TEXT("EnumerateRawData"), TEXT("PropertyEditor"), TEXT("LoadedConsumerLocatorCodec"),
			TEXT("FText::ToString"), TEXT("FTextInspector::GetDisplayString"), TEXT("IdenticalTo"),
			TEXT("Rebuild"), TEXT("ITextGenerator")})
		{
			Test.TestFalse(*FString::Printf(TEXT("Struct canonical READ owner forbids %s"), Forbidden),
				OwnerSource->Text.Contains(Forbidden));
		}
	}
	Test.TestTrue(TEXT("FText reads only the public non-generating source observation"),
		Implementation->Text.Contains(TEXT("FTextInspector::GetSourceString")));
	Test.TestEqual(TEXT("Null leaves reserve their node before constructing the DTO"),
		CountCodeOccurrences(Implementation->Text, TEXT(
			"if (!ConsumeNode(Context)) return false; OutValue = Scalar(EAssetDocumentRuntimeValueKind::Null); return true;")),
		2);
	Test.TestEqual(TEXT("Null leaves never construct the DTO before node accounting"),
		CountCodeOccurrences(Implementation->Text, TEXT(
			"OutValue = Scalar(EAssetDocumentRuntimeValueKind::Null); return ConsumeNode(Context);")),
		0);

	// The stock engine headers the canonical READ owner was reviewed against, per engine version.
	struct FEngineFingerprint { const TCHAR* RelativePath; const TCHAR* Sha256; };
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	const TCHAR* const StockEngine = TEXT("UE 5.8.1");
	const FEngineFingerprint Fingerprints[] = {
		{TEXT("Source/Runtime/AIModule/Classes/BehaviorTree/Blackboard/BlackboardKeyType_Struct.h"),
			TEXT("3f881deaa9f2c44e1fc41c03aca7233c92bd4c16cc478042bde84530786492c9")},
		{TEXT("Source/Runtime/CoreUObject/Public/StructUtils/InstancedStruct.h"),
			TEXT("607fd3776bf3161b7751d66a43dd56c01f9289c0a9846ff71e16d6f2efbff16c")},
		{TEXT("Source/Runtime/CoreUObject/Public/UObject/UnrealType.h"),
			TEXT("bb9705086a01c5ab0f620c1541651f3f2a1314a0e4c6646d53fbd70c5db609b5")},
		{TEXT("Source/Runtime/CoreUObject/Public/UObject/EnumProperty.h"),
			TEXT("5cee2ce9e91832741153764d86953fc0ae1b6aa2a24abbc657ac4d9bf7700e64")},
		{TEXT("Source/Runtime/Core/Public/Internationalization/Text.h"),
			TEXT("e7c0461001adc9cd3f527c50ef0fc43a099e2606f2d4c147ca464b7894f17dfe")},
		{TEXT("Source/Runtime/CoreUObject/Public/UObject/SoftObjectPath.h"),
			TEXT("05bf8b1ba2f3b47a621d72110501f203cffd022865f6a9fa8e6dbfa7cd3d9596")},
	};
#else
	const TCHAR* const StockEngine = TEXT("UE 5.7.4");
	const FEngineFingerprint Fingerprints[] = {
		{TEXT("Source/Runtime/AIModule/Classes/BehaviorTree/Blackboard/BlackboardKeyType_Struct.h"),
			TEXT("3f881deaa9f2c44e1fc41c03aca7233c92bd4c16cc478042bde84530786492c9")},
		{TEXT("Source/Runtime/CoreUObject/Public/StructUtils/InstancedStruct.h"),
			TEXT("63ccc338f9b92f79f05871867e6790f200782bf3e22151bc7e30a63ba010c870")},
		{TEXT("Source/Runtime/CoreUObject/Public/UObject/UnrealType.h"),
			TEXT("67a1b4f754987405f5cbeaaab0ebd357cd01c32ff17d26879ace43614e6ccc09")},
		{TEXT("Source/Runtime/CoreUObject/Public/UObject/EnumProperty.h"),
			TEXT("807ef753e5ebd24a872021788ff0cc3a58535826f3996423c999e5e7d4a8785b")},
		{TEXT("Source/Runtime/Core/Public/Internationalization/Text.h"),
			TEXT("32a90c4485c9c05d7d6e16e66f304e72efbd60a6cd0c3985995a68f374daceb9")},
		{TEXT("Source/Runtime/CoreUObject/Public/UObject/SoftObjectPath.h"),
			TEXT("89e04d9512794a36f4666ecac7d3462e0dfcb3f4f2194b1586a6adf03d58c06a")},
	};
#endif
	for (const FEngineFingerprint& Fingerprint : Fingerprints)
	{
		const FString Path = FPaths::Combine(FPaths::EngineDir(), Fingerprint.RelativePath);
		FString ActualSha256;
		bool bHashed = false;
#if PLATFORM_WINDOWS
		// No shasum on Windows, and a Windows engine install ships CRLF headers: hash the
		// LF-normalized bytes, which equal the fingerprint of the stock (LF) files.
		TArray<uint8> Raw;
		if (FFileHelper::LoadFileToArray(Raw, *Path))
		{
			TArray<uint8> Normalized;
			Normalized.Reserve(Raw.Num());
			for (int32 Index = 0; Index < Raw.Num(); ++Index)
			{
				if (Raw[Index] == '\r' && Index + 1 < Raw.Num() && Raw[Index + 1] == '\n') continue;
				Normalized.Add(Raw[Index]);
			}
			ActualSha256 = Sha256HexLower(Normalized);
			bHashed = ActualSha256.Len() == 64;
		}
#else
		int32 ReturnCode = INDEX_NONE;
		FString StandardOutput;
		FString StandardError;
		const bool bLaunched = FPlatformProcess::ExecProcess(TEXT("/usr/bin/shasum"),
			*FString::Printf(TEXT("-a 256 \"%s\""), *Path), &ReturnCode, &StandardOutput, &StandardError);
		StandardOutput.TrimStartAndEndInline();
		StandardError.TrimStartAndEndInline();
		ActualSha256 = StandardOutput.Left(64);
		bHashed = bLaunched && ReturnCode == 0 && StandardError.IsEmpty()
			&& ActualSha256.Len() == 64;
#endif
		Test.TestTrue(*FString::Printf(TEXT("stock %s fingerprint readable: %s"),
			StockEngine, Fingerprint.RelativePath), bHashed);
		if (bHashed) Test.TestEqual(*FString::Printf(TEXT("stock %s fingerprint exact: %s"),
			StockEngine, Fingerprint.RelativePath), ActualSha256, FString(Fingerprint.Sha256));
	}
}

void RequireEntryAddAuthority(FAutomationTestBase& Test)
{
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentAIPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* EntryAdd = nullptr;
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Provider->DescribePrimitives())
	{
		if (Descriptor.PrimitiveId == TEXT("ai.blackboard.entry.add.v2")
			&& Descriptor.Version == 2)
		{
			EntryAdd = &Descriptor;
			break;
		}
	}
	if (!Test.TestNotNull(TEXT("entry add descriptor publishes exact bridge authority"), EntryAdd)) return;
	const FString ExactApi = TEXT("AssetDocumentAIBlackboardStructuralPublication::StageKeyType");
	const FString ExactPrefix = TEXT(
		"ue-5.7.4-launcher-cl-51494982|ai|ai.blackboard.entry.add.v2|"
		"AD.AI.BLACKBOARD.KEYTYPE-STAGING-v1+AD.AI.BLACKBOARD.LOCAL-KEYS-PUBLICATION-v1|");
	const FString ExactChain = TEXT(
		"WithAssetDocumentEditorAssetIdentity>exact absence/inherited/shadow/index/full-entry preflight>"
		"AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::PrepareCanonicalWrite(AddV2)>"
		"CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage>"
		"EnlistFootprint(UObjectState[LocalKeysAggregate])>"
		"AssetDocumentAIBlackboardStructuralPublication::StageKeyType>"
		"AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::ApplyPreparedCanonicalWrite(AddV2)>"
		"typed complete KeyType configure+canonical validation>"
		"AssetDocumentAIBlackboardStructuralPublication::PublishLocalKeysSnapshot>"
		"IPropertyHandle::NotifyPreChange>UBlackboardData::Keys assignment>"
		"IPropertyHandle::NotifyPostChange(EPropertyChangeType::ValueSet)>"
		"IPropertyHandle::NotifyFinishedChangingProperties>"
		"UBlackboardData::UpdateParentKeys()>UBlackboardData::UpdateKeyIDs()>"
		"UBlackboardData::UpdateIfHasSynchronizedKeys()>"
		"UBlackboardData::PropagateKeyChangesToDerivedBlackboardAssets()>"
		"full canonical entry/order readback");
	Test.TestEqual(TEXT("entry add public module chain is exact"), EntryAdd->Admission.PublicModule,
		FString(TEXT("AssetDocumentAI+AIModule+PropertyEditor")));
	Test.TestEqual(TEXT("entry add private-public header chain is exact"), EntryAdd->Admission.PublicHeader,
		FString(TEXT("AuthorityException/AssetDocumentAIBlackboardStructuralPublication.h+BehaviorTree/BlackboardData.h+PropertyHandle.h")));
	Test.TestEqual(TEXT("entry add exported authority is the staging bridge"),
		EntryAdd->Admission.ExportedAuthoritativeApi, ExactApi);
	Test.TestEqual(TEXT("entry add authority fingerprint is complete and exact"),
		EntryAdd->Admission.AuthorityFingerprint, ExactPrefix + ExactChain);
	for (const TCHAR* Forbidden : {TEXT("generic structural"), TEXT("v1 fallback"),
		TEXT("manual cache"), TEXT("Python")})
	{
		Test.TestFalse(*FString::Printf(TEXT("entry add authority forbids %s"), Forbidden),
			EntryAdd->Admission.AuthorityFingerprint.Contains(Forbidden));
	}
}

void RequireEntryReplaceClassAuthority(FAutomationTestBase& Test)
{
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentAIPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* ReplaceClass = nullptr;
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Provider->DescribePrimitives())
	{
		if (Descriptor.PrimitiveId == TEXT("ai.blackboard.entry.replace_class.v2")
			&& Descriptor.Version == 2)
		{
			ReplaceClass = &Descriptor;
			break;
		}
	}
	if (!Test.TestNotNull(TEXT("replace-class descriptor publishes exact bridge authority"),
		ReplaceClass)) return;
	const FString ExactApi = TEXT("AssetDocumentAIBlackboardStructuralPublication::StageKeyType");
	const FString ExactPrefix = TEXT(
		"ue-5.7.4-launcher-cl-51494982|ai|ai.blackboard.entry.replace_class.v2|"
		"AD.AI.BLACKBOARD.KEYTYPE-STAGING-v1+AD.AI.BLACKBOARD.LOCAL-KEYS-PUBLICATION-v1|");
	const FString ExactChain = TEXT(
		"WithAssetDocumentEditorAssetIdentity>exact owned-local/same-name/distinct-class/full-entry preflight>"
		"AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::PrepareCanonicalWrite(ReplaceClassV2)>"
		"CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage>"
		"EnlistFootprint(UObjectState[LocalKeysAggregate])>"
		"AssetDocumentAIBlackboardStructuralPublication::StageKeyType>"
		"AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::ApplyPreparedCanonicalWrite(ReplaceClassV2)>"
		"typed complete KeyType configure+canonical validation>"
		"replace exact local snapshot slot>"
		"AssetDocumentAIBlackboardStructuralPublication::PublishLocalKeysSnapshot>"
		"IPropertyHandle::NotifyPreChange>UBlackboardData::Keys assignment>"
		"IPropertyHandle::NotifyPostChange(EPropertyChangeType::ValueSet)>"
		"IPropertyHandle::NotifyFinishedChangingProperties>"
		"UBlackboardData::UpdateParentKeys()>UBlackboardData::UpdateKeyIDs()>"
		"UBlackboardData::UpdateIfHasSynchronizedKeys()>"
		"UBlackboardData::PropagateKeyChangesToDerivedBlackboardAssets()>"
		"full canonical entry/order readback");
	Test.TestEqual(TEXT("replace-class public module chain is exact"),
		ReplaceClass->Admission.PublicModule,
		FString(TEXT("AssetDocumentAI+AIModule+PropertyEditor")));
	Test.TestEqual(TEXT("replace-class private-public header chain is exact"),
		ReplaceClass->Admission.PublicHeader,
		FString(TEXT("AuthorityException/AssetDocumentAIBlackboardStructuralPublication.h+BehaviorTree/BlackboardData.h+PropertyHandle.h")));
	Test.TestEqual(TEXT("replace-class exported authority is the staging bridge"),
		ReplaceClass->Admission.ExportedAuthoritativeApi, ExactApi);
	Test.TestEqual(TEXT("replace-class authority fingerprint is complete and exact"),
		ReplaceClass->Admission.AuthorityFingerprint, ExactPrefix + ExactChain);
	for (const TCHAR* Forbidden : {TEXT("generic structural"), TEXT("v1 fallback"),
		TEXT("manual cache"), TEXT("Python")})
	{
		Test.TestFalse(*FString::Printf(TEXT("replace-class authority forbids %s"), Forbidden),
			ReplaceClass->Admission.AuthorityFingerprint.Contains(Forbidden));
	}
}

void RequireEntryRemoveAuthority(FAutomationTestBase& Test)
{
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentAIPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* Remove = nullptr;
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Provider->DescribePrimitives())
	{
		if (Descriptor.PrimitiveId == TEXT("ai.blackboard.entry.remove.v2")
			&& Descriptor.Version == 2)
		{
			Remove = &Descriptor;
			break;
		}
	}
	if (!Test.TestNotNull(TEXT("SC-REMOVE descriptor publishes exact READ-only bridge authority"), Remove)) return;
	const FString ExactApi = TEXT(
		"AssetDocumentAIBlackboardStructuralPublication::PublishLocalKeysSnapshot>"
		"IPropertyHandle::NotifyPreChange>UBlackboardData::Keys assignment>"
		"IPropertyHandle::NotifyPostChange(EPropertyChangeType::ValueSet)>"
		"IPropertyHandle::NotifyFinishedChangingProperties>"
		"UBlackboardData::UpdateParentKeys()>UBlackboardData::UpdateKeyIDs()");
	const FString ExactFingerprint = TEXT(
		"ue-5.7.4-launcher-cl-51494982|ai|ai.blackboard.entry.remove.v2|"
		"AD.AI.BLACKBOARD.LOCAL-KEYS-PUBLICATION-v1|"
		"WithAssetDocumentEditorAssetIdentity>exact owned-local/full-snapshot preflight>"
		"CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage>"
		"EnlistFootprint(UObjectState[LocalKeysAggregate])>")
		+ ExactApi + TEXT(
			">UBlackboardData::UpdateIfHasSynchronizedKeys()>"
			"UBlackboardData::PropagateKeyChangesToDerivedBlackboardAssets()>"
			"complete loaded-consumer completion>absent-name full canonical retained-entry/order readback>"
			"AD.AI.BLACKBOARD.KEYTYPE-STRUCT-CANONICAL-READ-v1");
	Test.TestEqual(TEXT("SC-REMOVE has Apply-only mutating access"), Remove->Access,
		EAssetDocumentPrimitiveAccess::Mutating);
	Test.TestEqual(TEXT("SC-REMOVE has one Apply phase"), Remove->AllowedPhases,
		TArray<EAssetDocumentRuntimeRequestPhase>{EAssetDocumentRuntimeRequestPhase::Apply});
	Test.TestTrue(TEXT("SC-REMOVE requires the active transaction"),
		Remove->bRequiresActiveTransaction);
	Test.TestEqual(TEXT("SC-REMOVE has the frozen identity contract"),
		Remove->Admission.IdentityLifetimeContractId,
		FString(TEXT("ai.blackboard.entry.remove.v2.identity.ha-request-transaction-revision.v2")));
	Test.TestEqual(TEXT("SC-REMOVE has the frozen active-HA transaction contract"),
		Remove->Admission.TransactionContractId,
		FString(TEXT("ai.blackboard.entry.remove.v2.transaction.active-ha.v2")));
	Test.TestEqual(TEXT("SC-REMOVE has its frozen rollback contract"),
		Remove->Admission.RollbackContractId,
		FString(TEXT("assetdocument.ai.blackboard.entry.remove.rollback.v2")));
	Test.TestEqual(TEXT("SC-REMOVE has its frozen full-readback completion contract"),
		Remove->Admission.CompletionContractId, FString(TEXT(
			"ai.blackboard.entry.remove.v2.completion.absent_name_full_readback_and_order.v2")));
	Test.TestEqual(TEXT("SC-REMOVE has one mutable Asset handle argument"),
		Remove->HandleArgumentSchemas.Num(), 1);
	if (Remove->HandleArgumentSchemas.Num() == 1)
	{
		const FAssetDocumentPrimitiveHandleArgumentSchema& Asset = Remove->HandleArgumentSchemas[0];
		Test.TestTrue(TEXT("SC-REMOVE has the exact mutable Asset handle schema"),
			Asset.Name == TEXT("asset") && Asset.Kind == EAssetDocumentHandleKind::Asset
			&& Asset.RequiredAccess == EAssetDocumentHandleAccess::Mutable);
	}
	Test.TestTrue(TEXT("SC-REMOVE returns no handles"), Remove->HandleResultSchemas.IsEmpty());
	Test.TestEqual(TEXT("SC-REMOVE has its exact closed diagnostics"), Remove->StableDiagnosticCodes,
		TArray<FString>{TEXT("AIProvider.InvalidValueShape"), TEXT("AIProvider.InvalidContext"),
			TEXT("AIProvider.InvalidTransaction"), TEXT("AIProvider.InvalidHandle"),
			TEXT("AIProvider.AuthorityUnavailable"), TEXT("AIProvider.OperationRejected"),
			TEXT("AIProvider.Blackboard.KeyMissing"), TEXT("AIProvider.Blackboard.KeyInherited"),
			TEXT("AIProvider.Blackboard.OrderedKeysLifecycleUnavailable"),
			TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			TEXT("AIProvider.Blackboard.EntryChangedSetMismatch")});
	Test.TestEqual(TEXT("SC-REMOVE public module chain is exact"),
		Remove->Admission.PublicModule, FString(TEXT("AssetDocumentAI+AIModule+PropertyEditor")));
	Test.TestEqual(TEXT("SC-REMOVE private-public header chain is exact"),
		Remove->Admission.PublicHeader, FString(TEXT(
			"AuthorityException/AssetDocumentAIBlackboardStructuralPublication.h+"
			"BehaviorTree/BlackboardData.h+PropertyHandle.h")));
	Test.TestEqual(TEXT("SC-REMOVE exported authority is the publication bridge"),
		Remove->Admission.ExportedAuthoritativeApi,
		FString(TEXT("AssetDocumentAIBlackboardStructuralPublication::PublishLocalKeysSnapshot")));
	Test.TestEqual(TEXT("SC-REMOVE authority fingerprint is exact and READ-only"),
		Remove->Admission.AuthorityFingerprint, ExactFingerprint);
	for (const TCHAR* Forbidden : {TEXT("AD.AI.BLACKBOARD.KEYTYPE-STRUCT-CANONICAL-WRITE-v1"),
		TEXT("generic structural"), TEXT("v1 fallback"), TEXT("manual cache"), TEXT("Python")})
	{
		Test.TestFalse(*FString::Printf(TEXT("SC-REMOVE authority forbids %s"), Forbidden),
			Remove->Admission.AuthorityFingerprint.Contains(Forbidden));
	}

	TArray<FStructuralPublicationSource> Sources;
	if (!LoadStructuralPublicationSources(Test, Sources)) return;
	const auto* Contracts = FindStructuralPublicationSource(Sources,
		TEXT("Private/Provider/AssetDocumentAIProviderContracts.h"));
	const auto* Parser = FindStructuralPublicationSource(Sources,
		TEXT("Private/Provider/AssetDocumentAIProviderContractsEntryRemove.cpp"));
	const auto* Primitives = FindStructuralPublicationSource(Sources,
		TEXT("Private/Provider/Blackboard/AssetDocumentAIBlackboardPrimitives.h"));
	const auto* Descriptor = FindStructuralPublicationSource(Sources,
		TEXT("Private/Provider/Blackboard/AssetDocumentAIBlackboardEntryRemoveDescriptor.cpp"));
	const auto* Catalog = FindStructuralPublicationSource(Sources,
		TEXT("Private/Provider/Blackboard/AssetDocumentAIBlackboardCatalog.cpp"));
	const auto* Assembly = FindStructuralPublicationSource(Sources,
		TEXT("Private/Provider/AssetDocumentAIProviderAssembly.cpp"));
	const auto* Handler = FindStructuralPublicationSource(Sources,
		TEXT("Private/Provider/Blackboard/AssetDocumentAIBlackboardEntryRemove.cpp"));
	if (!Test.TestNotNull(TEXT("SC-REMOVE request declaration source exists"), Contracts)
		|| !Test.TestNotNull(TEXT("SC-REMOVE parser source exists"), Parser)
		|| !Test.TestNotNull(TEXT("SC-REMOVE primitive declaration source exists"), Primitives)
		|| !Test.TestNotNull(TEXT("SC-REMOVE descriptor source exists"), Descriptor)
		|| !Test.TestNotNull(TEXT("SC-REMOVE Catalog source exists"), Catalog)
		|| !Test.TestNotNull(TEXT("SC-REMOVE Assembly source exists"), Assembly)
		|| !Test.TestNotNull(TEXT("SC-REMOVE handler source exists"), Handler)) return;
	Test.TestEqual(TEXT("SC-REMOVE parser declaration is unique"),
		CountCodeOccurrences(Contracts->Text,
			TEXT("ParseAssetDocumentAIBlackboardEntryRemoveRequest(")), 1);
	Test.TestEqual(TEXT("SC-REMOVE parser definition is unique"),
		CountCodeOccurrences(Parser->Text,
			TEXT("bool ParseAssetDocumentAIBlackboardEntryRemoveRequest(")), 1);
	Test.TestEqual(TEXT("SC-REMOVE descriptor declaration is unique"),
		CountCodeOccurrences(Primitives->Text,
			TEXT("MakeAssetDocumentAIBlackboardEntryRemoveV2Descriptor(")), 1);
	Test.TestEqual(TEXT("SC-REMOVE descriptor definition is unique"),
		CountCodeOccurrences(Descriptor->Text,
			TEXT("FAssetDocumentPrimitiveDescriptor MakeAssetDocumentAIBlackboardEntryRemoveV2Descriptor(")), 1);
	Test.TestEqual(TEXT("SC-REMOVE Catalog has one factory call"), CountCodeOccurrences(Catalog->Text,
		TEXT("OutDescriptors.Add(MakeAssetDocumentAIBlackboardEntryRemoveV2Descriptor());")), 1);
	Test.TestEqual(TEXT("SC-REMOVE Assembly has one handler binding"), CountCodeOccurrences(Assembly->Text,
		TEXT("&HandleAssetDocumentAIBlackboardEntryRemove")), 1);
	Test.TestEqual(TEXT("SC-REMOVE handler declaration is unique"),
		CountCodeOccurrences(Primitives->Text,
			TEXT("HandleAssetDocumentAIBlackboardEntryRemove(")), 1);
	Test.TestEqual(TEXT("SC-REMOVE handler definition is unique"),
		CountCodeOccurrences(Handler->Text,
			TEXT("FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBlackboardEntryRemove(")), 1);
	Test.TestEqual(TEXT("SC-REMOVE handler has one parser call"), CountCodeOccurrences(Handler->Text,
		TEXT("ParseAssetDocumentAIBlackboardEntryRemoveRequest(")), 1);
	Test.TestEqual(TEXT("SC-REMOVE retained entries use the canonical KeyType reader"),
		CountCodeOccurrences(Handler->Text,
			TEXT("BuildAssetDocumentAIBlackboardCanonicalKeyTypeProperties(")), 1);
	for (const TCHAR* Forbidden : {TEXT("StageKeyType"), TEXT("CanonicalWrite"),
		TEXT("FPreparedWrite"), TEXT("ResolveObject"), TEXT("TryLoad"), TEXT("LoadObject"),
		TEXT("AssetRegistry"), TEXT("TObjectIterator"), TEXT("ForEachObject"), TEXT("GUObjectArray"),
		TEXT("TFieldIterator"), TEXT("Locator"), TEXT("GetChildHandle"), TEXT("CreateSingleProperty"),
		TEXT("IDetailTreeNode"), TEXT("EnumerateRawData"), TEXT("ExportText"),
		TEXT("ImportText"), TEXT("FJsonObjectConverter"), TEXT("NewObject"),
		TEXT("StaticConstructObject"), TEXT("DuplicateObject")})
	{
		Test.TestEqual(*FString::Printf(TEXT("SC-REMOVE source forbids %s"), Forbidden),
			CountCodeOccurrences(Handler->Text, Forbidden), 0);
	}
	FString ApplyPacket;
	if (Test.TestTrue(TEXT("SC-REMOVE handler lifecycle packet exists"),
		ExtractExactFunctionPacket(Handler->Text,
			TEXT("FAssetDocumentPrimitiveOutcome ApplyEntryRemoveWithIdentity("), ApplyPacket)))
	{
		RequireOrderedStructuralTokens(Test, ApplyPacket, {TEXT("PreflightEntryRemove("),
			TEXT("CaptureEntryRemoveState("), TEXT("PublishAndCompleteEntryRemove("),
			TEXT("CompleteEntryRemoveOutcome(")});
	}
	FString CapturePacket;
	if (Test.TestTrue(TEXT("SC-REMOVE capture packet exists"), ExtractExactFunctionPacket(
		Handler->Text, TEXT("TOptional<FAssetDocumentPrimitiveOutcome> CaptureEntryRemoveState("), CapturePacket)))
	{
		RequireOrderedStructuralTokens(Test, CapturePacket, {
			TEXT("CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage("),
			TEXT("Binding.EnlistFootprint(")});
	}
	FString PublishPacket;
	if (Test.TestTrue(TEXT("SC-REMOVE publication packet exists"), ExtractExactFunctionPacket(
		Handler->Text, TEXT("TOptional<FAssetDocumentPrimitiveOutcome> PublishEntryRemove("), PublishPacket)))
	{
		RequireOrderedStructuralTokens(Test, PublishPacket, {TEXT("::PublishLocalKeysSnapshot("),
			TEXT("UpdateIfHasSynchronizedKeys()"), TEXT("PropagateKeyChangesToDerivedBlackboardAssets()")});
	}
	FString CompletePacket;
	if (Test.TestTrue(TEXT("SC-REMOVE completion packet exists"), ExtractExactFunctionPacket(
		Handler->Text, TEXT("TOptional<FAssetDocumentPrimitiveOutcome> CompleteEntryRemoveConsumers("), CompletePacket)))
	{
		RequireOrderedStructuralTokens(Test, CompletePacket, {TEXT("BeforeLoadedConsumerForwardCompletion"),
			TEXT("CompletePublishedLoadedConsumers("), TEXT("AfterLoadedConsumerForwardCompletion"),
			TEXT("AfterLocalKeyIdRefresh")});
	}
	FString ReadbackPacket;
	if (Test.TestTrue(TEXT("SC-REMOVE final readback packet exists"), ExtractExactFunctionPacket(
		Handler->Text, TEXT("TOptional<FAssetDocumentPrimitiveOutcome> ReadFinalEntryRemoveState("), ReadbackPacket)))
	{
		RequireOrderedStructuralTokens(Test, ReadbackPacket, {
			TEXT("ReadCompleteAuthoredEntries("), TEXT("RuntimeValueEquals(")});
	}
}

void RequireLoadedConsumerCompletionSourceCheck(FAutomationTestBase& Test)
{
	TArray<FStructuralPublicationSource> Sources;
	if (!LoadStructuralPublicationSources(Test, Sources)) return;
	const FString CompletionHeaderPath = TEXT(
		"Private/Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardLoadedConsumerCompletion.h");
	const FString CompletionPath = TEXT(
		"Private/Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardLoadedConsumerCompletion.cpp");
	const FString LocatorCodecPath = TEXT(
		"Private/Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardLoadedConsumerLocatorCodec.cpp");
	const FString InventoryPath = TEXT(
		"Private/Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardLoadedConsumerInventory.cpp");
	const FString ValueOrHeaderPath = TEXT(
		"Private/Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardValueOrKeyCacheInvalidation.h");
	const FString ValueOrPath = TEXT(
		"Private/Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardValueOrKeyCacheInvalidation.cpp");
	const FString StructuralPath = TEXT(
		"Private/Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardStructuralPublication.cpp");
	const FString AddPath = TEXT("Private/Provider/Blackboard/AssetDocumentAIBlackboardEntryAdd.cpp");
	const FString ReplacePath = TEXT(
		"Private/Provider/Blackboard/AssetDocumentAIBlackboardEntryReplaceClass.cpp");
	const FString V1Path = TEXT("Private/Provider/Blackboard/AssetDocumentAIBlackboardLocalKeysMutation.cpp");
	const FStructuralPublicationSource* CompletionHeader =
		FindStructuralPublicationSource(Sources, CompletionHeaderPath);
	const FStructuralPublicationSource* Completion =
		FindStructuralPublicationSource(Sources, CompletionPath);
	const FStructuralPublicationSource* LocatorCodec =
		FindStructuralPublicationSource(Sources, LocatorCodecPath);
	const FStructuralPublicationSource* Inventory =
		FindStructuralPublicationSource(Sources, InventoryPath);
	const FStructuralPublicationSource* ValueOrHeader =
		FindStructuralPublicationSource(Sources, ValueOrHeaderPath);
	const FStructuralPublicationSource* ValueOr =
		FindStructuralPublicationSource(Sources, ValueOrPath);
	const FStructuralPublicationSource* Structural =
		FindStructuralPublicationSource(Sources, StructuralPath);
	const FStructuralPublicationSource* Add = FindStructuralPublicationSource(Sources, AddPath);
	const FStructuralPublicationSource* Replace = FindStructuralPublicationSource(Sources, ReplacePath);
	const FStructuralPublicationSource* V1 = FindStructuralPublicationSource(Sources, V1Path);
	if (!Test.TestNotNull(TEXT("loaded-consumer completion header exists"), CompletionHeader)
		|| !Test.TestNotNull(TEXT("loaded-consumer completion facade exists"), Completion)
		|| !Test.TestNotNull(TEXT("loaded-consumer Locator Codec split exists"), LocatorCodec)
		|| !Test.TestNotNull(TEXT("loaded-consumer Inventory split exists"), Inventory)
		|| !Test.TestNotNull(TEXT("ValueOr invalidation header exists"), ValueOrHeader)
		|| !Test.TestNotNull(TEXT("ValueOr invalidation implementation exists"), ValueOr)
		|| !Test.TestNotNull(TEXT("structural publication owner exists"), Structural)
		|| !Test.TestNotNull(TEXT("ADD caller exists"), Add)
		|| !Test.TestNotNull(TEXT("replace-class caller exists"), Replace)
		|| !Test.TestNotNull(TEXT("immutable v1 caller exists"), V1)) return;

	Test.TestEqual(TEXT("loaded-consumer governance ID has its frozen spelling exactly once"),
		CountOccurrences(CompletionHeader->Text,
			TEXT("AD.AI.BLACKBOARD.LOADED-BT-CONSUMER-COMPLETION-v1")), 1);
	Test.TestEqual(TEXT("ValueOr governance ID is declared exactly once"),
		CountOccurrences(ValueOrHeader->Text,
			TEXT("AD.AI.BLACKBOARD.VALUE-OR-KEY-CACHE-INVALIDATION-v1")), 1);

	Test.TestTrue(TEXT("Completion owns only the two private authority entry symbols"),
		Completion->Text.Contains(TEXT("CaptureLoadedBehaviorTreeConsumerReceipt"))
			&& Completion->Text.Contains(TEXT("CompleteLoadedBehaviorTreeConsumerReceipt")));
	Test.TestEqual(TEXT("Completion defines Capture exactly once"), CountOccurrences(Completion->Text,
		TEXT("AssetDocumentAIBlackboardLoadedConsumerCompletion::CaptureLoadedBehaviorTreeConsumerReceipt(")), 1);
	Test.TestEqual(TEXT("Completion defines Complete exactly once"), CountOccurrences(Completion->Text,
		TEXT("AssetDocumentAIBlackboardLoadedConsumerCompletion::CompleteLoadedBehaviorTreeConsumerReceipt(")), 1);
	Test.TestFalse(TEXT("Locator Codec does not gain a completion entry symbol"),
		LocatorCodec->Text.Contains(TEXT("CaptureLoadedBehaviorTreeConsumerReceipt"))
			|| LocatorCodec->Text.Contains(TEXT("CompleteLoadedBehaviorTreeConsumerReceipt")));
	Test.TestFalse(TEXT("Inventory does not gain a completion entry symbol"),
		Inventory->Text.Contains(TEXT("CaptureLoadedBehaviorTreeConsumerReceipt"))
			|| Inventory->Text.Contains(TEXT("CompleteLoadedBehaviorTreeConsumerReceipt")));
	Test.TestTrue(TEXT("Locator Codec owns the one frozen digest codec"),
		LocatorCodec->Text.Contains(TEXT("EncodeBlackboardBTLeafLocatorDigestV1"))
			&& LocatorCodec->Text.Contains(TEXT("assetdocument.ai.blackboard.bt_leaf_locator_digest.v1")));
	Test.TestEqual(TEXT("loaded-consumer cluster has one codec definition"),
		CountOccurrences(LocatorCodec->Text,
			TEXT("bool EncodeBlackboardBTLeafLocatorDigestV1(")), 1);
	Test.TestEqual(TEXT("Completion does not define or call the codec"),
		CountOccurrences(Completion->Text, TEXT("EncodeBlackboardBTLeafLocatorDigestV1(")), 0);
	Test.TestTrue(TEXT("Inventory calls the one Locator Codec entry directly"),
		CountOccurrences(Inventory->Text, TEXT("EncodeBlackboardBTLeafLocatorDigestV1(")) > 0);
	Test.TestTrue(TEXT("Completion owns exact Registry coordination helpers"),
		Completion->Text.Contains(TEXT("QueryExactBehaviorTrees"))
			&& Completion->Text.Contains(TEXT("QueryPackageAssets"))
			&& Completion->Text.Contains(TEXT("QueryPackageReferencers")));
	Test.TestFalse(TEXT("Inventory remains traversal-only without Registry coordination"),
		Inventory->Text.Contains(TEXT("QueryExactBehaviorTrees"))
			|| Inventory->Text.Contains(TEXT("QueryPackageAssets"))
			|| Inventory->Text.Contains(TEXT("QueryPackageReferencers")));
	Test.TestEqual(TEXT("completion facade has one Registry acquisition callsite"),
		CountOccurrences(Completion->Text, TEXT("IAssetRegistry::Get()")), 1);
	for (const TCHAR* Forbidden : {TEXT("TObjectIterator"), TEXT("ForEachObject"),
		TEXT("GUObjectArray"), TEXT("LoadObject"), TEXT("TryLoad"), TEXT("FAssetData::GetAsset")})
	{
		Test.TestEqual(*FString::Printf(TEXT("loaded-consumer cluster remains non-loading: %s"), Forbidden),
			CountOccurrences(Completion->Text, Forbidden) + CountOccurrences(LocatorCodec->Text, Forbidden)
				+ CountOccurrences(Inventory->Text, Forbidden), 0);
	}
	Test.TestTrue(TEXT("completion keeps paired Registry readiness and snapshot checks"),
		CountOccurrences(Completion->Text, TEXT("IsSearchAllAssets()")) == 2
			&& CountOccurrences(Completion->Text, TEXT("IsGathering()")) == 2
			&& Completion->Text.Contains(TEXT("SnapshotA")) && Completion->Text.Contains(TEXT("SnapshotB")));

	for (const TCHAR* Family : {TEXT("FValueOrBBKey_Bool"), TEXT("FValueOrBBKey_Class"),
		TEXT("FValueOrBBKey_Enum"), TEXT("FValueOrBBKey_Float"), TEXT("FValueOrBBKey_Int32"),
		TEXT("FValueOrBBKey_Name"), TEXT("FValueOrBBKey_String"), TEXT("FValueOrBBKey_Object"),
		TEXT("FValueOrBBKey_Rotator"), TEXT("FValueOrBBKey_Struct"), TEXT("FValueOrBBKey_Vector")})
	{
		Test.TestTrue(*FString::Printf(TEXT("ValueOr family remains frozen: %s"), Family),
			ValueOrHeader->Text.Contains(Family) && ValueOr->Text.Contains(Family));
	}
	Test.TestEqual(TEXT("ValueOr invalidation has exactly eleven family cases"),
		CountOccurrences(ValueOr->Text, TEXT("case EExactValueOrBlackboardKeyFamily::")), 11);
	for (const TCHAR* SchemaPath : {TEXT("FClassOrObjectSchema"), TEXT("FEnumSchema"),
		TEXT("FStructSchema"), TEXT("RebuildSimple<")})
	{
		Test.TestTrue(*FString::Printf(TEXT("ValueOr keeps a frozen schema path: %s"), SchemaPath),
			ValueOr->Text.Contains(SchemaPath));
	}

	FString AddPacket;
	if (Test.TestTrue(TEXT("ADD caller packet is physically present"),
		ExtractExactFunctionPacket(Add->Text, TEXTVIEW("ApplyAssetDocumentAIBlackboardEntryAddWithIdentity("), AddPacket)))
	{
		RequireOrderedStructuralTokens(Test, AddPacket, {
			TEXT("CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage("),
			TEXT("TransactionBinding.EnlistFootprint("),
			TEXT("PublishLocalKeysSnapshot("),
			TEXT("UpdateIfHasSynchronizedKeys()"),
			TEXT("PropagateKeyChangesToDerivedBlackboardAssets()"),
			TEXT("BeforeLoadedConsumerForwardCompletion"),
			TEXT("CompletePublishedLoadedConsumers("),
			TEXT("AfterLoadedConsumerForwardCompletion"),
			TEXT("EntryAddAfterPublication"),
			TEXT("ValidateEntryAddDerivedState("),
			TEXT("BuildAssetDocumentAIBlackboardEntryMetadataReadback(")});
	}
	FString ReplacePacket;
	if (Test.TestTrue(TEXT("replace-class caller packet is physically present"),
		ExtractExactFunctionPacket(Replace->Text, TEXTVIEW("ApplyAssetDocumentAIBlackboardEntryReplaceClassWithIdentity("), ReplacePacket)))
	{
		RequireOrderedStructuralTokens(Test, ReplacePacket, {
			TEXT("CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage("),
			TEXT("TransactionBinding.EnlistFootprint("),
			TEXT("PublishLocalKeysSnapshot("),
			TEXT("UpdateIfHasSynchronizedKeys()"),
			TEXT("PropagateKeyChangesToDerivedBlackboardAssets()"),
			TEXT("BeforeLoadedConsumerForwardCompletion"),
			TEXT("CompletePublishedLoadedConsumers("),
			TEXT("AfterLoadedConsumerForwardCompletion"),
			TEXT("EntryReplaceClassAfterPublication"),
			TEXT("ValidateEntryReplaceClassDerivedState("),
			TEXT("BuildAssetDocumentAIBlackboardEntryMetadataReadback(")});
	}
	FString V1Packet;
	if (Test.TestTrue(TEXT("immutable v1 caller packet is physically present"),
		ExtractExactFunctionPacket(V1->Text, TEXTVIEW("ApplyLocalKeyMutation("), V1Packet)))
	{
		RequireOrderedStructuralTokens(Test, V1Packet, {
			TEXT("EnlistLocalKeyWrite("),
			TEXT("WriteExistingKeyProperties("),
			TEXT("ReplaceLocalKeysThroughPropertyLifecycle("),
			TEXT("UpdateIfHasSynchronizedKeys()"),
			TEXT("PropagateKeyChangesToDerivedBlackboardAssets()"),
			TEXT("BeforeLoadedConsumerForwardCompletion"),
			TEXT("CompletePublishedLoadedConsumers("),
			TEXT("AfterLoadedConsumerForwardCompletion"),
			TEXT("AfterLocalKeyIdRefresh")});
	}
	FString V1EnlistmentPacket;
	if (Test.TestTrue(TEXT("immutable v1 typed-preimage and enlistment packet is physically present"),
		ExtractExactFunctionPacket(V1->Text, TEXTVIEW("EnlistLocalKeyWrite("), V1EnlistmentPacket)))
	{
		RequireOrderedStructuralTokens(Test, V1EnlistmentPacket, {
			TEXT("CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage("),
			TEXT("TransactionBinding.EnlistFootprint(")});
	}
	FString V1PublicationPacket;
	if (Test.TestTrue(TEXT("immutable v1 publication packet is physically present"),
		ExtractExactFunctionPacket(V1->Text, TEXTVIEW("ReplaceLocalKeysThroughPropertyLifecycle("), V1PublicationPacket)))
	{
		RequireOrderedStructuralTokens(Test, V1PublicationPacket, {
			TEXT("PublishLocalKeysSnapshot(")});
	}
	FString V1ExecutePacket;
	if (Test.TestTrue(TEXT("immutable v1 supported-entry packet is physically present"),
		ExtractExactFunctionPacket(V1->Text, TEXTVIEW("ExecuteLocalKeyReplace("), V1ExecutePacket)))
	{
		RequireOrderedStructuralTokens(Test, V1ExecutePacket, {
			TEXT("ApplyLocalKeyMutation("), TEXT("BuildLocalKeysSuccess(")});
	}
	RequireOrderedStructuralTokens(Test, Structural->Text, {
		TEXT("Inner->Rollback("), TEXT("ELoadedConsumerCompletionPhase::RollbackRestored")});
	Test.TestTrue(TEXT("rollback maps completion failure through the existing Atomic result"),
		Structural->Text.Contains(TEXT("Result.bValid = false"))
			&& Structural->Text.Contains(TEXT("CompleteRollback(Result.bValid)")));
}
} // namespace AssetDocumentAIProviderAuthorityTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAIProviderAuthorityTest,
	"UEAssetDoc.AIProvider.Authority.PublicSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIProviderAuthorityTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIProviderAuthorityTests;
	TestTrue(TEXT("authority executes on the GameThread"), IsInGameThread());
	TestTrue(TEXT("authority executes in an Editor process"), GIsEditor);
	TestFalse(TEXT("authority does not run as a commandlet"), IsRunningCommandlet());
	RequireSharedPublicAuthority(*this);
	RequireUpdateExistingAuthority(*this);
	RequireParentReadbackAuthority(*this);
	RequireParentSetV2Authority(*this);
	RequireLocalKeysReadbackAuthority(*this);
	RequireStructCanonicalReadAuthority(*this);
	RequireEntryAddAuthority(*this);
	RequireEntryReplaceClassAuthority(*this);
	RequireEntryRemoveAuthority(*this);

	FAssetDocumentAITestFixture Fixture;
	if (!Fixture.CreateFactoryOwnedScratchBundle(*this))
	{
		return false;
	}
	// Stock PostPlacedNewNode may pick a Blackboard already loaded by another
	// Editor test. This property-handle fixture deliberately starts unbound.
	auto* Root = Fixture.GetRootNode();
	if (!TestNotNull(TEXT("property-handle fixture has its exact scratch root"), Root)) return false;
	Root->BlackboardAsset = nullptr;
	Root->UpdateBlackboard();
	Fixture.RequireUniqueRootPropertyHandle(*this);
	const bool bCacheReady = Fixture.BootstrapBehaviorTreeEditorCache(*this);
	const bool bToolkitClean = Fixture.CloseBehaviorTreeEditorToolkit(*this);
	TestTrue(TEXT("stock public Editor path initializes BehaviorTree class cache"), bCacheReady);
	TestTrue(TEXT("temporary BehaviorTree toolkit is fully released"), bToolkitClean);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAIBlackboardStructuralSourceCheckTest,
	"UEAssetDoc.AIProvider.BlackboardV2.StructuralSourceCheck",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardStructuralSourceCheckTest::RunTest(const FString&)
{
	AssetDocumentAIProviderAuthorityTests::RequireBlackboardRowTopologySourceCheck(*this);
	AssetDocumentAIProviderAuthorityTests::RequireStructCanonicalWriteAuthority(*this);
	AssetDocumentAIProviderAuthorityTests::RequireEntryRemoveAuthority(*this);
	AssetDocumentAIProviderAuthorityTests::RequireStructuralPublicationSourceCheck(*this);
	AssetDocumentAIProviderAuthorityTests::RequireLoadedConsumerCompletionSourceCheck(*this);
	return !HasAnyErrors();
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIDerivedCompletionSourceCheck,
 "UEAssetDoc.AIProvider.BehaviorTree.DerivedCompletionSourceCheck",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIDerivedCompletionSourceCheck::RunTest(const FString&)
{
 using namespace AssetDocumentAIProviderAuthorityTests;
 TArray<FStructuralPublicationSource> Sources;
 if (!LoadStructuralPublicationSources(*this, Sources)) return false;
 const auto* Owner = FindStructuralPublicationSource(Sources,
  TEXT("Private/BehaviorTree/AssetDocumentAIBehaviorTreeDerivedCompletion.cpp"));
 const auto* Mirror = FindStructuralPublicationSource(Sources,
  TEXT("Private/BehaviorTree/AssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollback.cpp"));
 const auto* Handler = FindStructuralPublicationSource(Sources,
  TEXT("Private/Provider/BehaviorTree/AssetDocumentAIBehaviorTreeDecoratorRefresh.cpp"));
 if (!TestNotNull(TEXT("joint owner exists"), Owner) || !TestNotNull(TEXT("mirror exists"), Mirror)
  || !TestNotNull(TEXT("decorator handler exists"), Handler)) return false;
 TestEqual(TEXT("joint owner defines the two exclusive lifecycle finalizers"), CountCodeOccurrences(Owner->Text,
  TEXT("public IAssetDocumentPostRollbackFinalizer")), 2);
 TestEqual(TEXT("mirror has no second finalizer"), CountCodeOccurrences(Mirror->Text,
  TEXT("public IAssetDocumentPostRollbackFinalizer")), 0);
 TestEqual(TEXT("joint owner has one registration site per exclusive lifecycle"), CountCodeOccurrences(Owner->Text, TEXT("EnlistPostRollbackFinalizer(")), 2);
 TestEqual(TEXT("mirror never enlists finalizer"), CountCodeOccurrences(Mirror->Text, TEXT("EnlistPostRollbackFinalizer(")), 0);
 TestEqual(TEXT("handler never calls standalone mirror"), CountCodeOccurrences(Handler->Text,
  TEXT("RebuildAssetDocumentBehaviorTreeRuntimeMirror(")), 0);
 RequireOrderedStructuralTokens(*this, Owner->Text, {TEXT("Transaction.EnlistPostRollbackFinalizer("), TEXT("Transaction.EnlistFootprint(")});
 RequireOrderedStructuralTokens(*this, Owner->Text, {TEXT("Found->NotifyPreChange()"), TEXT("Found->NotifyPostChange("), TEXT("Found->NotifyFinishedChangingProperties()")});
 for (const auto* Source : {Owner, Handler})
 {
  for (const TCHAR* Forbidden : {TEXT("NewObject"), TEXT("DuplicateObject"), TEXT("ProcessEvent"),
   TEXT("CaptureLoadedBehaviorTreeConsumerReceipt")})
   TestEqual(*FString::Printf(TEXT("D3 excludes %s from %s"), Forbidden, *Source->RelativePath),
    CountCodeOccurrences(Source->Text, Forbidden), 0);
 }
 TestEqual(TEXT("decorator handler does not enter installed lifecycle"),
  CountCodeOccurrences(Handler->Text, TEXT("InstalledReplacement")), 0);
 FString Sink;
 TestTrue(TEXT("A3S exact private sink packet exists"), ExtractExactFunctionPacket(Owner->Text,
  TEXT("FValidation CompleteExactCurrentGraphValueOrInventory("), Sink));
 TestEqual(TEXT("A3S sole BT physical sink callsite"), CountCodeOccurrences(Sink, TEXT("InvalidateExactValueOrBlackboardKeyCache(")), 1);
 TestEqual(TEXT("A3S no caller outside exact sink"), CountCodeOccurrences(Owner->Text, TEXT("InvalidateExactValueOrBlackboardKeyCache(")), 1);
 auto AdmittedSinkSource = [&](const FString& Path, const FString& Text)
 {
  if (Path != Owner->RelativePath)
   return CountCodeOccurrences(Text, TEXT("InvalidateExactValueOrBlackboardKeyCache(")) == 0
    && CountCodeOccurrences(Text, TEXT("ValueOrKeyCacheInvalidation.h")) == 0;
  FString Packet;
  return ExtractExactFunctionPacket(Text, TEXT("FValidation CompleteExactCurrentGraphValueOrInventory("), Packet)
   && CountCodeOccurrences(Packet, TEXT("InvalidateExactValueOrBlackboardKeyCache(")) == 1
   && CountCodeOccurrences(Text, TEXT("InvalidateExactValueOrBlackboardKeyCache(")) == 1;
 };
 TestTrue(TEXT("approved exact sink source is admitted"), AdmittedSinkSource(Owner->RelativePath, Owner->Text));
 TestFalse(TEXT("handler sink mutation is rejected"), AdmittedSinkSource(Handler->RelativePath,
  Handler->Text + TEXT("\nvoid Wrong(){InvalidateExactValueOrBlackboardKeyCache(Value);}")));
 TestFalse(TEXT("inventory sink mutation is rejected"), AdmittedSinkSource(
  TEXT("Private/BehaviorTree/AssetDocumentAIBehaviorTreeValueOrKeyInventory.cpp"), Sink));
 TestFalse(TEXT("second lower-mode sink mutation is rejected"), AdmittedSinkSource(Owner->RelativePath,
  Owner->Text + TEXT("\nvoid MirrorOnly(){InvalidateExactValueOrBlackboardKeyCache(Value);}")));
 TestTrue(TEXT("forward top mode guards completion"), Owner->Text.Contains(TEXT("if (Top) { R = CompleteExactCurrentGraphValueOrInventory(")));
 TestTrue(TEXT("rollback immutable top mode guards completion"), Owner->Text.Contains(TEXT("if (Result.bValid && Top) Result = CompleteExactCurrentGraphValueOrInventory(")));
 for (const auto& Source : Sources)
 {
  if (!Source.RelativePath.StartsWith(TEXT("Private/BehaviorTree/")) && !Source.RelativePath.StartsWith(TEXT("Private/Provider/BehaviorTree/"))) continue;
  if (Source.RelativePath != Owner->RelativePath)
  {
   TestEqual(*FString::Printf(TEXT("A3S no unapproved sink in %s"), *Source.RelativePath),
    CountCodeOccurrences(Source.Text, TEXT("InvalidateExactValueOrBlackboardKeyCache(")), 0);
   TestEqual(*FString::Printf(TEXT("A3S no unapproved invalidator include in %s"), *Source.RelativePath),
    CountCodeOccurrences(Source.Text, TEXT("ValueOrKeyCacheInvalidation.h")), 0);
  }
  for (const TCHAR* Forbidden : {TEXT("CaptureLoadedBehaviorTreeConsumerReceipt("), TEXT("CompleteLoadedBehaviorTreeConsumerReceipt("),
   TEXT("EncodeBlackboardBTLeafLocatorDigestV1("), TEXT("AccessRawData("), TEXT("CachedKeyID")})
   TestEqual(*FString::Printf(TEXT("A3S excludes %s from %s"), Forbidden, *Source.RelativePath), CountCodeOccurrences(Source.Text, Forbidden), 0);
 }
 return !HasAnyErrors();
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAISelectorBatchSourceCheck,
 "UEAssetDoc.AIProvider.BehaviorTree.SelectorBatchSourceCheck",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAISelectorBatchSourceCheck::RunTest(const FString&)
{
 using namespace AssetDocumentAIProviderAuthorityTests;
 TArray<FStructuralPublicationSource> Sources;
 if (!LoadStructuralPublicationSources(*this, Sources)) return false;
 const auto* Scalar = FindStructuralPublicationSource(Sources,
  TEXT("Private/TypedPreimage/AssetDocumentAISelectorTypedPreimage.cpp"));
 const auto* Shared = FindStructuralPublicationSource(Sources,
  TEXT("Private/TypedPreimage/AssetDocumentAISelectorTypedPreimageState.cpp"));
 const auto* Batch = FindStructuralPublicationSource(Sources,
  TEXT("Private/TypedPreimage/AssetDocumentAISelectorBatchTypedPreimage.cpp"));
 const auto* Internal = FindStructuralPublicationSource(Sources,
  TEXT("Private/TypedPreimage/AssetDocumentAISelectorTypedPreimageInternal.h"));
 const auto* Handler = FindStructuralPublicationSource(Sources,
  TEXT("Private/Provider/BehaviorTree/AssetDocumentAIBehaviorTreeSelectorBatchResolve.cpp"));
 if (!TestNotNull(TEXT("scalar owner exists"), Scalar)
  || !TestNotNull(TEXT("shared capture owner exists"), Shared)
  || !TestNotNull(TEXT("aggregate owner exists"), Batch)
  || !TestNotNull(TEXT("private batch seam exists"), Internal)
  || !TestNotNull(TEXT("ordinary batch handler exists"), Handler)) return false;
 TestEqual(TEXT("scalar alone retains its existing enlist boundary"), CountCodeOccurrences(Scalar->Text,
  TEXT("CaptureAndEnlistAssetDocumentAIBehaviorTreeSelectorPreimage(")), 1);
 TestEqual(TEXT("scalar reuses one shared capture"), CountCodeOccurrences(Scalar->Text, TEXT("CaptureSelectorState(")), 1);
 TestEqual(TEXT("shared state does not enlist actions"), CountCodeOccurrences(Shared->Text, TEXT("EnlistAliasActions(")), 0);
 TestEqual(TEXT("shared state does not enlist footprints"), CountCodeOccurrences(Shared->Text, TEXT("EnlistFootprint(")), 0);
 TestEqual(TEXT("aggregate has one enlist owner"), CountCodeOccurrences(Batch->Text, TEXT("EnlistAliasActions(")), 1);
 TestEqual(TEXT("provider captures one aggregate"), CountCodeOccurrences(Handler->Text,
  TEXT("CaptureAndEnlistAssetDocumentAIBehaviorTreeSelectorBatchPreimage(")), 1);
 TestEqual(TEXT("provider declares one aggregate footprint"), CountCodeOccurrences(Handler->Text, TEXT("EnlistFootprint(")), 1);
 RequireOrderedStructuralTokens(*this, Batch->Text,
  {TEXT("Result = CaptureInventory("), TEXT("MakeShared<FSelectorBatchRollbackState>"), TEXT("Result = EnlistAliasActions(")});
 RequireOrderedStructuralTokens(*this, Handler->Text,
  {TEXT("CaptureAndEnlistAssetDocumentAIBehaviorTreeSelectorBatchPreimage("), TEXT("EnlistFootprint("), TEXT("ResolveSelectedKey(")});
 TestTrue(TEXT("aggregate fixes the sole scalar-compatible component"),
  Batch->Text.Contains(TEXT("assetdocument.ai.behavior_tree.selector_cache.v1"))
  && Batch->Text.Contains(TEXT("Components = {SelectorComponent}")));
 TestTrue(TEXT("aggregate restores reverse canonical inventory"),
  Batch->Text.Contains(TEXT("Index = Selectors.Num() - 1; Index >= 0; --Index")));
 for (const auto* Source : {Batch, Handler})
  TestEqual(TEXT("batch does not loop the scalar boundary"), CountCodeOccurrences(Source->Text,
   TEXT("CaptureAndEnlistAssetDocumentAIBehaviorTreeSelectorPreimage(")), 0);
 for (const auto* Source : {Scalar, Shared, Batch, Internal})
  for (const TCHAR* Forbidden : {TEXT("CopyCompleteValue"), TEXT("ImportText"), TEXT("ExportText"),
   TEXT("ProcessEvent"), TEXT("FindObject"), TEXT("LoadObject"), TEXT("TryLoad")})
   TestEqual(*FString::Printf(TEXT("selector private owner excludes %s from %s"), Forbidden, *Source->RelativePath),
    CountCodeOccurrences(Source->Text, Forbidden), 0);
 return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
