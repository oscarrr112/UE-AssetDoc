// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

// Test harness for the "graph" provider: resolves a real Editor asset lease
// (editor.asset.resolve_existing.v1), aliases it to the graph provider and
// invokes graph primitives with JSON value arguments, the way the runtime does.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "AssetDocumentEditorPrimitiveProvider.h"
#include "AssetDocumentGraphProvider.h"
#include "AssetDocumentGraphValue.h"
#include "AssetDocumentRollbackAction.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "ObjectTools.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"

namespace UE::AssetDocument::Graph::Tests
{
class FTestResolver final : public IAssetDocumentHandleResolver
{
public:
	struct FEntry
	{
		EAssetDocumentHandleKind Kind = EAssetDocumentHandleKind::Object;
		EAssetDocumentHandleAccess Access = EAssetDocumentHandleAccess::ReadOnly;
		FString ProviderId;
		TSharedPtr<IAssetDocumentNativeHandlePayload> Payload;
	};

	class FLease final : public IAssetDocumentVerifiedHandleLease
	{
	public:
		FLease(FAssetDocumentOpaqueHandle InHandle, IAssetDocumentNativeHandlePayload& InPayload)
			: Handle(MoveTemp(InHandle)), Payload(&InPayload)
		{
		}
		virtual const FAssetDocumentOpaqueHandle& GetHandle() const override { return Handle; }
		virtual IAssetDocumentNativeHandlePayload& GetPayloadChecked() const override { return *Payload; }

	private:
		FAssetDocumentOpaqueHandle Handle;
		IAssetDocumentNativeHandlePayload* Payload = nullptr;
	};

	explicit FTestResolver(const FAssetDocumentMutationToken& InToken) : Token(InToken) {}

	virtual FAssetDocumentIssuedHandleResult Issue(
		EAssetDocumentHandleKind Kind, EAssetDocumentHandleAccess Access,
		TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload) override
	{
		FAssetDocumentIssuedHandleResult Result;
		if (!Payload || !Payload->IsAlive())
		{
			return Result;
		}
		FEntry& Entry = Entries.AddDefaulted_GetRef();
		Entry.Kind = Kind;
		Entry.Access = Access;
		Entry.ProviderId = TEXT("editor");
		Entry.Payload = MakeShareable(Payload.Release());
		Result.Handle = MakeHandle(Entry, Entries.Num());
		return Result;
	}

	virtual FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(
		const FAssetDocumentOpaqueHandle& Handle, const FAssetDocumentHandleValidationContext& Context) override
	{
		FAssetDocumentVerifiedLeaseResult Result;
		const FAssetDocumentContractValidationResult Metadata = ValidateAssetDocumentHandleMetadata(Handle, Context);
		if (!Metadata.bValid)
		{
			Result.Diagnostics = Metadata.Diagnostics;
			return Result;
		}
		const int32 Index = static_cast<int32>(Handle.Slot) - 1;
		if (Entries.IsValidIndex(Index))
		{
			FEntry& Entry = Entries[Index];
			if (Entry.Kind == Handle.Kind && Entry.Access == Handle.Access && Entry.ProviderId == Handle.ProviderId
				&& Entry.Payload.IsValid() && Entry.Payload->IsAlive())
			{
				Result.Lease = MakeUnique<FLease>(Handle, *Entry.Payload);
			}
		}
		return Result;
	}

	virtual FAssetDocumentHandleInvalidationResult Invalidate(const FAssetDocumentHandleInvalidation& Event) override
	{
		const uint32 Count = static_cast<uint32>(Entries.Num());
		Entries.Reset();
		return {Count};
	}

	TOptional<FAssetDocumentOpaqueHandle> Alias(const FAssetDocumentOpaqueHandle& Source, FStringView InProviderId)
	{
		const int32 Index = static_cast<int32>(Source.Slot) - 1;
		if (!Entries.IsValidIndex(Index))
		{
			return {};
		}
		FEntry Copy = Entries[Index];
		Copy.ProviderId = FString(InProviderId);
		const int32 Slot = Entries.Add(MoveTemp(Copy)) + 1;
		return MakeHandle(Entries[Slot - 1], Slot);
	}

private:
	FAssetDocumentOpaqueHandle MakeHandle(const FEntry& Entry, int32 Slot) const
	{
		FAssetDocumentOpaqueHandle Handle;
		Handle.RequestId = Token.RequestId;
		Handle.TransactionId = Token.TransactionId;
		Handle.RuntimeRevision = Token.RuntimeRevision;
		Handle.ProviderId = Entry.ProviderId;
		Handle.Slot = Slot;
		Handle.Generation = 1;
		Handle.Kind = Entry.Kind;
		Handle.Access = Entry.Access;
		return Handle;
	}

	FAssetDocumentMutationToken Token;
	TArray<FEntry> Entries;
};

// Accepts every footprint; records them. Rollback is Core's job and is not
// exercised here (Core restores the package snapshot).
class FTestTransaction final : public IAssetDocumentAtomicTransaction
{
public:
	explicit FTestTransaction(const FAssetDocumentMutationToken& InToken) : Token(InToken) {}

	virtual FGuid GetTransactionId() const override { return Token.TransactionId; }
	virtual EAssetDocumentAtomicState GetState() const override { return State; }
	virtual const FAssetDocumentMutationToken& GetMutationToken() const override { return Token; }
	virtual FAssetDocumentContractValidationResult EnlistFootprint(
		TConstArrayView<FAssetDocumentMutationFootprint> Footprint) override
	{
		Footprints.Append(Footprint.GetData(), Footprint.Num());
		State = EAssetDocumentAtomicState::Mutating;
		return Valid();
	}
	virtual FAssetDocumentContractValidationResult MarkReadyToCommit() override { return Valid(); }
	virtual FAssetDocumentContractValidationResult Commit(const FAssetDocumentDurableCompletionReceipt&) override { return Valid(); }
	virtual FAssetDocumentContractValidationResult Rollback(FStringView) override { return Valid(); }
	virtual FAssetDocumentContractValidationResult EnlistRollbackAction(TSharedRef<IAssetDocumentRollbackAction>) override { return Valid(); }
	virtual FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(TSharedRef<IAssetDocumentPostRollbackFinalizer>) override { return Valid(); }

	TArray<FAssetDocumentMutationFootprint> Footprints;

private:
	static FAssetDocumentContractValidationResult Valid()
	{
		FAssetDocumentContractValidationResult Result;
		Result.bValid = true;
		return Result;
	}

	FAssetDocumentMutationToken Token;
	EAssetDocumentAtomicState State = EAssetDocumentAtomicState::Live;
};

inline FAssetDocumentMutationToken MakeToken()
{
	FAssetDocumentMutationToken Token;
	Token.RequestId = FGuid::NewGuid();
	Token.TransactionId = FGuid::NewGuid();
	Token.RuntimeRevision = 1;
	return Token;
}

// One request scope over one asset.
class FSession
{
public:
	FSession(FAutomationTestBase& InTest, UObject& InAsset)
		: FSession(InTest, InAsset, CreateAssetDocumentGraphPrimitiveProvider())
	{
	}

	// Another provider's primitives over the same request scope (its own module's tests).
	FSession(FAutomationTestBase& InTest, UObject& InAsset, TSharedRef<IAssetDocumentPrimitiveProvider> InProvider)
		: Test(InTest), Asset(&InAsset), Token(MakeToken()), Resolver(Token), Transaction(Token), Provider(MoveTemp(InProvider))
	{
		FAssetDocumentPrimitiveInvocation Resolve;
		Resolve.PrimitiveId = TEXT("editor.asset.resolve_existing.v1");
		Resolve.Version = 1;
		Resolve.RequestId = Token.RequestId;
		Resolve.TransactionId = Token.TransactionId;
		Resolve.RuntimeRevision = Token.RuntimeRevision;
		Resolve.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
		Resolve.ValueArguments = FObjectBuilder()
			.AddString(TEXT("path"), InAsset.GetPathName())
			.AddString(TEXT("expected_class"), InAsset.GetClass()->GetPathName())
			.Build();
		const FAssetDocumentPrimitiveOutcome Resolved = CreateAssetDocumentEditorPrimitiveProvider()->Invoke(Resolve, Resolver, nullptr);
		for (const FAssetDocumentPrimitiveHandleResult& Result : Resolved.HandleResults)
		{
			if (Result.Handles.Num() != 1)
			{
				continue;
			}
			if (Result.Name == TEXT("asset_read"))
			{
				ReadHandle = Resolver.Alias(Result.Handles[0], Provider->GetProviderId());
			}
			else if (Result.Name == TEXT("asset_mutable"))
			{
				MutableHandle = Resolver.Alias(Result.Handles[0], Provider->GetProviderId());
			}
		}
		Test.TestTrue(TEXT("editor.asset.resolve_existing.v1 issued asset handles"), ReadHandle.IsSet() && MutableHandle.IsSet());
	}

	bool IsReady() const { return ReadHandle.IsSet() && MutableHandle.IsSet(); }

	FAssetDocumentPrimitiveOutcome Invoke(const TCHAR* PrimitiveId, const FString& JsonArguments)
	{
		FAssetDocumentPrimitiveOutcome Failed;
		const FAssetDocumentPrimitiveDescriptor* Descriptor = nullptr;
		for (const FAssetDocumentPrimitiveDescriptor& Candidate : Provider->DescribePrimitives())
		{
			if (Candidate.PrimitiveId == PrimitiveId)
			{
				Descriptor = &Candidate;
			}
		}
		FValue Arguments;
		if (Descriptor == nullptr || !IsReady() || !TryParseJsonValue(JsonArguments, Arguments))
		{
			Test.AddError(FString::Printf(TEXT("cannot invoke %s with %s"), PrimitiveId, *JsonArguments));
			return Failed;
		}
		const bool bMutating = Descriptor->Access == EAssetDocumentPrimitiveAccess::Mutating;
		FAssetDocumentPrimitiveInvocation Invocation;
		Invocation.PrimitiveId = PrimitiveId;
		Invocation.Version = Descriptor->Version;
		Invocation.RequestId = Token.RequestId;
		Invocation.TransactionId = Token.TransactionId;
		Invocation.RuntimeRevision = Token.RuntimeRevision;
		Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
		Invocation.ValueArguments = MoveTemp(Arguments);
		Invocation.HandleArguments.Add({TEXT("asset"), bMutating ? MutableHandle.GetValue() : ReadHandle.GetValue()});
		return Provider->Invoke(Invocation, Resolver, &Transaction);
	}

	// Succeeds or records an error with the diagnostics.
	bool Call(const TCHAR* PrimitiveId, const FString& JsonArguments, FValue& OutValue)
	{
		const FAssetDocumentPrimitiveOutcome Outcome = Invoke(PrimitiveId, JsonArguments);
		if (Outcome.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
		{
			for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Outcome.Diagnostics)
			{
				Test.AddError(FString::Printf(TEXT("%s failed: %s %s (%s); args %s"), PrimitiveId,
					*Diagnostic.Code, *Diagnostic.Message, *Diagnostic.Path, *JsonArguments));
			}
			if (Outcome.Diagnostics.IsEmpty())
			{
				Test.AddError(FString::Printf(TEXT("%s failed without diagnostics"), PrimitiveId));
			}
			return false;
		}
		OutValue = Outcome.Value;
		return true;
	}

	FValue CallOrNull(const TCHAR* PrimitiveId, const FString& JsonArguments)
	{
		FValue Value;
		Call(PrimitiveId, JsonArguments, Value);
		return Value;
	}

	// Fails with exactly Code; returns the diagnostic.
	bool ExpectFailure(const TCHAR* PrimitiveId, const FString& JsonArguments, const TCHAR* Code,
		FAssetDocumentRuntimeDiagnostic& OutDiagnostic)
	{
		const FAssetDocumentPrimitiveOutcome Outcome = Invoke(PrimitiveId, JsonArguments);
		if (!Test.TestEqual(FString::Printf(TEXT("%s fails"), PrimitiveId),
			Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed) || Outcome.Diagnostics.IsEmpty())
		{
			return false;
		}
		OutDiagnostic = Outcome.Diagnostics[0];
		return Test.TestEqual(FString::Printf(TEXT("%s diagnostic code"), PrimitiveId), OutDiagnostic.Code, FString(Code));
	}

	FAutomationTestBase& Test;
	UObject* Asset = nullptr;
	FAssetDocumentMutationToken Token;
	FTestResolver Resolver;
	FTestTransaction Transaction;
	TSharedRef<IAssetDocumentPrimitiveProvider> Provider;
	TOptional<FAssetDocumentOpaqueHandle> ReadHandle;
	TOptional<FAssetDocumentOpaqueHandle> MutableHandle;
};

// ---- value navigation -------------------------------------------------------

// "a/b/0/c": object keys and array indices.
inline const FValue* At(const FValue& Root, FStringView Path)
{
	TArray<FString> Parts;
	FString(Path).ParseIntoArray(Parts, TEXT("/"));
	const FValue* Current = &Root;
	for (const FString& Part : Parts)
	{
		if (Current == nullptr)
		{
			return nullptr;
		}
		if (Current->Kind == EAssetDocumentRuntimeValueKind::Array)
		{
			const int32 Index = FCString::Atoi(*Part);
			Current = Current->Elements.IsValidIndex(Index) ? &Current->Elements[Index] : nullptr;
		}
		else
		{
			Current = FindField(*Current, Part);
		}
	}
	return Current;
}

inline FString Str(const FValue& Root, FStringView Path)
{
	const FValue* Value = At(Root, Path);
	return Value != nullptr && Value->Kind == EAssetDocumentRuntimeValueKind::String ? Value->StringValue : FString();
}

inline bool IsTrue(const FValue& Root, FStringView Path)
{
	const FValue* Value = At(Root, Path);
	return Value != nullptr && Value->Kind == EAssetDocumentRuntimeValueKind::Boolean && Value->BooleanValue;
}

inline TArray<FString> Strings(const FValue& Root, FStringView Path)
{
	TArray<FString> Out;
	if (const FValue* Value = At(Root, Path))
	{
		for (const FValue& Element : Value->Elements)
		{
			Out.Add(Element.StringValue);
		}
	}
	return Out;
}

// GraphSnapshot of graph.query.v1 by path.
inline const FValue* FindGraph(const FValue& Query, FStringView Path)
{
	if (const FValue* Graphs = At(Query, TEXT("graphs")))
	{
		for (const FValue& Graph : Graphs->Elements)
		{
			if (Str(Graph, TEXT("path")) == Path)
			{
				return &Graph;
			}
		}
	}
	return nullptr;
}

inline const FValue* FindNode(const FValue& Graph, FStringView Key, FStringView Value)
{
	if (const FValue* Nodes = At(Graph, TEXT("nodes")))
	{
		for (const FValue& Node : Nodes->Elements)
		{
			if (Str(Node, Key) == Value)
			{
				return &Node;
			}
		}
	}
	return nullptr;
}

inline const FValue* FindPin(const FValue& Node, FStringView Name, FStringView Direction = FStringView())
{
	if (const FValue* Pins = At(Node, TEXT("pins")))
	{
		for (const FValue& Pin : Pins->Elements)
		{
			if (Str(Pin, TEXT("name")) == Name && (Direction.IsEmpty() || Str(Pin, TEXT("direction")) == Direction))
			{
				return &Pin;
			}
		}
	}
	return nullptr;
}

inline bool HasLink(const FValue& Graph, FStringView FromNode, FStringView FromPin, FStringView ToNode, FStringView ToPin)
{
	if (const FValue* Links = At(Graph, TEXT("links")))
	{
		for (const FValue& Link : Links->Elements)
		{
			if (Str(Link, TEXT("from/node_guid")) == FromNode && Str(Link, TEXT("from/pin_name")) == FromPin
				&& Str(Link, TEXT("to/node_guid")) == ToNode && Str(Link, TEXT("to/pin_name")) == ToPin)
			{
				return true;
			}
		}
	}
	return false;
}

inline FString PinJson(FStringView NodeGuid, FStringView PinName, FStringView Direction)
{
	return FString::Printf(TEXT("{\"node_guid\":\"%.*s\",\"pin_name\":\"%.*s\",\"direction\":\"%.*s\"}"),
		NodeGuid.Len(), NodeGuid.GetData(), PinName.Len(), PinName.GetData(), Direction.Len(), Direction.GetData());
}

inline FString GraphJson(FStringView Path)
{
	return FString::Printf(TEXT("{\"graph_path\":\"%.*s\"}"), Path.Len(), Path.GetData());
}

// Context string of a diagnostic (e.g. "reason") or the joined string array ("suggestions").
inline FString DiagnosticContext(const FAssetDocumentRuntimeDiagnostic& Diagnostic, FStringView Key)
{
	for (const FAssetDocumentRuntimeDiagnosticContext& Context : Diagnostic.Context)
	{
		if (Context.Key == Key)
		{
			if (Context.Value.Kind == EAssetDocumentRuntimeValueKind::String)
			{
				return Context.Value.StringValue;
			}
			TArray<FString> Parts;
			for (const FValue& Element : Context.Value.Elements)
			{
				Parts.Add(Element.StringValue);
			}
			return FString::Join(Parts, TEXT("\n"));
		}
	}
	return FString();
}

// ---- test-owned Blueprint ---------------------------------------------------

// Actor Blueprint in a unique package (default under /Temp), registered with the asset registry
// (resolve_existing looks assets up there) and fully torn down on destruction.
class FTestBlueprint
{
public:
	explicit FTestBlueprint(const TCHAR* Label, const TCHAR* Root = TEXT("/Temp/AssetDocumentGraphTests"), UClass* Parent = AActor::StaticClass())
	{
		PackageName = FString::Printf(TEXT("%s/%s_%s"), Root, Label,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));
		Package = CreatePackage(*PackageName);
		const FString AssetName = FPackageName::GetShortName(PackageName);
		Blueprint = FKismetEditorUtilities::CreateBlueprint(Parent, Package, FName(*AssetName), BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), TEXT("AssetDocumentGraphTests"));
		if (Blueprint != nullptr)
		{
			FAssetRegistryModule::AssetCreated(Blueprint);
		}
	}

	~FTestBlueprint()
	{
		Destroy();
	}

	void Destroy()
	{
		if (Blueprint != nullptr)
		{
			ObjectTools::DeleteSingleObject(Blueprint, /*bPerformReferenceCheck=*/false);
			Blueprint = nullptr;
		}
		if (UPackage* Existing = FindPackage(nullptr, *PackageName))
		{
			TArray<UObject*> Objects;
			GetObjectsWithPackage(Existing, Objects);
			for (UObject* Object : Objects)
			{
				if (Object != nullptr && !Object->IsRooted())
				{
					Object->ClearFlags(RF_Public | RF_Standalone);
					Object->MarkAsGarbage();
				}
			}
			Existing->SetDirtyFlag(false);
			Existing->ClearFlags(RF_Public | RF_Standalone);
			Existing->MarkAsGarbage();
		}
		Package = nullptr;
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	}

	UBlueprint* Get() const { return Blueprint; }
	UBlueprint* operator->() const { return Blueprint; }

	FString PackageName;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = nullptr;
};
} // namespace UE::AssetDocument::Graph::Tests

#endif // WITH_DEV_AUTOMATION_TESTS
