// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreePrimitives.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeMutationInternal.h"
#include "Provider/AssetDocumentAIProviderContracts.h"
#include "AssetDocumentEditorNativeState.h"
#include "TypedPreimage/AssetDocumentAISelectorTypedPreimageInternal.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeAuthoredReadbackInternal.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeTypes.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTreeGraph.h"
#include "DetailWidgetRow.h"
#include "Editor.h"
#include "IDetailTreeNode.h"
#include "IPropertyRowGenerator.h"
#include "IPropertyTypeCustomization.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"
#include "UObject/StructOnScope.h"
#include <initializer_list>
namespace AIBehaviorTreeSelectorBatchResolveLocal
{
using namespace AssetDocumentAIBehaviorTreeMutation;
using UE::AssetDocument::AI::TypedPreimage::Selector::ResolveExactSelectorOwner;
constexpr const TCHAR* Component = TEXT("assetdocument.ai.behavior_tree.selector_cache.v1");
FAssetDocumentPrimitiveOutcome BatchFailure(const TCHAR* Code,const TCHAR* Message,
 TConstArrayView<FAssetDocumentRuntimeDiagnostic> Nested={})
{
 auto Out=MakeAssetDocumentAIPrimitiveFailure(EAssetDocumentPrimitiveOutcomeStatus::Failed,Code,Message);
 if (!Nested.IsEmpty())
 {
  auto& Context=Out.Diagnostics[0].Context.AddDefaulted_GetRef();
  Context.Key=TEXT("nested_diagnostics"); Context.Value.Kind=EAssetDocumentRuntimeValueKind::Array;
  for (const auto& Diagnostic:Nested)
   Context.Value.Elements.Add(StringValue(Diagnostic.Code+TEXT(": ")+Diagnostic.Message));
 }
 return Out;
}
class FAssetDocumentAIPassiveBlackboardSelectorCustomization final
	: public IPropertyTypeCustomization
{
public:
	static TSharedRef<IPropertyTypeCustomization> MakeInstance()
	{
		return MakeShared<FAssetDocumentAIPassiveBlackboardSelectorCustomization>();
	}

	virtual void CustomizeHeader(
		TSharedRef<IPropertyHandle> PropertyHandle,
		FDetailWidgetRow& HeaderRow,
		IPropertyTypeCustomizationUtils&) override
	{
		HeaderRow
			.NameContent()[PropertyHandle->CreatePropertyNameWidget()]
			.ValueContent()[PropertyHandle->CreatePropertyValueWidget()];
	}

	virtual void CustomizeChildren(
		TSharedRef<IPropertyHandle>,
		IDetailChildrenBuilder&,
		IPropertyTypeCustomizationUtils&) override
	{
	}
};

bool IsEditorPropertyContext(const UObject* Object)
{
	if (!IsInGameThread() || !GIsEditor || IsRunningCommandlet()
		|| IsGarbageCollecting() || Object == nullptr
		|| Object->HasAnyFlags(RF_ClassDefaultObject | RF_BeginDestroyed | RF_FinishDestroyed))
	{
		return false;
	}
	const UPackage* Package = Object->GetOutermost();
	return Package != nullptr && !Package->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed);
}

bool MakeRows(
	UObject& Object,
	const FString& PropertyPath,
	TSharedPtr<IPropertyRowGenerator>& OutRows, bool Nested=false)
{
	if (!IsEditorPropertyContext(&Object)) return false;
	FPropertyEditorModule* PropertyEditor =
		FModuleManager::LoadModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor"));
	if (PropertyEditor == nullptr) return false;
	FPropertyRowGeneratorArgs Args;
	Args.bAllowEditingClassDefaultObjects = false;
	OutRows = PropertyEditor->CreatePropertyRowGenerator(Args);
	if (!OutRows.IsValid()) return false;
	OutRows->RegisterInstancedCustomPropertyTypeLayout(
		FBlackboardKeySelector::StaticStruct()->GetFName(),
		FOnGetPropertyTypeCustomizationInstance::CreateStatic(
			&FAssetDocumentAIPassiveBlackboardSelectorCustomization::MakeInstance));
	if (!Nested) OutRows->SetPropertyGenerationAllowListPaths({PropertyPath});
	else UE::AssetDocumentAI::BehaviorTreeAuthoredReadback::ConfigureReadOnlyValueOrRows(*OutRows);
	OutRows->SetObjects({&Object});
	return true;
}

TSharedPtr<IPropertyHandle> FindUniqueSelectorHandle(
	IPropertyRowGenerator& Rows,
	UObject& Owner,
	const FString& PropertyPath)
{
	TArray<TSharedPtr<IPropertyHandle>> Matches;
	TFunction<void(const TArray<TSharedRef<IDetailTreeNode>>&)> Gather =
		[&](const TArray<TSharedRef<IDetailTreeNode>>& Nodes)
	{
		for (const TSharedRef<IDetailTreeNode>& Node : Nodes)
		{
			const TSharedPtr<IPropertyHandle> Candidate = Node->CreatePropertyHandle();
			TArray<UObject*> Outers;
			if (Candidate.IsValid()) Candidate->GetOuterObjects(Outers);
			FProperty* Property = Candidate.IsValid() ? Candidate->GetProperty() : nullptr;
			const FStructProperty* Struct = Property != nullptr
				? CastField<FStructProperty>(Property) : nullptr;
			if (Struct != nullptr && Struct->Struct == FBlackboardKeySelector::StaticStruct()
				&& Candidate->GeneratePathToProperty() == PropertyPath
				&& Outers.Num() == 1 && Outers[0] == &Owner)
			{
				Matches.Add(Candidate);
			}
			TArray<TSharedRef<IDetailTreeNode>> Children;
			Node->GetChildren(Children, true);
			Gather(Children);
		}
	};
	Gather(Rows.GetRootTreeNodes());
	return Matches.Num() == 1 ? Matches[0] : nullptr;
}


TSharedPtr<IPropertyHandle> FindNestedSelector(IPropertyRowGenerator& Rows,UObject& Owner,const FString& Path)
{
 TSharedPtr<IPropertyHandle> Match;void* MatchAddress=nullptr;int32 Budget=4096;bool Good=true;
 TFunction<void(TSharedPtr<IPropertyHandle>,int32)> Visit;
 Visit=[&](TSharedPtr<IPropertyHandle> H,int32 Depth)
 {
  if(!Good||!H.IsValid())return;
  if(Depth>64||--Budget<0){Good=false;return;}
  if(const auto Fixed=H->AsArray();Fixed.IsValid()&&H->GetProperty()&&H->GetProperty()->ArrayDim>1)
  {
   uint32 Count=0;if(Fixed->GetNumElements(Count)!=FPropertyAccess::Success||Count!=uint32(H->GetProperty()->ArrayDim)){Good=false;return;}
   for(uint32 I=0;I<Count;++I){const auto Element=Fixed->GetElement(I);Visit(Element,Depth+1);}
   return;
  }
  const auto* P=CastField<FStructProperty>(H->GetProperty());
  if(P&&P->Struct==FBlackboardKeySelector::StaticStruct())
  {
   if(FString(H->GetPropertyPath())==Path)
   {void* Address=nullptr;if(H->GetValueData(Address)!=FPropertyAccess::Success||!Address||(Match.IsValid()&&Address!=MatchAddress)){Good=false;return;}Match=H;MatchAddress=Address;}
   return;
  }
  if(const auto* O=CastField<FObjectPropertyBase>(H->GetProperty()))
  {
   if(!O->HasAnyPropertyFlags(CPF_InstancedReference))return;
   const UObject* Value=nullptr;if(H->GetValue(Value)!=FPropertyAccess::Success){Good=false;return;}
   if(!Value)return;if(!IsValid(Value)||!Value->IsIn(&Owner)){Good=false;return;}
  }
  const auto A=H->AsArray();const auto M=H->AsMap();const auto S=H->AsSet();uint32 Count=0;
  if(A.IsValid()||M.IsValid()||S.IsValid())
  {
   const auto R=A.IsValid()?A->GetNumElements(Count):M.IsValid()?M->GetNumElements(Count):S->GetNumElements(Count);
   if(R!=FPropertyAccess::Success||Count>4096){Good=false;return;}
   for(uint32 I=0;I<Count;++I)Visit(A.IsValid()?A->GetElement(I):M.IsValid()?M->GetElement(I):S->GetElement(I),Depth+1);
   return;
  }
  if(H->GetNumChildren(Count)!=FPropertyAccess::Success||Count>4096){Good=false;return;}
  for(uint32 I=0;I<Count;++I){const auto Child=H->GetChildHandle(I);if(Child.IsValid()&&(!Child->GetProperty()||UE::AssetDocumentAI::BehaviorTreeAuthoredReadback::IsAuthoredProperty(*Child)))Visit(Child,Depth+1);}
 };
 TFunction<void(const TArray<TSharedRef<IDetailTreeNode>>&)> Gather;
 TMap<FString,TSharedRef<IPropertyHandle>> Roots;
 Gather=[&](const auto& Nodes)
 {
  for(const auto& N:Nodes)
  {
   if(--Budget<0){Good=false;return;}auto H=N->CreatePropertyHandle();
   for(int32 D=0;H.IsValid()&&D<64;++D,H=H->GetParentHandle())
   {
    const auto* P=H->GetProperty();
    if(P&&P->GetOwnerProperty()==P&&P->GetOwnerClass()&&Owner.IsA(P->GetOwnerClass())&&UE::AssetDocumentAI::BehaviorTreeAuthoredReadback::IsAuthoredProperty(*H))Roots.Add(P->GetName(),H.ToSharedRef());
   }
   TArray<TSharedRef<IDetailTreeNode>> Children;N->GetChildren(Children,true);Gather(Children);
  }
 };
 Gather(Rows.GetRootTreeNodes());for(const auto& Root:Roots)Visit(Root.Value,0);return Good?Match:nullptr;
}
struct FNativeStep
{
 TFieldPath<FProperty> Property;FString Path;void* Address=nullptr;
 TArray<UObject*> Objects;TArray<TPair<const UStruct*,const uint8*>> Structs;uint32 Count=0;
 TArray<uint8> Authored,Key;
 bool operator==(const FNativeStep& B)const{return Property==B.Property&&Path==B.Path&&Address==B.Address&&Objects==B.Objects&&Structs==B.Structs&&Count==B.Count&&Authored==B.Authored&&Key==B.Key;}
};
bool ObserveBinding(IPropertyHandle& Leaf,UObject& Owner,TArray<FNativeStep>& Out)
{
 TSharedPtr<IPropertyHandle> H=Leaf.AsShared();bool Root=false;
 while(H.IsValid())
 {
  if(Out.Num()>=64||!H->IsValidHandle())return false;FNativeStep Step;Step.Property=H->GetProperty();Step.Path=H->GeneratePathToProperty();
  if(H->GetProperty()&&H->GetValueData(Step.Address)!=FPropertyAccess::Success)return false;
  if(H->GetProperty())
  {
   using namespace UE::AssetDocumentAI::BehaviorTreeAuthoredReadback;
   FPropertyReadContext Context;Context.Version=EAuthoredGraphReadbackVersion::V2;Context.RootObject=&Owner;FAssetDocumentRuntimeValueDto Value;
   if(!ReadAuthoredProperty(*H,Context,0,Value)||!EncodeAuthoredCanonicalValue(Value,Step.Authored))return false;
   if(const auto K=H->GetKeyHandle())
   {FPropertyReadContext KeyContext;if(!ReadAuthoredProperty(*K,KeyContext,0,Value)||!EncodeAuthoredCanonicalValue(Value,Step.Key))return false;}
  }
  H->GetOuterObjects(Step.Objects);
  for(UObject* O:Step.Objects){if(!IsValid(O)||(O!=&Owner&&!O->IsIn(&Owner)))return false;Root|=O==&Owner;}
  TArray<TSharedPtr<FStructOnScope>> Structs;H->GetOuterStructs(Structs);
  for(const auto& S:Structs)if(S.IsValid())Step.Structs.Add({S->GetStruct(),S->GetStructMemory()});
  if(const auto A=H->AsArray()) {if(A->GetNumElements(Step.Count)!=FPropertyAccess::Success)return false;}
  else if(const auto M=H->AsMap()){if(M->GetNumElements(Step.Count)!=FPropertyAccess::Success)return false;}
  else if(const auto S=H->AsSet()){if(S->GetNumElements(Step.Count)!=FPropertyAccess::Success)return false;}
  Out.Add(MoveTemp(Step));H=H->GetParentHandle();
 }
 return Root;
}
bool ObserveSetHashes(IPropertyHandle& Leaf,TMap<FString,uint32>& Out)
{
 TSharedPtr<IPropertyHandle> H=Leaf.AsShared();int32 Depth=0;
 while(H.IsValid())
 {
  if(++Depth>64)return false;
  const FProperty* P=H->GetProperty();
  if(P&&P->GetOwner<FSetProperty>())
  {
   void* Address=nullptr;if(H->GetValueData(Address)!=FPropertyAccess::Success||!Address)return false;
   Out.Add(H->GeneratePathToProperty(),P->GetValueTypeHash(Address));
  }
  H=H->GetParentHandle();
 }
 return true;
}
struct FEntry
{
 FString StableNodeId;
 FString PropertyPath;
 FString SelectedName;
 UObject* Owner=nullptr;
 TSharedPtr<IPropertyRowGenerator> Rows;
 TSharedPtr<IPropertyHandle> Handle;
 FBlackboardKeySelector* Selector=nullptr;
 TFunction<TSharedPtr<IPropertyHandle>()> Rebind;
 TFunction<bool(IPropertyHandle&)> Binding;
};
struct FRequest
{
 FString TreePath;
 FString BlackboardPath;
 bool Nullable = false;
 TArray<FEntry> Entries;
};
bool IsString(const FAssetDocumentRuntimeValueDto* V)
{ return V && V->Kind==EAssetDocumentRuntimeValueKind::String && !V->StringValue.IsEmpty(); }
bool Parse(const FAssetDocumentRuntimeValueDto& Value,FRequest& Out,FAssetDocumentPrimitiveOutcome& Error)
{
 if (!IsAssetDocumentAICanonicalRuntimeValue(Value)
  || !HasExactFields(Value,{TEXT("behavior_tree_path"),TEXT("blackboard_path"),TEXT("selectors")}))
 { Error=BatchFailure(TEXT("AIProvider.InvalidValueShape"),TEXT("The batch request is not canonical.")); return false; }
 const auto* Tree=FindField(Value,TEXT("behavior_tree_path"));
 const auto* Blackboard=FindField(Value,TEXT("blackboard_path"));
 const auto* Entries=FindField(Value,TEXT("selectors"));
 if (!IsCanonicalObjectPath(Tree) || (!IsCanonicalObjectPath(Blackboard)
   && !(Out.Nullable && Blackboard && Blackboard->Kind==EAssetDocumentRuntimeValueKind::Null))
  || !Entries || Entries->Kind!=EAssetDocumentRuntimeValueKind::Array)
 { Error=BatchFailure(TEXT("AIProvider.InvalidValueShape"),TEXT("The batch target and selector array are required.")); return false; }
 // The result adds two observations per row; admit its existing Runtime envelope before capture.
 if (3 + 6 * Entries->Elements.Num() > 4096)
 { Error=BatchFailure(TEXT("AIProvider.BehaviorTree.SelectorBatchInvalidInventory"),TEXT("The batch result would exceed the existing Runtime value envelope.")); return false; }
 Out.TreePath=Tree->StringValue; Out.BlackboardPath=Blackboard->StringValue;
 TSet<FString> Locators;
 for (const auto& ValueEntry:Entries->Elements)
 {
  const auto* Id=FindField(ValueEntry,TEXT("stable_node_id"));
  const auto* Path=FindField(ValueEntry,TEXT("property_path"));
  const auto* Name=FindField(ValueEntry,TEXT("selected_key_name"));
  FGuid Guid;
  if (!HasExactFields(ValueEntry,{TEXT("stable_node_id"),TEXT("property_path"),TEXT("selected_key_name")})
   || !IsString(Id) || !IsString(Path) || !IsString(Name)
   || !FGuid::ParseExact(Id->StringValue,EGuidFormats::Digits,Guid)
   || !Guid.IsValid() || Guid.ToString(EGuidFormats::Digits)!=Id->StringValue)
  { Error=BatchFailure(TEXT("AIProvider.BehaviorTree.SelectorBatchInvalidInventory"),TEXT("Each selector requires an exact typed canonical locator and authored name.")); return false; }
  const FString Locator=Id->StringValue+TEXT("/")+Path->StringValue;
  if (Locators.Contains(Locator))
  { Error=BatchFailure(TEXT("AIProvider.BehaviorTree.SelectorBatchDuplicateLocator"),TEXT("The selector locator is duplicated.")); return false; }
  Locators.Add(Locator);
  if (!Out.Entries.IsEmpty())
  {
   const auto& Prev=Out.Entries.Last();
   const int32 Order=Prev.StableNodeId.Compare(Id->StringValue,ESearchCase::CaseSensitive);
   if (Order>0 || (Order==0 && Prev.PropertyPath.Compare(Path->StringValue,ESearchCase::CaseSensitive)>=0))
   { Error=BatchFailure(TEXT("AIProvider.BehaviorTree.SelectorBatchInvalidInventory"),TEXT("Selectors must be in strict canonical order.")); return false; }
  }
  FEntry Entry; Entry.StableNodeId=Id->StringValue; Entry.PropertyPath=Path->StringValue; Entry.SelectedName=Name->StringValue;
  Out.Entries.Add(MoveTemp(Entry));
 }
 return true;
}
bool Prepare(UBehaviorTreeGraph& Graph,FEntry& Entry,FAssetDocumentPrimitiveOutcome& Error,bool Nested)
{
 Entry.Owner=ResolveExactSelectorOwner(Graph,Entry.StableNodeId);
 if (!IsEditorPropertyContext(Entry.Owner) || !MakeRows(*Entry.Owner,Entry.PropertyPath,Entry.Rows,Nested))
 { Error=BatchFailure(TEXT("AIProvider.BehaviorTree.SelectorPropertyRejected"),TEXT("The selector owner must be exactly bound to the authored tree.")); return false; }
 Entry.Handle=Nested?FindNestedSelector(*Entry.Rows,*Entry.Owner,Entry.PropertyPath):FindUniqueSelectorHandle(*Entry.Rows,*Entry.Owner,Entry.PropertyPath);
 auto* Property=Entry.Handle.IsValid()?CastField<FStructProperty>(Entry.Handle->GetProperty()):nullptr;
 void* Address=nullptr;
 if (!Entry.Handle.IsValid() || !Entry.Handle->IsEditable() || !Property
  || (!Nested&&(Property->GetOwnerProperty()!=Property || !Property->HasAnyPropertyFlags(CPF_Edit)))
  || Entry.Handle->GetValueData(Address)!=FPropertyAccess::Success || !Address)
 { Error=BatchFailure(TEXT("AIProvider.BehaviorTree.SelectorPropertyRejected"),TEXT("The exact admitted top-level selector property is unavailable.")); return false; }
 Entry.Selector=static_cast<FBlackboardKeySelector*>(Address);
 if(Nested)
 {
  TArray<FNativeStep> Steps;if(!ObserveBinding(*Entry.Handle,*Entry.Owner,Steps)){Error=BatchFailure(TEXT("AIProvider.BehaviorTree.SelectorPropertyRejected"),TEXT("The nested selector binding is unavailable."));return false;}
  TWeakObjectPtr<UObject> Owner=Entry.Owner;auto Rows=Entry.Rows;const FString Path=Entry.PropertyPath;
  Entry.Rebind=[Owner,Rows,Path]()->TSharedPtr<IPropertyHandle>{if(!Owner.IsValid())return nullptr;Rows->SetObjects({Owner.Get()});return FindNestedSelector(*Rows,*Owner.Get(),Path);};
  Entry.Binding=[Owner,Steps](IPropertyHandle& H){TArray<FNativeStep> Current;return Owner.IsValid()&&ObserveBinding(H,*Owner.Get(),Current)&&Current==Steps;};
 }
 if (Entry.Selector->SelectedKeyName.ToString()!=Entry.SelectedName)
 { Error=BatchFailure(TEXT("AIProvider.BehaviorTree.BlackboardMismatch"),TEXT("The selector authored name drifted from the inventory.")); return false; }
 return true;
}
bool Observe(UBehaviorTreeGraph& Graph,const FEntry& Entry,UBlackboardData* Blackboard,bool Nullable,FAssetDocumentRuntimeValueDto& Value)
{
 if (ResolveExactSelectorOwner(Graph,Entry.StableNodeId)!=Entry.Owner) return false;
 const auto Handle=Entry.Rebind?Entry.Rebind():FindUniqueSelectorHandle(*Entry.Rows,*Entry.Owner,Entry.PropertyPath);
 void* Address=nullptr;
 if (!Handle.IsValid() || (Entry.Binding&&!Entry.Binding(*Handle)) || (!Entry.Rebind&&Handle->GetProperty()!=Entry.Handle->GetProperty())
  || Handle->GetValueData(Address)!=FPropertyAccess::Success || Address!=Entry.Selector) return false;
 const auto& Selector=*static_cast<const FBlackboardKeySelector*>(Address);
 const auto Id=Selector.GetSelectedKeyID();
 const bool None = Nullable && Selector.SelectedKeyName.IsNone();
 if (Selector.SelectedKeyName.ToString()!=Entry.SelectedName) return false;
 if (None ? !Selector.IsNone() : (!Blackboard || Id==FBlackboard::InvalidKey
  || Selector.SelectedKeyType==nullptr || Blackboard->GetKeyID(Selector.SelectedKeyName)!=Id
  || Blackboard->GetKeyType(Id)!=Selector.SelectedKeyType)) return false;
 FAssetDocumentRuntimeValueDto Type;
 Type.Kind=EAssetDocumentRuntimeValueKind::Null;
 if (Selector.SelectedKeyType) Type=StringValue(Selector.SelectedKeyType->GetPathName());
 Value=ObjectValue({TEXT("property_path"),TEXT("selected_key_id"),TEXT("selected_key_name"),TEXT("selected_key_type_path"),TEXT("stable_node_id")},
  {StringValue(Entry.PropertyPath),IntegerValue(None ? -1 : static_cast<int32>(Id)),StringValue(Selector.SelectedKeyName.ToString()),MoveTemp(Type),StringValue(Entry.StableNodeId)});
 return true;
}
FAssetDocumentPrimitiveOutcome Run(FRequest& Request,UBlackboardData* Blackboard,
 const IAssetDocumentVerifiedHandleLease& Lease,IAssetDocumentAtomicTransaction& Transaction)
{
 FAssetDocumentPrimitiveOutcome Outcome;
 const auto IdentityResult=WithAssetDocumentEditorAssetIdentity(Lease,[&](const FAssetDocumentEditorAssetIdentityView& Identity)
 {
  FAssetDocumentContractValidationResult Invalid;
  auto* Tree=Cast<UBehaviorTree>(&Identity.GetObject());
  auto* Graph=Tree?Cast<UBehaviorTreeGraph>(Tree->BTGraph):nullptr;
  if (!Tree || Tree->GetClass()!=UBehaviorTree::StaticClass() || Identity.GetCanonicalObjectPath()!=Request.TreePath
   || !Graph || Graph->GetOuter()!=Tree || !IsEditorPropertyContext(Graph))
  { Outcome=BatchFailure(TEXT("AIProvider.InvalidHandle"),TEXT("The Asset lease does not bind the exact live authored tree.")); return Invalid; }
  if (Tree->BlackboardAsset!=Blackboard)
  { Outcome=BatchFailure(TEXT("AIProvider.BehaviorTree.BlackboardMismatch"),TEXT("The live tree has a different Blackboard.")); return Invalid; }
  TArray<FAssetDocumentAISelectorBatchPreimageEntry> Captures;
  for (auto& Entry:Request.Entries)
  {
   if (!Prepare(*Graph,Entry,Outcome,Request.Nullable)) return Invalid;
   if (Request.Nullable)
   {
    // Inspect native None policy without mutating the live selector or selecting
    // a default key. ResolveSelectedKey would auto-select when None is forbidden.
    FBlackboardKeySelector Observation=*Entry.Selector;
    Observation.InvalidateResolvedKey();
    if (Entry.Selector->SelectedKeyName.IsNone() ? !Observation.IsNone()
     : (!Blackboard || Blackboard->GetKeyID(Entry.Selector->SelectedKeyName)==FBlackboard::InvalidKey))
    { Outcome=BatchFailure(TEXT("AIProvider.BehaviorTree.SelectorResolveFailed"),TEXT("The authored selector requires an existing key or an allowed None value.")); return Invalid; }
   }
  }
  // Row construction for later entries must not invalidate an earlier authored selection.
  for (auto& Entry:Request.Entries)
  {
   const FProperty* CapturedProperty=Entry.Handle->GetProperty();
   const auto Current=Entry.Rebind?Entry.Rebind():FindUniqueSelectorHandle(*Entry.Rows,*Entry.Owner,Entry.PropertyPath);
   void* Address=nullptr;
   if (ResolveExactSelectorOwner(*Graph,Entry.StableNodeId)!=Entry.Owner
    || !Current.IsValid() || (Entry.Binding&&!Entry.Binding(*Current)) || Current->GetProperty()!=CapturedProperty
    || Current->GetValueData(Address)!=FPropertyAccess::Success || Address!=Entry.Selector)
   { Outcome=BatchFailure(TEXT("AIProvider.BehaviorTree.SelectorPropertyRejected"),TEXT("The captured owner/property inventory changed.")); return Invalid; }
   if (Entry.Selector->SelectedKeyName.ToString()!=Entry.SelectedName)
   { Outcome=BatchFailure(TEXT("AIProvider.BehaviorTree.BlackboardMismatch"),TEXT("The complete authored-name inventory changed before capture.")); return Invalid; }
   Entry.Handle=Current;
   Captures.Add({Entry.Owner,Entry.Rows.ToSharedRef(),Entry.Handle.ToSharedRef(),Entry.StableNodeId,Entry.PropertyPath,Entry.Rebind,Entry.Binding});
  }
  const auto Capture=CaptureAndEnlistAssetDocumentAIBehaviorTreeSelectorBatchPreimage(Lease,Captures,Transaction);
  if (!Capture.bSucceeded)
  { Outcome=BatchFailure(TEXT("AIProvider.OperationRejected"),TEXT("The aggregate selector preimage was rejected."),Capture.Diagnostics); return Invalid; }
  const FString StablePath=TEXT("asset:")+Request.TreePath+TEXT("/SelectorCache");
  const auto Enlisted=Transaction.EnlistFootprint({{EAssetDocumentMutationFootprintKind::UObjectState,StablePath,Component}});
  if (!Enlisted.bValid)
  { Outcome=BatchFailure(TEXT("AIProvider.OperationRejected"),TEXT("The aggregate selector row was rejected."),Enlisted.Diagnostics); return Invalid; }
  FAssetDocumentRuntimeValueDto Results; Results.Kind=EAssetDocumentRuntimeValueKind::Array;
  for (auto& Entry:Request.Entries)
  {
   if (Entry.Rebind)
   {
    auto Current=Entry.Rebind();
    if(!Current.IsValid()||!Entry.Binding(*Current))
    {Outcome=BatchFailure(TEXT("AIProvider.BehaviorTree.SelectorPropertyRejected"),TEXT("The nested selector structure changed before completion."));return Invalid;}
    TMap<FString,uint32> BeforeHashes;
    if(!ObserveSetHashes(*Current,BeforeHashes)){Outcome=BatchFailure(TEXT("AIProvider.BehaviorTree.SelectorPropertyRejected"),TEXT("Native set hash evidence is unavailable."));return Invalid;}
    const UObject* OldType=Entry.Selector->SelectedKeyType.Get();const int32 OldId=int32(Entry.Selector->GetSelectedKeyID());
    FBlackboardKeySelector Resolved=*Entry.Selector;
    if(Resolved.SelectedKeyName.IsNone())Resolved.InvalidateResolvedKey();else Resolved.ResolveSelectedKey(*Blackboard);
    const auto Type=Current->GetChildHandle(TEXT("SelectedKeyType"),false);
    const UObject* ResolvedType=Resolved.SelectedKeyType.Get();
    if(!Type.IsValid()||Type->SetValue(ResolvedType,EPropertyValueSetFlags::NotTransactable)!=FPropertyAccess::Success)
    {Outcome=BatchFailure(TEXT("AIProvider.BehaviorTree.SelectorResolveFailed"),TEXT("The nested selector type could not publish through stock PropertyEditor."));return Invalid;}
    Current=Entry.Rebind();const auto Id=Current.IsValid()?Current->GetChildHandle(TEXT("SelectedKeyID"),false):nullptr;
    if(!Current.IsValid()||!Entry.Binding(*Current)||!Id.IsValid()
     ||Id->SetValue(int32(Resolved.GetSelectedKeyID()),EPropertyValueSetFlags::NotTransactable)!=FPropertyAccess::Success)
    {Outcome=BatchFailure(TEXT("AIProvider.BehaviorTree.SelectorResolveFailed"),TEXT("The nested selector ID could not publish through stock PropertyEditor."));return Invalid;}
    Current=Entry.Rebind();TMap<FString,uint32> AfterHashes;
    if(!Current.IsValid()||!ObserveSetHashes(*Current,AfterHashes)||!BeforeHashes.OrderIndependentCompareEqual(AfterHashes))
    {
     // Child setters do not rehash enclosing set structs. Restore the exact
     // cache before rejecting a native hash-changing operation; never repair
     // hash storage ourselves or claim it was maintained by SetValue.
     bool Restored=Current.IsValid()&&Entry.Binding(*Current);
     auto T=Restored?Current->GetChildHandle(TEXT("SelectedKeyType"),false):nullptr;
     Restored=T.IsValid()&&T->SetValue(OldType,EPropertyValueSetFlags::NotTransactable)==FPropertyAccess::Success;
     Current=Entry.Rebind();auto K=Current.IsValid()?Current->GetChildHandle(TEXT("SelectedKeyID"),false):nullptr;
     Restored=Restored&&Current.IsValid()&&Entry.Binding(*Current)&&K.IsValid()&&K->SetValue(OldId,EPropertyValueSetFlags::NotTransactable)==FPropertyAccess::Success;
     Current=Entry.Rebind();AfterHashes.Reset();Restored=Restored&&Current.IsValid()&&ObserveSetHashes(*Current,AfterHashes)&&BeforeHashes.OrderIndependentCompareEqual(AfterHashes);
     Outcome=BatchFailure(Restored?TEXT("AIProvider.BehaviorTree.SelectorResolveFailed"):TEXT("AIProvider.BehaviorTree.SelectorBatchCompletionMismatch"),TEXT("The native set hash depends on selector cache state; this completion cannot publish that change."));return Invalid;
    }
   }
   else if (Request.Nullable && Entry.Selector->SelectedKeyName.IsNone()) Entry.Selector->InvalidateResolvedKey();
   else Entry.Selector->ResolveSelectedKey(*Blackboard);
   FAssetDocumentRuntimeValueDto Value;
   if (!Observe(*Graph,Entry,Blackboard,Request.Nullable,Value))
   { Outcome=BatchFailure(TEXT("AIProvider.BehaviorTree.SelectorResolveFailed"),TEXT("The selector could not resolve its authored key.")); return Invalid; }
   Results.Elements.Add(MoveTemp(Value));
  }
  // Re-observe every current field after the entire inventory has completed.
  if (Tree->BTGraph!=Graph || Tree->BlackboardAsset!=Blackboard)
  { Outcome=BatchFailure(TEXT("AIProvider.BehaviorTree.SelectorBatchCompletionMismatch"),TEXT("The aggregate tree binding changed.")); return Invalid; }
  for (int32 Index=0;Index<Request.Entries.Num();++Index)
  {
   FAssetDocumentRuntimeValueDto Observed;
   if (!Observe(*Graph,Request.Entries[Index],Blackboard,Request.Nullable,Observed)
    || Observed.Elements[1].IntegerValue!=Results.Elements[Index].Elements[1].IntegerValue
    || Observed.Elements[3].Kind!=Results.Elements[Index].Elements[3].Kind
    || Observed.Elements[3].StringValue!=Results.Elements[Index].Elements[3].StringValue)
   { Outcome=BatchFailure(TEXT("AIProvider.BehaviorTree.SelectorBatchCompletionMismatch"),TEXT("The aggregate public observation changed.")); return Invalid; }
  }
  Outcome.Status=EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
  Outcome.Value=ObjectValue({TEXT("selector_count"),TEXT("selectors")},{IntegerValue(Request.Entries.Num()),MoveTemp(Results)});
  FAssetDocumentChangedRecord Changed; Changed.AuthorityId=Component; Changed.StableObjectId=Request.TreePath; Changed.Path=StablePath; Changed.ChangeKind=TEXT("Update");
  Outcome.ChangedSet.Add(MoveTemp(Changed));
  FAssetDocumentContractValidationResult Valid; Valid.bValid=true; return Valid;
 });
 return IdentityResult.bValid || !Outcome.Diagnostics.IsEmpty()?Outcome:
  BatchFailure(TEXT("AIProvider.InvalidHandle"),TEXT("The supplied Asset lease cannot enter exact Editor identity."),IdentityResult.Diagnostics);
}
}
FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBehaviorTreeSelectorBatchResolve(
 const FAssetDocumentPrimitiveInvocation& Invocation,IAssetDocumentHandleResolver& Handles,IAssetDocumentAtomicTransaction* Transaction)
{
 using namespace AIBehaviorTreeSelectorBatchResolveLocal;
 if (!IsAssetDocumentAITransactionValid(Invocation,Transaction))
  return BatchFailure(TEXT("AIProvider.InvalidTransaction"),TEXT("Selector batch requires its active transaction."));
 FRequest Request; FAssetDocumentPrimitiveOutcome Error;
 Request.Nullable=Invocation.Version==2;
 if (!Parse(Invocation.ValueArguments,Request,Error)) return Error;
 auto* Blackboard=Cast<UBlackboardData>(FSoftObjectPath(Request.BlackboardPath).ResolveObject());
 if ((!Blackboard && !(Request.Nullable && Request.BlackboardPath.IsEmpty()))
  || (Blackboard && Blackboard->GetPathName()!=Request.BlackboardPath))
  return BatchFailure(TEXT("AIProvider.BehaviorTree.BlackboardMismatch"),TEXT("The Blackboard must be exactly loaded."));
 auto Lease=AcquireAssetDocumentAIMutableAssetLease(Handles,Invocation);
 if (!Lease.Lease) return BatchFailure(TEXT("AIProvider.InvalidHandle"),TEXT("An exact AI mutable Asset lease is required."),Lease.Diagnostics);
 return Run(Request,Blackboard,*Lease.Lease,*Transaction);
}
