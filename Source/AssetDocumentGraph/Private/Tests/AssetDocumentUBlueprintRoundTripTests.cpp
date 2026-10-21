// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Blueprint extract -> Create round trips through the production Service:
// - RoundTrip.EngineSkySphere: a /Game copy of the engine's BP_Sky_Sphere (components, a construction
//   script calling component functions) extracts, the extraction re-targeted to a new path applies as
//   Create and the new Blueprint extracts as that document.
// - RoundTrip.FailedCreateLeavesNothing: a Create of a Blueprint with components and a construction
//   script that fails in the graph stage leaves no object, package, registry entry or file, and the
//   framework is not poisoned (the same Create applies afterwards).
// - RoundTrip.LargeBlueprint: FS_MasterField, whose member query exceeds one runtime value budget
//   (blueprint.member.query.v1 pages by "offset"), extracts and its extraction re-applies as an Update.
// - RoundTrip.BlueprintTypes: a Macro Library (ActorMacros), a Blueprint Interface
//   (Tutorial_BP_Interface) and a Blueprint Function Library (RenderToTextureFunctionLibrary) are
//   created by their own factories (Body.BlueprintType), without an event graph or construction script.
// - RoundTrip.SelectIndexType: RenderToTextureMacros, whose Select node has an unlinked enum Index
//   (PinTypes, graph.node.pin_type.set.v1) with defaults on the enumerator option pins.
// - RoundTrip.NodeState.*: node state the editor sets after placing a node (promotable operator
//   Pin Conversions, purity, struct / collapsed-graph nodes, class pins and self casts).
// - BlueprintType.Rules: an Update cannot change the type and a type has only its own regions.
// - Coverage.BlueprintRoundTripSweep (manual group): every Blueprint under /Engine and the enabled
//   plugins: extract -> Create of the extraction re-targeted to a /Game path (the source's object path,
//   package and self-cast pin names replaced, as an author copying the document does); logs the pass
//   rate and failure classes.
// - UBlueprint.RoundTrip.SweepPlan: the sweep's filter file (filters, resume, shard) and its row file.
//
// Running the sweep. It takes longer than the default editor test watchdog (1500 s), so run it with a longer
// timeout (e.g. 14400 s) for the test filter "UEAssetDoc.Coverage.BlueprintRoundTripSweep".
// Each Blueprint's row is appended to <project>/Saved/BlueprintRoundTripSweep.tsv as soon as it
// finishes (path, outcome, failure class, detail); a "Started" row precedes it, so a run the watchdog
// killed (or that crashed) keeps every finished row and names the Blueprint it was on.
// <project>/Saved/BlueprintRoundTripSweep.filter (optional), one directive per line:
//   <text>    only Blueprints whose object path contains one of these lines (re-run a failure class);
//   resume    keep the existing rows and run only Blueprints without an outcome row; one with only a
//             "Started" row (the run died on it) is recorded as "Killed" and skipped;
//   shard K/N only the K-th of N slices (K = 1..N) of the sorted list: run N sandboxes in parallel,
//             each with its own filter file, and concatenate their row files;
//   # ...     a comment.
// Without "resume" a run starts a new row file. The final summary (pass rate, failure classes) covers
// every outcome row of the file, earlier runs' rows included.

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphSchema.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/Blueprint.h"
#include "HAL/FileManager.h"
#include "K2Node_Composite.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/ReferenceChainSearch.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentUBlueprintRoundTripTests
{
const TCHAR* Folder = TEXT("AssetDocumentUBlueprintRoundTrip");
const TCHAR* EngineSkySphere = TEXT("/Engine/EngineSky/BP_Sky_Sphere.BP_Sky_Sphere");

FAssetDocumentService& Service()
{
	return FAssetDocumentModule::Get().GetService();
}

void ReleaseUndoHistory()
{
	if (GEditor != nullptr && GEditor->Trans != nullptr && !GIsTransacting && !GEditor->IsTransactionActive()
		&& GEditor->Trans->GetQueueLength() > 0)
	{
		GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentUBlueprintRoundTripTests", "Cleanup", "Blueprint round trip test cleanup"));
	}
}

struct FTarget
{
	FString AssetName;
	FString PackageName;
	FString ObjectPath;
	FString PackageFile;
	FString SidecarPath;

	explicit FTarget(const FString& Prefix)
		: AssetName(FString::Printf(TEXT("%s_%s"), *Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12)))
	{
		PackageName = FString::Printf(TEXT("/Game/%s/%s"), Folder, *AssetName);
		ObjectPath = PackageName + TEXT(".") + AssetName;
		PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		SidecarPath = FPaths::Combine(FPaths::ProjectContentDir(), Folder, AssetName + TEXT(".assetdoc.json"));
	}

	UBlueprint* Get() const { return Cast<UBlueprint>(FSoftObjectPath(ObjectPath).ResolveObject()); }

	// Each place a created asset can be left behind; empty when nothing is.
	FString Residue() const
	{
		TArray<FString> Left;
		if (FSoftObjectPath(ObjectPath).ResolveObject() != nullptr)
		{
			Left.Add(TEXT("object"));
		}
		if (FindPackage(nullptr, *PackageName) != nullptr)
		{
			Left.Add(TEXT("package"));
		}
		if (IAssetRegistry::GetChecked().GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid())
		{
			Left.Add(TEXT("registry"));
		}
		if (IFileManager::Get().FileExists(*PackageFile))
		{
			Left.Add(TEXT("file"));
		}
		return FString::Join(Left, TEXT(","));
	}

	void Cleanup() const
	{
		ReleaseUndoHistory();
		if (UBlueprint* Blueprint = Get())
		{
			if (IFileManager::Get().FileExists(*PackageFile))
			{
				UPackageTools::UnloadPackages({Blueprint->GetOutermost()});
			}
			else
			{
				Blueprint->GetOutermost()->ClearDirtyFlag();
				ObjectTools::DeleteObjects({Blueprint}, false, ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
			}
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		if (IFileManager::Get().FileExists(*PackageFile))
		{
			IFileManager::Get().Delete(*PackageFile, false, true);
			IAssetRegistry::GetChecked().ScanModifiedAssetFiles({PackageFile});
		}
		IFileManager::Get().Delete(*SidecarPath, false, true);
	}
};

TSharedPtr<FJsonObject> Parse(const FString& Text)
{
	TSharedPtr<FJsonObject> Object;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object);
	return Object;
}

FString Serialize(const TSharedPtr<FJsonObject>& Object)
{
	FString Text;
	if (Object.IsValid())
	{
		const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}
	return Text;
}

FString Summary(const FAssetDocumentResult& Result)
{
	TArray<FString> Lines;
	Lines.Add(Result.Message);
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Lines.Add(FString::Printf(TEXT("[%s] %s %s %s"), *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message,
			*Serialize(Diagnostic.Details)));
	}
	return FString::Join(Lines, TEXT("\n  "));
}

FAssetDocumentResult Extract(const FString& ObjectPath)
{
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = ObjectPath;
	return Service().Extract(Request);
}

FAssetDocumentResult Apply(const FTarget& Target, const FString& Json, bool bSave)
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target.SidecarPath), true);
	FFileHelper::SaveStringToFile(Json, *Target.SidecarPath);
	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = Target.SidecarPath;
	Request.bSaveAsset = bSave;
	return Service().ApplyFile(Request);
}

// The document an AI makes from an extract to create a copy: the source's own path (Target and
// any self-reference, such as the Blueprint's generated class) becomes the new path.
FString Retarget(const TSharedPtr<FJsonObject>& Extracted, const FString& SourceObjectPath, const FTarget& Target, const TCHAR* Action)
{
	const FString SourcePackage = FPackageName::ObjectPathToPackageName(SourceObjectPath);
	TSharedPtr<FJsonObject> Document = Parse(Serialize(Extracted));
	Document->SetStringField(TEXT("Action"), Action);
	// A cast to the Blueprint's own class names its output pin after the class ("AsDatasmith Layer"):
	// the copy's cast pin is named after the copy, as an author retargeting the document renames it ...
	const FString SourceCastPin = TEXT("As") + FName::NameToDisplayString(
		FPackageName::ObjectPathToObjectName(SourceObjectPath), false) + TEXT("\"");
	const FString TargetCastPin = TEXT("As") + FName::NameToDisplayString(Target.AssetName, false) + TEXT("\"");
	// ... and its node id after the class ("Cast_DatasmithLayer_C").
	const FString SourceCastId = TEXT("\"Cast_") + FPackageName::ObjectPathToObjectName(SourceObjectPath) + TEXT("_C");
	const FString TargetCastId = TEXT("\"Cast_") + Target.AssetName + TEXT("_C");
	return Serialize(Document)
		.Replace(*SourceCastId, *TargetCastId, ESearchCase::CaseSensitive)
		.Replace(*SourceObjectPath, *(Target.ObjectPath), ESearchCase::CaseSensitive)
		.Replace(*(SourcePackage + TEXT("\"")), *(Target.PackageName + TEXT("\"")), ESearchCase::CaseSensitive)
		.Replace(*(TEXT(":") + SourceCastPin), *(TEXT(":") + TargetCastPin), ESearchCase::CaseSensitive)
		.Replace(*(TEXT("\"") + SourceCastPin), *(TEXT("\"") + TargetCastPin), ESearchCase::CaseSensitive);
}

FString Retarget(const TSharedPtr<FJsonObject>& Extracted, const FTarget& Source, const FTarget& Target, const TCHAR* Action)
{
	return Retarget(Extracted, Source.ObjectPath, Target, Action);
}

// A /Game copy of SourceObjectPath made with the editor's Duplicate; null when it cannot be copied.
UBlueprint* DuplicateToGame(const FString& SourceObjectPath, const FTarget& Copy)
{
	UBlueprint* Source = LoadObject<UBlueprint>(nullptr, *SourceObjectPath);
	if (Source == nullptr)
	{
		return nullptr;
	}
	TSet<UPackage*> Refused;
	UObject* Duplicated = ObjectTools::DuplicateSingleObject(Source,
		ObjectTools::FPackageGroupName{Copy.PackageName, FString(), Copy.AssetName}, Refused, /*bPromptToOverwrite=*/false);
	return Cast<UBlueprint>(Duplicated);
}

void RemoveGeneratedGuids(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid())
	{
		return;
	}
	if (Value->Type == EJson::Object)
	{
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		Object->RemoveField(TEXT("NodeGuid"));
		Object->RemoveField(TEXT("GraphGuid"));
		for (const auto& Pair : Object->Values)
		{
			RemoveGeneratedGuids(Pair.Value);
		}
	}
	else if (Value->Type == EJson::Array)
	{
		for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
		{
			RemoveGeneratedGuids(Item);
		}
	}
}

// Nodes and Links are sets: their array order follows the live graph (a new Blueprint's reused
// default events keep their slots) and carries no meaning. Sorted by Id / by link text.
void SortGraphArrays(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid())
	{
		return;
	}
	auto TextOf = [](const TSharedPtr<FJsonValue>& Item)
	{
		FString Result;
		const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Result);
		FJsonSerializer::Serialize(Item, TEXT(""), Writer);
		return Result;
	};
	if (Value->Type == EJson::Object)
	{
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		for (const auto& Pair : Object->Values)
		{
			SortGraphArrays(Pair.Value);
		}
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		if (Object->TryGetArrayField(TEXT("Nodes"), Nodes))
		{
			TArray<TSharedPtr<FJsonValue>> Sorted = *Nodes;
			Sorted.StableSort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
			{
				FString IdA, IdB;
				if (A->Type == EJson::Object) { A->AsObject()->TryGetStringField(TEXT("Id"), IdA); }
				if (B->Type == EJson::Object) { B->AsObject()->TryGetStringField(TEXT("Id"), IdB); }
				return IdA < IdB;
			});
			Object->SetArrayField(TEXT("Nodes"), Sorted);
		}
		// Components by scope and name: the profile matches them by name (_components_match), and the
		// flat SCS node order is creation history (a child added before its parent), not the tree order.
		const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
		if (Object->TryGetArrayField(TEXT("Components"), Components))
		{
			auto KeyOf = [](const TSharedPtr<FJsonValue>& Item)
			{
				FString Scope, Name;
				if (Item->Type == EJson::Object)
				{
					Item->AsObject()->TryGetStringField(TEXT("Scope"), Scope);
					Item->AsObject()->TryGetStringField(TEXT("Name"), Name);
				}
				return Scope + TEXT("|") + Name;
			};
			TArray<TSharedPtr<FJsonValue>> Sorted = *Components;
			Sorted.StableSort([&KeyOf](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B) { return KeyOf(A) < KeyOf(B); });
			Object->SetArrayField(TEXT("Components"), Sorted);
		}
		const TArray<TSharedPtr<FJsonValue>>* Links = nullptr;
		if (Object->TryGetArrayField(TEXT("Links"), Links))
		{
			TArray<TSharedPtr<FJsonValue>> Sorted = *Links;
			Sorted.StableSort([&TextOf](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B) { return TextOf(A) < TextOf(B); });
			Object->SetArrayField(TEXT("Links"), Sorted);
		}
	}
	else if (Value->Type == EJson::Array)
	{
		for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
		{
			SortGraphArrays(Item);
		}
	}
}

// Negative zero as zero, as canonical JSON writes it (a source rotation's -0 Roll comes back 0).
TSharedPtr<FJsonValue> WithoutNegativeZero(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid())
	{
		return Value;
	}
	if (Value->Type == EJson::Number && Value->AsNumber() == 0.0)
	{
		return MakeShared<FJsonValueNumber>(0.0);
	}
	if (Value->Type == EJson::Object)
	{
		for (auto& Pair : Value->AsObject()->Values)
		{
			Pair.Value = WithoutNegativeZero(Pair.Value);
		}
	}
	else if (Value->Type == EJson::Array)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
		{
			Items.Add(WithoutNegativeZero(Item));
		}
		return MakeShared<FJsonValueArray>(Items);
	}
	return Value;
}

// The comma-separated items of a parenthesized UE text value (quotes respected); false for other text.
bool TopLevelItems(const FString& Text, TArray<FString>& Out)
{
	Out.Reset();
	if (Text.Len() < 2 || Text[0] != TEXT('(') || Text[Text.Len() - 1] != TEXT(')'))
	{
		return false;
	}
	int32 Depth = 0;
	int32 Start = 1;
	bool bQuoted = false;
	for (int32 Index = 1; Index < Text.Len() - 1; ++Index)
	{
		const TCHAR Char = Text[Index];
		if (bQuoted)
		{
			Index += Char == TEXT('\\') ? 1 : 0;
			bQuoted = Char != TEXT('"');
		}
		else if (Char == TEXT('"'))
		{
			bQuoted = true;
		}
		else if (Char == TEXT('(') || Char == TEXT(')'))
		{
			Depth += Char == TEXT('(') ? 1 : -1;
			if (Depth < 0)
			{
				return false;
			}
		}
		else if (Char == TEXT(',') && Depth == 0)
		{
			Out.Add(Text.Mid(Start, Index - Start));
			Start = Index + 1;
		}
	}
	Out.Add(Text.Mid(Start, Text.Len() - 1 - Start));
	return !bQuoted && Depth == 0;
}

// A map's text value, (("k", "v"),("k2", "v2")), with its entries in sorted order: entries in another
// order are the same map (FMapProperty::Identical), and compiling a Blueprint reorders a map whose
// struct default has entries (the class default object is copied by delta over that default:
// BP_Dynamic_Force_SkelMesh's SocketsandEndpoints). A map writes each pair "(key, value)"; a struct "(A=1,B=2)".
TSharedPtr<FJsonValue> WithMapsSorted(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid())
	{
		return Value;
	}
	if (Value->Type == EJson::String)
	{
		TArray<FString> Entries;
		TArray<FString> Parts;
		if (!TopLevelItems(Value->AsString(), Entries) || Entries.Num() < 2)
		{
			return Value;
		}
		for (const FString& Entry : Entries)
		{
			if (!TopLevelItems(Entry, Parts) || Parts.Num() != 2 || !Parts[1].StartsWith(TEXT(" ")))
			{
				return Value;
			}
		}
		Entries.Sort();
		return MakeShared<FJsonValueString>(TEXT("(") + FString::Join(Entries, TEXT(",")) + TEXT(")"));
	}
	if (Value->Type == EJson::Object)
	{
		for (auto& Pair : Value->AsObject()->Values)
		{
			Pair.Value = WithMapsSorted(Pair.Value);
		}
	}
	else if (Value->Type == EJson::Array)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
		{
			Items.Add(WithMapsSorted(Item));
		}
		return MakeShared<FJsonValueArray>(Items);
	}
	return Value;
}

// The document as compared: no NodeGuid / GraphGuid fields, graph nodes, links and components in canonical
// order, map entries sorted.
FString WithoutGeneratedGuids(const FString& Json)
{
	const TSharedPtr<FJsonObject> Object = Parse(Json);
	RemoveGeneratedGuids(MakeShared<FJsonValueObject>(Object));
	WithoutNegativeZero(MakeShared<FJsonValueObject>(Object));
	WithMapsSorted(MakeShared<FJsonValueObject>(Object));
	SortGraphArrays(MakeShared<FJsonValueObject>(Object));
	return Serialize(Object);
}

// Saved/BlueprintRoundTripSweep/<name>.<kind>.json, for diagnosing a sweep row.
void Dump(const FString& SourceObjectPath, const TCHAR* Kind, const FString& Json)
{
	const FString Name(FPackageName::ObjectPathToObjectName(SourceObjectPath));
	FFileHelper::SaveStringToFile(Json, *FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BlueprintRoundTripSweep"),
		FString::Printf(TEXT("%s.%s.json"), *Name, Kind)), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

// Paths where two JSON values differ: "<path>: <expected> -> <actual>" (at most 40).
void DiffJson(const TSharedPtr<FJsonValue>& Expected, const TSharedPtr<FJsonValue>& Actual, const FString& Path, TArray<FString>& Out)
{
	if (Out.Num() >= 40)
	{
		return;
	}
	auto Text = [](const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return FString(TEXT("(absent)"));
		}
		FString Result;
		const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Result);
		FJsonSerializer::Serialize(Value, TEXT(""), Writer);
		return Result.Left(160);
	};
	if (Expected.IsValid() && Actual.IsValid() && Expected->Type == EJson::Object && Actual->Type == EJson::Object)
	{
		const TSharedPtr<FJsonObject> A = Expected->AsObject();
		const TSharedPtr<FJsonObject> B = Actual->AsObject();
		TSet<FString> Keys;
		for (const auto& Pair : A->Values) { Keys.Add(FString(*Pair.Key)); }
		for (const auto& Pair : B->Values) { Keys.Add(FString(*Pair.Key)); }
		for (const FString& Key : Keys)
		{
			DiffJson(A->TryGetField(Key), B->TryGetField(Key), Path + TEXT("/") + Key, Out);
		}
		return;
	}
	if (Expected.IsValid() && Actual.IsValid() && Expected->Type == EJson::Array && Actual->Type == EJson::Array
		&& Expected->AsArray().Num() == Actual->AsArray().Num())
	{
		const TArray<TSharedPtr<FJsonValue>>& A = Expected->AsArray();
		const TArray<TSharedPtr<FJsonValue>>& B = Actual->AsArray();
		for (int32 Index = 0; Index < A.Num(); ++Index)
		{
			DiffJson(A[Index], B[Index], FString::Printf(TEXT("%s/%d"), *Path, Index), Out);
		}
		return;
	}
	const FString ExpectedText = Text(Expected);
	const FString ActualText = Text(Actual);
	if (ExpectedText != ActualText)
	{
		Out.Add(FString::Printf(TEXT("%s: %s -> %s"), *Path, *ExpectedText, *ActualText));
	}
}

enum class ERoundTrip
{
	Equal,
	CopyFailed,
	ExtractFailed,
	CreateFailed,
	CopyExtractFailed,
	Mismatch,
	Refused,      // a Create refused with a documented code: content the editor itself cannot author
};

// Refusals of states the editor cannot author (the round trip documents them instead of recreating them):
// - Graph.UnresolvedReference: a node naming a variable, function or input action that does not exist
//   (no palette action places it; deleting a variable deletes its nodes);
// - BlueprintMember.InvalidName: a member name FKismetNameValidator refuses (e.g. containing '.');
// - BlueprintLifecycle.InvalidParentClass: a parent the editor's class picker does not offer for that
//   Blueprint type (a Macro Library of a LevelScriptActor).
bool IsDocumentedRefusal(const FString& Code)
{
	return Code == TEXT("Graph.UnresolvedReference") || Code == TEXT("BlueprintMember.InvalidName")
		|| Code == TEXT("BlueprintLifecycle.InvalidParentClass") || Code == TEXT("Graph.EditorOnlyFunction");
}

// - Graph.ConnectDisallowed naming the re-targeted copy's own class: the copy's self references move to
//   the copy, but another asset still hands out the source's class (BP_LandmassBrush gets its brushes
//   from a layer stack variable typed with BP_LandmassBrush), so the editor's schema refuses that link in
//   the copy as it would in an editor-duplicated asset. The source itself round-trips only in place.
// A link the copy's new class refuses: the copy's own type where the source's fitted, or the copy's
// "self" into a pin of the source's class that another asset's function declares (the re-target only
// rewrites the document, not what other assets declare).
bool IsRetargetRefusal(const FAssetDocumentResult& Applied, const FString& CopyAssetName, const FString& SourceAssetName)
{
	auto Squash = [](FString Text) { return Text.Replace(TEXT(" "), TEXT("")).Replace(TEXT("_"), TEXT("")); };
	if (Applied.Diagnostics.IsEmpty() || Applied.Diagnostics[0].Code != TEXT("Graph.ConnectDisallowed"))
	{
		return false;
	}
	const FString Message = Squash(Applied.Diagnostics[0].Message);
	return Message.Contains(Squash(CopyAssetName))
		|| (Message.Contains(TEXT("SelfObjectReferenceisnotcompatiblewith")) && Message.Contains(Squash(SourceAssetName) + TEXT("ObjectReference")));
}

// Graph.ConnectDisallowed for a link the source itself holds although its schema refuses it (a function
// whose output type changed after the link was made: BP_FluidSim_01's GetLocalPawn Pawn into a Player
// Camera Manager target, on pure nodes the compiler prunes). The editor cannot make that link.
bool IsSourceRefusedLink(const FAssetDocumentResult& Applied, const FString& SourceObjectPath)
{
	FString FromPin;
	FString ToPin;
	if (Applied.Diagnostics.IsEmpty() || Applied.Diagnostics[0].Code != TEXT("Graph.ConnectDisallowed"))
	{
		return false;
	}
	// "Cannot connect <node>:<pin> to <node>:<pin>: <reason>"
	FString Message = Applied.Diagnostics[0].Message;
	FString From;
	FString To;
	if (!Message.RemoveFromStart(TEXT("Cannot connect ")) || !Message.Split(TEXT(" to "), &From, &To)
		|| !From.Split(TEXT(":"), nullptr, &FromPin) || !To.Split(TEXT(": "), &To, nullptr) || !To.Split(TEXT(":"), nullptr, &ToPin))
	{
		return false;
	}
	const UBlueprint* Source = LoadObject<UBlueprint>(nullptr, *SourceObjectPath);
	TArray<UEdGraph*> Graphs;
	if (Source != nullptr)
	{
		Source->GetAllGraphs(Graphs);
	}
	for (const UEdGraph* Graph : Graphs)
	{
		for (const UEdGraphNode* Node : Graph != nullptr && Graph->GetSchema() != nullptr ? Graph->Nodes : TArray<TObjectPtr<UEdGraphNode>>())
		{
			for (UEdGraphPin* Pin : Node != nullptr ? Node->Pins : TArray<UEdGraphPin*>())
			{
				if (Pin == nullptr || Pin->Direction != EGPD_Output || Pin->PinName.ToString() != FromPin)
				{
					continue;
				}
				for (UEdGraphPin* Link : Pin->LinkedTo)
				{
					if (Link != nullptr && Link->PinName.ToString() == ToPin
						&& Graph->GetSchema()->CanCreateConnection(Pin, Link).Response == CONNECT_RESPONSE_DISALLOW)
					{
						return true;
					}
				}
			}
		}
	}
	return false;
}

struct FRoundTrip
{
	ERoundTrip Outcome = ERoundTrip::Equal;
	FString Class;        // failure class (first diagnostic code), for the sweep
	FString Detail;
	FString Residue;      // what a failed Create left behind
	bool bPoisoned = false;
};

// Extract of the source -> re-targeted Create (the source's own path, and so every self reference such
// as its generated class, becomes the new path: what an AI re-targeting an extract does) -> extract of the
// new Blueprint equals the re-targeted document. The source itself is only read.
FRoundTrip RoundTrip(const FString& SourceObjectPath, const FString& Prefix, bool bSave)
{
	FRoundTrip Out;
	const FTarget Created(Prefix + TEXT("_RT"));
	ON_SCOPE_EXIT
	{
		Created.Cleanup();
	};
	if (LoadObject<UBlueprint>(nullptr, *SourceObjectPath) == nullptr)
	{
		Out.Outcome = ERoundTrip::CopyFailed;
		Out.Class = TEXT("Load");
		return Out;
	}
	const FAssetDocumentResult Extracted = Extract(SourceObjectPath);
	if (!Extracted.IsSuccess() || !Extracted.Payload.IsValid())
	{
		Out.Outcome = ERoundTrip::ExtractFailed;
		Out.Class = Extracted.Diagnostics.IsEmpty() ? FString(TEXT("Extract")) : Extracted.Diagnostics[0].Code;
		Out.Detail = Summary(Extracted);
		return Out;
	}
	const FString CreateDocument = Retarget(Extracted.Payload, SourceObjectPath, Created, TEXT("Create"));
	const FAssetDocumentResult Applied = Apply(Created, CreateDocument, bSave);
	if (!Applied.IsSuccess())
	{
		Dump(SourceObjectPath, TEXT("create"), CreateDocument);
		Out.Class = Applied.Diagnostics.IsEmpty() ? FString(TEXT("Create")) : Applied.Diagnostics[0].Code;
		Out.Outcome = IsDocumentedRefusal(Out.Class) || IsRetargetRefusal(Applied, Created.AssetName, FSoftObjectPath(SourceObjectPath).GetAssetName())
			|| IsSourceRefusedLink(Applied, SourceObjectPath)
			? ERoundTrip::Refused : ERoundTrip::CreateFailed;
		Out.Detail = Summary(Applied);
		Out.Residue = Created.Residue();
		Out.bPoisoned = Applied.Diagnostics.ContainsByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
			{ return Diagnostic.Code.StartsWith(TEXT("Atomic.Poisoned")) || Diagnostic.Code.Contains(TEXT("Residue")); });
		return Out;
	}
	const FAssetDocumentResult Readback = Extract(Created.ObjectPath);
	if (!Readback.IsSuccess() || !Readback.Payload.IsValid())
	{
		Out.Outcome = ERoundTrip::CopyExtractFailed;
		Out.Class = Readback.Diagnostics.IsEmpty() ? FString(TEXT("Extract")) : Readback.Diagnostics[0].Code;
		Out.Detail = Summary(Readback);
		return Out;
	}
	// NodeGuid / GraphGuid are generated metadata: a Create generates its own (the profile contract).
	const FString Expected = WithoutGeneratedGuids(Retarget(Extracted.Payload, SourceObjectPath, Created, *Extracted.Payload->GetStringField(TEXT("Action"))));
	const FString Actual = WithoutGeneratedGuids(Serialize(Readback.Payload));
	if (Actual != Expected)
	{
		Dump(SourceObjectPath, TEXT("expected"), Expected);
		Dump(SourceObjectPath, TEXT("actual"), Actual);
		Out.Outcome = ERoundTrip::Mismatch;
		TArray<FString> Differences;
		DiffJson(MakeShared<FJsonValueObject>(Parse(Expected)), MakeShared<FJsonValueObject>(Parse(Actual)), TEXT(""), Differences);
		// The class names the kind of field that differs: its last key (array indexes dropped).
		TSet<FString> Fields;
		for (const FString& Difference : Differences)
		{
			FString Path;
			Difference.Split(TEXT(":"), &Path, nullptr);
			TArray<FString> Parts;
			Path.ParseIntoArray(Parts, TEXT("/"));
			Parts.RemoveAll([](const FString& Part) { return Part.IsNumeric(); });
			Fields.Add(Parts.IsEmpty() ? FString(TEXT("(root)")) : Parts.Last());
		}
		TArray<FString> Sorted = Fields.Array();
		Sorted.Sort();
		Out.Class = TEXT("Mismatch:") + FString::Join(Sorted, TEXT(","));
		Out.Detail = FString::Join(Differences, TEXT("\n  "));
	}
	return Out;
}

// The sweep's filter file, parsed.
struct FSweepPlan
{
	TArray<FString> Filters;
	bool bResume = false;
	int32 Shard = 0;      // 1-based; 0: no sharding
	int32 ShardCount = 0;
	TArray<FString> Errors;
};

FSweepPlan ParseSweepFilter(const FString& Text)
{
	FSweepPlan Plan;
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);
	for (FString Line : Lines)
	{
		Line.TrimStartAndEndInline();
		if (Line.IsEmpty() || Line.StartsWith(TEXT("#")))
		{
			continue;
		}
		if (Line.Equals(TEXT("resume"), ESearchCase::IgnoreCase))
		{
			Plan.bResume = true;
			continue;
		}
		FString Slice;
		if (Line.Split(TEXT(" "), nullptr, &Slice) && Line.StartsWith(TEXT("shard "), ESearchCase::IgnoreCase))
		{
			FString K;
			FString N;
			Slice.TrimStartAndEndInline();
			if (Slice.Split(TEXT("/"), &K, &N) && K.IsNumeric() && N.IsNumeric()
				&& FCString::Atoi(*N) > 0 && FCString::Atoi(*K) >= 1 && FCString::Atoi(*K) <= FCString::Atoi(*N))
			{
				Plan.Shard = FCString::Atoi(*K);
				Plan.ShardCount = FCString::Atoi(*N);
			}
			else
			{
				Plan.Errors.Add(FString::Printf(TEXT("'%s': expected 'shard K/N' with 1 <= K <= N"), *Line));
			}
			continue;
		}
		Plan.Filters.Add(Line);
	}
	return Plan;
}

// Rows of a sweep row file: "<path>\t<outcome>\t<class>\t<detail>" (outcome "Started" before an asset runs).
struct FSweepRows
{
	TMap<FString, FString> Outcomes;   // path -> last outcome row (without the path)
	TSet<FString> Started;             // paths with a Started row
};

FSweepRows ReadSweepRows(const FString& Text)
{
	FSweepRows Rows;
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);
	for (const FString& Line : Lines)
	{
		FString Path;
		FString Rest;
		if (!Line.Split(TEXT("\t"), &Path, &Rest))
		{
			continue;
		}
		if (Rest == TEXT("Started"))
		{
			Rows.Started.Add(Path);
		}
		else
		{
			Rows.Outcomes.Add(Path, Rest);
		}
	}
	return Rows;
}

// The Blueprints a run of the sweep covers (sorted paths in): filters, then the shard slice, then (resume)
// those without an outcome row. OutKilled: resumed paths that only have a Started row.
TArray<FString> SelectSweepAssets(const TArray<FString>& Sorted, const FSweepPlan& Plan, const FSweepRows& Existing, TArray<FString>& OutKilled)
{
	TArray<FString> Selected;
	int32 Index = 0;
	for (const FString& Path : Sorted)
	{
		if (!Plan.Filters.IsEmpty() && !Plan.Filters.ContainsByPredicate([&Path](const FString& Filter) { return Path.Contains(Filter); }))
		{
			continue;
		}
		const bool bInShard = Plan.ShardCount == 0 || Index % Plan.ShardCount == Plan.Shard - 1;
		++Index;
		if (!bInShard)
		{
			continue;
		}
		if (Plan.bResume && Existing.Outcomes.Contains(Path))
		{
			continue;
		}
		if (Plan.bResume && Existing.Started.Contains(Path))
		{
			OutKilled.Add(Path);
			continue;
		}
		Selected.Add(Path);
	}
	return Selected;
}

// Appends one row (and its line end) to the row file now, so a killed run keeps it.
bool AppendSweepRow(const FString& File, const FString& Row)
{
	return FFileHelper::SaveStringToFile(Row + TEXT("\n"), *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
		&IFileManager::Get(), FILEWRITE_Append);
}
} // namespace AssetDocumentUBlueprintRoundTripTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUBlueprintRoundTripSweepPlanTest,
	"UEAssetDoc.UBlueprint.RoundTrip.SweepPlan",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintRoundTripSweepPlanTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentUBlueprintRoundTripTests;
	const TArray<FString> Sorted = {TEXT("/A/BP_1.BP_1"), TEXT("/A/BP_2.BP_2"), TEXT("/A/BP_3.BP_3"), TEXT("/B/BP_4.BP_4"), TEXT("/B/BP_5.BP_5")};
	TArray<FString> Killed;
	TestEqual(TEXT("no filter file: every Blueprint"), SelectSweepAssets(Sorted, ParseSweepFilter(FString()), FSweepRows(), Killed), Sorted);
	TestEqual(TEXT("a filter line selects by path"), SelectSweepAssets(Sorted, ParseSweepFilter(TEXT("# re-run\n/B/\n")), FSweepRows(), Killed),
		TArray<FString>{TEXT("/B/BP_4.BP_4"), TEXT("/B/BP_5.BP_5")});
	TestEqual(TEXT("shard 1/2"), SelectSweepAssets(Sorted, ParseSweepFilter(TEXT("shard 1/2")), FSweepRows(), Killed),
		TArray<FString>{TEXT("/A/BP_1.BP_1"), TEXT("/A/BP_3.BP_3"), TEXT("/B/BP_5.BP_5")});
	TestEqual(TEXT("shard 2/2"), SelectSweepAssets(Sorted, ParseSweepFilter(TEXT("shard 2/2")), FSweepRows(), Killed),
		TArray<FString>{TEXT("/A/BP_2.BP_2"), TEXT("/B/BP_4.BP_4")});
	TestEqual(TEXT("a bad shard is reported"), ParseSweepFilter(TEXT("shard 3/2")).Errors.Num(), 1);

	// A run killed on BP_3: BP_1 and BP_2 have rows, BP_3 only its Started row.
	const FString File = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AssetDocumentSweepPlanTest"), FGuid::NewGuid().ToString() + TEXT(".tsv"));
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*FPaths::GetPath(File), false, true); };
	for (const TCHAR* Row : {TEXT("/A/BP_1.BP_1\tStarted"), TEXT("/A/BP_1.BP_1\tEqual\t\t"), TEXT("/A/BP_2.BP_2\tStarted"),
		TEXT("/A/BP_2.BP_2\tCreateFailed\tGraph.X\tdetail"), TEXT("/A/BP_3.BP_3\tStarted")})
	{
		FString Before;
		FFileHelper::LoadFileToString(Before, *File);
		TestTrue(TEXT("a row is appended"), AppendSweepRow(File, Row));
		FString After;
		FFileHelper::LoadFileToString(After, *File);
		TestEqual(TEXT("the row is on disk as soon as it is appended"), After, Before + Row + TEXT("\n"));
	}
	FString Text;
	FFileHelper::LoadFileToString(Text, *File);
	const FSweepRows Existing = ReadSweepRows(Text);
	TestEqual(TEXT("two outcome rows"), Existing.Outcomes.Num(), 2);
	Killed.Reset();
	TestEqual(TEXT("resume runs the Blueprints without an outcome, after the one the run died on"),
		SelectSweepAssets(Sorted, ParseSweepFilter(TEXT("resume")), Existing, Killed),
		TArray<FString>{TEXT("/B/BP_4.BP_4"), TEXT("/B/BP_5.BP_5")});
	TestEqual(TEXT("the Blueprint the run died on is Killed"), Killed, TArray<FString>{TEXT("/A/BP_3.BP_3")});
	Killed.Reset();
	TestEqual(TEXT("resume within a shard"), SelectSweepAssets(Sorted, ParseSweepFilter(TEXT("resume\nshard 1/2")), Existing, Killed),
		TArray<FString>{TEXT("/B/BP_5.BP_5")});
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUBlueprintRoundTripEngineSkySphereTest,
	"UEAssetDoc.UBlueprint.RoundTrip.EngineSkySphere",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintRoundTripEngineSkySphereTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentUBlueprintRoundTripTests;
	const FRoundTrip Result = RoundTrip(EngineSkySphere, TEXT("BP_Sky"), /*bSave=*/true);
	if (!Result.Detail.IsEmpty())
	{
		AddInfo(Result.Detail);
	}
	TestEqual(TEXT("BP_Sky_Sphere copy round-trips through extract -> Create"), static_cast<int32>(Result.Outcome),
		static_cast<int32>(ERoundTrip::Equal));
	TestEqual(TEXT("a failed Create leaves nothing"), Result.Residue, FString());
	TestFalse(TEXT("rollback is not poisoned"), Result.bPoisoned);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUBlueprintRoundTripFailedCreateTest,
	"UEAssetDoc.UBlueprint.RoundTrip.FailedCreateLeavesNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintRoundTripFailedCreateTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentUBlueprintRoundTripTests;
	const FTarget Target(TEXT("BP_FailedCreate"));
	ON_SCOPE_EXIT { Target.Cleanup(); };
	// Variables with defaults, components, a construction script reading a component and calling a
	// function on it, then a graph-stage failure (an action key that does not exist) after all of them
	// are populated. The variable steps leave the new Blueprint in the undo history and the compile
	// registers it with the Blueprint action database: the rollback must still delete it completely.
	auto Document = [&Target](const TCHAR* LastAction)
	{
		return FString::Printf(TEXT(
			"{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.Blueprint\",\"Action\":\"Create\","
			"\"Body\":{\"ParentClass\":\"/Script/Engine.Actor\","
			"\"Variables\":[{\"Name\":\"Brightness\",\"Type\":\"float\",\"DefaultValue\":2.5,\"Category\":\"Look\"},"
			"{\"Name\":\"bRefresh\",\"Type\":\"bool\",\"DefaultValue\":true}],"
			"\"Components\":["
			"{\"Name\":\"Root\",\"Class\":\"/Script/Engine.SceneComponent\"},"
			"{\"Name\":\"Mesh\",\"Class\":\"/Script/Engine.StaticMeshComponent\",\"AttachTo\":\"Root\"}],"
			"\"FunctionGraphs\":[{\"Name\":\"UserConstructionScript\",\"Nodes\":["
			"{\"Id\":\"GetMesh\",\"Action\":\"K2:Get:Mesh\"},"
			"{\"Id\":\"MakeMid\",\"Action\":\"K2:CallFunction:/Script/Engine.PrimitiveComponent:CreateDynamicMaterialInstance\"},"
			"{\"Id\":\"Last\",\"Action\":\"%s\"}],"
			"\"Links\":["
			"{\"From\":\"Entry:then\",\"To\":\"MakeMid:execute\"},"
			"{\"From\":\"GetMesh:Mesh\",\"To\":\"MakeMid:self\"},"
			"{\"From\":\"MakeMid:then\",\"To\":\"Last:execute\"}]}]}}"),
			*Target.PackageName, LastAction);
	};
	const FAssetDocumentResult Failed = Apply(Target, Document(TEXT("K2:CallFunction:/Script/Engine.KismetSystemLibrary:NoSuchFunction")), true);
	AddInfo(Summary(Failed));
	TestFalse(TEXT("the Create fails in the graph stage"), Failed.IsSuccess());
	TestTrue(TEXT("the failure is the unknown action"), Failed.Diagnostics.ContainsByPredicate(
		[](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == TEXT("Graph.ActionNotFound"); }));
	TestFalse(TEXT("the rollback is not poisoned"), Failed.Diagnostics.ContainsByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
		{ return Diagnostic.Code.StartsWith(TEXT("Atomic.Poisoned")) || Diagnostic.Code.Contains(TEXT("Residue")); }));
	// Checked as the rollback left it: no undo-history reset or garbage collection by the test.
	TestEqual(TEXT("the failed Create leaves nothing"), Target.Residue(), FString());

	// Not poisoned: the same target is created by a valid document afterwards.
	const FAssetDocumentResult Created = Apply(Target, Document(TEXT("K2:PrintString")), true);
	AddInfo(Summary(Created));
	TestTrue(TEXT("the same Create applies after the failed one"), Created.IsSuccess());
	return !HasAnyErrors();
}

namespace AssetDocumentUBlueprintRoundTripTests
{
// One engine Blueprint round trip as a test: Equal, nothing left behind, rollback not poisoned.
void ExpectEqualRoundTrip(FAutomationTestBase& Test, const TCHAR* Source, const TCHAR* Prefix, const TCHAR* What)
{
	const FRoundTrip Result = RoundTrip(Source, Prefix, /*bSave=*/true);
	if (!Result.Detail.IsEmpty())
	{
		Test.AddInfo(Result.Detail);
	}
	Test.TestEqual(*FString::Printf(TEXT("%s round-trips through extract -> Create (%s)"), What, *Result.Class),
		static_cast<int32>(Result.Outcome), static_cast<int32>(ERoundTrip::Equal));
	Test.TestEqual(TEXT("a failed Create leaves nothing"), Result.Residue, FString());
	Test.TestFalse(TEXT("rollback is not poisoned"), Result.bPoisoned);
}

bool HasCode(const FAssetDocumentResult& Result, const TCHAR* Code)
{
	return Result.Diagnostics.ContainsByPredicate([Code](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == Code; });
}
} // namespace AssetDocumentUBlueprintRoundTripTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUBlueprintRoundTripLargeTest,
	"UEAssetDoc.UBlueprint.RoundTrip.LargeBlueprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintRoundTripLargeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentUBlueprintRoundTripTests;
	// (Its re-targeted Create stops at a legacy Vector -> Float link into a promotable operator, a
	// separate sweep class; the paging is what this test covers.)
	const FTarget Copy(TEXT("FS_Master"));
	ON_SCOPE_EXIT { Copy.Cleanup(); };
	UBlueprint* Source = DuplicateToGame(TEXT("/Engine/EditorResources/FieldNodes/FS_MasterField.FS_MasterField"), Copy);
	if (!TestNotNull(TEXT("FS_MasterField copies to /Game"), Source))
	{
		return false;
	}
	// An Update applies to a saved asset (a dirty target fails closed).
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	if (!TestTrue(TEXT("the copy saves"), UPackage::SavePackage(Source->GetOutermost(), Source, *Copy.PackageFile, SaveArgs)))
	{
		return false;
	}
	const FAssetDocumentResult Extracted = Extract(Copy.ObjectPath);
	AddInfo(Summary(Extracted));
	if (!TestTrue(TEXT("a member query larger than one value budget extracts"), Extracted.IsSuccess() && Extracted.Payload.IsValid()))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Body = Extracted.Payload->GetObjectField(TEXT("Body"));
	TestTrue(TEXT("every variable is extracted"), Body->GetArrayField(TEXT("Variables")).Num() > 50);
	const FAssetDocumentResult Reapplied = Apply(Copy, Retarget(Extracted.Payload, Copy, Copy, TEXT("Update")), true);
	AddInfo(Summary(Reapplied));
	TestTrue(TEXT("the extraction re-applies as an Update"), Reapplied.IsSuccess());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUBlueprintRoundTripTypesTest,
	"UEAssetDoc.UBlueprint.RoundTrip.BlueprintTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintRoundTripTypesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentUBlueprintRoundTripTests;
	ExpectEqualRoundTrip(*this, TEXT("/Engine/EditorBlueprintResources/ActorMacros.ActorMacros"), TEXT("Macros"),
		TEXT("ActorMacros (Macro Library)"));
	ExpectEqualRoundTrip(*this, TEXT("/Engine/Tutorial/BlueprintTutorials/TutorialAssets/Tutorial_BP_Interface.Tutorial_BP_Interface"),
		TEXT("Interface"), TEXT("Tutorial_BP_Interface (Blueprint Interface)"));
	// A Function Library whose functions keep local variables (LocalVariables).
	ExpectEqualRoundTrip(*this, TEXT("/Engine/ArtTools/RenderToTexture/Macros/RenderToTextureFunctionLibrary.RenderToTextureFunctionLibrary"),
		TEXT("R2TLib"), TEXT("RenderToTextureFunctionLibrary (Function Library, local variables)"));
	return !HasAnyErrors();
}

// Node state the editor sets on a node after placing it (sweep classes), one engine Blueprint each.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUBlueprintRoundTripPromotableTest,
	"UEAssetDoc.UBlueprint.RoundTrip.NodeState.PromotableOperators",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintRoundTripPromotableTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentUBlueprintRoundTripTests;
	// Multiply / Add with operands of differing types (vector * float): PinTypes (Pin Conversions) and
	// the drag-drop promotion of a link. (FS_BombField_Prototype, whose operator chains run through
	// reroutes, still comes back with other operand types: a known gap, not covered here.)
	ExpectEqualRoundTrip(*this, TEXT("/Engine/EditorResources/FieldNodes/Niagara/FS_BaseField.FS_BaseField"),
		TEXT("FSBase"), TEXT("FS_BaseField (vector * float operators)"));
	// A double result into an operator its link first made a vector multiply, and Add's int operands
	// with a double result (the function picked again once both ends have their stated types).
	ExpectEqualRoundTrip(*this, TEXT("/Landmass/Landscape/BlueprintBrushes/Brushes/LandmassBrush_MeshStamp.LandmassBrush_MeshStamp"),
		TEXT("MeshStamp"), TEXT("LandmassBrush_MeshStamp (double * double into vector * vector)"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUBlueprintRoundTripPurityTest,
	"UEAssetDoc.UBlueprint.RoundTrip.NodeState.Purity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintRoundTripPurityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentUBlueprintRoundTripTests;
	// A "Convert to Validated Get" variable get (Properties.Pure = false).
	ExpectEqualRoundTrip(*this, TEXT("/Harmonix/Tests/HarmonixMetasoundMusicClockComponent/FTA_RecordMusicClock.FTA_RecordMusicClock"),
		TEXT("FTARecord"), TEXT("FTA_RecordMusicClock (impure variable get)"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUBlueprintRoundTripNodeKeysTest,
	"UEAssetDoc.UBlueprint.RoundTrip.NodeState.StructAndCollapsedNodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintRoundTripNodeKeysTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentUBlueprintRoundTripTests;
	// Break of a user-defined struct (K2:BreakStruct:<struct>).
	ExpectEqualRoundTrip(*this, TEXT("/DatasmithContent/Datasmith/DatasmithAreaLight.DatasmithAreaLight"),
		TEXT("DSArea"), TEXT("DatasmithAreaLight (Break of a user-defined struct)"));
	// Make / Break of native Blueprint structs without native make / break functions.
	ExpectEqualRoundTrip(*this, TEXT("/Harmonix/Tests/HarmonixMetasoundMusicClockComponent/A_MusicClockFunctionalTest.A_MusicClockFunctionalTest"),
		TEXT("MusicClock"), TEXT("A_MusicClockFunctionalTest (native Make / Break struct)"));
	// Collapsed graphs (K2:Composite, "Collapse Nodes") in an event graph and a function graph, authored
	// and round-tripped. (RenderToTexture_LevelBP holds such graphs too; its size leaves it to the sweep.)
	const FTarget Collapsed(TEXT("BP_Collapsed"));
	ON_SCOPE_EXIT { Collapsed.Cleanup(); };
	const FAssetDocumentResult Created = Apply(Collapsed, FString::Printf(TEXT(
		"{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.Blueprint\",\"Action\":\"Create\",\"Body\":{"
		"\"ParentClass\":\"/Script/Engine.Actor\","
		"\"UbergraphPages\":[{\"Name\":\"EventGraph\",\"Nodes\":[{\"Id\":\"BeginPlay\",\"Action\":\"K2:BeginPlay\"},"
		"{\"Id\":\"Setup\",\"Action\":\"K2:Composite\",\"Name\":\"Setup\"}],"
		"\"Links\":[{\"From\":\"BeginPlay:then\",\"To\":\"Setup:execute\"}],"
		"\"Subgraphs\":[{\"Name\":\"Setup\",\"Owner\":\"Setup\",\"Signature\":{\"Inputs\":[{\"Name\":\"execute\",\"Type\":\"exec\"}],"
		"\"Outputs\":[{\"Name\":\"then\",\"Type\":\"exec\"}]},\"Nodes\":[{\"Id\":\"Print\",\"Action\":\"K2:PrintString\"}],"
		"\"Links\":[{\"From\":\"Entry:execute\",\"To\":\"Print:execute\"},{\"From\":\"Print:then\",\"To\":\"Result:then\"}]}]}],"
		"\"FunctionGraphs\":[{\"Name\":\"Square\",\"Signature\":{\"Inputs\":[{\"Name\":\"In\",\"Type\":\"float\"}],"
		"\"Outputs\":[{\"Name\":\"Out\",\"Type\":\"float\"}]},"
		"\"Nodes\":[{\"Id\":\"Squared\",\"Action\":\"K2:Composite\",\"Name\":\"Squared\"}],"
		"\"Links\":[{\"From\":\"Entry:then\",\"To\":\"Result:execute\"},{\"From\":\"Entry:In\",\"To\":\"Squared:Value\"},"
		"{\"From\":\"Squared:Product\",\"To\":\"Result:Out\"}],"
		"\"Subgraphs\":[{\"Name\":\"Squared\",\"Owner\":\"Squared\",\"Signature\":{\"Inputs\":[{\"Name\":\"Value\",\"Type\":\"float\"}],"
		"\"Outputs\":[{\"Name\":\"Product\",\"Type\":\"float\"}]},"
		"\"Nodes\":[{\"Id\":\"Multiply\",\"Action\":\"K2:PromotableOperator:Multiply\"}],"
		"\"Links\":[{\"From\":\"Entry:Value\",\"To\":\"Multiply:A\"},{\"From\":\"Entry:Value\",\"To\":\"Multiply:B\"},"
		"{\"From\":\"Multiply:ReturnValue\",\"To\":\"Result:Product\"}]}]}]}}"), *Collapsed.PackageName), true);
	AddInfo(Summary(Created));
	const UBlueprint* CollapsedBlueprint = Collapsed.Get();
	if (TestTrue(TEXT("a Blueprint with collapsed graphs is created"), Created.IsSuccess() && CollapsedBlueprint != nullptr))
	{
		int32 Composites = 0;
		TArray<UEdGraph*> Graphs;
		CollapsedBlueprint->GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				Composites += Node->IsA<UK2Node_Composite>() ? 1 : 0;
			}
		}
		TestEqual(TEXT("both collapsed nodes are placed"), Composites, 2);
		const FRoundTrip Trip = RoundTrip(Collapsed.ObjectPath, TEXT("Collapsed"), /*bSave=*/true);
		AddInfo(Trip.Detail);
		TestEqual(TEXT("the collapsed graphs round-trip"), static_cast<int32>(Trip.Outcome), static_cast<int32>(ERoundTrip::Equal));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUBlueprintRoundTripClassPinTest,
	"UEAssetDoc.UBlueprint.RoundTrip.NodeState.ClassPinsAndSelf",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintRoundTripClassPinTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentUBlueprintRoundTripTests;
	// Add Component by Class: the picked class types ReturnValue before it is linked.
	ExpectEqualRoundTrip(*this, TEXT("/Harmonix/Tests/HarmonixMetasoundMusicClockComponent/FTA_StartWallClock.FTA_StartWallClock"),
		TEXT("FTAWall"), TEXT("FTA_StartWallClock (class pin before links)"));
	// A cast to the Blueprint's own class feeding a self call: re-targeted with the document.
	ExpectEqualRoundTrip(*this, TEXT("/DatasmithContent/Datasmith/DatasmithLayer.DatasmithLayer"),
		TEXT("DSLayer"), TEXT("DatasmithLayer (self references re-targeted)"));
	// Package "DatasmithArealightMesh", class "DatasmithAreaLightMesh_C": its own function calls are Self.
	ExpectEqualRoundTrip(*this, TEXT("/DatasmithContent/Datasmith/DatasmithArealightMesh.DatasmithAreaLightMesh"),
		TEXT("DSAreaMesh"), TEXT("DatasmithAreaLightMesh (own class spelled with other case)"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUBlueprintRoundTripSelectTest,
	"UEAssetDoc.UBlueprint.RoundTrip.SelectIndexType",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintRoundTripSelectTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentUBlueprintRoundTripTests;
	ExpectEqualRoundTrip(*this, TEXT("/Engine/ArtTools/RenderToTexture/Macros/RenderToTextureMacros.RenderToTextureMacros"),
		TEXT("R2TMacros"), TEXT("RenderToTextureMacros (an unlinked enum Select)"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUBlueprintRoundTripReturnNodeTest,
	"UEAssetDoc.UBlueprint.RoundTrip.NodeState.ReturnNodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintRoundTripReturnNodeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentUBlueprintRoundTripTests;
	// Overrides of a parent Blueprint's functions with outputs (no Signature stated): the created graph
	// already has its return node, which the document's Result is (not a second one).
	ExpectEqualRoundTrip(*this, TEXT("/Niagara/Blueprints/NiagaraPreviewAxisLODDistance.NiagaraPreviewAxisLODDistance"),
		TEXT("NPAxisLOD"), TEXT("NiagaraPreviewAxisLODDistance (function overrides keep one return node)"));
	return !HasAnyErrors();
}

namespace AssetDocumentUBlueprintRoundTripTests
{
// A source the editor itself cannot author: the Create is refused with this code, leaving nothing.
void ExpectRefusedRoundTrip(FAutomationTestBase& Test, const TCHAR* Source, const TCHAR* Prefix, const TCHAR* Code, const TCHAR* What)
{
	const FRoundTrip Result = RoundTrip(Source, Prefix, /*bSave=*/true);
	if (!Result.Detail.IsEmpty())
	{
		Test.AddInfo(Result.Detail);
	}
	Test.TestEqual(*FString::Printf(TEXT("%s is refused"), What), static_cast<int32>(Result.Outcome), static_cast<int32>(ERoundTrip::Refused));
	Test.TestEqual(*FString::Printf(TEXT("%s: refusal code"), What), Result.Class, FString(Code));
	Test.TestEqual(TEXT("a refused Create leaves nothing"), Result.Residue, FString());
	Test.TestFalse(TEXT("rollback is not poisoned"), Result.bPoisoned);
}
} // namespace AssetDocumentUBlueprintRoundTripTests

// Sweep classes: graph nodes the editor places only in context. The sweep-class tests round-trip
// many engine Blueprints each, so they sit in top-level groups of their own (UBlueprintSweepContext,
// UBlueprintSweepNodes): a suite runs one editor process, with one time limit, per top-level group.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUBlueprintRoundTripGraphContextTest,
	"UEAssetDoc.UBlueprintSweepContext.GraphContext",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintRoundTripGraphContextTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentUBlueprintRoundTripTests;
	// "Parent: Construction Script" (K2:CallParentFunction) in a child Blueprint's construction script.
	ExpectEqualRoundTrip(*this, TEXT("/SunPosition/BP_SunPosition.BP_SunPosition"), TEXT("SunPos"),
		TEXT("BP_SunPosition (parent construction script call)"));
	// Overrides of inherited functions: their flags are the overridden function's.
	ExpectEqualRoundTrip(*this, TEXT("/MoverExamples/Gameplay/MovementModes/BP_FlyingMode.BP_FlyingMode"), TEXT("FlyMode"),
		TEXT("BP_FlyingMode (function overrides keep inherited flags)"));
	// A bound event of a component whose instance class (not its declared class) declares the delegate.
	ExpectEqualRoundTrip(*this, TEXT("/NetworkPredictionExtras/NetworkPredictionExtras_FlyingPawn_MockAbility.NetworkPredictionExtras_FlyingPawn_MockAbility"),
		TEXT("MockAbility"), TEXT("FlyingPawn_MockAbility (bound event of the component instance class)"));
	// Get Engine Subsystem nodes of several subsystem classes (one spawner per class, one signature).
	ExpectEqualRoundTrip(*this, TEXT("/MovieRenderPipeline/Blueprints/BP_MovieRenderRuntimeSample.BP_MovieRenderRuntimeSample"),
		TEXT("MRQRuntime"), TEXT("BP_MovieRenderRuntimeSample (Get Engine Subsystem by class)"));
	// Divide -> Multiply (Vector * float) and -> Multiply (Rotator * float32): operands linked first.
	ExpectEqualRoundTrip(*this, TEXT("/MoverExamples/Gameplay/BP_ExtraMovementInducer.BP_ExtraMovementInducer"),
		TEXT("ExtraInducer"), TEXT("BP_ExtraMovementInducer (an operator output linked after its operands)"));
	// Pin Conversions (Multiply's double B next to a vector A) held while later links re-pick the function.
	ExpectEqualRoundTrip(*this, TEXT("/Engine/EditorResources/FieldNodes/FS_BombField_Prototype.FS_BombField_Prototype"),
		TEXT("BombField"), TEXT("FS_BombField_Prototype (operator Pin Conversions kept across links)"));
	ExpectEqualRoundTrip(*this, TEXT("/Engine/EditorResources/FieldNodes/Linear_Force_for_Cloth.Linear_Force_for_Cloth"),
		TEXT("ClothForce"), TEXT("Linear_Force_for_Cloth (operator Pin Conversions kept across links)"));
	// Collapsed graphs copied within a Blueprint keep one GraphGuid: reported unique.
	ExpectEqualRoundTrip(*this, TEXT("/Landmass/Landscape/OldPrototype_BP/Components/ErosionComponent.ErosionComponent"),
		TEXT("Erosion"), TEXT("ErosionComponent (collapsed graphs sharing a GraphGuid)"));
	// Interface graphs stored in the interface's history order read in the order implementing it makes.
	ExpectEqualRoundTrip(*this, TEXT("/Landmass/Landscape/BlueprintBrushes/Brushes/LandmassBrush_ProceduralNoise.LandmassBrush_ProceduralNoise"),
		TEXT("BrushNoise"), TEXT("LandmassBrush_ProceduralNoise (interface graph order)"));
	return !HasAnyErrors();
}

// Sweep classes: node state and palette admission.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUBlueprintRoundTripGraphNodesTest,
	"UEAssetDoc.UBlueprintSweepNodes.GraphNodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintRoundTripGraphNodesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentUBlueprintRoundTripTests;
	// A Blueprint with no graph content but its (const) event graph.
	ExpectEqualRoundTrip(*this, TEXT("/Engine/EngineDamageTypes/DmgTypeBP_Environmental.DmgTypeBP_Environmental"), TEXT("DmgEnv"),
		TEXT("DmgTypeBP_Environmental (an empty event graph)"));
	// Calls of the Blueprint's own custom events, placed before the events exist in the skeleton.
	ExpectEqualRoundTrip(*this, TEXT("/Water/FluidSimulation/WaveFoam/WaveFoamRenderer.WaveFoamRenderer"), TEXT("WaveFoam"),
		TEXT("WaveFoamRenderer (calls of own custom events)"));
	// An Enhanced Input action event (its spawner is filed under the UInputAction asset).
	ExpectEqualRoundTrip(*this, TEXT("/MoverExamples/Pawns/AnimatedMannyPawnExtended.AnimatedMannyPawnExtended"), TEXT("MannyExt"),
		TEXT("AnimatedMannyPawnExtended (Enhanced Input action event)"));
	// A function taking a map (key and value types).
	ExpectEqualRoundTrip(*this, TEXT("/Landmass/Landscape/BlueprintBrushes/Libraries/Landmass_FunctionLibrary.Landmass_FunctionLibrary"),
		TEXT("LandmassLib"), TEXT("Landmass_FunctionLibrary (map parameters)"));
	// Links between wildcard pins wait for the link that types one of them.
	ExpectEqualRoundTrip(*this, TEXT("/Water/FluidSimulation/Blueprints/Components/ShallowWaterSimComponent.ShallowWaterSimComponent"),
		TEXT("ShallowSim"), TEXT("ShallowWaterSimComponent (wildcard-to-wildcard links)"));
	// A container function's container linked before its element (Array Add of a Texture 2D into an Object array).
	ExpectEqualRoundTrip(*this, TEXT("/Landmass/Landscape/BlueprintBrushes/Brushes/LandmassBrush_CopyRegion.LandmassBrush_CopyRegion"),
		TEXT("CopyRegion"), TEXT("LandmassBrush_CopyRegion (container before element)"));
	return !HasAnyErrors();
}

// Sweep classes: member values the editor holds as instanced subobjects and type normalization.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUBlueprintRoundTripInstancedValuesTest,
	"UEAssetDoc.UBlueprintSweepContext.InstancedValues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintRoundTripInstancedValuesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentUBlueprintRoundTripTests;
	// A Mover component's instanced MovementModes map (an instanced object per mode, with its values).
	ExpectEqualRoundTrip(*this, TEXT("/MoverExamples/Gameplay/BP_MoverPlatform.BP_MoverPlatform"), TEXT("MoverPlat"),
		TEXT("BP_MoverPlatform (instanced movement modes)"));
	// An object variable whose class is gone is an Object variable to the editor.
	ExpectEqualRoundTrip(*this, TEXT("/Engine/FunctionalTesting/Blueprints/AITesting_MoveGoal.AITesting_MoveGoal"), TEXT("MoveGoal"),
		TEXT("AITesting_MoveGoal (class-less object variable)"));
	// A map in a struct variable's default, which compiling reverses (the struct default has entries):
	// the same map in another order (FMapProperty::Identical).
	ExpectEqualRoundTrip(*this, TEXT("/Water/FluidSimulation/Blueprints/Examples/BP_Dynamic_Force_SkelMesh.BP_Dynamic_Force_SkelMesh"),
		TEXT("DynForce"), TEXT("BP_Dynamic_Force_SkelMesh (map default reordered by a compile)"));
	return !HasAnyErrors();
}

// Sweep refusals: sources the editor itself cannot author.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUBlueprintRoundTripRefusalsTest,
	"UEAssetDoc.UBlueprintSweepNodes.Refusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintRoundTripRefusalsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentUBlueprintRoundTripTests;
	// A variable get of a variable that no longer exists.
	ExpectRefusedRoundTrip(*this, TEXT("/Landmass/DistanceFields/Blueprints/JumpFlood_Component2D.JumpFlood_Component2D"), TEXT("JumpFlood"),
		TEXT("Graph.UnresolvedReference"), TEXT("JumpFlood_Component2D (a variable get of a removed variable)"));
	// A variable named with a '.' (the editor's name validation rejects it).
	ExpectRefusedRoundTrip(*this, TEXT("/Landmass/Landscape/BlueprintBrushes/CustomBrush_MaterialOnly.CustomBrush_MaterialOnly"), TEXT("BrushMat"),
		TEXT("BlueprintMember.InvalidName"), TEXT("CustomBrush_MaterialOnly (a variable name with '.')"));
	// An input action event of an action the project's Input settings do not define.
	ExpectRefusedRoundTrip(*this, TEXT("/NetworkPredictionExtras/Animation/NetworkPrediction_RootMotionActor.NetworkPrediction_RootMotionActor"),
		TEXT("NPRootMotion"), TEXT("Graph.UnresolvedReference"), TEXT("NetworkPrediction_RootMotionActor (undefined input action)"));
	// A macro library whose parent the editor's class picker does not offer.
	ExpectRefusedRoundTrip(*this, TEXT("/NetworkPredictionExtras/NetworkPredictionExtras_BP_Macros.NetworkPredictionExtras_BP_Macros"),
		TEXT("NPMacros"), TEXT("BlueprintLifecycle.InvalidParentClass"), TEXT("NetworkPredictionExtras_BP_Macros (LevelScriptActor macro library)"));
	// A runtime macro library calling a function of an editor-only Blueprint class (LandmassManagerBase).
	ExpectRefusedRoundTrip(*this, TEXT("/Landmass/Landscape/BlueprintBrushes/Libraries/LandscapeBrushLibrary.LandscapeBrushLibrary"),
		TEXT("BrushLib"), TEXT("Graph.EditorOnlyFunction"), TEXT("LandscapeBrushLibrary (an editor-only function in a runtime macro)"));
	// The copy's self into a Brush Manager pin another asset declares (the re-target's limit); reaching that
	// link means its event graph's collapsed "Shape Drawing", which calls an editor-only function of a
	// Blueprint of an editor-only class, was made (checked against its top-level graph).
	ExpectRefusedRoundTrip(*this, TEXT("/Landmass/Landscape/BlueprintBrushes/LandmassBrushManager.LandmassBrushManager"),
		TEXT("BrushMgr"), TEXT("Graph.ConnectDisallowed"), TEXT("LandmassBrushManager (an editor-only call in a collapsed graph, then the re-target's self)"));
	// A link the source holds though its schema refuses it (GetLocalPawn's Pawn into a Player Camera
	// Manager target); reaching it means the "Follow Player " function, whose name ends in a space, was called.
	ExpectRefusedRoundTrip(*this, TEXT("/Water/FluidSimulation/Blueprints/BP_FluidSim_01.BP_FluidSim_01"),
		TEXT("FluidSim"), TEXT("Graph.ConnectDisallowed"), TEXT("BP_FluidSim_01 (a source link the schema refuses)"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUBlueprintTypeRulesTest,
	"UEAssetDoc.UBlueprint.BlueprintType.Rules",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintTypeRulesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentUBlueprintRoundTripTests;
	const FTarget Target(TEXT("BPI_Rules"));
	ON_SCOPE_EXIT { Target.Cleanup(); };
	auto Document = [&Target](const TCHAR* Action, const TCHAR* Body)
	{
		return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.Blueprint\",\"Action\":\"%s\",\"Body\":{%s}}"),
			*Target.PackageName, Action, Body);
	};
	// A Blueprint Interface (parent defaults to Interface) with one function signature.
	const FAssetDocumentResult Created = Apply(Target, Document(TEXT("Create"),
		TEXT("\"BlueprintType\":\"Interface\",\"FunctionGraphs\":[{\"Name\":\"Use\",\"Signature\":{\"Inputs\":[{\"Name\":\"Amount\",\"Type\":\"int\"}]},")
		TEXT("\"Nodes\":[],\"Links\":[]}]")), true);
	AddInfo(Summary(Created));
	TestTrue(TEXT("a Blueprint Interface is created"), Created.IsSuccess());
	const UBlueprint* Blueprint = Target.Get();
	if (!TestNotNull(TEXT("the interface exists"), Blueprint))
	{
		return false;
	}
	TestEqual(TEXT("made by the interface factory"), static_cast<int32>(Blueprint->BlueprintType), static_cast<int32>(BPTYPE_Interface));
	TestEqual(TEXT("no event graph"), Blueprint->UbergraphPages.Num(), 0);
	const FAssetDocumentResult Extracted = Extract(Target.ObjectPath);
	FString Type;
	TestTrue(TEXT("the extraction states the type"), Extracted.Payload.IsValid()
		&& Extracted.Payload->GetObjectField(TEXT("Body"))->TryGetStringField(TEXT("BlueprintType"), Type) && Type == TEXT("Interface"));

	const FAssetDocumentResult Changed = Apply(Target, Document(TEXT("Update"), TEXT("\"BlueprintType\":\"Normal\"")), true);
	AddInfo(Summary(Changed));
	TestTrue(TEXT("an Update cannot change the type"), !Changed.IsSuccess() && HasCode(Changed, TEXT("UBlueprint.BlueprintTypeImmutable")));
	const FAssetDocumentResult Region = Apply(Target, Document(TEXT("Update"),
		TEXT("\"BlueprintType\":\"Interface\",\"Variables\":[{\"Name\":\"Health\",\"Type\":\"float\"}]")), true);
	AddInfo(Summary(Region));
	TestTrue(TEXT("an interface has no variables"), !Region.IsSuccess() && HasCode(Region, TEXT("UBlueprint.BlueprintTypeRegionUnsupported")));
	const UBlueprint* After = Target.Get();
	TestTrue(TEXT("the refused Updates left the interface"), After != nullptr && After->BlueprintType == BPTYPE_Interface);

	// A Blueprint Function Library (parent defaults to BlueprintFunctionLibrary): a static function,
	// no event graph; extract -> re-targeted Create -> extract is equal.
	const FTarget Library(TEXT("BFL_Rules"));
	ON_SCOPE_EXIT { Library.Cleanup(); };
	const FAssetDocumentResult LibraryCreated = Apply(Library, FString::Printf(TEXT(
		"{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.Blueprint\",\"Action\":\"Create\",\"Body\":{"
		"\"BlueprintType\":\"FunctionLibrary\",\"FunctionGraphs\":[{\"Name\":\"Twice\","
		"\"Signature\":{\"Inputs\":[{\"Name\":\"Value\",\"Type\":\"int\"}],\"Outputs\":[{\"Name\":\"Result\",\"Type\":\"int\"}],\"Pure\":true},"
		"\"Nodes\":[{\"Id\":\"Add\",\"Action\":\"K2:PromotableOperator:Add\"}],"
		"\"Links\":[{\"From\":\"Entry:Value\",\"To\":\"Add:A\"},{\"From\":\"Entry:Value\",\"To\":\"Add:B\"},"
		"{\"From\":\"Add:ReturnValue\",\"To\":\"Result:Result\"}]}]}}"), *Library.PackageName), true);
	AddInfo(Summary(LibraryCreated));
	const UBlueprint* LibraryBlueprint = Library.Get();
	TestTrue(TEXT("a Function Library is created by its factory"), LibraryCreated.IsSuccess() && LibraryBlueprint != nullptr
		&& LibraryBlueprint->BlueprintType == BPTYPE_FunctionLibrary && LibraryBlueprint->UbergraphPages.IsEmpty());
	if (LibraryBlueprint != nullptr)
	{
		const FRoundTrip LibraryTrip = RoundTrip(Library.ObjectPath, TEXT("BFL"), /*bSave=*/true);
		AddInfo(LibraryTrip.Detail);
		TestEqual(TEXT("the Function Library round-trips"), static_cast<int32>(LibraryTrip.Outcome), static_cast<int32>(ERoundTrip::Equal));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintRoundTripSweepTest,
	"UEAssetDoc.Coverage.BlueprintRoundTripSweep",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// Every Blueprint (class /Script/Engine.Blueprint) outside /Game: copy -> extract -> re-targeted
// Create -> extract equality. Logs the pass rate and the failure classes; fails only when a failed
// Create leaves something behind or poisons the rollback.
bool FAssetDocumentBlueprintRoundTripSweepTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentUBlueprintRoundTripTests;
	TArray<FAssetData> Assets;
	IAssetRegistry::GetChecked().GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), Assets, /*bSearchSubClasses=*/false);
	Assets.RemoveAll([](const FAssetData& Asset)
	{
		const FString Package = Asset.PackageName.ToString();
		return Package.StartsWith(TEXT("/Game/")) || Package.StartsWith(TEXT("/Temp/")) || Package.StartsWith(TEXT("/Script/"));
	});
	Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.LexicalLess(B.PackageName); });
	TArray<FString> Sorted;
	for (const FAssetData& Asset : Assets)
	{
		Sorted.Add(Asset.GetObjectPathString());
	}
	// Saved/BlueprintRoundTripSweep.filter (optional): filters, resume, shard (see the top of this file).
	FString FilterText;
	FFileHelper::LoadFileToString(FilterText, *FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BlueprintRoundTripSweep.filter")));
	const FSweepPlan Plan = ParseSweepFilter(FilterText);
	for (const FString& Error : Plan.Errors)
	{
		AddError(FString::Printf(TEXT("Saved/BlueprintRoundTripSweep.filter: %s"), *Error));
	}
	if (!Plan.Filters.IsEmpty() || Plan.bResume || Plan.ShardCount > 0)
	{
		AddInfo(FString::Printf(TEXT("Saved/BlueprintRoundTripSweep.filter: filters [%s]%s%s"), *FString::Join(Plan.Filters, TEXT(", ")),
			Plan.bResume ? TEXT(", resume") : TEXT(""),
			Plan.ShardCount > 0 ? *FString::Printf(TEXT(", shard %d/%d"), Plan.Shard, Plan.ShardCount) : TEXT("")));
	}
	const FString ReportFile = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BlueprintRoundTripSweep.tsv"));
	FString Previous;
	if (Plan.bResume)
	{
		FFileHelper::LoadFileToString(Previous, *ReportFile);
	}
	else
	{
		IFileManager::Get().Delete(*ReportFile, false, true);
	}
	TArray<FString> Killed;
	const TArray<FString> Selected = SelectSweepAssets(Sorted, Plan, ReadSweepRows(Previous), Killed);
	for (const FString& Path : Killed)
	{
		AppendSweepRow(ReportFile, FString::Printf(TEXT("%s\tKilled\t\tthe previous run died on this Blueprint (watchdog or crash)"), *Path));
	}
	AddInfo(FString::Printf(TEXT("rows: %s (%d Blueprints to run, %d killed before)"), *ReportFile, Selected.Num(), Killed.Num()));
	// Engine content that does not compile cleanly once copied (level script self references,
	// deprecated nodes) logs compiler errors; they are a property of the source, not of the round trip.
	// (A negative count suppresses any number of them; 0 would require at least one.)
	AddExpectedError(TEXT("[Compiler]"), EAutomationExpectedErrorFlags::Contains, -1, /*bIsRegex=*/false);
	static const TCHAR* Names[] = {TEXT("Equal"), TEXT("CopyFailed"), TEXT("ExtractFailed"), TEXT("CreateFailed"),
		TEXT("CopyExtractFailed"), TEXT("Mismatch"), TEXT("Refused")};
	for (const FString& Path : Selected)
	{
		AppendSweepRow(ReportFile, Path + TEXT("\tStarted"));
		// A Create that does not save stops after Preflight: the round trip needs the saved Blueprint.
		const FRoundTrip Result = RoundTrip(Path, TEXT("Sweep"), /*bSave=*/true);
		AppendSweepRow(ReportFile, FString::Printf(TEXT("%s\t%s\t%s\t%s"), *Path, Names[static_cast<int32>(Result.Outcome)], *Result.Class,
			*Result.Detail.Replace(TEXT("\n"), TEXT(" | ")).Replace(TEXT("\t"), TEXT(" ")).Left(2000)));
		TestEqual(*FString::Printf(TEXT("%s: a failed Create leaves nothing"), *Path), Result.Residue, FString());
		TestFalse(*FString::Printf(TEXT("%s: rollback is not poisoned"), *Path), Result.bPoisoned);
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	}
	// Summary of every outcome row of the file (earlier runs' rows included when resuming).
	FString AllRows;
	FFileHelper::LoadFileToString(AllRows, *ReportFile);
	const FSweepRows Rows = ReadSweepRows(AllRows);
	TMap<FString, TArray<FString>> Classes;
	int32 Passed = 0;
	int32 RefusedRows = 0;
	for (const TPair<FString, FString>& Row : Rows.Outcomes)
	{
		TArray<FString> Fields;
		Row.Value.ParseIntoArray(Fields, TEXT("\t"), /*InCullEmpty=*/false);
		const FString Outcome = Fields.IsValidIndex(0) ? Fields[0] : FString();
		if (Outcome == TEXT("Equal"))
		{
			++Passed;
		}
		else
		{
			RefusedRows += Outcome == TEXT("Refused") ? 1 : 0;
			Classes.FindOrAdd(Outcome + TEXT(" ") + (Fields.IsValidIndex(1) ? Fields[1] : FString())).Add(Row.Key);
		}
	}
	AddInfo(FString::Printf(TEXT("Blueprint round trip sweep: %d/%d equal, %d documented refusals"), Passed, Rows.Outcomes.Num(), RefusedRows));
	Classes.ValueSort([](const TArray<FString>& A, const TArray<FString>& B) { return A.Num() > B.Num(); });
	for (const TPair<FString, TArray<FString>>& Class : Classes)
	{
		AddInfo(FString::Printf(TEXT("%4d %s: %s"), Class.Value.Num(), *Class.Key, *FString::Join(Class.Value, TEXT(", ")).Left(1500)));
	}
	return !HasAnyErrors();
}
#endif

