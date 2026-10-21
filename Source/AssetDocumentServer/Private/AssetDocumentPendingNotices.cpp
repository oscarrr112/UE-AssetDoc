// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentPendingNotices.h"

#include "Dom/JsonValue.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Widgets/Notifications/SNotificationList.h"

namespace AssetDocumentPendingNoticesLocal
{
FCriticalSection& Lock()
{
	static FCriticalSection Section;
	return Section;
}

TArray<FAssetDocumentPendingNotice>& Notices()
{
	static TArray<FAssetDocumentPendingNotice> Open;
	return Open;
}

// One spelling per file: absolute, forward slashes.
FString NormalizeNoticeFile(const FString& File)
{
	FString Full = File.IsEmpty() ? FString() : FPaths::ConvertRelativePathToFull(File);
	FPaths::NormalizeFilename(Full);
	return Full;
}

int32 FindNotice(const FString& Kind, const FString& File)
{
	return Notices().IndexOfByPredicate([&Kind, &File](const FAssetDocumentPendingNotice& Notice)
	{
		return Notice.Kind == Kind && Notice.File.Equals(File, ESearchCase::IgnoreCase);
	});
}
} // namespace AssetDocumentPendingNoticesLocal

TSharedRef<FJsonObject> FAssetDocumentPendingNotice::ToJson() const
{
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("kind"), Kind);
	Object->SetStringField(TEXT("file"), File);
	Object->SetStringField(TEXT("code"), Code);
	Object->SetStringField(TEXT("message"), Message);
	Object->SetStringField(TEXT("time_utc"), TimeUtc.ToIso8601());
	return Object;
}

void FAssetDocumentPendingNotices::Post(const FString& Kind, const FString& File, const FString& Code, const FString& Message,
	const FString& NotificationText)
{
	using namespace AssetDocumentPendingNoticesLocal;
	FAssetDocumentPendingNotice Notice;
	Notice.Kind = Kind;
	Notice.File = NormalizeNoticeFile(File);
	Notice.Code = Code;
	Notice.Message = Message;
	Notice.TimeUtc = FDateTime::UtcNow();
	{
		FScopeLock ScopeLock(&Lock());
		const int32 Existing = FindNotice(Kind, Notice.File);
		if (Existing != INDEX_NONE)
		{
			Notices().RemoveAt(Existing);
		}
		Notices().Add(MoveTemp(Notice));
	}
	if (!NotificationText.IsEmpty() && IsInGameThread() && FSlateApplication::IsInitialized())
	{
		FNotificationInfo Info(FText::FromString(NotificationText));
		Info.bFireAndForget = true;
		Info.ExpireDuration = 10.0f;
		Info.bUseLargeFont = false;
		if (const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
		{
			Item->SetCompletionState(SNotificationItem::CS_Fail);
		}
	}
}

void FAssetDocumentPendingNotices::Resolve(const FString& Kind, const FString& File)
{
	using namespace AssetDocumentPendingNoticesLocal;
	const FString Normalized = NormalizeNoticeFile(File);
	FScopeLock ScopeLock(&Lock());
	const int32 Existing = FindNotice(Kind, Normalized);
	if (Existing != INDEX_NONE)
	{
		Notices().RemoveAt(Existing);
	}
}

TArray<FAssetDocumentPendingNotice> FAssetDocumentPendingNotices::GetOpen()
{
	using namespace AssetDocumentPendingNoticesLocal;
	FScopeLock ScopeLock(&Lock());
	return Notices();
}

TArray<FAssetDocumentPendingNotice> FAssetDocumentPendingNotices::TakeUnreported()
{
	using namespace AssetDocumentPendingNoticesLocal;
	TArray<FAssetDocumentPendingNotice> Unreported;
	FScopeLock ScopeLock(&Lock());
	for (FAssetDocumentPendingNotice& Notice : Notices())
	{
		if (!Notice.bReported)
		{
			Unreported.Add(Notice);
			Notice.bReported = true;
		}
	}
	return Unreported;
}

TArray<TSharedPtr<FJsonValue>> FAssetDocumentPendingNotices::OpenToJson()
{
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FAssetDocumentPendingNotice& Notice : GetOpen())
	{
		Values.Add(MakeShared<FJsonValueObject>(Notice.ToJson()));
	}
	return Values;
}

FString FAssetDocumentPendingNotices::DescribeForToolResult(TConstArrayView<FAssetDocumentPendingNotice> Notices)
{
	if (Notices.IsEmpty())
	{
		return FString();
	}
	TArray<FString> Lines;
	Lines.Add(TEXT("Editor notices since the last tool call (work the editor did on its own; get_editor_context lists the open ones):"));
	for (const FAssetDocumentPendingNotice& Notice : Notices)
	{
		Lines.Add(FString::Printf(TEXT("- %s: %s [%s] %s"), *Notice.Kind, *Notice.File, *Notice.Code, *Notice.Message));
	}
	return FString::Join(Lines, TEXT("\n"));
}

void FAssetDocumentPendingNotices::AppendUnreportedToToolResult(FJsonObject& ToolResult)
{
	const FString Text = DescribeForToolResult(TakeUnreported());
	if (Text.IsEmpty())
	{
		return;
	}
	TArray<TSharedPtr<FJsonValue>> Content;
	const TArray<TSharedPtr<FJsonValue>>* Existing = nullptr;
	if (ToolResult.TryGetArrayField(TEXT("content"), Existing))
	{
		Content = *Existing;
	}
	const TSharedRef<FJsonObject> Block = MakeShared<FJsonObject>();
	Block->SetStringField(TEXT("type"), TEXT("text"));
	Block->SetStringField(TEXT("text"), Text);
	Content.Add(MakeShared<FJsonValueObject>(Block));
	ToolResult.SetArrayField(TEXT("content"), MoveTemp(Content));
}
