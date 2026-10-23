// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// rig.status.v1 and the "Compile" describer of IK Rigs and IK Retargeters (design §5.1): the processor
// initializations behind the editors' output log, run headlessly.
//   IK Rig:      FIKRigProcessor::Initialize(Rig, PreviewMesh) (errors / warnings of its logger) plus each initialized
//                solver's GetWarningMessage (the warning icon of the solver stack, SIKRigSolverStackItem).
//   Retargeter:  FIKRetargetProcessor::Initialize(SourceMesh, TargetMesh, Retargeter, asset profile) as the
//                retarget anim node does, plus every op the processor could not initialize (the op stack's
//                "Not initialized" line, SRetargetOpSingleItem::GetWarningMessage).
// The retarget processor writes editor-instance pointers back onto the asset's ops while it copies them
// (FIKRetargetProcessor::InitialOpStackSetup), so it runs on a transient duplicate, never on the asset.
// Status: UpToDate, UpToDateWithWarnings, Error, or Unknown when the preview mesh(es) are not set (nothing can be
// initialized, as in the editor). Messages carry a document path when they name a goal, chain, solver or op.

#include "AssetDocumentRigPrimitives.h"

#include "AssetDocumentGraphValue.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/EngineVersionComparison.h"
#include "Retargeter/IKRetargetOps.h"
#include "Retargeter/IKRetargetProcessor.h"
#include "Retargeter/IKRetargetProfile.h"
#include "Retargeter/IKRetargeter.h"
#include "Rig/IKRigDefinition.h"
#include "Rig/IKRigProcessor.h"
#include "Rig/Solvers/IKRigSolverBase.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Package.h"

namespace UE::AssetDocument::Graph::Rig
{
namespace
{
struct FStatusMessage
{
	FString Severity;
	FString Message;
	FString Path;
	FString Source;
};

// Single-quoted names in a processor message ("IK Goal, 'LeftHandIK' is not connected ...").
TArray<FString> QuotedNames(const FString& Message)
{
	TArray<FString> Names;
	int32 Start = INDEX_NONE;
	for (int32 Index = 0; Index < Message.Len(); ++Index)
	{
		if (Message[Index] != TCHAR('\''))
		{
			continue;
		}
		if (Start == INDEX_NONE)
		{
			Start = Index + 1;
		}
		else
		{
			Names.Add(Message.Mid(Start, Index - Start));
			Start = INDEX_NONE;
		}
	}
	return Names;
}

FString PathByNames(const FString& Message, const TMap<FString, FString>& Paths)
{
	for (const FString& Name : QuotedNames(Message))
	{
		if (const FString* Path = Paths.Find(Name))
		{
			return *Path;
		}
	}
	return FString();
}

void AddLog(const FIKRigLogger& Log, const TMap<FString, FString>& Paths, TArray<FStatusMessage>& OutMessages)
{
	for (const FText& Error : Log.GetErrors())
	{
		const FString Text = Error.ToString();
		OutMessages.Add({TEXT("Error"), Text, PathByNames(Text, Paths), TEXT("Processor")});
	}
	for (const FText& Warning : Log.GetWarnings())
	{
		const FString Text = Warning.ToString();
		OutMessages.Add({TEXT("Warning"), Text, PathByNames(Text, Paths), TEXT("Processor")});
	}
}

bool DescribeIKRig(const UIKRigDefinition& Rig, FString& OutStatus, TArray<FStatusMessage>& OutMessages)
{
	const USkeletalMesh* Mesh = Rig.GetPreviewMesh();
	if (Mesh == nullptr)
	{
		OutStatus = TEXT("Unknown");
		return true;
	}
	TMap<FString, FString> Paths;
	for (const UIKRigEffectorGoal* Goal : Rig.GetGoalArray())
	{
		if (Goal != nullptr)
		{
			Paths.Add(Goal->GoalName.ToString(), TEXT("/Body/Goals/") + Goal->GoalName.ToString());
		}
	}
	for (const FBoneChain& Chain : Rig.GetRetargetChains())
	{
		Paths.FindOrAdd(Chain.ChainName.ToString(), TEXT("/Body/Chains/") + Chain.ChainName.ToString());
	}

	FIKRigProcessor Processor;
	Processor.Initialize(&Rig, Mesh, FIKRigGoalContainer());
	AddLog(Processor.Log, Paths, OutMessages);
	if (Processor.IsInitialized())
	{
		for (int32 Index = 0; Index < Rig.GetSolverStructs().Num(); ++Index)
		{
			FText Warning;
			const FIKRigSolverBase* Solver = Processor.GetSolver(Index);
			if (Solver != nullptr && Solver->GetWarningMessage(Warning))
			{
				OutMessages.Add({TEXT("Warning"), Warning.ToString(), FString::Printf(TEXT("/Body/Solvers/%d"), Index), TEXT("Solver")});
			}
		}
	}
	return true;
}

// Document paths of the ops: top-level ops by stack position, child ops (the stack keeps them before their parent)
// under their parent's Children in stack order. A child whose parent is missing stays top-level.
TMap<FString, FString> OpPaths(const UIKRetargeter& Retargeter)
{
	TSet<FName> Names;
	for (const FInstancedStruct& OpStruct : Retargeter.GetRetargetOps())
	{
		if (const FIKRetargetOpBase* Op = OpStruct.GetPtr<FIKRetargetOpBase>())
		{
			Names.Add(Op->GetName());
		}
	}
	auto IsChild = [&Names](const FIKRetargetOpBase& Op)
	{
		return !Op.GetParentOpName().IsNone() && Op.GetParentOpName() != Op.GetName() && Names.Contains(Op.GetParentOpName());
	};
	TMap<FString, FString> Paths;
	int32 TopLevel = 0;
	for (const FInstancedStruct& OpStruct : Retargeter.GetRetargetOps())
	{
		const FIKRetargetOpBase* Op = OpStruct.GetPtr<FIKRetargetOpBase>();
		if (Op != nullptr && !IsChild(*Op))
		{
			Paths.Add(Op->GetName().ToString(), FString::Printf(TEXT("/Body/Ops/%d"), TopLevel++));
		}
	}
	TMap<FName, int32> ChildCount;
	for (const FInstancedStruct& OpStruct : Retargeter.GetRetargetOps())
	{
		const FIKRetargetOpBase* Op = OpStruct.GetPtr<FIKRetargetOpBase>();
		if (Op == nullptr || !IsChild(*Op))
		{
			continue;
		}
		const FString* ParentPath = Paths.Find(Op->GetParentOpName().ToString());
		if (ParentPath == nullptr)
		{
			continue;
		}
		const FString ParentPathCopy = *ParentPath;
		int32& Child = ChildCount.FindOrAdd(Op->GetParentOpName());
		Paths.Add(Op->GetName().ToString(), FString::Printf(TEXT("%s/Children/%d"), *ParentPathCopy, Child));
		++Child;
	}
	return Paths;
}

bool DescribeRetargeter(const UIKRetargeter& Retargeter, FString& OutStatus, TArray<FStatusMessage>& OutMessages)
{
	const USkeletalMesh* SourceMesh = Retargeter.GetPreviewMesh(ERetargetSourceOrTarget::Source);
	const USkeletalMesh* TargetMesh = Retargeter.GetPreviewMesh(ERetargetSourceOrTarget::Target);
	if (SourceMesh == nullptr || TargetMesh == nullptr)
	{
		OutStatus = TEXT("Unknown");
		return true;
	}
	UIKRetargeter* Copy = DuplicateObject<UIKRetargeter>(&Retargeter, GetTransientPackage(), NAME_None);
	if (Copy == nullptr)
	{
		return false;
	}
	Copy->ClearFlags(RF_Public | RF_Standalone);
	const TMap<FString, FString> Paths = OpPaths(*Copy);
	{
		FIKRetargetProcessor Processor;
		FRetargetProfile Profile;
		Profile.FillProfileWithAssetSettings(Copy);
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
		FRetargetInitParameters InitParameters;
		InitParameters.SourceSkeletalMesh = SourceMesh;
		InitParameters.TargetSkeletalMesh = TargetMesh;
		InitParameters.RetargeterAsset = Copy;
		InitParameters.CustomProfile = &Profile;
		Processor.Initialize(InitParameters);
#else
		Processor.Initialize(SourceMesh, TargetMesh, Copy, Profile, false);
#endif
		AddLog(Processor.Log, Paths, OutMessages);
		if (Processor.IsInitialized())
		{
			for (const FInstancedStruct& OpStruct : Processor.GetRetargetOps())
			{
				const FIKRetargetOpBase* Op = OpStruct.GetPtr<FIKRetargetOpBase>();
				if (Op != nullptr && !Op->IsInitialized())
				{
					const FString* Path = Paths.Find(Op->GetName().ToString());
					OutMessages.Add({TEXT("Warning"), FString::Printf(TEXT("Op '%s': %s"), *Op->GetName().ToString(), *Op->GetWarningMessage().ToString()),
						Path != nullptr ? *Path : FString(), TEXT("Op")});
				}
			}
		}
	}
	Copy->MarkAsGarbage();
	return true;
}

bool Describe(const UObject& Asset, FString& OutStatus, TArray<FStatusMessage>& OutMessages)
{
	if (const UIKRigDefinition* Rig = Cast<UIKRigDefinition>(&Asset))
	{
		return DescribeIKRig(*Rig, OutStatus, OutMessages);
	}
	if (const UIKRetargeter* Retargeter = Cast<UIKRetargeter>(&Asset))
	{
		return DescribeRetargeter(*Retargeter, OutStatus, OutMessages);
	}
	return false;
}

FString StatusOf(const FString& Initial, const TArray<FStatusMessage>& Messages)
{
	if (!Initial.IsEmpty())
	{
		return Initial;
	}
	bool bWarning = false;
	for (const FStatusMessage& Message : Messages)
	{
		if (Message.Severity == TEXT("Error"))
		{
			return TEXT("Error");
		}
		bWarning |= Message.Severity == TEXT("Warning");
	}
	return bWarning ? TEXT("UpToDateWithWarnings") : TEXT("UpToDate");
}
} // namespace

bool DescribeStatusValue(const UObject& Asset, FValue& OutStatus)
{
	FString Status;
	TArray<FStatusMessage> Messages;
	if (!Describe(Asset, Status, Messages))
	{
		return false;
	}
	TArray<FValue> Rows;
	for (const FStatusMessage& Message : Messages)
	{
		FObjectBuilder Row;
		Row.AddString(TEXT("Severity"), Message.Severity);
		Row.AddString(TEXT("Message"), Message.Message);
		if (!Message.Path.IsEmpty())
		{
			Row.AddString(TEXT("Path"), Message.Path);
		}
		Row.AddString(TEXT("Source"), Message.Source);
		Rows.Add(Row.Build());
	}
	FObjectBuilder Object;
	Object.AddString(TEXT("Status"), StatusOf(Status, Messages));
	Object.Add(TEXT("Messages"), MakeArray(MoveTemp(Rows)));
	OutStatus = Object.Build();
	return true;
}

TSharedPtr<FJsonObject> DescribeStatus(const UObject& Asset)
{
	FValue Status;
	if (!DescribeStatusValue(Asset, Status))
	{
		return nullptr;
	}
	const TSharedPtr<FJsonValue> Json = ValueToJson(Status);
	return Json.IsValid() ? Json->AsObject() : nullptr;
}

bool HandleStatus(FCall& Call, FObjectBuilder& Result)
{
	if (!Call.Args.OnlyFields({}))
	{
		return false;
	}
	FValue Status;
	if (!DescribeStatusValue(Call.Asset, Status))
	{
		return Fail(Call, TEXT("IKRig.UnsupportedAsset"), TEXT("rig.status.v1 reads IK Rig and IK Retargeter assets."), TEXT("/HandleArguments/asset"));
	}
	Result.Add(TEXT("compile"), MoveTemp(Status));
	return true;
}
} // namespace UE::AssetDocument::Graph::Rig
