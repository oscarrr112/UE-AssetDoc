// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentIngressQueue.h"

#include "AssetDocumentService.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include <type_traits>

#if WITH_DEV_AUTOMATION_TESTS
static_assert(
	!std::is_copy_constructible_v<FAssetDocumentIngressQueue>,
	"The single ingress coordinator is shared, not copied.");

namespace
{
FString LoadAssetDocumentSource(
	FAutomationTestBase& Test,
	const FString& RelativePath)
{
	const TSharedPtr<IPlugin> Plugin =
		IPluginManager::Get().FindPlugin(TEXT("UEAssetDoc"));
	if (!Plugin.IsValid())
	{
		Test.AddError(TEXT("UEAssetDoc plugin is unavailable"));
		return FString();
	}

	const FString FilePath =
		FPaths::Combine(Plugin->GetBaseDir(), RelativePath);
	FString Source;
	if (!FFileHelper::LoadFileToString(Source, *FilePath))
	{
		Test.AddError(FString::Printf(
			TEXT("Failed to read source gate file '%s'"),
			*FilePath));
	}
	return Source;
}

FString ExtractFunctionBody(
	FAutomationTestBase& Test,
	const FString& Source,
	const FString& Signature)
{
	const int32 SignatureIndex = Source.Find(Signature);
	if (SignatureIndex == INDEX_NONE)
	{
		Test.AddError(FString::Printf(
			TEXT("Missing source function '%s'"),
			*Signature));
		return FString();
	}

	const int32 OpenBraceIndex = Source.Find(
		TEXT("{"),
		ESearchCase::CaseSensitive,
		ESearchDir::FromStart,
		SignatureIndex);
	if (OpenBraceIndex == INDEX_NONE)
	{
		Test.AddError(FString::Printf(
			TEXT("Missing source body for '%s'"),
			*Signature));
		return FString();
	}

	int32 Depth = 0;
	for (int32 Index = OpenBraceIndex; Index < Source.Len(); ++Index)
	{
		if (Source[Index] == TEXT('{'))
		{
			++Depth;
		}
		else if (Source[Index] == TEXT('}'))
		{
			--Depth;
			if (Depth == 0)
			{
				return Source.Mid(
					OpenBraceIndex,
					Index - OpenBraceIndex + 1);
			}
		}
	}

	Test.AddError(FString::Printf(
		TEXT("Unterminated source body for '%s'"),
		*Signature));
	return FString();
}

int32 CountOccurrences(
	const FString& Source,
	const FString& Needle)
{
	int32 Count = 0;
	int32 SearchFrom = 0;
	while (true)
	{
		const int32 FoundIndex = Source.Find(
			Needle,
			ESearchCase::CaseSensitive,
			ESearchDir::FromStart,
			SearchFrom);
		if (FoundIndex == INDEX_NONE)
		{
			return Count;
		}
		++Count;
		SearchFrom = FoundIndex + Needle.Len();
	}
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentIngressSourceArchitectureTest,
	"UEAssetDoc.GenericRuntime.IngressSourceArchitecture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentIngressSourceArchitectureTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const FString ServiceSource = LoadAssetDocumentSource(
		*this,
		TEXT("Source/AssetDocument/Private/AssetDocumentService.cpp"));
	const FString QueueSource = LoadAssetDocumentSource(
		*this,
		TEXT("Source/AssetDocument/Private/AssetDocumentIngressQueue.cpp"));
	const FString WatcherSource = LoadAssetDocumentSource(
		*this,
		TEXT("Source/AssetDocument/Private/AssetDocumentFileWatcher.cpp"));
	const FString EditorSource = LoadAssetDocumentSource(
		*this,
		TEXT("Source/AssetDocument/Private/AssetDocumentEditorSync.cpp"));

	// ApplyFile runs one ApplyFileOnce (and records a watcher apply's outcome); that re-enters
	// Apply exactly once.
	const FString ApplyFileWrapperBody = ExtractFunctionBody(
		*this,
		ServiceSource,
		TEXT("FAssetDocumentService::ApplyFile("));
	TestEqual(
		TEXT("ApplyFile runs ApplyFileOnce exactly once"),
		CountOccurrences(ApplyFileWrapperBody, TEXT("ApplyFileOnce(Request)")),
		1);
	TestFalse(
		TEXT("ApplyFile does not re-enter Apply itself"),
		ApplyFileWrapperBody.Contains(TEXT("ApplyParsedDocument(")));
	const FString ApplyFileBody = ExtractFunctionBody(
		*this,
		ServiceSource,
		TEXT("FAssetDocumentService::ApplyFileOnce("));
	TestEqual(
		TEXT("ApplyFile re-enters Apply exactly once"),
		CountOccurrences(ApplyFileBody, TEXT("ApplyParsedDocument(ApplyRequest)")),
		1);
	TestFalse(
		TEXT("ApplyFile does not select the generic route"),
		ApplyFileBody.Contains(TEXT("GenericRuntimeRoute")) || ApplyFileWrapperBody.Contains(TEXT("GenericRuntimeRoute")));
	TestFalse(
		TEXT("FileWatcher never calls ApplyFile directly"),
		WatcherSource.Contains(TEXT("SubmitApplyFile("))
			|| WatcherSource.Contains(TEXT("Service->ApplyFile(")));

	const FString WatcherRegisterBody = ExtractFunctionBody(
		*this,
		WatcherSource,
		TEXT("FAssetDocumentFileWatcher::Register("));
	TestTrue(
		TEXT("real DirectoryWatcher delegate is registered to HandleDirectoryChanged"),
		WatcherRegisterBody.Contains(
			TEXT("&FAssetDocumentFileWatcher::HandleDirectoryChanged")));

	const FString DirectoryChangedBody = ExtractFunctionBody(
		*this,
		WatcherSource,
		TEXT("FAssetDocumentFileWatcher::HandleDirectoryChanged("));
	TestEqual(
		TEXT("real DirectoryWatcher delegate creates one typed event"),
		CountOccurrences(
			DirectoryChangedBody,
			TEXT("FAssetDocumentIngressEvent")),
		1);
	TestEqual(
		TEXT("real DirectoryWatcher delegate has one queue admission"),
		CountOccurrences(
			DirectoryChangedBody,
			TEXT("Ingress->Enqueue(")),
		1);
	for (const FString& Forbidden : {
		TEXT("QueueFileChange("),
		TEXT("CollectAssetDocumentSidecarsUnderDirectory("),
		TEXT("TryReadFileState("),
		TEXT("IsStableFileState("),
		TEXT("IsSidecarWriteSuppressed("),
		TEXT("IFileManager"),
		TEXT("FileExists("),
		TEXT("DirectoryExists("),
		TEXT("FindFiles")})
	{
		TestFalse(
			FString::Printf(
				TEXT("real DirectoryWatcher delegate excludes pre-enqueue IO path '%s'"),
				*Forbidden),
			DirectoryChangedBody.Contains(Forbidden));
	}
	TestFalse(
		TEXT("watcher source removes the old indirect pre-enqueue QueueFileChange path"),
		WatcherSource.Contains(TEXT("FAssetDocumentFileWatcher::QueueFileChange(")));
	TestFalse(
		TEXT("watcher source removes the old Tick pre-enqueue stat path"),
		WatcherSource.Contains(TEXT("FAssetDocumentFileWatcher::Tick(")));

	for (const FString& Signature : {
		TEXT("FAssetDocumentEditorSync::HandleAssetRenamed("),
		TEXT("FAssetDocumentEditorSync::HandleAssetAdded("),
		TEXT("FAssetDocumentEditorSync::HandleAssetRemoved(")})
	{
		const FString CallbackBody =
			ExtractFunctionBody(*this, EditorSource, Signature);
		TestEqual(
			FString::Printf(TEXT("%s creates one typed event"), *Signature),
			CountOccurrences(
				CallbackBody,
				TEXT("FAssetDocumentIngressEvent")),
			1);
		TestEqual(
			FString::Printf(TEXT("%s has one queue admission"), *Signature),
			CountOccurrences(
				CallbackBody,
				TEXT("Ingress->Enqueue(")),
			1);
		for (const FString& Forbidden : {
			TEXT("SyncSidecar"),
			TEXT("ArchiveSidecar"),
			TEXT("InferDuplicate"),
			TEXT("IFileManager"),
			TEXT("FAssetDocumentSidecar")})
		{
			TestFalse(
				FString::Printf(
					TEXT("%s excludes callback side effect '%s'"),
					*Signature,
					*Forbidden),
				CallbackBody.Contains(Forbidden));
		}
	}

	const FString DrainBody = ExtractFunctionBody(
		*this,
		QueueSource,
		TEXT("FAssetDocumentIngressQueue::DrainOnGameThread("));
	TestEqual(
		TEXT("one drain calls one Service admission"),
		CountOccurrences(
			DrainBody,
			TEXT("Service->")),
		1);
	for (const FString& Forbidden : {
		TEXT("retry"),
		TEXT("Retry"),
		TEXT("Enqueue("),
		TEXT("SubmitApply("),
		TEXT("SubmitApplyFile("),
		TEXT("DispatchApply(")})
	{
		TestFalse(
			FString::Printf(
				TEXT("queue drain excludes '%s'"),
				*Forbidden),
			DrainBody.Contains(Forbidden));
	}
	return true;
}
#endif
