// Copyright Epic Games, Inc. All Rights Reserved.

#include "SHMocapPreview.h"
#include "HMocapToolModule.h"
#include "SEditorViewport.h"
#include "EditorViewportClient.h"
#include "Viewports.h"
#include "PreviewScene.h"
#include "Animation/DebugSkelMeshComponent.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "MediaPlayer.h"
#include "MediaTexture.h"
#include "FileMediaSource.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/UObjectBase.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Framework/Application/SlateApplication.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "PropertyCustomizationHelpers.h"
#include "AssetRegistry/AssetData.h"
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "SHMocapPreview"

namespace
{
	// Seek 뒤 플레이어 시간이 안정될 때까지 기다리는 시간(초).
	constexpr float VideoSyncHoldSeconds = 0.3f;

	void ShowPreviewWarning(const FText& Message)
	{
		FNotificationInfo Info(Message);
		Info.ExpireDuration = 5.f;
		TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
		if (Item.IsValid())
		{
			Item->SetCompletionState(SNotificationItem::CS_Fail);
		}
	}

	class FHMocapPreviewViewportClient : public FEditorViewportClient
	{
	public:
		FHMocapPreviewViewportClient(FPreviewScene& InPreviewScene, const TSharedRef<SEditorViewport>& InViewport)
			: FEditorViewportClient(nullptr, &InPreviewScene, InViewport)
		{
			SetViewMode(VMI_Lit);
			EngineShowFlags.SetCompositeEditorPrimitives(true);
			EngineShowFlags.DisableAdvancedFeatures();

			// 발이 바닥에 닿는지 보기 쉽게 격자만 남긴다.
			DrawHelper.bDrawPivot = false;
			DrawHelper.bDrawWorldBox = false;
			DrawHelper.bDrawKillZ = false;
			DrawHelper.bDrawGrid = true;
		}

		virtual void Tick(float DeltaSeconds) override
		{
			// 프리뷰 월드는 아무도 틱하지 않으므로 여기서 돌려야 포즈가 갱신된다.
			if (PreviewScene)
			{
				PreviewScene->GetWorld()->Tick(LEVELTICK_All, DeltaSeconds);
			}
			FEditorViewportClient::Tick(DeltaSeconds);
		}
	};
}

/** 결과 애니메이션을 보여주는 3D 뷰포트. 재생 위치는 바깥(SHMocapPreview)에서 매 프레임 정해 준다. */
class SHMocapPreviewViewport : public SEditorViewport
{
public:
	SLATE_BEGIN_ARGS(SHMocapPreviewViewport) {}
	SLATE_END_ARGS()

	SHMocapPreviewViewport()
		: PreviewScene(FPreviewScene::ConstructionValues())
	{
	}

	void Construct(const FArguments& InArgs)
	{
		SEditorViewport::Construct(SEditorViewport::FArguments());

		// 컴포넌트는 PreviewScene이 참조를 잡아 준다(FGCObject).
		MeshComponent = MakeComponent();
		CompareComponent = MakeComponent();
	}

	/** 결과 애니메이션(기준). 메쉬를 찾지 못하면 false. */
	bool SetAnimation(UAnimSequence* InAnimation)
	{
		const bool bHasMesh = ApplyAnimation(MeshComponent, InAnimation);
		LayoutComponents();
		FrameCamera();
		return bHasMesh;
	}

	/** 나란히 놓고 비교할 애니메이션(리타깃 결과 등). nullptr이면 숨긴다. 메쉬를 찾지 못하면 false. */
	bool SetCompareAnimation(UAnimSequence* InAnimation)
	{
		const bool bHasMesh = ApplyAnimation(CompareComponent, InAnimation);
		LayoutComponents();
		FrameCamera();
		return bHasMesh;
	}

	void SetTime(float Time)
	{
		// 길이가 다른 애니메이션은 자기 끝에서 멈춰 있게 한다.
		for (UDebugSkelMeshComponent* Component : { MeshComponent.Get(), CompareComponent.Get() })
		{
			if (UAnimSingleNodeInstance* Instance = Component->GetSingleNodeInstance())
			{
				Instance->SetPosition(FMath::Clamp(Time, 0.f, Instance->GetLength()), false);
			}
		}
	}

	/** 캐릭터 정면(+Y)에서 전신이 들어오게 카메라를 맞춘다. 영상이 정면 촬영이라 같은 방향에서 비교한다. */
	void FrameCamera()
	{
		if (!Client.IsValid())
		{
			return;
		}

		FBox Box(ForceInit);
		for (const UDebugSkelMeshComponent* Component : { MeshComponent.Get(), CompareComponent.Get() })
		{
			if (Component->GetSkeletalMeshAsset())
			{
				Box += Component->Bounds.GetBox();
			}
		}
		if (!Box.IsValid)
		{
			return;
		}

		Client->SetViewRotation(FRotator(-5.f, -90.f, 0.f));
		Client->FocusViewportOnBox(Box, true);
		Client->Invalidate();
	}

protected:
	virtual TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override
	{
		TSharedRef<FHMocapPreviewViewportClient> ViewportClient =
			MakeShared<FHMocapPreviewViewportClient>(PreviewScene, SharedThis(this));

		ViewportClient->ViewportType = LVT_Perspective;
		ViewportClient->bSetListenerPosition = false;
		ViewportClient->SetViewLocation(EditorViewportDefs::DefaultPerspectiveViewLocation);
		ViewportClient->SetViewRotation(EditorViewportDefs::DefaultPerspectiveViewRotation);
		ViewportClient->SetRealtime(true);

		return ViewportClient;
	}

private:
	UDebugSkelMeshComponent* MakeComponent()
	{
		UDebugSkelMeshComponent* Component = NewObject<UDebugSkelMeshComponent>();
		Component->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
		// 루트 모션을 소비하지 않고 애니메이션에 적힌 그대로 루트를 움직여 보여 준다.
		Component->SetProcessRootMotionMode(EProcessRootMotionMode::Ignore);
		PreviewScene.AddComponent(Component, FTransform::Identity);
		return Component;
	}

	static bool ApplyAnimation(UDebugSkelMeshComponent* Component, UAnimSequence* InAnimation)
	{
		USkeletalMesh* Mesh = InAnimation ? InAnimation->GetPreviewMesh(true) : nullptr;
		if (!Mesh && InAnimation && InAnimation->GetSkeleton())
		{
			Mesh = InAnimation->GetSkeleton()->GetPreviewMesh(true);
		}

		Component->SetSkeletalMesh(Mesh);
		Component->EnablePreview(true, Mesh ? InAnimation : nullptr);
		Component->SetVisibility(Mesh != nullptr);

		// 재생은 직접 하지 않고 위치만 받아서 보여 준다.
		if (UAnimSingleNodeInstance* Instance = Component->GetSingleNodeInstance())
		{
			// 프리뷰 인스턴스는 기본으로 루트 모션을 뽑아내고 root를 제자리에 묶는다.
			// 그러면 Enable Root Motion이 켜진 애니메이션만 제자리에서 재생되므로, 뽑지 않고 적힌 그대로 보여 준다.
			Instance->SetRootMotionMode(ERootMotionMode::NoRootMotionExtraction);
			Instance->SetLooping(false);
			Instance->SetPlaying(false);
			Instance->SetPosition(0.f, false);
		}
		return Mesh != nullptr;
	}

	/** 비교 캐릭터를 기준 캐릭터의 화면 오른쪽(+X)에 겹치지 않게 놓는다. */
	void LayoutComponents()
	{
		constexpr double Gap = 40.0;
		double Offset = 0.0;
		if (MeshComponent->GetSkeletalMeshAsset() && CompareComponent->GetSkeletalMeshAsset())
		{
			Offset = MeshComponent->Bounds.BoxExtent.X + CompareComponent->Bounds.BoxExtent.X + Gap;
		}
		CompareComponent->SetWorldLocation(FVector(Offset, 0.0, 0.0));
	}

	FPreviewScene PreviewScene;
	TObjectPtr<UDebugSkelMeshComponent> MeshComponent;
	TObjectPtr<UDebugSkelMeshComponent> CompareComponent;
};

SHMocapPreview::SHMocapPreview() = default;

SHMocapPreview::~SHMocapPreview()
{
	if (UObjectInitialized() && MediaPlayer.IsValid())
	{
		MediaPlayer->Close();
	}
}

void SHMocapPreview::Construct(const FArguments& InArgs)
{
	MediaPlayer.Reset(NewObject<UMediaPlayer>(GetTransientPackage()));
	MediaPlayer->PlayOnOpen = false;

	MediaTexture.Reset(NewObject<UMediaTexture>(GetTransientPackage()));
	MediaTexture->AutoClear = true;
	MediaTexture->SetMediaPlayer(MediaPlayer.Get());
	MediaTexture->UpdateResource();

	MediaSource.Reset(NewObject<UFileMediaSource>(GetTransientPackage()));

	VideoBrush.DrawAs = ESlateBrushDrawType::Image;
	VideoBrush.ImageSize = FVector2D(1920.0, 1080.0);
	VideoBrush.SetResourceObject(MediaTexture.Get());

	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.f)
		[
			BuildAssetBar()
		]

		+ SVerticalBox::Slot()
		.FillHeight(1.f)
		[
			SNew(SOverlay)

			+ SOverlay::Slot()
			[
				SNew(SSplitter)
				.Orientation(Orient_Horizontal)

				+ SSplitter::Slot()
				.Value(0.5f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(4.f, 2.f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[
							SNew(STextBlock).Text(LOCTEXT("SourceVideo", "원본 영상"))
						]
						+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(8.f, 0.f)
						[
							SNew(STextBlock)
							.Text_Lambda([this]() { return FText::FromString(LoadedVideoName); })
							.ColorAndOpacity(FSlateColor::UseSubduedForeground())
							.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[
							SNew(SButton)
							.IsEnabled_Lambda([this]() { return IsLoaded(); })
							.Text(LOCTEXT("PickVideo", "영상 선택…"))
							.ToolTipText(LOCTEXT("PickVideoTooltip", "지금 보고 있는 결과 애니메이션의 원본 영상 파일을 고릅니다. 고른 영상은 이 애니메이션에 기억되어 다음부터 자동으로 열립니다."))
							.OnClicked(this, &SHMocapPreview::OnPickVideoClicked)
						]
					]
					+ SVerticalBox::Slot().FillHeight(1.f)
					[
						SNew(SOverlay)

						+ SOverlay::Slot()
						[
							SNew(SBorder)
							.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
							.BorderBackgroundColor(FLinearColor::Black)
							.Padding(0.f)
							[
								SNew(SScaleBox)
								.Stretch(EStretch::ScaleToFit)
								[
									SAssignNew(VideoImage, SImage).Image(&VideoBrush)
								]
							]
						]

						// 영상이 연결되지 않았거나 열지 못했을 때의 안내.
						+ SOverlay::Slot()
						.HAlign(HAlign_Center)
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Visibility_Lambda([this]() { return GetVideoHintText().IsEmpty() ? EVisibility::Collapsed : EVisibility::HitTestInvisible; })
							.Text(this, &SHMocapPreview::GetVideoHintText)
							.Justification(ETextJustify::Center)
						]
					]
				]

				+ SSplitter::Slot()
				.Value(0.5f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(4.f, 2.f)
					[
						SNew(STextBlock)
						.Text_Lambda([this]()
						{
							return CompareAnimation.IsValid()
								? LOCTEXT("ResultAndCompare", "결과 애니메이션 (왼쪽)  |  비교 애니메이션 (오른쪽)")
								: LOCTEXT("ResultAnimation", "결과 애니메이션");
						})
					]
					+ SVerticalBox::Slot().FillHeight(1.f)
					[
						SAssignNew(Viewport, SHMocapPreviewViewport)
					]
				]
			]

			// 아무것도 불러오지 않았을 때의 안내.
			+ SOverlay::Slot()
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
				.Padding(12.f)
				.Visibility_Lambda([this]() { return IsLoaded() ? EVisibility::Collapsed : EVisibility::HitTestInvisible; })
				[
					SNew(STextBlock)
					.Text(LOCTEXT("EmptyHint", "완료된 영상을 목록에서 선택하거나, 위에서 결과 애니메이션 에셋을 직접 고르세요."))
				]
			]
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.f)
		[
			BuildTransportBar()
		]
	];
}

TSharedRef<SWidget> SHMocapPreview::BuildAssetBar()
{
	return SNew(SHorizontalBox)

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.f, 0.f, 6.f, 0.f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("AnimationPicker", "결과 애니메이션"))
			.ToolTipText(LOCTEXT("AnimationPickerTooltip", "목록에서 완료된 영상을 고르면 자동으로 채워집니다. 예전에 만든 애니메이션을 직접 골라도 됩니다. 원본 영상은 기록이 있으면 자동으로 열리고, 없으면 [영상 선택…]으로 고릅니다."))
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
		[
			SNew(SObjectPropertyEntryBox)
			.AllowedClass(UAnimSequence::StaticClass())
			.ObjectPath(this, &SHMocapPreview::GetAnimationPath)
			.OnObjectChanged(this, &SHMocapPreview::OnAnimationPicked)
			.AllowClear(true)
			.DisplayThumbnail(false)
			.DisplayUseSelected(true)
			.DisplayBrowse(true)
		]

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(16.f, 0.f, 6.f, 0.f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("ComparePicker", "비교 애니메이션"))
			.ToolTipText(LOCTEXT("ComparePickerTooltip", "결과 옆에 같은 시간으로 함께 재생할 애니메이션입니다. 우리 캐릭터로 리타깃한 애니메이션을 고르면 원본과 비교할 수 있습니다."))
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
		[
			SNew(SObjectPropertyEntryBox)
			.IsEnabled_Lambda([this]() { return IsLoaded(); })
			.AllowedClass(UAnimSequence::StaticClass())
			.ObjectPath(this, &SHMocapPreview::GetCompareAnimationPath)
			.OnObjectChanged(this, &SHMocapPreview::OnCompareAnimationPicked)
			.AllowClear(true)
			.DisplayThumbnail(false)
			.DisplayUseSelected(true)
			.DisplayBrowse(true)
		];
}

FString SHMocapPreview::GetAnimationPath() const
{
	return Animation.IsValid() ? Animation->GetPathName() : FString();
}

FString SHMocapPreview::GetCompareAnimationPath() const
{
	return CompareAnimation.IsValid() ? CompareAnimation->GetPathName() : FString();
}

void SHMocapPreview::OnAnimationPicked(const FAssetData& AssetData)
{
	if (UAnimSequence* Picked = Cast<UAnimSequence>(AssetData.GetAsset()))
	{
		// 영상 경로를 비우면 이 애니메이션에 기록된 원본 영상을 찾아서 연다.
		Load(FString(), Picked);
	}
	else
	{
		SetCompareAnimation(nullptr);
		Unload();
	}
}

void SHMocapPreview::OnCompareAnimationPicked(const FAssetData& AssetData)
{
	SetCompareAnimation(Cast<UAnimSequence>(AssetData.GetAsset()));
}

void SHMocapPreview::SetCompareAnimation(UAnimSequence* InAnimation)
{
	CompareAnimation.Reset(InAnimation);
	if (Viewport.IsValid() && !Viewport->SetCompareAnimation(InAnimation) && InAnimation)
	{
		ShowPreviewWarning(LOCTEXT("CompareNoMesh", "비교 애니메이션에 쓸 프리뷰 메쉬를 찾지 못했습니다. 애니메이션이나 스켈레톤에 프리뷰 메쉬를 지정하세요."));
	}
}

TSharedRef<SWidget> SHMocapPreview::BuildTransportBar()
{
	return SNew(SHorizontalBox)
		.IsEnabled_Lambda([this]() { return IsLoaded(); })

		+ SHorizontalBox::Slot().AutoWidth().Padding(2.f)
		[
			SNew(SButton)
			.Text_Lambda([this]() { return bPlaying ? LOCTEXT("Pause", "일시정지") : LOCTEXT("Play", "재생"); })
			.OnClicked(this, &SHMocapPreview::OnPlayPauseClicked)
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(2.f)
		[
			SNew(SButton)
			.Text(LOCTEXT("ToStart", "처음으로"))
			.OnClicked_Lambda([this]() { SeekTo(0.f); return FReply::Handled(); })
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(2.f)
		[
			SNew(SButton)
			.Text(LOCTEXT("PrevFrame", "◀ 1프레임"))
			.ToolTipText(LOCTEXT("PrevFrameTooltip", "한 프레임 뒤로 (일시정지됩니다)"))
			.OnClicked_Lambda([this]() { StepFrames(-1); return FReply::Handled(); })
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(2.f)
		[
			SNew(SButton)
			.Text(LOCTEXT("NextFrame", "1프레임 ▶"))
			.ToolTipText(LOCTEXT("NextFrameTooltip", "한 프레임 앞으로 (일시정지됩니다)"))
			.OnClicked_Lambda([this]() { StepFrames(1); return FReply::Handled(); })
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(6.f, 2.f)
		[
			SNew(SSlider)
			.MinValue(0.f)
			.MaxValue(1.f)
			.Value_Lambda([this]() { return Duration > 0.f ? CurrentTime / Duration : 0.f; })
			.OnMouseCaptureBegin(this, &SHMocapPreview::OnScrubBegin)
			.OnMouseCaptureEnd(this, &SHMocapPreview::OnScrubEnd)
			.OnValueChanged(this, &SHMocapPreview::OnScrubValueChanged)
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.f, 2.f)
		[
			SNew(STextBlock).Text(this, &SHMocapPreview::GetTimeText)
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.f, 2.f)
		[
			SNew(SCheckBox)
			.IsChecked_Lambda([this]() { return bLoop ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { bLoop = State == ECheckBoxState::Checked; })
			[
				SNew(STextBlock).Text(LOCTEXT("Loop", "반복"))
			]
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(2.f)
		[
			SNew(SButton)
			.Text(LOCTEXT("FrameCamera", "카메라 맞춤"))
			.ToolTipText(LOCTEXT("FrameCameraTooltip", "결과 애니메이션 뷰를 정면 전신 구도로 되돌립니다. 뷰 안에서는 마우스로 자유롭게 돌려 볼 수 있습니다."))
			.OnClicked_Lambda([this]()
			{
				if (Viewport.IsValid()) { Viewport->FrameCamera(); }
				return FReply::Handled();
			})
		];
}

bool SHMocapPreview::Load(const FString& VideoPath, UAnimSequence* InAnimation)
{
	Unload();
	if (!InAnimation)
	{
		return false;
	}

	Animation.Reset(InAnimation);
	Duration = InAnimation->GetPlayLength();
	const double Interval = InAnimation->GetSamplingFrameRate().AsInterval();
	FrameInterval = Interval > 0.0 ? static_cast<float>(Interval) : 1.f / 30.f;
	CurrentTime = 0.f;

	if (Viewport.IsValid() && !Viewport->SetAnimation(InAnimation))
	{
		ShowPreviewWarning(LOCTEXT("AnimationNoMesh", "결과 애니메이션에 쓸 프리뷰 메쉬를 찾지 못했습니다. 애니메이션이나 스켈레톤에 프리뷰 메쉬를 지정하세요."));
	}

	// 영상은 비동기로 열린다. 없거나 못 열어도 애니메이션은 자체 시계로 재생된다.
	OpenVideo(VideoPath.IsEmpty() ? HMocapSourceVideo::Find(InAnimation->GetPathName()) : VideoPath);

	SetPlaying(true);
	return true;
}

bool SHMocapPreview::OpenVideo(const FString& VideoPath)
{
	bVideoConfigured = false;
	VideoSyncHold = 0.f;
	LoadedVideoName.Reset();
	if (!MediaPlayer.IsValid())
	{
		return false;
	}

	MediaPlayer->Close();
	if (VideoPath.IsEmpty())
	{
		return false;
	}
	if (!FPaths::FileExists(VideoPath))
	{
		UE_LOG(LogHMocapTool, Warning, TEXT("프리뷰 영상 파일이 없습니다: %s"), *VideoPath);
		return false;
	}

	MediaSource->SetFilePath(VideoPath);
	if (!MediaPlayer->OpenSource(MediaSource.Get()))
	{
		UE_LOG(LogHMocapTool, Warning, TEXT("프리뷰 영상을 열 수 없습니다: %s"), *VideoPath);
		return false;
	}

	UE_LOG(LogHMocapTool, Log, TEXT("프리뷰 영상 열기: %s"), *VideoPath);
	LoadedVideoName = FPaths::GetCleanFilename(VideoPath);
	return true;
}

FReply SHMocapPreview::OnPickVideoClicked()
{
	IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
	if (!Desktop || !IsLoaded())
	{
		return FReply::Handled();
	}

	TArray<FString> Paths;
	if (Desktop->OpenFileDialog(FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared()),
		LOCTEXT("ChooseVideo", "원본 영상 선택").ToString(), LastVideoDirectory, FString(),
		LOCTEXT("VideoFilter", "영상 파일 (*.mp4;*.mov)|*.mp4;*.mov").ToString(), EFileDialogFlags::None, Paths)
		&& !Paths.IsEmpty())
	{
		const FString VideoPath = FPaths::ConvertRelativePathToFull(Paths[0]);
		LastVideoDirectory = FPaths::GetPath(VideoPath);
		if (OpenVideo(VideoPath))
		{
			HMocapSourceVideo::Remember(Animation->GetPathName(), VideoPath);
		}
		else
		{
			ShowPreviewWarning(LOCTEXT("VideoOpenFailed", "영상을 열지 못했습니다. 출력 로그를 확인하세요."));
		}
	}
	return FReply::Handled();
}

FText SHMocapPreview::GetVideoHintText() const
{
	if (!IsLoaded())
	{
		// 아무것도 불러오지 않았을 때는 전체 안내가 따로 뜬다.
		return FText::GetEmpty();
	}
	if (LoadedVideoName.IsEmpty())
	{
		return LOCTEXT("NoVideoHint", "연결된 원본 영상이 없습니다.\n위의 [영상 선택…]으로 이 애니메이션의 원본 영상을 고르세요.");
	}
	if (MediaPlayer.IsValid() && MediaPlayer->HasError())
	{
		return LOCTEXT("VideoErrorHint", "영상을 재생하지 못했습니다. 출력 로그를 확인하세요.");
	}
	return FText::GetEmpty();
}

void SHMocapPreview::Unload()
{
	bPlaying = false;
	bScrubbing = false;
	bVideoConfigured = false;
	VideoSyncHold = 0.f;
	CurrentTime = 0.f;
	Duration = 0.f;
	LoadedVideoName.Reset();
	if (MediaPlayer.IsValid())
	{
		MediaPlayer->Close();
	}
	if (Viewport.IsValid() && Animation.IsValid())
	{
		Viewport->SetAnimation(nullptr);
	}
	Animation.Reset();
}

bool SHMocapPreview::IsLoaded() const
{
	return Animation.IsValid();
}

UAnimSequence* SHMocapPreview::GetAnimation() const
{
	return Animation.Get();
}

bool SHMocapPreview::IsVideoReady() const
{
	return MediaPlayer.IsValid() && MediaPlayer->IsReady();
}

void SHMocapPreview::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

	if (!IsLoaded())
	{
		return;
	}

	const bool bVideoReady = IsVideoReady();
	if (bVideoReady && !bVideoConfigured)
	{
		// 영상이 방금 열렸다. 반복은 직접 처리하므로 플레이어 반복은 끄고 현재 위치로 맞춘다.
		bVideoConfigured = true;
		MediaPlayer->SetLooping(false);
		MediaPlayer->Seek(FTimespan::FromSeconds(CurrentTime));
		VideoSyncHold = VideoSyncHoldSeconds;
		if (bPlaying)
		{
			MediaPlayer->Play();
		}
	}

	if (bPlaying && !bScrubbing)
	{
		if (VideoSyncHold > 0.f)
		{
			VideoSyncHold -= InDeltaTime;
			CurrentTime += InDeltaTime;
		}
		else if (bVideoReady && MediaPlayer->IsPlaying())
		{
			// 영상이 기준. 애니메이션이 영상 프레임을 따라간다.
			CurrentTime = static_cast<float>(MediaPlayer->GetTime().GetTotalSeconds());
		}
		else
		{
			CurrentTime += InDeltaTime;
		}

		if (CurrentTime >= Duration)
		{
			if (bLoop)
			{
				SeekTo(0.f);
				if (bVideoReady)
				{
					MediaPlayer->Play();
				}
			}
			else
			{
				CurrentTime = Duration;
				SetPlaying(false);
			}
		}
	}

	if (Viewport.IsValid())
	{
		Viewport->SetTime(FMath::Clamp(CurrentTime, 0.f, Duration));
	}

	// 영상 해상도는 첫 프레임이 나온 뒤에야 알 수 있다.
	const int32 Width = MediaTexture->GetWidth();
	const int32 Height = MediaTexture->GetHeight();
	if (Width > 0 && Height > 0 && VideoSize != FIntPoint(Width, Height))
	{
		VideoSize = FIntPoint(Width, Height);
		VideoBrush.ImageSize = FVector2D(Width, Height);
		if (VideoImage.IsValid())
		{
			VideoImage->Invalidate(EInvalidateWidgetReason::Layout);
		}
	}
}

void SHMocapPreview::SetPlaying(bool bInPlaying)
{
	if (bInPlaying && CurrentTime >= Duration)
	{
		SeekTo(0.f);
	}
	bPlaying = bInPlaying;
	if (IsVideoReady())
	{
		if (bPlaying)
		{
			MediaPlayer->Play();
		}
		else
		{
			MediaPlayer->Pause();
		}
	}
}

void SHMocapPreview::SeekTo(float Time)
{
	CurrentTime = FMath::Clamp(Time, 0.f, Duration);
	if (IsVideoReady())
	{
		MediaPlayer->Seek(FTimespan::FromSeconds(CurrentTime));
		VideoSyncHold = VideoSyncHoldSeconds;
	}
}

void SHMocapPreview::StepFrames(int32 FrameDelta)
{
	SetPlaying(false);
	// 프레임 경계에 맞춘 뒤 이동한다.
	const int32 Frame = FMath::RoundToInt(CurrentTime / FrameInterval) + FrameDelta;
	SeekTo(Frame * FrameInterval);
}

FReply SHMocapPreview::OnPlayPauseClicked()
{
	SetPlaying(!bPlaying);
	return FReply::Handled();
}

void SHMocapPreview::OnScrubBegin()
{
	bScrubbing = true;
	bResumeAfterScrub = bPlaying;
	SetPlaying(false);
}

void SHMocapPreview::OnScrubEnd()
{
	bScrubbing = false;
	if (bResumeAfterScrub)
	{
		SetPlaying(true);
	}
}

void SHMocapPreview::OnScrubValueChanged(float Value)
{
	SeekTo(Value * Duration);
}

FText SHMocapPreview::GetTimeText() const
{
	if (!IsLoaded())
	{
		return LOCTEXT("NoTime", "--:--");
	}
	const int32 Frame = FMath::RoundToInt(CurrentTime / FrameInterval);
	const int32 TotalFrames = FMath::RoundToInt(Duration / FrameInterval);
	return FText::Format(LOCTEXT("TimeFormat", "{0}  {1} / {2}초  (프레임 {3} / {4})"),
		FText::FromString(LoadedVideoName),
		FText::FromString(FString::Printf(TEXT("%.2f"), CurrentTime)),
		FText::FromString(FString::Printf(TEXT("%.2f"), Duration)),
		Frame, TotalFrames);
}

#undef LOCTEXT_NAMESPACE
