// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// find_assets `uses` scanner (AssetDocumentUsageSearch.h) of the graph module:
// - Blueprint family (and Level Script Blueprints of maps): the saved Find-in-Blueprints index (Asset Registry tag
//   FiBData), without loading; node keys are derived from what the index records (node class, called / implemented
//   function and the classes of its pins, variable references). Loaded assets with unsaved changes, and loaded
//   assets without index data, are read from their graphs (the schema adapters' DescribeNode keys).
// - Other assets with a graph host adapter (Sound Cue, Niagara): their loaded graphs; unloaded ones are reported
//   as not searched (no saved node index).

#include "AssetDocumentGraphUsageScanner.h"

#include "Adapters/AssetDocumentGraphBuiltinAdapters.h"
#include "AssetDocumentGraphModel.h"
#include "AssetDocumentUsageSearch.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Blueprint/BlueprintSupport.h"
#include "BlueprintTypePromotion.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/World.h"
#include "FindInBlueprintManager.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_PromotableOperator.h"
#include "K2Node_StructOperation.h"
#include "K2Node_Variable.h"
#include "UObject/EditorObjectVersion.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/PackageName.h"
#include "Serialization/CustomVersion.h"
#include "UObject/Package.h"

namespace UE::AssetDocument::Graph
{
namespace UsageScan
{
// Mirror of asset_document/graphs/actions.py ALIASES (the AI-friendly spellings extracts write); the Python test
// test_usage_search_aliases_match_python keeps the two in step.
const TCHAR* const KismetSystemLibrary = TEXT("/Script/Engine.KismetSystemLibrary");
const TCHAR* const StandardMacros = TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros");
const TCHAR* const KismetMathLibrary = TEXT("/Script/Engine.KismetMathLibrary");

struct FAlias
{
	const TCHAR* Alias;
	const TCHAR* Kind; // canonical kind, or the whole canonical key when Owner is null
	const TCHAR* Owner;
	const TCHAR* Field;
};

const FAlias Aliases[] = {
	{TEXT("K2:Branch"), TEXT("K2:IfThenElse"), nullptr, nullptr},
	{TEXT("K2:If"), TEXT("K2:IfThenElse"), nullptr, nullptr},
	{TEXT("K2:Sequence"), TEXT("K2:ExecutionSequence"), nullptr, nullptr},
	{TEXT("K2:PrintString"), TEXT("CallFunction"), KismetSystemLibrary, TEXT("PrintString")},
	{TEXT("K2:PrintText"), TEXT("CallFunction"), KismetSystemLibrary, TEXT("PrintText")},
	{TEXT("K2:Delay"), TEXT("CallFunction"), KismetSystemLibrary, TEXT("Delay")},
	{TEXT("K2:IsValidObject"), TEXT("CallFunction"), KismetSystemLibrary, TEXT("IsValid")},
	{TEXT("K2:BeginPlay"), TEXT("Event"), TEXT("/Script/Engine.Actor"), TEXT("ReceiveBeginPlay")},
	{TEXT("K2:Tick"), TEXT("Event"), TEXT("/Script/Engine.Actor"), TEXT("ReceiveTick")},
	{TEXT("K2:EndPlay"), TEXT("Event"), TEXT("/Script/Engine.Actor"), TEXT("ReceiveEndPlay")},
	{TEXT("K2:ActorBeginOverlap"), TEXT("Event"), TEXT("/Script/Engine.Actor"), TEXT("ReceiveActorBeginOverlap")},
	{TEXT("K2:ActorEndOverlap"), TEXT("Event"), TEXT("/Script/Engine.Actor"), TEXT("ReceiveActorEndOverlap")},
	{TEXT("K2:AnyDamage"), TEXT("Event"), TEXT("/Script/Engine.Actor"), TEXT("ReceiveAnyDamage")},
	{TEXT("K2:Destroyed"), TEXT("Event"), TEXT("/Script/Engine.Actor"), TEXT("ReceiveDestroyed")},
	{TEXT("K2:ForLoop"), TEXT("MacroInstance"), StandardMacros, TEXT("ForLoop")},
	{TEXT("K2:ForEachLoop"), TEXT("MacroInstance"), StandardMacros, TEXT("ForEachLoop")},
	{TEXT("K2:ForEachLoopWithBreak"), TEXT("MacroInstance"), StandardMacros, TEXT("ForEachLoopWithBreak")},
	{TEXT("K2:WhileLoop"), TEXT("MacroInstance"), StandardMacros, TEXT("WhileLoop")},
	{TEXT("K2:DoOnce"), TEXT("MacroInstance"), StandardMacros, TEXT("DoOnce")},
	{TEXT("K2:DoN"), TEXT("MacroInstance"), StandardMacros, TEXT("DoN")},
	{TEXT("K2:FlipFlop"), TEXT("MacroInstance"), StandardMacros, TEXT("FlipFlop")},
	{TEXT("K2:Gate"), TEXT("MacroInstance"), StandardMacros, TEXT("Gate")},
	{TEXT("K2:IsValid"), TEXT("MacroInstance"), StandardMacros, TEXT("IsValid")},
	{TEXT("K2:Add"), TEXT("PromotableOperator"), TEXT(""), TEXT("Add")},
	{TEXT("K2:Subtract"), TEXT("PromotableOperator"), TEXT(""), TEXT("Subtract")},
	{TEXT("K2:Multiply"), TEXT("PromotableOperator"), TEXT(""), TEXT("Multiply")},
	{TEXT("K2:Divide"), TEXT("PromotableOperator"), TEXT(""), TEXT("Divide")},
	{TEXT("K2:Greater"), TEXT("PromotableOperator"), TEXT(""), TEXT("Greater")},
	{TEXT("K2:GreaterEqual"), TEXT("PromotableOperator"), TEXT(""), TEXT("GreaterEqual")},
	{TEXT("K2:Less"), TEXT("PromotableOperator"), TEXT(""), TEXT("Less")},
	{TEXT("K2:LessEqual"), TEXT("PromotableOperator"), TEXT(""), TEXT("LessEqual")},
	{TEXT("K2:Equal"), TEXT("PromotableOperator"), TEXT(""), TEXT("EqualEqual")},
	{TEXT("K2:NotEqual"), TEXT("PromotableOperator"), TEXT(""), TEXT("NotEqual")},
	{TEXT("K2:Cast"), TEXT("K2:DynamicCast"), nullptr, nullptr},
	{TEXT("K2:Get"), TEXT("K2:VariableGet"), nullptr, nullptr},
	{TEXT("K2:Set"), TEXT("K2:VariableSet"), nullptr, nullptr},
	{TEXT("K2:Return"), TEXT("K2:FunctionResult"), nullptr, nullptr},
	{TEXT("K2:Reroute"), TEXT("K2:Knot"), nullptr, nullptr},
	{TEXT("K2:Comment"), TEXT("K2:EdGraphNode_Comment"), nullptr, nullptr},
	{TEXT("K2:EqualEqual"), TEXT("PromotableOperator"), TEXT(""), TEXT("EqualEqual")},
	{TEXT("K2:EqualEnum"), TEXT("K2:EnumEquality"), nullptr, nullptr},
	{TEXT("K2:NotEqualEnum"), TEXT("K2:EnumInequality"), nullptr, nullptr},
	{TEXT("K2:SelectFloat"), TEXT("CallFunction"), KismetMathLibrary, TEXT("SelectFloat")},
	{TEXT("K2:SelectInt"), TEXT("CallFunction"), KismetMathLibrary, TEXT("SelectInt")},
	{TEXT("K2:SelectString"), TEXT("CallFunction"), KismetMathLibrary, TEXT("SelectString")},
	{TEXT("K2:SelectName"), TEXT("CallFunction"), KismetMathLibrary, TEXT("SelectName")},
	{TEXT("K2:SelectText"), TEXT("CallFunction"), KismetMathLibrary, TEXT("SelectText")},
	{TEXT("K2:SelectVector"), TEXT("CallFunction"), KismetMathLibrary, TEXT("SelectVector")},
	{TEXT("K2:SelectRotator"), TEXT("CallFunction"), KismetMathLibrary, TEXT("SelectRotator")},
	{TEXT("K2:SelectTransform"), TEXT("CallFunction"), KismetMathLibrary, TEXT("SelectTransform")},
	{TEXT("K2:SelectColor"), TEXT("CallFunction"), KismetMathLibrary, TEXT("SelectColor")},
	{TEXT("K2:SelectObject"), TEXT("CallFunction"), KismetMathLibrary, TEXT("SelectObject")},
	{TEXT("K2:SelectClass"), TEXT("CallFunction"), KismetMathLibrary, TEXT("SelectClass")},
	{TEXT("K2:And"), TEXT("CallFunction"), KismetMathLibrary, TEXT("BooleanAND")},
	{TEXT("K2:Or"), TEXT("CallFunction"), KismetMathLibrary, TEXT("BooleanOR")},
	{TEXT("K2:Xor"), TEXT("CallFunction"), KismetMathLibrary, TEXT("BooleanXOR")},
	{TEXT("K2:Nand"), TEXT("CallFunction"), KismetMathLibrary, TEXT("BooleanNAND")},
	{TEXT("K2:Nor"), TEXT("CallFunction"), KismetMathLibrary, TEXT("BooleanNOR")},
	{TEXT("K2:Not"), TEXT("CallFunction"), KismetMathLibrary, TEXT("Not_PreBool")},
	{TEXT("K2:Abs"), TEXT("CallFunction"), KismetMathLibrary, TEXT("Abs")},
	{TEXT("K2:AbsInt"), TEXT("CallFunction"), KismetMathLibrary, TEXT("Abs_Int")},
	{TEXT("K2:Min"), TEXT("CallFunction"), KismetMathLibrary, TEXT("FMin")},
	{TEXT("K2:Max"), TEXT("CallFunction"), KismetMathLibrary, TEXT("FMax")},
	{TEXT("K2:MinInt"), TEXT("CallFunction"), KismetMathLibrary, TEXT("Min")},
	{TEXT("K2:MaxInt"), TEXT("CallFunction"), KismetMathLibrary, TEXT("Max")},
	{TEXT("K2:Clamp"), TEXT("CallFunction"), KismetMathLibrary, TEXT("FClamp")},
	{TEXT("K2:ClampInt"), TEXT("CallFunction"), KismetMathLibrary, TEXT("Clamp")},
	{TEXT("K2:Lerp"), TEXT("CallFunction"), KismetMathLibrary, TEXT("Lerp")},
	{TEXT("K2:Percent"), TEXT("CallFunction"), KismetMathLibrary, TEXT("Percent_FloatFloat")},
	{TEXT("K2:PercentInt"), TEXT("CallFunction"), KismetMathLibrary, TEXT("Percent_IntInt")},
	{TEXT("K2:NearlyEqual"), TEXT("CallFunction"), KismetMathLibrary, TEXT("NearlyEqual_FloatFloat")},
	{TEXT("K2:InRange"), TEXT("CallFunction"), KismetMathLibrary, TEXT("InRange_FloatFloat")},
};

FString AliasTarget(const FAlias& Alias)
{
	if (Alias.Owner == nullptr)
	{
		return Alias.Kind;
	}
	FString Key = FString(TEXT("K2:")) + Alias.Kind;
	if (*Alias.Owner != 0)
	{
		Key += FString(TEXT(":")) + Alias.Owner;
	}
	return Key + TEXT(":") + Alias.Field;
}

// Members of the asset's own class: "K2:VariableGet:Health" -> "K2:VariableGet:Self:Health" (Self matches any owner).
bool IsSelfMemberKind(const FString& Kind)
{
	static const TCHAR* const Kinds[] = {TEXT("VariableGet"), TEXT("VariableSet"), TEXT("CallDelegate"), TEXT("AddDelegate"),
		TEXT("RemoveDelegate"), TEXT("ClearDelegate"), TEXT("AssignDelegate"), TEXT("CustomEvent")};
	for (const TCHAR* Each : Kinds)
	{
		if (Kind == Each)
		{
			return true;
		}
	}
	return false;
}

FString NormalizeActionKey(const FString& InKey)
{
	FString Key = InKey.TrimStartAndEnd();
	FString Tag;
	FString Rest;
	if (!Key.Split(TEXT(":"), &Tag, &Rest))
	{
		return InKey;
	}
	if (!Tag.Equals(TEXT("K2"), ESearchCase::IgnoreCase))
	{
		return Key;
	}
	Rest.RemoveFromStart(TEXT("K2Node_"));
	Key = TEXT("K2:") + Rest;
	for (const FAlias& Alias : Aliases)
	{
		if (Key.Equals(Alias.Alias, ESearchCase::IgnoreCase))
		{
			return AliasTarget(Alias);
		}
	}
	TArray<FString> Parts;
	Key.ParseIntoArray(Parts, TEXT(":"), false);
	if (Parts.Num() >= 3)
	{
		// Prefix aliases keep their tail ("K2:Get:Health").
		if (Parts[1].Equals(TEXT("Get"), ESearchCase::IgnoreCase))
		{
			Parts[1] = TEXT("VariableGet");
		}
		else if (Parts[1].Equals(TEXT("Set"), ESearchCase::IgnoreCase))
		{
			Parts[1] = TEXT("VariableSet");
		}
		else if (Parts[1].Equals(TEXT("Cast"), ESearchCase::IgnoreCase))
		{
			Parts[1] = TEXT("DynamicCast");
		}
		if (Parts.Num() == 3 && IsSelfMemberKind(Parts[1]))
		{
			Parts.Insert(TEXT("Self"), 2);
		}
	}
	return FString::Join(Parts, TEXT(":"));
}

FString ActionKeyNodeClass(const FString& CanonicalKey)
{
	TArray<FString> Parts;
	CanonicalKey.ParseIntoArray(Parts, TEXT(":"), false);
	if (Parts.Num() < 2 || Parts[1].IsEmpty())
	{
		return FString();
	}
	FString ClassName;
	if (Parts[0] == TEXT("K2"))
	{
		ClassName = TEXT("K2Node_") + Parts[1];
	}
	else if (Parts[0] == TEXT("Anim"))
	{
		ClassName = TEXT("AnimGraphNode_") + Parts[1];
	}
	else
	{
		return FString();
	}
	// Only a class that exists can be a token (a misspelt kind matches nothing either way).
	return FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::ExactClass | EFindFirstObjectOptions::NativeFirst, ELogVerbosity::NoLogging) != nullptr ? ClassName : FString();
}

bool IsBlueprintTag(const FString& Tag)
{
	return Tag == TEXT("K2") || Tag == TEXT("Anim") || Tag == TEXT("SM");
}

// ---- Node records --------------------------------------------------------------------------------------

const UFunction* RootFunction(const UFunction* Function)
{
	while (Function != nullptr && Function->GetSuperFunction() != nullptr)
	{
		Function = Function->GetSuperFunction();
	}
	return Function;
}

FString FunctionOwnerPath(const UFunction* Function)
{
	if (Function == nullptr)
	{
		return FString();
	}
	const UClass* Owner = Function->GetOwnerClass();
	return Owner != nullptr ? K2Keys::OwnerPath(Owner) : K2Keys::OwnerPath(Function->GetOuter());
}

void AddFunction(FAssetDocumentUsageNode& Record, const UFunction* Function)
{
	if (Function == nullptr)
	{
		return;
	}
	Record.Function = Function->GetName();
	Record.FunctionOwners.AddUnique(FunctionOwnerPath(RootFunction(Function)));
	Record.FunctionOwners.AddUnique(FunctionOwnerPath(Function));
}

FAssetDocumentUsageNode LiveRecord(const FGraphEntry& Entry, const UEdGraphNode& Node)
{
	FAssetDocumentUsageNode Record;
	Record.Graph = Entry.Path;
	Record.NodeGuid = FormatAssetDocumentGraphGuid(Node.NodeGuid);
	Record.Title = Node.GetNodeTitle(ENodeTitleType::ListView).ToString();
	Record.NodeClass = Node.GetClass()->GetPathName();
	if (Entry.Schema.IsValid())
	{
		FAssetDocumentGraphNodeDescription Description;
		Entry.Schema->DescribeNode(Node, Description);
		Record.Action = Description.ActionKey;
	}
	if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(&Node))
	{
		AddFunction(Record, Call->GetTargetFunction());
	}
	else if (const UK2Node_Event* Event = Cast<UK2Node_Event>(&Node))
	{
		AddFunction(Record, Event->FindEventSignatureFunction());
	}
	return Record;
}

bool ScanLoaded(UObject& Asset, FAssetDocumentUsageScan& OutScan)
{
	OutScan.Source = TEXT("Loaded");
	FAssetGraphs Graphs;
	FAssetDocumentGraphError Error;
	if (!CollectGraphs(Asset, Graphs, Error))
	{
		OutScan.Reason = TEXT("GraphsUnreadable");
		OutScan.Detail = Error.Message;
		return false;
	}
	for (const FGraphEntry& Entry : Graphs.Graphs)
	{
		if (Entry.Graph == nullptr)
		{
			continue;
		}
		for (const UEdGraphNode* Node : Entry.Graph->Nodes)
		{
			if (Node != nullptr)
			{
				OutScan.Nodes.Add(LiveRecord(Entry, *Node));
			}
		}
	}
	return true;
}

// ---- The saved Find-in-Blueprints index ------------------------------------------------------------------

// Each byte of the index's binary part is stored as one character (byte + 1).
FString EncodeToken(const FString& Token)
{
	FString Encoded;
	Encoded.Reserve(Token.Len());
	for (const TCHAR Char : Token)
	{
		if (Char > 0x7e)
		{
			return FString();
		}
		Encoded.AppendChar(static_cast<TCHAR>(Char + 1));
	}
	return Encoded;
}

// False when some term's tokens are all absent: no node of the asset can match.
bool MayMatch(const FString& Encoded, const FAssetDocumentUsageQuery& Query)
{
	for (const TArray<FAssetDocumentUsageToken>& Tokens : Query.TermTokens)
	{
		if (Tokens.IsEmpty())
		{
			continue;
		}
		bool bFound = false;
		for (const FAssetDocumentUsageToken& Token : Tokens)
		{
			const FString Needle = EncodeToken(Token.Text);
			if (Needle.IsEmpty() || Encoded.Contains(Needle, Token.bIgnoreCase ? ESearchCase::IgnoreCase : ESearchCase::CaseSensitive))
			{
				bFound = true;
				break;
			}
		}
		if (!bFound)
		{
			return false;
		}
	}
	return true;
}

int32 DecodeInt32(const FString& Encoded, int32 Start)
{
	uint32 Value = 0;
	for (int32 Index = 0; Index < 4; ++Index)
	{
		const uint32 Byte = static_cast<uint32>(Encoded[Start + Index] - 1) & 0xff;
		Value |= Byte << (8 * Index);
	}
	return static_cast<int32>(Value);
}

struct FIndexReader
{
	TMap<FString, FString> Lut;
	// The index's own names, compared as the editor wrote them.
	FString Nodes = FFindInBlueprintSearchTags::FiB_Nodes.ToString();
	FString Name = FFindInBlueprintSearchTags::FiB_Name.ToString();
	FString ClassName = FFindInBlueprintSearchTags::FiB_ClassName.ToString();
	FString NodeGuid = FFindInBlueprintSearchTags::FiB_NodeGuid.ToString();
	FString NativeName = FFindInBlueprintSearchTags::FiB_NativeName.ToString();
	FString FuncOriginClass = FFindInBlueprintSearchTags::FiB_FuncOriginClass.ToString();
	FString Pins = FFindInBlueprintSearchTags::FiB_Pins.ToString();
	FString ObjectClass = FFindInBlueprintSearchTags::FiB_ObjectClass.ToString();
	TArray<FString> GraphLists = {
		FFindInBlueprintSearchTags::FiB_UberGraphs.ToString(), FFindInBlueprintSearchTags::FiB_Functions.ToString(),
		FFindInBlueprintSearchTags::FiB_Macros.ToString(), FFindInBlueprintSearchTags::FiB_SubGraphs.ToString(),
		FFindInBlueprintSearchTags::FiB_ExtensionGraphs.ToString()};

	FString Text(const FString& Ref) const
	{
		const FString* Found = Lut.Find(Ref);
		return Found != nullptr ? *Found : Ref;
	}

	FString Value(const TSharedPtr<FJsonValue>& JsonValue) const
	{
		FString Raw;
		return JsonValue.IsValid() && JsonValue->TryGetString(Raw) ? Text(Raw) : FString();
	}

	// Fields of one object by their names.
	TMap<FString, TSharedPtr<FJsonValue>> Fields(const FJsonObject& Object) const
	{
		TMap<FString, TSharedPtr<FJsonValue>> Out;
		for (const auto& Field : Object.Values)
		{
			Out.Add(Text(FString(*Field.Key)), Field.Value);
		}
		return Out;
	}

	// A searchable property's leaf value by name, anywhere below Object.
	FString FindLeaf(const FJsonObject& Object, const FString& LeafName, int32 Depth = 0) const
	{
		if (Depth > 6)
		{
			return FString();
		}
		for (const auto& Field : Object.Values)
		{
			const bool bNamed = Text(FString(*Field.Key)) == LeafName;
			const TSharedPtr<FJsonObject>* Child = nullptr;
			if (Field.Value.IsValid() && Field.Value->TryGetObject(Child) && Child != nullptr)
			{
				const FString Found = FindLeaf(**Child, LeafName, Depth + 1);
				if (!Found.IsEmpty())
				{
					return Found;
				}
			}
			else if (bNamed)
			{
				return Value(Field.Value);
			}
		}
		return FString();
	}
};

const UClass* FindNodeClass(const FString& ShortName)
{
	// Node classes are native; a name no loaded class has (a removed node class) is looked up once per session.
	static TMap<FString, TWeakObjectPtr<const UClass>> Cache;
	static TSet<FString> Missing;
	if (ShortName.IsEmpty() || Missing.Contains(ShortName))
	{
		return nullptr;
	}
	if (const TWeakObjectPtr<const UClass>* Found = Cache.Find(ShortName))
	{
		if (Found->IsValid())
		{
			return Found->Get();
		}
	}
	const UClass* Class = FindFirstObject<UClass>(*ShortName, EFindFirstObjectOptions::ExactClass | EFindFirstObjectOptions::NativeFirst, ELogVerbosity::NoLogging);
	if (Class == nullptr)
	{
		Missing.Add(ShortName);
		return nullptr;
	}
	Cache.Add(ShortName, Class);
	return Class;
}

// The function an index node calls: the first of its pins' classes that has a function of that name.
const UFunction* ResolveIndexedFunction(const FString& Name, const TArray<FString>& OwnerCandidates)
{
	const FName FunctionName(*Name);
	for (const FString& Candidate : OwnerCandidates)
	{
		const UClass* Class = FindObject<UClass>(nullptr, *Candidate);
		if (const UFunction* Function = Class != nullptr ? Class->FindFunctionByName(FunctionName) : nullptr)
		{
			return Function;
		}
	}
	return nullptr;
}

FString JoinKey(const FString& Kind, const FString& Owner, const FString& Field)
{
	return FString::Printf(TEXT("K2:%s:%s:%s"), *Kind, *Owner, *Field);
}

void DeriveIndexedKey(FAssetDocumentUsageNode& Record, const FString& ClassShortName, const UClass* NodeClass, const FString& FuncOrigin,
	const FJsonObject& NodeObject, const FIndexReader& Reader, const FString& SelfClassPath)
{
	if (ClassShortName.StartsWith(TEXT("AnimGraphNode_")))
	{
		Record.Action = TEXT("Anim:") + ClassShortName.Mid(14);
		return;
	}
	if (!ClassShortName.StartsWith(TEXT("K2Node_")))
	{
		return;
	}
	const FString Kind = ClassShortName.Mid(7);
	if (NodeClass != nullptr && NodeClass->IsChildOf(UK2Node_CallFunction::StaticClass()))
	{
		const UFunction* Function = ResolveIndexedFunction(Record.Function, Record.FunctionOwners);
		if (Function != nullptr)
		{
			// Owners as a loaded node reports them (declaring class first).
			const TArray<FString> PinClasses = Record.FunctionOwners;
			Record.FunctionOwners.Reset();
			AddFunction(Record, Function);
			for (const FString& Each : PinClasses)
			{
				Record.FunctionOwners.AddUnique(Each);
			}
		}
		if (NodeClass->IsChildOf(UK2Node_PromotableOperator::StaticClass()))
		{
			FString Operation = Function != nullptr ? FTypePromotion::GetOpNameFromFunction(Function).ToString() : FString();
			if (Operation.IsEmpty() || Operation == TEXT("None"))
			{
				Record.Function.Split(TEXT("_"), &Operation, nullptr);
			}
			Record.Action = TEXT("K2:PromotableOperator:") + (Operation.IsEmpty() ? Record.Function : Operation);
			return;
		}
		const FString Owner = Function != nullptr
			? FunctionOwnerPath(NodeClass->IsChildOf(UK2Node_CallParentFunction::StaticClass()) ? Function : RootFunction(Function))
			: FuncOrigin;
		Record.Action = JoinKey(Kind, Owner, Record.Function);
		return;
	}
	if (NodeClass != nullptr && NodeClass->IsChildOf(UK2Node_CustomEvent::StaticClass()))
	{
		Record.Action = TEXT("K2:") + Kind;
		return;
	}
	if (NodeClass == UK2Node_Event::StaticClass() && !Record.Function.IsEmpty())
	{
		Record.Action = JoinKey(Kind, FuncOrigin, Record.Function);
		return;
	}
	if (NodeClass != nullptr && NodeClass->IsChildOf(UK2Node_Variable::StaticClass()) && !NodeClass->IsChildOf(UK2Node_StructOperation::StaticClass()))
	{
		const FString Member = Reader.FindLeaf(NodeObject, TEXT("MemberName"));
		if (!Member.IsEmpty())
		{
			FString Owner = FPackageName::ExportTextPathToObjectPath(Reader.FindLeaf(NodeObject, TEXT("MemberParent")));
			const FString Scope = Reader.FindLeaf(NodeObject, TEXT("MemberScope"));
			if (Owner.IsEmpty() || Owner == TEXT("None"))
			{
				Owner = SelfClassPath;
			}
			if (!Scope.IsEmpty())
			{
				Owner = SelfClassPath + TEXT(":") + Scope;
			}
			Record.Action = JoinKey(Kind, Owner, Member);
			return;
		}
	}
	if (NodeClass != nullptr && NodeClass->IsChildOf(UK2Node_MacroInstance::StaticClass()))
	{
		// The index records the macro's title (its graph name, with display spaces), not the macro library.
		Record.Action = JoinKey(Kind, FString(), Record.Title.Replace(TEXT(" "), TEXT("")));
		return;
	}
	Record.Action = TEXT("K2:") + Kind;
}

void ReadIndexedGraph(const FJsonObject& Graph, const FIndexReader& Reader, const FString& SelfClassPath, FAssetDocumentUsageScan& OutScan)
{
	const TMap<FString, TSharedPtr<FJsonValue>> GraphFields = Reader.Fields(Graph);
	const FString GraphName = GraphFields.Contains(Reader.Name) ? Reader.Value(GraphFields[Reader.Name]) : FString();
	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	if (!GraphFields.Contains(Reader.Nodes) || !GraphFields[Reader.Nodes]->TryGetArray(Nodes))
	{
		return;
	}
	for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
	{
		const TSharedPtr<FJsonObject>* NodeObject = nullptr;
		if (!NodeValue.IsValid() || !NodeValue->TryGetObject(NodeObject) || NodeObject == nullptr)
		{
			continue;
		}
		const TMap<FString, TSharedPtr<FJsonValue>> Fields = Reader.Fields(**NodeObject);
		auto Field = [&Fields, &Reader](const FString& Key)
		{
			const TSharedPtr<FJsonValue>* Found = Fields.Find(Key);
			return Found != nullptr ? Reader.Value(*Found) : FString();
		};
		FAssetDocumentUsageNode& Record = OutScan.Nodes.AddDefaulted_GetRef();
		Record.Graph = GraphName;
		Record.NodeGuid = Field(Reader.NodeGuid);
		Record.Title = Field(Reader.Name);
		const FString ClassShortName = Field(Reader.ClassName);
		const UClass* NodeClass = FindNodeClass(ClassShortName);
		Record.NodeClass = NodeClass != nullptr ? NodeClass->GetPathName() : ClassShortName;
		Record.Function = Field(Reader.NativeName);
		const FString FuncOrigin = Field(Reader.FuncOriginClass);
		if (!Record.Function.IsEmpty())
		{
			if (!FuncOrigin.IsEmpty())
			{
				Record.FunctionOwners.Add(FuncOrigin);
			}
			const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
			if (Fields.Contains(Reader.Pins) && Fields[Reader.Pins]->TryGetArray(Pins))
			{
				for (const TSharedPtr<FJsonValue>& PinValue : *Pins)
				{
					const TSharedPtr<FJsonObject>* Pin = nullptr;
					if (PinValue.IsValid() && PinValue->TryGetObject(Pin) && Pin != nullptr)
					{
						const TMap<FString, TSharedPtr<FJsonValue>> PinFields = Reader.Fields(**Pin);
						if (const TSharedPtr<FJsonValue>* ObjectClass = PinFields.Find(Reader.ObjectClass))
						{
							const FString Path = Reader.Value(*ObjectClass);
							if (Path.StartsWith(TEXT("/")))
							{
								Record.FunctionOwners.AddUnique(Path);
							}
						}
					}
				}
			}
		}
		DeriveIndexedKey(Record, ClassShortName, NodeClass, FuncOrigin, **NodeObject, Reader, SelfClassPath);
	}
}

// The Find-in-Blueprints version info of the asset's saved search data: the data version, and the custom versions its
// package was saved with (the current ones when the registry does not know them).
FSearchDataVersionInfo SearchDataVersionOf(const FAssetData& Asset, const FString& Encoded)
{
	FSearchDataVersionInfo VersionInfo;
	VersionInfo.FiBDataVersion = Encoded.Len() >= 4 ? DecodeInt32(Encoded, 0) : EFiBVersion::FIB_VER_NONE;
	FAssetPackageData PackageData;
	const IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	const bool bKnown = Registry.TryGetAssetPackageData(Asset.PackageName, PackageData) == UE::AssetRegistry::EExists::Exists;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	if (bKnown)
	{
		for (const UE::AssetRegistry::FPackageCustomVersion& Version : PackageData.GetCustomVersions())
		{
			VersionInfo.CustomVersions.SetVersion(Version.Key, Version.Version, NAME_None);
		}
	}
	if (VersionInfo.CustomVersions.GetAllVersions().Num() == 0)
	{
		VersionInfo.CustomVersions = FCurrentCustomVersions::GetAll();
	}
#else
	VersionInfo.EditorObjectVersion = FEditorObjectVersion::LatestVersion;
	if (bKnown)
	{
		for (const UE::AssetRegistry::FPackageCustomVersion& Version : PackageData.GetCustomVersions())
		{
			if (Version.Key == FEditorObjectVersion::GUID)
			{
				VersionInfo.EditorObjectVersion = Version.Version;
			}
		}
	}
#endif
	return VersionInfo;
}

FString SelfClassPathOf(const FAssetData& Asset)
{
	FString Generated;
	if (Asset.GetTagValue(FBlueprintTags::GeneratedClassPath, Generated) && !Generated.IsEmpty())
	{
		return FPackageName::ExportTextPathToObjectPath(Generated);
	}
	return Asset.GetObjectPathString() + TEXT("_C");
}

bool ScanIndex(const FAssetData& Asset, const FString& Encoded, const FAssetDocumentUsageQuery& Query, FAssetDocumentUsageScan& OutScan)
{
	OutScan.Source = TEXT("Index");
	if (!MayMatch(Encoded, Query))
	{
		return true;
	}
	FSearchDataVersionInfo VersionInfo = SearchDataVersionOf(Asset, Encoded);
	if (VersionInfo.FiBDataVersion <= EFiBVersion::FIB_VER_BASE || VersionInfo.FiBDataVersion > EFiBVersion::FIB_VER_LATEST)
	{
		OutScan.Reason = TEXT("IndexUnreadable");
		OutScan.Detail = TEXT("The saved Find-in-Blueprints data has a version this editor does not read; resave the asset.");
		return false;
	}
	TMap<int32, FText> Lut;
	const TSharedPtr<FJsonObject> Root = FFindInBlueprintSearchManager::ConvertJsonStringToObject(VersionInfo, Encoded, Lut);
	if (!Root.IsValid())
	{
		OutScan.Reason = TEXT("IndexUnreadable");
		OutScan.Detail = TEXT("The saved Find-in-Blueprints data could not be parsed; resave the asset.");
		return false;
	}
	FIndexReader Reader;
	for (const TPair<int32, FText>& Entry : Lut)
	{
		Reader.Lut.Add(FString::FromInt(Entry.Key), Entry.Value.ToString());
	}
	const FString SelfClassPath = SelfClassPathOf(Asset);
	for (const auto& Field : Root->Values)
	{
		const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
		if (!Reader.GraphLists.Contains(Reader.Text(FString(*Field.Key))) || !Field.Value.IsValid() || !Field.Value->TryGetArray(Graphs))
		{
			continue;
		}
		for (const TSharedPtr<FJsonValue>& GraphValue : *Graphs)
		{
			const TSharedPtr<FJsonObject>* Graph = nullptr;
			if (GraphValue.IsValid() && GraphValue->TryGetObject(Graph) && Graph != nullptr)
			{
				ReadIndexedGraph(**Graph, Reader, SelfClassPath, OutScan);
			}
		}
	}
	return true;
}

const UClass* FindAssetClass(const FTopLevelAssetPath& ClassPath)
{
	static TMap<FTopLevelAssetPath, TWeakObjectPtr<const UClass>> Cache;
	if (const TWeakObjectPtr<const UClass>* Found = Cache.Find(ClassPath))
	{
		if (Found->IsValid())
		{
			return Found->Get();
		}
	}
	const UClass* Class = FindObject<UClass>(ClassPath);
	Cache.Add(ClassPath, Class);
	return Class;
}

bool HasGraphHost(const UClass& AssetClass)
{
	static TMap<const UClass*, bool> Cache;
	if (const bool* Found = Cache.Find(&AssetClass))
	{
		return *Found;
	}
	const bool bHost = FindAssetDocumentGraphHostAdapter(AssetClass.GetDefaultObject()).IsValid();
	Cache.Add(&AssetClass, bHost);
	return bHost;
}

EAssetDocumentUsageScan Scan(const FAssetData& Asset, const FAssetDocumentUsageQuery& Query, FAssetDocumentUsageScan& OutScan)
{
	const UClass* AssetClass = FindAssetClass(Asset.AssetClassPath);
	if (AssetClass == nullptr)
	{
		return EAssetDocumentUsageScan::NotHandled;
	}
	const bool bBlueprint = AssetClass->IsChildOf(UBlueprint::StaticClass());
	const bool bWorld = AssetClass->IsChildOf(UWorld::StaticClass());
	UObject* Loaded = Asset.FastGetAsset(false);
	const bool bDirty = Loaded != nullptr && Loaded->GetPackage()->IsDirty();
	if (bBlueprint || bWorld)
	{
		UObject* GraphOwner = Loaded;
		if (UWorld* World = Cast<UWorld>(Loaded))
		{
			GraphOwner = World->PersistentLevel != nullptr ? World->PersistentLevel->GetLevelScriptBlueprint(true) : nullptr;
		}
		FString Encoded;
		const bool bIndexed = Asset.GetTagValue(FBlueprintTags::FindInBlueprintsData, Encoded) && !Encoded.IsEmpty();
		if (GraphOwner != nullptr && (bDirty || !bIndexed))
		{
			return ScanLoaded(*GraphOwner, OutScan) ? EAssetDocumentUsageScan::Searched : EAssetDocumentUsageScan::NotSearched;
		}
		if (bIndexed)
		{
			return ScanIndex(Asset, Encoded, Query, OutScan) ? EAssetDocumentUsageScan::Searched : EAssetDocumentUsageScan::NotSearched;
		}
		if (bWorld)
		{
			// A map without index data has no Level Script graphs the editor knows of.
			return EAssetDocumentUsageScan::NotHandled;
		}
		OutScan.Reason = TEXT("NotIndexed");
		OutScan.Detail = TEXT("The Blueprint was saved without Find-in-Blueprints data (an older engine version or a commandlet); it is searched while loaded (open it), or after it is resaved.");
		return EAssetDocumentUsageScan::NotSearched;
	}
	if (!HasGraphHost(*AssetClass) || Query.bBlueprintTermsOnly)
	{
		return EAssetDocumentUsageScan::NotHandled;
	}
	if (Loaded != nullptr)
	{
		return ScanLoaded(*Loaded, OutScan) ? EAssetDocumentUsageScan::Searched : EAssetDocumentUsageScan::NotSearched;
	}
	OutScan.Reason = TEXT("NotLoaded");
	OutScan.Detail = TEXT("This asset type has no saved node index: its graphs are searched only while it is loaded (open it); an asset it uses can be searched by its path.");
	return EAssetDocumentUsageScan::NotSearched;
}
}

void RegisterGraphUsageScanner()
{
	FAssetDocumentUsageScanner Scanner;
	Scanner.Scan = &UsageScan::Scan;
	Scanner.NormalizeActionKey = &UsageScan::NormalizeActionKey;
	Scanner.ActionKeyNodeClass = &UsageScan::ActionKeyNodeClass;
	Scanner.IsBlueprintTag = &UsageScan::IsBlueprintTag;
	RegisterAssetDocumentUsageScanner(TEXT("graph"), Scanner);
}

void UnregisterGraphUsageScanner()
{
	UnregisterAssetDocumentUsageScanner(TEXT("graph"));
}
} // namespace UE::AssetDocument::Graph
