// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

enum class EAssetDocumentResultStatus : uint8
{
	Success,
	Failed,
	PartialFailed
};

struct ASSETDOCUMENT_API FAssetDocumentDiagnostic
{
	FString Path;
	FString Code;
	FString Message;
	// "error", "warning" or "info"; empty (not serialized) for diagnostics that do not carry one.
	FString Severity;
	// Internal detail (primitive id, primitive-side path, compiler messages, ...); kept out of Message.
	TSharedPtr<FJsonObject> Details;
};

struct FAssetDocumentRuntimeDiagnostic;
struct FAssetDocumentRuntimeValueDto;

// Public projection of a runtime diagnostic: path/code/message unchanged, severity as text,
// context entries under Details.
ASSETDOCUMENT_API FAssetDocumentDiagnostic ProjectAssetDocumentRuntimeDiagnostic(const FAssetDocumentRuntimeDiagnostic& Source);
ASSETDOCUMENT_API TSharedPtr<FJsonValue> ProjectAssetDocumentRuntimeValue(const FAssetDocumentRuntimeValueDto& Value);

struct ASSETDOCUMENT_API FAssetDocumentApplyRequest
{
	TSharedPtr<FJsonObject> Document;
	FString SourceDocumentPath;
	bool bSaveAsset = true;
};

struct ASSETDOCUMENT_API FAssetDocumentApplyFileRequest
{
	FString FilePath;
	// A saved apply that changes the asset always rewrites the sidecar with the canonical document
	// (refreshed _meta.sync; a Create is written back as CreateOrUpdate). A preview (bSaveAsset false)
	// or a no-op apply writes nothing.
	bool bSaveAsset = true;
	bool bTriggeredByWatcher = false;
};

struct ASSETDOCUMENT_API FAssetDocumentInspectRequest
{
	FString ClassOrAsset;
	// Ask a profile with large inspect details (Niagara's NiagaraDetails) for a summary of them. The HTTP
	// route summarizes unless the request passes detail=true.
	bool bSummary = false;
};

struct ASSETDOCUMENT_API FAssetDocumentProfileRequest
{
	FString ClassOrAsset;
	// Debug only: keep the runtime registration (lifecycle, required primitives) in the payload.
	bool bIncludeRuntimeRegistration = false;
};

struct ASSETDOCUMENT_API FAssetDocumentTemplateRequest
{
	FString Class;
	FString Target;
};

// A document's optional top-level base revision: the extract it was edited from (the HTTP extract emits it). An apply
// or validate of a document that carries it is refused with Document.StaleBase when the asset changed since, unless
// the document would change nothing; the profiles never see the field.
inline const TCHAR* const AssetDocumentBaseRevisionField = TEXT("BaseRevision");

struct ASSETDOCUMENT_API FAssetDocumentExtractRequest
{
	FString AssetPath;
	bool bDiffOnly = true;
	bool bIncludeAllWritable = false;
	// Add BaseRevision (after Target) to the extracted document (the HTTP extract does).
	bool bIncludeBaseRevision = false;
};

struct ASSETDOCUMENT_API FAssetDocumentValidateRequest
{
	TSharedPtr<FJsonObject> Document;
	FString FilePath;
};

struct ASSETDOCUMENT_API FAssetDocumentDiffRequest
{
	TSharedPtr<FJsonObject> Document;
	FString FilePath;
};

// Duplicate an asset the way the Content Browser's Duplicate does (IAssetTools::DuplicateAsset):
// Source is any readable asset (project, engine or plugin content), Target an absent /Game
// package path. The copy is saved; dependencies are not duplicated.
struct ASSETDOCUMENT_API FAssetDocumentDuplicateRequest
{
	FString Source;
	FString Target;
};

struct ASSETDOCUMENT_API FAssetDocumentResult
{
	EAssetDocumentResultStatus Status = EAssetDocumentResultStatus::Failed;
	FString Message;
	FString Target;
	FString SidecarFilePath;
	FString AssetPath;
	bool bSavedAsset = false;
	bool bWroteSidecar = false;
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	TSharedPtr<FJsonObject> Payload;
	// Blueprint-family targets: the editor's compile status ({Status, Messages}), serialized as "compile".
	TSharedPtr<FJsonObject> Compile;
	// Extract of engine or plugin content (outside /Game): the document cannot be applied to
	// its own Target. Serialized as "read_only": true when set.
	bool bReadOnly = false;

	static FAssetDocumentResult Success(const FString& InMessage);
	static FAssetDocumentResult Failure(const FString& InMessage);
	// Failure that leads with the root cause: Diagnostics are ordered by OrderRootCauseFirst and
	// Message is "<Code>: <Message> (at <Path>)" of Diagnostics[0], plus " (+N more)" when more
	// diagnostics exist. Falls back to FallbackMessage when there are no diagnostics.
	static FAssetDocumentResult FailureFromDiagnostics(
		const TArray<FAssetDocumentDiagnostic>& InDiagnostics,
		const FString& FallbackMessage);
	// Stable reorder: concrete errors first, then errors that only report a consequence of
	// another failure (RequestCoordinator.*, Atomic.*, Persistence.CanonicalMismatch, route
	// wrappers such as GenericRuntime.RequestAdaptationFailed), then warnings and infos.
	static TArray<FAssetDocumentDiagnostic> OrderRootCauseFirst(
		const TArray<FAssetDocumentDiagnostic>& InDiagnostics);
	static FString FormatDiagnosticsSummary(
		const TArray<FAssetDocumentDiagnostic>& InDiagnostics,
		const FString& FallbackMessage);
	bool IsSuccess() const { return Status == EAssetDocumentResultStatus::Success; }
	TSharedPtr<FJsonObject> ToJson() const;
};
