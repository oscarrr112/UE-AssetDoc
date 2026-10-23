// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Control Rig apply, graphs and functions (docs/architecture/asset-document-controlrig-design.md §7.2: graphs, functions).
// UEAssetDoc.ControlRig.GraphFidelity: §6.2 on CR_AllKinds (every node kind, a local public function
//   with an input default and a local variable, a function node, a collapse node) and every corpus rig and rig module
//   (#1-#9, #11-#13; modular rigs are tested separately and reported as skipped): Create a new asset from the whole extract; it
//   extracts as the source, compiles, its VM (ComputeVMHash, instruction count) and its runtime pose after
//   construction + Forwards Solve (every element's global transform, every curve) equal the source's; the unchanged
//   re-apply is a no-op; reload stable; timings.
// UEAssetDoc.ControlRig.GraphFreeEdits: §6.3 graph edits made once through the RigVM controller calls
//   the editor's graph panel makes and once through a document, on two duplicates of #7 (and of CR_AllKinds minus
//   functions): equal state, compile status, VM and pose.
// UEAssetDoc.ControlRig.GraphRollback: a refused graph op after others ran, a new deprecated unit and a
//   function node of a missing function are refused and roll back; a deprecated unit on Create is kept with a warning;
//   on the reloaded rig, a node of a rolled-back apply is added again after a garbage collection.
// UEAssetDoc.ControlRig.FunctionFreeEdits: §6.3 function edits (a new function with two inputs and a
//   local variable, called twice, a collapse node; then renamed, made public, its default and local renamed) made once
//   through the controller calls of My Blueprint / the function Details / the graph panel and once through a document,
//   on duplicates of #7 and CR_AllKinds: equal state, compile status, VM and pose.
// UEAssetDoc.ControlRig.FunctionRollback: rigs made from documents (a public function reading a member
//   variable, another rig calling it with a variable map); a failing apply after function, interface, variable map and
//   collapse edits rolls back (the variable map restored); a rename of the public function warns
//   ControlRig.DependentsAffected naming the caller.
// UEAssetDoc.ControlRig.FunctionUndo: undo / redo after a function apply and after a failed one.
// UEAssetDoc.ControlRig.GraphUndo: undo / redo of a graph apply and of a failed one keep the rig whole.

#include "AssetDocumentIKRigTestSupport.h"

#include "AssetDocumentControlRig.h"
#include "Misc/EngineVersionComparison.h"

#include "ControlRig.h"
#include "ControlRigBlueprintLegacy.h"
#include "EdGraph/RigVMEdGraph.h"
#include "EdGraph/RigVMEdGraphNode.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Rigs/RigHierarchy.h"
#include "RigVMCore/RigVM.h"
#include "RigVMCore/RigVMMemoryStorageStruct.h"
#include "RigVMCore/RigVMStruct.h"
#include "RigVMModel/Nodes/RigVMCommentNode.h"
#include "RigVMModel/Nodes/RigVMLibraryNode.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMFunctionLibrary.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMLink.h"
#include "RigVMModel/RigVMPin.h"
#include "Runtime/AssetDocumentUndoHistory.h"
#include "ScopedTransaction.h"
#include "Units/Execution/RigUnit_BeginExecution.h"
#include "Units/RigUnit.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/StructOnScope.h"
#include "UObject/UObjectIterator.h"
#include "Algo/Unique.h"

#if WITH_DEV_AUTOMATION_TESTS

// Fixtures of the read tests (AssetDocumentControlRigTests.cpp).
namespace AssetDocumentControlRigTests
{
struct FTemplateContent;
UControlRigBlueprint* MakeAllKinds(FAutomationTestBase& Test, const AssetDocumentIKRigTests::FTarget& Target);
TSharedPtr<FTemplateContent> ShareCorpusTemplates();
void ForEachCorpusEntry(TFunctionRef<void(const TCHAR* Path, const TCHAR* Why)> Visit);
} // namespace AssetDocumentControlRigTests

namespace AssetDocumentControlRigGraphTests
{
using namespace AssetDocumentIKRigTests;

TSharedPtr<FJsonObject> ForTarget(const TSharedPtr<FJsonObject>& Document, const FTarget& Target, const TCHAR* Action)
{
	const TSharedPtr<FJsonObject> Copy = Parse(Serialize(Document));
	Copy->SetStringField(TEXT("Target"), Target.PackageName);
	Copy->SetStringField(TEXT("Action"), Action);
	return Copy;
}

TSharedPtr<FJsonObject> Json(const ANSICHAR* Text)
{
	return Parse(FString(UTF8_TO_TCHAR(Text)));
}

TSharedPtr<FJsonObject> ObjectAt(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	const TSharedPtr<FJsonObject>* Found = nullptr;
	return Object.IsValid() && Object->TryGetObjectField(Field, Found) ? *Found : nullptr;
}

// Body/Graphs/<Graph>/Nodes.
TSharedPtr<FJsonObject> NodesOf(const TSharedPtr<FJsonObject>& Document, const TCHAR* Graph = TEXT("Rig"))
{
	return ObjectAt(ObjectAt(ObjectAt(BodyOf(Document), TEXT("Graphs")), Graph), TEXT("Nodes"));
}

bool ApplyOk(FAutomationTestBase& Test, const FTarget& Target, const TSharedPtr<FJsonObject>& Document, const FString& Stage,
	FAssetDocumentResult* OutResult = nullptr)
{
	const FAssetDocumentResult Result = Apply(Target, Document);
	if (OutResult != nullptr)
	{
		*OutResult = Result;
	}
	if (!Test.TestTrue(*FString::Printf(TEXT("%s applies"), *Stage), Result.IsSuccess()))
	{
		Test.AddInfo(FString::Printf(TEXT("%s: %s"), *Stage, *DiagnosticsText(Result)));
		return false;
	}
	return true;
}

FString StatusName(const FAssetDocumentResult& Extracted)
{
	return Extracted.Compile.IsValid() ? Extracted.Compile->GetStringField(TEXT("Status")) : FString(TEXT("-"));
}

FString MessagesOf(const FAssetDocumentResult& Extracted, const FString& PackageName)
{
	const TArray<TSharedPtr<FJsonValue>>* Messages = nullptr;
	if (!Extracted.Compile.IsValid() || !Extracted.Compile->TryGetArrayField(TEXT("Messages"), Messages))
	{
		return FString();
	}
	TArray<FString> Lines;
	for (const TSharedPtr<FJsonValue>& Message : *Messages)
	{
		Lines.Add(Serialize(Message->AsObject()).Replace(*PackageName, TEXT("")));
	}
	return FString::Join(Lines, TEXT("\n"));
}

bool Compiles(const FAssetDocumentResult& Extracted)
{
	return StatusName(Extracted) == TEXT("UpToDate") || StatusName(Extracted) == TEXT("UpToDateWithWarnings");
}

// What the rig computes: the class VM (hash, instruction count, and the hash's parts for a mismatch report) and the
// pose of a new instance after the construction event and one Forwards Solve, element by element (global transforms,
// curve values).
struct FRigElementPose
{
	FString Key;
	TArray<double> Values;
	// The Blueprint hierarchy's initial global transform (what the instance starts from), for a mismatch report.
	FString Initial;
};

struct FPinText
{
	FString Text;
	FString CPPType;
	TWeakObjectPtr<UObject> TypeObject;
};

struct FRigFacts
{
	uint32 VMHash = 0;
	int32 Instructions = -1;
	// ComputeVMHash's parts: function names (per Execute instruction), byte code hash, literal memory (name type value), work memory (name type).
	FString Functions;
	uint32 ByteCodeHash = 0;
	FString Literals;
	FString Work;
	// "<graph>/<node>.<pin>" -> the default text of each unlinked input pin: the compiler shares one literal between pins
	// whose default texts are equal, so the same value in another text form changes literal memory.
	TMap<FString, FPinText> PinDefaults;
	TArray<FRigElementPose> Pose;
};

FString Number(double Value)
{
	const double Rounded = FMath::RoundToDouble(Value * 1000.0) / 1000.0;
	return FString::Printf(TEXT("%.3f"), Rounded == 0.0 ? 0.0 : Rounded);
}

FString MemoryText(const FRigVMMemoryStorageStruct& Memory, bool bValues)
{
	TArray<FString> Lines;
	for (const FProperty* Property : Memory.GetProperties())
	{
		FString Line = Property->GetName() + TEXT(" ") + Property->GetCPPType();
		if (bValues)
		{
			FString Value;
			const uint8* Data = Memory.GetData<uint8>(Property);
			Property->ExportTextItem_Direct(Value, Data, Data, nullptr, PPF_None, nullptr);
			Line += TEXT(" = ") + Value;
		}
		Lines.Add(MoveTemp(Line));
	}
	return FString::Join(Lines, TEXT("\n"));
}

// The editor-equivalent compile first (both sides of a comparison: a rig loaded or reloaded after a persisted apply
// may hold a generated class the last garbage collection left without its VM).
FRigFacts FactsOf(UControlRigBlueprint& Blueprint)
{
	FRigFacts Facts;
	UPackage* Package = Blueprint.GetOutermost();
	const bool bWasDirty = Package->IsDirty();
	FKismetEditorUtilities::CompileBlueprint(&Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
	Blueprint.PropagateHierarchyFromBPToInstances();
	Package->SetDirtyFlag(bWasDirty);
	UClass* Class = Blueprint.GeneratedClass;
	if (Class == nullptr || !Class->IsChildOf(UControlRig::StaticClass()))
	{
		return Facts;
	}
	static const FObjectProperty* VMProperty = FindFProperty<FObjectProperty>(URigVMHost::StaticClass(), TEXT("VM"));
	const UObject* Defaults = Class->GetDefaultObject(false);
	if (const URigVM* VM = VMProperty != nullptr && Defaults != nullptr ? Cast<URigVM>(VMProperty->GetObjectPropertyValue_InContainer(Defaults)) : nullptr)
	{
		Facts.VMHash = VM->ComputeVMHash();
		Facts.Instructions = VM->GetByteCode().GetNumInstructions();
		// The function each Execute instruction calls (the VM's function name table is not public).
		TArray<FString> Names;
		const FRigVMByteCode& ByteCode = VM->GetByteCode();
		for (const FRigVMInstruction& Instruction : ByteCode.GetInstructions())
		{
			if (ByteCode.GetOpCodeAt(Instruction.ByteCodeIndex) == ERigVMOpCode::Execute)
			{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
				Names.Add(VM->GetRigVMFunctionName(ByteCode.GetOpAt<FRigVMExecuteOp>(Instruction).CallableIndex));
#else
				Names.Add(VM->GetRigVMFunctionName(ByteCode.GetOpAt<FRigVMExecuteOp>(Instruction).FunctionIndex));
#endif
			}
		}
		Facts.Functions = FString::Join(Names, TEXT("\n"));
		Facts.ByteCodeHash = VM->GetByteCode().GetByteCodeHash();
		if (const FRigVMMemoryStorageStruct* Literals = VM->GetLiteralMemory())
		{
			Facts.Literals = MemoryText(*Literals, true);
		}
		Facts.Work = MemoryText(VM->GetDefaultWorkMemory(), false);
	}
	if (const FRigVMClient* Client = static_cast<URigVMBlueprint&>(Blueprint).GetRigVMClient())
	{
		for (const URigVMGraph* Model : Client->GetAllModels(true, true))
		{
			for (const URigVMNode* Node : Model != nullptr ? Model->GetNodes() : TArray<URigVMNode*>())
			{
				for (const URigVMPin* Pin : Node != nullptr && !Node->IsInjected() ? Node->GetPins() : TArray<URigVMPin*>())
				{
					// A hidden pin is the unit's own state (a cache), not a value anyone authors.
					if (!Pin->IsExecuteContext() && Pin->GetDirection() != ERigVMPinDirection::Output && Pin->GetDirection() != ERigVMPinDirection::Hidden
						&& Pin->GetSourceLinks(true).IsEmpty())
					{
						Facts.PinDefaults.Add(Model->GetNodePath() + TEXT("/") + Pin->GetPinPath(),
							{Pin->GetDefaultValue(), Pin->GetCPPType(), Pin->GetCPPTypeObject()});
					}
				}
			}
		}
	}
	TStrongObjectPtr<UControlRig> Rig(NewObject<UControlRig>(GetTransientPackage(), Class, NAME_None, RF_Transient));
	Rig->Initialize(true);
	Rig->RequestConstruction();
	Rig->Evaluate_AnyThread();
	Rig->Evaluate_AnyThread();
	URigHierarchy* Hierarchy = Rig->GetHierarchy();
	if (Hierarchy == nullptr)
	{
		return Facts;
	}
	for (const FRigElementKey& Key : Hierarchy->GetAllKeys(true))
	{
		if (Key.Type == ERigElementType::Curve)
		{
			Facts.Pose.Add({Key.ToString(), {static_cast<double>(Hierarchy->GetCurveValue(Key))}});
			continue;
		}
		if (Key.Type != ERigElementType::Bone && Key.Type != ERigElementType::Null && Key.Type != ERigElementType::Control)
		{
			continue;
		}
		// An animation channel is not a 3D thing (its parent weight is fixed: 0 for one made today, 1 for an older one):
		// its value is its pose.
		if (const FRigControlElement* Channel = Hierarchy->Find<FRigControlElement>(Key); Channel != nullptr && Channel->IsAnimationChannel())
		{
			const FTransform Value = Hierarchy->GetControlValue(Key, ERigControlValueType::Current).GetAsTransform(Channel->Settings.ControlType,
				Channel->Settings.PrimaryAxis);
			const FVector T = Value.GetTranslation();
			const FRotator R = Value.Rotator();
			Facts.Pose.Add({Key.ToString(), {T.X, T.Y, T.Z, R.Pitch, R.Yaw, R.Roll}, FString(TEXT("channel"))});
			continue;
		}
		const FTransform Global = Hierarchy->GetGlobalTransform(Key);
		const FVector T = Global.GetTranslation();
		// q and -q are the same rotation: compare the one with W >= 0.
		FQuat Q = Global.GetRotation();
		if (Q.W < 0.0)
		{
			Q = FQuat(-Q.X, -Q.Y, -Q.Z, -Q.W);
		}
		const FVector S = Global.GetScale3D();
		const URigHierarchy* Authored = Blueprint.GetHierarchy();
		const FString Initial = Authored != nullptr && Authored->Contains(Key)
			? Authored->GetInitialGlobalTransform(Key).GetTranslation().ToString() : FString(TEXT("-"));
		Facts.Pose.Add({Key.ToString(), {T.X, T.Y, T.Z, Q.X, Q.Y, Q.Z, Q.W, S.X, S.Y, S.Z}, Initial});
	}
	return Facts;
}

FString PoseLine(const FRigElementPose& Element)
{
	TArray<FString> Values;
	for (const double Value : Element.Values)
	{
		Values.Add(Number(Value));
	}
	return Element.Key + TEXT(" ") + FString::Join(Values, TEXT(" "));
}

// Equal VM content; equal pose within 0.01 (cm / quaternion component / scale / curve value: float evaluation order).
// True when the poses are equal.
// bFromSource (a rig created from a shipped rig's document): the compiler shares one literal between pins of equal
// default text and names work memory after nodes, so a source whose texts or hidden names no editor action makes
// again (a bool toggled to "true" where a new pin reads "True", "(Type=Bone)" for "(Type=Bone,Name="None")", aggregate
// inner nodes an older engine named, pins of a stale function reference node in an older order) compiles to another
// ComputeVMHash. Such a rig counts as equal when it calls the same functions (in the same order, or in another order
// with an equal runtime pose), has as many instructions, an equal pose, and no pin whose value (not text) differs.
// Work memory that a function reference's inlined function uses is named "Hash<n>_<name>" on some engines, n a hash of
// the reference node's object path, which names the rig's package: the same program in another package differs there.
FString WithoutReferenceHash(const FString& Line)
{
	int32 Digits = 4;
	while (Line.StartsWith(TEXT("Hash")) && Digits < Line.Len() && FChar::IsDigit(Line[Digits]))
	{
		++Digits;
	}
	if (Digits <= 4 || Digits >= Line.Len() || Line[Digits] != TEXT('_'))
	{
		return Line;
	}
	// Branch state memory of an inlined function: its name is cut to fit after the hash, so it shifts with the hash.
	static const FString BranchState(TEXT(" FRigVMInstructionSetExecuteState"));
	return Line.EndsWith(BranchState) ? TEXT("Hash#_#") + BranchState : TEXT("Hash#") + Line.RightChop(Digits);
}

FString WithoutReferenceHashes(const FString& Text)
{
	TArray<FString> Lines;
	Text.ParseIntoArray(Lines, TEXT("\n"));
	for (FString& Line : Lines)
	{
		Line = WithoutReferenceHash(Line);
	}
	return FString::Join(Lines, TEXT("\n"));
}

bool CompareFacts(FAutomationTestBase& Test, const FString& Stage, const FRigFacts& Expected, const FRigFacts& Got, bool bFromSource = false)
{
	// ComputeVMHash, and the VM by content (the functions it calls, its literal values, its work memory, as sorted
	// lists) so that a hash mismatch says what differs. The compiler visits events and unlinked nodes in the model's
	// node order, which the document carries as a graph's Order (its Nodes keys are sorted).
	// bWithoutNames: literal properties are shared by pins with equal values and named after the first such pin the
	// compiler visits, so literals compare as the set of their typed values.
	auto Sorted = [](const FString& Text, bool bWithoutNames = false)
	{
		TArray<FString> Lines;
		Text.ParseIntoArray(Lines, TEXT("\n"));
		for (FString& Line : Lines)
		{
			int32 Space = INDEX_NONE;
			// Branch state memory is named after a hash of the block's position in the instruction list.
			if ((bWithoutNames || Line.StartsWith(TEXT("BlockExecuteState_"))) && Line.FindChar(TEXT(' '), Space))
			{
				Line = (bWithoutNames ? FString() : FString(TEXT("BlockExecuteState_# "))) + Line.RightChop(Space + 1);
			}
			Line = WithoutReferenceHash(Line);
			// -0 and 0 are the same value (a document writes 0).
			Line.ReplaceInline(TEXT("=-0.000000"), TEXT("=0.000000"));
			Line.ReplaceInline(TEXT("= -0.000000"), TEXT("= 0.000000"));
		}
		Lines.Sort();
		if (bWithoutNames)
		{
			// Pins with equal literal values may share one literal, depending on the order the compiler visits them.
			Lines.SetNum(Algo::Unique(Lines));
		}
		return FString::Join(Lines, TEXT("\n"));
	};

	// The runtime pose.
	const bool bSamePoseCount = Got.Pose.Num() == Expected.Pose.Num();
	int32 Differing = 0;
	FString First;
	for (int32 Index = 0; bSamePoseCount && Index < Expected.Pose.Num(); ++Index)
	{
		const FRigElementPose& Want = Expected.Pose[Index];
		const FRigElementPose& Have = Got.Pose[Index];
		bool bEqual = Want.Key == Have.Key && Want.Values.Num() == Have.Values.Num();
		for (int32 Value = 0; bEqual && Value < Want.Values.Num(); ++Value)
		{
			bEqual = FMath::IsNearlyEqual(Want.Values[Value], Have.Values[Value], 0.01);
		}
		if (!bEqual && Differing++ == 0)
		{
			First = FString::Printf(TEXT("expected %s, got %s (initial translation in the asset hierarchy: expected side %s, other %s)"),
				*PoseLine(Want), *PoseLine(Have), *Want.Initial, *Have.Initial);
		}
	}
	const bool bPoseEqual = bSamePoseCount && Differing == 0;

	// Pin texts: the same value in another spelling, or another value.
	UE::AssetDocument::Graph::Rig::ControlRig::FTextValues Values;
	int32 Spellings = 0;
	TArray<FString> ValueDifferences;
	TArray<FString> SpellingExamples;
	for (const TPair<FString, FPinText>& Pin : Expected.PinDefaults)
	{
		const FPinText* Other = Got.PinDefaults.Find(Pin.Key);
		if (Other == nullptr || Other->Text.Equals(Pin.Value.Text, ESearchCase::CaseSensitive))
		{
			continue;
		}
		const FString Line = FString::Printf(TEXT("%s: '%s' vs '%s'"), *Pin.Key, *Pin.Value.Text.Left(200), *Other->Text.Left(200));
		// A name pin reads '' and 'None' as the same NAME_None.
		const bool bNoneName = Pin.Value.CPPType == TEXT("FName") && (Pin.Value.Text.IsEmpty() || Pin.Value.Text == TEXT("None"))
			&& (Other->Text.IsEmpty() || Other->Text == TEXT("None"));
		if (bNoneName || Values.Same(Pin.Value.CPPType, Pin.Value.TypeObject.Get(), Pin.Value.Text, Other->Text))
		{
			++Spellings;
			if (SpellingExamples.Num() < 3)
			{
				SpellingExamples.Add(Line);
			}
		}
		else
		{
			ValueDifferences.Add(Line);
		}
	}

	const TCHAR* Parts[] = {TEXT("functions"), TEXT("literal values"), TEXT("work memory")};
	const FString ExpectedParts[] = {Sorted(Expected.Functions), Sorted(Expected.Literals, true), Sorted(Expected.Work)};
	const FString GotParts[] = {Sorted(Got.Functions), Sorted(Got.Literals, true), Sorted(Got.Work)};
	// A document number has no negative zero ("-0" reads as 0): a VM that differs from the source only by the sign of a
	// zero literal is the same VM.
	const bool bSignOfZero = Got.VMHash != Expected.VMHash && Got.Functions == Expected.Functions && Got.ByteCodeHash == Expected.ByteCodeHash
		&& Got.Work == Expected.Work && Got.Literals.Replace(TEXT("-0.000000"), TEXT("0.000000")) == Expected.Literals.Replace(TEXT("-0.000000"), TEXT("0.000000"));
	const bool bReferenceHashes = Got.VMHash != Expected.VMHash && !bSignOfZero && Got.Functions == Expected.Functions
		&& Got.ByteCodeHash == Expected.ByteCodeHash && Got.Instructions == Expected.Instructions
		&& WithoutReferenceHashes(Got.Work) != Got.Work && GotParts[1] == ExpectedParts[1] && GotParts[2] == ExpectedParts[2]
		&& bPoseEqual && ValueDifferences.IsEmpty();
	const bool bSpelling = bFromSource && Got.VMHash != Expected.VMHash && !bSignOfZero && !bReferenceHashes && ValueDifferences.IsEmpty() && bPoseEqual
		&& Got.Instructions == Expected.Instructions && GotParts[0] == ExpectedParts[0]
		&& (Got.Functions == Expected.Functions || Expected.Pose.Num() > 0);
	if (bSignOfZero)
	{
		Test.AddInfo(FString::Printf(TEXT("%s: ComputeVMHash %u vs %u: equal VM but for the sign of zero literals (a document writes -0 as 0)"),
			*Stage, Expected.VMHash, Got.VMHash));
	}
	else if (bReferenceHashes)
	{
		Test.AddInfo(FString::Printf(TEXT("%s: ComputeVMHash %u vs %u: equal VM but for the work memory names of inlined function "
			"references, which hash the reference node's path in its package"), *Stage, Expected.VMHash, Got.VMHash));
	}
	else if (bSpelling)
	{
		Test.AddInfo(FString::Printf(TEXT("%s: ComputeVMHash %u vs %u: the same program (%d instructions, functions %s, %s pose) whose "
			"literals and memory differ by source spellings no editor action makes: %d pin texts of equal value%s%s; literal values %s, "
			"work memory %s."), *Stage, Expected.VMHash, Got.VMHash, Got.Instructions,
			Got.Functions == Expected.Functions ? TEXT("in the same order") : TEXT("in another order"),
			bPoseEqual && Expected.Pose.Num() > 0 ? TEXT("equal") : TEXT("no"), Spellings,
			SpellingExamples.IsEmpty() ? TEXT("") : TEXT(" such as "), *FString::Join(SpellingExamples, TEXT("; ")),
			GotParts[1] == ExpectedParts[1] ? TEXT("equal as a set") : TEXT("differ as a set"),
			GotParts[2] == ExpectedParts[2] ? TEXT("equal as a set") : TEXT("differs in node names")));
	}
	else
	{
		for (int32 Part = 0; Part < UE_ARRAY_COUNT(Parts); ++Part)
		{
			if (!Test.TestEqual(*FString::Printf(TEXT("%s: VM %s (as a set)"), *Stage, Parts[Part]), GotParts[Part], ExpectedParts[Part]))
			{
				Test.AddInfo(FString::Printf(TEXT("%s: VM %s: %s"), *Stage, Parts[Part], *FirstDifference(ExpectedParts[Part], GotParts[Part])));
			}
		}
		if (!Test.TestEqual(*FString::Printf(TEXT("%s: ComputeVMHash"), *Stage), Got.VMHash, Expected.VMHash))
		{
			for (const FString& Line : ValueDifferences)
			{
				Test.AddInfo(FString::Printf(TEXT("%s: pin value %s"), *Stage, *Line));
			}
			Test.AddInfo(FString::Printf(TEXT("%s: %d pin texts of equal value differ%s%s"), *Stage, Spellings,
				SpellingExamples.IsEmpty() ? TEXT("") : TEXT(", such as "), *FString::Join(SpellingExamples, TEXT("; "))));
			Test.AddInfo(FString::Printf(TEXT("%s: ComputeVMHash %u vs %u (order: functions %s, byte code %s, literal memory %s, work memory %s)"),
				*Stage, Expected.VMHash, Got.VMHash, Got.Functions == Expected.Functions ? TEXT("same order") : TEXT("other order"),
				Got.ByteCodeHash == Expected.ByteCodeHash ? TEXT("equal") : TEXT("differs"),
				Got.Literals == Expected.Literals ? TEXT("same order") : TEXT("other order"), Got.Work == Expected.Work ? TEXT("same order") : TEXT("other order")));
			const TCHAR* Lists[] = {TEXT("functions"), TEXT("literal memory"), TEXT("work memory")};
			const FString* ExpectedLists[] = {&Expected.Functions, &Expected.Literals, &Expected.Work};
			const FString* GotLists[] = {&Got.Functions, &Got.Literals, &Got.Work};
			for (int32 List = 0; List < UE_ARRAY_COUNT(Lists); ++List)
			{
				if (*ExpectedLists[List] == *GotLists[List])
				{
					continue;
				}
				TArray<FString> ExpectedEntries;
				TArray<FString> GotEntries;
				ExpectedLists[List]->ParseIntoArray(ExpectedEntries, TEXT("\n"));
				GotLists[List]->ParseIntoArray(GotEntries, TEXT("\n"));
				Test.AddInfo(FString::Printf(TEXT("%s: %s in order (%d vs %d entries) %s"), *Stage, Lists[List], ExpectedEntries.Num(),
					GotEntries.Num(), *FirstDifference(*ExpectedLists[List], *GotLists[List]).Replace(TEXT("\n"), TEXT(" | "))));
				const TSet<FString> ExpectedSet(ExpectedEntries);
				const TSet<FString> GotSet(GotEntries);
				Test.AddInfo(FString::Printf(TEXT("%s: %s only expected: %s; only got: %s"), *Stage, Lists[List],
					*FString::Join(ExpectedSet.Difference(GotSet).Array(), TEXT(" | ")).Left(1500),
					*FString::Join(GotSet.Difference(ExpectedSet).Array(), TEXT(" | ")).Left(1500)));
			}
		}
	}
	Test.TestEqual(*FString::Printf(TEXT("%s: VM instructions"), *Stage), Got.Instructions, Expected.Instructions);
	if (!Test.TestEqual(*FString::Printf(TEXT("%s: posed elements"), *Stage), Got.Pose.Num(), Expected.Pose.Num()))
	{
		TSet<FString> ExpectedKeys;
		TSet<FString> GotKeys;
		for (const FRigElementPose& Element : Expected.Pose) { ExpectedKeys.Add(Element.Key); }
		for (const FRigElementPose& Element : Got.Pose) { GotKeys.Add(Element.Key); }
		Test.AddInfo(FString::Printf(TEXT("%s: only expected: %s; only got: %s"), *Stage,
			*FString::Join(ExpectedKeys.Difference(GotKeys).Array(), TEXT(", ")).Left(600), *FString::Join(GotKeys.Difference(ExpectedKeys).Array(), TEXT(", ")).Left(600)));
		return false;
	}
	if (!Test.TestEqual(*FString::Printf(TEXT("%s: runtime pose (%d elements) differing elements"), *Stage, Expected.Pose.Num()), Differing, 0))
	{
		Test.AddInfo(FString::Printf(TEXT("%s: first differing element: %s"), *Stage, *First));
		return false;
	}
	return true;
}

// CR_AllKinds without its function content (the function reference, the collapse node, the local function), made
// with the controllers the editor uses, compiled and saved.
UControlRigBlueprint* MakeAllKindsWithoutFunctions(FAutomationTestBase& Test, const FTarget& Target)
{
	UControlRigBlueprint* Blueprint = AssetDocumentControlRigTests::MakeAllKinds(Test, Target);
	if (Blueprint == nullptr)
	{
		return nullptr;
	}
	URigVMBlueprint& RigVM = *Blueprint;
	URigVMController* Rig = RigVM.GetOrCreateController(RigVM.GetDefaultModel());
	Test.TestTrue(TEXT("remove the function reference"), Rig->RemoveNodeByName(TEXT("CallScale"), false, false));
	Test.TestTrue(TEXT("remove the collapse node"), Rig->RemoveNodeByName(TEXT("Collapsed"), false, false));
	URigVMFunctionLibrary* Library = RigVM.GetLocalFunctionLibrary();
	URigVMController* LibraryController = Library != nullptr ? RigVM.GetOrCreateController(Library) : nullptr;
	Test.TestTrue(TEXT("remove the function"), LibraryController != nullptr
		&& LibraryController->RemoveFunctionFromLibrary(TEXT("Scale Weight"), false, false));
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	Test.TestTrue(TEXT("CR_AllKinds minus functions saved"), Save(*Blueprint));
	return Blueprint;
}

// The import source and the first imported bone of a document (for a pose mismatch report).
FString DescribeImport(const TSharedPtr<FJsonObject>& Document)
{
	FString Source = TEXT("-");
	if (const TSharedPtr<FJsonObject> Properties = ObjectAt(Document, TEXT("Properties")))
	{
		Properties->TryGetStringField(TEXT("SourceHierarchyImport"), Source);
	}
	FString Bone = TEXT("-");
	const TArray<TSharedPtr<FJsonValue>>* Hierarchy = nullptr;
	if (BodyOf(Document).IsValid() && BodyOf(Document)->TryGetArrayField(TEXT("Hierarchy"), Hierarchy))
	{
		for (const TSharedPtr<FJsonValue>& Element : *Hierarchy)
		{
			const TSharedPtr<FJsonObject> Entry = Element->AsObject();
			if (Entry.IsValid() && Entry->HasField(TEXT("Imported")) && Entry->GetStringField(TEXT("Name")) != TEXT("root"))
			{
				Bone = Serialize(Entry);
				break;
			}
		}
	}
	return FString::Printf(TEXT("SourceHierarchyImport %s, first imported bone %s"), *Source, *Bone);
}

// Nodes of a graph and its collapse graphs.
int32 CountNodes(const TSharedPtr<FJsonObject>& Container)
{
	int32 Count = 0;
	if (const TSharedPtr<FJsonObject> Nodes = ObjectAt(Container, TEXT("Nodes")))
	{
		for (const auto& Node : Nodes->Values)
		{
			Count += 1 + CountNodes(ObjectAt(Node.Value->AsObject(), TEXT("Collapse")));
		}
	}
	return Count;
}

// Nodes of every graph and function body.
int32 NodeCount(const TSharedPtr<FJsonObject>& Document)
{
	int32 Count = 0;
	for (const TCHAR* Section : {TEXT("Graphs"), TEXT("Functions")})
	{
		if (const TSharedPtr<FJsonObject> Containers = ObjectAt(BodyOf(Document), Section))
		{
			for (const auto& Container : Containers->Values)
			{
				Count += CountNodes(Container.Value->AsObject());
			}
		}
	}
	return Count;
}

URigVMUnitNode* FindEvent(URigVMGraph& Graph, const UScriptStruct* Struct)
{
	for (URigVMNode* Node : Graph.GetNodes())
	{
		URigVMUnitNode* Unit = Cast<URigVMUnitNode>(Node);
		if (Unit != nullptr && Unit->GetScriptStruct() == Struct)
		{
			return Unit;
		}
	}
	return nullptr;
}

// A user event as the palette adds it: the event name is a transient (details-only) property, so it is set on the
// defaults struct rather than imported from text.
URigVMUnitNode* AddUserEvent(URigVMController& Controller, UScriptStruct* Struct, const TCHAR* EventName, const FVector2D& Position,
	const TCHAR* NodeName)
{
	FStructOnScope Defaults(Struct);
	if (const FNameProperty* Property = FindFProperty<FNameProperty>(Struct, TEXT("EventName")))
	{
		Property->SetPropertyValue_InContainer(Defaults.GetStructMemory(), FName(EventName));
	}
	return Controller.AddUnitNodeWithDefaults(Struct, FRigStructScope(Defaults), TEXT("Execute"), Position, NodeName, false, false);
}

UScriptStruct* UnitStruct(const TCHAR* Path)
{
	return FindObject<UScriptStruct>(nullptr, Path);
}

// A unit marked Deprecated that the palette no longer offers (any; null when the build has none).
UScriptStruct* FindDeprecatedUnit()
{
	for (TObjectIterator<UScriptStruct> It; It; ++It)
	{
		if (It->IsChildOf(FRigUnit::StaticStruct()) && It->HasMetaData(FRigVMStruct::DeprecatedMetaName) && !It->HasMetaData(TEXT("Abstract"))
			&& !It->GetName().StartsWith(TEXT("RigUnit_Prototype")) && It->GetCppStructOps() != nullptr)
		{
			return *It;
		}
	}
	return nullptr;
}
} // namespace AssetDocumentControlRigGraphTests

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigGraphFidelityTest, "UEAssetDoc.ControlRig.GraphFidelity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentControlRigGraphFidelityTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentControlRigGraphTests;
	const TSharedPtr<AssetDocumentControlRigTests::FTemplateContent> Templates = AssetDocumentControlRigTests::ShareCorpusTemplates();
	// MoverExamples' CR_Mannequin_Body logs these when it compiles (stale pins on Setup / Forward Fingers,
	// unmapped variables on Forward Spine): its own content, compiled here as the source of a comparison.
	if (FPackageName::DoesPackageExist(TEXT("/MoverExamples/Characters/Mannequins/Rigs/CR_Mannequin_Body")))
	{
		AssetDocumentIKRigTests::ExpectStalePinErrors(*this, 0);
		AssetDocumentIKRigTests::ExpectUnmappedVariableErrors(*this, 0);
	}
	FFixtures Fixtures;
	TArray<TPair<FString, const FTarget*>> Sources;
	const FTarget& AllKinds = Fixtures.Add(TEXT("CR_AllKinds"));
	if (UControlRigBlueprint* Made = AssetDocumentControlRigTests::MakeAllKinds(*this, AllKinds))
	{
		FKismetEditorUtilities::CompileBlueprint(Made);
		if (TestTrue(TEXT("CR_AllKinds saved"), Save(*Made)))
		{
			Sources.Emplace(TEXT("fixture CR_AllKinds"), &AllKinds);
		}
	}
	TMap<const FTarget*, FString> PathOf;
	TArray<FString> Paths;
	AssetDocumentControlRigTests::ForEachCorpusEntry([&Paths](const TCHAR* Path, const TCHAR* Why)
	{
		if (!FString(Why).StartsWith(TEXT("#10")) && !FString(Why).Contains(TEXT("shape library")))
		{
			Paths.Add(Path);
		}
	});
	for (const FString& Path : Paths)
	{
		UObject* Source = LoadQuiet(Path);
		if (Source == nullptr)
		{
			AddInfo(FString::Printf(TEXT("SKIP %s: not available in this project (template or plugin content)"), *Path));
			continue;
		}
		if (!Source->IsA<UControlRigBlueprint>())
		{
			AddInfo(FString::Printf(TEXT("SKIP %s: a %s, not a Control Rig"), *Path, *Source->GetClass()->GetName()));
			continue;
		}
		const FTarget& Copy = Fixtures.Add(TEXT("CR_Corpus"));
		if (Duplicate(*this, Source, Copy) != nullptr)
		{
			Sources.Emplace(FPackageName::ObjectPathToObjectName(Path), &Copy);
			PathOf.Add(&Copy, Path);
		}
	}
	// Sources holding what the 5.7 editor cannot make (content of older versions or of rig modules): Create refuses the
	// document as the editor would, and the rig as it is stays editable (its unchanged document applies as a no-op).
	const TPair<const TCHAR*, const TCHAR*> KnownRefusals[] = {
		{TEXT("/ControlRig/StandardFunctionLibrary/StandardFunctionLibrary.StandardFunctionLibrary"), TEXT("connector units such as Get Candidates")},
		{TEXT("/ControlRigSpline/SplineFunctionLibrary/SplineFunctionLibrary.SplineFunctionLibrary"), TEXT("local variables to functions only")},
		{TEXT("/ControlRigModules/Modules56/Leg.Leg"), TEXT("one execute pin")},
		{TEXT("/MoverExamples/Characters/Mannequins/Rigs/CR_Mannequin_Body.CR_Mannequin_Body"), TEXT("is private")},
	};
	int32 Compared = 0;
	int32 Refused = 0;
	TArray<FString> Skipped;
	for (const TPair<FString, const FTarget*>& Source : Sources)
	{
		const FString& Name = Source.Key;
		if (const UControlRigBlueprint* Rig = Cast<UControlRigBlueprint>(Source.Value->Get()); Rig != nullptr && Rig->IsModularRig())
		{
			Skipped.Add(Name);
			continue;
		}
		// The source as loaded, like the created rig it is compared with (an in-memory hierarchy can hold initial
		// global transforms that its locals and parent weights give otherwise after a load).
		if (!TestNotNull(*FString::Printf(TEXT("%s reloads"), *Name), Reload(*Source.Value)))
		{
			continue;
		}
		const FAssetDocumentResult Original = ExtractResult(Source.Value->ObjectPath);
		if (!TestTrue(*FString::Printf(TEXT("%s extracts"), *Name), Original.IsSuccess() && Original.Payload.IsValid()))
		{
			continue;
		}
		UControlRigBlueprint* SourceRig = Cast<UControlRigBlueprint>(Source.Value->Get());
		if (!TestNotNull(*FString::Printf(TEXT("%s is a Control Rig"), *Name), SourceRig))
		{
			continue;
		}
		// A preview mesh the source names but this project does not have (MoverExamples' CR_Mannequin_Body names the
		// template mannequin) cannot be set on the copy: both sides are compared without it.
		const TSharedPtr<FJsonObject> Properties = ObjectAt(Original.Payload, TEXT("Properties"));
		FString PreviewMesh;
		if (Properties.IsValid() && Properties->TryGetStringField(TEXT("PreviewSkeletalMesh"), PreviewMesh) && !PreviewMesh.IsEmpty() && PreviewMesh != TEXT("None") && LoadQuiet(PreviewMesh) == nullptr)
		{
			Properties->RemoveField(TEXT("PreviewSkeletalMesh"));
			AddInfo(FString::Printf(TEXT("%s: preview mesh %s is not in this project; compared without it"), *Name, *PreviewMesh));
		}
		FKismetEditorUtilities::CompileBlueprint(SourceRig);
		const FRigFacts Expected = FactsOf(*SourceRig);
		const FAssetDocumentResult Compiled = ExtractResult(Source.Value->ObjectPath);

		const FTarget& Created = Fixtures.Add(TEXT("CR_GraphCreated"));
		const FString* SourcePath = PathOf.Find(Source.Value);
		const TCHAR* Refusal = nullptr;
		for (const TPair<const TCHAR*, const TCHAR*>& Known : KnownRefusals)
		{
			if (SourcePath != nullptr && *SourcePath == Known.Key)
			{
				Refusal = Known.Value;
			}
		}
		if (Refusal != nullptr)
		{
			const FAssetDocumentResult NotMade = Apply(Created, ForTarget(Original.Payload, Created, TEXT("Create")));
			const FString Why = DiagnosticsText(NotMade);
			TestFalse(*FString::Printf(TEXT("%s: Create is refused"), *Name), NotMade.IsSuccess());
			TestTrue(*FString::Printf(TEXT("%s: refused as the editor would (ControlRig.EditRefused, '%s'): %s"), *Name, Refusal, *Why.Left(600)),
				Why.Contains(TEXT("ControlRig.EditRefused")) && Why.Contains(Refusal));
			const FAssetDocumentResult Unchanged = Apply(*Source.Value, ForTarget(Original.Payload, *Source.Value, TEXT("Update")));
			TestTrue(*FString::Printf(TEXT("%s: its unchanged document applies (%s)"), *Name, *DiagnosticsText(Unchanged).Left(600)), Unchanged.IsSuccess());
			TestFalse(*FString::Printf(TEXT("%s: its unchanged document is a no-op"), *Name), Unchanged.bSavedAsset);
			++Refused;
			AddInfo(FString::Printf(TEXT("KNOWN LIMIT %s: Create refused (%s); the rig stays editable"), *Name, *Why.Left(400)));
			continue;
		}
		double Start = FPlatformTime::Seconds();
		FAssetDocumentResult Applied;
		if (!ApplyOk(*this, Created, ForTarget(Original.Payload, Created, TEXT("Create")), Name + TEXT(" Create"), &Applied))
		{
			continue;
		}
		const double CreateMs = Milliseconds(Start);
		const FAssetDocumentResult Recreated = ExtractResult(Created.ObjectPath);
		const FString Want = WithoutTarget(Original.Payload);
		const FString Got = WithoutTarget(Recreated.Payload);
		if (!TestEqual(*FString::Printf(TEXT("%s: the created rig extracts as the source"), *Name), Got, Want))
		{
			AddInfo(FirstDifference(Want, Got));
		}
		TestTrue(*FString::Printf(TEXT("%s: the created rig compiles (%s)"), *Name, *MessagesOf(Recreated, Created.PackageName)), Compiles(Recreated));
		TestEqual(*FString::Printf(TEXT("%s: compile messages"), *Name), MessagesOf(Recreated, Created.PackageName),
			MessagesOf(Compiled, Source.Value->PackageName));
		UControlRigBlueprint* CreatedRig = Cast<UControlRigBlueprint>(Created.Get());
		// A rig module's preview connector targets (the hierarchy tab's connector resolves, ArrayConnectionMap) are
		// editor preview state a document does not carry: both sides run with the source's.
		if (CreatedRig != nullptr && !SourceRig->GetArrayConnectionMap().IsEmpty())
		{
			CreatedRig->GetArrayConnectionMap() = SourceRig->GetArrayConnectionMap();
			AddInfo(FString::Printf(TEXT("%s: compared with the source's %d preview connector targets"), *Name, SourceRig->GetArrayConnectionMap().Num()));
		}
		Start = FPlatformTime::Seconds();
		const FRigFacts Facts = CreatedRig != nullptr ? FactsOf(*CreatedRig) : FRigFacts();
		const double FactsMs = Milliseconds(Start);
		if (!CompareFacts(*this, Name, Expected, Facts, true))
		{
			AddInfo(FString::Printf(TEXT("%s: source %s; created %s"), *Name, *DescribeImport(Original.Payload), *DescribeImport(Recreated.Payload)));
		}

		Start = FPlatformTime::Seconds();
		const FAssetDocumentResult Again = Apply(Created, ForTarget(Recreated.Payload, Created, TEXT("Update")));
		const double ReapplyMs = Milliseconds(Start);
		TestTrue(*FString::Printf(TEXT("%s: unchanged re-apply succeeds (%s)"), *Name, *DiagnosticsText(Again)), Again.IsSuccess());
		TestFalse(*FString::Printf(TEXT("%s: unchanged re-apply is a no-op"), *Name), Again.bSavedAsset);
		Start = FPlatformTime::Seconds();
		TestNotNull(*FString::Printf(TEXT("%s: reload"), *Name), Reload(Created));
		const double ReloadMs = Milliseconds(Start);
		const FAssetDocumentResult Reloaded = ExtractResult(Created.ObjectPath);
		if (!TestEqual(*FString::Printf(TEXT("%s: equal after reload"), *Name), WithoutTarget(Reloaded.Payload), Got))
		{
			AddInfo(FirstDifference(Got, WithoutTarget(Reloaded.Payload)));
		}
		++Compared;
		AddInfo(FString::Printf(TEXT("PASS %s: %d nodes' doc %d chars; Create %.0f ms, VM hash %u / %d instructions, pose of %d elements "
			"(%.0f ms), unchanged re-apply %.0f ms, reload %.0f ms, status %s%s"), *Name, NodeCount(Original.Payload), Want.Len(),
			CreateMs, Facts.VMHash, Facts.Instructions, Facts.Pose.Num(), FactsMs, ReapplyMs, ReloadMs, *StatusName(Recreated),
			Applied.Diagnostics.IsEmpty() ? TEXT("") : *(TEXT("; apply warnings: ") + DiagnosticsText(Applied).Left(400))));
	}
	TestTrue(TEXT("at least CR_AllKinds compared"), Compared >= 1);
	AddInfo(FString::Printf(TEXT("%d rigs compared, %d refused as the editor would"), Compared, Refused));
	if (!Skipped.IsEmpty())
	{
		AddInfo(FString::Printf(TEXT("SKIP (modular rigs, tested separately): %s"), *FString::Join(Skipped, TEXT(", "))));
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigGraphFreeEditsTest, "UEAssetDoc.ControlRig.GraphFreeEdits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentControlRigGraphFreeEditsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentControlRigGraphTests;
	const TSharedPtr<AssetDocumentControlRigTests::FTemplateContent> Templates = AssetDocumentControlRigTests::ShareCorpusTemplates();
	FFixtures Fixtures;
	TArray<TPair<FString, UObject*>> Sources;
	const FTarget& AllKinds = Fixtures.Add(TEXT("CR_AllKindsNoFn"));
	if (UControlRigBlueprint* Made = MakeAllKindsWithoutFunctions(*this, AllKinds))
	{
		Sources.Emplace(TEXT("CR_AllKinds minus functions"), Made);
	}
	const TCHAR* const BodyRig = TEXT("/Game/Characters/Mannequins/Rigs/CR_Mannequin_Body.CR_Mannequin_Body");
	if (UObject* Body = LoadQuiet(BodyRig))
	{
		Sources.Emplace(TEXT("#7 CR_Mannequin_Body"), Body);
	}
	else
	{
		AddInfo(TEXT("SKIP #7: the mannequin template content is not available"));
	}
	UScriptStruct* SetTransform = UnitStruct(TEXT("/Script/ControlRig.RigUnit_SetTransform"));
	UScriptStruct* GetTransform = UnitStruct(TEXT("/Script/ControlRig.RigUnit_GetTransform"));
	UScriptStruct* Add = UnitStruct(TEXT("/Script/RigVM.RigVMFunction_MathFloatAdd"));
	UScriptStruct* Items = UnitStruct(TEXT("/Script/ControlRig.RigUnit_CollectionItems"));
	UScriptStruct* UserEvent = UnitStruct(TEXT("/Script/RigVM.RigVMFunction_UserDefinedEvent"));
	if (!TestTrue(TEXT("units load"), SetTransform && GetTransform && Add && Items && UserEvent))
	{
		return false;
	}
	for (const TPair<FString, UObject*>& Source : Sources)
	{
		const FString& Name = Source.Key;
		const FTarget& EditorSide = Fixtures.Add(TEXT("CR_GraphEditor"));
		const FTarget& DocumentSide = Fixtures.Add(TEXT("CR_GraphDocument"));
		if (Duplicate(*this, Source.Value, EditorSide) == nullptr || Duplicate(*this, Source.Value, DocumentSide) == nullptr)
		{
			continue;
		}
		UControlRigBlueprint* Edited = Cast<UControlRigBlueprint>(Reload(EditorSide));
		if (!TestNotNull(*FString::Printf(TEXT("%s reloads"), *Name), Edited) || Reload(DocumentSide) == nullptr)
		{
			continue;
		}
		URigVMBlueprint& RigVM = *Edited;
		URigVMGraph* Model = RigVM.GetDefaultModel();
		URigVMController* Rig = RigVM.GetOrCreateController(Model);
		URigVMUnitNode* Begin = FindEvent(*Model, FRigUnit_BeginExecution::StaticStruct());
		if (!TestNotNull(*FString::Printf(TEXT("%s has a Forwards Solve"), *Name), Begin))
		{
			continue;
		}
		const FVector2D Origin = Begin->GetPosition();
		const FString BeginExec = Begin->GetName() + TEXT(".ExecutePin");

		// The graph panel's edits (palette spawns, wire drags, pin widgets, Details, My Blueprint's "+").
		// 1. A Set Transform inserted after the Forwards Solve: the first wire is broken and re-routed through it.
		FString Next;
		if (const URigVMPin* ExecPin = Begin->FindPin(TEXT("ExecutePin")))
		{
			const auto Links = ExecPin->GetTargetLinks();
			if (!Links.IsEmpty())
			{
				Next = Links[0]->GetTargetPin()->GetPinPath();
			}
		}
		TestNotNull(TEXT("insert Set Transform"), Rig->AddUnitNode(SetTransform, TEXT("Execute"), Origin + FVector2D(250.0, 300.0), TEXT("AI_Set"), false, false));
		if (!Next.IsEmpty())
		{
			TestTrue(TEXT("break the first wire"), Rig->BreakLink(BeginExec, Next, false, false));
			TestTrue(TEXT("wire into Set Transform"), Rig->AddLink(TEXT("AI_Set.ExecutePin"), Next, false, false));
		}
		TestTrue(TEXT("wire Forwards Solve"), Rig->AddLink(BeginExec, TEXT("AI_Set.ExecutePin"), false, false));
		// 2. Pin values: an element key, an enum, a pin expanded.
		TestTrue(TEXT("element key value"), Rig->SetPinDefaultValue(TEXT("AI_Set.Item"), TEXT("(Type=Bone,Name=\"spine_01\")"), true, false, false, false));
		TestTrue(TEXT("enum value"), Rig->SetPinDefaultValue(TEXT("AI_Set.Space"), TEXT("LocalSpace"), true, false, false, false));
		TestTrue(TEXT("expand Value"), Rig->SetPinExpansion(TEXT("AI_Set.Value"), true, false, false));
		// 3. A sub-pin wire from a Get Transform.
		TestNotNull(TEXT("Get Transform"), Rig->AddUnitNode(GetTransform, TEXT("Execute"), Origin + FVector2D(0.0, 600.0), TEXT("AI_Get"), false, false));
		TestTrue(TEXT("Get item"), Rig->SetPinDefaultValue(TEXT("AI_Get.Item"), TEXT("(Type=Bone,Name=\"pelvis\")"), true, false, false, false));
		TestTrue(TEXT("sub-pin wire"), Rig->AddLink(TEXT("AI_Get.Transform.Rotation"), TEXT("AI_Set.Value.Rotation"), false, false));
		// 4. An Add with a third operand ("+" on the node) driving the weight.
		TestNotNull(TEXT("Add"), Rig->AddUnitNode(Add, TEXT("Execute"), Origin + FVector2D(0.0, 900.0), TEXT("AI_Sum"), false, false));
		// Value texts as the editor's widgets write them (SGraphPinNum: LexToString, "%f"); the "+" adds an operand
		// without a value.
		TestTrue(TEXT("Add value"), Rig->SetPinDefaultValue(TEXT("AI_Sum.A"), TEXT("0.500000"), true, false, false, false));
		TestFalse(TEXT("third operand"), Rig->AddAggregatePin(TEXT("AI_Sum"), FString(), FString(), false, false).IsEmpty());
		TestTrue(TEXT("third operand value"), Rig->SetPinDefaultValue(TEXT("AI_Sum.C"), TEXT("0.250000"), true, false, false, false));
		TestTrue(TEXT("Add -> Weight"), Rig->AddLink(TEXT("AI_Sum.Result"), TEXT("AI_Set.Weight"), false, false));
		// 5. An array pin resized and an element set.
		TestNotNull(TEXT("Items"), Rig->AddUnitNode(Items, TEXT("Execute"), Origin + FVector2D(0.0, 1200.0), TEXT("AI_Items"), false, false));
		TestTrue(TEXT("resize Items"), Rig->SetArrayPinSize(TEXT("AI_Items.Items"), 3, FString(), false, false));
		TestTrue(TEXT("Items element"), Rig->SetPinDefaultValue(TEXT("AI_Items.Items.1"), TEXT("(Type=Bone,Name=\"pelvis\")"), true, false, false, false));
		// 6. A user event and a Run Event of it.
		TestNotNull(TEXT("user event"), AddUserEvent(*Rig, UserEvent, TEXT("AI Event"), Origin + FVector2D(0.0, 1500.0), TEXT("AI_Event")));
		TestNotNull(TEXT("run event"), Rig->AddInvokeEntryNode(TEXT("AI Event"), Origin + FVector2D(400.0, 1500.0), TEXT("AI_Run"), false, false));
		// 7. A comment deleted, a node renamed and moved.
		for (URigVMNode* Node : Model->GetNodes())
		{
			if (Node->IsA<URigVMCommentNode>())
			{
				TestTrue(TEXT("delete a comment"), Rig->RemoveNodeByName(Node->GetFName(), false, false));
				break;
			}
		}
		FString RenamedFrom;
		FString RenamedTo;
		if (!Next.IsEmpty())
		{
			FString NodeName;
			Next.Split(TEXT("."), &NodeName, nullptr);
			RenamedFrom = NodeName;
			RenamedTo = NodeName + TEXT("_Moved");
			URigVMNode* Node = Model->FindNodeByName(*NodeName);
			TestTrue(TEXT("rename a node"), Node != nullptr && Rig->RenameNode(Node, *RenamedTo, false, false));
			TestTrue(TEXT("move it"), Rig->SetNodePositionByName(*RenamedTo, Origin + FVector2D(700.0, 50.0), false, false, false));
		}
		// 8. A bound pin (when the rig has a float member variable).
		for (const FBPVariableDescription& Variable : Edited->NewVariables)
		{
			if (Variable.VarType.PinCategory == TEXT("real") || Variable.VarType.PinCategory == TEXT("float"))
			{
				TestTrue(TEXT("bind an operand"), Rig->BindPinToVariable(TEXT("AI_Sum.B"), Variable.VarName.ToString(), false, false));
				break;
			}
		}
		// 9. A new event graph ("+" in My Blueprint) with a user event and a comment.
		if (URigVMGraph* Added = RigVM.AddModel(TEXT("AI Graph"), false, false))
		{
			URigVMController* Second = RigVM.GetOrCreateController(Added);
			TestNotNull(TEXT("event in the new graph"), AddUserEvent(*Second, UserEvent, TEXT("AI Graph Event"), FVector2D::ZeroVector, TEXT("AI_GraphEvent")));
			TestNotNull(TEXT("comment in the new graph"), Second->AddCommentNode(TEXT("Added by the editor"), FVector2D(-50.0, -80.0),
				FVector2D(300.0, 160.0), FLinearColor(0.1f, 0.6f, 0.3f), TEXT("AI_Comment"), false, false));
		}
		else
		{
			AddError(TEXT("new event graph"));
		}
		FKismetEditorUtilities::CompileBlueprint(Edited);
		TestTrue(TEXT("editor edits saved"), Save(*Edited));
		Edited = Cast<UControlRigBlueprint>(Reload(EditorSide));
		if (!TestNotNull(*FString::Printf(TEXT("%s: editor side reloads"), *Name), Edited))
		{
			continue;
		}
		const FRigFacts EditorFacts = FactsOf(*Edited);
		const FAssetDocumentResult EditorResult = ExtractResult(EditorSide.ObjectPath);
		if (!TestTrue(*FString::Printf(TEXT("%s: editor side extracts"), *Name), EditorResult.IsSuccess()))
		{
			continue;
		}

		// The same edit as a document: the editor side's state, the rename marked.
		const TSharedPtr<FJsonObject> Document = ForTarget(EditorResult.Payload, DocumentSide, TEXT("Update"));
		if (!RenamedTo.IsEmpty())
		{
			if (const TSharedPtr<FJsonObject> Renamed = ObjectAt(NodesOf(Document), *RenamedTo))
			{
				Renamed->SetStringField(TEXT("RenamedFrom"), RenamedFrom);
			}
		}
		const double Start = FPlatformTime::Seconds();
		FAssetDocumentResult Applied;
		if (!ApplyOk(*this, DocumentSide, Document, Name + TEXT(" graph document edit"), &Applied))
		{
			continue;
		}
		const double ApplyMs = Milliseconds(Start);
		const FAssetDocumentResult DocumentResult = ExtractResult(DocumentSide.ObjectPath);
		const FString Expected = WithoutTarget(EditorResult.Payload);
		const FString Got = WithoutTarget(DocumentResult.Payload);
		if (!TestEqual(*FString::Printf(TEXT("%s: document edit equals the editor's"), *Name), Got, Expected))
		{
			AddInfo(FirstDifference(Expected, Got));
		}
		TestEqual(*FString::Printf(TEXT("%s: equal compile status"), *Name), StatusName(DocumentResult), StatusName(EditorResult));
		TestEqual(*FString::Printf(TEXT("%s: equal compile messages"), *Name), MessagesOf(DocumentResult, DocumentSide.PackageName),
			MessagesOf(EditorResult, EditorSide.PackageName));
		const FAssetDocumentResult Again = Apply(DocumentSide, ForTarget(DocumentResult.Payload, DocumentSide, TEXT("Update")));
		TestFalse(*FString::Printf(TEXT("%s: re-apply is a no-op"), *Name), Again.bSavedAsset);
		UControlRigBlueprint* DocumentRig = Cast<UControlRigBlueprint>(Reload(DocumentSide));
		const FRigFacts DocumentFacts = DocumentRig != nullptr ? FactsOf(*DocumentRig) : FRigFacts();
		CompareFacts(*this, Name, EditorFacts, DocumentFacts);
		AddInfo(FString::Printf(TEXT("PASS %s: document edit %.0f ms, VM hash %u, pose of %d elements, status %s%s"), *Name, ApplyMs,
			DocumentFacts.VMHash, DocumentFacts.Pose.Num(), *StatusName(DocumentResult),
			Applied.Diagnostics.IsEmpty() ? TEXT("") : *(TEXT("; warnings: ") + DiagnosticsText(Applied).Left(400))));
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigGraphRollbackTest, "UEAssetDoc.ControlRig.GraphRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentControlRigGraphRollbackTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentControlRigGraphTests;
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("CR_GraphRollback"));
	if (MakeAllKindsWithoutFunctions(*this, Target) == nullptr)
	{
		return false;
	}
	const TSharedPtr<FJsonObject> State = Extract(*this, Target.ObjectPath, TEXT("before"));
	auto ExpectRefused = [this, &Target](const TSharedPtr<FJsonObject>& Document, const TCHAR* Code, const TCHAR* Stage)
	{
		const FString Before = Serialize(ExtractResult(Target.ObjectPath).Payload);
		const FAssetDocumentResult Result = Apply(Target, Document);
		TestFalse(*FString::Printf(TEXT("%s is refused"), Stage), Result.IsSuccess());
		TestTrue(*FString::Printf(TEXT("%s: %s (got %s)"), Stage, Code, *DiagnosticsText(Result)), HasCode(Result, Code));
		AddInfo(FString::Printf(TEXT("%s: %s"), Stage, *DiagnosticsText(Result)));
		const FString After = Serialize(ExtractResult(Target.ObjectPath).Payload);
		if (!TestEqual(*FString::Printf(TEXT("%s: the rig is unchanged"), Stage), After, Before))
		{
			AddInfo(FirstDifference(Before, After));
		}
		if (const UObject* Asset = Target.Get())
		{
			TestFalse(*FString::Printf(TEXT("%s: package clean"), Stage), Asset->GetOutermost()->IsDirty());
		}
	};

	// 1. Nodes added, values set, then a link the controller refuses (float -> element key): all rolled back.
	{
		const TSharedPtr<FJsonObject> Failing = ForTarget(State, Target, TEXT("Update"));
		const TSharedPtr<FJsonObject> Nodes = NodesOf(Failing);
		Nodes->SetObjectField(TEXT("Fine"), Json(R"J({"Unit": "RigVMFunction_MathFloatAdd", "Position": [0, 1400], "Defaults": {"A": 3.0}})J"));
		Nodes->SetObjectField(TEXT("Bad"), Json(R"J({"Unit": "RigUnit_SetTransform", "Position": [300, 1400], "Inputs": {"Item": "Fine.Result"}})J"));
		ExpectRefused(Failing, TEXT("ControlRig.LinkRefused"), TEXT("refused link"));
	}
	// 2. A new node of a deprecated unit: refused naming the replacement (the palette does not offer it).
	UScriptStruct* Deprecated = FindDeprecatedUnit();
	if (Deprecated != nullptr)
	{
		const TSharedPtr<FJsonObject> Failing = ForTarget(State, Target, TEXT("Update"));
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("Unit"), Deprecated->GetName());
		NodesOf(Failing)->SetObjectField(TEXT("Old"), Entry);
		ExpectRefused(Failing, TEXT("ControlRig.DeprecatedUnit"), *FString::Printf(TEXT("new deprecated unit %s"), *Deprecated->GetName()));
		// On Create (a round trip of a rig that has one) the node is kept with a warning.
		const FTarget& Created = Fixtures.Add(TEXT("CR_GraphDeprecated"));
		const TSharedPtr<FJsonObject> Copy = Json(R"J({"SchemaVersion": 1, "Action": "Create", "Body": {"Graphs": {"Rig": {"Nodes": {"Begin": {"Unit": "RigUnit_BeginExecution", "Position": [0, 0]}}}}}})J");
		Copy->SetStringField(TEXT("Class"), TEXT("/Script/ControlRigDeveloper.ControlRigBlueprint"));
		Copy->SetStringField(TEXT("Target"), Created.PackageName);
		NodesOf(Copy)->SetObjectField(TEXT("Old"), Entry);
		FAssetDocumentResult Applied;
		if (ApplyOk(*this, Created, Copy, TEXT("Create with a deprecated unit"), &Applied))
		{
			TestTrue(*FString::Printf(TEXT("deprecated unit warning (%s)"), *DiagnosticsText(Applied)), HasCode(Applied, TEXT("ControlRig.DeprecatedUnit")));
			TestTrue(TEXT("the deprecated node is kept"), ObjectAt(NodesOf(Extract(*this, Created.ObjectPath, TEXT("created"))), TEXT("Old")).IsValid());
		}
	}
	else
	{
		AddInfo(TEXT("No deprecated unit is loaded in this build; the deprecated-unit rule is not exercised."));
	}
	// 3. A function node of a function the rig does not have: refused (after the other nodes were added) and rolled back.
	{
		const TSharedPtr<FJsonObject> Failing = ForTarget(State, Target, TEXT("Update"));
		NodesOf(Failing)->SetObjectField(TEXT("Fine"), Json(R"J({"Unit": "RigVMFunction_MathFloatAdd", "Position": [0, 1500]})J"));
		NodesOf(Failing)->SetObjectField(TEXT("Call"), Json(R"J({"Function": "Missing", "Position": [0, 1600]})J"));
		ExpectRefused(Failing, TEXT("ControlRig.FunctionNotFound"), TEXT("function node"));
	}
	// 4. An unstable reroute (no linked input: the editor removes it on load).
	{
		const TSharedPtr<FJsonObject> Failing = ForTarget(State, Target, TEXT("Update"));
		NodesOf(Failing)->SetObjectField(TEXT("Loose"), Json(R"J({"Reroute": "float", "Position": [0, 1700]})J"));
		ExpectRefused(Failing, TEXT("ControlRig.UnstableReroute"), TEXT("unstable reroute"));
	}
	// 5. On the rig as loaded (editor graphs bound and caching their nodes): the editor nodes a rolled-back apply made
	// are freed by the next garbage collection, and a later apply of a node of the same name must not reach them.
	if (TestNotNull(TEXT("reload"), Reload(Target)))
	{
		const TSharedPtr<FJsonObject> Loaded = Extract(*this, Target.ObjectPath, TEXT("loaded"));
		const TSharedPtr<FJsonObject> Failing = ForTarget(Loaded, Target, TEXT("Update"));
		NodesOf(Failing)->SetObjectField(TEXT("Fine"), Json(R"J({"Unit": "RigVMFunction_MathFloatAdd", "Position": [0, 1400], "Defaults": {"A": 3.0}})J"));
		NodesOf(Failing)->SetObjectField(TEXT("Bad"), Json(R"J({"Unit": "RigUnit_SetTransform", "Position": [300, 1400], "Inputs": {"Item": "Fine.Result"}})J"));
		ExpectRefused(Failing, TEXT("ControlRig.LinkRefused"), TEXT("refused link on the loaded rig"));
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		const TSharedPtr<FJsonObject> Adding = ForTarget(Loaded, Target, TEXT("Update"));
		NodesOf(Adding)->SetObjectField(TEXT("Fine"), Json(R"J({"Unit": "RigVMFunction_MathFloatAdd", "Position": [0, 1400], "Defaults": {"A": 3.0}})J"));
		if (ApplyOk(*this, Target, Adding, TEXT("the same node added after the rollback")))
		{
			// The node is added to the editor graph too (a stale cached editor node would stand in for it).
			UControlRigBlueprint* Rig = Cast<UControlRigBlueprint>(Target.Get());
			bool bShown = false;
			TArray<UEdGraph*> Graphs;
			if (Rig != nullptr)
			{
				Rig->GetAllGraphs(Graphs);
			}
			for (const UEdGraph* Graph : Graphs)
			{
				const URigVMEdGraph* RigGraph = Cast<URigVMEdGraph>(Graph);
				if (RigGraph == nullptr || RigGraph->GetModel() != static_cast<URigVMBlueprint*>(Rig)->GetDefaultModel())
				{
					continue;
				}
				for (const UEdGraphNode* Node : RigGraph->Nodes)
				{
					const URigVMEdGraphNode* RigNode = Cast<URigVMEdGraphNode>(Node);
					bShown |= RigNode != nullptr && RigNode->GetModelNodePath() == TEXT("Fine");
				}
			}
			TestTrue(TEXT("the editor graph shows the node added again"), bShown);
		}
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigGraphUndoTest, "UEAssetDoc.ControlRig.GraphUndo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentControlRigGraphUndoTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentControlRigGraphTests;
	if (GEditor == nullptr || GEditor->Trans == nullptr)
	{
		return false;
	}
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("CR_GraphUndo"));
	if (MakeAllKindsWithoutFunctions(*this, Target) == nullptr)
	{
		return false;
	}
	ReleaseUndoHistory();
	auto UndoRedoAll = [this, &Target](const TCHAR* Stage)
	{
		int32 Undone = 0;
		while (Undone < 16 && GEditor->Trans->CanUndo() && GEditor->UndoTransaction())
		{
			++Undone;
			TestTrue(*FString::Printf(TEXT("%s: whole after undo %d"), Stage, Undone), ExtractResult(Target.ObjectPath).IsSuccess());
		}
		int32 Redone = 0;
		while (Redone < Undone && GEditor->Trans->CanRedo() && GEditor->RedoTransaction())
		{
			++Redone;
		}
		TestEqual(*FString::Printf(TEXT("%s: every undone step redoes"), Stage), Redone, Undone);
	};

	// 1. A human graph edit (the graph panel's move, one transaction with controller undo), saved; then an AI graph apply:
	// the history referencing the rig is cleared, both edits are there, undo / redo keep it whole.
	UControlRigBlueprint* Rig = Cast<UControlRigBlueprint>(Target.Get());
	if (Rig == nullptr)
	{
		return false;
	}
	{
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentControlRigGraphTests", "HumanMove", "Move Node"));
		URigVMController* Controller = static_cast<URigVMBlueprint&>(*Rig).GetOrCreateController(static_cast<URigVMBlueprint&>(*Rig).GetDefaultModel());
		TestTrue(TEXT("human move"), Controller->SetNodePositionByName(TEXT("GetHead"), FVector2D(-120.0, 260.0), true, false, false));
	}
	TestTrue(TEXT("human save"), UEditorLoadingAndSavingUtils::SavePackages({Rig->GetOutermost()}, false));
	const TSharedPtr<FJsonObject> HumanState = Extract(*this, Target.ObjectPath, TEXT("after human edit"));
	const TSharedPtr<FJsonObject> AiEdit = ForTarget(HumanState, Target, TEXT("Update"));
	NodesOf(AiEdit)->SetObjectField(TEXT("AiAdd"), Json(R"J({"Unit": "RigVMFunction_MathFloatAdd", "Position": [900, 600], "Defaults": {"B": 4.0}, "Inputs": {"A": "GetWeight.Value"}})J"));
	NodesOf(AiEdit)->RemoveField(TEXT("Comment"));
	if (!ApplyOk(*this, Target, AiEdit, TEXT("AI graph edit")))
	{
		return false;
	}
	TestFalse(TEXT("after AI edit: no undo history references the rig"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	const FString AfterEdit = Serialize(Extract(*this, Target.ObjectPath, TEXT("after AI edit")));
	{
		const TSharedPtr<FJsonObject> Head = ObjectAt(NodesOf(Parse(AfterEdit)), TEXT("GetHead"));
		const TArray<TSharedPtr<FJsonValue>>* Position = nullptr;
		const bool bKept = Head.IsValid() && Head->TryGetArrayField(TEXT("Position"), Position) && Position->Num() == 2
			&& FMath::IsNearlyEqual((*Position)[0]->AsNumber(), -120.0) && FMath::IsNearlyEqual((*Position)[1]->AsNumber(), 260.0);
		if (!TestTrue(TEXT("human edit kept"), bKept))
		{
			AddInfo(TEXT("GetHead after the AI edit: ") + (Head.IsValid() ? Serialize(Head) : FString(TEXT("missing"))));
		}
	}
	TestTrue(TEXT("AI edit made"), AfterEdit.Contains(TEXT("\"AiAdd\"")) && !AfterEdit.Contains(TEXT("\"All node kinds\"")));
	UndoRedoAll(TEXT("after AI graph edit"));
	TestEqual(TEXT("after AI graph edit + undo/redo"), Serialize(Extract(*this, Target.ObjectPath, TEXT("after undo/redo"))), AfterEdit);

	// 2. A failing AI graph apply (a refused link after a node add) rolls back to the saved state; undo stays safe.
	const TSharedPtr<FJsonObject> Failing = ForTarget(Parse(AfterEdit), Target, TEXT("Update"));
	NodesOf(Failing)->SetObjectField(TEXT("Fine"), Json(R"J({"Unit": "RigVMFunction_MathFloatAdd", "Position": [0, 1400]})J"));
	NodesOf(Failing)->SetObjectField(TEXT("Bad"), Json(R"J({"Unit": "RigUnit_SetTransform", "Position": [300, 1400], "Inputs": {"Item": "Fine.Result"}})J"));
	const FAssetDocumentResult Failed = Apply(Target, Failing);
	TestFalse(TEXT("failing graph apply fails"), Failed.IsSuccess());
	AddInfo(FString::Printf(TEXT("failing graph apply: %s"), *DiagnosticsText(Failed)));
	TestEqual(TEXT("after rollback: the saved state"), Serialize(Extract(*this, Target.ObjectPath, TEXT("after rollback"))), AfterEdit);
	UndoRedoAll(TEXT("after rollback"));
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

namespace AssetDocumentControlRigGraphTests
{
// Body/Functions/<Function>.
TSharedPtr<FJsonObject> FunctionOf(const TSharedPtr<FJsonObject>& Document, const TCHAR* Function)
{
	return ObjectAt(ObjectAt(BodyOf(Document), TEXT("Functions")), Function);
}

// The entry of the list field Field whose Name is Name.
TSharedPtr<FJsonObject> NamedEntry(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, const TCHAR* Name)
{
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (Object.IsValid() && Object->TryGetArrayField(Field, Items))
	{
		for (const TSharedPtr<FJsonValue>& Item : *Items)
		{
			const TSharedPtr<FJsonObject> Entry = Item->AsObject();
			if (Entry.IsValid() && Entry->GetStringField(TEXT("Name")) == Name)
			{
				return Entry;
			}
		}
	}
	return nullptr;
}

// The editor side of FunctionFreeEdits, first edit: My Blueprint's Functions "+" (the editor's New Function), the Details
// panel's "+" on Inputs / Outputs and Local Variables, the function graph's palette and wires; two function nodes
// dragged into the rig graph and wired into the Forwards Solve chain; two nodes collapsed (Collapse Nodes).
bool MakeFunctionEdits(FAutomationTestBase& Test, UControlRigBlueprint& Blueprint)
{
	URigVMBlueprint& RigVM = Blueprint;
	URigVMGraph* Model = RigVM.GetDefaultModel();
	URigVMController* Rig = RigVM.GetOrCreateController(Model);
	URigVMFunctionLibrary* LibraryGraph = RigVM.GetLocalFunctionLibrary();
	URigVMController* Library = LibraryGraph != nullptr ? RigVM.GetOrCreateController(LibraryGraph) : nullptr;
	URigVMUnitNode* Begin = FindEvent(*Model, FRigUnit_BeginExecution::StaticStruct());
	UScriptStruct* Add = UnitStruct(TEXT("/Script/RigVM.RigVMFunction_MathFloatAdd"));
	UScriptStruct* Mul = UnitStruct(TEXT("/Script/RigVM.RigVMFunction_MathFloatMul"));
	if (!Test.TestTrue(TEXT("rig graph, library, Forwards Solve and units"), Rig != nullptr && Library != nullptr && Begin != nullptr && Add != nullptr
		&& Mul != nullptr))
	{
		return false;
	}
	URigVMLibraryNode* Function = Library->AddFunctionToLibrary(TEXT("AI Blend"), true, FVector2D::ZeroVector, false, false);
	if (!Test.TestNotNull(TEXT("new function"), Function))
	{
		return false;
	}
	URigVMController* Body = RigVM.GetOrCreateController(Function->GetContainedGraph());
	Test.TestEqual(TEXT("input A"), Body->AddExposedPin(TEXT("A"), ERigVMPinDirection::Input, TEXT("float"), NAME_None, TEXT("1.000000"), false, false),
		FName(TEXT("A")));
	Test.TestEqual(TEXT("input B"), Body->AddExposedPin(TEXT("B"), ERigVMPinDirection::Input, TEXT("float"), NAME_None, FString(), false, false),
		FName(TEXT("B")));
	Test.TestEqual(TEXT("output"), Body->AddExposedPin(TEXT("Result"), ERigVMPinDirection::Output, TEXT("float"), NAME_None, FString(), false, false),
		FName(TEXT("Result")));
	Test.TestEqual(TEXT("local variable"), Body->AddLocalVariable(TEXT("Scale"), TEXT("float"), nullptr, TEXT("0.500000"), false, false).Name,
		FName(TEXT("Scale")));
	Test.TestNotNull(TEXT("Sum"), Body->AddUnitNode(Add, TEXT("Execute"), FVector2D(0.0, 0.0), TEXT("Sum"), false, false));
	Test.TestNotNull(TEXT("local getter"), Body->AddVariableNode(TEXT("Scale"), TEXT("float"), nullptr, true, FString(), FVector2D(0.0, 200.0),
		TEXT("GetScale"), false, false));
	Test.TestNotNull(TEXT("Times"), Body->AddUnitNode(Mul, TEXT("Execute"), FVector2D(250.0, 100.0), TEXT("Times"), false, false));
	for (const TPair<const TCHAR*, const TCHAR*>& Wire : TArray<TPair<const TCHAR*, const TCHAR*>>{{TEXT("Entry.A"), TEXT("Sum.A")},
		{TEXT("Entry.B"), TEXT("Sum.B")}, {TEXT("Sum.Result"), TEXT("Times.A")}, {TEXT("GetScale.Value"), TEXT("Times.B")},
		{TEXT("Times.Result"), TEXT("Return.Result")}})
	{
		Test.TestTrue(*FString::Printf(TEXT("function wire %s -> %s"), Wire.Key, Wire.Value), Body->AddLink(Wire.Key, Wire.Value, false, false));
	}
	TArray<FString> Following;
	if (const URigVMPin* ExecPin = Begin->FindPin(TEXT("ExecutePin")))
	{
		for (const URigVMLink* Link : ExecPin->GetTargetLinks())
		{
			Following.Add(Link->GetTargetPin()->GetPinPath());
		}
	}
	const FVector2D Origin = Begin->GetPosition();
	const FString BeginExec = Begin->GetName() + TEXT(".ExecutePin");
	Test.TestNotNull(TEXT("first call"), Rig->AddFunctionReferenceNode(Function, Origin + FVector2D(250.0, 400.0), TEXT("AI_Call1"), false, false));
	Test.TestNotNull(TEXT("second call"), Rig->AddFunctionReferenceNode(Function, Origin + FVector2D(550.0, 400.0), TEXT("AI_Call2"), false, false));
	for (const FString& Next : Following)
	{
		Test.TestTrue(TEXT("break the first wire"), Rig->BreakLink(BeginExec, Next, false, false));
		Test.TestTrue(TEXT("wire after the calls"), Rig->AddLink(TEXT("AI_Call2.ExecuteContext"), Next, false, false));
	}
	Test.TestTrue(TEXT("wire the first call"), Rig->AddLink(BeginExec, TEXT("AI_Call1.ExecuteContext"), false, false));
	Test.TestTrue(TEXT("wire the second call"), Rig->AddLink(TEXT("AI_Call1.ExecuteContext"), TEXT("AI_Call2.ExecuteContext"), false, false));
	Test.TestTrue(TEXT("call value"), Rig->SetPinDefaultValue(TEXT("AI_Call1.B"), TEXT("2.000000"), true, false, false, false));
	Test.TestTrue(TEXT("call to call"), Rig->AddLink(TEXT("AI_Call1.Result"), TEXT("AI_Call2.A"), false, false));
	// Two nodes collapsed into a collapse node feeding the second call.
	Test.TestNotNull(TEXT("AI_X"), Rig->AddUnitNode(Add, TEXT("Execute"), Origin + FVector2D(250.0, 700.0), TEXT("AI_X"), false, false));
	Test.TestNotNull(TEXT("AI_Y"), Rig->AddUnitNode(Mul, TEXT("Execute"), Origin + FVector2D(500.0, 700.0), TEXT("AI_Y"), false, false));
	Test.TestTrue(TEXT("AI_X value"), Rig->SetPinDefaultValue(TEXT("AI_X.A"), TEXT("1.500000"), true, false, false, false));
	Test.TestTrue(TEXT("AI_X -> AI_Y"), Rig->AddLink(TEXT("AI_X.Result"), TEXT("AI_Y.A"), false, false));
	Test.TestTrue(TEXT("AI_Y -> second call"), Rig->AddLink(TEXT("AI_Y.Result"), TEXT("AI_Call2.B"), false, false));
	return Test.TestNotNull(TEXT("collapse"), Rig->CollapseNodes(TArray<FName>{FName(TEXT("AI_X")), FName(TEXT("AI_Y"))}, TEXT("AI_Collapsed"), false,
		false));
}

// Second edit: the function renamed (My Blueprint), made public and its input default changed (the function Details), its
// local variable renamed (My Blueprint's Local Variables).
bool MakeFunctionChanges(FAutomationTestBase& Test, UControlRigBlueprint& Blueprint)
{
	URigVMBlueprint& RigVM = Blueprint;
	URigVMController* Library = RigVM.GetOrCreateController(RigVM.GetLocalFunctionLibrary());
	Test.TestTrue(TEXT("rename the function"), Library->RenameFunction(TEXT("AI Blend"), TEXT("AI Blend Renamed"), false, false));
	RigVM.MarkFunctionPublic(TEXT("AI Blend Renamed"), true);
	Test.TestTrue(TEXT("input default"), Library->SetPinDefaultValue(TEXT("AI Blend Renamed.A"), TEXT("3.000000"), true, false, false, false));
	URigVMLibraryNode* Function = RigVM.GetLocalFunctionLibrary()->FindFunction(TEXT("AI Blend Renamed"));
	URigVMController* Body = Function != nullptr ? RigVM.GetOrCreateController(Function->GetContainedGraph()) : nullptr;
	return Test.TestTrue(TEXT("rename the local variable"), Body != nullptr && Body->RenameLocalVariable(TEXT("Scale"), TEXT("Gain"), false, false));
}
} // namespace AssetDocumentControlRigGraphTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigFunctionFreeEditsTest, "UEAssetDoc.ControlRig.FunctionFreeEdits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentControlRigFunctionFreeEditsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentControlRigGraphTests;
	const TSharedPtr<AssetDocumentControlRigTests::FTemplateContent> Templates = AssetDocumentControlRigTests::ShareCorpusTemplates();
	FFixtures Fixtures;
	TArray<TPair<FString, UObject*>> Sources;
	const FTarget& AllKinds = Fixtures.Add(TEXT("CR_AllKindsFn"));
	if (UControlRigBlueprint* Made = AssetDocumentControlRigTests::MakeAllKinds(*this, AllKinds))
	{
		FKismetEditorUtilities::CompileBlueprint(Made);
		if (TestTrue(TEXT("CR_AllKinds saved"), Save(*Made)))
		{
			Sources.Emplace(TEXT("CR_AllKinds"), Made);
		}
	}
	if (UObject* Body = LoadQuiet(TEXT("/Game/Characters/Mannequins/Rigs/CR_Mannequin_Body.CR_Mannequin_Body")))
	{
		Sources.Emplace(TEXT("#7 CR_Mannequin_Body"), Body);
	}
	else
	{
		AddInfo(TEXT("SKIP #7: the mannequin template content is not available"));
	}
	for (const TPair<FString, UObject*>& Source : Sources)
	{
		const FString& Name = Source.Key;
		const FTarget& EditorSide = Fixtures.Add(TEXT("CR_FnEditor"));
		const FTarget& DocumentSide = Fixtures.Add(TEXT("CR_FnDocument"));
		if (Duplicate(*this, Source.Value, EditorSide) == nullptr || Duplicate(*this, Source.Value, DocumentSide) == nullptr
			|| Reload(DocumentSide) == nullptr)
		{
			continue;
		}
		for (int32 Round = 1; Round <= 2; ++Round)
		{
			const FString Stage = FString::Printf(TEXT("%s round %d"), *Name, Round);
			UControlRigBlueprint* Edited = Cast<UControlRigBlueprint>(Reload(EditorSide));
			if (!TestNotNull(*FString::Printf(TEXT("%s: editor side reloads"), *Stage), Edited)
				|| !(Round == 1 ? MakeFunctionEdits(*this, *Edited) : MakeFunctionChanges(*this, *Edited)))
			{
				break;
			}
			FKismetEditorUtilities::CompileBlueprint(Edited);
			TestTrue(*FString::Printf(TEXT("%s: editor edits saved"), *Stage), Save(*Edited));
			Edited = Cast<UControlRigBlueprint>(Reload(EditorSide));
			if (!TestNotNull(*FString::Printf(TEXT("%s: editor side reloads after save"), *Stage), Edited))
			{
				break;
			}
			const FRigFacts EditorFacts = FactsOf(*Edited);
			const FAssetDocumentResult EditorResult = ExtractResult(EditorSide.ObjectPath);
			if (!TestTrue(*FString::Printf(TEXT("%s: editor side extracts"), *Stage), EditorResult.IsSuccess()))
			{
				break;
			}
			// The same edit as a document: the editor side's state, renames marked.
			const TSharedPtr<FJsonObject> Document = ForTarget(EditorResult.Payload, DocumentSide, TEXT("Update"));
			if (Round == 2)
			{
				const TSharedPtr<FJsonObject> Function = FunctionOf(Document, TEXT("AI Blend Renamed"));
				if (TestTrue(*FString::Printf(TEXT("%s: renamed function extracted"), *Stage), Function.IsValid()))
				{
					Function->SetStringField(TEXT("RenamedFrom"), TEXT("AI Blend"));
					if (const TSharedPtr<FJsonObject> Local = NamedEntry(Function, TEXT("LocalVariables"), TEXT("Gain")))
					{
						Local->SetStringField(TEXT("RenamedFrom"), TEXT("Scale"));
					}
				}
			}
			const double Start = FPlatformTime::Seconds();
			FAssetDocumentResult Applied;
			if (!ApplyOk(*this, DocumentSide, Document, Stage + TEXT(" function document edit"), &Applied))
			{
				break;
			}
			const double ApplyMs = Milliseconds(Start);
			const FAssetDocumentResult DocumentResult = ExtractResult(DocumentSide.ObjectPath);
			const FString Expected = WithoutTarget(EditorResult.Payload);
			const FString Got = WithoutTarget(DocumentResult.Payload);
			if (!TestEqual(*FString::Printf(TEXT("%s: document edit equals the editor's"), *Stage), Got, Expected))
			{
				AddInfo(FirstDifference(Expected, Got));
			}
			TestEqual(*FString::Printf(TEXT("%s: equal compile status"), *Stage), StatusName(DocumentResult), StatusName(EditorResult));
			TestEqual(*FString::Printf(TEXT("%s: equal compile messages"), *Stage), MessagesOf(DocumentResult, DocumentSide.PackageName),
				MessagesOf(EditorResult, EditorSide.PackageName));
			const FAssetDocumentResult Again = Apply(DocumentSide, ForTarget(DocumentResult.Payload, DocumentSide, TEXT("Update")));
			TestFalse(*FString::Printf(TEXT("%s: re-apply is a no-op"), *Stage), Again.bSavedAsset);
			UControlRigBlueprint* DocumentRig = Cast<UControlRigBlueprint>(Reload(DocumentSide));
			const FRigFacts DocumentFacts = DocumentRig != nullptr ? FactsOf(*DocumentRig) : FRigFacts();
			CompareFacts(*this, Stage, EditorFacts, DocumentFacts);
			AddInfo(FString::Printf(TEXT("PASS %s: document edit %.0f ms, VM hash %u / %d instructions, pose of %d elements, status %s%s"), *Stage,
				ApplyMs, DocumentFacts.VMHash, DocumentFacts.Instructions, DocumentFacts.Pose.Num(), *StatusName(DocumentResult),
				Applied.Diagnostics.IsEmpty() ? TEXT("") : *(TEXT("; warnings: ") + DiagnosticsText(Applied).Left(400))));
		}
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

namespace AssetDocumentControlRigGraphTests
{
// A rig made from a document (Create), saved.
bool CreateRig(FAutomationTestBase& Test, const FTarget& Target, const ANSICHAR* Body, const TCHAR* Stage)
{
	const TSharedPtr<FJsonObject> Document = Json(R"J({"SchemaVersion": 1, "Action": "Create", "Properties": {}})J");
	Document->SetStringField(TEXT("Class"), TEXT("/Script/ControlRigDeveloper.ControlRigBlueprint"));
	Document->SetStringField(TEXT("Target"), Target.PackageName);
	Document->SetObjectField(TEXT("Body"), Json(Body));
	return ApplyOk(Test, Target, Document, Stage);
}
} // namespace AssetDocumentControlRigGraphTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigFunctionRollbackTest, "UEAssetDoc.ControlRig.FunctionRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentControlRigFunctionRollbackTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentControlRigGraphTests;
	FFixtures Fixtures;
	const FTarget& Host = Fixtures.Add(TEXT("CR_FnHost"));
	const FTarget& Caller = Fixtures.Add(TEXT("CR_FnCaller"));
	// The host: a public function that reads a member variable (a caller in another rig remaps it).
	if (!CreateRig(*this, Host, R"J({"Variables": [{"Name": "Speed", "Type": "float", "Default": 2.0}],
		"Graphs": {"Rig": {"Nodes": {"Begin": {"Unit": "RigUnit_BeginExecution", "Position": [0, 0]}}}},
		"Functions": {"Read Speed": {"Public": true, "Category": "Motion", "Description": "The speed, doubled.",
			"Inputs": [{"Name": "ExecuteContext", "Type": "/Script/RigVM.RigVMExecuteContext", "Direction": "IO"}, {"Name": "Gain", "Type": "float", "Default": 2.0}],
			"Outputs": [{"Name": "Value", "Type": "float"}],
			"LocalVariables": [{"Name": "Bias", "Type": "float", "Default": 0.25}],
			"Nodes": {"Get": {"Variable": "Speed", "Position": [0, 100]}, "GetBias": {"Variable": "Bias", "Position": [0, 250]},
				"Times": {"Unit": "RigVMFunction_MathFloatMul", "Position": [200, 100], "Inputs": {"A": "Get.Value", "B": "Entry.Gain"}},
				"Plus": {"Unit": "RigVMFunction_MathFloatAdd", "Position": [400, 100], "Inputs": {"A": "Times.Result", "B": "GetBias.Value"}},
				"Box": {"Collapse": {"Inputs": [{"Name": "In", "Type": "float"}], "Outputs": [{"Name": "Out", "Type": "float"}],
					"Nodes": {"Neg": {"Unit": "RigVMFunction_MathFloatNegate", "Position": [0, 0], "Inputs": {"Value": "Entry.In"}}},
					"Return": {"Position": [300, 0], "Inputs": {"Out": "Neg.Result"}}}, "Position": [600, 100], "Inputs": {"In": "Plus.Result"}}},
			"Order": ["Get", "GetBias", "Times", "Plus", "Box"],
			"Return": {"Position": [800, 0], "Inputs": {"ExecuteContext": "Entry.ExecuteContext", "Value": "Box.Out"}}}}})J",
		TEXT("Create the host")))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> HostState = Extract(*this, Host.ObjectPath, TEXT("host"));
	const TSharedPtr<FJsonObject> Made = FunctionOf(HostState, TEXT("Read Speed"));
	TestTrue(TEXT("host: function made as written"), Made.IsValid() && Made->GetBoolField(TEXT("Public"))
		&& Made->GetStringField(TEXT("Category")) == TEXT("Motion") && NamedEntry(Made, TEXT("LocalVariables"), TEXT("Bias")).IsValid()
		&& ObjectAt(ObjectAt(ObjectAt(Made, TEXT("Nodes")), TEXT("Box")), TEXT("Collapse")).IsValid());
	const FString HostFunction = Host.ObjectPath + TEXT(":Read Speed");
	// The caller: the host's public function with its Speed variable remapped to the caller's Velocity.
	const FString CallerBody = FString(UTF8_TO_TCHAR(R"J({"Variables": [{"Name": "Velocity", "Type": "float", "Default": 3.0}, {"Name": "Other", "Type": "float"}],
		"Graphs": {"Rig": {"Nodes": {"Begin": {"Unit": "RigUnit_BeginExecution", "Position": [0, 0]},
			"Call": {"Function": "@HOST@", "Position": [300, 0], "VariableMap": {"Speed": "Velocity"}, "Inputs": {"ExecuteContext": "Begin.ExecutePin"}}}}}})J"))
		.Replace(TEXT("@HOST@"), *HostFunction);
	if (!CreateRig(*this, Caller, TCHAR_TO_UTF8(*CallerBody), TEXT("Create the caller")))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> CallerState = Extract(*this, Caller.ObjectPath, TEXT("caller"));
	const TSharedPtr<FJsonObject> Call = ObjectAt(NodesOf(CallerState), TEXT("Call"));
	TestTrue(TEXT("caller: the external function node and its variable map"), Call.IsValid() && Call->GetStringField(TEXT("Function")) == HostFunction
		&& ObjectAt(Call, TEXT("VariableMap")).IsValid() && ObjectAt(Call, TEXT("VariableMap"))->GetStringField(TEXT("Speed")) == TEXT("Velocity"));

	auto ExpectRolledBack = [this](const FTarget& Target, const TSharedPtr<FJsonObject>& Document, const TCHAR* Code, const TCHAR* Stage)
	{
		const FString Before = Serialize(ExtractResult(Target.ObjectPath).Payload);
		const FAssetDocumentResult Result = Apply(Target, Document);
		TestFalse(*FString::Printf(TEXT("%s is refused"), Stage), Result.IsSuccess());
		TestTrue(*FString::Printf(TEXT("%s: %s (got %s)"), Stage, Code, *DiagnosticsText(Result)), HasCode(Result, Code));
		const FString After = Serialize(ExtractResult(Target.ObjectPath).Payload);
		if (!TestEqual(*FString::Printf(TEXT("%s: the rig is unchanged"), Stage), After, Before))
		{
			AddInfo(FirstDifference(Before, After));
		}
		if (const UObject* Asset = Target.Get())
		{
			TestFalse(*FString::Printf(TEXT("%s: package clean"), Stage), Asset->GetOutermost()->IsDirty());
		}
	};
	// 1. Caller: the variable map changed, then a refused link: rolled back with the variable map restored.
	{
		const TSharedPtr<FJsonObject> Failing = ForTarget(CallerState, Caller, TEXT("Update"));
		ObjectAt(ObjectAt(NodesOf(Failing), TEXT("Call")), TEXT("VariableMap"))->SetStringField(TEXT("Speed"), TEXT("Other"));
		NodesOf(Failing)->SetObjectField(TEXT("Fine"), Json(R"J({"Unit": "RigVMFunction_MathFloatAdd", "Position": [0, 400]})J"));
		NodesOf(Failing)->SetObjectField(TEXT("Bad"), Json(R"J({"Unit": "RigUnit_SetTransform", "Position": [300, 400], "Inputs": {"Item": "Fine.Result"}})J"));
		ExpectRolledBack(Caller, Failing, TEXT("ControlRig.LinkRefused"), TEXT("caller: remap then a refused link"));
	}
	// 2. Host: a new function, a renamed pin, a new local, a collapse node's pin added, then a refused link inside the
	// function body: all rolled back.
	{
		const TSharedPtr<FJsonObject> Failing = ForTarget(HostState, Host, TEXT("Update"));
		ObjectAt(BodyOf(Failing), TEXT("Functions"))->SetObjectField(TEXT("Extra"), Json(R"J({"Inputs": [{"Name": "X", "Type": "int32"}], "Nodes": {}})J"));
		const TSharedPtr<FJsonObject> Function = FunctionOf(Failing, TEXT("Read Speed"));
		if (const TSharedPtr<FJsonObject> Gain = NamedEntry(Function, TEXT("Inputs"), TEXT("Gain")))
		{
			Gain->SetStringField(TEXT("Name"), TEXT("Factor"));
			Gain->SetStringField(TEXT("RenamedFrom"), TEXT("Gain"));
			ObjectAt(ObjectAt(Function, TEXT("Nodes")), TEXT("Times"))->GetObjectField(TEXT("Inputs"))->SetStringField(TEXT("B"), TEXT("Entry.Factor"));
		}
		TArray<TSharedPtr<FJsonValue>> Locals = Function->GetArrayField(TEXT("LocalVariables"));
		Locals.Add(MakeShared<FJsonValueObject>(Json(R"J({"Name": "Count", "Type": "int32", "Default": 3})J")));
		Function->SetArrayField(TEXT("LocalVariables"), Locals);
		const TSharedPtr<FJsonObject> Box = ObjectAt(ObjectAt(ObjectAt(Function, TEXT("Nodes")), TEXT("Box")), TEXT("Collapse"));
		TArray<TSharedPtr<FJsonValue>> BoxInputs = Box->GetArrayField(TEXT("Inputs"));
		BoxInputs.Add(MakeShared<FJsonValueObject>(Json(R"J({"Name": "Extra", "Type": "float"})J")));
		Box->SetArrayField(TEXT("Inputs"), BoxInputs);
		ObjectAt(Function, TEXT("Nodes"))->SetObjectField(TEXT("Bad"), Json(R"J({"Unit": "RigUnit_SetTransform", "Position": [300, 400], "Inputs": {"Item": "Plus.Result"}})J"));
		ExpectRolledBack(Host, Failing, TEXT("ControlRig.LinkRefused"), TEXT("host: function edits then a refused link"));
	}
	// 3. The public function renamed: the caller is reported (warn only), the host renamed.
	{
		const TSharedPtr<FJsonObject> Renaming = ForTarget(HostState, Host, TEXT("Update"));
		const TSharedPtr<FJsonObject> Functions = ObjectAt(BodyOf(Renaming), TEXT("Functions"));
		const TSharedPtr<FJsonObject> Function = FunctionOf(Renaming, TEXT("Read Speed"));
		Function->SetStringField(TEXT("RenamedFrom"), TEXT("Read Speed"));
		Functions->RemoveField(TEXT("Read Speed"));
		Functions->SetObjectField(TEXT("Read Velocity"), Function);
		FAssetDocumentResult Renamed;
		if (ApplyOk(*this, Host, Renaming, TEXT("rename the public function"), &Renamed))
		{
			TestTrue(*FString::Printf(TEXT("ControlRig.DependentsAffected names the caller (%s)"), *DiagnosticsText(Renamed)),
				HasCode(Renamed, TEXT("ControlRig.DependentsAffected")) && DiagnosticsText(Renamed).Contains(Caller.PackageName));
			TestTrue(TEXT("renamed"), FunctionOf(Extract(*this, Host.ObjectPath, TEXT("renamed")), TEXT("Read Velocity")).IsValid());
		}
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigFunctionUndoTest, "UEAssetDoc.ControlRig.FunctionUndo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentControlRigFunctionUndoTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentControlRigGraphTests;
	if (GEditor == nullptr || GEditor->Trans == nullptr)
	{
		return false;
	}
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("CR_FnUndo"));
	UControlRigBlueprint* Made = AssetDocumentControlRigTests::MakeAllKinds(*this, Target);
	if (Made == nullptr)
	{
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(Made);
	TestTrue(TEXT("CR_AllKinds saved"), Save(*Made));
	ReleaseUndoHistory();
	auto UndoRedoAll = [this, &Target](const TCHAR* Stage)
	{
		int32 Undone = 0;
		while (Undone < 16 && GEditor->Trans->CanUndo() && GEditor->UndoTransaction())
		{
			++Undone;
			TestTrue(*FString::Printf(TEXT("%s: whole after undo %d"), Stage, Undone), ExtractResult(Target.ObjectPath).IsSuccess());
		}
		int32 Redone = 0;
		while (Redone < Undone && GEditor->Trans->CanRedo() && GEditor->RedoTransaction())
		{
			++Redone;
		}
		TestEqual(*FString::Printf(TEXT("%s: every undone step redoes"), Stage), Redone, Undone);
	};
	// 1. A function apply: the function's pin renamed, a local added, a second call; undo / redo keep the rig whole.
	const TSharedPtr<FJsonObject> State = Extract(*this, Target.ObjectPath, TEXT("before"));
	const TSharedPtr<FJsonObject> Edit = ForTarget(State, Target, TEXT("Update"));
	const TSharedPtr<FJsonObject> Function = FunctionOf(Edit, TEXT("Scale Weight"));
	if (!TestTrue(TEXT("CR_AllKinds has Scale Weight"), Function.IsValid()))
	{
		return false;
	}
	if (const TSharedPtr<FJsonObject> Amount = NamedEntry(Function, TEXT("Inputs"), TEXT("Amount")))
	{
		Amount->SetStringField(TEXT("Name"), TEXT("Value"));
		Amount->SetStringField(TEXT("RenamedFrom"), TEXT("Amount"));
		ObjectAt(ObjectAt(Function, TEXT("Nodes")), TEXT("Times"))->GetObjectField(TEXT("Inputs"))->SetStringField(TEXT("A"), TEXT("Entry.Value"));
	}
	TArray<TSharedPtr<FJsonValue>> Locals = Function->GetArrayField(TEXT("LocalVariables"));
	Locals.Add(MakeShared<FJsonValueObject>(Json(R"J({"Name": "Offset", "Type": "float", "Default": 1.0})J")));
	Function->SetArrayField(TEXT("LocalVariables"), Locals);
	NodesOf(Edit)->SetObjectField(TEXT("CallAgain"), Json(R"J({"Function": "Scale Weight", "Position": [1100, 300], "Defaults": {"Value": 4.0}})J"));
	if (!ApplyOk(*this, Target, Edit, TEXT("function edit")))
	{
		return false;
	}
	TestFalse(TEXT("after the function edit: no undo history references the rig"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	const FString AfterEdit = Serialize(Extract(*this, Target.ObjectPath, TEXT("after function edit")));
	TestTrue(TEXT("function edit made"), AfterEdit.Contains(TEXT("\"CallAgain\"")) && AfterEdit.Contains(TEXT("\"Offset\"")));
	UndoRedoAll(TEXT("after function edit"));
	TestEqual(TEXT("after function edit + undo/redo"), Serialize(Extract(*this, Target.ObjectPath, TEXT("after undo/redo"))), AfterEdit);
	// 2. A failing function apply (a function removed, then a refused link) rolls back; undo stays safe.
	const TSharedPtr<FJsonObject> Failing = ForTarget(Parse(AfterEdit), Target, TEXT("Update"));
	ObjectAt(BodyOf(Failing), TEXT("Functions"))->RemoveField(TEXT("Scale Weight"));
	NodesOf(Failing)->RemoveField(TEXT("CallScale"));
	NodesOf(Failing)->RemoveField(TEXT("CallAgain"));
	NodesOf(Failing)->SetObjectField(TEXT("Fine"), Json(R"J({"Unit": "RigVMFunction_MathFloatAdd", "Position": [0, 1400]})J"));
	NodesOf(Failing)->SetObjectField(TEXT("Bad"), Json(R"J({"Unit": "RigUnit_SetTransform", "Position": [300, 1400], "Inputs": {"Item": "Fine.Result"}})J"));
	const FAssetDocumentResult Failed = Apply(Target, Failing);
	TestFalse(TEXT("failing function apply fails"), Failed.IsSuccess());
	AddInfo(FString::Printf(TEXT("failing function apply: %s"), *DiagnosticsText(Failed)));
	const FString AfterRollback = Serialize(Extract(*this, Target.ObjectPath, TEXT("after rollback")));
	if (!TestEqual(TEXT("after rollback: the saved state"), AfterRollback, AfterEdit))
	{
		AddInfo(FirstDifference(AfterEdit, AfterRollback));
	}
	UndoRedoAll(TEXT("after rollback"));
	return true;
}

#endif
