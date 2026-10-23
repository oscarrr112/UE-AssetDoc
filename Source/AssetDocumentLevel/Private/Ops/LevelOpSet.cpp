// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// set: sparse Details edits of one actor (transform, label, folder, tags, properties,
// component properties, layers, data layers); omitted fields are unchanged.

#include "GameFramework/Actor.h"
#include "LevelEditTypes.h"

namespace UE::AssetDocumentLevel::Private
{
TSharedRef<FJsonObject> MakeLevelOpResult(const TCHAR* Kind, const FString& CreatedPath);

namespace
{
class FLevelOpSet final : public FLevelEditOp
{
public:
	FActorRef Target;
	FLevelActorFieldEdits Edits;

	virtual const TCHAR* GetKind() const override { return TEXT("set"); }

	virtual void Preflight(FLevelEditPlan& Plan) const override
	{
		const int32 ActorIndex = Plan.Resolve(Target, Pointer + TEXT("/actor"));
		if (ActorIndex != INDEX_NONE)
		{
			Plan.PreflightFields(ActorIndex, Edits, Pointer);
		}
	}

	virtual bool Apply(FLevelEditApply& Apply) const override
	{
		AActor* Actor = Apply.ResolveRef(Target, Pointer + TEXT("/actor"));
		if (Actor == nullptr)
		{
			return false;
		}
		const FString Path = GetActorPath(*Actor);
		TSharedRef<FJsonObject> Result = MakeLevelOpResult(GetKind(), FString());
		Result->SetStringField(TEXT("actor"), Path);
		Apply.OpResults[Index] = Result;
		return Apply.ApplyFields(Path, Edits, Pointer);
	}
};

TUniquePtr<FLevelEditOp> ParseSet(const FJsonObject& Json, const FString& Pointer, TArray<FAssetDocumentDiagnostic>& OutDiagnostics)
{
	static const TCHAR* const Fields[] = {TEXT("actor"), TEXT("transform"), TEXT("label"), TEXT("folder"), TEXT("tags"),
		TEXT("properties"), TEXT("components"), TEXT("layers"), TEXT("data_layers")};
	const int32 Before = OutDiagnostics.Num();
	CheckLevelOpFields(Json, Pointer, Fields, OutDiagnostics);
	TUniquePtr<FLevelOpSet> Op = MakeUnique<FLevelOpSet>();
	ParseLevelActorRefField(Json, TEXT("actor"), Pointer, true, Op->Target, OutDiagnostics);
	ParseLevelActorFieldEdits(Json, Pointer, Op->Edits, OutDiagnostics);
	if (OutDiagnostics.Num() == Before && !Op->Edits.HasAny())
	{
		OutDiagnostics.Add(MakeInvalidRequest(Pointer, TEXT("set changes nothing: give at least one of transform, label, folder, tags, "
			"properties, components, layers, data_layers.")));
	}
	return OutDiagnostics.Num() == Before ? TUniquePtr<FLevelEditOp>(MoveTemp(Op)) : nullptr;
}

const FLevelEditOpRegistration SetRegistration(TEXT("set"), &ParseSet);
}
}
