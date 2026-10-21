// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetDocumentAIBlackboardConsumerCompletionTestTypes.h"

#include "AssetDocumentAIPrimitiveProvider.h"
#include "AssetDocumentEditorPrimitiveProvider.h"
#include "AssetDocumentRollbackAction.h"
#include "AIGraphTypes.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Name.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Rotator.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_String.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Struct.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTreeDecoratorGraph.h"
#include "BehaviorTreeDecoratorGraphNode_Decorator.h"
#include "BehaviorTreeDecoratorGraphNode_Logic.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_CompositeDecorator.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "PackageTools.h"
#include "Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardLoadedConsumerCompletion.h"
#include "Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardValueOrKeyCacheInvalidation.h"
#include "Provider/Blackboard/AssetDocumentAIBlackboardMutationInternal.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "StructUtils/StructView.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

#include <limits>

namespace AssetDocumentAIBlackboardConsumerCompletionTests
{
using namespace AssetDocumentAIBlackboardLoadedConsumerCompletion;
using namespace AssetDocumentAIBlackboardValueOrKeyCacheInvalidation;

constexpr TCHAR LoadedConsumerCompletionAuthorityId[] =
	TEXT("AD.AI.BLACKBOARD.LOADED-BT-CONSUMER-COMPLETION-v1");
constexpr TCHAR ValueOrCacheInvalidationAuthorityId[] =
	TEXT("AD.AI.BLACKBOARD.VALUE-OR-KEY-CACHE-INVALIDATION-v1");

const FName KeyBool(TEXT("KeyBool"));
const FName KeyClass(TEXT("KeyClass"));
const FName KeyEnum(TEXT("KeyEnum"));
const FName KeyFloat(TEXT("KeyFloat"));
const FName KeyInt(TEXT("KeyInt"));
const FName KeyName(TEXT("KeyName"));
const FName KeyString(TEXT("KeyString"));
const FName KeyObject(TEXT("KeyObject"));
const FName KeyRotator(TEXT("KeyRotator"));
const FName KeyStruct(TEXT("KeyStruct"));
const FName KeyVector(TEXT("KeyVector"));

struct FRegisteredAsset
{
	explicit FRegisteredAsset(UObject& InAsset) : Asset(&InAsset)
	{
		FAssetRegistryModule::AssetCreated(&InAsset);
	}

	~FRegisteredAsset()
	{
		if (UObject* Existing = Asset.Get())
		{
			FAssetRegistryModule::AssetDeleted(Existing);
		}
	}

	TWeakObjectPtr<UObject> Asset;
};

struct FScopedPackageFile
{
	explicit FScopedPackageFile(FString InFilename)
		: Filename(MoveTemp(InFilename))
	{
	}

	~FScopedPackageFile()
	{
		if (!Filename.IsEmpty())
		{
			IFileManager::Get().Delete(*Filename, false, true);
		}
	}

	FString Filename;
};

FAssetDocumentRuntimeValueDto CompletionProofString(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

FAssetDocumentRuntimeValueDto CompletionProofInteger(const int64 Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Integer;
	Result.IntegerValue = Value;
	return Result;
}

FAssetDocumentRuntimeValueDto CompletionProofNumber(const double Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Number;
	Result.NumberValue = Value;
	return Result;
}

FAssetDocumentRuntimeValueDto CompletionProofBoolean(const bool Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Boolean;
	Result.BooleanValue = Value;
	return Result;
}

FAssetDocumentRuntimeValueDto CompletionProofArray(TArray<FAssetDocumentRuntimeValueDto> Values)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Array;
	Result.Elements = MoveTemp(Values);
	return Result;
}

FAssetDocumentRuntimeValueDto CompletionProofObject(
	TArray<FString> Keys, TArray<FAssetDocumentRuntimeValueDto> Values)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Object;
	Result.ObjectKeys = MoveTemp(Keys);
	Result.Elements = MoveTemp(Values);
	return Result;
}

/** Local registry which retains the real editor-issued payload and only aliases
 * it into the provider-local AI slot.  It does not create an authored payload. */
class FCompletionProofHandleResolver final : public IAssetDocumentHandleResolver
{
public:
	struct FEntry
	{
		EAssetDocumentHandleKind Kind = EAssetDocumentHandleKind::Object;
		EAssetDocumentHandleAccess Access = EAssetDocumentHandleAccess::ReadOnly;
		FString ProviderId;
		uint32 Generation = 1;
		bool bInvalidated = false;
		TSharedPtr<IAssetDocumentNativeHandlePayload> Payload;
	};

	class FLease final : public IAssetDocumentVerifiedHandleLease
	{
	public:
		FLease(FAssetDocumentOpaqueHandle InHandle, IAssetDocumentNativeHandlePayload& InPayload)
			: Handle(MoveTemp(InHandle)), Payload(&InPayload) {}
		virtual const FAssetDocumentOpaqueHandle& GetHandle() const override { return Handle; }
		virtual IAssetDocumentNativeHandlePayload& GetPayloadChecked() const override
		{
			check(Payload != nullptr);
			return *Payload;
		}
	private:
		FAssetDocumentOpaqueHandle Handle;
		IAssetDocumentNativeHandlePayload* Payload = nullptr;
	};

	void SetMutationToken(const FAssetDocumentMutationToken& InToken) { Token = InToken; }

	virtual FAssetDocumentIssuedHandleResult Issue(EAssetDocumentHandleKind Kind,
		EAssetDocumentHandleAccess Access,
		TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload) override
	{
		FAssetDocumentIssuedHandleResult Result;
		if (!Payload || !Payload->IsAlive()) return Result;
		FEntry& Entry = Entries.AddDefaulted_GetRef();
		Entry.Kind = Kind;
		Entry.Access = Access;
		Entry.ProviderId = TEXT("editor");
		Entry.Payload = MakeShareable(Payload.Release());
		Result.Handle = MakeHandle(Entry, Entries.Num());
		return Result;
	}

	virtual FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(
		const FAssetDocumentOpaqueHandle& Handle,
		const FAssetDocumentHandleValidationContext& Context) override
	{
		FAssetDocumentVerifiedLeaseResult Result;
		const FAssetDocumentContractValidationResult Metadata =
			ValidateAssetDocumentHandleMetadata(Handle, Context);
		if (!Metadata.bValid)
		{
			Result.Diagnostics = Metadata.Diagnostics;
			return Result;
		}
		const int32 Index = static_cast<int32>(Handle.Slot) - 1;
		if (!Entries.IsValidIndex(Index)) return Result;
		FEntry& Entry = Entries[Index];
		if (Entry.Generation == Handle.Generation && Entry.Kind == Handle.Kind
			&& Entry.Access == Handle.Access && Entry.ProviderId == Handle.ProviderId
			&& !Entry.bInvalidated && Entry.Payload.IsValid() && Entry.Payload->IsAlive())
		{
			Result.Lease = MakeUnique<FLease>(Handle, *Entry.Payload);
		}
		return Result;
	}

	virtual FAssetDocumentHandleInvalidationResult Invalidate(
		const FAssetDocumentHandleInvalidation& Event) override
	{
		FAssetDocumentHandleInvalidationResult Result;
		for (FEntry& Entry : Entries)
		{
			if (!Entry.bInvalidated && Entry.ProviderId == Event.ProviderId
				&& Event.RequestId == Token.RequestId && Event.TransactionId == Token.TransactionId
				&& Event.RuntimeRevision == Token.RuntimeRevision)
			{
				Entry.bInvalidated = true;
				++Result.InvalidatedHandleCount;
			}
		}
		return Result;
	}

	TOptional<FAssetDocumentOpaqueHandle> AliasEditorAssetForAI(
		const FAssetDocumentOpaqueHandle& EditorHandle)
	{
		const int32 Index = static_cast<int32>(EditorHandle.Slot) - 1;
		if (!Entries.IsValidIndex(Index)) return {};
		const FEntry& Source = Entries[Index];
		if (EditorHandle.RequestId != Token.RequestId || EditorHandle.TransactionId != Token.TransactionId
			|| EditorHandle.RuntimeRevision != Token.RuntimeRevision
			|| EditorHandle.Kind != EAssetDocumentHandleKind::Asset
			|| EditorHandle.Access != EAssetDocumentHandleAccess::Mutable
			|| Source.ProviderId != TEXT("editor") || Source.Generation != EditorHandle.Generation
			|| !Source.Payload.IsValid() || !Source.Payload->IsAlive()) return {};
		FEntry& Target = Entries.AddDefaulted_GetRef();
		Target.Kind = Source.Kind;
		Target.Access = Source.Access;
		Target.ProviderId = TEXT("ai");
		Target.Payload = Source.Payload;
		return MakeHandle(Target, Entries.Num());
	}

private:
	FAssetDocumentOpaqueHandle MakeHandle(const FEntry& Entry, const int32 OneBasedSlot) const
	{
		FAssetDocumentOpaqueHandle Handle;
		Handle.RequestId = Token.RequestId;
		Handle.TransactionId = Token.TransactionId;
		Handle.RuntimeRevision = Token.RuntimeRevision;
		Handle.ProviderId = Entry.ProviderId;
		Handle.Slot = static_cast<uint32>(OneBasedSlot);
		Handle.Generation = Entry.Generation;
		Handle.Kind = Entry.Kind;
		Handle.Access = Entry.Access;
		return Handle;
	}

	FAssetDocumentMutationToken Token;
	TArray<FEntry> Entries;
};

class FCompletionProofNoCancellation final : public IAssetDocumentCancellationView
{
public:
	virtual bool IsCancellationRequested() const override { return false; }
};

FAssetDocumentPrimitiveInvocation MakeCompletionProofResolveInvocation(
	const UObject& Object, const FAssetDocumentMutationToken& Token)
{
	FAssetDocumentPrimitiveInvocation Invocation;
	Invocation.PrimitiveId = TEXT("editor.asset.resolve_existing.v1");
	Invocation.Version = 1;
	Invocation.RequestId = Token.RequestId;
	Invocation.TransactionId = Token.TransactionId;
	Invocation.RuntimeRevision = Token.RuntimeRevision;
	Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
	Invocation.ValueArguments = CompletionProofObject({TEXT("expected_class"), TEXT("path")},
		{CompletionProofString(Object.GetClass()->GetPathName()),
			CompletionProofString(Object.GetPathName())});
	return Invocation;
}

struct FCompletionProofRealAtomicScenario
{
	FCompletionProofHandleResolver Handles;
	TUniquePtr<FAssetDocumentRollbackActionComposition> Composition;
	TUniquePtr<IAssetDocumentAtomicTransaction> Transaction;
	FAssetDocumentOpaqueHandle AIHandle;

	bool Begin(FAutomationTestBase& Test, UBlackboardData& Blackboard,
		const FStringView RollbackComponentId = TEXTVIEW(
			"assetdocument.ai.blackboard.local_keys.v1"))
	{
		FAssetDocumentMutationFootprint Footprint;
		Footprint.Kind = EAssetDocumentMutationFootprintKind::UObjectState;
		Footprint.StableId = FString::Printf(TEXT("asset:%s/Keys[*]"), *Blackboard.GetPathName());
		Footprint.RollbackComponentId = FString(RollbackComponentId);
		FAssetDocumentPreflightReceipt Receipt;
		Receipt.RequestId = FGuid::NewGuid();
		Receipt.RuntimeRevision = 1;
		Receipt.CompleteFootprint = {Footprint};
		Receipt.ReceiptHash = TEXT("sha256:completion-proof-real-atomic");
		Receipt.bSucceeded = true;
		TArray<TSharedRef<IAssetDocumentRollbackAction>> InitialActions;
		Composition = MakeUnique<FAssetDocumentRollbackActionComposition>(
			Handles, MoveTemp(InitialActions),
			[](EAssetDocumentRuntimeRequestPhase, const FAssetDocumentRuntimeDiagnostic&) {});
		if (!Test.TestTrue(TEXT("completion proof uses a valid real rollback composition"),
			Composition->IsValid())) return false;
		FCompletionProofNoCancellation Cancellation;
		Transaction = Composition->BeginLiveAfterPreflight(Receipt, Cancellation);
		if (!Test.TestNotNull(TEXT("completion proof begins a live Atomic transaction"),
			Transaction.Get())) return false;
		Handles.SetMutationToken(Transaction->GetMutationToken());
		const FAssetDocumentPrimitiveOutcome EditorResult =
			CreateAssetDocumentEditorPrimitiveProvider()->Invoke(
				MakeCompletionProofResolveInvocation(Blackboard, Transaction->GetMutationToken()),
				Handles, nullptr);
		if (!Test.TestEqual(TEXT("completion proof resolves the real editor asset"),
			EditorResult.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded)) return false;
		const FAssetDocumentPrimitiveHandleResult* EditorHandle =
			EditorResult.HandleResults.FindByPredicate([](const FAssetDocumentPrimitiveHandleResult& Result)
			{
				return Result.Name == TEXT("asset_mutable") && Result.Handles.Num() == 1;
			});
		if (!Test.TestNotNull(TEXT("completion proof receives one real editor handle"), EditorHandle)) return false;
		const TOptional<FAssetDocumentOpaqueHandle> Alias =
			Handles.AliasEditorAssetForAI(EditorHandle->Handles[0]);
		if (!Test.TestTrue(TEXT("completion proof creates only a provider-local AI alias"), Alias.IsSet())) return false;
		AIHandle = Alias.GetValue();
		return true;
	}
};

struct FFixture
{
	// Declared first so the Registry registrations are removed before the
	// exact file owned by this fixture is deleted during reverse destruction.
	TUniquePtr<FScopedPackageFile> SavedPackageFile;
	TUniquePtr<FScopedPackageFile> SavedBlackboardPackageFile;
	TStrongObjectPtr<UPackage> Package;
	TStrongObjectPtr<UPackage> BlackboardPackage;
	TStrongObjectPtr<UBlackboardData> Blackboard;
	TStrongObjectPtr<UBehaviorTree> Tree;
	TStrongObjectPtr<UBehaviorTreeGraph> Graph;
	TStrongObjectPtr<UBehaviorTreeGraphNode_Root> Root;
	TStrongObjectPtr<UBehaviorTreeGraphNode_Composite> CompositeGraph;
	TStrongObjectPtr<UBehaviorTreeGraphNode_Task> TaskGraph;
	TStrongObjectPtr<UAssetDocumentAIBlackboardConsumerCompletionTestTask> Task;
	TArray<FBlackboardEntry> OriginalKeys;
	TUniquePtr<FRegisteredAsset> BlackboardRegistration;
	TUniquePtr<FRegisteredAsset> TreeRegistration;
};

template <typename TKeyType>
TKeyType* AddKey(UBlackboardData& Blackboard, const FName Name)
{
	FBlackboardEntry Entry;
	Entry.EntryName = Name;
	TKeyType* KeyType = NewObject<TKeyType>(&Blackboard);
	Entry.KeyType = KeyType;
	Blackboard.Keys.Add(MoveTemp(Entry));
	return KeyType;
}

UEdGraphPin* FindPin(const UEdGraphNode& Node, const EEdGraphPinDirection Direction)
{
	for (UEdGraphPin* Pin : Node.Pins)
	{
		if (Pin != nullptr && Pin->Direction == Direction) return Pin;
	}
	return nullptr;
}

void ConfigureSelector(FBlackboardKeySelector& Selector, UObject& Owner,
	const FName PropertyName, const FName Key, const EExactValueOrBlackboardKeyFamily Family)
{
	Selector.SelectedKeyName = Key;
	switch (Family)
	{
	case EExactValueOrBlackboardKeyFamily::Bool: Selector.AddBoolFilter(&Owner, PropertyName); break;
	case EExactValueOrBlackboardKeyFamily::Class: Selector.AddClassFilter(&Owner, PropertyName, UObject::StaticClass()); break;
	case EExactValueOrBlackboardKeyFamily::Enum: Selector.AddEnumFilter(&Owner, PropertyName, StaticEnum<EBasicKeyOperation::Type>()); break;
	case EExactValueOrBlackboardKeyFamily::Float: Selector.AddFloatFilter(&Owner, PropertyName); break;
	case EExactValueOrBlackboardKeyFamily::Int32: Selector.AddIntFilter(&Owner, PropertyName); break;
	case EExactValueOrBlackboardKeyFamily::Name: Selector.AddNameFilter(&Owner, PropertyName); break;
	case EExactValueOrBlackboardKeyFamily::String: Selector.AddStringFilter(&Owner, PropertyName); break;
	case EExactValueOrBlackboardKeyFamily::Object: Selector.AddObjectFilter(&Owner, PropertyName, UObject::StaticClass()); break;
	case EExactValueOrBlackboardKeyFamily::Rotator: Selector.AddRotatorFilter(&Owner, PropertyName); break;
	case EExactValueOrBlackboardKeyFamily::Struct: Selector.AddStructFilter<FVector>(&Owner, PropertyName); break;
	case EExactValueOrBlackboardKeyFamily::Vector: Selector.AddVectorFilter(&Owner, PropertyName); break;
	}
}

bool ConfigureTask(FAutomationTestBase& Test,
	UAssetDocumentAIBlackboardConsumerCompletionTestTask& Task)
{
	ConfigureSelector(Task.DirectSelector, Task,
		GET_MEMBER_NAME_CHECKED(UAssetDocumentAIBlackboardConsumerCompletionTestTask, DirectSelector),
		KeyInt, EExactValueOrBlackboardKeyFamily::Int32);
	Task.ValueOrBool = FValueOrBBKey_Bool(false); Task.ValueOrBool.SetKey(KeyBool);
	Task.ValueOrClass = FValueOrBBKey_Class(); Task.ValueOrClass.SetBaseClass(UObject::StaticClass()); Task.ValueOrClass.SetKey(KeyClass);
	Task.ValueOrEnum = FValueOrBBKey_Enum(); Task.ValueOrEnum.SetEnumType(StaticEnum<EBasicKeyOperation::Type>()); Task.ValueOrEnum.SetKey(KeyEnum);
	Task.ValueOrFloat = FValueOrBBKey_Float(1.25f); Task.ValueOrFloat.SetKey(KeyFloat);
	Task.ValueOrInt32 = FValueOrBBKey_Int32(37); Task.ValueOrInt32.SetKey(KeyInt);
	Task.ValueOrName = FValueOrBBKey_Name(TEXT("DefaultName")); Task.ValueOrName.SetKey(KeyName);
	Task.ValueOrString = FValueOrBBKey_String(TEXT("DefaultString")); Task.ValueOrString.SetKey(KeyString);
	Task.ValueOrObject = FValueOrBBKey_Object(); Task.ValueOrObject.SetBaseClass(UObject::StaticClass()); Task.ValueOrObject.SetKey(KeyObject);
	Task.ValueOrRotator = FValueOrBBKey_Rotator(FRotator(1.0, 2.0, 3.0)); Task.ValueOrRotator.SetKey(KeyRotator);
	Task.ValueOrStruct = FValueOrBBKey_Struct(FVector(2.0, 3.0, 5.0)); Task.ValueOrStruct.SetStructType(TBaseStructure<FVector>::Get()); Task.ValueOrStruct.SetKey(KeyStruct);
	Task.ValueOrVector = FValueOrBBKey_Vector(FVector(5.0, 8.0, 13.0)); Task.ValueOrVector.SetKey(KeyVector);

	ConfigureSelector(Task.Matrix.NestedSelector, Task,
		GET_MEMBER_NAME_CHECKED(FAssetDocumentAIBlackboardConsumerCompletionTestMatrix, NestedSelector),
		KeyBool, EExactValueOrBlackboardKeyFamily::Bool);
	Task.Matrix.NestedValueOrInt32 = FValueOrBBKey_Int32(71);
	Task.Matrix.NestedValueOrInt32.SetKey(KeyInt);
	Task.Matrix.NestedInstancedValue.InitializeAs<
		FAssetDocumentAIBlackboardConsumerCompletionInstancedTestValue>();
	auto& Instanced = Task.Matrix.NestedInstancedValue.GetMutable<
		FAssetDocumentAIBlackboardConsumerCompletionInstancedTestValue>();
	ConfigureSelector(Instanced.Selector, Task,
		GET_MEMBER_NAME_CHECKED(FAssetDocumentAIBlackboardConsumerCompletionInstancedTestValue, Selector),
		KeyName, EExactValueOrBlackboardKeyFamily::Name);
	Instanced.ValueOrName = FValueOrBBKey_Name(TEXT("InstancedDefault"));
	Instanced.ValueOrName.SetKey(KeyName);

	FBlackboardKeySelector ArrayA;
	ConfigureSelector(ArrayA, Task,
		GET_MEMBER_NAME_CHECKED(FAssetDocumentAIBlackboardConsumerCompletionTestMatrix, UniqueSelectors),
		KeyName, EExactValueOrBlackboardKeyFamily::Name);
	FBlackboardKeySelector ArrayB;
	ConfigureSelector(ArrayB, Task,
		GET_MEMBER_NAME_CHECKED(FAssetDocumentAIBlackboardConsumerCompletionTestMatrix, UniqueSelectors),
		KeyString, EExactValueOrBlackboardKeyFamily::String);
	Task.Matrix.UniqueSelectors = {MoveTemp(ArrayA), MoveTemp(ArrayB)};
	FBlackboardKeySelector MapSelector;
	ConfigureSelector(MapSelector, Task,
		GET_MEMBER_NAME_CHECKED(FAssetDocumentAIBlackboardConsumerCompletionTestMatrix, StableSelectorValues),
		KeyVector, EExactValueOrBlackboardKeyFamily::Vector);
	Task.Matrix.StableSelectorValues.Add(TEXT("Stable"), MoveTemp(MapSelector));
	Task.ConditionalNegativeContainer.InitializeAs<
		FAssetDocumentAIBlackboardConsumerCompletionEmptyContainer>();
	Task.CaptureBudgetStringA = FValueOrBBKey_String(TEXT("CaptureBudgetDefaultA"));
	Task.CaptureBudgetStringB = FValueOrBBKey_String(TEXT("CaptureBudgetDefaultB"));
	return Test.TestTrue(TEXT("instanced fixture uses the exact runtime struct"),
		Task.Matrix.NestedInstancedValue.GetScriptStruct()
			== FAssetDocumentAIBlackboardConsumerCompletionInstancedTestValue::StaticStruct());
}

TOptional<FFixture> MakeFixture(FAutomationTestBase& Test,
	const FString& RequestedAssetName = FString(), const bool bRetainSavedPackage = false,
	const bool bSeparateBlackboardPackage = false, const bool bPersistPackage = true)
{
	FFixture Fixture;
	const FString AssetName = RequestedAssetName.IsEmpty()
		? TEXT("BT_ConsumerCompletion_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)
		: RequestedAssetName;
	Fixture.Package.Reset(CreatePackage(*FString::Printf(
		TEXT("/Game/AssetDocumentTests/P_AI/%s"), *AssetName)));
	Fixture.BlackboardPackage.Reset(bSeparateBlackboardPackage
		? CreatePackage(*FString::Printf(TEXT("/Game/AssetDocumentTests/P_AI/%s_Blackboard"),
			*AssetName))
		: Fixture.Package.Get());
	Fixture.Blackboard.Reset(NewObject<UBlackboardData>(Fixture.BlackboardPackage.Get(),
		TEXT("BB_ConsumerCompletion"), RF_Public | RF_Standalone | RF_Transactional));
	Fixture.Tree.Reset(NewObject<UBehaviorTree>(Fixture.Package.Get(), *AssetName,
		RF_Public | RF_Standalone | RF_Transactional));
	Fixture.Graph.Reset(Cast<UBehaviorTreeGraph>(FBlueprintEditorUtils::CreateNewGraph(
		Fixture.Tree.Get(), TEXT("BehaviorTreeGraph"), UBehaviorTreeGraph::StaticClass(),
		UEdGraphSchema_BehaviorTree::StaticClass())));
	if (!Test.TestNotNull(TEXT("registered fixture package"), Fixture.Package.Get())
		|| !Test.TestNotNull(TEXT("registered fixture Blackboard package"),
			Fixture.BlackboardPackage.Get())
		|| !Test.TestNotNull(TEXT("registered fixture Blackboard"), Fixture.Blackboard.Get())
		|| !Test.TestNotNull(TEXT("registered fixture BehaviorTree"), Fixture.Tree.Get())
		|| !Test.TestNotNull(TEXT("registered fixture graph"), Fixture.Graph.Get())) return {};

	AddKey<UBlackboardKeyType_Int>(*Fixture.Blackboard, TEXT("BeforeReorder"));
	AddKey<UBlackboardKeyType_Bool>(*Fixture.Blackboard, KeyBool);
	UBlackboardKeyType_Class* ClassKey = AddKey<UBlackboardKeyType_Class>(*Fixture.Blackboard, KeyClass);
	UBlackboardKeyType_Enum* EnumKey = AddKey<UBlackboardKeyType_Enum>(*Fixture.Blackboard, KeyEnum);
	AddKey<UBlackboardKeyType_Float>(*Fixture.Blackboard, KeyFloat);
	AddKey<UBlackboardKeyType_Int>(*Fixture.Blackboard, KeyInt);
	AddKey<UBlackboardKeyType_Name>(*Fixture.Blackboard, KeyName);
	AddKey<UBlackboardKeyType_String>(*Fixture.Blackboard, KeyString);
	UBlackboardKeyType_Object* ObjectKey = AddKey<UBlackboardKeyType_Object>(*Fixture.Blackboard, KeyObject);
	AddKey<UBlackboardKeyType_Rotator>(*Fixture.Blackboard, KeyRotator);
	UBlackboardKeyType_Struct* StructKey =
		AddKey<UBlackboardKeyType_Struct>(*Fixture.Blackboard, KeyStruct);
	AddKey<UBlackboardKeyType_Vector>(*Fixture.Blackboard, KeyVector);
	if (!Test.TestNotNull(TEXT("class key policy"), ClassKey)
		|| !Test.TestNotNull(TEXT("enum key policy"), EnumKey)
		|| !Test.TestNotNull(TEXT("object key policy"), ObjectKey)
		|| !Test.TestNotNull(TEXT("struct key policy"), StructKey)) return {};
	ClassKey->BaseClass = UObject::StaticClass();
	EnumKey->EnumType = StaticEnum<EBasicKeyOperation::Type>();
	EnumKey->EnumName = EnumKey->EnumType->GetPathName();
	ObjectKey->BaseClass = UObject::StaticClass();
	StructKey->DefaultValue.InitializeAs<FVector>(FVector::ZeroVector);
	StructKey->PostLoad();
	Fixture.Blackboard->UpdateParentKeys();
	Fixture.Blackboard->UpdateKeyIDs();
	Fixture.OriginalKeys = Fixture.Blackboard->Keys;

	Fixture.Tree->BTGraph = Fixture.Graph.Get();
	Fixture.Tree->BlackboardAsset = Fixture.Blackboard.Get();
	Fixture.Graph->GetSchema()->CreateDefaultNodesForGraph(*Fixture.Graph);
	Fixture.Graph->OnCreated();
	Fixture.Graph->Initialize();
	Fixture.Root.Reset(Fixture.Graph->Nodes.Num() == 1
		? Cast<UBehaviorTreeGraphNode_Root>(Fixture.Graph->Nodes[0]) : nullptr);
	if (!Test.TestNotNull(TEXT("stock root node"), Fixture.Root.Get())) return {};
	Fixture.Root->BlackboardAsset = Fixture.Blackboard.Get();
	Fixture.Root->UpdateBlackboard();

	FAISchemaAction_NewNode CompositeAction;
	CompositeAction.NodeTemplate = NewObject<UBehaviorTreeGraphNode_Composite>();
	CompositeAction.NodeTemplate->NodeInstance =
		NewObject<UBTComposite_Sequence>(CompositeAction.NodeTemplate);
	UEdGraphPin* RootOutput = FindPin(*Fixture.Root, EGPD_Output);
	Fixture.CompositeGraph.Reset(Cast<UBehaviorTreeGraphNode_Composite>(
		CompositeAction.PerformAction(Fixture.Graph.Get(), RootOutput,
			FVector2f(160.0f, 0.0f), false)));
	FAISchemaAction_NewNode TaskAction;
	TaskAction.NodeTemplate = NewObject<UBehaviorTreeGraphNode_Task>();
	TaskAction.NodeTemplate->NodeInstance =
		NewObject<UAssetDocumentAIBlackboardConsumerCompletionTestTask>(TaskAction.NodeTemplate);
	UEdGraphPin* CompositeOutput = Fixture.CompositeGraph.IsValid()
		? FindPin(*Fixture.CompositeGraph, EGPD_Output) : nullptr;
	Fixture.TaskGraph.Reset(Cast<UBehaviorTreeGraphNode_Task>(
		TaskAction.PerformAction(Fixture.Graph.Get(), CompositeOutput,
			FVector2f(320.0f, 0.0f), false)));
	Fixture.Task.Reset(Fixture.TaskGraph.IsValid()
		? Cast<UAssetDocumentAIBlackboardConsumerCompletionTestTask>(
			Fixture.TaskGraph->NodeInstance) : nullptr);
	if (!Test.TestNotNull(TEXT("stock composite node"), Fixture.CompositeGraph.Get())
		|| !Test.TestNotNull(TEXT("typed completion task node"), Fixture.Task.Get())
		|| !ConfigureTask(Test, *Fixture.Task)) return {};
	Fixture.Graph->UpdateAsset(UBehaviorTreeGraph::KeepRebuildCounter);
	if (!Test.TestEqual(TEXT("task is owned by exact tree"),
		Fixture.Task->GetTreeAsset(), Fixture.Tree.Get())) return {};

	Fixture.BlackboardRegistration = MakeUnique<FRegisteredAsset>(*Fixture.Blackboard);
	Fixture.TreeRegistration = MakeUnique<FRegisteredAsset>(*Fixture.Tree);
	const FString PackageFilename = FPackageName::LongPackageNameToFilename(
		Fixture.Package->GetName(), FPackageName::GetAssetPackageExtension());
	const FString BlackboardPackageFilename = FPackageName::LongPackageNameToFilename(
		Fixture.BlackboardPackage->GetName(), FPackageName::GetAssetPackageExtension());
	if (bPersistPackage && !bRetainSavedPackage)
	{
		Fixture.SavedPackageFile = MakeUnique<FScopedPackageFile>(PackageFilename);
		if (bSeparateBlackboardPackage)
		{
			Fixture.SavedBlackboardPackageFile =
				MakeUnique<FScopedPackageFile>(BlackboardPackageFilename);
		}
	}
	Fixture.BlackboardPackage->MarkPackageDirty();
	Fixture.Package->MarkPackageDirty();
	if (bPersistPackage)
	{
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		if (bSeparateBlackboardPackage
			&& !Test.TestTrue(TEXT("Registry/no-load fixture saves its Blackboard asset"),
				UPackage::SavePackage(Fixture.BlackboardPackage.Get(), Fixture.Blackboard.Get(),
					*BlackboardPackageFilename, SaveArgs))) return {};
		if (!Test.TestTrue(TEXT("Registry/no-load fixture saves to the dependency graph"),
			UPackage::SavePackage(Fixture.Package.Get(), Fixture.Tree.Get(),
				*PackageFilename, SaveArgs))) return {};
		IAssetRegistry* Registry = IAssetRegistry::Get();
		if (!Test.TestNotNull(TEXT("saved fixture has Asset Registry authority"), Registry)) return {};
		Registry->ScanFilesSynchronous(bSeparateBlackboardPackage
			? TArray<FString>{BlackboardPackageFilename, PackageFilename}
			: TArray<FString>{PackageFilename}, true);
		Fixture.BlackboardPackage->SetDirtyFlag(false);
		Fixture.Package->SetDirtyFlag(false);
	}
	return Fixture;
}

UAssetDocumentAIBlackboardConsumerCompletionTestDecorator* AddCompositeDecoratorFixture(
	FAutomationTestBase& Test, FFixture& Fixture)
{
	FAISchemaAction_NewSubNode Action;
	Action.ParentNode = Fixture.TaskGraph.Get();
	Action.NodeTemplate = NewObject<UBehaviorTreeGraphNode_CompositeDecorator>();
	const int32 DecoratorIndex = Fixture.TaskGraph->Decorators.Num();
	Action.PerformAction(Fixture.Graph.Get(), nullptr, FVector2f::ZeroVector, false);
	UBehaviorTreeGraphNode_CompositeDecorator* Composite =
		Fixture.TaskGraph->Decorators.IsValidIndex(DecoratorIndex)
			? Cast<UBehaviorTreeGraphNode_CompositeDecorator>(
				Fixture.TaskGraph->Decorators[DecoratorIndex]) : nullptr;
	UBehaviorTreeDecoratorGraph* BoundGraph = Composite != nullptr
		? Cast<UBehaviorTreeDecoratorGraph>(Composite->GetBoundGraph()) : nullptr;
	if (!Test.TestNotNull(TEXT("stock composite decorator is attached"), Composite)
		|| !Test.TestNotNull(TEXT("stock composite decorator owns its bound graph"), BoundGraph))
	{
		return nullptr;
	}

	UBehaviorTreeDecoratorGraphNode_Logic* Sink = nullptr;
	for (UEdGraphNode* Node : BoundGraph->Nodes)
	{
		UBehaviorTreeDecoratorGraphNode_Logic* Logic =
			Cast<UBehaviorTreeDecoratorGraphNode_Logic>(Node);
		if (Logic != nullptr && Logic->LogicMode == EDecoratorLogicMode::Sink)
		{
			Sink = Logic;
			break;
		}
	}
	if (!Test.TestNotNull(TEXT("stock decorator graph owns its sink"), Sink)) return nullptr;

	FGraphNodeCreator<UBehaviorTreeDecoratorGraphNode_Decorator> Creator(*BoundGraph);
	UBehaviorTreeDecoratorGraphNode_Decorator* Leaf = Creator.CreateNode(false);
	Leaf->ClassData = FGraphNodeClassData(
		UAssetDocumentAIBlackboardConsumerCompletionTestDecorator::StaticClass(), FString());
	Creator.Finalize();
	auto* Decorator = Leaf != nullptr
		? Cast<UAssetDocumentAIBlackboardConsumerCompletionTestDecorator>(Leaf->NodeInstance)
		: nullptr;
	if (!Test.TestNotNull(TEXT("bound graph owns its typed decorator leaf"), Decorator))
	{
		return nullptr;
	}
	ConfigureSelector(Decorator->Selector, *Decorator,
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAIBlackboardConsumerCompletionTestDecorator, Selector),
		KeyInt, EExactValueOrBlackboardKeyFamily::Int32);
	UEdGraphPin* Output = FindPin(*Leaf, EGPD_Output);
	UEdGraphPin* Input = FindPin(*Sink, EGPD_Input);
	if (!Test.TestTrue(TEXT("bound decorator leaf connects to the stock sink"),
		Output != nullptr && Input != nullptr
			&& BoundGraph->GetSchema()->TryCreateConnection(Output, Input)))
	{
		return nullptr;
	}

	UEdGraphNode_Comment* Comment = NewObject<UEdGraphNode_Comment>(BoundGraph);
	Comment->CreateNewGuid();
	BoundGraph->AddNode(Comment, false, false);
	BoundGraph->NotifyGraphChanged();
	Composite->OnInnerGraphChanged();
	return Decorator;
}

UWorld* TestWorld(FAutomationTestBase& Test)
{
	UWorld* World = GEditor != nullptr ? GEditor->GetEditorWorldContext().World() : nullptr;
	Test.TestNotNull(TEXT("completion test has an Editor world"), World);
	return World;
}

TStrongObjectPtr<UBlackboardComponent> MakeComponent(
	FAutomationTestBase& Test, UWorld& World, UBlackboardData& Blackboard)
{
	TStrongObjectPtr<UBlackboardComponent> Component(NewObject<UBlackboardComponent>(&World));
	if (!Test.TestNotNull(TEXT("real Blackboard component"), Component.Get())
		|| !Test.TestTrue(TEXT("real Blackboard component initializes"),
			Component->InitializeBlackboard(Blackboard)))
	{
		return TStrongObjectPtr<UBlackboardComponent>();
	}
	return Component;
}

void PopulateRuntimeValues(UBlackboardComponent& Component)
{
	Component.SetValueAsBool(KeyBool, true);
	Component.SetValueAsClass(KeyClass, AActor::StaticClass());
	Component.SetValueAsEnum(KeyEnum, static_cast<uint8>(EBasicKeyOperation::Set));
	Component.SetValueAsFloat(KeyFloat, 42.5f);
	Component.SetValueAsInt(KeyInt, 4242);
	Component.SetValueAsName(KeyName, TEXT("RuntimeName"));
	Component.SetValueAsString(KeyString, TEXT("RuntimeString"));
	Component.SetValueAsObject(KeyObject, &Component);
	Component.SetValueAsRotator(KeyRotator, FRotator(9.0, 8.0, 7.0));
	FInstancedStruct StructValue;
	StructValue.InitializeAs<FVector>(FVector(21.0, 34.0, 55.0));
	Component.SetValue<UBlackboardKeyType_Struct>(KeyStruct, FConstStructView(StructValue));
	Component.SetValueAsVector(KeyVector, FVector(89.0, 144.0, 233.0));
}

void PrimeAllLeaves(UAssetDocumentAIBlackboardConsumerCompletionTestTask& Task,
	UBlackboardData& Blackboard, UBlackboardComponent& Component)
{
	Task.DirectSelector.ResolveSelectedKey(Blackboard);
	Task.Matrix.NestedSelector.ResolveSelectedKey(Blackboard);
	for (FBlackboardKeySelector& Selector : Task.Matrix.UniqueSelectors)
	{
		Selector.ResolveSelectedKey(Blackboard);
	}
	for (TPair<FName, FBlackboardKeySelector>& Pair : Task.Matrix.StableSelectorValues)
	{
		Pair.Value.ResolveSelectedKey(Blackboard);
	}
	auto& Instanced = Task.Matrix.NestedInstancedValue.GetMutable<
		FAssetDocumentAIBlackboardConsumerCompletionInstancedTestValue>();
	Instanced.Selector.ResolveSelectedKey(Blackboard);
	(void)Task.ValueOrBool.GetValue(&Component);
	(void)Task.ValueOrClass.GetValue(&Component);
	(void)Task.ValueOrEnum.GetValue(&Component);
	(void)Task.ValueOrFloat.GetValue(&Component);
	(void)Task.ValueOrInt32.GetValue(&Component);
	(void)Task.ValueOrName.GetValue(&Component);
	(void)Task.ValueOrString.GetValue(&Component);
	(void)Task.ValueOrObject.GetValue(&Component);
	(void)Task.ValueOrRotator.GetValue(&Component);
	(void)Task.ValueOrStruct.GetValue(&Component);
	(void)Task.ValueOrVector.GetValue(&Component);
	(void)Task.Matrix.NestedValueOrInt32.GetValue(&Component);
	(void)Instanced.ValueOrName.GetValue(&Component);
}

bool VerifyAllValueOrRuntimeValues(FAutomationTestBase& Test,
	UAssetDocumentAIBlackboardConsumerCompletionTestTask& Task,
	UBlackboardComponent& Component)
{
	bool bPassed = true;
	bPassed &= Test.TestTrue(TEXT("Bool resolves new mapping"), Task.ValueOrBool.GetValue(&Component));
	bPassed &= Test.TestEqual(TEXT("Class resolves new mapping"), Task.ValueOrClass.GetValue(&Component), AActor::StaticClass());
	bPassed &= Test.TestEqual(TEXT("Enum resolves new mapping"), Task.ValueOrEnum.GetValue(&Component), static_cast<uint8>(EBasicKeyOperation::Set));
	bPassed &= Test.TestEqual(TEXT("Float resolves new mapping"), Task.ValueOrFloat.GetValue(&Component), 42.5f);
	bPassed &= Test.TestEqual(TEXT("Int32 resolves new mapping"), Task.ValueOrInt32.GetValue(&Component), 4242);
	bPassed &= Test.TestEqual(TEXT("Name resolves new mapping"), Task.ValueOrName.GetValue(&Component), FName(TEXT("RuntimeName")));
	bPassed &= Test.TestEqual(TEXT("String resolves new mapping"), Task.ValueOrString.GetValue(&Component), FString(TEXT("RuntimeString")));
	bPassed &= Test.TestEqual(TEXT("Object resolves new mapping"), Task.ValueOrObject.GetValue(&Component), static_cast<UObject*>(&Component));
	bPassed &= Test.TestEqual(TEXT("Rotator resolves new mapping"), Task.ValueOrRotator.GetValue(&Component), FRotator(9.0, 8.0, 7.0));
	const FConstStructView StructValue = Task.ValueOrStruct.GetValue(&Component);
	bPassed &= Test.TestTrue(TEXT("Struct resolves exact FVector mapping"),
		StructValue.GetScriptStruct() == TBaseStructure<FVector>::Get()
		&& StructValue.GetMemory() != nullptr
		&& *reinterpret_cast<const FVector*>(StructValue.GetMemory())
			== FVector(21.0, 34.0, 55.0));
	bPassed &= Test.TestEqual(TEXT("Vector resolves new mapping"), Task.ValueOrVector.GetValue(&Component), FVector(89.0, 144.0, 233.0));
	return bPassed;
}

FName DeletedSlotDecoy(const FName OriginalName)
{
	return FName(*FString::Printf(TEXT("DeletedSlot_%s"), *OriginalName.ToString()));
}

void RenameAllBoundKeysToDeletedSlotDecoys(UBlackboardData& Blackboard)
{
	const FName BoundKeys[] = {
		KeyBool, KeyClass, KeyEnum, KeyFloat, KeyInt, KeyName, KeyString,
		KeyObject, KeyRotator, KeyStruct, KeyVector,
	};
	for (FBlackboardEntry& Entry : Blackboard.Keys)
	{
		for (const FName BoundKey : BoundKeys)
		{
			if (Entry.EntryName == BoundKey)
			{
				Entry.EntryName = DeletedSlotDecoy(BoundKey);
				break;
			}
		}
	}
	Blackboard.UpdateParentKeys();
	Blackboard.UpdateKeyIDs();
}

void PopulateDeletedSlotDecoyValues(UBlackboardComponent& Component)
{
	Component.SetValueAsBool(DeletedSlotDecoy(KeyBool), true);
	Component.SetValueAsClass(DeletedSlotDecoy(KeyClass), AActor::StaticClass());
	Component.SetValueAsEnum(DeletedSlotDecoy(KeyEnum), static_cast<uint8>(EBasicKeyOperation::Set));
	Component.SetValueAsFloat(DeletedSlotDecoy(KeyFloat), 42.5f);
	Component.SetValueAsInt(DeletedSlotDecoy(KeyInt), 4242);
	Component.SetValueAsName(DeletedSlotDecoy(KeyName), TEXT("DeletedSlotRuntimeName"));
	Component.SetValueAsString(DeletedSlotDecoy(KeyString), TEXT("DeletedSlotRuntimeString"));
	Component.SetValueAsObject(DeletedSlotDecoy(KeyObject), &Component);
	Component.SetValueAsRotator(DeletedSlotDecoy(KeyRotator), FRotator(9.0, 8.0, 7.0));
	FInstancedStruct StructValue;
	StructValue.InitializeAs<FVector>(FVector(21.0, 34.0, 55.0));
	Component.SetValue<UBlackboardKeyType_Struct>(DeletedSlotDecoy(KeyStruct),
		FConstStructView(StructValue));
	Component.SetValueAsVector(DeletedSlotDecoy(KeyVector), FVector(89.0, 144.0, 233.0));
}

bool VerifyAllValueOrAuthoredKeys(FAutomationTestBase& Test,
	UAssetDocumentAIBlackboardConsumerCompletionTestTask& Task)
{
	bool bPassed = true;
	bPassed &= Test.TestEqual(TEXT("deleted Bool retains authored key"), Task.ValueOrBool.GetKey(), KeyBool);
	bPassed &= Test.TestEqual(TEXT("deleted Class retains authored key"), Task.ValueOrClass.GetKey(), KeyClass);
	bPassed &= Test.TestEqual(TEXT("deleted Enum retains authored key"), Task.ValueOrEnum.GetKey(), KeyEnum);
	bPassed &= Test.TestEqual(TEXT("deleted Float retains authored key"), Task.ValueOrFloat.GetKey(), KeyFloat);
	bPassed &= Test.TestEqual(TEXT("deleted Int32 retains authored key"), Task.ValueOrInt32.GetKey(), KeyInt);
	bPassed &= Test.TestEqual(TEXT("deleted Name retains authored key"), Task.ValueOrName.GetKey(), KeyName);
	bPassed &= Test.TestEqual(TEXT("deleted String retains authored key"), Task.ValueOrString.GetKey(), KeyString);
	bPassed &= Test.TestEqual(TEXT("deleted Object retains authored key"), Task.ValueOrObject.GetKey(), KeyObject);
	bPassed &= Test.TestEqual(TEXT("deleted Rotator retains authored key"), Task.ValueOrRotator.GetKey(), KeyRotator);
	bPassed &= Test.TestEqual(TEXT("deleted Struct retains authored key"), Task.ValueOrStruct.GetKey(), KeyStruct);
	bPassed &= Test.TestEqual(TEXT("deleted Vector retains authored key"), Task.ValueOrVector.GetKey(), KeyVector);
	return bPassed;
}

bool VerifyAllValueOrDeletedDefaults(FAutomationTestBase& Test,
	UAssetDocumentAIBlackboardConsumerCompletionTestTask& Task,
	UBlackboardComponent& Component)
{
	bool bPassed = VerifyAllValueOrAuthoredKeys(Test, Task);
	bPassed &= Test.TestFalse(TEXT("deleted Bool does not read its old type-compatible slot"),
		Task.ValueOrBool.GetValue(&Component));
	bPassed &= Test.TestEqual(TEXT("deleted Class returns authored default"),
		Task.ValueOrClass.GetValue(&Component), static_cast<UClass*>(nullptr));
	bPassed &= Test.TestEqual(TEXT("deleted Enum returns authored default"),
		Task.ValueOrEnum.GetValue(&Component), static_cast<uint8>(0));
	bPassed &= Test.TestEqual(TEXT("deleted Float returns authored default"),
		Task.ValueOrFloat.GetValue(&Component), 1.25f);
	bPassed &= Test.TestEqual(TEXT("deleted Int32 returns authored default"),
		Task.ValueOrInt32.GetValue(&Component), 37);
	bPassed &= Test.TestEqual(TEXT("deleted Name returns authored default"),
		Task.ValueOrName.GetValue(&Component), FName(TEXT("DefaultName")));
	bPassed &= Test.TestEqual(TEXT("deleted String returns authored default"),
		Task.ValueOrString.GetValue(&Component), FString(TEXT("DefaultString")));
	bPassed &= Test.TestEqual(TEXT("deleted Object returns authored default"),
		Task.ValueOrObject.GetValue(&Component), static_cast<UObject*>(nullptr));
	bPassed &= Test.TestEqual(TEXT("deleted Rotator returns authored default"),
		Task.ValueOrRotator.GetValue(&Component), FRotator(1.0, 2.0, 3.0));
	const FConstStructView StructDefault = Task.ValueOrStruct.GetValue(&Component);
	bPassed &= Test.TestTrue(TEXT("deleted Struct retains its authored default/type policy"),
		StructDefault.GetScriptStruct() == TBaseStructure<FVector>::Get()
		&& StructDefault.GetMemory() != nullptr
		&& *reinterpret_cast<const FVector*>(StructDefault.GetMemory()) == FVector(2.0, 3.0, 5.0));
	bPassed &= Test.TestEqual(TEXT("deleted Vector returns authored default"),
		Task.ValueOrVector.GetValue(&Component), FVector(5.0, 8.0, 13.0));
	return bPassed;
}

bool VerifySupportedValueOrClosure(FAutomationTestBase& Test,
	UAssetDocumentAIBlackboardConsumerCompletionTestTask& Task,
	UBlackboardComponent& Component)
{
	bool bPassed = VerifyAllValueOrAuthoredKeys(Test, Task);
	bPassed &= Test.TestFalse(TEXT("missing Bool retains authored default"), Task.ValueOrBool.GetValue(static_cast<const UBlackboardComponent*>(nullptr)));
	bPassed &= Test.TestEqual(TEXT("missing Class retains authored default"), Task.ValueOrClass.GetValue(static_cast<const UBlackboardComponent*>(nullptr)), static_cast<UClass*>(nullptr));
	bPassed &= Test.TestEqual(TEXT("missing Enum retains authored default"), Task.ValueOrEnum.GetValue(static_cast<const UBlackboardComponent*>(nullptr)), static_cast<uint8>(0));
	bPassed &= Test.TestEqual(TEXT("missing Float retains authored default"), Task.ValueOrFloat.GetValue(static_cast<const UBlackboardComponent*>(nullptr)), 1.25f);
	bPassed &= Test.TestEqual(TEXT("supported Int32 resolves restored mapping"), Task.ValueOrInt32.GetValue(&Component), 4242);
	bPassed &= Test.TestEqual(TEXT("missing Name retains authored default"), Task.ValueOrName.GetValue(static_cast<const UBlackboardComponent*>(nullptr)), FName(TEXT("DefaultName")));
	bPassed &= Test.TestEqual(TEXT("missing String retains authored default"), Task.ValueOrString.GetValue(static_cast<const UBlackboardComponent*>(nullptr)), FString(TEXT("DefaultString")));
	bPassed &= Test.TestEqual(TEXT("missing Object retains authored default"), Task.ValueOrObject.GetValue(static_cast<const UBlackboardComponent*>(nullptr)), static_cast<UObject*>(nullptr));
	bPassed &= Test.TestEqual(TEXT("missing Rotator retains authored default"), Task.ValueOrRotator.GetValue(static_cast<const UBlackboardComponent*>(nullptr)), FRotator(1.0, 2.0, 3.0));
	const FConstStructView StructDefault = Task.ValueOrStruct.GetValue(
		static_cast<const UBlackboardComponent*>(nullptr));
	bPassed &= Test.TestTrue(TEXT("missing Struct retains authored default/type"),
		StructDefault.GetScriptStruct() == TBaseStructure<FVector>::Get()
		&& StructDefault.GetMemory() != nullptr
		&& *reinterpret_cast<const FVector*>(StructDefault.GetMemory()) == FVector(2.0, 3.0, 5.0));
	bPassed &= Test.TestEqual(TEXT("missing Vector retains authored default"),
		Task.ValueOrVector.GetValue(static_cast<const UBlackboardComponent*>(nullptr)),
		FVector(5.0, 8.0, 13.0));
	return bPassed;
}

void RestoreOriginalKeys(FFixture& Fixture)
{
	Fixture.Blackboard->Keys = Fixture.OriginalKeys;
	Fixture.Blackboard->UpdateParentKeys();
	Fixture.Blackboard->UpdateKeyIDs();
}

void RotateKeys(FFixture& Fixture)
{
	FBlackboardEntry First = MoveTemp(Fixture.Blackboard->Keys[0]);
	Fixture.Blackboard->Keys.RemoveAt(0);
	Fixture.Blackboard->Keys.Add(MoveTemp(First));
	Fixture.Blackboard->UpdateParentKeys();
	Fixture.Blackboard->UpdateKeyIDs();
}

bool CompletionProofRuntimeValueEquals(const FAssetDocumentRuntimeValueDto& Left,
	const FAssetDocumentRuntimeValueDto& Right)
{
	if (Left.Kind != Right.Kind || Left.BooleanValue != Right.BooleanValue
		|| Left.IntegerValue != Right.IntegerValue || Left.NumberValue != Right.NumberValue
		|| Left.StringValue != Right.StringValue || Left.BytesValue != Right.BytesValue
		|| Left.ObjectKeys != Right.ObjectKeys || Left.Elements.Num() != Right.Elements.Num()) return false;
	for (int32 Index = 0; Index < Left.Elements.Num(); ++Index)
	{
		if (!CompletionProofRuntimeValueEquals(Left.Elements[Index], Right.Elements[Index])) return false;
	}
	return true;
}

FAssetDocumentRuntimeValueDto MakeCompletionProofExpectedV1Value(
	const FAssetDocumentPrimitiveInvocation& Invocation)
{
	const TArray<FString> ExpectedKeys = {TEXT("blackboard_path"), TEXT("ordered_keys")};
	check(Invocation.ValueArguments.Kind == EAssetDocumentRuntimeValueKind::Object
		&& Invocation.ValueArguments.ObjectKeys == ExpectedKeys
		&& Invocation.ValueArguments.Elements.Num() == 2);
	return CompletionProofObject(
		{TEXT("inherited_key_names"), TEXT("ordered_keys")},
		{CompletionProofArray({}), Invocation.ValueArguments.Elements[1]});
}

bool RequireCompletionProofSuccess(FAutomationTestBase& Test, const FString& Label,
	const FAssetDocumentPrimitiveOutcome& Outcome,
	const FAssetDocumentRuntimeValueDto& ExpectedValue,
	const FString& BlackboardPath)
{
	const FAssetDocumentChangedRecord* const Change = Outcome.ChangedSet.Num() == 1
		? &Outcome.ChangedSet[0] : nullptr;
	return Test.TestEqual(*(Label + TEXT(" returns Succeeded")), Outcome.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
		&& Test.TestTrue(*(Label + TEXT(" freezes the complete recursive Value DTO")),
			CompletionProofRuntimeValueEquals(Outcome.Value, ExpectedValue))
		&& Test.TestTrue(*(Label + TEXT(" keeps diagnostics byte-empty")),
			Outcome.Diagnostics.IsEmpty())
		&& Test.TestTrue(*(Label + TEXT(" keeps handle results byte-empty")),
			Outcome.HandleResults.IsEmpty())
		&& Test.TestNotNull(*(Label + TEXT(" freezes one legacy ChangedSet row")), Change)
		&& (Change == nullptr || (Test.TestEqual(*(Label + TEXT(" freezes authority ID")),
			Change->AuthorityId, FString(TEXT("ai.blackboard.local_keys.replace.v1")))
			&& Test.TestEqual(*(Label + TEXT(" freezes stable object ID")),
				Change->StableObjectId, BlackboardPath)
			&& Test.TestEqual(*(Label + TEXT(" freezes changed path")), Change->Path,
				FString::Printf(TEXT("asset:%s/Keys[*]"), *BlackboardPath))
			&& Test.TestEqual(*(Label + TEXT(" freezes change kind")),
				Change->ChangeKind, FString(TEXT("Update")))));
}

bool MakeCompletionProofLocalKeyRow(FAutomationTestBase& Test, FBlackboardEntry& Entry,
	FAssetDocumentRuntimeValueDto& OutRow)
{
	FAssetDocumentRuntimeValueDto Properties;
	if (!Test.TestNotNull(*FString::Printf(TEXT("completion proof key %s has a type"),
		*Entry.EntryName.ToString()), Entry.KeyType.Get())) return false;
	if (!Test.TestTrue(*FString::Printf(TEXT("completion proof key %s has canonical properties"),
		*Entry.EntryName.ToString()), AssetDocumentAIBlackboardMutation::
			BuildAssetDocumentAIBlackboardCanonicalKeyTypeProperties(*Entry.KeyType, Properties)))
	{
		return false;
	}
	OutRow = CompletionProofObject(
		{TEXT("canonical_properties"), TEXT("key_type_class_path"), TEXT("name")},
		{MoveTemp(Properties), CompletionProofString(Entry.KeyType->GetClass()->GetPathName()),
			CompletionProofString(Entry.EntryName.ToString())});
	return true;
}

bool MakeCompletionProofLocalKeysReplaceInvocation(FAutomationTestBase& Test,
	const FCompletionProofRealAtomicScenario& Scenario, UBlackboardData& Blackboard,
	const bool bReverse, FAssetDocumentPrimitiveInvocation& OutInvocation)
{
	TArray<FAssetDocumentRuntimeValueDto> Ordered;
	for (int32 Offset = 0; Offset < Blackboard.Keys.Num(); ++Offset)
	{
		const int32 Index = bReverse ? Blackboard.Keys.Num() - Offset - 1 : Offset;
		if (!AssetDocumentAIBlackboardMutation::IsEngineDerivedSelfKey(Blackboard.Keys[Index]))
		{
			FAssetDocumentRuntimeValueDto Row;
			if (!MakeCompletionProofLocalKeyRow(Test, Blackboard.Keys[Index], Row)) return false;
			Ordered.Add(MoveTemp(Row));
		}
	}
	OutInvocation.PrimitiveId = TEXT("ai.blackboard.local_keys.replace.v1");
	OutInvocation.Version = 1;
	OutInvocation.RequestId = Scenario.AIHandle.RequestId;
	OutInvocation.TransactionId = Scenario.AIHandle.TransactionId;
	OutInvocation.RuntimeRevision = Scenario.AIHandle.RuntimeRevision;
	OutInvocation.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
	OutInvocation.HandleArguments = {{TEXT("asset"), Scenario.AIHandle}};
	OutInvocation.ValueArguments = CompletionProofObject(
		{TEXT("blackboard_path"), TEXT("ordered_keys")},
		{CompletionProofString(Blackboard.GetPathName()), CompletionProofArray(MoveTemp(Ordered))});
	return true;
}

FAssetDocumentRuntimeValueDto MakeStructuralCallerEntry(const FString& Name,
	const UClass& KeyTypeClass, FAssetDocumentRuntimeValueDto CanonicalProperties)
{
	return CompletionProofObject(
		{TEXT("Category"), TEXT("Description"), TEXT("KeyTypeClass"),
			TEXT("KeyTypeProperties"), TEXT("Name"), TEXT("bInstanceSynced")},
		{CompletionProofString(TEXT("CompletionProof")),
			CompletionProofString(TEXT("Loaded-consumer caller proof")),
			CompletionProofObject({TEXT("Kind"), TEXT("Path")},
				{CompletionProofString(TEXT("ClassRef")),
					CompletionProofString(KeyTypeClass.GetPathName())}),
			MoveTemp(CanonicalProperties), CompletionProofString(Name),
			CompletionProofBoolean(false)});
}

FAssetDocumentPrimitiveInvocation MakeStructuralCallerInvocation(
	const FCompletionProofRealAtomicScenario& Scenario, const UBlackboardData& Blackboard,
	const bool bAdd, FAssetDocumentRuntimeValueDto Entry)
{
	FAssetDocumentPrimitiveInvocation Invocation;
	Invocation.PrimitiveId = bAdd
		? TEXT("ai.blackboard.entry.add.v2")
		: TEXT("ai.blackboard.entry.replace_class.v2");
	Invocation.Version = 2;
	Invocation.RequestId = Scenario.AIHandle.RequestId;
	Invocation.TransactionId = Scenario.AIHandle.TransactionId;
	Invocation.RuntimeRevision = Scenario.AIHandle.RuntimeRevision;
	Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
	Invocation.HandleArguments = {{TEXT("asset"), Scenario.AIHandle}};
	if (bAdd)
	{
		Invocation.ValueArguments = CompletionProofObject(
			{TEXT("blackboard_path"), TEXT("entry"), TEXT("insertion")},
			{CompletionProofString(Blackboard.GetPathName()), MoveTemp(Entry),
				CompletionProofObject({TEXT("Index"), TEXT("Kind")},
					{CompletionProofInteger(0), CompletionProofString(TEXT("AtIndex"))})});
	}
	else
	{
		Invocation.ValueArguments = CompletionProofObject(
			{TEXT("blackboard_path"), TEXT("entry"), TEXT("key_name")},
			{CompletionProofString(Blackboard.GetPathName()), MoveTemp(Entry),
				CompletionProofString(TEXT("BeforeReorder"))});
	}
	return Invocation;
}

TArray<FName> CompletionProofKeyOrder(const UBlackboardData& Blackboard)
{
	TArray<FName> Names;
	for (const FBlackboardEntry& Entry : Blackboard.Keys) Names.Add(Entry.EntryName);
	return Names;
}

bool RequireCompletionProofFailure(FAutomationTestBase& Test, const FString& Label,
	const FAssetDocumentPrimitiveOutcome& Outcome, const FString& ExpectedCode,
	const FString& ExpectedMessage)
{
	const FAssetDocumentRuntimeDiagnostic* const Terminal = Outcome.Diagnostics.Num() == 1
		? &Outcome.Diagnostics[0] : nullptr;
	return Test.TestEqual(*(Label + TEXT(" returns Failed")), Outcome.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Failed)
		&& Test.TestNotNull(*(Label + TEXT(" returns exactly one existing terminal")), Terminal)
		&& (Terminal == nullptr || Test.TestEqual(*(Label + TEXT(" preserves exact terminal")),
			Terminal->Code, ExpectedCode))
		&& (Terminal == nullptr || Test.TestEqual(*(Label + TEXT(" preserves terminal path")),
			Terminal->Path, FString(TEXT("/Primitive"))))
		&& (Terminal == nullptr || Test.TestEqual(*(Label + TEXT(" preserves terminal message")),
			Terminal->Message, ExpectedMessage))
		&& (Terminal == nullptr || Test.TestEqual(*(Label + TEXT(" preserves terminal severity")),
			Terminal->Severity, EAssetDocumentDiagnosticSeverity::Error))
		&& (Terminal == nullptr || Test.TestTrue(*(Label + TEXT(" preserves empty terminal context")),
			Terminal->Context.IsEmpty()))
		&& Test.TestEqual(*(Label + TEXT(" preserves legacy empty-object Value kind")),
			Outcome.Value.Kind, EAssetDocumentRuntimeValueKind::Object)
		&& Test.TestTrue(*(Label + TEXT(" keeps Value payload byte-empty")),
			Outcome.Value.Elements.IsEmpty() && Outcome.Value.ObjectKeys.IsEmpty()
				&& Outcome.Value.StringValue.IsEmpty() && Outcome.Value.BytesValue.IsEmpty())
		&& Test.TestTrue(*(Label + TEXT(" emits no handles")), Outcome.HandleResults.IsEmpty())
		&& Test.TestTrue(*(Label + TEXT(" emits no ChangedSet")), Outcome.ChangedSet.IsEmpty());
}

void ReduceToSupportedEntryKeys(FFixture& Fixture)
{
	Fixture.Blackboard->Keys.RemoveAll([](const FBlackboardEntry& Entry)
	{
		return Entry.EntryName != TEXT("BeforeReorder") && Entry.EntryName != KeyInt;
	});
	Fixture.Blackboard->UpdateParentKeys();
	Fixture.Blackboard->UpdateKeyIDs();
}

bool RunCompletionFailureRow(FAutomationTestBase& Test, FFixture& Fixture,
	const ELoadedConsumerCompletionTestFailurePoint Point)
{
	RestoreOriginalKeys(Fixture);
	auto Captured = CaptureLoadedBehaviorTreeConsumerReceipt(*Fixture.Blackboard);
	if (!Test.TestTrue(TEXT("failure row captures receipt"), Captured.HasValue())) return false;
	auto Receipt = Captured.StealValue();
	RotateKeys(Fixture);
	SetLoadedConsumerCompletionTestFailurePoint(Point);
	bool bPassed = Test.TestEqual(TEXT("injected forward completion fails closed"),
		CompleteLoadedBehaviorTreeConsumerReceipt(Receipt, *Fixture.Blackboard,
			ELoadedConsumerCompletionPhase::ForwardPublished),
		ELoadedConsumerCompletionResult::DerivedRefreshFailed);
	RestoreOriginalKeys(Fixture);
	bPassed &= Test.TestEqual(TEXT("same receipt completes restored rollback"),
		CompleteLoadedBehaviorTreeConsumerReceipt(Receipt, *Fixture.Blackboard,
			ELoadedConsumerCompletionPhase::RollbackRestored),
		ELoadedConsumerCompletionResult::Succeeded);
	return bPassed;
}

bool VerifyDirectValueOrOwner(FAutomationTestBase& Test,
	UAssetDocumentAIBlackboardConsumerCompletionTestTask& Task)
{
	bool bPassed = true;
#define VERIFY_VALUE_OR(Type, Field) \
	{ FResolvedExactValueOrLeaf Leaf(Task.Field, *Type::StaticStruct()); \
	  bPassed &= Test.TestEqual(TEXT(#Field " direct invalidation succeeds"), \
		InvalidateExactValueOrBlackboardKeyCache(Leaf), EExactValueOrKeyCacheInvalidationResult::Success); }
	VERIFY_VALUE_OR(FValueOrBBKey_Bool, ValueOrBool)
	VERIFY_VALUE_OR(FValueOrBBKey_Class, ValueOrClass)
	VERIFY_VALUE_OR(FValueOrBBKey_Enum, ValueOrEnum)
	VERIFY_VALUE_OR(FValueOrBBKey_Float, ValueOrFloat)
	VERIFY_VALUE_OR(FValueOrBBKey_Int32, ValueOrInt32)
	VERIFY_VALUE_OR(FValueOrBBKey_Name, ValueOrName)
	VERIFY_VALUE_OR(FValueOrBBKey_String, ValueOrString)
	VERIFY_VALUE_OR(FValueOrBBKey_Object, ValueOrObject)
	VERIFY_VALUE_OR(FValueOrBBKey_Rotator, ValueOrRotator)
	VERIFY_VALUE_OR(FValueOrBBKey_Struct, ValueOrStruct)
	VERIFY_VALUE_OR(FValueOrBBKey_Vector, ValueOrVector)
#undef VERIFY_VALUE_OR
	return bPassed;
}

bool VerifyValueOrInvalidationFailureSeams(FAutomationTestBase& Test,
	UAssetDocumentAIBlackboardConsumerCompletionTestTask& Task)
{
	bool bPassed = true;
#define VERIFY_VALUE_OR_SEAMS(Type, Field, Before, After) \
	{ \
		const EValueOrKeyCacheInvalidationTestFailurePoint Points[] = { \
			EValueOrKeyCacheInvalidationTestFailurePoint::Before, \
			EValueOrKeyCacheInvalidationTestFailurePoint::After, \
			EValueOrKeyCacheInvalidationTestFailurePoint::BeforeFinalAssignment }; \
		for (const EValueOrKeyCacheInvalidationTestFailurePoint Point : Points) \
		{ \
			SetValueOrKeyCacheInvalidationTestFailurePoint(Point); \
			FResolvedExactValueOrLeaf Leaf(Task.Field, *Type::StaticStruct()); \
			bPassed &= Test.TestEqual(*FString::Printf(TEXT(#Field " failure seam %d rejects"), \
				static_cast<int32>(Point)), InvalidateExactValueOrBlackboardKeyCache(Leaf), \
				EExactValueOrKeyCacheInvalidationResult::TestFailure); \
			SetValueOrKeyCacheInvalidationTestFailurePoint( \
				EValueOrKeyCacheInvalidationTestFailurePoint::None); \
		} \
	}
	VERIFY_VALUE_OR_SEAMS(FValueOrBBKey_Bool, ValueOrBool, BeforeBool, AfterBool)
	VERIFY_VALUE_OR_SEAMS(FValueOrBBKey_Class, ValueOrClass, BeforeClass, AfterClass)
	VERIFY_VALUE_OR_SEAMS(FValueOrBBKey_Enum, ValueOrEnum, BeforeEnum, AfterEnum)
	VERIFY_VALUE_OR_SEAMS(FValueOrBBKey_Float, ValueOrFloat, BeforeFloat, AfterFloat)
	VERIFY_VALUE_OR_SEAMS(FValueOrBBKey_Int32, ValueOrInt32, BeforeInt32, AfterInt32)
	VERIFY_VALUE_OR_SEAMS(FValueOrBBKey_Name, ValueOrName, BeforeName, AfterName)
	VERIFY_VALUE_OR_SEAMS(FValueOrBBKey_String, ValueOrString, BeforeString, AfterString)
	VERIFY_VALUE_OR_SEAMS(FValueOrBBKey_Object, ValueOrObject, BeforeObject, AfterObject)
	VERIFY_VALUE_OR_SEAMS(FValueOrBBKey_Rotator, ValueOrRotator, BeforeRotator, AfterRotator)
	VERIFY_VALUE_OR_SEAMS(FValueOrBBKey_Struct, ValueOrStruct, BeforeStruct, AfterStruct)
	VERIFY_VALUE_OR_SEAMS(FValueOrBBKey_Vector, ValueOrVector, BeforeVector, AfterVector)
#undef VERIFY_VALUE_OR_SEAMS
	return bPassed;
}

bool VerifyEmptyStructValueOrInvalidation(FAutomationTestBase& Test,
	UAssetDocumentAIBlackboardConsumerCompletionTestTask& Task)
{
	FResolvedExactValueOrLeaf Leaf(Task.EmptyDefaultValueOrStruct,
		*FValueOrBBKey_Struct::StaticStruct());
	const EExactValueOrKeyCacheInvalidationResult Result =
		InvalidateExactValueOrBlackboardKeyCache(Leaf);
	const FConstStructView EmptyDefault =
		Task.EmptyDefaultValueOrStruct.GetValue(static_cast<const UBlackboardComponent*>(nullptr));
	return Test.TestEqual(TEXT("default empty Struct ValueOr invalidation succeeds"), Result,
		EExactValueOrKeyCacheInvalidationResult::Success)
		&& Test.TestTrue(TEXT("default empty Struct ValueOr preserves the absent authored key"),
			Task.EmptyDefaultValueOrStruct.GetKey().IsNone())
		&& Test.TestTrue(TEXT("default empty Struct ValueOr preserves the absent authored default"),
			EmptyDefault.GetScriptStruct() == nullptr && EmptyDefault.GetMemory() == nullptr);
}

bool RequireCodecDigest(FAutomationTestBase& Test,
	FAssetDocumentAIBlackboardConsumerCompletionCodecScalars& Values,
	const FName PropertyName, const FString& ExpectedDigest)
{
	const FProperty* const Property = FindFProperty<FProperty>(
		FAssetDocumentAIBlackboardConsumerCompletionCodecScalars::StaticStruct(), PropertyName);
	if (!Test.TestNotNull(*FString::Printf(TEXT("codec property %s exists"), *PropertyName.ToString()),
		Property)) return false;
	FString ActualDigest;
	return Test.TestTrue(*FString::Printf(TEXT("codec vector %s encodes"), *PropertyName.ToString()),
		Private::EncodeBlackboardBTLeafLocatorDigestV1(*Property,
			Property->ContainerPtrToValuePtr<void>(&Values), ActualDigest))
		&& Test.TestEqual(*FString::Printf(TEXT("codec vector %s is independently frozen"),
			*PropertyName.ToString()), ActualDigest, ExpectedDigest);
}

} // namespace AssetDocumentAIBlackboardConsumerCompletionTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAIBlackboardConsumerCompletionTest,
	"UEAssetDoc.AIProvider.BlackboardConsumerCompletion.Core",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardConsumerCompletionTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBlackboardConsumerCompletionTests;
	TestEqual(TEXT("loaded-consumer authority ID"), FString(LoadedConsumerCompletionAuthorityId),
		FString(AssetDocumentAIBlackboardLoadedConsumerCompletion::AuthorityId));
	TestEqual(TEXT("ValueOr authority ID"), FString(ValueOrCacheInvalidationAuthorityId),
		FString(AssetDocumentAIBlackboardValueOrKeyCacheInvalidation::AuthorityId));
	TOptional<FFixture> OptionalFixture = MakeFixture(*this);
	if (!OptionalFixture.IsSet()) return false;
	FFixture& Fixture = OptionalFixture.GetValue();
	UWorld* World = TestWorld(*this);
	if (World == nullptr) return false;

	TStrongObjectPtr<UBlackboardComponent> Priming =
		MakeComponent(*this, *World, *Fixture.Blackboard);
	if (!Priming.IsValid()) return false;
	PopulateRuntimeValues(*Priming);
	PrimeAllLeaves(*Fixture.Task, *Fixture.Blackboard, *Priming);
	const FBlackboard::FKey PrimedId = Fixture.Task->DirectSelector.GetSelectedKeyID();
	if (!TestTrue(TEXT("selector is genuinely pre-primed"),
		PrimedId != FBlackboard::InvalidKey)) return false;
	IAssetRegistry* Registry = IAssetRegistry::Get();
	TArray<FName> FixtureReferencers;
	if (!TestNotNull(TEXT("fixture has Asset Registry authority"), Registry)
		|| !TestTrue(TEXT("fixture package has an on-disk referencer query node"),
			Registry->GetReferencers(Fixture.Blackboard->GetOutermost()->GetFName(),
				FixtureReferencers))) return false;
	auto ReorderCapture = CaptureLoadedBehaviorTreeConsumerReceipt(*Fixture.Blackboard);
	if (!TestTrue(TEXT("paired Registry/no-load capture succeeds"),
		ReorderCapture.HasValue())) return false;
	auto ReorderReceipt = ReorderCapture.StealValue();
	RotateKeys(Fixture);
	TestNotEqual(TEXT("retained name moves to a new key ID"),
		Fixture.Blackboard->GetKeyID(KeyInt), PrimedId);
	const ELoadedConsumerCompletionResult ReorderCompletion =
		CompleteLoadedBehaviorTreeConsumerReceipt(ReorderReceipt, *Fixture.Blackboard,
			ELoadedConsumerCompletionPhase::ForwardPublished);
	if (!TestEqual(TEXT("forward completion succeeds"), ReorderCompletion,
		ELoadedConsumerCompletionResult::Succeeded)) return false;
	TestEqual(TEXT("selector resolves retained name at new ID"),
		Fixture.Task->DirectSelector.GetSelectedKeyID(), Fixture.Blackboard->GetKeyID(KeyInt));
	TStrongObjectPtr<UBlackboardComponent> Reordered =
		MakeComponent(*this, *World, *Fixture.Blackboard);
	if (!Reordered.IsValid()) return false;
	PopulateRuntimeValues(*Reordered);
	if (!VerifyAllValueOrRuntimeValues(*this, *Fixture.Task, *Reordered)
		|| !VerifyDirectValueOrOwner(*this, *Fixture.Task)
		|| !VerifyValueOrInvalidationFailureSeams(*this, *Fixture.Task)) return false;
	RestoreOriginalKeys(Fixture);
	if (!TestEqual(TEXT("same receipt restores rollback caches"),
		CompleteLoadedBehaviorTreeConsumerReceipt(ReorderReceipt, *Fixture.Blackboard,
			ELoadedConsumerCompletionPhase::RollbackRestored),
		ELoadedConsumerCompletionResult::Succeeded)) return false;

	auto DeleteCapture = CaptureLoadedBehaviorTreeConsumerReceipt(*Fixture.Blackboard);
	if (!TestTrue(TEXT("delete row captures receipt"), DeleteCapture.HasValue())) return false;
	auto DeleteReceipt = DeleteCapture.StealValue();
	Fixture.Blackboard->Keys.SetNum(1);
	Fixture.Blackboard->UpdateParentKeys();
	Fixture.Blackboard->UpdateKeyIDs();
	AddExpectedError(TEXT("Failed to find key"),
		EAutomationExpectedErrorFlags::Contains, 6);
	if (!TestEqual(TEXT("deleted-key completion succeeds"),
		CompleteLoadedBehaviorTreeConsumerReceipt(DeleteReceipt, *Fixture.Blackboard,
			ELoadedConsumerCompletionPhase::ForwardPublished),
		ELoadedConsumerCompletionResult::Succeeded)) return false;
	TestEqual(TEXT("deleted selector is invalid"), Fixture.Task->DirectSelector.GetSelectedKeyID(),
		FBlackboard::InvalidKey);
	TStrongObjectPtr<UBlackboardComponent> Deleted =
		MakeComponent(*this, *World, *Fixture.Blackboard);
	if (!Deleted.IsValid()) return false;
	TestEqual(TEXT("deleted Int32 returns authored default"),
		Fixture.Task->ValueOrInt32.GetValue(Deleted.Get()), 37);
	TestEqual(TEXT("deleted Vector returns authored default"),
		Fixture.Task->ValueOrVector.GetValue(Deleted.Get()), FVector(5.0, 8.0, 13.0));
	RestoreOriginalKeys(Fixture);
	if (!TestEqual(TEXT("delete row same receipt restores rollback"),
		CompleteLoadedBehaviorTreeConsumerReceipt(DeleteReceipt, *Fixture.Blackboard,
			ELoadedConsumerCompletionPhase::RollbackRestored),
		ELoadedConsumerCompletionResult::Succeeded)) return false;

	auto DeletedSlotCapture = CaptureLoadedBehaviorTreeConsumerReceipt(*Fixture.Blackboard);
	if (!TestTrue(TEXT("type-compatible deleted-slot row captures receipt"),
		DeletedSlotCapture.HasValue())) return false;
	auto DeletedSlotReceipt = DeletedSlotCapture.StealValue();
	RenameAllBoundKeysToDeletedSlotDecoys(*Fixture.Blackboard);
	AddExpectedError(TEXT("Failed to find key"), EAutomationExpectedErrorFlags::Contains, 0);
	if (!TestEqual(TEXT("type-compatible deleted-slot completion succeeds"),
		CompleteLoadedBehaviorTreeConsumerReceipt(DeletedSlotReceipt, *Fixture.Blackboard,
			ELoadedConsumerCompletionPhase::ForwardPublished),
		ELoadedConsumerCompletionResult::Succeeded)) return false;
	TStrongObjectPtr<UBlackboardComponent> DeletedSlot =
		MakeComponent(*this, *World, *Fixture.Blackboard);
	if (!DeletedSlot.IsValid()) return false;
	PopulateDeletedSlotDecoyValues(*DeletedSlot);
	if (!VerifyAllValueOrDeletedDefaults(*this, *Fixture.Task, *DeletedSlot)) return false;
	RestoreOriginalKeys(Fixture);
	if (!TestEqual(TEXT("type-compatible deleted-slot rollback restores same receipt"),
		CompleteLoadedBehaviorTreeConsumerReceipt(DeletedSlotReceipt, *Fixture.Blackboard,
			ELoadedConsumerCompletionPhase::RollbackRestored),
		ELoadedConsumerCompletionResult::Succeeded)) return false;

	const ELoadedConsumerCompletionTestFailurePoint CompletionFailures[] = {
		ELoadedConsumerCompletionTestFailurePoint::BeforeGraph,
		ELoadedConsumerCompletionTestFailurePoint::AfterGraph,
		ELoadedConsumerCompletionTestFailurePoint::BeforeSelector,
		ELoadedConsumerCompletionTestFailurePoint::AfterSelector,
		ELoadedConsumerCompletionTestFailurePoint::BeforeBool,
		ELoadedConsumerCompletionTestFailurePoint::AfterBool,
		ELoadedConsumerCompletionTestFailurePoint::BeforeClass,
		ELoadedConsumerCompletionTestFailurePoint::AfterClass,
		ELoadedConsumerCompletionTestFailurePoint::BeforeEnum,
		ELoadedConsumerCompletionTestFailurePoint::AfterEnum,
		ELoadedConsumerCompletionTestFailurePoint::BeforeFloat,
		ELoadedConsumerCompletionTestFailurePoint::AfterFloat,
		ELoadedConsumerCompletionTestFailurePoint::BeforeInt32,
		ELoadedConsumerCompletionTestFailurePoint::AfterInt32,
		ELoadedConsumerCompletionTestFailurePoint::BeforeName,
		ELoadedConsumerCompletionTestFailurePoint::AfterName,
		ELoadedConsumerCompletionTestFailurePoint::BeforeString,
		ELoadedConsumerCompletionTestFailurePoint::AfterString,
		ELoadedConsumerCompletionTestFailurePoint::BeforeObject,
		ELoadedConsumerCompletionTestFailurePoint::AfterObject,
		ELoadedConsumerCompletionTestFailurePoint::BeforeRotator,
		ELoadedConsumerCompletionTestFailurePoint::AfterRotator,
		ELoadedConsumerCompletionTestFailurePoint::BeforeStruct,
		ELoadedConsumerCompletionTestFailurePoint::AfterStruct,
		ELoadedConsumerCompletionTestFailurePoint::BeforeVector,
		ELoadedConsumerCompletionTestFailurePoint::AfterVector,
		ELoadedConsumerCompletionTestFailurePoint::BeforeVerification,
		ELoadedConsumerCompletionTestFailurePoint::AfterVerification,
	};
	for (const ELoadedConsumerCompletionTestFailurePoint Point : CompletionFailures)
	{
		if (!RunCompletionFailureRow(*this, Fixture, Point)) return false;
	}

	const ELoadedConsumerCompletionTestFailurePoint CaptureFailures[] = {
		ELoadedConsumerCompletionTestFailurePoint::RegistryBeforeReadiness,
		ELoadedConsumerCompletionTestFailurePoint::RegistrySnapshotA,
		ELoadedConsumerCompletionTestFailurePoint::RegistryReferencers,
		ELoadedConsumerCompletionTestFailurePoint::RegistrySnapshotB,
		ELoadedConsumerCompletionTestFailurePoint::RegistryAfterReadiness,
	};
	for (const ELoadedConsumerCompletionTestFailurePoint Point : CaptureFailures)
	{
		SetLoadedConsumerCompletionTestFailurePoint(Point);
		auto Rejected = CaptureLoadedBehaviorTreeConsumerReceipt(*Fixture.Blackboard);
		if (!TestFalse(TEXT("Registry failure rejects capture"), Rejected.HasValue())) return false;
	}

	Fixture.Task->ConditionalNegativeContainer.InitializeAs<
		FAssetDocumentAIBlackboardConsumerCompletionExcludedSelector>();
	auto DefaultExcludedRejected = CaptureLoadedBehaviorTreeConsumerReceipt(*Fixture.Blackboard);
	TestFalse(TEXT("default excluded selector is rejected"), DefaultExcludedRejected.HasValue());
	Fixture.Task->ConditionalNegativeContainer.InitializeAs<
		FAssetDocumentAIBlackboardConsumerCompletionExcludedValueOr>();
	auto DefaultValueOrRejected = CaptureLoadedBehaviorTreeConsumerReceipt(*Fixture.Blackboard);
	TestFalse(TEXT("default excluded ValueOr is rejected"), DefaultValueOrRejected.HasValue());
	auto& ExcludedValueOr = Fixture.Task->ConditionalNegativeContainer.GetMutable<
		FAssetDocumentAIBlackboardConsumerCompletionExcludedValueOr>();
	ExcludedValueOr.ValueOrInt32 = FValueOrBBKey_Int32(911);
	ExcludedValueOr.ValueOrInt32.SetKey(KeyInt);
	auto KeyedValueOrRejected = CaptureLoadedBehaviorTreeConsumerReceipt(*Fixture.Blackboard);
	TestFalse(TEXT("keyed excluded ValueOr is rejected"), KeyedValueOrRejected.HasValue());
	Fixture.Task->ConditionalNegativeContainer.InitializeAs<
		FAssetDocumentAIBlackboardConsumerCompletionExcludedMapKeyContainer>();
	auto& ExcludedMap = Fixture.Task->ConditionalNegativeContainer.GetMutable<
		FAssetDocumentAIBlackboardConsumerCompletionExcludedMapKeyContainer>();
	ExcludedMap.LeafKeys.Add(
		FAssetDocumentAIBlackboardConsumerCompletionExcludedMapKey(), 0);
	auto ExcludedMapKeyRejected = CaptureLoadedBehaviorTreeConsumerReceipt(*Fixture.Blackboard);
	TestFalse(TEXT("excluded map-key selector is rejected"), ExcludedMapKeyRejected.HasValue());
	Fixture.Task->ConditionalNegativeContainer.InitializeAs<
		FAssetDocumentAIBlackboardConsumerCompletionEmptyContainer>();
	TStrongObjectPtr<UBlackboardComponent> TransientReference(
		NewObject<UBlackboardComponent>(GetTransientPackage(), NAME_None, RF_Transient));
	Fixture.Task->RejectedTraversalReference = TransientReference.Get();
	auto ReferenceRejected = CaptureLoadedBehaviorTreeConsumerReceipt(*Fixture.Blackboard);
	TestFalse(TEXT("transient object traversal reference is rejected"), ReferenceRejected.HasValue());
	Fixture.Task->RejectedTraversalReference = nullptr;
	Fixture.Task->Matrix.UniqueSelectors[1] = Fixture.Task->Matrix.UniqueSelectors[0];
	auto DuplicateRejected = CaptureLoadedBehaviorTreeConsumerReceipt(*Fixture.Blackboard);
	TestFalse(TEXT("duplicate array authored digest is rejected"), DuplicateRejected.HasValue());
	ConfigureSelector(Fixture.Task->Matrix.UniqueSelectors[1], *Fixture.Task,
		GET_MEMBER_NAME_CHECKED(FAssetDocumentAIBlackboardConsumerCompletionTestMatrix,
			UniqueSelectors), KeyString, EExactValueOrBlackboardKeyFamily::String);
	auto OriginalTaskNodeInstance = Fixture.TaskGraph->NodeInstance;
	Fixture.TaskGraph->NodeInstance = nullptr;
	auto NullTaskNodeRejected = CaptureLoadedBehaviorTreeConsumerReceipt(*Fixture.Blackboard);
	TestFalse(TEXT("non-root task graph node without NodeInstance is rejected"),
		NullTaskNodeRejected.HasValue());
	Fixture.TaskGraph->NodeInstance = OriginalTaskNodeInstance;

	Fixture.Task->ConditionalNegativeContainer.InitializeAs<
		FAssetDocumentAIBlackboardConsumerCompletionUnsupportedContainer>();
	auto& UnsupportedSet = Fixture.Task->ConditionalNegativeContainer.GetMutable<
		FAssetDocumentAIBlackboardConsumerCompletionUnsupportedContainer>();
	UnsupportedSet.UnsupportedSet.Add(TEXT("Unordered"));
	auto SetRejected = CaptureLoadedBehaviorTreeConsumerReceipt(*Fixture.Blackboard);
	TestFalse(TEXT("set-shaped locator container is rejected"), SetRejected.HasValue());
	Fixture.Task->ConditionalNegativeContainer.InitializeAs<
		FAssetDocumentAIBlackboardConsumerCompletionObjectPathMapContainer>();
	auto& ObjectPathMap = Fixture.Task->ConditionalNegativeContainer.GetMutable<
		FAssetDocumentAIBlackboardConsumerCompletionObjectPathMapContainer>();
	ObjectPathMap.ObjectPathKeyedSelectors.Add(FSoftObjectPath(Fixture.Blackboard.Get()), {});
	auto ObjectPathMapRejected = CaptureLoadedBehaviorTreeConsumerReceipt(*Fixture.Blackboard);
	TestFalse(TEXT("object-path map-key locator container is rejected"),
		ObjectPathMapRejected.HasValue());
	Fixture.Task->ConditionalNegativeContainer.InitializeAs<
		FAssetDocumentAIBlackboardConsumerCompletionEmptyContainer>();

	TOptional<FFixture> OptionalFieldBudgetFixture = MakeFixture(*this);
	if (!OptionalFieldBudgetFixture.IsSet()) return false;
	FFixture& FieldBudgetFixture = OptionalFieldBudgetFixture.GetValue();
	int32 FieldsPerTaskValue = 0;
	for (TFieldIterator<FProperty> It(
		UAssetDocumentAIBlackboardConsumerCompletionTestTask::StaticClass()); It; ++It)
	{
		++FieldsPerTaskValue;
	}
	if (!TestTrue(TEXT("field-budget probe node is individually within the per-value limit"),
		FieldsPerTaskValue > 0 && FieldsPerTaskValue <= Private::LocatorMaximumFields())) return false;
	const int32 TotalTaskValues = Private::LocatorMaximumFields() / FieldsPerTaskValue + 2;
	for (int32 Index = 1; Index < TotalTaskValues; ++Index)
	{
		UBehaviorTreeGraphNode_Task* ExtraNode = NewObject<UBehaviorTreeGraphNode_Task>(
			FieldBudgetFixture.Graph.Get());
		ExtraNode->CreateNewGuid();
		UAssetDocumentAIBlackboardConsumerCompletionTestTask* ExtraTask =
			NewObject<UAssetDocumentAIBlackboardConsumerCompletionTestTask>(ExtraNode);
		ExtraNode->NodeInstance = ExtraTask;
		FieldBudgetFixture.Graph->AddNode(ExtraNode, false, false);
		if (!TestNotNull(TEXT("field-budget probe adds a typed node value"), ExtraTask)
			|| !ConfigureTask(*this, *ExtraTask)) return false;
	}
	auto PerValueFieldBudgetCapture = CaptureLoadedBehaviorTreeConsumerReceipt(
		*FieldBudgetFixture.Blackboard);
	TestTrue(TEXT("multiple individually valid node values do not share the 256-field budget"),
		PerValueFieldBudgetCapture.HasValue());

	const int32 IndividuallyValidBytes = Private::LocatorMaximumBytes() / 2;
	Fixture.Task->CaptureBudgetStringA = FValueOrBBKey_String(
		FString::ChrN(IndividuallyValidBytes, TEXT('a')));
	Fixture.Task->CaptureBudgetStringB = FValueOrBBKey_String(
		FString::ChrN(IndividuallyValidBytes, TEXT('b')));
	const FProperty* CaptureBudgetA = FindFProperty<FProperty>(
		UAssetDocumentAIBlackboardConsumerCompletionTestTask::StaticClass(),
		GET_MEMBER_NAME_CHECKED(UAssetDocumentAIBlackboardConsumerCompletionTestTask,
			CaptureBudgetStringA));
	const FProperty* CaptureBudgetB = FindFProperty<FProperty>(
		UAssetDocumentAIBlackboardConsumerCompletionTestTask::StaticClass(),
		GET_MEMBER_NAME_CHECKED(UAssetDocumentAIBlackboardConsumerCompletionTestTask,
			CaptureBudgetStringB));
	FString IndividuallyValidDigest;
	if (!TestNotNull(TEXT("first aggregate-byte leaf property exists"), CaptureBudgetA)
		|| !TestNotNull(TEXT("second aggregate-byte leaf property exists"), CaptureBudgetB)) return false;
	TestTrue(TEXT("first aggregate-byte leaf remains individually encodable"),
		Private::EncodeLeafAuthoredFingerprint(*CaptureBudgetA,
			CaptureBudgetA->ContainerPtrToValuePtr<void>(Fixture.Task.Get()), IndividuallyValidDigest));
	TestTrue(TEXT("second aggregate-byte leaf remains individually encodable"),
		Private::EncodeLeafAuthoredFingerprint(*CaptureBudgetB,
			CaptureBudgetB->ContainerPtrToValuePtr<void>(Fixture.Task.Get()), IndividuallyValidDigest));
	auto AggregateByteBudgetRejected = CaptureLoadedBehaviorTreeConsumerReceipt(*Fixture.Blackboard);
	TestFalse(TEXT("two individually valid leaves exceeding the receipt byte budget are rejected"),
		AggregateByteBudgetRejected.HasValue());
	Fixture.Task->CaptureBudgetStringA = FValueOrBBKey_String(TEXT("CaptureBudgetDefaultA"));
	Fixture.Task->CaptureBudgetStringB = FValueOrBBKey_String(TEXT("CaptureBudgetDefaultB"));
	VerifyEmptyStructValueOrInvalidation(*this, *Fixture.Task);

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAIBlackboardConsumerCompletionCompositeBoundGraphTest,
	"UEAssetDoc.AIProvider.BlackboardConsumerCompletion.CompositeBoundGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardConsumerCompletionCompositeBoundGraphTest::RunTest(
	const FString&)
{
	using namespace AssetDocumentAIBlackboardConsumerCompletionTests;
	TOptional<FFixture> OptionalFixture = MakeFixture(*this);
	if (!OptionalFixture.IsSet()) return false;
	FFixture& Fixture = OptionalFixture.GetValue();
	UAssetDocumentAIBlackboardConsumerCompletionTestDecorator* Decorator =
		AddCompositeDecoratorFixture(*this, Fixture);
	if (Decorator == nullptr) return false;
	Decorator->Selector.ResolveSelectedKey(*Fixture.Blackboard);
	const FBlackboard::FKey OriginalId = Decorator->Selector.GetSelectedKeyID();
	if (!TestTrue(TEXT("bound decorator selector is genuinely pre-primed"),
		OriginalId != FBlackboard::InvalidKey)) return false;

	auto Capture = CaptureLoadedBehaviorTreeConsumerReceipt(*Fixture.Blackboard);
	if (!TestTrue(TEXT("stock composite bound graph captures"), Capture.HasValue())) return false;
	auto Receipt = Capture.StealValue();
	RotateKeys(Fixture);
	if (!TestEqual(TEXT("bound decorator forward completion succeeds"),
		CompleteLoadedBehaviorTreeConsumerReceipt(Receipt, *Fixture.Blackboard,
			ELoadedConsumerCompletionPhase::ForwardPublished),
		ELoadedConsumerCompletionResult::Succeeded)) return false;
	TestEqual(TEXT("bound decorator selector follows the retained key name"),
		Decorator->Selector.GetSelectedKeyID(), Fixture.Blackboard->GetKeyID(KeyInt));

	RestoreOriginalKeys(Fixture);
	if (!TestEqual(TEXT("bound decorator rollback uses the same receipt"),
		CompleteLoadedBehaviorTreeConsumerReceipt(Receipt, *Fixture.Blackboard,
			ELoadedConsumerCompletionPhase::RollbackRestored),
		ELoadedConsumerCompletionResult::Succeeded)) return false;
	TestEqual(TEXT("bound decorator rollback restores the original selector ID"),
		Decorator->Selector.GetSelectedKeyID(), OriginalId);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAIBlackboardConsumerCompletionRegistryCodecProofTest,
	"UEAssetDoc.AIProvider.LoadedBlackboardConsumerRegistryCodecProof",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardConsumerCompletionRegistryCodecProofTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBlackboardConsumerCompletionTests;

	// This is intentionally separate from BlackboardConsumerCompletion: the
	// latter owns the exhaustive eleven-family invalidation matrix.  This cell
	// freezes receipt identity/no-load and codec behavior at the caller boundary.
	FAssetDocumentAIBlackboardConsumerCompletionCodecScalars Values;
	Values.EndianInt = 0x01020304;
	Values.CaseAndNumber = FName(TEXT("CaSe"), 7);
	Values.Utf8Text = TEXT("h\u00E9llo");
	Values.NegativeZero = -0.0f;
	bool Passed = true;
	Passed &= RequireCodecDigest(*this, Values, TEXT("EndianInt"),
		TEXT("c3f5927cdc7600da3961734abcd59568e8e00c2405751443eb20090785c76d26"));
	Passed &= RequireCodecDigest(*this, Values, TEXT("CaseAndNumber"),
		TEXT("94e0b39af85193dc66757bb52b69feab8c6fee39d0cdc5b6479ff9de40059ec1"));
	Passed &= RequireCodecDigest(*this, Values, TEXT("Utf8Text"),
		TEXT("0cae2eda4a9984e73ac9a583ef6b07b0fc5ae61e3507a1c28106992894754942"));
	Passed &= RequireCodecDigest(*this, Values, TEXT("NegativeZero"),
		TEXT("c360d29872a3d70d5f06fb2a993bef0b58904fbaf07813f590bbd0cd2aa222d6"));

	const FProperty* const NonFinite = FindFProperty<FProperty>(
		FAssetDocumentAIBlackboardConsumerCompletionCodecScalars::StaticStruct(), TEXT("NonFinite"));
	Values.NonFinite = std::numeric_limits<double>::infinity();
	FString RejectedDigest;
	Passed &= TestNotNull(TEXT("non-finite codec property exists"), NonFinite);
	if (NonFinite != nullptr)
	{
		Passed &= TestFalse(TEXT("non-finite locator values fail closed"),
			Private::EncodeBlackboardBTLeafLocatorDigestV1(*NonFinite,
				NonFinite->ContainerPtrToValuePtr<void>(&Values), RejectedDigest));
	}

	const FProperty* const MapProperty = FindFProperty<FProperty>(
		FAssetDocumentAIBlackboardConsumerCompletionCodecScalars::StaticStruct(), TEXT("StableValues"));
	Values.StableValues = {{FName(TEXT("First")), 1}, {FName(TEXT("Second")), 2}};
	FString FirstMapDigest;
	Passed &= TestNotNull(TEXT("map codec property exists"), MapProperty);
	if (MapProperty != nullptr)
	{
		Passed &= TestTrue(TEXT("stable-key map encodes"),
			Private::EncodeBlackboardBTLeafLocatorDigestV1(*MapProperty,
				MapProperty->ContainerPtrToValuePtr<void>(&Values), FirstMapDigest));
		Values.StableValues.Empty();
		Values.StableValues.Add(FName(TEXT("Second")), 2);
		Values.StableValues.Add(FName(TEXT("First")), 1);
		FString ReorderedMapDigest;
		Passed &= TestTrue(TEXT("map insertion reorder encodes"),
			Private::EncodeBlackboardBTLeafLocatorDigestV1(*MapProperty,
				MapProperty->ContainerPtrToValuePtr<void>(&Values), ReorderedMapDigest));
		Passed &= TestEqual(TEXT("map digest is insertion-order independent"),
			ReorderedMapDigest, FirstMapDigest);
	}

	const FProperty* const ArrayProperty = FindFProperty<FProperty>(
		FAssetDocumentAIBlackboardConsumerCompletionCodecScalars::StaticStruct(), TEXT("OrderedValues"));
	Values.OrderedValues.Init(7, Private::LocatorMaximumElements() + 1);
	FString BudgetDigest;
	Passed &= TestNotNull(TEXT("array codec property exists"), ArrayProperty);
	if (ArrayProperty != nullptr)
	{
		Passed &= TestFalse(TEXT("element budget rejects without truncation"),
			Private::EncodeBlackboardBTLeafLocatorDigestV1(*ArrayProperty,
				ArrayProperty->ContainerPtrToValuePtr<void>(&Values), BudgetDigest));
	}
	Values.Utf8Text = FString::ChrN(Private::LocatorMaximumBytes() + 1, TEXT('x'));
	const FProperty* const Utf8Property = FindFProperty<FProperty>(
		FAssetDocumentAIBlackboardConsumerCompletionCodecScalars::StaticStruct(), TEXT("Utf8Text"));
	Passed &= TestNotNull(TEXT("UTF-8 codec property exists"), Utf8Property);
	if (Utf8Property != nullptr)
	{
		Passed &= TestFalse(TEXT("byte budget rejects without truncation"),
			Private::EncodeBlackboardBTLeafLocatorDigestV1(*Utf8Property,
				Utf8Property->ContainerPtrToValuePtr<void>(&Values), BudgetDigest));
	}
	FAssetDocumentAIBlackboardConsumerCompletionCodecBudgetEnvelope BudgetEnvelope;
	FInstancedStruct* RecursiveSlot = &BudgetEnvelope.RecursiveValue;
	for (int32 Index = 0; Index <= Private::LocatorMaximumDepth(); ++Index)
	{
		RecursiveSlot->InitializeAs<
			FAssetDocumentAIBlackboardConsumerCompletionCodecRecursiveValue>();
		RecursiveSlot = &RecursiveSlot->GetMutable<
			FAssetDocumentAIBlackboardConsumerCompletionCodecRecursiveValue>().Next;
	}
	const FProperty* const RecursiveProperty = FindFProperty<FProperty>(
		FAssetDocumentAIBlackboardConsumerCompletionCodecBudgetEnvelope::StaticStruct(), TEXT("RecursiveValue"));
	BudgetEnvelope.FieldBudgetValues.SetNum(Private::LocatorMaximumFields() + 1);
	const FProperty* const FieldsProperty = FindFProperty<FProperty>(
		FAssetDocumentAIBlackboardConsumerCompletionCodecBudgetEnvelope::StaticStruct(), TEXT("FieldBudgetValues"));
	Passed &= TestNotNull(TEXT("depth-budget codec property exists"), RecursiveProperty);
	Passed &= TestNotNull(TEXT("field-budget codec property exists"), FieldsProperty);
	if (RecursiveProperty != nullptr)
	{
		Passed &= TestFalse(TEXT("nesting depth budget rejects without truncation"),
			Private::EncodeBlackboardBTLeafLocatorDigestV1(*RecursiveProperty,
				RecursiveProperty->ContainerPtrToValuePtr<void>(&BudgetEnvelope), BudgetDigest));
	}
	if (FieldsProperty != nullptr)
	{
		Passed &= TestFalse(TEXT("reflected-field budget rejects without truncation"),
			Private::EncodeBlackboardBTLeafLocatorDigestV1(*FieldsProperty,
				FieldsProperty->ContainerPtrToValuePtr<void>(&BudgetEnvelope), BudgetDigest));
	}
	Passed &= TestEqual(TEXT("frozen maximum nesting depth"), Private::LocatorMaximumDepth(), 8);
	Passed &= TestEqual(TEXT("frozen maximum reflected fields"), Private::LocatorMaximumFields(), 256);
	Passed &= TestEqual(TEXT("frozen maximum elements"), Private::LocatorMaximumElements(), 1024);
	Passed &= TestEqual(TEXT("frozen maximum bytes"), Private::LocatorMaximumBytes(), 1024 * 1024);

	UPackage* const UnregisteredPackage = CreatePackage(*FString::Printf(
		TEXT("/Temp/AssetDocumentAI/ConsumerCompletion_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	TStrongObjectPtr<UPackage> UnregisteredPackageGuard(UnregisteredPackage);
	TStrongObjectPtr<UBlackboardData> UnregisteredTarget(NewObject<UBlackboardData>(
		UnregisteredPackage, TEXT("Blackboard"), RF_Public | RF_Standalone | RF_Transactional));
	if (!TestNotNull(TEXT("unregistered target probe exists"), UnregisteredTarget.Get())) return false;
	UnregisteredTarget->UpdateParentKeys();
	UnregisteredTarget->UpdateKeyIDs();
	UnregisteredPackage->SetDirtyFlag(false);
	IAssetRegistry* const UnregisteredRegistry = IAssetRegistry::Get();
	Passed &= TestNotNull(TEXT("unregistered target probe has Registry authority"),
		UnregisteredRegistry);
	if (UnregisteredRegistry != nullptr)
	{
		Passed &= TestTrue(TEXT("unregistered target probe sees complete Registry search"),
			UnregisteredRegistry->IsSearchAllAssets() && !UnregisteredRegistry->IsGathering());
		TArray<FAssetData> Audit;
		FString AuditDigest;
		TArray<FAssetData> Candidates;
		TArray<Private::FConsumer> Consumers;
		TArray<FString> Skipped;
		Passed &= TestFalse(TEXT("unregistered target has no persistent Registry package identity"),
			UnregisteredRegistry->GetAssetPackageDataCopy(
				UnregisteredTarget->GetOutermost()->GetFName()).IsSet());
		Passed &= TestTrue(TEXT("unregistered target exact-class audit succeeds"),
			Private::QueryExactBehaviorTrees(*UnregisteredRegistry, Audit));
		Passed &= TestTrue(TEXT("unregistered target Registry digest succeeds"),
			Private::RegistryIdentityDigest(Audit, AuditDigest));
		const bool bUnion = Private::BuildCandidateUnion(
			*UnregisteredRegistry, *UnregisteredTarget, Audit, Candidates);
		Passed &= TestTrue(TEXT("unregistered target candidate union succeeds"), bUnion);
		if (bUnion)
		{
			Passed &= TestTrue(TEXT("unregistered target consumer capture succeeds"),
				Private::CaptureConsumers(*UnregisteredTarget, Candidates, Consumers, Skipped));
		}
	}
	Passed &= TestTrue(TEXT("unregistered target facade captures an empty/live-audit receipt"),
		CaptureLoadedBehaviorTreeConsumerReceipt(*UnregisteredTarget).HasValue());

	const FString IsolatedPackageName = FString::Printf(
		TEXT("/Game/AssetDocumentTests/P_AI/BB_ConsumerCompletion_Isolated_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString IsolatedFilename = FPackageName::LongPackageNameToFilename(
		IsolatedPackageName, FPackageName::GetAssetPackageExtension());
	FScopedPackageFile IsolatedPackageFile(IsolatedFilename);
	UPackage* const IsolatedPackage = CreatePackage(*IsolatedPackageName);
	UBlackboardData* const IsolatedTarget = NewObject<UBlackboardData>(
		IsolatedPackage, TEXT("Blackboard"), RF_Public | RF_Standalone | RF_Transactional);
	if (!TestNotNull(TEXT("isolated registered target exists"), IsolatedTarget)) return false;
	IsolatedTarget->UpdateParentKeys();
	IsolatedTarget->UpdateKeyIDs();
	FSavePackageArgs IsolatedSaveArgs;
	IsolatedSaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	IsolatedSaveArgs.SaveFlags = SAVE_NoError;
	if (!TestTrue(TEXT("isolated target saves without any referencer"),
		UPackage::SavePackage(IsolatedPackage, IsolatedTarget,
			*IsolatedFilename, IsolatedSaveArgs))) return false;
	UnregisteredRegistry->ScanFilesSynchronous({IsolatedFilename}, true);
	IsolatedPackage->SetDirtyFlag(false);
	Passed &= TestTrue(TEXT("isolated target has persistent Registry package identity"),
		UnregisteredRegistry->GetAssetPackageDataCopy(IsolatedPackage->GetFName()).IsSet());
	const bool bIsolatedCaptured =
		CaptureLoadedBehaviorTreeConsumerReceipt(*IsolatedTarget).HasValue();
	if (!TestTrue(TEXT("isolated target package unloads after capture"),
		UPackageTools::UnloadPackages({IsolatedPackage}))) return false;
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	Passed &= TestTrue(TEXT("registered target with zero referencers captures an empty receipt"),
		bIsolatedCaptured);

	TOptional<FFixture> OptionalFixture = MakeFixture(*this);
	if (!OptionalFixture.IsSet()) return false;
	FFixture& Fixture = OptionalFixture.GetValue();
	TOptional<FFixture> OptionalDerived = MakeFixture(*this);
	TOptional<FFixture> OptionalRetargeted = MakeFixture(*this);
	TOptional<FFixture> OptionalUnrelated = MakeFixture(*this);
	TOptional<FFixture> OptionalUnsaved = MakeFixture(*this, FString(), false, false, false);
	if (!OptionalDerived.IsSet() || !OptionalRetargeted.IsSet() || !OptionalUnrelated.IsSet()
		|| !OptionalUnsaved.IsSet())
	{
		return false;
	}
	FFixture& Derived = OptionalDerived.GetValue();
	FFixture& Retargeted = OptionalRetargeted.GetValue();
	FFixture& Unrelated = OptionalUnrelated.GetValue();
	FFixture& Unsaved = OptionalUnsaved.GetValue();
	IAssetRegistry* Registry = IAssetRegistry::Get();
	if (!TestNotNull(TEXT("loaded-unsaved coverage probe has Registry authority"), Registry)) return false;
	Unsaved.Tree->BlackboardAsset = Fixture.Blackboard.Get();
	Unsaved.Root->BlackboardAsset = Fixture.Blackboard.Get();
	Unsaved.Root->UpdateBlackboard();
	TArray<FAssetData> UnsavedAudit;
	TArray<FAssetData> UnsavedCandidates;
	const FSoftObjectPath UnsavedTreePath(Unsaved.Tree.Get());
	TestFalse(TEXT("loaded-unsaved content tree has no package-data row before first save"),
		Registry->GetAssetPackageDataCopy(Unsaved.Package->GetFName()).IsSet());
	if (!TestTrue(TEXT("loaded-unsaved exact-class audit succeeds"),
		Private::QueryExactBehaviorTrees(*Registry, UnsavedAudit))) return false;
	bool bUnsavedAuditRow = false;
	for (const FAssetData& Asset : UnsavedAudit)
	{
		bUnsavedAuditRow |= Asset.GetSoftObjectPath() == UnsavedTreePath && Asset.IsAssetLoaded();
	}
	TestTrue(TEXT("exact-class audit includes the registered loaded-unsaved tree"), bUnsavedAuditRow);
	if (!TestTrue(TEXT("loaded-unsaved candidate union builds"),
		Private::BuildCandidateUnion(*Registry, *Fixture.Blackboard,
			UnsavedAudit, UnsavedCandidates))) return false;
	TestTrue(TEXT("loaded-unsaved exact-class row is never silently omitted"),
		UnsavedCandidates.ContainsByPredicate([&](const FAssetData& Asset)
		{
			return Asset.GetSoftObjectPath() == UnsavedTreePath;
		}));
	const FString UnloadedPackageName = FString::Printf(
		TEXT("/Game/AssetDocumentTests/P_AI/BT_UnloadedReferencer_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UPackage* UnloadedPackage = CreatePackage(*UnloadedPackageName);
	UBehaviorTree* UnloadedTree = NewObject<UBehaviorTree>(UnloadedPackage,
		TEXT("BT_UnloadedReferencer"), RF_Public | RF_Standalone | RF_Transactional);
	if (!TestNotNull(TEXT("unloaded on-disk referencer package exists"), UnloadedPackage)
		|| !TestNotNull(TEXT("unloaded on-disk referencer tree exists"), UnloadedTree)) return false;
	UnloadedTree->BlackboardAsset = Fixture.Blackboard.Get();
	const FString UnloadedFilename = FPackageName::LongPackageNameToFilename(
		UnloadedPackageName, FPackageName::GetAssetPackageExtension());
	FScopedPackageFile UnloadedPackageFile(UnloadedFilename);
	FSavePackageArgs UnloadedSaveArgs;
	UnloadedSaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	UnloadedSaveArgs.SaveFlags = SAVE_NoError;
	if (!TestTrue(TEXT("unloaded referencer saves its Blackboard dependency"),
		UPackage::SavePackage(UnloadedPackage, UnloadedTree, *UnloadedFilename, UnloadedSaveArgs))) return false;
	if (!TestNotNull(TEXT("unloaded referencer has Registry authority"), Registry)) return false;
	Registry->ScanFilesSynchronous({UnloadedFilename}, true);
	const FSoftObjectPath UnloadedObjectPath(UnloadedTree);
	UnloadedPackage->SetDirtyFlag(false);
	if (!TestTrue(TEXT("saved referencer package unloads before capture"),
		UPackageTools::UnloadPackages({UnloadedPackage}))) return false;
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	const FAssetData UnloadedBeforeCapture = Registry->GetAssetByObjectPath(UnloadedObjectPath, true);
	if (!TestTrue(TEXT("unloaded referencer remains registered on disk"), UnloadedBeforeCapture.IsValid())
		|| !TestFalse(TEXT("unloaded referencer starts unloaded"), UnloadedBeforeCapture.IsAssetLoaded())) return false;
	Derived.Blackboard->Keys.Reset();
	Derived.Blackboard->Parent = Fixture.Blackboard.Get();
	Derived.Blackboard->UpdateParentKeys();
	Derived.Blackboard->UpdateKeyIDs();
	Derived.Tree->BlackboardAsset = Derived.Blackboard.Get();
	Derived.Root->BlackboardAsset = Derived.Blackboard.Get();
	Derived.Root->UpdateBlackboard();
	// Retarget only after its package was saved: Registry dependency metadata
	// still names the old Blackboard, so the exact-class loaded audit is the
	// only admissible way for capture to find this consumer.
	Retargeted.Tree->BlackboardAsset = Fixture.Blackboard.Get();
	Retargeted.Root->BlackboardAsset = Fixture.Blackboard.Get();
	Retargeted.Root->UpdateBlackboard();
	UWorld* World = TestWorld(*this);
	if (World == nullptr) return false;
	TStrongObjectPtr<UBlackboardComponent> Priming = MakeComponent(*this, *World, *Fixture.Blackboard);
	TStrongObjectPtr<UBlackboardComponent> DerivedPriming =
		MakeComponent(*this, *World, *Derived.Blackboard);
	TStrongObjectPtr<UBlackboardComponent> RetargetedPriming =
		MakeComponent(*this, *World, *Fixture.Blackboard);
	TStrongObjectPtr<UBlackboardComponent> UnrelatedPriming =
		MakeComponent(*this, *World, *Unrelated.Blackboard);
	if (!Priming.IsValid() || !DerivedPriming.IsValid() || !RetargetedPriming.IsValid()
		|| !UnrelatedPriming.IsValid()) return false;
	PopulateRuntimeValues(*Priming);
	PopulateRuntimeValues(*DerivedPriming);
	PopulateRuntimeValues(*RetargetedPriming);
	PopulateRuntimeValues(*UnrelatedPriming);
	PrimeAllLeaves(*Fixture.Task, *Fixture.Blackboard, *Priming);
	PrimeAllLeaves(*Derived.Task, *Derived.Blackboard, *DerivedPriming);
	PrimeAllLeaves(*Retargeted.Task, *Fixture.Blackboard, *RetargetedPriming);
	PrimeAllLeaves(*Unrelated.Task, *Unrelated.Blackboard, *UnrelatedPriming);
	const FBlackboard::FKey UnrelatedBefore = Unrelated.Task->DirectSelector.GetSelectedKeyID();
	auto Receipt = CaptureLoadedBehaviorTreeConsumerReceipt(*Fixture.Blackboard);
	if (!TestTrue(TEXT("Registry receipt captures direct, derived, and in-memory retargeted consumers"),
		Receipt.HasValue())) return false;
	Fixture.Task->Matrix.UniqueSelectors.Swap(0, 1);
	RotateKeys(Fixture);
	Passed &= TestEqual(TEXT("unique-array semantic locators survive authored reorder"),
		CompleteLoadedBehaviorTreeConsumerReceipt(Receipt.GetValue(), *Fixture.Blackboard,
			ELoadedConsumerCompletionPhase::ForwardPublished),
		ELoadedConsumerCompletionResult::Succeeded);
	Passed &= TestEqual(TEXT("direct loaded selector resolves target's moved ID"),
		Fixture.Task->DirectSelector.GetSelectedKeyID(), Fixture.Blackboard->GetKeyID(KeyInt));
	Passed &= TestEqual(TEXT("derived loaded selector resolves inherited target's moved ID"),
		Derived.Task->DirectSelector.GetSelectedKeyID(), Derived.Blackboard->GetKeyID(KeyInt));
	Passed &= TestEqual(TEXT("retargeted loaded selector resolves target's moved ID"),
		Retargeted.Task->DirectSelector.GetSelectedKeyID(), Fixture.Blackboard->GetKeyID(KeyInt));
	Passed &= TestEqual(TEXT("unrelated loaded selector remains untouched"),
		Unrelated.Task->DirectSelector.GetSelectedKeyID(), UnrelatedBefore);
	TStrongObjectPtr<UBlackboardComponent> DirectAfter =
		MakeComponent(*this, *World, *Fixture.Blackboard);
	TStrongObjectPtr<UBlackboardComponent> DerivedAfter =
		MakeComponent(*this, *World, *Derived.Blackboard);
	TStrongObjectPtr<UBlackboardComponent> RetargetedAfter =
		MakeComponent(*this, *World, *Fixture.Blackboard);
	if (!DirectAfter.IsValid() || !DerivedAfter.IsValid() || !RetargetedAfter.IsValid()) return false;
	PopulateRuntimeValues(*DirectAfter);
	PopulateRuntimeValues(*DerivedAfter);
	PopulateRuntimeValues(*RetargetedAfter);
	Passed &= TestEqual(TEXT("direct loaded ValueOr resolves target's moved Name-to-ID"),
		Fixture.Task->ValueOrInt32.GetValue(DirectAfter.Get()), 4242);
	Passed &= TestEqual(TEXT("derived loaded ValueOr resolves inherited moved Name-to-ID"),
		Derived.Task->ValueOrInt32.GetValue(DerivedAfter.Get()), 4242);
	Passed &= TestEqual(TEXT("retargeted loaded ValueOr resolves target's moved Name-to-ID"),
		Retargeted.Task->ValueOrInt32.GetValue(RetargetedAfter.Get()), 4242);
	const FAssetData UnloadedAfterCapture = Registry->GetAssetByObjectPath(UnloadedObjectPath, true);
	Passed &= TestTrue(TEXT("unloaded referencer stays registered after capture"),
		UnloadedAfterCapture.IsValid());
	Passed &= TestFalse(TEXT("unloaded referencer remains unloaded after capture"),
		UnloadedAfterCapture.IsAssetLoaded());
	return Passed && !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAIBlackboardConsumerCompletionSupportedEntryProofTest,
	"UEAssetDoc.AIProvider.LoadedBlackboardConsumerSupportedEntryProof",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardConsumerCompletionSupportedEntryProofTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBlackboardConsumerCompletionTests;
	// Test Design Scan:
	// - Behavior under test: v1 local-key replacement completes real loaded BT
	//   consumers inside one active Atomic transaction and rolls the same receipt
	//   back after its late forward-completion terminals.
	// - Candidate technique: public supported-entry state transitions.
	// - Risk or gap: a direct handler/receipt test could pass while the v1 caller
	//   publishes before capture, skips completion, or retries poisoned rollback.
	// - Next RED test: invoke the public v1 entry through a real composition,
	//   inject the lifecycle seams, and observe terminal/output/preimage state.
	// - Expected observable outcome: capture is pre-effect; success/no-op share
	//   one golden DTO; all completion failures are empty DerivedRefreshFailed.
	// - Expected failure reason: the caller has no receipt wiring or rollback
	//   wrapper at the required structural-publication lifecycle point.
	TOptional<FFixture> OptionalFixture = MakeFixture(*this);
	if (!OptionalFixture.IsSet()) return false;
	FFixture& Fixture = OptionalFixture.GetValue();
	UWorld* World = TestWorld(*this);
	if (World == nullptr) return false;
	TStrongObjectPtr<UBlackboardComponent> Priming = MakeComponent(*this, *World, *Fixture.Blackboard);
	if (!Priming.IsValid()) return false;
	PopulateRuntimeValues(*Priming);
	PrimeAllLeaves(*Fixture.Task, *Fixture.Blackboard, *Priming);
	Priming.Reset();
	CollectGarbage(RF_NoFlags);
	// Keep this supported-entry cell inside local_keys.replace.v1's existing
	// scalar-property surface. Remove the other keys only after all leaves were
	// primed, so completion still has to invalidate ten genuinely stale missing-
	// key caches; the exhaustive cell above covers all resolved family paths.
	ReduceToSupportedEntryKeys(Fixture);
	const FBlackboard::FKey StalePrimedSelectorId =
		Fixture.Task->DirectSelector.GetSelectedKeyID();
	const FBlackboard::FKey OriginalSelectorId = Fixture.Blackboard->GetKeyID(KeyInt);
	if (!TestNotEqual(TEXT("supported entry begins with a genuinely stale pre-primed selector"),
		StalePrimedSelectorId, OriginalSelectorId)) return false;
	AddExpectedError(TEXT("Failed to find key"),
		EAutomationExpectedErrorFlags::Contains, 5 + (28 * 2 * 10));
	const TArray<FName> OriginalOrder = [&Fixture]
	{
		TArray<FName> Names;
		for (const FBlackboardEntry& Entry : Fixture.Blackboard->Keys) Names.Add(Entry.EntryName);
		return Names;
	}();
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider = CreateAssetDocumentAIPrimitiveProvider();
	bool Passed = true;

	FCompletionProofRealAtomicScenario Normal;
	if (!Normal.Begin(*this, *Fixture.Blackboard)) return false;
	FAssetDocumentPrimitiveInvocation ReorderInvocation;
	if (!MakeCompletionProofLocalKeysReplaceInvocation(
		*this, Normal, *Fixture.Blackboard, true, ReorderInvocation)) return false;
	const FAssetDocumentPrimitiveOutcome Reorder = Provider->Invoke(
		ReorderInvocation, Normal.Handles, Normal.Transaction.Get());
	Passed &= RequireCompletionProofSuccess(*this, TEXT("v1 normal reorder"), Reorder,
		MakeCompletionProofExpectedV1Value(ReorderInvocation), Fixture.Blackboard->GetPathName());
	Passed &= TestNotEqual(TEXT("v1 normal reorder moves pre-primed selector ID"),
		Fixture.Task->DirectSelector.GetSelectedKeyID(), OriginalSelectorId);
	Passed &= TestTrue(TEXT("normal real Atomic rollback restores typed preimage"),
		Normal.Transaction->Rollback(TEXT("Test.CompletionProof.Normal")).bValid);
	Passed &= TestEqual(TEXT("normal rollback restores original key order"),
		[&Fixture]
		{
			TArray<FName> Names;
			for (const FBlackboardEntry& Entry : Fixture.Blackboard->Keys) Names.Add(Entry.EntryName);
			return Names;
		}(), OriginalOrder);
	Passed &= TestEqual(TEXT("normal rollback recompletes selector from restored Name-to-ID"),
		Fixture.Task->DirectSelector.GetSelectedKeyID(), OriginalSelectorId);

	FCompletionProofRealAtomicScenario NoOpScenario;
	if (!NoOpScenario.Begin(*this, *Fixture.Blackboard)) return false;
	FAssetDocumentPrimitiveInvocation NoOpInvocation;
	if (!MakeCompletionProofLocalKeysReplaceInvocation(
		*this, NoOpScenario, *Fixture.Blackboard, false, NoOpInvocation)) return false;
	const FAssetDocumentPrimitiveOutcome NoOp = Provider->Invoke(
		NoOpInvocation, NoOpScenario.Handles, NoOpScenario.Transaction.Get());
	Passed &= RequireCompletionProofSuccess(*this, TEXT("v1 ordinary no-op"), NoOp,
		MakeCompletionProofExpectedV1Value(NoOpInvocation), Fixture.Blackboard->GetPathName());
	Passed &= TestTrue(TEXT("v1 no-op real Atomic rollback remains valid"),
		NoOpScenario.Transaction->Rollback(TEXT("Test.CompletionProof.NoOp")).bValid);

	FCompletionProofRealAtomicScenario CaptureFailure;
	if (!CaptureFailure.Begin(*this, *Fixture.Blackboard)) return false;
	SetLoadedConsumerCompletionTestFailurePoint(
		ELoadedConsumerCompletionTestFailurePoint::RegistryBeforeReadiness);
	auto DirectCaptureRejected = CaptureLoadedBehaviorTreeConsumerReceipt(*Fixture.Blackboard);
	Passed &= TestFalse(TEXT("direct Registry-before-readiness seam rejects capture"),
		DirectCaptureRejected.HasValue());
	SetLoadedConsumerCompletionTestFailurePoint(
		ELoadedConsumerCompletionTestFailurePoint::RegistryBeforeReadiness);
	FAssetDocumentPrimitiveInvocation CaptureInvocation;
	if (!MakeCompletionProofLocalKeysReplaceInvocation(
		*this, CaptureFailure, *Fixture.Blackboard, true, CaptureInvocation)) return false;
	const FAssetDocumentPrimitiveOutcome CaptureRejected = Provider->Invoke(
		CaptureInvocation,
		CaptureFailure.Handles, CaptureFailure.Transaction.Get());
	Passed &= RequireCompletionProofFailure(*this, TEXT("v1 Registry-before-readiness"),
		CaptureRejected, TEXT("AIProvider.AuthorityUnavailable"),
		TEXT("The loaded BehaviorTree consumer coverage receipt is unavailable."));
	Passed &= TestEqual(TEXT("Registry-before-readiness is pre-effect"),
		Fixture.Task->DirectSelector.GetSelectedKeyID(), OriginalSelectorId);
	Passed &= TestEqual(TEXT("Registry-before-readiness leaves authored key order untouched"),
		[&Fixture]
		{
			TArray<FName> Names;
			for (const FBlackboardEntry& Entry : Fixture.Blackboard->Keys) Names.Add(Entry.EntryName);
			return Names;
		}(), OriginalOrder);

	auto RunForwardFailure = [&](FFixture& ActiveFixture,
		const TArray<FName>& ExpectedOrder, const FBlackboard::FKey ExpectedSelectorId,
		const ELoadedConsumerCompletionTestFailurePoint Point, const FString& Label,
		const FString& ExpectedMessage, const bool bPoisonRollback)
	{
		FCompletionProofRealAtomicScenario Scenario;
		if (!Scenario.Begin(*this, *ActiveFixture.Blackboard)) return false;
		SetLoadedConsumerCompletionTestFailurePoint(Point);
		FAssetDocumentPrimitiveInvocation FailureInvocation;
		if (!MakeCompletionProofLocalKeysReplaceInvocation(
			*this, Scenario, *ActiveFixture.Blackboard, true, FailureInvocation)) return false;
		const FAssetDocumentPrimitiveOutcome Failure = Provider->Invoke(
			FailureInvocation,
			Scenario.Handles, Scenario.Transaction.Get());
		bool RowPassed = RequireCompletionProofFailure(*this, Label, Failure,
			TEXT("AIProvider.Blackboard.DerivedRefreshFailed"), ExpectedMessage);
		if (bPoisonRollback)
		{
			SetLoadedConsumerCompletionTestFailurePoint(
				ELoadedConsumerCompletionTestFailurePoint::RollbackVerification);
		}
		const FAssetDocumentContractValidationResult Rollback =
			Scenario.Transaction->Rollback(TEXT("Test.CompletionProof.ForwardFailure"));
		RowPassed &= TestEqual(*(Label + TEXT(" selects the exact Atomic terminal")),
			Scenario.Transaction->GetState(), bPoisonRollback
				? EAssetDocumentAtomicState::Poisoned : EAssetDocumentAtomicState::RolledBack);
		RowPassed &= TestEqual(*(Label + TEXT(" rollback result matches terminal")), Rollback.bValid,
			!bPoisonRollback);
		RowPassed &= TestEqual(*(Label + TEXT(" restores selector by name, never old numeric cache")),
			ActiveFixture.Task->DirectSelector.GetSelectedKeyID(), ExpectedSelectorId);
		TStrongObjectPtr<UBlackboardComponent> RestoredComponent =
			MakeComponent(*this, *World, *ActiveFixture.Blackboard);
		if (!RestoredComponent.IsValid()) return false;
		PopulateRuntimeValues(*RestoredComponent);
		RowPassed &= VerifySupportedValueOrClosure(
			*this, *ActiveFixture.Task, *RestoredComponent);
		RestoredComponent.Reset();
		CollectGarbage(RF_NoFlags);
		RowPassed &= TestEqual(*(Label + TEXT(" restores the typed local-key preimage")),
			[&ActiveFixture]
			{
				TArray<FName> Names;
				for (const FBlackboardEntry& Entry : ActiveFixture.Blackboard->Keys)
				{
					Names.Add(Entry.EntryName);
				}
				return Names;
			}(), ExpectedOrder);
		if (bPoisonRollback)
		{
			const FAssetDocumentContractValidationResult Repeat =
				Scenario.Transaction->Rollback(TEXT("Test.CompletionProof.NoRetry"));
			RowPassed &= TestFalse(*(Label + TEXT(" poisoned rollback has no retry")), Repeat.bValid);
			RowPassed &= TestEqual(*(Label + TEXT(" remains poisoned after no-retry probe")),
				Scenario.Transaction->GetState(), EAssetDocumentAtomicState::Poisoned);
		}
		return RowPassed;
	};
	Passed &= RunForwardFailure(Fixture, OriginalOrder, OriginalSelectorId,
		ELoadedConsumerCompletionTestFailurePoint::BeforeLoadedConsumerForwardCompletion,
		TEXT("v1 before-loaded-consumer completion"),
		TEXT("The test seam rejected loaded-consumer completion before execution."), false);
	Passed &= RunForwardFailure(Fixture, OriginalOrder, OriginalSelectorId,
		ELoadedConsumerCompletionTestFailurePoint::AfterLoadedConsumerForwardCompletion,
		TEXT("v1 late after-loaded-consumer completion"),
		TEXT("The test seam rejected loaded-consumer completion after execution."), false);
	Passed &= RunForwardFailure(Fixture, OriginalOrder, OriginalSelectorId,
		ELoadedConsumerCompletionTestFailurePoint::AfterLoadedConsumerForwardCompletion,
		TEXT("v1 rollback-verification poison"),
		TEXT("The test seam rejected loaded-consumer completion after execution."), true);

	TOptional<FFixture> FullMatrixFixture = MakeFixture(*this);
	if (!FullMatrixFixture.IsSet()) return false;
	FFixture& MatrixFixture = FullMatrixFixture.GetValue();
	TStrongObjectPtr<UBlackboardComponent> MatrixPriming =
		MakeComponent(*this, *World, *MatrixFixture.Blackboard);
	if (!MatrixPriming.IsValid()) return false;
	PopulateRuntimeValues(*MatrixPriming);
	PrimeAllLeaves(*MatrixFixture.Task, *MatrixFixture.Blackboard, *MatrixPriming);
	MatrixPriming.Reset();
	CollectGarbage(RF_NoFlags);
	ReduceToSupportedEntryKeys(MatrixFixture);
	const FBlackboard::FKey MatrixSelectorId =
		MatrixFixture.Blackboard->GetKeyID(KeyInt);
	const TArray<FName> MatrixOriginalOrder = [&MatrixFixture]
	{
		TArray<FName> Names;
		for (const FBlackboardEntry& Entry : MatrixFixture.Blackboard->Keys)
		{
			Names.Add(Entry.EntryName);
		}
		return Names;
	}();
	const ELoadedConsumerCompletionTestFailurePoint InternalCompletionPoints[] = {
		ELoadedConsumerCompletionTestFailurePoint::BeforeGraph,
		ELoadedConsumerCompletionTestFailurePoint::AfterGraph,
		ELoadedConsumerCompletionTestFailurePoint::BeforeSelector,
		ELoadedConsumerCompletionTestFailurePoint::AfterSelector,
		ELoadedConsumerCompletionTestFailurePoint::BeforeBool,
		ELoadedConsumerCompletionTestFailurePoint::AfterBool,
		ELoadedConsumerCompletionTestFailurePoint::BeforeClass,
		ELoadedConsumerCompletionTestFailurePoint::AfterClass,
		ELoadedConsumerCompletionTestFailurePoint::BeforeEnum,
		ELoadedConsumerCompletionTestFailurePoint::AfterEnum,
		ELoadedConsumerCompletionTestFailurePoint::BeforeFloat,
		ELoadedConsumerCompletionTestFailurePoint::AfterFloat,
		ELoadedConsumerCompletionTestFailurePoint::BeforeInt32,
		ELoadedConsumerCompletionTestFailurePoint::AfterInt32,
		ELoadedConsumerCompletionTestFailurePoint::BeforeName,
		ELoadedConsumerCompletionTestFailurePoint::AfterName,
		ELoadedConsumerCompletionTestFailurePoint::BeforeString,
		ELoadedConsumerCompletionTestFailurePoint::AfterString,
		ELoadedConsumerCompletionTestFailurePoint::BeforeObject,
		ELoadedConsumerCompletionTestFailurePoint::AfterObject,
		ELoadedConsumerCompletionTestFailurePoint::BeforeRotator,
		ELoadedConsumerCompletionTestFailurePoint::AfterRotator,
		ELoadedConsumerCompletionTestFailurePoint::BeforeStruct,
		ELoadedConsumerCompletionTestFailurePoint::AfterStruct,
		ELoadedConsumerCompletionTestFailurePoint::BeforeVector,
		ELoadedConsumerCompletionTestFailurePoint::AfterVector,
		ELoadedConsumerCompletionTestFailurePoint::BeforeVerification,
		ELoadedConsumerCompletionTestFailurePoint::AfterVerification,
	};
	for (const ELoadedConsumerCompletionTestFailurePoint Point : InternalCompletionPoints)
	{
		const FString Label = FString::Printf(TEXT("v1 real Atomic completion point %d"),
			static_cast<int32>(Point));
		Passed &= RunForwardFailure(MatrixFixture, MatrixOriginalOrder, MatrixSelectorId,
			Point, Label, TEXT("Loaded BehaviorTree consumer cache completion failed."),
			false);
		Passed &= RunForwardFailure(MatrixFixture, MatrixOriginalOrder, MatrixSelectorId,
			Point, Label + TEXT(" poison"),
			TEXT("Loaded BehaviorTree consumer cache completion failed."), true);
	}
	return Passed && !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAIBlackboardConsumerCompletionStructuralCallerProofTest,
	"UEAssetDoc.AIProvider.LoadedBlackboardConsumerStructuralCallerProof",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardConsumerCompletionStructuralCallerProofTest::RunTest(
	const FString&)
{
	using namespace AssetDocumentAIBlackboardConsumerCompletionTests;
	TOptional<FFixture> OptionalFixture = MakeFixture(*this);
	if (!OptionalFixture.IsSet()) return false;
	UWorld* World = TestWorld(*this);
	if (World == nullptr) return false;
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentAIPrimitiveProvider();
	// Each caller executes six completion phases over five deliberately missing
	// selectors after reduction: success forward/rollback plus two late-failure
	// forward/rollback pairs. These stock warnings are part of the missing-key
	// cache-completion proof, not an unexpected warning lane.
	AddExpectedError(TEXT("Failed to find key"),
		EAutomationExpectedErrorFlags::Contains, 2 * 6 * 5);
	bool Passed = true;

	auto RunCaller = [&](FFixture& Fixture, const bool bAdd)
	{
		TStrongObjectPtr<UBlackboardComponent> Priming =
			MakeComponent(*this, *World, *Fixture.Blackboard);
		if (!Priming.IsValid()) return false;
		PopulateRuntimeValues(*Priming);
		PrimeAllLeaves(*Fixture.Task, *Fixture.Blackboard, *Priming);
		// The task caches are now primed. A component whose value memory was
		// allocated for the old schema must not survive an add/replace mutation.
		Priming.Reset();
		CollectGarbage(RF_NoFlags);
		ReduceToSupportedEntryKeys(Fixture);
		const TArray<FName> OriginalOrder = CompletionProofKeyOrder(*Fixture.Blackboard);
		const FBlackboard::FKey StalePrimedSelectorId =
			Fixture.Task->DirectSelector.GetSelectedKeyID();
		const FBlackboard::FKey OriginalSelectorId =
			Fixture.Blackboard->GetKeyID(KeyInt);
		if (!TestNotEqual(TEXT("structural caller begins with a stale pre-primed selector"),
			StalePrimedSelectorId, OriginalSelectorId)) return false;
		UBlackboardKeyType* const OriginalReplaceKeyType =
			Fixture.Blackboard->Keys[0].KeyType;
		auto MakeCallerInvocation = [&](const FCompletionProofRealAtomicScenario& Scenario)
		{
			FAssetDocumentRuntimeValueDto Properties = bAdd
				? CompletionProofObject({TEXT("DefaultValue")}, {CompletionProofInteger(17)})
				: CompletionProofObject({TEXT("DefaultValue")}, {CompletionProofNumber(6.25)});
			return MakeStructuralCallerInvocation(Scenario, *Fixture.Blackboard, bAdd,
				MakeStructuralCallerEntry(
					bAdd ? TEXT("AddedByCompletionProof") : TEXT("BeforeReorder"),
					bAdd ? *UBlackboardKeyType_Int::StaticClass()
						: *UBlackboardKeyType_Float::StaticClass(),
					MoveTemp(Properties)));
		};
		const FStringView RollbackComponentId = bAdd
			? TEXTVIEW("assetdocument.ai.blackboard.entry.add.v2")
			: TEXTVIEW("assetdocument.ai.blackboard.entry.replace_class.v2");
		auto VerifyRestored = [&](const FString& Label)
		{
			bool RowPassed = TestEqual(*(Label + TEXT(" restores exact key order")),
				CompletionProofKeyOrder(*Fixture.Blackboard), OriginalOrder);
			const bool bSelectorRestored = TestEqual(
				*(Label + TEXT(" restores selector Name-to-ID")),
				Fixture.Task->DirectSelector.GetSelectedKeyID(), OriginalSelectorId);
			RowPassed &= bSelectorRestored;
			RowPassed &= TestEqual(*(Label + TEXT(" restores replacement KeyType identity")),
				Fixture.Blackboard->Keys[0].KeyType.Get(), OriginalReplaceKeyType);
			if (!bSelectorRestored)
			{
				return false;
			}
			TStrongObjectPtr<UBlackboardComponent> Restored =
				MakeComponent(*this, *World, *Fixture.Blackboard);
			if (!Restored.IsValid()) return false;
			PopulateRuntimeValues(*Restored);
			RowPassed &= VerifySupportedValueOrClosure(*this, *Fixture.Task, *Restored);
			Restored.Reset();
			CollectGarbage(RF_NoFlags);
			return RowPassed;
		};
		const FString Caller = bAdd ? TEXT("add") : TEXT("replace-class");
		bool CallerPassed = true;
		{
			FCompletionProofRealAtomicScenario Scenario;
			if (!Scenario.Begin(*this, *Fixture.Blackboard, RollbackComponentId)) return false;
			const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
				MakeCallerInvocation(Scenario), Scenario.Handles,
				Scenario.Transaction.Get());
			if (Outcome.Diagnostics.Num() == 1)
			{
				AddInfo(FString::Printf(TEXT("%s unexpected terminal %s: %s"),
					*Caller, *Outcome.Diagnostics[0].Code,
					*Outcome.Diagnostics[0].Message));
			}
			const bool bForwardSucceeded = TestEqual(
				*(Caller + TEXT(" loaded-consumer forward succeeds")),
				Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
			CallerPassed &= bForwardSucceeded;
			CallerPassed &= TestTrue(*(Caller + TEXT(" success has no diagnostic/handle")),
				Outcome.Diagnostics.IsEmpty() && Outcome.HandleResults.IsEmpty());
			CallerPassed &= TestTrue(*(Caller + TEXT(" success reports its authored change")),
				!Outcome.ChangedSet.IsEmpty());
			if (bForwardSucceeded && bAdd)
			{
				CallerPassed &= TestNotEqual(TEXT("add forward moves the pre-primed selector ID"),
					Fixture.Task->DirectSelector.GetSelectedKeyID(), OriginalSelectorId);
			}
			else if (bForwardSucceeded)
			{
				CallerPassed &= TestEqual(TEXT("replace forward publishes the requested class"),
					Fixture.Blackboard->Keys[0].KeyType->GetClass(),
					UBlackboardKeyType_Float::StaticClass());
			}
			const bool bRollbackValid = TestTrue(
				*(Caller + TEXT(" success rollback is valid")),
				Scenario.Transaction->Rollback(TEXT("Test.StructuralCaller.Success")).bValid);
			CallerPassed &= bRollbackValid;
			if (bRollbackValid)
			{
				CallerPassed &= VerifyRestored(Caller + TEXT(" success rollback"));
			}
		}
		{
			FCompletionProofRealAtomicScenario Scenario;
			if (!Scenario.Begin(*this, *Fixture.Blackboard, RollbackComponentId)) return false;
			SetLoadedConsumerCompletionTestFailurePoint(
				ELoadedConsumerCompletionTestFailurePoint::RegistryBeforeReadiness);
			const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
				MakeCallerInvocation(Scenario), Scenario.Handles,
				Scenario.Transaction.Get());
			CallerPassed &= RequireCompletionProofFailure(*this,
				Caller + TEXT(" capture failure"), Outcome,
				TEXT("AIProvider.AuthorityUnavailable"),
				TEXT("The loaded BehaviorTree consumer coverage receipt is unavailable."));
			CallerPassed &= TestEqual(*(Caller + TEXT(" capture failure is pre-effect")),
				CompletionProofKeyOrder(*Fixture.Blackboard), OriginalOrder);
			CallerPassed &= TestTrue(*(Caller + TEXT(" capture failure rollback is valid")),
				Scenario.Transaction->Rollback(TEXT("Test.StructuralCaller.Capture")).bValid);
		}
		auto RunLateFailure = [&](const bool bPoison)
		{
			FCompletionProofRealAtomicScenario Scenario;
			if (!Scenario.Begin(*this, *Fixture.Blackboard, RollbackComponentId)) return false;
			SetLoadedConsumerCompletionTestFailurePoint(
				ELoadedConsumerCompletionTestFailurePoint::AfterLoadedConsumerForwardCompletion);
			const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
				MakeCallerInvocation(Scenario), Scenario.Handles,
				Scenario.Transaction.Get());
			const FString Label = Caller + (bPoison
				? TEXT(" late rollback poison") : TEXT(" late rollback"));
			bool RowPassed = RequireCompletionProofFailure(*this, Label, Outcome,
				TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
				TEXT("The test seam rejected loaded-consumer completion after execution."));
			if (bPoison)
			{
				SetLoadedConsumerCompletionTestFailurePoint(
					ELoadedConsumerCompletionTestFailurePoint::RollbackVerification);
			}
			const FAssetDocumentContractValidationResult Rollback =
				Scenario.Transaction->Rollback(TEXT("Test.StructuralCaller.Late"));
			RowPassed &= TestEqual(*(Label + TEXT(" selects exact Atomic state")),
				Scenario.Transaction->GetState(), bPoison
					? EAssetDocumentAtomicState::Poisoned
					: EAssetDocumentAtomicState::RolledBack);
			RowPassed &= TestEqual(*(Label + TEXT(" rollback result matches state")),
				Rollback.bValid, !bPoison);
			if (bPoison)
			{
				RowPassed &= TestFalse(*(Label + TEXT(" has no retry")),
					Scenario.Transaction->Rollback(TEXT("Test.StructuralCaller.NoRetry")).bValid);
			}
			return RowPassed && VerifyRestored(Label);
		};
		CallerPassed &= RunLateFailure(false);
		CallerPassed &= RunLateFailure(true);
		return CallerPassed;
	};

	Passed &= RunCaller(OptionalFixture.GetValue(), true);
	// A poisoned Atomic rollback is deliberately non-reusable even when every
	// observable row was restored. Retire that exact asset before proving the
	// other public caller against a fresh target and transaction lineage.
	OptionalFixture.Reset();
	TOptional<FFixture> ReplaceFixture = MakeFixture(*this);
	if (!ReplaceFixture.IsSet()) return false;
	Passed &= RunCaller(ReplaceFixture.GetValue(), false);
	return Passed && !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
