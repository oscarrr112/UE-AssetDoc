// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentUsageSearch.h"

#include "Discovery/AssetDocumentUsageSearchInternal.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphNode.h"
#include "K2Node.h"
#include "Misc/Paths.h"
#include "UObject/Class.h"
#include "UObject/UObjectHash.h"

namespace
{
TArray<TPair<FName, FAssetDocumentUsageScanner>>& GetUsageScanners()
{
	static TArray<TPair<FName, FAssetDocumentUsageScanner>> Scanners;
	return Scanners;
}
}

bool RegisterAssetDocumentUsageScanner(FName ScannerId, const FAssetDocumentUsageScanner& Scanner)
{
	check(IsInGameThread());
	TArray<TPair<FName, FAssetDocumentUsageScanner>>& Scanners = GetUsageScanners();
	if (ScannerId.IsNone() || Scanner.Scan == nullptr
		|| Scanners.ContainsByPredicate([ScannerId](const TPair<FName, FAssetDocumentUsageScanner>& Each) { return Each.Key == ScannerId; }))
	{
		return false;
	}
	Scanners.Emplace(ScannerId, Scanner);
	return true;
}

bool UnregisterAssetDocumentUsageScanner(FName ScannerId)
{
	check(IsInGameThread());
	return GetUsageScanners().RemoveAll([ScannerId](const TPair<FName, FAssetDocumentUsageScanner>& Each)
	{
		return Each.Key == ScannerId;
	}) > 0;
}

namespace UE::AssetDocument::Discovery
{
namespace UsageSearch
{
constexpr int32 MaxListedNodes = 20;
constexpr int32 MaxListedNotSearched = 20;

enum class ETermKind : uint8
{
	ActionKey,
	NodeClass,
	Function,
	Asset,
};

struct FTermAlternative
{
	ETermKind Kind = ETermKind::ActionKey;
	FString Text;
	// ActionKey: the canonical key and its segments (wildcard keys match the whole key).
	FString Key;
	TArray<FString> Segments;
	bool bWildcard = false;
	// NodeClass / Function owner.
	const UClass* Class = nullptr;
	FString OwnerPath;
	FString FunctionName;
	// Asset: packages that reference it.
	TSet<FName> Referencers;
	TArray<FAssetDocumentUsageToken> Tokens;
	bool bBlueprintOnly = false;
};

struct FTerm
{
	FString Text;
	TArray<FTermAlternative> Alternatives;
	bool HasNodeAlternative() const
	{
		return Alternatives.ContainsByPredicate([](const FTermAlternative& Alt) { return Alt.Kind != ETermKind::Asset; });
	}
};

IAssetRegistry& Registry()
{
	return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
}

bool IsStaleTypeName(const FString& Name)
{
	return Name.StartsWith(TEXT("SKEL_")) || Name.StartsWith(TEXT("REINST_")) || Name.StartsWith(TEXT("TRASHCLASS_"))
		|| Name.StartsWith(TEXT("HOTRELOADED_")) || Name.StartsWith(TEXT("PLACEHOLDER-")) || Name.StartsWith(TEXT("LIVECODING_"));
}

// A token the encoded index is searched for. Upper and lower case letters stay letters once encoded (each byte
// is stored +1), except 'Z' / 'z'; a case-insensitive name with one of those gets no token.
void AddNameToken(TArray<FAssetDocumentUsageToken>& Tokens, const FString& Name, bool bIgnoreCase)
{
	if (Name.Len() < 3 || (bIgnoreCase && (Name.Contains(TEXT("z"), ESearchCase::IgnoreCase))))
	{
		return;
	}
	Tokens.Add({Name, bIgnoreCase});
}

const UClass* FindLoadedClass(const FString& PathOrName)
{
	if (PathOrName.StartsWith(TEXT("/")))
	{
		return FindObject<UClass>(nullptr, *PathOrName);
	}
	return FindFirstObject<UClass>(*PathOrName, EFindFirstObjectOptions::NativeFirst, ELogVerbosity::NoLogging);
}

void SetClassAlternative(FTermAlternative& Alt, const UClass* Class)
{
	Alt.Class = Class;
	if (Class->IsChildOf(UEdGraphNode::StaticClass()))
	{
		Alt.Kind = ETermKind::NodeClass;
		Alt.bBlueprintOnly = Class->IsChildOf(UK2Node::StaticClass());
		TArray<UClass*> Derived;
		GetDerivedClasses(Class, Derived, true);
		Derived.Add(const_cast<UClass*>(Class));
		for (const UClass* Each : Derived)
		{
			if (!IsStaleTypeName(Each->GetName()))
			{
				AddNameToken(Alt.Tokens, Each->GetName(), false);
			}
		}
		return;
	}
	// Any function of a class (a function library, Actor).
	Alt.Kind = ETermKind::Function;
	Alt.bBlueprintOnly = true;
	Alt.OwnerPath = Class->GetPathName();
	AddNameToken(Alt.Tokens, Class->GetName(), false);
}

bool ParseAlternative(const FString& Text, FTermAlternative& Alt, FString& OutError)
{
	Alt.Text = Text;
	if (Text.StartsWith(TEXT("/")))
	{
		FString Owner;
		FString Function;
		if (Text.Split(TEXT(":"), &Owner, &Function) && !Function.IsEmpty())
		{
			// "<class path>:<function>".
			Alt.Kind = ETermKind::Function;
			Alt.bBlueprintOnly = true;
			Alt.OwnerPath = Owner;
			Alt.Class = FindLoadedClass(Owner);
			Alt.FunctionName = Function;
			const UFunction* Resolved = Alt.Class != nullptr ? Alt.Class->FindFunctionByName(FName(*Function)) : nullptr;
			AddNameToken(Alt.Tokens, Resolved != nullptr ? Resolved->GetName() : Function, Resolved == nullptr);
			return true;
		}
		if (const UClass* Class = FindLoadedClass(Text))
		{
			SetClassAlternative(Alt, Class);
			return true;
		}
		if (const UFunction* Function_ = FindObject<UFunction>(nullptr, *Text))
		{
			// "/Script/Engine.KismetArrayLibrary.Array_Length".
			Alt.Kind = ETermKind::Function;
			Alt.bBlueprintOnly = true;
			Alt.Class = Function_->GetOwnerClass();
			Alt.OwnerPath = Alt.Class != nullptr ? Alt.Class->GetPathName() : FString();
			Alt.FunctionName = Function_->GetName();
			AddNameToken(Alt.Tokens, Alt.FunctionName, false);
			return true;
		}
		FString ObjectPath = Text;
		if (!ObjectPath.Contains(TEXT(".")))
		{
			ObjectPath += TEXT(".") + FPaths::GetBaseFilename(Text);
		}
		const FAssetData Asset = Registry().GetAssetByObjectPath(FSoftObjectPath(ObjectPath), true);
		if (!Asset.IsValid())
		{
			OutError = FString::Printf(TEXT("uses '%s' names no class, function or asset (a node class such as /Script/BlueprintGraph.K2Node_CallArrayFunction, a function such as /Script/Engine.KismetMathLibrary:Add_VectorVector, or an asset path)."), *Text);
			return false;
		}
		Alt.Kind = ETermKind::Asset;
		TArray<FName> Referencers;
		Registry().GetReferencers(Asset.PackageName, Referencers, UE::AssetRegistry::EDependencyCategory::Package);
		Alt.Referencers.Append(Referencers);
		return true;
	}
	int32 Colon = INDEX_NONE;
	if (Text.FindChar(TEXT(':'), Colon))
	{
		const FString Tag = Text.Left(Colon);
		if (Colon == 0 || Colon == Text.Len() - 1)
		{
			OutError = FString::Printf(TEXT("uses '%s' is not an action key <Tag>:<Kind>[:<Owner>][:<Field>] (e.g. K2:PromotableOperator:Add, K2:CallArrayFunction)."), *Text);
			return false;
		}
		Alt.Kind = ETermKind::ActionKey;
		Alt.Key = Text;
		for (const TPair<FName, FAssetDocumentUsageScanner>& Each : GetUsageScanners())
		{
			if (Each.Value.NormalizeActionKey != nullptr)
			{
				const FString Normalized = Each.Value.NormalizeActionKey(Text);
				if (Normalized != Text)
				{
					Alt.Key = Normalized;
					break;
				}
			}
		}
		Alt.bWildcard = Alt.Key.Contains(TEXT("*")) || Alt.Key.Contains(TEXT("?"));
		Alt.Key.ParseIntoArray(Alt.Segments, TEXT(":"), false);
		for (const TPair<FName, FAssetDocumentUsageScanner>& Each : GetUsageScanners())
		{
			if (Each.Value.IsBlueprintTag != nullptr && Each.Value.IsBlueprintTag(Tag))
			{
				Alt.bBlueprintOnly = true;
			}
			if (!Alt.bWildcard && Each.Value.ActionKeyNodeClass != nullptr && Alt.Tokens.IsEmpty())
			{
				AddNameToken(Alt.Tokens, Each.Value.ActionKeyNodeClass(Alt.Key), false);
			}
		}
		return true;
	}
	if (const UClass* Class = FindLoadedClass(Text))
	{
		SetClassAlternative(Alt, Class);
		return true;
	}
	// A bare function name.
	Alt.Kind = ETermKind::Function;
	Alt.bBlueprintOnly = true;
	Alt.FunctionName = Text;
	AddNameToken(Alt.Tokens, Text, true);
	return true;
}

bool ParseTerms(const FString& Uses, TArray<FTerm>& OutTerms, FString& OutError)
{
	TArray<FString> TermTexts;
	Uses.ParseIntoArray(TermTexts, TEXT(","), true);
	for (FString& TermText : TermTexts)
	{
		TermText.TrimStartAndEndInline();
		if (TermText.IsEmpty())
		{
			continue;
		}
		FTerm& Term = OutTerms.AddDefaulted_GetRef();
		Term.Text = TermText;
		TArray<FString> AltTexts;
		TermText.ParseIntoArray(AltTexts, TEXT("|"), true);
		for (FString& AltText : AltTexts)
		{
			AltText.TrimStartAndEndInline();
			if (AltText.IsEmpty())
			{
				continue;
			}
			if (!ParseAlternative(AltText, Term.Alternatives.AddDefaulted_GetRef(), OutError))
			{
				return false;
			}
		}
		if (Term.Alternatives.IsEmpty())
		{
			OutTerms.Pop();
		}
	}
	if (OutTerms.IsEmpty())
	{
		OutError = TEXT("'uses' lists no term; give action keys, node classes, functions or asset paths, comma-separated (all must match), alternatives separated by |.");
		return false;
	}
	return true;
}

struct FMatchContext
{
	TMap<FString, const UClass*> Classes;

	const UClass* Resolve(const FString& Path)
	{
		if (const UClass** Found = Classes.Find(Path))
		{
			return *Found;
		}
		const UClass* Class = Path.StartsWith(TEXT("/")) ? FindObject<UClass>(nullptr, *Path) : nullptr;
		Classes.Add(Path, Class);
		return Class;
	}
};

bool KeyMatches(const FString& NodeKey, const FTermAlternative& Alt)
{
	TArray<FString> Segments;
	NodeKey.ParseIntoArray(Segments, TEXT(":"), false);
	// A node key read from a saved index may lack its owner (a macro instance records only its macro's name):
	// that owner stands for any owner when the key names its member. Any other empty segment (a call node
	// whose function no longer resolves records neither) is unknown: the key is known only up to it, and a
	// term naming more of it does not match.
	const bool bOwnerUnknown = Segments.Num() >= 4 && Segments[2].IsEmpty() && !Segments.Last().IsEmpty();
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		if (Segments[Index].IsEmpty() && !(Index == 2 && bOwnerUnknown))
		{
			Segments.SetNum(Index);
			break;
		}
	}
	if (Segments.Num() < 2)
	{
		return false;
	}
	if (Alt.bWildcard)
	{
		return FString::Join(Segments, TEXT(":")).MatchesWildcard(Alt.Key, ESearchCase::IgnoreCase);
	}
	if (Alt.Segments.Num() > Segments.Num())
	{
		return false;
	}
	for (int32 Index = 0; Index < Alt.Segments.Num(); ++Index)
	{
		const FString& Wanted = Alt.Segments[Index];
		const FString& Have = Segments[Index];
		if (Have.IsEmpty() || (Index >= 2 && Wanted == TEXT("Self")) || Wanted.Equals(Have, ESearchCase::IgnoreCase))
		{
			continue;
		}
		// The term's last segment without the node's @binding / #disambiguator.
		const bool bLast = Index == Alt.Segments.Num() - 1;
		if (bLast && Have.Len() > Wanted.Len() && Have.StartsWith(Wanted, ESearchCase::IgnoreCase)
			&& (Have[Wanted.Len()] == TEXT('@') || Have[Wanted.Len()] == TEXT('#')))
		{
			continue;
		}
		return false;
	}
	return true;
}

bool NodeMatches(const FAssetDocumentUsageNode& Node, const FTermAlternative& Alt, FMatchContext& Context)
{
	switch (Alt.Kind)
	{
	case ETermKind::ActionKey:
		return KeyMatches(Node.Action, Alt);
	case ETermKind::NodeClass:
	{
		const UClass* NodeClass = Context.Resolve(Node.NodeClass);
		return NodeClass != nullptr ? NodeClass->IsChildOf(Alt.Class) : Node.NodeClass.Equals(Alt.Class->GetPathName(), ESearchCase::IgnoreCase);
	}
	case ETermKind::Function:
	{
		if (Node.Function.IsEmpty() || (!Alt.FunctionName.IsEmpty() && !Node.Function.Equals(Alt.FunctionName, ESearchCase::IgnoreCase)))
		{
			return false;
		}
		if (Alt.OwnerPath.IsEmpty())
		{
			return true;
		}
		for (const FString& Owner : Node.FunctionOwners)
		{
			if (Owner.Equals(Alt.OwnerPath, ESearchCase::IgnoreCase))
			{
				return true;
			}
			const UClass* OwnerClass = Alt.Class != nullptr ? Context.Resolve(Owner) : nullptr;
			if (OwnerClass != nullptr && OwnerClass->IsChildOf(Alt.Class))
			{
				return true;
			}
		}
		return false;
	}
	default:
		return false;
	}
}

// Asset-level alternatives (references) of every term; terms with node alternatives may still be met by nodes.
bool AssetTermsMet(const FAssetData& Asset, const TArray<FTerm>& Terms, TArray<bool>& OutTermMet)
{
	OutTermMet.Init(false, Terms.Num());
	bool bAllPossible = true;
	for (int32 Index = 0; Index < Terms.Num(); ++Index)
	{
		for (const FTermAlternative& Alt : Terms[Index].Alternatives)
		{
			if (Alt.Kind == ETermKind::Asset && Alt.Referencers.Contains(Asset.PackageName))
			{
				OutTermMet[Index] = true;
			}
		}
		bAllPossible &= OutTermMet[Index] || Terms[Index].HasNodeAlternative();
	}
	return bAllPossible;
}

}

bool FilterAssetsByUsage(const FString& Uses, const TArray<FAssetData>& Candidates, TArray<TSharedPtr<FJsonObject>>& OutRows,
	TSharedPtr<FJsonObject>& OutSummary, FString& OutError)
{
	using namespace UsageSearch;
	TArray<FTerm> Terms;
	if (!ParseTerms(Uses, Terms, OutError))
	{
		return false;
	}
	FAssetDocumentUsageQuery Query;
	Query.bBlueprintTermsOnly = true;
	bool bAnyNodeTerm = false;
	for (const FTerm& Term : Terms)
	{
		TArray<FAssetDocumentUsageToken>& Tokens = Query.TermTokens.AddDefaulted_GetRef();
		bool bAllHaveTokens = true;
		for (const FTermAlternative& Alt : Term.Alternatives)
		{
			bAllHaveTokens &= !Alt.Tokens.IsEmpty();
			Tokens.Append(Alt.Tokens);
			Query.bBlueprintTermsOnly &= Alt.Kind == ETermKind::Asset || Alt.bBlueprintOnly;
		}
		if (!bAllHaveTokens)
		{
			Tokens.Reset();
		}
		bAnyNodeTerm |= Term.HasNodeAlternative();
	}

	FMatchContext Context;
	int32 Searched = 0;
	TArray<TSharedPtr<FJsonValue>> NotSearched;
	int32 NotSearchedTotal = 0;
	TMap<FString, TPair<int32, FString>> NotSearchedByReason;
	for (const FAssetData& Asset : Candidates)
	{
		TArray<bool> TermMet;
		if (!AssetTermsMet(Asset, Terms, TermMet))
		{
			continue;
		}
		FAssetDocumentUsageScan Scan;
		EAssetDocumentUsageScan Status = EAssetDocumentUsageScan::NotHandled;
		if (bAnyNodeTerm)
		{
			for (const TPair<FName, FAssetDocumentUsageScanner>& Each : GetUsageScanners())
			{
				Scan = FAssetDocumentUsageScan();
				Status = Each.Value.Scan(Asset, Query, Scan);
				if (Status != EAssetDocumentUsageScan::NotHandled)
				{
					break;
				}
			}
		}
		if (Status == EAssetDocumentUsageScan::NotSearched)
		{
			++NotSearchedTotal;
			TPair<int32, FString>& Reason = NotSearchedByReason.FindOrAdd(Scan.Reason, TPair<int32, FString>(0, Scan.Detail));
			++Reason.Key;
			if (NotSearched.Num() < MaxListedNotSearched)
			{
				TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("ObjectPath"), Asset.GetObjectPathString());
				Row->SetStringField(TEXT("Class"), Asset.AssetClassPath.ToString());
				Row->SetStringField(TEXT("Reason"), Scan.Reason);
				NotSearched.Add(MakeShared<FJsonValueObject>(Row));
			}
			continue;
		}
		if (Status == EAssetDocumentUsageScan::Searched)
		{
			++Searched;
		}
		// Matching nodes, per graph in report order.
		TArray<const FAssetDocumentUsageNode*> Matching;
		for (const FAssetDocumentUsageNode& Node : Scan.Nodes)
		{
			bool bMatched = false;
			for (int32 Index = 0; Index < Terms.Num(); ++Index)
			{
				for (const FTermAlternative& Alt : Terms[Index].Alternatives)
				{
					if (Alt.Kind != ETermKind::Asset && NodeMatches(Node, Alt, Context))
					{
						TermMet[Index] = true;
						bMatched = true;
						break;
					}
				}
			}
			if (bMatched)
			{
				Matching.Add(&Node);
			}
		}
		if (TermMet.Contains(false))
		{
			continue;
		}
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("ObjectPath"), Asset.GetObjectPathString());
		Row->SetStringField(TEXT("Class"), Asset.AssetClassPath.ToString());
		if (Status == EAssetDocumentUsageScan::Searched)
		{
			Row->SetStringField(TEXT("Source"), Scan.Source);
			Row->SetNumberField(TEXT("Matches"), Matching.Num());
			TArray<FString> GraphOrder;
			TMap<FString, TArray<TSharedPtr<FJsonValue>>> NodesByGraph;
			for (int32 Index = 0; Index < Matching.Num() && Index < MaxListedNodes; ++Index)
			{
				const FAssetDocumentUsageNode& Node = *Matching[Index];
				if (!NodesByGraph.Contains(Node.Graph))
				{
					GraphOrder.Add(Node.Graph);
				}
				TSharedPtr<FJsonObject> NodeRow = MakeShared<FJsonObject>();
				NodeRow->SetStringField(TEXT("NodeGuid"), Node.NodeGuid);
				NodeRow->SetStringField(TEXT("Title"), Node.Title);
				if (!Node.Action.IsEmpty())
				{
					NodeRow->SetStringField(TEXT("Action"), Node.Action);
				}
				else
				{
					NodeRow->SetStringField(TEXT("Class"), Node.NodeClass);
				}
				NodesByGraph.FindOrAdd(Node.Graph).Add(MakeShared<FJsonValueObject>(NodeRow));
			}
			TArray<TSharedPtr<FJsonValue>> Graphs;
			for (const FString& Graph : GraphOrder)
			{
				TSharedPtr<FJsonObject> GraphRow = MakeShared<FJsonObject>();
				GraphRow->SetStringField(TEXT("Graph"), Graph);
				GraphRow->SetArrayField(TEXT("Nodes"), NodesByGraph[Graph]);
				Graphs.Add(MakeShared<FJsonValueObject>(GraphRow));
			}
			Row->SetArrayField(TEXT("Graphs"), Graphs);
		}
		OutRows.Add(Row);
	}

	OutSummary = MakeShared<FJsonObject>();
	if (bAnyNodeTerm)
	{
		OutSummary->SetNumberField(TEXT("Searched"), Searched);
	}
	if (NotSearchedTotal > 0)
	{
		TSharedPtr<FJsonObject> NotSearchedObject = MakeShared<FJsonObject>();
		NotSearchedObject->SetNumberField(TEXT("Total"), NotSearchedTotal);
		TArray<TSharedPtr<FJsonValue>> Reasons;
		NotSearchedByReason.KeySort(TLess<FString>());
		for (const TPair<FString, TPair<int32, FString>>& Reason : NotSearchedByReason)
		{
			TSharedPtr<FJsonObject> ReasonRow = MakeShared<FJsonObject>();
			ReasonRow->SetStringField(TEXT("Reason"), Reason.Key);
			ReasonRow->SetNumberField(TEXT("Count"), Reason.Value.Key);
			ReasonRow->SetStringField(TEXT("Detail"), Reason.Value.Value);
			Reasons.Add(MakeShared<FJsonValueObject>(ReasonRow));
		}
		NotSearchedObject->SetArrayField(TEXT("Reasons"), Reasons);
		NotSearchedObject->SetArrayField(TEXT("Assets"), NotSearched);
		OutSummary->SetObjectField(TEXT("NotSearched"), NotSearchedObject);
	}
	return true;
}
}
