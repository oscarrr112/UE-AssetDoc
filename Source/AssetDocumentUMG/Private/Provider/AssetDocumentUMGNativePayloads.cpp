// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/AssetDocumentUMGNativePayloads.h"

#include "Animation/WidgetAnimation.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Blueprint/UserWidget.h"
#include "WidgetBlueprint.h"
#include "Components/Widget.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"

namespace AssetDocumentUMGNativePayloadsPrivate
{
const FName UObjectPayloadType(TEXT("AssetDocument.UMG.UObject.v1"));
const FName ChannelPayloadType(TEXT("AssetDocument.UMG.FloatChannel.v1"));

class FObjectPayload final : public IAssetDocumentNativeHandlePayload
{
public:
	explicit FObjectPayload(UObject* InObject)
		: Object(InObject)
	{
	}

	virtual FName GetPayloadTypeId() const override
	{
		return UObjectPayloadType;
	}

	virtual bool IsAlive() const override
	{
		return Object.IsValid();
	}

	UObject* Get() const
	{
		return Object.Get();
	}

private:
	TWeakObjectPtr<UObject> Object;
};

class FChannelPayload final : public IAssetDocumentNativeHandlePayload
{
public:
	FChannelPayload(
		UMovieSceneSection* InOwningSection,
		FMovieSceneFloatChannel* InChannel,
		FName InChannelName)
		: OwningSection(InOwningSection)
		, Channel(InChannel)
		, ChannelName(InChannelName)
	{
	}

	virtual FName GetPayloadTypeId() const override
	{
		return ChannelPayloadType;
	}

	virtual bool IsAlive() const override
	{
		return OwningSection.IsValid() && Channel != nullptr;
	}

	UMovieSceneSection* GetOwningSection() const
	{
		return OwningSection.Get();
	}

	FMovieSceneFloatChannel* GetChannel() const
	{
		return Channel;
	}

	FName GetChannelName() const
	{
		return ChannelName;
	}

private:
	TWeakObjectPtr<UMovieSceneSection> OwningSection;
	FMovieSceneFloatChannel* Channel = nullptr;
	FName ChannelName;
};

void AppendInvalidHandle(
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics,
	FStringView ArgumentName,
	const TCHAR* Message)
{
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = FString::Printf(
		TEXT("/Handles/%.*s"),
		ArgumentName.Len(),
		ArgumentName.GetData());
	Diagnostic.Code = TEXT("AD.UMG.PROVIDER.INVALID_HANDLE");
	Diagnostic.Message = Message;
	OutDiagnostics.Add(MoveTemp(Diagnostic));
}

const FAssetDocumentOpaqueHandle* FindArgument(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	FStringView ArgumentName)
{
	for (const FAssetDocumentPrimitiveHandleArgument& Argument :
		Invocation.HandleArguments)
	{
		if (Argument.Name == ArgumentName)
		{
			return &Argument.Handle;
		}
	}
	return nullptr;
}

TUniquePtr<IAssetDocumentVerifiedHandleLease> Acquire(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	FStringView ArgumentName,
	EAssetDocumentHandleKind Kind,
	EAssetDocumentHandleAccess Access,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics,
	bool bRequired)
{
	const FAssetDocumentOpaqueHandle* Handle =
		FindArgument(Invocation, ArgumentName);
	if (Handle == nullptr)
	{
		if (bRequired)
		{
			AppendInvalidHandle(
				OutDiagnostics, ArgumentName,
				TEXT("The required typed handle argument is absent."));
		}
		return nullptr;
	}
	FAssetDocumentHandleValidationContext Context;
	Context.RequestId = Invocation.RequestId;
	Context.TransactionId = Invocation.TransactionId;
	Context.RuntimeRevision = Invocation.RuntimeRevision;
	Context.ProviderId = TEXT("umg");
	Context.Phase = Invocation.Phase;
	Context.RequiredKind = Kind;
	Context.RequiredAccess = Access;
	FAssetDocumentVerifiedLeaseResult Result =
		Handles.AcquireVerifiedLease(*Handle, Context);
	if (!Result.Lease)
	{
		OutDiagnostics.Append(
			Result.Diagnostics.GetData(), Result.Diagnostics.Num());
		const bool bHasStableProviderDiagnostic =
			Result.Diagnostics.ContainsByPredicate(
				[](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
				{
					return Diagnostic.Code
						== TEXT("AD.UMG.PROVIDER.INVALID_HANDLE");
				});
		if (!bHasStableProviderDiagnostic)
		{
			AppendInvalidHandle(
				OutDiagnostics, ArgumentName,
				TEXT("The typed handle could not be verified."));
		}
	}
	return MoveTemp(Result.Lease);
}

UObject* ResolveObject(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	FStringView ArgumentName,
	EAssetDocumentHandleKind Kind,
	EAssetDocumentHandleAccess Access,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics,
	bool bRequired)
{
	TUniquePtr<IAssetDocumentVerifiedHandleLease> Lease =
		Acquire(
			Invocation, Handles, ArgumentName, Kind, Access,
			OutDiagnostics, bRequired);
	if (!Lease)
	{
		return nullptr;
	}
	IAssetDocumentNativeHandlePayload& Payload =
		Lease->GetPayloadChecked();
	if (Payload.GetPayloadTypeId() != UObjectPayloadType
		|| !Payload.IsAlive())
	{
		AppendInvalidHandle(
			OutDiagnostics, ArgumentName,
			TEXT("The handle payload is not a live UMG UObject."));
		return nullptr;
	}
	return static_cast<FObjectPayload&>(Payload).Get();
}

template <typename ObjectType>
ObjectType* ResolveTypedObject(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	FStringView ArgumentName,
	EAssetDocumentHandleKind Kind,
	EAssetDocumentHandleAccess Access,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics,
	bool bRequired = true)
{
	UObject* Object = ResolveObject(
		Invocation, Handles, ArgumentName, Kind, Access,
		OutDiagnostics, bRequired);
	if (Object == nullptr)
	{
		return nullptr;
	}
	ObjectType* Typed = Cast<ObjectType>(Object);
	if (Typed == nullptr)
	{
		AppendInvalidHandle(
			OutDiagnostics, ArgumentName,
			TEXT("The verified UObject has the wrong concrete type."));
	}
	return Typed;
}

FAssetDocumentPrimitiveHandleResult& FindOrAddResult(
	FAssetDocumentPrimitiveOutcome& Outcome,
	const TCHAR* ResultName)
{
	for (FAssetDocumentPrimitiveHandleResult& Result : Outcome.HandleResults)
	{
		if (Result.Name == ResultName)
		{
			return Result;
		}
	}
	FAssetDocumentPrimitiveHandleResult& Result =
		Outcome.HandleResults.AddDefaulted_GetRef();
	Result.Name = ResultName;
	return Result;
}

bool AppendIssued(
	FAssetDocumentIssuedHandleResult&& Issued,
	const TCHAR* ResultName,
	FAssetDocumentPrimitiveOutcome& Outcome)
{
	if (!Issued.Handle.IsSet())
	{
		Outcome.Diagnostics.Append(
			Issued.Diagnostics.GetData(), Issued.Diagnostics.Num());
		if (Issued.Diagnostics.IsEmpty())
		{
			FAssetDocumentRuntimeDiagnostic Diagnostic;
			Diagnostic.Path = TEXT("/Handles");
			Diagnostic.Code = TEXT("AD.UMG.PROVIDER.INVALID_HANDLE");
			Diagnostic.Message = TEXT("The provider could not issue an opaque handle.");
			Outcome.Diagnostics.Add(MoveTemp(Diagnostic));
		}
		return false;
	}
	FindOrAddResult(Outcome, ResultName).Handles.Add(
		Issued.Handle.GetValue());
	return true;
}
} // namespace AssetDocumentUMGNativePayloadsPrivate


TUniquePtr<IAssetDocumentNativeHandlePayload>
MakeAssetDocumentUMGUObjectPayload(UObject* Object)
{
	using namespace AssetDocumentUMGNativePayloadsPrivate;
	return MakeUnique<FObjectPayload>(Object);
}

TUniquePtr<IAssetDocumentNativeHandlePayload>
MakeAssetDocumentUMGChannelPayload(
	UMovieSceneSection* OwningSection,
	FMovieSceneFloatChannel* Channel,
	FName ChannelName)
{
	using namespace AssetDocumentUMGNativePayloadsPrivate;
	return MakeUnique<FChannelPayload>(
		OwningSection, Channel, ChannelName);
}

UWidgetBlueprint* ResolveWidgetBlueprint(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	FStringView ArgumentName,
	EAssetDocumentHandleAccess Access,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	using namespace AssetDocumentUMGNativePayloadsPrivate;
	return ResolveTypedObject<UWidgetBlueprint>(
		Invocation, Handles, ArgumentName,
		EAssetDocumentHandleKind::Asset, Access, OutDiagnostics);
}

UClass* ResolveWidgetClass(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	FStringView ArgumentName,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	using namespace AssetDocumentUMGNativePayloadsPrivate;
	return ResolveTypedObject<UClass>(
		Invocation, Handles, ArgumentName,
		EAssetDocumentHandleKind::Class,
		EAssetDocumentHandleAccess::ReadOnly,
		OutDiagnostics);
}

UWidget* ResolveWidget(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	FStringView ArgumentName,
	EAssetDocumentHandleAccess Access,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics,
	bool bRequired)
{
	using namespace AssetDocumentUMGNativePayloadsPrivate;
	return ResolveTypedObject<UWidget>(
		Invocation, Handles, ArgumentName,
		EAssetDocumentHandleKind::Node, Access,
		OutDiagnostics, bRequired);
}

UWidgetAnimation* ResolveWidgetAnimation(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	FStringView ArgumentName,
	EAssetDocumentHandleAccess Access,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	using namespace AssetDocumentUMGNativePayloadsPrivate;
	return ResolveTypedObject<UWidgetAnimation>(
		Invocation, Handles, ArgumentName,
		EAssetDocumentHandleKind::Subobject, Access,
		OutDiagnostics);
}

UUserWidget* ResolvePreviewRoot(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	FStringView ArgumentName,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	using namespace AssetDocumentUMGNativePayloadsPrivate;
	return ResolveTypedObject<UUserWidget>(
		Invocation, Handles, ArgumentName,
		EAssetDocumentHandleKind::Scratch,
		EAssetDocumentHandleAccess::Mutable,
		OutDiagnostics);
}

UMovieSceneTrack* ResolveMovieSceneTrack(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	FStringView ArgumentName,
	EAssetDocumentHandleAccess Access,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	using namespace AssetDocumentUMGNativePayloadsPrivate;
	return ResolveTypedObject<UMovieSceneTrack>(
		Invocation, Handles, ArgumentName,
		EAssetDocumentHandleKind::Track, Access,
		OutDiagnostics);
}

UMovieSceneSection* ResolveMovieSceneSection(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	FStringView ArgumentName,
	EAssetDocumentHandleAccess Access,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics,
	bool bRequired)
{
	using namespace AssetDocumentUMGNativePayloadsPrivate;
	return ResolveTypedObject<UMovieSceneSection>(
		Invocation, Handles, ArgumentName,
		EAssetDocumentHandleKind::Section, Access,
		OutDiagnostics, bRequired);
}

FMovieSceneFloatChannel* ResolveMovieSceneChannel(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	FStringView ArgumentName,
	EAssetDocumentHandleAccess Access,
	UMovieSceneSection*& OutOwningSection,
	FName& OutChannelName,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	using namespace AssetDocumentUMGNativePayloadsPrivate;
	TUniquePtr<IAssetDocumentVerifiedHandleLease> Lease =
		Acquire(
			Invocation, Handles, ArgumentName,
			EAssetDocumentHandleKind::Channel,
			Access, OutDiagnostics, true);
	if (!Lease)
	{
		return nullptr;
	}
	IAssetDocumentNativeHandlePayload& Payload =
		Lease->GetPayloadChecked();
	if (Payload.GetPayloadTypeId() != ChannelPayloadType
		|| !Payload.IsAlive())
	{
		AppendInvalidHandle(
			OutDiagnostics, ArgumentName,
			TEXT("The handle payload is not a live MovieScene float channel."));
		return nullptr;
	}
	FChannelPayload& ChannelPayload =
		static_cast<FChannelPayload&>(Payload);
	OutOwningSection = ChannelPayload.GetOwningSection();
	OutChannelName = ChannelPayload.GetChannelName();
	return ChannelPayload.GetChannel();
}

bool IssueAssetDocumentUMGUObjectResult(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	const TCHAR* ResultName,
	EAssetDocumentHandleKind Kind,
	EAssetDocumentHandleAccess Access,
	UObject* Object,
	FAssetDocumentPrimitiveOutcome& InOutOutcome)
{
	using namespace AssetDocumentUMGNativePayloadsPrivate;
	if (!IsValid(Object))
	{
		return false;
	}
	FAssetDocumentIssuedHandleResult Issued =
		Handles.Issue(
			Kind, Access,
			MakeAssetDocumentUMGUObjectPayload(Object));
	return AppendIssued(
		MoveTemp(Issued), ResultName, InOutOutcome);
}

bool IssueAssetDocumentUMGChannelResult(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	const TCHAR* ResultName,
	EAssetDocumentHandleAccess Access,
	UMovieSceneSection* OwningSection,
	FMovieSceneFloatChannel* Channel,
	FName ChannelName,
	FAssetDocumentPrimitiveOutcome& InOutOutcome)
{
	using namespace AssetDocumentUMGNativePayloadsPrivate;
	if (!IsValid(OwningSection) || Channel == nullptr)
	{
		return false;
	}
	FAssetDocumentIssuedHandleResult Issued =
		Handles.Issue(
			EAssetDocumentHandleKind::Channel,
			Access,
			MakeAssetDocumentUMGChannelPayload(
				OwningSection, Channel, ChannelName));
	return AppendIssued(
		MoveTemp(Issued), ResultName, InOutOutcome);
}
