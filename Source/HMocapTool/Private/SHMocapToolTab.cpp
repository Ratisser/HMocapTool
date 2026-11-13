// Copyright Epic Games, Inc. All Rights Reserved.

#include "SHMocapToolTab.h"
#include "HMocapToolModule.h"
#include "SHMocapPreview.h"
#include "Animation/AnimSequence.h"
#include "Engine/SkeletalMesh.h"
#include "Retargeter/IKRetargeter.h"
#include "PropertyCustomizationHelpers.h"
#include "AssetRegistry/AssetData.h"
#include "Widgets/Layout/SScrollBox.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Modules/ModuleManager.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Views/STableRow.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "SHMocapToolTab"

namespace
{
	void ShowNotification(const FText& Message, bool bSuccess)
	{
		FNotificationInfo Info(Message);
		Info.ExpireDuration = 5.f;
		TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
		if (Item.IsValid())
		{
			Item->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
		}
	}
}

void SHMocapToolTab::Construct(const FArguments& InArgs)
{
	Runner = &FHMocapToolModule::Get().GetRunner();
	Settings.Retarget.Load();
	Jobs = Runner->GetJobs();
	for (const TSharedPtr<FHMocapJob>& Job : Jobs)
	{
		PreviousStates.Add(Job, Job->State);
	}
	Runner->OnJobsChanged.AddSP(this, &SHMocapToolTab::JobsChanged);
	Runner->OnLog.AddSP(this, &SHMocapToolTab::AppendLog);

	ChildSlot
	[
		// 왼쪽: 영상 목록 | 가운데: 영상 비교(프리뷰) | 오른쪽: 설정·진행
		SNew(SSplitter)
		.Orientation(Orient_Horizontal)

		+ SSplitter::Slot().Value(0.2f)
		[
			BuildVideoPanel()
		]

		+ SSplitter::Slot().Value(0.56f)
		[
			MakePanel(LOCTEXT("PreviewPanel", "영상 비교"), SAssignNew(Preview, SHMocapPreview))
		]

		+ SSplitter::Slot().Value(0.24f)
		[
			BuildSettingsPanel()
		]
	];
}

TSharedRef<SWidget> SHMocapToolTab::MakePanel(const FText& Title, const TSharedRef<SWidget>& Content) const
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(4.f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(4.f, 2.f, 4.f, 6.f)
			[
				SNew(STextBlock).Text(Title).Font(FAppStyle::GetFontStyle("NormalFontBold"))
			]
			+ SVerticalBox::Slot().FillHeight(1.f)
			[
				Content
			]
		];
}

TSharedRef<SWidget> SHMocapToolTab::BuildVideoPanel()
{
	return MakePanel(LOCTEXT("VideoPanel", "영상 목록"),
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(4.f)
		[
			// 목록 칸이 좁아서 버튼이 넘치면 다음 줄로 넘긴다.
			SNew(SWrapBox)
			.UseAllottedSize(true)
			+ SWrapBox::Slot().Padding(2.f)
			[
				SNew(SButton).Text(LOCTEXT("AddVideos", "영상 추가..."))
				.OnClicked(this, &SHMocapToolTab::AddVideos)
			]
			+ SWrapBox::Slot().Padding(2.f)
			[
				SNew(SButton).Text(LOCTEXT("RemoveSelected", "선택 제거"))
				.OnClicked(this, &SHMocapToolTab::RemoveSelected)
			]
			+ SWrapBox::Slot().Padding(2.f)
			[
				SNew(SButton).Text(LOCTEXT("ClearFinished", "완료 항목 지우기"))
				.OnClicked_Lambda([this]()
				{
					if (Runner.IsValid()) { Runner->ClearFinished(); }
					return FReply::Handled();
				})
			]
		]
		+ SVerticalBox::Slot().FillHeight(1.f).Padding(4.f)
		[
			SAssignNew(ListView, SListView<TSharedPtr<FHMocapJob>>)
			.ListItemsSource(&Jobs)
			.SelectionMode(ESelectionMode::Multi)
			.OnGenerateRow(this, &SHMocapToolTab::GenerateRow)
			.OnSelectionChanged(this, &SHMocapToolTab::OnJobSelectionChanged)
		]);
}

TSharedRef<SWidget> SHMocapToolTab::BuildSettingsPanel()
{
	return MakePanel(LOCTEXT("SettingsPanel", "설정 / 진행"),
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(4.f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("MissingTracker", "MetaHuman Animator Markerless Motion Capture 플러그인이 없어서 영상을 처리할 수 없습니다. Fab에서 받아 이 프로젝트의 Plugins/MetaHumanBodyTracker 에 두고 플러그인을 켠 뒤 에디터를 다시 시작하세요."))
			.ColorAndOpacity(FLinearColor(1.f, 0.35f, 0.25f))
			.AutoWrapText(true)
			.Visibility_Lambda([]() { return UHMocapJobRunner::IsBodyTrackerAvailable() ? EVisibility::Collapsed : EVisibility::Visible; })
		]
		// 설정이 길어서 스크롤 상자에 넣는다. 아래 로그와 높이를 나눠 쓴다.
		+ SVerticalBox::Slot().FillHeight(1.f).Padding(4.f)
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot()
			[
			SNew(SVerticalBox)
			.IsEnabled_Lambda([this]() { return Runner.IsValid() && !IsRunning(); })
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
			[
				SNew(STextBlock).Text(LOCTEXT("OutputFolder", "출력 폴더"))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
			[
				SNew(SEditableTextBox)
				.Text(FText::FromString(Settings.OutputFolder))
				.OnTextChanged_Lambda([this](const FText& Text) { Settings.OutputFolder = Text.ToString(); })
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([this]() { return Settings.bFootLocking ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { Settings.bFootLocking = State == ECheckBoxState::Checked; })
				[
					SNew(STextBlock).Text(LOCTEXT("FootLocking", "발 고정"))
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([this]() { return Settings.bAutoBodyHeight ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { Settings.bAutoBodyHeight = State == ECheckBoxState::Checked; })
				[
					SNew(STextBlock).Text(LOCTEXT("AutoBodyHeight", "키 자동 추정"))
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(0.4f).VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(LOCTEXT("BodyHeight", "키 (cm)"))
				]
				+ SHorizontalBox::Slot().FillWidth(0.6f)
				[
					SNew(SSpinBox<float>).MinValue(100.f).MaxValue(230.f)
					.Value_Lambda([this]() { return Settings.BodyHeightCm; })
					.OnValueChanged_Lambda([this](float Value) { Settings.BodyHeightCm = Value; })
					.IsEnabled_Lambda([this]() { return !Settings.bAutoBodyHeight; })
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 12.f, 0.f, 2.f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("RetargetHeader", "우리 캐릭터로 변환"))
				.Font(FAppStyle::GetFontStyle("NormalFontBold"))
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				BuildRetargetSettings()
			]
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(4.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(2.f)
			[
				SNew(SButton).Text(LOCTEXT("Start", "처리 시작"))
				.IsEnabled(this, &SHMocapToolTab::CanStart)
				// 버튼이 꺼져 있을 때 이유를 알 수 있게 한다.
				.ToolTipText_Lambda([this]()
				{
					if (IsRunning())
					{
						return LOCTEXT("StartTooltipRunning", "처리 중입니다.");
					}
					if (!UHMocapJobRunner::IsBodyTrackerAvailable())
					{
						return LOCTEXT("StartTooltipNoTracker", "MetaHuman Animator Markerless Motion Capture 플러그인이 없어서 처리할 수 없습니다.");
					}
					if (!Jobs.ContainsByPredicate([](const TSharedPtr<FHMocapJob>& Job) { return Job->State == EHMocapJobState::Pending; }))
					{
						return LOCTEXT("StartTooltipNoPending", "대기 중인 영상이 없습니다. 영상을 추가하세요.");
					}
					return LOCTEXT("StartTooltip", "대기 중인 영상을 순서대로 처리합니다.");
				})
				.OnClicked_Lambda([this]()
				{
					if (Runner.IsValid()) { Runner->Start(Settings); }
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(2.f)
			[
				SNew(SButton).Text(LOCTEXT("Cancel", "취소"))
				.IsEnabled(this, &SHMocapToolTab::IsRunning)
				.OnClicked_Lambda([this]()
				{
					if (Runner.IsValid()) { Runner->Cancel(); }
					return FReply::Handled();
				})
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(4.f)
		[
			SNew(STextBlock).AutoWrapText(true)
			.Text_Lambda([this]()
			{
				const TSharedPtr<FHMocapJob> Job = GetCurrentJob();
				return Job ? FText::FromString(FPaths::GetCleanFilename(Job->VideoPath)) : LOCTEXT("Idle", "진행 중인 영상이 없습니다.");
			})
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(4.f)
		[
			SNew(STextBlock).AutoWrapText(true)
			.Text_Lambda([this]()
			{
				const TSharedPtr<FHMocapJob> Job = GetCurrentJob();
				return Job ? GetStateText(*Job) : FText::GetEmpty();
			})
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(4.f)
		[
			SNew(SProgressBar).Percent(this, &SHMocapToolTab::GetProgress)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(4.f)
		[
			SNew(STextBlock).Text(LOCTEXT("Log", "작업 로그"))
		]
		+ SVerticalBox::Slot().FillHeight(0.6f).Padding(4.f)
		[
			SAssignNew(LogBox, SMultiLineEditableTextBox).IsReadOnly(true)
		]);
}

TSharedRef<SWidget> SHMocapToolTab::BuildRetargetSettings()
{
	const auto MakeRow = [](const FText& Label, const FText& Tooltip, const TSharedRef<SWidget>& Content) -> TSharedRef<SWidget>
	{
		return SNew(SHorizontalBox)
			.ToolTipText(Tooltip)
			+ SHorizontalBox::Slot().FillWidth(0.4f).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(Label)
			]
			+ SHorizontalBox::Slot().FillWidth(0.6f).VAlign(VAlign_Center)
			[
				Content
			];
	};

	return SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
		[
			SNew(SCheckBox)
			.ToolTipText(LOCTEXT("RetargetEnabledTooltip", "영상 처리가 끝나면 결과를 아래 설정으로 바로 변환합니다. 꺼 두어도 아래 버튼으로 나중에 변환할 수 있습니다."))
			.IsChecked_Lambda([this]() { return Settings.Retarget.bEnabled ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
			{
				Settings.Retarget.bEnabled = State == ECheckBoxState::Checked;
				Settings.Retarget.Save();
			})
			[
				SNew(STextBlock).Text(LOCTEXT("RetargetEnabled", "영상 처리 후 자동으로 변환"))
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
		[
			MakeRow(LOCTEXT("Retargeter", "IK 리타게터"),
				LOCTEXT("RetargeterTooltip", "원본이 모캡 결과(MetaHuman 스켈레톤), 대상이 우리 캐릭터인 IK 리타게터 에셋입니다."),
				SNew(SObjectPropertyEntryBox)
				.AllowedClass(UIKRetargeter::StaticClass())
				.ObjectPath_Lambda([this]() { return Settings.Retarget.Retargeter.ToString(); })
				.OnObjectChanged_Lambda([this](const FAssetData& AssetData)
				{
					Settings.Retarget.Retargeter = AssetData.GetSoftObjectPath();
					Settings.Retarget.Save();
				})
				.AllowClear(true)
				.DisplayThumbnail(false)
				.DisplayUseSelected(true)
				.DisplayBrowse(true))
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
		[
			MakeRow(LOCTEXT("TargetMesh", "대상 메쉬"),
				LOCTEXT("TargetMeshTooltip", "애니메이션을 입힐 우리 캐릭터의 스켈레탈 메쉬입니다. 비워 두면 리타게터의 대상 프리뷰 메쉬를 씁니다."),
				SNew(SObjectPropertyEntryBox)
				.AllowedClass(USkeletalMesh::StaticClass())
				.ObjectPath_Lambda([this]() { return Settings.Retarget.TargetMesh.ToString(); })
				.OnObjectChanged_Lambda([this](const FAssetData& AssetData)
				{
					Settings.Retarget.TargetMesh = AssetData.GetSoftObjectPath();
					Settings.Retarget.Save();
				})
				.AllowClear(true)
				.DisplayThumbnail(false)
				.DisplayUseSelected(true)
				.DisplayBrowse(true))
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
		[
			MakeRow(LOCTEXT("RetargetFolder", "변환 결과 폴더"),
				LOCTEXT("RetargetFolderTooltip", "변환한 애니메이션을 둘 콘텐츠 경로입니다. 이름은 원본 뒤에 _RT가 붙습니다. 비우면 원본과 같은 폴더에 둡니다."),
				SNew(SEditableTextBox)
				.Text(FText::FromString(Settings.Retarget.OutputFolder))
				.OnTextChanged_Lambda([this](const FText& Text) { Settings.Retarget.OutputFolder = Text.ToString(); })
				.OnTextCommitted_Lambda([this](const FText&, ETextCommit::Type) { Settings.Retarget.Save(); }))
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
		[
			SNew(SCheckBox)
			.ToolTipText(LOCTEXT("RootMotionTooltip", "골반 뼈에 들어 있는 수평 이동을 root 뼈로 옮기고 Enable Root Motion을 켭니다. 몸의 포즈는 바뀌지 않습니다."))
			.IsChecked_Lambda([this]() { return Settings.Retarget.bRootMotion ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
			{
				Settings.Retarget.bRootMotion = State == ECheckBoxState::Checked;
				Settings.Retarget.Save();
			})
			[
				SNew(STextBlock).Text(LOCTEXT("RootMotion", "루트 모션으로 변환"))
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
		[
			SNew(SCheckBox)
			.IsEnabled_Lambda([this]() { return Settings.Retarget.bRootMotion; })
			.ToolTipText(LOCTEXT("AlignStartLocationTooltip", "첫 프레임에 root가 원점에 오도록 애니메이션 전체를 옮깁니다. 방향은 바꾸지 않습니다. 끄면 영상 속에서 서 있던 위치가 그대로 남습니다."))
			.IsChecked_Lambda([this]() { return Settings.Retarget.bAlignStartLocation ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
			{
				Settings.Retarget.bAlignStartLocation = State == ECheckBoxState::Checked;
				Settings.Retarget.Save();
			})
			[
				SNew(STextBlock).Text(LOCTEXT("AlignStartLocation", "시작 위치를 원점으로 맞춤"))
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
		[
			SNew(SCheckBox)
			.IsEnabled_Lambda([this]() { return Settings.Retarget.bRootMotion; })
			.ToolTipText(LOCTEXT("AlignStartRotationTooltip", "첫 프레임에 몸이 정면을 보도록 애니메이션 전체를 시작 위치를 축으로 돌립니다. 위치는 바꾸지 않습니다. 게임에서 캡슐의 정면과 몸의 방향이 맞으려면 켜 두세요."))
			.IsChecked_Lambda([this]() { return Settings.Retarget.bAlignStartRotation ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
			{
				Settings.Retarget.bAlignStartRotation = State == ECheckBoxState::Checked;
				Settings.Retarget.Save();
			})
			[
				SNew(STextBlock).Text(LOCTEXT("AlignStartRotation", "시작 방향을 정면으로 맞춤"))
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
		[
			MakeRow(LOCTEXT("SmoothSeconds", "이동 경로 다듬기 (초)"),
				LOCTEXT("SmoothSecondsTooltip", "root가 지나가는 경로의 떨림을 이 시간 폭으로 평균 냅니다. 0이면 다듬지 않습니다. 몸의 포즈는 그대로이고 캡슐이 움직이는 길만 부드러워집니다."),
				SNew(SSpinBox<float>).MinValue(0.f).MaxValue(1.f).Delta(0.05f)
				.IsEnabled_Lambda([this]() { return Settings.Retarget.bRootMotion; })
				.Value_Lambda([this]() { return Settings.Retarget.SmoothSeconds; })
				.OnValueChanged_Lambda([this](float Value) { Settings.Retarget.SmoothSeconds = Value; })
				.OnValueCommitted_Lambda([this](float Value, ETextCommit::Type)
				{
					Settings.Retarget.SmoothSeconds = Value;
					Settings.Retarget.Save();
				}))
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 2.f)
		[
			SNew(SButton)
			.HAlign(HAlign_Center)
			.Text(LOCTEXT("ConvertPreview", "프리뷰의 결과 애니메이션 변환"))
			.ToolTipText(LOCTEXT("ConvertPreviewTooltip", "프리뷰에 올라와 있는 결과 애니메이션을 위 설정으로 변환합니다. 원본은 그대로 두고 새 애니메이션을 만들며, 결과는 비교 애니메이션으로 올라갑니다."))
			.IsEnabled(this, &SHMocapToolTab::CanConvertPreview)
			.OnClicked(this, &SHMocapToolTab::ConvertPreviewAnimation)
		];
}

bool SHMocapToolTab::CanConvertPreview() const
{
	return Preview.IsValid() && Preview->GetAnimation() != nullptr && !Settings.Retarget.Retargeter.IsNull();
}

FReply SHMocapToolTab::ConvertPreviewAnimation()
{
	UAnimSequence* Source = Preview.IsValid() ? Preview->GetAnimation() : nullptr;
	if (!Source)
	{
		return FReply::Handled();
	}

	FText Error;
	UAnimSequence* Converted = HMocapRetarget::Convert(Source, Settings.Retarget, Error);
	if (!Converted)
	{
		const FText Message = FText::Format(LOCTEXT("ConvertFailed", "변환 실패: {0}"), Error);
		AppendLog(Message);
		ShowNotification(Message, false);
		return FReply::Handled();
	}

	AppendLog(FText::Format(LOCTEXT("ConvertDone", "변환 완료: {0}"), FText::FromString(Converted->GetPathName())));
	if (!Error.IsEmpty())
	{
		const FText Message = FText::Format(LOCTEXT("ConvertProblem", "변환 문제: {0}"), Error);
		AppendLog(Message);
		ShowNotification(Message, false);
	}
	Preview->SetCompareAnimation(Converted);
	return FReply::Handled();
}

TSharedRef<ITableRow> SHMocapToolTab::GenerateRow(TSharedPtr<FHMocapJob> Job, const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(STableRow<TSharedPtr<FHMocapJob>>, OwnerTable)
		[
			// 목록 칸이 좁으므로 파일 이름 아래에 상태를 둔다.
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f).Padding(4.f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Text(FText::FromString(FPaths::GetCleanFilename(Job->VideoPath)))
					.ToolTipText(FText::FromString(Job->VideoPath))
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f, 0.f, 0.f)
				[
					SNew(STextBlock).AutoWrapText(true)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text_Lambda([Job]() { return GetStateText(*Job); })
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.f)
			[
				SNew(SButton).Text(LOCTEXT("OpenAnimation", "열기"))
				.Visibility_Lambda([Job]() { return Job->State == EHMocapJobState::Done ? EVisibility::Visible : EVisibility::Collapsed; })
				.OnClicked_Lambda([Job]() { return OpenAnimation(Job); })
			]
		];
}

FReply SHMocapToolTab::AddVideos()
{
	IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
	if (!Desktop)
	{
		ShowNotification(LOCTEXT("DesktopUnavailable", "파일 선택 창을 사용할 수 없습니다."), false);
		return FReply::Handled();
	}
	TArray<FString> Paths;
	if (Desktop->OpenFileDialog(FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared()),
		LOCTEXT("ChooseVideos", "영상 추가").ToString(), LastDirectory, FString(),
		LOCTEXT("VideoFilter", "영상 파일 (*.mp4;*.mov)|*.mp4;*.mov").ToString(), EFileDialogFlags::Multiple, Paths)
		&& !Paths.IsEmpty())
	{
		LastDirectory = FPaths::GetPath(Paths[0]);
		if (Runner.IsValid()) { Runner->AddVideos(Paths); }
	}
	return FReply::Handled();
}

FReply SHMocapToolTab::RemoveSelected()
{
	if (Runner.IsValid() && ListView.IsValid())
	{
		const TArray<TSharedPtr<FHMocapJob>> Selected = ListView->GetSelectedItems();
		for (const TSharedPtr<FHMocapJob>& Job : Selected)
		{
			Runner->RemoveJob(Job);
		}
	}
	return FReply::Handled();
}

bool SHMocapToolTab::IsRunning() const
{
	return Runner.IsValid() && Runner->IsRunning();
}

bool SHMocapToolTab::CanStart() const
{
	return Runner.IsValid() && !IsRunning() && UHMocapJobRunner::IsBodyTrackerAvailable()
		&& Jobs.ContainsByPredicate([](const TSharedPtr<FHMocapJob>& Job) { return Job->State == EHMocapJobState::Pending; });
}

TSharedPtr<FHMocapJob> SHMocapToolTab::GetCurrentJob() const
{
	const TSharedPtr<FHMocapJob>* Current = Jobs.FindByPredicate([](const TSharedPtr<FHMocapJob>& Job)
	{
		return Job->State == EHMocapJobState::Ingesting || Job->State == EHMocapJobState::Processing || Job->State == EHMocapJobState::Exporting;
	});
	return Current ? *Current : nullptr;
}

TOptional<float> SHMocapToolTab::GetProgress() const
{
	const TSharedPtr<FHMocapJob> Job = GetCurrentJob();
	if (!Job) { return 0.f; }
	if (Job->State != EHMocapJobState::Processing) { return TOptional<float>(); }
	const int32 FrameCount = Job->FrameEnd - Job->FrameStart;
	return FrameCount > 0 ? FMath::Clamp(static_cast<float>(Job->FrameCurrent - Job->FrameStart) / FrameCount, 0.f, 1.f) : 0.f;
}

FText SHMocapToolTab::GetStateText(const FHMocapJob& Job)
{
	switch (Job.State)
	{
	case EHMocapJobState::Pending: return LOCTEXT("Pending", "대기");
	case EHMocapJobState::Ingesting: return LOCTEXT("Ingesting", "가져오는 중");
	case EHMocapJobState::Processing:
	{
		const int32 Count = Job.FrameEnd - Job.FrameStart;
		const float Progress = Count > 0 ? FMath::Clamp(static_cast<float>(Job.FrameCurrent - Job.FrameStart) / Count, 0.f, 1.f) : 0.f;
		return FText::Format(LOCTEXT("Processing", "처리 중 (단계 {0}/5, {1}%)"),
			FMath::Min(Job.StagesDone + 1, 5), FMath::RoundToInt(Progress * 100.f));
	}
	case EHMocapJobState::Exporting: return LOCTEXT("Exporting", "내보내는 중");
	case EHMocapJobState::Done:
		return Job.Warning.IsEmpty() ? LOCTEXT("Done", "완료") : FText::Format(LOCTEXT("DoneWithWarning", "완료 (변환 문제: {0})"), Job.Warning);
	case EHMocapJobState::Failed: return FText::Format(LOCTEXT("Failed", "실패: {0}"), Job.Message);
	case EHMocapJobState::Cancelled: return LOCTEXT("Cancelled", "취소됨");
	default: return FText::GetEmpty();
	}
}

void SHMocapToolTab::JobsChanged()
{
	if (!Runner.IsValid()) { return; }
	Jobs = Runner->GetJobs();
	TMap<TSharedPtr<FHMocapJob>, EHMocapJobState> NewStates;
	for (const TSharedPtr<FHMocapJob>& Job : Jobs)
	{
		const EHMocapJobState* Previous = PreviousStates.Find(Job);
		if (Previous && *Previous != Job->State && (Job->State == EHMocapJobState::Done || Job->State == EHMocapJobState::Failed))
		{
			ShowNotification(FText::Format(LOCTEXT("JobToast", "{0}: {1}"),
				FText::FromString(FPaths::GetCleanFilename(Job->VideoPath)), GetStateText(*Job)), Job->State == EHMocapJobState::Done);

			// 방금 끝난 결과를 바로 확인할 수 있게 프리뷰에 올린다.
			if (Job->State == EHMocapJobState::Done)
			{
				LoadPreview(Job);
			}
		}
		NewStates.Add(Job, Job->State);
	}
	PreviousStates = MoveTemp(NewStates);
	if (ListView.IsValid()) { ListView->RequestListRefresh(); }
}

void SHMocapToolTab::AppendLog(const FText& Message)
{
	LogLines.Add(FText::Format(LOCTEXT("TimestampedLog", "[{0}] {1}"),
		FText::FromString(FDateTime::Now().ToString(TEXT("%H:%M:%S"))), Message).ToString());
	if (LogLines.Num() > 500)
	{
		LogLines.RemoveAt(0, LogLines.Num() - 500);
	}
	if (LogBox.IsValid()) { LogBox->SetText(FText::FromString(FString::Join(LogLines, TEXT("\n")))); }
}

void SHMocapToolTab::OnJobSelectionChanged(TSharedPtr<FHMocapJob> Job, ESelectInfo::Type SelectInfo)
{
	if (Job.IsValid() && Job->State == EHMocapJobState::Done)
	{
		LoadPreview(Job);
	}
}

void SHMocapToolTab::LoadPreview(const TSharedPtr<FHMocapJob>& Job)
{
	if (!Preview.IsValid() || !Job.IsValid())
	{
		return;
	}
	UAnimSequence* Animation = Cast<UAnimSequence>(Job->AnimSequencePath.TryLoad());
	if (!Animation || !Preview->Load(Job->VideoPath, Animation))
	{
		ShowNotification(LOCTEXT("PreviewFailed", "프리뷰할 애니메이션 에셋을 불러올 수 없습니다."), false);
		return;
	}

	// 변환 결과가 있으면 모캡 결과 옆에 나란히 올린다.
	if (UAnimSequence* Converted = Cast<UAnimSequence>(Job->RetargetedAnimPath.TryLoad()))
	{
		Preview->SetCompareAnimation(Converted);
	}
}

FReply SHMocapToolTab::OpenAnimation(TSharedPtr<FHMocapJob> Job)
{
	UObject* Asset = Job->AnimSequencePath.TryLoad();
	UAssetEditorSubsystem* Editors = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
	if (!Asset || !Editors)
	{
		ShowNotification(LOCTEXT("OpenFailed", "애니메이션 에셋을 열 수 없습니다."), false);
		return FReply::Handled();
	}
	Editors->OpenEditorForAsset(Asset);
	FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser").Get().SyncBrowserToAssets(TArray<UObject*>{ Asset });
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
