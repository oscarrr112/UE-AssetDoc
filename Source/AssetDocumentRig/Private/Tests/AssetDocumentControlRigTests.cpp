// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Control Rig reads (docs/architecture/asset-document-controlrig-design.md §7.2); the rig apply tests are in
// AssetDocumentControlRigApplyTests.cpp).
// UEAssetDoc.ControlRig.Read: CR_AllKinds built with the editor's controllers (every node kind of
//   §3.3, two models, a function with a local variable, every element type, every control type, multi-parent
//   weights, spaces, channel hosts, metadata) and a modular rig from the factory: Extract stable twice and after
//   reload, Diff empty, no dirty package, Inspect pins equal the registry's pins; Graphs on a modular rig are refused
//   (ControlRig.ModularRigRegionUnsupported; modular edits are tested in AssetDocumentControlRigModularTests.cpp).
// UEAssetDoc.ControlRig.Corpus: the shipped Control Rig content (design §6.1 #1-#13; Template rigs
//   copied into /Game as a project created from the template has them) duplicated into /Game by the duplicate
//   operation: the same checks, with per-asset timings; shape libraries through the generic route with their Inspect facts.
// UEAssetDoc.ControlRig.Duplicate: the duplicate operation's copy of a rig with a reroute and of a rig whose post-load
//   skips the graph refresh extracts and compiles as the source, also after a reload; the Content Browser's Duplicate
//   of the same rigs as the reference.
// UEAssetDoc.ControlRig.ListTypes: list_types kinds rigvm_node / rigvm_type / controlrig_module.
// UEAssetDoc.ControlRig.Status: the compile status read without compiling: UBlueprint::Status, the
//   VM hash, per-node messages at the node's document path.

#include "AssetDocumentIKRigTestSupport.h"

#include "ControlRig.h"
#include "ControlRigBlueprintFactory.h"
#include "ControlRigBlueprintLegacy.h"
#include "ControlRigGizmoLibrary.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/RigVMEdGraph.h"
#include "EdGraph/RigVMEdGraphNode.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/EngineVersionComparison.h"
#include "ModularRig.h"
#include "ModularRigController.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "RigVMCore/RigVM.h"
#include "RigVMCore/RigVMDispatchFactory.h"
#include "RigVMCore/RigVMFunction.h"
#include "RigVMCore/RigVMRegistry.h"
#include "RigVMCore/RigVMTemplate.h"
#include "RigVMModel/Nodes/RigVMLibraryNode.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMFunctionLibrary.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMNode.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentControlRigTests
{
using namespace AssetDocumentIKRigTests;

const TCHAR* const ControlRigClass = TEXT("/Script/ControlRigDeveloper.ControlRigBlueprint");
const TCHAR* const BipedMesh = TEXT("/ControlRigModules/Modules/Meshes/SKM_Biped_Template.SKM_Biped_Template");

// ---- Template content -------------------------------------------------------------------------------------------

// Template content copied into /Game as creating a project from the template does: a whole content pack (so every
// reference of its meshes resolves) or one file. Only what this test copied is removed again; a top-level folder
// the project already has is left alone (its files are not overwritten).
struct FTemplateCopy
{
	const TCHAR* Source; // relative to the engine's Templates directory
	const TCHAR* Mount;  // /Game/<Mount>
	const TCHAR* Only;   // one asset (base name) of Source, or null for every asset under it
};

struct FTemplateContent
{
	TArray<FString> CopiedFiles;
	TArray<FString> CopiedPackages;
	TArray<FString> CreatedDirectories;

	explicit FTemplateContent(std::initializer_list<FTemplateCopy> Copies)
	{
		IFileManager& Files = IFileManager::Get();
		for (const FTemplateCopy& Copy : Copies)
		{
			const FString SourceDir = FPaths::Combine(FPaths::RootDir(), TEXT("Templates"), Copy.Source);
			const FString TargetDir = FPaths::Combine(FPaths::ProjectContentDir(), Copy.Mount);
			FString Top = Copy.Mount;
			Top.Split(TEXT("/"), &Top, nullptr);
			const FString TopDir = FPaths::Combine(FPaths::ProjectContentDir(), Top.IsEmpty() ? FString(Copy.Mount) : Top);
			if (!Files.DirectoryExists(*SourceDir))
			{
				continue;
			}
			if (Copy.Only == nullptr && Files.DirectoryExists(*TargetDir))
			{
				continue; // the project has the pack already
			}
			if (!Files.DirectoryExists(*TopDir))
			{
				CreatedDirectories.AddUnique(TopDir);
			}
			TArray<FString> Found;
			if (Copy.Only != nullptr)
			{
				Found.Add(FPaths::Combine(SourceDir, FString(Copy.Only) + TEXT(".uasset")));
			}
			else
			{
				Files.FindFilesRecursive(Found, *SourceDir, TEXT("*.uasset"), true, false);
			}
			for (const FString& From : Found)
			{
				FString Relative = From;
				FPaths::MakePathRelativeTo(Relative, *(SourceDir + TEXT("/")));
				const FString To = FPaths::Combine(TargetDir, Relative);
				if (!Files.FileExists(*From) || Files.FileExists(*To))
				{
					continue;
				}
				if (Files.Copy(*To, *From) == COPY_OK)
				{
					CopiedFiles.Add(To);
					CopiedPackages.Add(FString(TEXT("/Game/")) + Copy.Mount + TEXT("/") + FPaths::GetBaseFilename(Relative, false));
				}
			}
		}
		if (!CopiedFiles.IsEmpty())
		{
			FAssetRegistryModule::GetRegistry().ScanFilesSynchronous(CopiedFiles, true);
		}
	}

	FTemplateContent(const FTemplateContent&) = delete;
	FTemplateContent& operator=(const FTemplateContent&) = delete;

	~FTemplateContent()
	{
		if (CopiedFiles.IsEmpty())
		{
			return;
		}
		ReleaseUndoHistory();
		TArray<UPackage*> Loaded;
		for (const FString& Package : CopiedPackages)
		{
			if (UPackage* Found = FindPackage(nullptr, *Package))
			{
				Found->ClearDirtyFlag();
				Loaded.Add(Found);
			}
		}
		if (!Loaded.IsEmpty())
		{
			UPackageTools::UnloadPackages(Loaded);
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		for (const FString& File : CopiedFiles)
		{
			IFileManager::Get().Delete(*File, false, true);
		}
		FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles(CopiedFiles);
		for (const FString& Dir : CreatedDirectories)
		{
			IFileManager::Get().DeleteDirectory(*Dir, false, true);
		}
	}
};

// Design §6.1 #7-#9: the UE5 mannequin pack (rigs, meshes, materials), the vehicles pack and the first person /
// shooter rigs (which reference the mannequin meshes).
inline TUniquePtr<FTemplateContent> CopyCorpusTemplates()
{
	return MakeUnique<FTemplateContent>(std::initializer_list<FTemplateCopy>{
		{TEXT("TemplateResources/High/Characters/Content"), TEXT("Characters"), nullptr},
		{TEXT("TemplateResources/Standard/Vehicles/Content"), TEXT("Vehicles"), nullptr},
		{TEXT("TP_FirstPerson/Content/FirstPerson/Anims"), TEXT("FirstPerson/Anims"), TEXT("CtrlRig_FPWarp")},
		{TEXT("TemplateResources/Standard/Variant_Shooter/Content/Anims"), TEXT("Variant_Shooter/Anims"), TEXT("Ctrl_HandAdjusment")},
	});
}

// For the graph tests (AssetDocumentControlRigGraphTests.cpp): the template copy held through a shared pointer
// (its type stays private to this file) and the corpus entries.
TSharedPtr<FTemplateContent> ShareCorpusTemplates()
{
	return TSharedPtr<FTemplateContent>(CopyCorpusTemplates().Release());
}

// ---- document walking ---------------------------------------------------------------------------------------------

inline FString EscapeKey(const FString& Key)
{
	return Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
}

inline TSharedPtr<FJsonObject> ObjectField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	const TSharedPtr<FJsonObject>* Found = nullptr;
	return Object.IsValid() && Object->TryGetObjectField(Field, Found) ? *Found : nullptr;
}

// Every node entry of the Body (graphs, functions and collapsed graphs inside them) with its document path.
inline void CollectNodes(const TSharedPtr<FJsonObject>& Container, const FString& Path, TArray<TPair<FString, TSharedPtr<FJsonObject>>>& Out)
{
	const TSharedPtr<FJsonObject> Nodes = ObjectField(Container, TEXT("Nodes"));
	if (!Nodes.IsValid())
	{
		return;
	}
	for (const auto& Pair : Nodes->Values)
	{
		const FString NodePath = Path + TEXT("/Nodes/") + EscapeKey(FString(*Pair.Key));
		const TSharedPtr<FJsonObject> Node = Pair.Value->AsObject();
		Out.Emplace(NodePath, Node);
		if (const TSharedPtr<FJsonObject> Collapse = ObjectField(Node, TEXT("Collapse")))
		{
			CollectNodes(Collapse, NodePath + TEXT("/Collapse"), Out);
		}
	}
}

inline TArray<TPair<FString, TSharedPtr<FJsonObject>>> AllNodes(const TSharedPtr<FJsonObject>& Body)
{
	TArray<TPair<FString, TSharedPtr<FJsonObject>>> Out;
	for (const TCHAR* Region : {TEXT("Graphs"), TEXT("Functions")})
	{
		if (const TSharedPtr<FJsonObject> Containers = ObjectField(Body, Region))
		{
			for (const auto& Pair : Containers->Values)
			{
				CollectNodes(Pair.Value->AsObject(), FString(TEXT("/Body/")) + Region + TEXT("/") + EscapeKey(FString(*Pair.Key)), Out);
			}
		}
	}
	return Out;
}

inline int32 CountElements(const TSharedPtr<FJsonObject>& Body)
{
	const TArray<TSharedPtr<FJsonValue>>* Hierarchy = nullptr;
	return Body.IsValid() && Body->TryGetArrayField(TEXT("Hierarchy"), Hierarchy) ? Hierarchy->Num() : 0;
}

inline FString PinSignature(const TSharedPtr<FJsonObject>& Pin)
{
	return FString::Printf(TEXT("%s:%s:%s"), *Pin->GetStringField(TEXT("Name")), *Pin->GetStringField(TEXT("Type")),
		*Pin->GetStringField(TEXT("Direction")));
}

// "Inspect pins equal the registry" (design §7.2): every unit node's pins in Inspect (minus trait pins) equal the
// pins list_types rigvm_node gives for its unit, in order. Rows are cached across calls.
struct FRegistryPins
{
	TMap<FString, TArray<FString>> Rows;

	const TArray<FString>* Find(const FString& Unit)
	{
		if (const TArray<FString>* Known = Rows.Find(Unit))
		{
			return Known;
		}
		TArray<FString>& Signature = Rows.Add(Unit);
		const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().ListTypes(
			{{TEXT("kind"), TEXT("rigvm_node")}, {TEXT("name"), Unit}, {TEXT("limit"), TEXT("500")}});
		const TArray<TSharedPtr<FJsonValue>>* Types = nullptr;
		if (Result.IsSuccess() && Result.Payload.IsValid() && Result.Payload->TryGetArrayField(TEXT("Types"), Types))
		{
			for (const TSharedPtr<FJsonValue>& Row : *Types)
			{
				if (Row->AsObject()->GetStringField(TEXT("Name")) == Unit)
				{
					for (const TSharedPtr<FJsonValue>& Pin : Row->AsObject()->GetArrayField(TEXT("Pins")))
					{
						Signature.Add(PinSignature(Pin->AsObject()));
					}
					Signature.Insert(TEXT("<listed>"), 0);
					break;
				}
			}
		}
		return &Signature;
	}

	// Mismatches (path: detail) and the number of unit nodes compared.
	int32 Compare(const TSharedPtr<FJsonObject>& Body, const TSharedPtr<FJsonObject>& InspectPayload, TArray<FString>& OutProblems)
	{
		const TSharedPtr<FJsonObject> Pins = ObjectField(InspectPayload, TEXT("Pins"));
		int32 Compared = 0;
		for (const TPair<FString, TSharedPtr<FJsonObject>>& Node : AllNodes(Body))
		{
			FString Unit;
			if (!Node.Value->TryGetStringField(TEXT("Unit"), Unit) || Node.Value->HasField(TEXT("Aggregate")))
			{
				continue;
			}
			const TArray<TSharedPtr<FJsonValue>>* NodePins = nullptr;
			if (!Pins.IsValid() || !Pins->TryGetArrayField(Node.Key, NodePins))
			{
				OutProblems.Add(FString::Printf(TEXT("%s: Inspect has no pins"), *Node.Key));
				continue;
			}
			TArray<FString> Actual = {TEXT("<listed>")};
			for (const TSharedPtr<FJsonValue>& Pin : *NodePins)
			{
				bool bTrait = false;
				if (!Pin->AsObject()->TryGetBoolField(TEXT("Trait"), bTrait) || !bTrait)
				{
					Actual.Add(PinSignature(Pin->AsObject()));
				}
			}
			const TArray<FString>* Expected = Find(Unit);
			++Compared;
			if (Expected->IsEmpty())
			{
				OutProblems.Add(FString::Printf(TEXT("%s: unit %s is not listed by rigvm_node"), *Node.Key, *Unit));
			}
			else if (*Expected != Actual)
			{
				OutProblems.Add(FString::Printf(TEXT("%s (%s): pins [%s] vs registry [%s]"), *Node.Key, *Unit,
					*FString::Join(Actual, TEXT(", ")), *FString::Join(*Expected, TEXT(", "))));
			}
		}
		return Compared;
	}
};

// ---- stability ----------------------------------------------------------------------------------------------------

struct FReadReport
{
	TArray<FString> Problems;
	TSharedPtr<FJsonObject> Extracted;
	TSharedPtr<FJsonObject> Inspected;
	FString Timings;
};

// Extract twice, Diff, Inspect, no new dirty package, reload, extract again (design §7.2 read acceptance).
FReadReport CheckStable(FAutomationTestBase& Test, const FTarget& Target, const FString& Name)
{
	FReadReport Report;
	const FReadSideEffects SideEffects;
	double Start = FPlatformTime::Seconds();
	const FAssetDocumentResult First = ExtractResult(Target.ObjectPath);
	const double ExtractMs = Milliseconds(Start);
	if (!First.IsSuccess() || !First.Payload.IsValid())
	{
		Report.Problems.Add(FString::Printf(TEXT("extract failed: %s"), *DiagnosticsText(First)));
		return Report;
	}
	Report.Extracted = First.Payload;
	const FString Baseline = Serialize(First.Payload);
	const FString Again = Serialize(Extract(Test, Target.ObjectPath, Name + TEXT(" again")));
	if (Again != Baseline)
	{
		Report.Problems.Add(FString::Printf(TEXT("second extraction differs %s"), *FirstDifference(Baseline, Again)));
	}
	Start = FPlatformTime::Seconds();
	const FAssetDocumentResult Inspected = Inspect(Target.ObjectPath);
	const double InspectMs = Milliseconds(Start);
	if (!Inspected.IsSuccess() || !Inspected.Payload.IsValid())
	{
		Report.Problems.Add(FString::Printf(TEXT("inspect failed: %s"), *DiagnosticsText(Inspected)));
	}
	Report.Inspected = Inspected.Payload;
	FString DiffText;
	const int32 Changes = DiffCount(First.Payload, DiffText);
	if (Changes != 0)
	{
		Report.Problems.Add(FString::Printf(TEXT("Diff of the extraction reports %d changes: %s"), Changes, *DiffText.Left(600)));
	}
	SideEffects.Check(Report.Problems);
	FString Status = TEXT("-");
	if (First.Compile.IsValid())
	{
		Status = First.Compile->GetStringField(TEXT("Status"));
		const TArray<TSharedPtr<FJsonValue>>* Messages = nullptr;
		if (First.Compile->TryGetArrayField(TEXT("Messages"), Messages) && !Messages->IsEmpty())
		{
			Status += FString::Printf(TEXT(" (%d messages)"), Messages->Num());
		}
	}
	Start = FPlatformTime::Seconds();
	const bool bReloaded = Reload(Target) != nullptr;
	const double ReloadMs = Milliseconds(Start);
	if (!bReloaded)
	{
		Report.Problems.Add(TEXT("reload failed"));
	}
	else
	{
		const FString Reloaded = Serialize(Extract(Test, Target.ObjectPath, Name + TEXT(" after reload")));
		if (Reloaded != Baseline)
		{
			Report.Problems.Add(FString::Printf(TEXT("extraction after reload differs %s"), *FirstDifference(Baseline, Reloaded)));
		}
	}
	Report.Timings = FString::Printf(TEXT("extract %.0f ms, inspect %.0f ms, reload %.0f ms, %d chars, status %s"), ExtractMs,
		InspectMs, ReloadMs, Baseline.Len(), *Status);
	return Report;
}

// What stays read only in the rig apply (graphs, modular rigs) is refused with a stable code and changes nothing: the extract
// with one change of that region applied (an unchanged Update is a no-op before the profile is asked).
void CheckRefused(FAutomationTestBase& Test, const FTarget& Target, const TSharedPtr<FJsonObject>& Document, const FString& Name,
	TFunctionRef<void(const TSharedPtr<FJsonObject>& Body)> Change, const TCHAR* Code)
{
	if (!Document.IsValid() || !BodyOf(Document).IsValid())
	{
		return;
	}
	const TSharedPtr<FJsonObject> Changed = Parse(Serialize(Document));
	Change(BodyOf(Changed));
	const FAssetDocumentResult Refused = Apply(Target, Changed);
	Test.TestFalse(*FString::Printf(TEXT("%s: apply is refused"), *Name), Refused.IsSuccess());
	Test.TestTrue(*FString::Printf(TEXT("%s: refusal code %s (%s)"), *Name, Code, *DiagnosticsText(Refused)), HasCode(Refused, Code));
	Test.TestFalse(*FString::Printf(TEXT("%s: nothing saved"), *Name), Refused.bSavedAsset);
	Test.TestEqual(*FString::Printf(TEXT("%s: unchanged"), *Name), Serialize(ExtractResult(Target.ObjectPath).Payload), Serialize(Document));
}

// ---- fixtures -----------------------------------------------------------------------------------------------------

// The Content Browser's "Control Rig" (UControlRigBlueprintFactory, as FControlRigBlueprintActions::
// CreateNewControlRigAsset does), on a mesh as "Create Control Rig" on a skeletal mesh does
// (CreateControlRigFromSkeletalMeshOrSkeleton: bones, curves, source import, preview mesh).
UControlRigBlueprint* MakeControlRig(FAutomationTestBase& Test, const FTarget& Target, const TCHAR* MeshPath, bool bModular)
{
	UControlRigBlueprintFactory* Factory = NewObject<UControlRigBlueprintFactory>();
	Factory->ParentClass = bModular ? UModularRig::StaticClass() : UControlRig::StaticClass();
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UControlRigBlueprint* Blueprint = Cast<UControlRigBlueprint>(AssetTools.CreateAsset(Target.AssetName,
		FPackageName::GetLongPackagePath(Target.PackageName), UControlRigBlueprint::StaticClass(), Factory));
	if (!Test.TestNotNull(TEXT("Control Rig created"), Blueprint) || MeshPath == nullptr)
	{
		return Blueprint;
	}
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, MeshPath);
	if (!Test.TestNotNull(*FString::Printf(TEXT("mesh %s loads"), MeshPath), Mesh))
	{
		return Blueprint;
	}
	URigHierarchyController* Hierarchy = Blueprint->GetHierarchy()->GetController(true);
	Hierarchy->ImportBones(Mesh->GetRefSkeleton(), NAME_None, false, false, false, false);
	Hierarchy->ImportCurvesFromSkeletalMesh(Mesh, NAME_None, false, false);
	Blueprint->GetSourceHierarchyImport() = Mesh->GetSkeleton();
	Blueprint->SetPreviewMesh(Mesh);
	return Blueprint;
}

URigVMUnitNode* AddUnit(FAutomationTestBase& Test, URigVMController& Controller, const TCHAR* StructPath, const TCHAR* Name, double X, double Y)
{
	URigVMUnitNode* Node = Controller.AddUnitNodeFromStructPath(StructPath, TEXT("Execute"), FVector2D(X, Y), Name, false);
	Test.TestNotNull(*FString::Printf(TEXT("unit node %s (%s)"), Name, StructPath), Node);
	return Node;
}

void Link(FAutomationTestBase& Test, URigVMController& Controller, const TCHAR* From, const TCHAR* To)
{
	Test.TestTrue(*FString::Printf(TEXT("link %s -> %s"), From, To), Controller.AddLink(From, To, false));
}

// CR_AllKinds (design §6.1): built only through the controllers the editor's panels use, compiled as the
// editor's Compile button does, saved.
UControlRigBlueprint* MakeAllKinds(FAutomationTestBase& Test, const FTarget& Target)
{
	UControlRigBlueprint* Blueprint = MakeControlRig(Test, Target, TutorialMesh, false);
	if (Blueprint == nullptr)
	{
		return nullptr;
	}
	URigVMBlueprint& RigVM = *Blueprint;

	// Variables.
	Test.TestEqual(TEXT("member variable"), RigVM.AddMemberVariable(TEXT("Weight"), TEXT("float"), true, false, TEXT("0.5")), FName(TEXT("Weight")));
	RigVM.AddMemberVariable(TEXT("Target"), TEXT("/Script/CoreUObject.Vector"), false, false, FString());

	// Rig graph: events, units, a template, a dispatch, variables, comment, reroute, enum, invoke, bound pin,
	// sub-pin link, aggregate, collapse.
	URigVMController* Rig = RigVM.GetOrCreateController(RigVM.GetDefaultModel());
	if (!Test.TestNotNull(TEXT("rig graph controller"), Rig))
	{
		return Blueprint;
	}
	AddUnit(Test, *Rig, TEXT("/Script/ControlRig.RigUnit_BeginExecution"), TEXT("ForwardsSolve"), 0, 0);
	AddUnit(Test, *Rig, TEXT("/Script/ControlRig.RigUnit_InverseExecution"), TEXT("BackwardsSolve"), 0, 400);
	AddUnit(Test, *Rig, TEXT("/Script/RigVM.RigVMFunction_UserDefinedEvent"), TEXT("UserEvent"), 0, 800);
	AddUnit(Test, *Rig, TEXT("/Script/ControlRig.RigUnit_GetTransform"), TEXT("GetHead"), 0, 200);
	AddUnit(Test, *Rig, TEXT("/Script/ControlRig.RigUnit_SetTransform"), TEXT("SetHead"), 400, 0);
	Link(Test, *Rig, TEXT("ForwardsSolve.ExecutePin"), TEXT("SetHead.ExecutePin"));
	Link(Test, *Rig, TEXT("GetHead.Transform.Translation"), TEXT("SetHead.Value.Translation"));
	Test.TestTrue(TEXT("element default"), Rig->SetPinDefaultValue(TEXT("SetHead.Item"), TEXT("(Type=Bone,Name=\"head\")"), true, false));
	Test.TestTrue(TEXT("element default (get)"), Rig->SetPinDefaultValue(TEXT("GetHead.Item"), TEXT("(Type=Bone,Name=\"neck_01\")"), true, false));
	Test.TestTrue(TEXT("bound pin"), Rig->BindPinToVariable(TEXT("SetHead.Weight"), TEXT("Weight"), false));
	Test.TestTrue(TEXT("pin expansion"), Rig->SetPinExpansion(TEXT("SetHead.Value"), true, false));
	Rig->AddVariableNode(TEXT("Weight"), TEXT("float"), nullptr, true, FString(), FVector2D(-300, 200), TEXT("GetWeight"), false);
	Rig->AddVariableNode(TEXT("Weight"), TEXT("float"), nullptr, false, FString(), FVector2D(800, 0), TEXT("SetWeight"), false);
	Link(Test, *Rig, TEXT("SetHead.ExecutePin"), TEXT("SetWeight.ExecuteContext"));
	Rig->AddCommentNode(TEXT("All node kinds"), FVector2D(-400, -300), FVector2D(500, 200), FLinearColor(0.2f, 0.4f, 0.8f), TEXT("Comment"), false);
	Rig->AddFreeRerouteNode(TEXT("float"), NAME_None, false, NAME_None, TEXT("0.25"), FVector2D(200, 600), TEXT("Reroute"), false);
	Test.TestNotNull(TEXT("enum node"), Rig->AddEnumNode(TEXT("/Script/ControlRig.ERigElementType"), FVector2D(200, 700), TEXT("ElementType"), false));
	Test.TestNotNull(TEXT("invoke event node"), Rig->AddInvokeEntryNode(TEXT("My Event"), FVector2D(400, 800), TEXT("RunMyEvent"), false));
	Test.TestNotNull(TEXT("dispatch node"), Rig->AddIfNode(TEXT("float"), NAME_None, FVector2D(600, 400), TEXT("If"), false));
	// A reroute without links is removed on load (URigVMController::PatchRerouteNodesOnLoad): link it through.
	Link(Test, *Rig, TEXT("GetWeight.Value"), TEXT("Reroute.Value"));
	Link(Test, *Rig, TEXT("Reroute.Value"), TEXT("If.True"));
	// Node titles are editable on renamable dispatches only (URigVMSchema::CanSetNodeTitle): a Constant.
	const UScriptStruct* ConstantStruct = FindObject<UScriptStruct>(nullptr, TEXT("/Script/RigVM.RigVMDispatch_Constant"));
	const FRigVMDispatchFactory* ConstantFactory = ConstantStruct != nullptr
		? FRigVMRegistry::Get().FindDispatchFactory(FRigVMDispatchFactory::GetFactoryName(ConstantStruct)) : nullptr;
	const FRigVMTemplate* ConstantTemplate = ConstantFactory != nullptr ? ConstantFactory->GetTemplate() : nullptr;
	if (Test.TestNotNull(TEXT("Constant dispatch"), ConstantTemplate))
	{
		URigVMNode* Constant = Rig->AddTemplateNode(ConstantTemplate->GetNotation(), FVector2D(200, 900), TEXT("Constant"), false);
		Test.TestTrue(TEXT("constant type"), Constant != nullptr
			&& Rig->ResolveWildCardPin(TEXT("Constant.Value"), TEXT("float"), NAME_None, false));
		Test.TestTrue(TEXT("node title"), Constant != nullptr && Rig->SetNodeTitle(Constant, TEXT("Blend Amount"), false));
	}
	UScriptStruct* AddStruct = FindObject<UScriptStruct>(nullptr, TEXT("/Script/RigVM.RigVMFunction_MathFloatAdd"));
	const FRigVMFunction* AddFunction = AddStruct != nullptr ? FRigVMRegistry::Get().FindFunction(AddStruct, TEXT("Execute")) : nullptr;
	const FRigVMTemplate* AddTemplate = AddFunction != nullptr ? AddFunction->GetTemplate() : nullptr;
	if (Test.TestNotNull(TEXT("Add template"), AddTemplate))
	{
		Test.TestNotNull(TEXT("template node"), Rig->AddTemplateNode(AddTemplate->GetNotation(), FVector2D(600, 600), TEXT("AnyAdd"), false));
	}
	if (URigVMUnitNode* Sum = AddUnit(Test, *Rig, TEXT("/Script/RigVM.RigVMFunction_MathFloatAdd"), TEXT("Sum"), 600, 200))
	{
		Test.TestFalse(TEXT("aggregate pin"), Rig->AddAggregatePin(Sum, FString(), TEXT("2.0"), false).IsEmpty());
	}
	AddUnit(Test, *Rig, TEXT("/Script/RigVM.RigVMFunction_MathFloatMul"), TEXT("Mul"), 0, 1000);
	AddUnit(Test, *Rig, TEXT("/Script/RigVM.RigVMFunction_MathFloatSub"), TEXT("Sub"), 300, 1000);
	Link(Test, *Rig, TEXT("Mul.Result"), TEXT("Sub.A"));
	Test.TestNotNull(TEXT("collapse node"), Rig->CollapseNodes(TArray<FName>{FName(TEXT("Mul")), FName(TEXT("Sub"))}, TEXT("Collapsed"), false));
	// Node colors are editable on comments and library nodes only (URigVMSchema::CanRecolorNode).
	Test.TestTrue(TEXT("node color"), Rig->SetNodeColor(Rig->GetGraph()->FindNodeByName(TEXT("Collapsed")), FLinearColor(1.f, 0.5f, 0.f), false));

	// Function with an input, an output, a local variable; public; referenced from the rig graph.
	URigVMFunctionLibrary* Library = RigVM.GetLocalFunctionLibrary();
	URigVMController* LibraryController = Library != nullptr ? RigVM.GetOrCreateController(Library) : nullptr;
	URigVMLibraryNode* Function = LibraryController != nullptr ? LibraryController->AddFunctionToLibrary(TEXT("Scale Weight"), true, FVector2D::ZeroVector, false) : nullptr;
	if (Test.TestNotNull(TEXT("function"), Function))
	{
		URigVMController* Body = RigVM.GetOrCreateController(Function->GetContainedGraph());
		Body->AddExposedPin(TEXT("Amount"), ERigVMPinDirection::Input, TEXT("float"), NAME_None, TEXT("1.5"), false);
		Body->AddExposedPin(TEXT("Scaled"), ERigVMPinDirection::Output, TEXT("float"), NAME_None, TEXT("0.0"), false);
		Body->AddLocalVariable(TEXT("Factor"), TEXT("float"), nullptr, TEXT("2.0"), false);
		AddUnit(Test, *Body, TEXT("/Script/RigVM.RigVMFunction_MathFloatMul"), TEXT("Times"), 200, 0);
		Link(Test, *Body, TEXT("Entry.Amount"), TEXT("Times.A"));
		Link(Test, *Body, TEXT("Times.Result"), TEXT("Return.Scaled"));
		RigVM.MarkFunctionPublic(Function->GetFName(), true);
		Test.TestNotNull(TEXT("function reference"), Rig->AddFunctionReferenceNode(Function, FVector2D(1100, 0), TEXT("CallScale"), false));
	}

	// A second model with the Construction event.
	if (URigVMGraph* Construction = RigVM.AddModel(TEXT("Construction"), false, false))
	{
		URigVMController* Second = RigVM.GetOrCreateController(Construction);
		AddUnit(Test, *Second, TEXT("/Script/ControlRig.RigUnit_PrepareForExecution"), TEXT("Construction"), 0, 0);
	}
	else
	{
		Test.AddError(TEXT("second model"));
	}

	// Hierarchy: nulls (one with two weighted parents), every control type (limits on the float), a channel with a
	// second host, a space, a curve, a connector, a socket, metadata.
	URigHierarchy* Hierarchy = Blueprint->GetHierarchy();
	URigHierarchyController* Elements = Hierarchy->GetController(true);
	const FRigElementKey Root(TEXT("root"), ERigElementType::Bone);
	const FRigElementKey Pelvis(TEXT("pelvis"), ERigElementType::Bone);
	const FRigElementKey Head(TEXT("head"), ERigElementType::Bone);
	const FRigElementKey Space = Elements->AddNull(TEXT("space_null"), Root, FTransform(FVector(0.0, 0.0, 100.0)), true, false);
	const FRigElementKey Multi = Elements->AddNull(TEXT("multi_null"), Root, FTransform(FVector(10.0, 0.0, 0.0)), true, false);
	Test.TestTrue(TEXT("second parent"), Elements->AddParent(Multi, Pelvis, 0.5f, true, TEXT("Pelvis"), false));
	const ERigControlType Types[] = {ERigControlType::Bool, ERigControlType::Float, ERigControlType::ScaleFloat,
		ERigControlType::Integer, ERigControlType::Vector2D, ERigControlType::Position, ERigControlType::Scale,
		ERigControlType::Rotator, ERigControlType::Transform, ERigControlType::TransformNoScale, ERigControlType::EulerTransform};
	TMap<ERigControlType, FRigElementKey> Controls;
	for (const ERigControlType Type : Types)
	{
		FRigControlSettings Settings;
		Settings.ControlType = Type;
		if (Type == ERigControlType::Float)
		{
			Settings.SetupLimitArrayForType(true, true, true);
			Settings.MinimumValue = FRigControlValue::Make<float>(-1.f);
			Settings.MaximumValue = FRigControlValue::Make<float>(2.f);
		}
		FRigControlValue Value;
		Value.SetFromTransform(FTransform(FRotator(0.0, 30.0, 0.0), FVector(1.0, 2.0, 3.0), FVector::OneVector), Type, Settings.PrimaryAxis);
		const FString Name = FString::Printf(TEXT("%s_ctrl"), *StaticEnum<ERigControlType>()->GetNameStringByValue(static_cast<int64>(Type)));
		const FRigElementKey Key = Elements->AddControl(*Name, Head, Settings, Value, FTransform(FVector(0.0, 5.0, 0.0)), FTransform::Identity, false);
		Test.TestTrue(*FString::Printf(TEXT("control %s"), *Name), Key.IsValid());
		Controls.Add(Type, Key);
	}
	FRigControlSettings ChannelSettings;
	ChannelSettings.ControlType = ERigControlType::Float;
	ChannelSettings.AnimationType = ERigControlAnimationType::AnimationChannel;
	const FRigElementKey Channel = Elements->AddAnimationChannel(TEXT("blend"), Controls.FindRef(ERigControlType::EulerTransform), ChannelSettings, false);
	Test.TestTrue(TEXT("animation channel"), Channel.IsValid());
	Test.TestTrue(TEXT("channel host"), Elements->AddChannelHost(Channel, Controls.FindRef(ERigControlType::Transform), false));
	Test.TestTrue(TEXT("available space"), Elements->AddAvailableSpace(Controls.FindRef(ERigControlType::EulerTransform), Space, NAME_None, false));
	Test.TestTrue(TEXT("curve"), Elements->AddCurve(TEXT("test_curve"), 0.25f, false).IsValid());
	Test.TestTrue(TEXT("socket"), Elements->AddSocket(TEXT("test_socket"), Head, FTransform(FVector(0.0, 0.0, 10.0)), true, FLinearColor::Red, TEXT("a socket"), false).IsValid());
	const FRigElementKey Connector = Elements->AddConnector(TEXT("test_connector"), FRigConnectorSettings::DefaultSettings(), false);
	if (!Connector.IsValid())
	{
		Test.AddInfo(TEXT("the hierarchy controller refuses a connector outside a module (not exercised)"));
	}
	const FRigElementKey Euler = Controls.FindRef(ERigControlType::EulerTransform);
	Test.TestTrue(TEXT("float metadata"), Hierarchy->SetFloatMetadata(Euler, TEXT("meta_float"), 1.5f));
	Test.TestTrue(TEXT("name metadata"), Hierarchy->SetNameMetadata(Euler, TEXT("meta_name"), TEXT("hello")));
	Test.TestTrue(TEXT("key metadata"), Hierarchy->SetRigElementKeyMetadata(Euler, TEXT("meta_key"), Head));

	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	Test.TestTrue(TEXT("CR_AllKinds saved"), Save(*Blueprint));
	return Blueprint;
}

// A modular rig from the factory (Root module from the Control Rig settings) on the biped template mesh, a Spine
// module under Root, the Root connector connected to the root bone (auto-resolve on, as the editor's drop does).
UControlRigBlueprint* MakeModular(FAutomationTestBase& Test, const FTarget& Target)
{
	UControlRigBlueprint* Blueprint = MakeControlRig(Test, Target, BipedMesh, true);
	if (Blueprint == nullptr)
	{
		return nullptr;
	}
	UModularRigController* Modules = Blueprint->GetModularRigController();
	const UControlRigBlueprint* Spine = LoadObject<UControlRigBlueprint>(nullptr, TEXT("/ControlRigModules/Modules56/Spine.Spine"));
	if (Modules != nullptr && Test.TestNotNull(TEXT("Spine module asset"), Spine) && Spine->GeneratedClass != nullptr)
	{
		const TSubclassOf<UControlRig> SpineClass(Spine->GeneratedClass.Get());
		Test.TestEqual(TEXT("Spine module added"), Modules->AddModule(TEXT("Spine"), SpineClass, TEXT("Root"), false), FName(TEXT("Spine")));
		if (!Modules->ConnectConnectorToElement(FRigElementKey(TEXT("Root/Root"), ERigElementType::Connector),
			FRigElementKey(TEXT("root"), ERigElementType::Bone), false, true, true))
		{
			Test.AddInfo(TEXT("Root/Root connector not connected headlessly (the modular read is still checked)"));
		}
	}
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	Test.TestTrue(TEXT("modular rig saved"), Save(*Blueprint));
	return Blueprint;
}

// ---- corpus ---------------------------------------------------------------------------------------------------------

struct FControlRigCorpusEntry
{
	const TCHAR* Path;
	const TCHAR* Source;
	const TCHAR* Why;
};

// Design §6.1. Template entries are copied into /Game first; plugin entries need their plugin (ControlRig,
// ControlRigSpline, ControlRigModules are on by default; MoverExamples and AnimatorKit are enabled in the test
// sandbox; MetaHumanCharacter is optional).
const FControlRigCorpusEntry ControlRigCorpus[] = {
	{TEXT("/ControlRig/StandardFunctionLibrary/StandardFunctionLibrary.StandardFunctionLibrary"), TEXT("ControlRig"), TEXT("#1 function library")},
	{TEXT("/ControlRigSpline/SplineFunctionLibrary/SplineFunctionLibrary.SplineFunctionLibrary"), TEXT("ControlRigSpline"), TEXT("#2 older library")},
	{TEXT("/ControlRig/Modules/Modules56/Root.Root"), TEXT("ControlRig"), TEXT("#3 module")},
	{TEXT("/ControlRigModules/Modules56/Arm.Arm"), TEXT("ControlRigModules"), TEXT("#4 large module")},
	{TEXT("/ControlRigModules/Modules56/Leg.Leg"), TEXT("ControlRigModules"), TEXT("#4 large module")},
	{TEXT("/ControlRigModules/Modules56/Spine.Spine"), TEXT("ControlRigModules"), TEXT("#4 large module")},
	{TEXT("/ControlRigModules/Modules56/Constraint.Constraint"), TEXT("ControlRigModules"), TEXT("#5 small module")},
	{TEXT("/ControlRigModules/Modules56/Hinge.Hinge"), TEXT("ControlRigModules"), TEXT("#5 small module")},
	{TEXT("/ControlRigModules/Modules56/Wheel.Wheel"), TEXT("ControlRigModules"), TEXT("#5 small module")},
	{TEXT("/ControlRigModules/Modules56/Finger.Finger"), TEXT("ControlRigModules"), TEXT("#5 small module")},
	{TEXT("/ControlRigModules/Modules/Arm.Arm"), TEXT("ControlRigModules"), TEXT("#6 pre-5.6 module")},
	{TEXT("/Game/Characters/Mannequins/Rigs/CR_Mannequin_Body.CR_Mannequin_Body"), TEXT("Template"), TEXT("#7 body rig")},
	{TEXT("/Game/Characters/Mannequins/Rigs/CR_Mannequin_FootIK.CR_Mannequin_FootIK"), TEXT("Template"), TEXT("#8 foot IK")},
	{TEXT("/Game/Characters/Mannequins/Rigs/CR_Mannequin_Procedural.CR_Mannequin_Procedural"), TEXT("Template"), TEXT("#8 procedural")},
	{TEXT("/Game/FirstPerson/Anims/CtrlRig_FPWarp.CtrlRig_FPWarp"), TEXT("Template"), TEXT("#9 first person")},
	{TEXT("/Game/Variant_Shooter/Anims/Ctrl_HandAdjusment.Ctrl_HandAdjusment"), TEXT("Template"), TEXT("#9 shooter")},
	{TEXT("/Game/Vehicles/SportsCar/CtrlRig_SportsCar.CtrlRig_SportsCar"), TEXT("Template"), TEXT("#9 vehicle")},
	{TEXT("/Game/Vehicles/OffroadCar/Offroad_CtrlRig.Offroad_CtrlRig"), TEXT("Template"), TEXT("#9 vehicle")},
	{TEXT("/ControlRig/Controls/DefaultGizmoLibraryNormalized.DefaultGizmoLibraryNormalized"), TEXT("ControlRig"), TEXT("#10 shape library")},
	{TEXT("/ControlRig/Controls/DefaultGizmoLibrary.DefaultGizmoLibrary"), TEXT("ControlRig"), TEXT("#10 shape library (legacy)")},
	{TEXT("/MoverExamples/Characters/Mannequins/Rigs/CR_Mannequin_Body.CR_Mannequin_Body"), TEXT("MoverExamples"), TEXT("#11 older body rig")},
	{TEXT("/MoverExamples/Characters/Mannequins/Rigs/CR_Mannequin_BasicFootIK.CR_Mannequin_BasicFootIK"), TEXT("MoverExamples"), TEXT("#11")},
	{TEXT("/MoverExamples/Characters/Mannequins/Rigs/CR_Mannequin_Procedural.CR_Mannequin_Procedural"), TEXT("MoverExamples"), TEXT("#11")},
	{TEXT("/AnimatorKit/UtilityRigs/CRU_AddLocator.CRU_AddLocator"), TEXT("AnimatorKit"), TEXT("#12")},
	{TEXT("/AnimatorKit/UtilityRigs/CRU_MorphTargetDriver.CRU_MorphTargetDriver"), TEXT("AnimatorKit"), TEXT("#12 morph driver")},
	{TEXT("/AnimatorKit/UtilityRigs/CRU_ChainSim.CRU_ChainSim"), TEXT("AnimatorKit"), TEXT("#12 simulation")},
	{TEXT("/AnimatorKit/UtilityRigs/CRU_SplinePath.CRU_SplinePath"), TEXT("AnimatorKit"), TEXT("#12 spline")},
	{TEXT("/AnimatorKit/UtilityRigs/CRU_SingleSim.CRU_SingleSim"), TEXT("AnimatorKit"), TEXT("#12 simulation")},
	{TEXT("/AnimatorKit/UtilityRigs/CRU_BlendParent.CRU_BlendParent"), TEXT("AnimatorKit"), TEXT("#12")},
	{TEXT("/AnimatorKit/UtilityRigs/CRU_3Node.CRU_3Node"), TEXT("AnimatorKit"), TEXT("#12")},
	{TEXT("/AnimatorKit/DeformerRigs/LatticeDeformer/CRD_Lattice.CRD_Lattice"), TEXT("AnimatorKit"), TEXT("#12 deformer")},
	{TEXT("/AnimatorKit/DeformerRigs/LatticeDeformer/CRD_Lattice_2x2x2.CRD_Lattice_2x2x2"), TEXT("AnimatorKit"), TEXT("#12 deformer")},
	{TEXT("/AnimatorKit/DeformerRigs/LatticeDeformer/CRD_Lattice_3x3x3.CRD_Lattice_3x3x3"), TEXT("AnimatorKit"), TEXT("#12 deformer")},
	{TEXT("/AnimatorKit/DeformerRigs/LatticeDeformer/CRD_Lattice_4x4x4.CRD_Lattice_4x4x4"), TEXT("AnimatorKit"), TEXT("#12 deformer")},
	{TEXT("/AnimatorKit/Locator/ShapeControlLibLocator.ShapeControlLibLocator"), TEXT("AnimatorKit"), TEXT("#12 custom shape library")},
	{TEXT("/MetaHumanCharacter/Animation/CR_PlaceHolder.CR_PlaceHolder"), TEXT("MetaHumanCharacter"), TEXT("#13 (optional)")},
	{TEXT("/MetaHumanCharacter/Controls/CR_MH_Function_Library.CR_MH_Function_Library"), TEXT("MetaHumanCharacter"), TEXT("#13 (optional)")},
	{TEXT("/MetaHumanCharacter/Common/MetaHuman_ControlRig.MetaHuman_ControlRig"), TEXT("MetaHumanCharacter"), TEXT("#13 (optional)")},
	{TEXT("/MetaHumanCharacter/Controls/CRSL_MetaHuman_Gizmo.CRSL_MetaHuman_Gizmo"), TEXT("MetaHumanCharacter"), TEXT("#13 (optional)")},
};
void ForEachCorpusEntry(TFunctionRef<void(const TCHAR* Path, const TCHAR* Why)> Visit)
{
	for (const FControlRigCorpusEntry& Entry : ControlRigCorpus)
	{
		Visit(Entry.Path, Entry.Why);
	}
}
} // namespace AssetDocumentControlRigTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigReadTest, "UEAssetDoc.ControlRig.Read",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentControlRigReadTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentControlRigTests;
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("CR_AllKinds"));
	if (MakeAllKinds(*this, Target) == nullptr)
	{
		return false;
	}
	const FReadReport Report = CheckStable(*this, Target, TEXT("CR_AllKinds"));
	for (const FString& Problem : Report.Problems)
	{
		AddError(FString::Printf(TEXT("CR_AllKinds: %s"), *Problem));
	}
	AddInfo(FString::Printf(TEXT("CR_AllKinds: %s"), *Report.Timings));
	AddInfo(SerializePretty(Report.Extracted));
	if (!Report.Extracted.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("Class"), Report.Extracted->GetStringField(TEXT("Class")), FString(ControlRigClass));
	const TSharedPtr<FJsonObject> Body = BodyOf(Report.Extracted);

	// Every node kind.
	TSet<FString> Kinds;
	for (const TPair<FString, TSharedPtr<FJsonObject>>& Node : AllNodes(Body))
	{
		for (const TCHAR* Kind : {TEXT("Unit"), TEXT("Variable"), TEXT("Comment"), TEXT("Reroute"), TEXT("Enum"), TEXT("InvokeEvent"),
			TEXT("Function"), TEXT("Aggregate"), TEXT("Collapse"), TEXT("Setter")})
		{
			if (Node.Value->HasField(Kind))
			{
				Kinds.Add(Kind);
			}
		}
		FString Template;
		if (Node.Value->TryGetStringField(TEXT("Template"), Template))
		{
			Kinds.Add(Template.StartsWith(TEXT("DISPATCH_")) ? TEXT("Dispatch") : TEXT("Template"));
		}
	}
	for (const TCHAR* Kind : {TEXT("Unit"), TEXT("Template"), TEXT("Dispatch"), TEXT("Variable"), TEXT("Setter"), TEXT("Comment"),
		TEXT("Reroute"), TEXT("Enum"), TEXT("InvokeEvent"), TEXT("Function"), TEXT("Aggregate"), TEXT("Collapse")})
	{
		TestTrue(*FString::Printf(TEXT("node kind %s is read"), Kind), Kinds.Contains(Kind));
	}
	const TSharedPtr<FJsonObject> Graphs = ObjectField(Body, TEXT("Graphs"));
	TestTrue(TEXT("two models"), Graphs.IsValid() && Graphs->Values.Num() == 2 && Graphs->HasField(TEXT("Rig")));
	const TSharedPtr<FJsonObject> Functions = ObjectField(Body, TEXT("Functions"));
	const TSharedPtr<FJsonObject> Function = Functions.IsValid() && Functions->Values.Num() == 1
		? Functions->Values.CreateConstIterator()->Value->AsObject() : nullptr;
	if (TestNotNull(TEXT("one function"), Function.Get()))
	{
		bool bPublic = false;
		TestTrue(TEXT("function is public"), Function->TryGetBoolField(TEXT("Public"), bPublic) && bPublic);
		TestEqual(TEXT("function local variable"), CountField(Function, TEXT("LocalVariables")), 1);
		// A mutable function lists its execute pin first (ExecuteContext, Direction IO), then Amount.
		const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
		if (TestTrue(TEXT("function inputs"), Function->TryGetArrayField(TEXT("Inputs"), Inputs)) && TestEqual(TEXT("function inputs"), Inputs->Num(), 2))
		{
			const TSharedPtr<FJsonObject> Execute = (*Inputs)[0]->AsObject();
			TestTrue(TEXT("function execute pin"), Execute.IsValid() && Execute->GetStringField(TEXT("Name")) == TEXT("ExecuteContext")
				&& Execute->GetStringField(TEXT("Direction")) == TEXT("IO"));
			const TSharedPtr<FJsonObject> Amount = (*Inputs)[1]->AsObject();
			TestTrue(TEXT("function input Amount"), Amount.IsValid() && Amount->GetStringField(TEXT("Name")) == TEXT("Amount"));
		}
		TestEqual(TEXT("function outputs"), CountField(Function, TEXT("Outputs")), 1);
	}
	const TSharedPtr<FJsonObject> Rig = ObjectField(ObjectField(Graphs, TEXT("Rig")), TEXT("Nodes"));
	const TSharedPtr<FJsonObject> SetHead = ObjectField(Rig, TEXT("SetHead"));
	if (TestNotNull(TEXT("SetHead"), SetHead.Get()))
	{
		TestTrue(TEXT("bound pin"), ObjectField(SetHead, TEXT("Bindings")).IsValid());
		TestTrue(TEXT("sub-pin link"), ObjectField(SetHead, TEXT("Inputs")).IsValid()
			&& ObjectField(SetHead, TEXT("Inputs"))->HasField(TEXT("Value.Translation")));
	}
	const TSharedPtr<FJsonObject> Constant = ObjectField(Rig, TEXT("Constant"));
	FString Title;
	TestTrue(TEXT("node title is read"), Constant.IsValid() && Constant->TryGetStringField(TEXT("Title"), Title) && Title == TEXT("Blend Amount"));
	const TSharedPtr<FJsonObject> Collapsed = ObjectField(Rig, TEXT("Collapsed"));
	TestTrue(TEXT("node color is read"), Collapsed.IsValid() && Collapsed->HasField(TEXT("Color")));
	const TSharedPtr<FJsonObject> Reroute = ObjectField(Rig, TEXT("Reroute"));
	TestTrue(TEXT("linked reroute is read"), Reroute.IsValid() && ObjectField(Reroute, TEXT("Inputs")).IsValid());
	TestEqual(TEXT("variables"), CountField(Body, TEXT("Variables")), 2);

	// Every element type, weights, spaces, hosts, metadata.
	TSet<FString> ElementTypes;
	bool bWeighted = false;
	bool bSpaces = false;
	bool bHosts = false;
	bool bMetadata = false;
	bool bImportedOmitted = false;
	const TArray<TSharedPtr<FJsonValue>>* Hierarchy = nullptr;
	if (TestTrue(TEXT("hierarchy"), Body->TryGetArrayField(TEXT("Hierarchy"), Hierarchy)))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Hierarchy)
		{
			const TSharedPtr<FJsonObject> Element = Value->AsObject();
			ElementTypes.Add(Element->GetStringField(TEXT("Type")));
			const TArray<TSharedPtr<FJsonValue>>* Parents = nullptr;
			if (Element->TryGetArrayField(TEXT("Parents"), Parents))
			{
				// A plain single parent (weight 1, no label) reads as "Parent"; the two-parent null keeps its label.
				const TSharedPtr<FJsonObject> First = Parents->Num() > 0 ? (*Parents)[0]->AsObject() : nullptr;
				double FirstWeight = 0.0;
				TestFalse(TEXT("a plain single parent is not listed"), Parents->Num() == 1 && First.IsValid() && !First->HasField(TEXT("Label"))
					&& First->TryGetNumberField(TEXT("Weight"), FirstWeight) && FirstWeight == 1.0);
				if (Element->GetStringField(TEXT("Name")) == TEXT("multi_null"))
				{
					const TSharedPtr<FJsonObject> Second = Parents->Num() == 2 ? (*Parents)[1]->AsObject() : nullptr;
					FString Label;
					double Weight = 0.0;
					bWeighted = Second.IsValid() && Second->TryGetStringField(TEXT("Label"), Label) && Label == TEXT("Pelvis")
						&& Second->TryGetNumberField(TEXT("Weight"), Weight) && FMath::IsNearlyEqual(Weight, 0.5)
						&& First.IsValid() && !First->HasField(TEXT("Label"));
				}
			}
			bSpaces |= Element->HasField(TEXT("Spaces"));
			bHosts |= Element->HasField(TEXT("Hosts"));
			bMetadata |= Element->HasField(TEXT("Metadata"));
			bool bImported = false;
			bImportedOmitted |= Element->TryGetBoolField(TEXT("Imported"), bImported) && bImported;
		}
	}
	for (const TCHAR* Type : {TEXT("Bone"), TEXT("Null"), TEXT("Control"), TEXT("Curve"), TEXT("Socket")})
	{
		TestTrue(*FString::Printf(TEXT("element type %s"), Type), ElementTypes.Contains(Type));
	}
	TestTrue(TEXT("multi-parent weights"), bWeighted);
	TestTrue(TEXT("control spaces"), bSpaces);
	TestTrue(TEXT("channel hosts"), bHosts);
	TestTrue(TEXT("metadata"), bMetadata);
	TestTrue(TEXT("imported bones at the source pose are marked Imported"), bImportedOmitted);

	// Inspect: pins equal the registry; facts.
	FRegistryPins Registry;
	TArray<FString> PinProblems;
	const int32 Compared = Registry.Compare(Body, Report.Inspected, PinProblems);
	TestTrue(TEXT("unit nodes compared"), Compared >= 5);
	for (const FString& Problem : PinProblems)
	{
		AddError(FString::Printf(TEXT("CR_AllKinds pins: %s"), *Problem));
	}
	if (Report.Inspected.IsValid())
	{
		TestTrue(TEXT("inspect lists events"), CountField(Report.Inspected, TEXT("Events")) >= 3);
		TestTrue(TEXT("inspect lists public functions"), CountField(Report.Inspected, TEXT("PublicFunctions")) == 1);
		TestTrue(TEXT("inspect lists shapes"), CountField(Report.Inspected, TEXT("Shapes")) > 0);
		TestTrue(TEXT("inspect global transforms"), CountField(Report.Inspected, TEXT("GlobalTransforms")) > 0);
		TestTrue(TEXT("inspect compile status"), Report.Inspected->HasTypedField<EJson::Object>(TEXT("Compile")));
	}
	// Modular rig.
	const FTarget& ModularTarget = Fixtures.Add(TEXT("CR_Modular"));
	if (MakeModular(*this, ModularTarget) != nullptr)
	{
		const FReadReport Modular = CheckStable(*this, ModularTarget, TEXT("CR_Modular"));
		for (const FString& Problem : Modular.Problems)
		{
			AddError(FString::Printf(TEXT("CR_Modular: %s"), *Problem));
		}
		AddInfo(FString::Printf(TEXT("CR_Modular: %s"), *Modular.Timings));
		AddInfo(SerializePretty(Modular.Extracted));
		const TSharedPtr<FJsonObject> ModularBody = BodyOf(Modular.Extracted);
		TestEqual(TEXT("modular rig type"), ModularBody->GetStringField(TEXT("RigType")), FString(TEXT("Modular")));
		TestEqual(TEXT("two modules"), CountField(ModularBody, TEXT("Modules")), 2);
		TestFalse(TEXT("a modular rig has no graphs"), ModularBody->HasField(TEXT("Graphs")));
		// A modular rig has no event graphs to author (its modules hold the graphs): Graphs is refused.
		CheckRefused(*this, ModularTarget, Modular.Extracted, TEXT("CR_Modular graphs"), [](const TSharedPtr<FJsonObject>& Edited)
		{
			Edited->SetObjectField(TEXT("Graphs"), Parse(TEXT(R"({"Rig": {"Nodes": {}}})")));
		}, TEXT("ControlRig.ModularRigRegionUnsupported"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigCorpusTest, "UEAssetDoc.ControlRig.Corpus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentControlRigCorpusTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentControlRigTests;
	const TUniquePtr<FTemplateContent> Templates = CopyCorpusTemplates();
	// MoverExamples' CR_Mannequin_Body has stale function references: the Blueprint compile on load and on
	// duplicate reports them (engine content, not this test).
	if (FPackageName::DoesPackageExist(TEXT("/MoverExamples/Characters/Mannequins/Rigs/CR_Mannequin_Body")))
	{
		AssetDocumentIKRigTests::ExpectStalePinErrors(*this, 0);
		AssetDocumentIKRigTests::ExpectUnmappedVariableErrors(*this, 0);
	}
	FRegistryPins Registry;
	int32 Tried = 0;
	int32 Passed = 0;
	for (const FControlRigCorpusEntry& Entry : ControlRigCorpus)
	{
		const FString Path = Entry.Path;
		const FString Name = FString(Entry.Source) + TEXT(":") + FPackageName::ObjectPathToObjectName(Path);
		// Content that is not there (a plugin that is not enabled, a template not installed): skip without loading.
		const FString Package = FPackageName::ObjectPathToPackageName(Path);
		UObject* Source = FPackageName::IsValidLongPackageName(Package) && FPackageName::DoesPackageExist(Package) ? LoadQuiet(Path) : nullptr;
		if (Source == nullptr)
		{
			AddInfo(FString::Printf(TEXT("SKIP %s: %s content is not available in this project"), *Name, Entry.Source));
			continue;
		}
		++Tried;
		const bool bShapeLibrary = Source->IsA<UControlRigShapeLibrary>();
		FFixtures Fixtures;
		const FTarget& Target = Fixtures.Add(bShapeLibrary ? TEXT("CRSL_Corpus") : TEXT("CR_Corpus"));
		const double DuplicateStart = FPlatformTime::Seconds();
		if (Duplicate(*this, Source, Target) == nullptr)
		{
			AddError(FString::Printf(TEXT("%s: duplicate failed"), *Name));
			continue;
		}
		const double DuplicateMs = Milliseconds(DuplicateStart);
		FReadReport Report = CheckStable(*this, Target, Name);
		const TSharedPtr<FJsonObject> Body = BodyOf(Report.Extracted);
		FString Counts;
		if (bShapeLibrary)
		{
			const TSharedPtr<FJsonObject> Facts = ObjectField(Report.Inspected, TEXT("ShapeLibrary"));
			if (!Facts.IsValid() || CountField(Facts, TEXT("Shapes")) == 0)
			{
				Report.Problems.Add(TEXT("Inspect has no ShapeLibrary facts"));
			}
			Counts = FString::Printf(TEXT("shapes %d"), Facts.IsValid() ? CountField(Facts, TEXT("Shapes")) : 0);
		}
		else
		{
			if (Report.Extracted.IsValid() && Report.Extracted->GetStringField(TEXT("Class")) != ControlRigClass)
			{
				Report.Problems.Add(FString::Printf(TEXT("class %s"), *Report.Extracted->GetStringField(TEXT("Class"))));
			}
			TArray<FString> PinProblems;
			const int32 Compared = Report.Inspected.IsValid() ? Registry.Compare(Body, Report.Inspected, PinProblems) : 0;
			for (int32 Index = 0; Index < PinProblems.Num() && Index < 8; ++Index)
			{
				Report.Problems.Add(FString::Printf(TEXT("pins: %s"), *PinProblems[Index]));
			}
			if (PinProblems.Num() > 8)
			{
				Report.Problems.Add(FString::Printf(TEXT("pins: %d more mismatches"), PinProblems.Num() - 8));
			}
			FString RigType = TEXT("Rig");
			Body->TryGetStringField(TEXT("RigType"), RigType);
			Counts = FString::Printf(TEXT("%s, graphs %d, functions %d, nodes %d, elements %d, variables %d, modules %d, unit pins compared %d"),
				*RigType, CountField(Body, TEXT("Graphs")), CountField(Body, TEXT("Functions")), AllNodes(Body).Num(),
				CountElements(Body), CountField(Body, TEXT("Variables")), CountField(Body, TEXT("Modules")), Compared);
		}
		for (const FString& Problem : Report.Problems)
		{
			AddError(FString::Printf(TEXT("%s: %s"), *Name, *Problem));
		}
		Passed += Report.Problems.IsEmpty() ? 1 : 0;
		AddInfo(FString::Printf(TEXT("%s %s (%s): duplicate %.0f ms, %s; %s"), Report.Problems.IsEmpty() ? TEXT("PASS") : TEXT("FAIL"),
			*Name, Entry.Why, DuplicateMs, *Report.Timings, *Counts));
	}
	AddInfo(FString::Printf(TEXT("corpus: %d of %d available assets stable"), Passed, Tried));
	TestTrue(TEXT("the default-enabled corpus is available"), Tried >= 10);
	return true;
}

namespace AssetDocumentControlRigTests
{
// The extract without what differs between a rig and its copy by design: the Target, the asset variant's guid.
FString DuplicateComparable(const TSharedPtr<FJsonObject>& Document)
{
	if (!Document.IsValid())
	{
		return FString();
	}
	const TSharedPtr<FJsonObject> Copy = Parse(WithoutTarget(Document));
	if (const TSharedPtr<FJsonObject> Variant = ObjectField(ObjectField(Copy, TEXT("Properties")), TEXT("AssetVariant")))
	{
		Variant->RemoveField(TEXT("Guid"));
	}
	return Serialize(Copy);
}

// The source's extract as its duplicate has it: the hierarchy and curve import skeletons are DuplicateTransient, so a
// copy (the editor's too) has none and lists its imported bones' transforms instead of leaving out the reference pose.
FString ExtractAsDuplicated(FAutomationTestBase& Test, UControlRigBlueprint& Source, const FString& Name)
{
	TSoftObjectPtr<UObject>& HierarchyImport = Source.GetSourceHierarchyImport();
	TSoftObjectPtr<UObject>& CurveImport = Source.GetSourceCurveImport();
	const TSoftObjectPtr<UObject> KeptHierarchyImport = HierarchyImport;
	const TSoftObjectPtr<UObject> KeptCurveImport = CurveImport;
	HierarchyImport.Reset();
	CurveImport.Reset();
	const FString Extracted = DuplicateComparable(Extract(Test, Source.GetPathName(), Name));
	HierarchyImport = KeptHierarchyImport;
	CurveImport = KeptCurveImport;
	return Extracted;
}

// The extract without the node order of its graphs. An apply adds the nodes a rig lacks after its existing ones (no
// editor command reorders nodes; ControlRig.OrderKept).
FString WithoutNodeOrder(const FString& Extracted)
{
	const TSharedPtr<FJsonObject> Copy = Parse(Extracted);
	if (const TSharedPtr<FJsonObject> Graphs = ObjectField(BodyOf(Copy), TEXT("Graphs")))
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Graph : Graphs->Values)
		{
			const TSharedPtr<FJsonObject>* GraphObject = nullptr;
			if (Graph.Value.IsValid() && Graph.Value->TryGetObject(GraphObject))
			{
				(*GraphObject)->RemoveField(TEXT("Order"));
			}
		}
	}
	return Serialize(Copy);
}

// Instructions of the rig's VM after the editor's compile (the package's dirty state kept).
int32 CompiledInstructions(UControlRigBlueprint& Blueprint)
{
	UPackage* Package = Blueprint.GetOutermost();
	const bool bWasDirty = Package->IsDirty();
	FKismetEditorUtilities::CompileBlueprint(&Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
	Package->SetDirtyFlag(bWasDirty);
	static const FObjectProperty* VMProperty = FindFProperty<FObjectProperty>(URigVMHost::StaticClass(), TEXT("VM"));
	const UObject* Defaults = Blueprint.GeneratedClass != nullptr ? Blueprint.GeneratedClass->GetDefaultObject(false) : nullptr;
	const URigVM* VM = VMProperty != nullptr && Defaults != nullptr ? Cast<URigVM>(VMProperty->GetObjectPropertyValue_InContainer(Defaults)) : nullptr;
	return VM != nullptr ? VM->GetByteCode().GetNumInstructions() : -1;
}
} // namespace AssetDocumentControlRigTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigDuplicateTest, "UEAssetDoc.ControlRig.Duplicate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// The duplicate operation's copy of a rig is the rig: it extracts as the source (minus Target, the asset variant guid
// and the import skeletons, which no copy keeps) before and after a reload and compiles to as many instructions, for CR_AllKinds (a reroute between a variable
// getter and an If) and AnimatorKit's CRU_AddLocator (a rig whose post-load skips the graph refresh). The Content
// Browser's own Duplicate of the same rigs runs too, as the reference: on engines whose RigVM post-load relies on the
// graphs being flagged for PostLoad, its in-memory copy of CR_AllKinds drops the reroute and logs "Cannot break link"
// for the links through it (expected only for that copy's package), and its copy of CRU_AddLocator compiles without
// its links until it is reloaded (reported, not asserted).
bool FAssetDocumentControlRigDuplicateTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentControlRigTests;
	FFixtures Fixtures;
	TArray<TPair<FString, UControlRigBlueprint*>> Sources;
	const FTarget& AllKinds = Fixtures.Add(TEXT("CR_AllKinds"));
	if (UControlRigBlueprint* Made = MakeAllKinds(*this, AllKinds))
	{
		Sources.Emplace(TEXT("CR_AllKinds"), Made);
	}
	const FString Locator = TEXT("/AnimatorKit/UtilityRigs/CRU_AddLocator.CRU_AddLocator");
	if (FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Locator)))
	{
		if (UControlRigBlueprint* Loaded = Cast<UControlRigBlueprint>(LoadQuiet(Locator)))
		{
			Sources.Emplace(TEXT("CRU_AddLocator"), Loaded);
		}
	}
	else
	{
		AddInfo(TEXT("SKIP CRU_AddLocator: AnimatorKit content is not available in this project"));
	}
	for (const TPair<FString, UControlRigBlueprint*>& Source : Sources)
	{
		const FString& Name = Source.Key;
		const FString Expected = ExtractAsDuplicated(*this, *Source.Value, Name);
		const int32 ExpectedInstructions = CompiledInstructions(*Source.Value);

		const FTarget& Copy = Fixtures.Add(TEXT("CR_Duplicate"));
		UControlRigBlueprint* Duplicated = Cast<UControlRigBlueprint>(Duplicate(*this, Source.Value, Copy));
		if (!TestNotNull(*FString::Printf(TEXT("%s: duplicate"), *Name), Duplicated))
		{
			continue;
		}
		const FString Got = DuplicateComparable(Extract(*this, Copy.ObjectPath, Name + TEXT(" copy")));
		if (!TestEqual(*FString::Printf(TEXT("%s: the copy extracts as the source"), *Name), Got, Expected))
		{
			AddInfo(FirstDifference(Expected, Got));
		}
		TestEqual(*FString::Printf(TEXT("%s: the copy compiles to the source's instructions"), *Name), CompiledInstructions(*Duplicated),
			ExpectedInstructions);
		UControlRigBlueprint* Reloaded = Cast<UControlRigBlueprint>(Reload(Copy));
		if (TestNotNull(*FString::Printf(TEXT("%s: the copy reloads"), *Name), Reloaded))
		{
			const FString AfterReload = DuplicateComparable(Extract(*this, Copy.ObjectPath, Name + TEXT(" copy after reload")));
			if (!TestEqual(*FString::Printf(TEXT("%s: the copy extracts the same after a reload"), *Name), AfterReload, Got))
			{
				AddInfo(FirstDifference(Got, AfterReload));
			}
			TestEqual(*FString::Printf(TEXT("%s: the reloaded copy compiles to the source's instructions"), *Name),
				CompiledInstructions(*Reloaded), ExpectedInstructions);
		}

		// The reference: the Content Browser's Duplicate.
		const FTarget& EditorCopy = Fixtures.Add(TEXT("CR_EditorDuplicate"));
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 3)
		if (Name == TEXT("CR_AllKinds"))
		{
			AddExpectedMessage(FString::Printf(TEXT("Cannot break link .+ in package %s"), *EditorCopy.PackageName), ELogVerbosity::Error,
				EAutomationExpectedMessageFlags::Contains, 0);
		}
#endif
		UControlRigBlueprint* EditorDuplicated = Cast<UControlRigBlueprint>(EditorDuplicate(*this, Source.Value, EditorCopy));
		if (EditorDuplicated == nullptr)
		{
			continue;
		}
		if (UControlRigBlueprint* Ours = Cast<UControlRigBlueprint>(Copy.Get()))
		{
			TestEqual(*FString::Printf(TEXT("%s: the copies keep the same import skeleton"), *Name),
				Ours->GetSourceHierarchyImport().ToString(), EditorDuplicated->GetSourceHierarchyImport().ToString());
		}
		const TSharedPtr<FJsonObject> EditorExtract = Extract(*this, EditorCopy.ObjectPath, Name + TEXT(" editor copy"));
		const FString EditorGot = DuplicateComparable(EditorExtract);
		AddInfo(FString::Printf(TEXT("%s: the Content Browser's copy extracts as the source: %d, compiles to %d of %d instructions%s"), *Name,
			EditorGot == Expected, CompiledInstructions(*EditorDuplicated), ExpectedInstructions,
			EditorGot == Expected ? TEXT("") : *(TEXT("; ") + FirstDifference(Expected, EditorGot))));
		// The known issue's repair of such a copy: the copy's extract with the source's graphs and functions, applied.
		if (Name == TEXT("CR_AllKinds") && EditorExtract.IsValid())
		{
			const TSharedPtr<FJsonObject> SourceBody = BodyOf(Extract(*this, Source.Value->GetPathName(), Name));
			const TSharedPtr<FJsonObject> Repair = Parse(Serialize(EditorExtract));
			Repair->SetStringField(TEXT("Action"), TEXT("Update"));
			for (const TCHAR* Region : {TEXT("Graphs"), TEXT("GraphOrder"), TEXT("Functions")})
			{
				if (const TSharedPtr<FJsonValue> Value = SourceBody->TryGetField(Region))
				{
					BodyOf(Repair)->SetField(Region, Value);
				}
			}
			const FAssetDocumentResult Repaired = Apply(EditorCopy, Repair);
			TestTrue(*FString::Printf(TEXT("%s: the repair of the Content Browser's copy applies (%s)"), *Name, *DiagnosticsText(Repaired)),
				Repaired.IsSuccess());
			const FString AfterRepair = WithoutNodeOrder(
				DuplicateComparable(Extract(*this, EditorCopy.ObjectPath, Name + TEXT(" repaired editor copy"))));
			const FString ExpectedUnordered = WithoutNodeOrder(Expected);
			if (!TestEqual(*FString::Printf(TEXT("%s: the repaired copy extracts as the source (node order aside)"), *Name), AfterRepair,
				ExpectedUnordered))
			{
				AddInfo(FirstDifference(ExpectedUnordered, AfterRepair));
			}
			if (UControlRigBlueprint* RepairedRig = Cast<UControlRigBlueprint>(EditorCopy.Get()))
			{
				TestEqual(*FString::Printf(TEXT("%s: the repaired copy compiles to the source's instructions"), *Name),
					CompiledInstructions(*RepairedRig), ExpectedInstructions);
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigListTypesTest, "UEAssetDoc.ControlRig.ListTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentControlRigListTypesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentControlRigTests;
	const FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	auto List = [&](const TMap<FString, FString>& Query, const TCHAR* Stage, int32& OutTotal) -> TArray<TSharedPtr<FJsonValue>>
	{
		const FAssetDocumentResult Result = Service.ListTypes(Query);
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		OutTotal = 0;
		if (!TestTrue(*FString::Printf(TEXT("%s: lists"), Stage), Result.IsSuccess() && Result.Payload.IsValid() && Result.Payload->TryGetArrayField(TEXT("Types"), Rows)))
		{
			AddInfo(DiagnosticsText(Result));
			return {};
		}
		OutTotal = static_cast<int32>(Result.Payload->GetNumberField(TEXT("Total")));
		return *Rows;
	};
	auto Find = [](const TArray<TSharedPtr<FJsonValue>>& Rows, const FString& Name) -> TSharedPtr<FJsonObject>
	{
		for (const TSharedPtr<FJsonValue>& Row : Rows)
		{
			FString RowName;
			FString RowPath;
			Row->AsObject()->TryGetStringField(TEXT("Name"), RowName);
			Row->AsObject()->TryGetStringField(TEXT("Path"), RowPath);
			if (RowName == Name || RowPath == Name)
			{
				return Row->AsObject();
			}
		}
		return nullptr;
	};
	int32 Total = 0;

	// Units and events.
	const TArray<TSharedPtr<FJsonValue>> SetTransform = List({{TEXT("kind"), TEXT("rigvm_node")}, {TEXT("name"), TEXT("RigUnit_SetTransform")}}, TEXT("set transform"), Total);
	if (const TSharedPtr<FJsonObject> Row = Find(SetTransform, TEXT("RigUnit_SetTransform")))
	{
		AddInfo(Serialize(Row));
		TestEqual(TEXT("unit path"), Row->GetStringField(TEXT("Path")), FString(TEXT("/Script/ControlRig.RigUnit_SetTransform")));
		TestEqual(TEXT("unit kind"), Row->GetStringField(TEXT("Kind")), FString(TEXT("Unit")));
		TSet<FString> Pins;
		for (const TSharedPtr<FJsonValue>& Pin : Row->GetArrayField(TEXT("Pins")))
		{
			Pins.Add(Pin->AsObject()->GetStringField(TEXT("Name")));
		}
		for (const TCHAR* Pin : {TEXT("ExecutePin"), TEXT("Item"), TEXT("Space"), TEXT("Value"), TEXT("Weight")})
		{
			TestTrue(*FString::Printf(TEXT("SetTransform pin %s"), Pin), Pins.Contains(Pin));
		}
		for (const TSharedPtr<FJsonValue>& Pin : Row->GetArrayField(TEXT("Pins")))
		{
			const TSharedPtr<FJsonObject> Item = Pin->AsObject();
			if (Item->GetStringField(TEXT("Name")) == TEXT("CachedIndex"))
			{
				TestEqual(TEXT("cache pin is hidden (as on the node)"), Item->GetStringField(TEXT("Direction")), FString(TEXT("Hidden")));
			}
			if (Item->GetStringField(TEXT("Name")) == TEXT("ExecutePin"))
			{
				TestEqual(TEXT("execute pin type (as on the node)"), Item->GetStringField(TEXT("Type")), FString(TEXT("/Script/RigVM.RigVMExecuteContext")));
			}
		}
		TestEqual(TEXT("execute pin first (base struct fields first)"), Row->GetArrayField(TEXT("Pins"))[0]->AsObject()->GetStringField(TEXT("Name")), FString(TEXT("ExecutePin")));
	}
	else
	{
		AddError(TEXT("RigUnit_SetTransform is not listed"));
	}
	const TArray<TSharedPtr<FJsonValue>> Begin = List({{TEXT("kind"), TEXT("rigvm_node")}, {TEXT("name"), TEXT("RigUnit_BeginExecution")}}, TEXT("forwards solve"), Total);
	if (const TSharedPtr<FJsonObject> Row = Find(Begin, TEXT("RigUnit_BeginExecution")))
	{
		TestEqual(TEXT("Forwards Solve event name"), Row->GetStringField(TEXT("Event")), FString(TEXT("Forwards Solve")));
		bool bOnce = false;
		TestTrue(TEXT("Forwards Solve can only exist once"), Row->TryGetBoolField(TEXT("CanOnlyExistOnce"), bOnce) && bOnce);
	}
	else
	{
		AddError(TEXT("RigUnit_BeginExecution is not listed"));
	}
	const TArray<TSharedPtr<FJsonValue>> Dispatch = List({{TEXT("kind"), TEXT("rigvm_node")}, {TEXT("name"), TEXT("DISPATCH_RigVMDispatch_If")}}, TEXT("dispatch"), Total);
	if (const TSharedPtr<FJsonObject> Row = Find(Dispatch, TEXT("DISPATCH_RigVMDispatch_If")))
	{
		AddInfo(Serialize(Row));
		TestEqual(TEXT("dispatch kind"), Row->GetStringField(TEXT("Kind")), FString(TEXT("Dispatch")));
		TestTrue(TEXT("dispatch permutations"), Row->GetNumberField(TEXT("Permutations")) > 1);
	}
	else
	{
		AddError(TEXT("DISPATCH_RigVMDispatch_If is not listed"));
	}
	UScriptStruct* AddStruct = FindObject<UScriptStruct>(nullptr, TEXT("/Script/RigVM.RigVMFunction_MathFloatAdd"));
	const FRigVMFunction* AddFunction = AddStruct != nullptr ? FRigVMRegistry::Get().FindFunction(AddStruct, TEXT("Execute")) : nullptr;
	const FRigVMTemplate* AddTemplate = AddFunction != nullptr ? AddFunction->GetTemplate() : nullptr;
	if (TestNotNull(TEXT("Add template"), AddTemplate))
	{
		const FString Notation = AddTemplate->GetNotation().ToString();
		const TSharedPtr<FJsonObject> Row = Find(List({{TEXT("kind"), TEXT("rigvm_node")}, {TEXT("name"), Notation}, {TEXT("limit"), TEXT("500")}}, TEXT("template"), Total), Notation);
		if (TestNotNull(*FString::Printf(TEXT("template %s is listed"), *Notation), Row.Get()))
		{
			AddInfo(Serialize(Row));
			TestEqual(TEXT("template kind"), Row->GetStringField(TEXT("Kind")), FString(TEXT("Template")));
		}
	}
	int32 AllNodes = 0;
	List({{TEXT("kind"), TEXT("rigvm_node")}, {TEXT("limit"), TEXT("1")}}, TEXT("all nodes"), AllNodes);
	AddInfo(FString::Printf(TEXT("rigvm_node: %d rows"), AllNodes));
	TestTrue(TEXT("hundreds of node types"), AllNodes > 300);

	// Value types.
	int32 AllTypes = 0;
	List({{TEXT("kind"), TEXT("rigvm_type")}, {TEXT("limit"), TEXT("1")}}, TEXT("types"), AllTypes);
	AddInfo(FString::Printf(TEXT("rigvm_type: %d rows"), AllTypes));
	for (const TCHAR* Name : {TEXT("float"), TEXT("bool"), TEXT("int32"), TEXT("FName"), TEXT("/Script/CoreUObject.Vector"),
		TEXT("/Script/CoreUObject.Transform"), TEXT("/Script/ControlRig.RigElementKey")})
	{
		const TSharedPtr<FJsonObject> Row = Find(List({{TEXT("kind"), TEXT("rigvm_type")}, {TEXT("name"), Name}, {TEXT("limit"), TEXT("500")}}, Name, Total), Name);
		TestNotNull(*FString::Printf(TEXT("value type %s"), Name), Row.Get());
		if (Row.IsValid() && FString(Name).StartsWith(TEXT("/")))
		{
			TestEqual(*FString::Printf(TEXT("%s is a struct type"), Name), Row->GetStringField(TEXT("ObjectKind")), FString(TEXT("Struct")));
		}
	}
	List({{TEXT("kind"), TEXT("rigvm_type")}, {TEXT("name"), TEXT("ExecuteContext")}}, TEXT("execute contexts"), Total);
	TestEqual(TEXT("execute contexts are not value types"), Total, 0);

	// Modules.
	const TArray<TSharedPtr<FJsonValue>> Modules = List({{TEXT("kind"), TEXT("controlrig_module")}, {TEXT("limit"), TEXT("500")}}, TEXT("modules"), Total);
	AddInfo(FString::Printf(TEXT("controlrig_module: %d rows"), Total));
	TSharedPtr<FJsonObject> Arm;
	for (const TSharedPtr<FJsonValue>& Row : Modules)
	{
		if (Row->AsObject()->GetStringField(TEXT("Path")) == TEXT("/ControlRigModules/Modules56/Arm.Arm"))
		{
			Arm = Row->AsObject();
		}
	}
	if (TestNotNull(TEXT("Arm module listed"), Arm.Get()))
	{
		AddInfo(Serialize(Arm));
		TestEqual(TEXT("module class"), Arm->GetStringField(TEXT("Class")), FString(TEXT("/ControlRigModules/Modules56/Arm.Arm_C")));
		TestTrue(TEXT("module connectors"), CountField(Arm, TEXT("Connectors")) >= 1);
	}
	TestTrue(TEXT("Root module listed"), Modules.ContainsByPredicate([](const TSharedPtr<FJsonValue>& Row)
	{
		return Row->AsObject()->GetStringField(TEXT("Path")) == TEXT("/ControlRig/Modules/Modules56/Root.Root");
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigStatusTest, "UEAssetDoc.ControlRig.Status",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentControlRigStatusTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentControlRigTests;
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("CR_Status"));
	UControlRigBlueprint* Created = MakeControlRig(*this, Target, TutorialMesh, false);
	if (Created == nullptr)
	{
		return false;
	}
	{
		URigVMBlueprint& RigVM = *Created;
		URigVMController* Rig = RigVM.GetOrCreateController(RigVM.GetDefaultModel());
		AddUnit(*this, *Rig, TEXT("/Script/ControlRig.RigUnit_BeginExecution"), TEXT("ForwardsSolve"), 0, 0);
		AddUnit(*this, *Rig, TEXT("/Script/ControlRig.RigUnit_SetTransform"), TEXT("SetHead"), 300, 0);
		Link(*this, *Rig, TEXT("ForwardsSolve.ExecutePin"), TEXT("SetHead.ExecutePin"));
		FKismetEditorUtilities::CompileBlueprint(Created);
		Save(*Created);
	}
	TWeakObjectPtr<UControlRigBlueprint> Blueprint = Created;

	// The last compile: status, VM hash, instructions.
	const FAssetDocumentResult Compiled = ExtractResult(Target.ObjectPath);
	double Hash = 0.0;
	if (TestTrue(TEXT("status after compile"), Compiled.Compile.IsValid()))
	{
		AddInfo(Serialize(Compiled.Compile));
		const FString Status = Compiled.Compile->GetStringField(TEXT("Status"));
		TestTrue(TEXT("compiled"), Status == TEXT("UpToDate") || Status == TEXT("UpToDateWithWarnings"));
		TestTrue(TEXT("VM hash"), Compiled.Compile->TryGetNumberField(TEXT("VMHash"), Hash));
		double Instructions = 0.0;
		TestTrue(TEXT("instructions"), Compiled.Compile->TryGetNumberField(TEXT("Instructions"), Instructions) && Instructions > 0.0);
	}

	// Reading never compiles: a Dirty status reads Dirty and stays Dirty.
	if (!TestTrue(TEXT("blueprint alive"), Blueprint.IsValid()))
	{
		return false;
	}
	const EBlueprintStatus Before = Blueprint->Status;
	Blueprint->Status = BS_Dirty;
	const FAssetDocumentResult Dirty = ExtractResult(Target.ObjectPath);
	TestTrue(TEXT("extract of a dirty-status rig"), Dirty.IsSuccess());
	TestTrue(TEXT("status Dirty is reported"), Dirty.Compile.IsValid() && Dirty.Compile->GetStringField(TEXT("Status")) == TEXT("Dirty"));
	if (TestTrue(TEXT("blueprint alive after read"), Blueprint.IsValid()))
	{
		TestEqual(TEXT("reading did not compile"), static_cast<int32>(Blueprint->Status), static_cast<int32>(BS_Dirty));
		double DirtyHash = 0.0;
		TestTrue(TEXT("VM untouched"), Dirty.Compile.IsValid() && Dirty.Compile->TryGetNumberField(TEXT("VMHash"), DirtyHash) && DirtyHash == Hash);
		Blueprint->Status = Before;
	}

	// A compiler message on a node is reported at the node's document path.
	URigVMEdGraphNode* Marked = nullptr;
	if (Blueprint.IsValid())
	{
		// A rig made headlessly has empty editor graphs. Opening the Control Rig editor binds each editor graph to the
		// asset (FRigVMEditorBase::InitRigVMEditor: InitializeFromAsset) and its refresh rebuilds them from the model.
		TArray<UEdGraph*> EditorGraphs;
		Blueprint->GetAllGraphs(EditorGraphs);
		for (UEdGraph* Graph : EditorGraphs)
		{
			if (URigVMEdGraph* RigGraph = Cast<URigVMEdGraph>(Graph))
			{
				RigGraph->InitializeFromAsset(FRigVMAssetInterfacePtr(Blueprint.Get()));
			}
		}
		static_cast<URigVMBlueprint*>(Blueprint.Get())->OnRigVMRegistryChanged();
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			if (Graph == nullptr)
			{
				continue;
			}
			URigVMEdGraph* RigGraph = Cast<URigVMEdGraph>(Graph);
			URigVMEdGraphNode* RigNode = RigGraph != nullptr ? Cast<URigVMEdGraphNode>(RigGraph->FindNodeForModelNodeName(TEXT("SetHead"))) : nullptr;
			if (RigNode != nullptr && Marked == nullptr)
			{
				Marked = RigNode;
			}
			TArray<FString> Names;
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				const URigVMEdGraphNode* Each = Cast<URigVMEdGraphNode>(Node);
				Names.Add(Each != nullptr ? FString::Printf(TEXT("%s->%s"), *Each->GetName(), *Each->GetModelNodePath()) : GetNameSafe(Node));
			}
			AddInfo(FString::Printf(TEXT("EdGraph %s (%s): %s"), *Graph->GetName(), *GetNameSafe(Graph->GetClass()), *FString::Join(Names, TEXT(", "))));
		}
	}
	if (TestNotNull(TEXT("SetHead has an editor node"), Marked))
	{
		Marked->bHasCompilerMessage = true;
		Marked->ErrorType = EMessageSeverity::Warning;
		Marked->ErrorMsg = TEXT("AssetDocument test warning");
		const FAssetDocumentResult Warned = ExtractResult(Target.ObjectPath);
		bool bFound = false;
		if (Warned.Compile.IsValid())
		{
			AddInfo(Serialize(Warned.Compile));
			for (const TSharedPtr<FJsonValue>& Message : Warned.Compile->GetArrayField(TEXT("Messages")))
			{
				const TSharedPtr<FJsonObject> Row = Message->AsObject();
				FString Path;
				if (Row->GetStringField(TEXT("Message")) == TEXT("AssetDocument test warning") && Row->TryGetStringField(TEXT("Path"), Path))
				{
					bFound = true;
					TestEqual(TEXT("message path"), Path, FString(TEXT("/Body/Graphs/Rig/Nodes/SetHead")));
					TestEqual(TEXT("message severity"), Row->GetStringField(TEXT("Severity")), FString(TEXT("Warning")));
					TestEqual(TEXT("message graph"), Row->GetStringField(TEXT("Graph")), FString(TEXT("Rig")));
				}
			}
		}
		TestTrue(TEXT("the node message is reported"), bFound);
		Marked->bHasCompilerMessage = false;
		Marked->ErrorMsg.Empty();
	}
	return true;
}

#endif
