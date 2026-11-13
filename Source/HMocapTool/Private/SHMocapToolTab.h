// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"
#include "HMocapJobRunner.h"

class SMultiLineEditableTextBox;
class SHMocapPreview;

/**
 * 영상 모캡 탭 본문.
 * 왼쪽: 영상 목록 | 가운데: 영상 비교(원본 영상과 결과 애니메이션 동기 재생) | 오른쪽: 설정·진행.
 */
class SHMocapToolTab : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHMocapToolTab) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	TSharedRef<SWidget> MakePanel(const FText& Title, const TSharedRef<SWidget>& Content) const;
	TSharedRef<SWidget> BuildVideoPanel();
	TSharedRef<SWidget> BuildSettingsPanel();
	/** 리타깃·루트 모션 변환 설정. 바꿀 때마다 개인 설정에 저장한다. */
	TSharedRef<SWidget> BuildRetargetSettings();
	/** 프리뷰에 올라와 있는 결과 애니메이션을 지금 설정으로 변환하고, 결과를 비교 애니메이션으로 올린다. */
	FReply ConvertPreviewAnimation();
	bool CanConvertPreview() const;
	TSharedRef<ITableRow> GenerateRow(TSharedPtr<FHMocapJob> Job, const TSharedRef<STableViewBase>& OwnerTable);
	FReply AddVideos();
	FReply RemoveSelected();
	bool IsRunning() const;
	bool CanStart() const;
	void JobsChanged();
	void AppendLog(const FText& Message);
	void OnJobSelectionChanged(TSharedPtr<FHMocapJob> Job, ESelectInfo::Type SelectInfo);
	/** 완료된 작업의 원본 영상과 결과 애니메이션을 프리뷰에 올린다. */
	void LoadPreview(const TSharedPtr<FHMocapJob>& Job);
	TSharedPtr<FHMocapJob> GetCurrentJob() const;
	TOptional<float> GetProgress() const;
	static FText GetStateText(const FHMocapJob& Job);
	static FReply OpenAnimation(TSharedPtr<FHMocapJob> Job);

	TWeakObjectPtr<UHMocapJobRunner> Runner;
	FHMocapSettings Settings;
	FString LastDirectory;
	TArray<TSharedPtr<FHMocapJob>> Jobs;
	TMap<TSharedPtr<FHMocapJob>, EHMocapJobState> PreviousStates;
	TArray<FString> LogLines;
	TSharedPtr<SListView<TSharedPtr<FHMocapJob>>> ListView;
	TSharedPtr<SMultiLineEditableTextBox> LogBox;
	TSharedPtr<SHMocapPreview> Preview;
};
