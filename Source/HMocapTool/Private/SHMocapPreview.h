// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Styling/SlateBrush.h"
#include "UObject/StrongObjectPtr.h"

class SHMocapPreviewViewport;
class SImage;
class UAnimSequence;
class UFileMediaSource;
class UMediaPlayer;
class UMediaTexture;
struct FAssetData;

/**
 * 원본 영상(왼쪽)과 결과 애니메이션(오른쪽)을 같은 시간축으로 나란히 재생하는 프리뷰.
 * 영상이 재생 중이면 영상 시간을 기준으로 애니메이션 위치를 맞추고,
 * 영상을 열 수 없으면 자체 시계로 애니메이션만 재생한다.
 * 비교 애니메이션(리타깃 결과 등)을 지정하면 3D 뷰에서 결과 옆에 같은 시간으로 함께 재생한다.
 */
class SHMocapPreview : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHMocapPreview) {}
	SLATE_END_ARGS()

	SHMocapPreview();
	virtual ~SHMocapPreview() override;

	void Construct(const FArguments& InArgs);

	/**
	 * 영상 파일과 결과 애니메이션을 불러온다. 애니메이션이 없으면 false.
	 * 영상 경로를 비우면 이 애니메이션에 기록된 원본 영상(HMocapSourceVideo)을 찾아서 연다.
	 */
	bool Load(const FString& VideoPath, UAnimSequence* InAnimation);
	void Unload();
	bool IsLoaded() const;
	/** 지금 올라와 있는 결과 애니메이션. 없으면 nullptr. */
	UAnimSequence* GetAnimation() const;

	/** 결과 옆에 나란히 재생할 애니메이션. nullptr이면 비교를 끈다. Load/Unload와 무관하게 유지된다. */
	void SetCompareAnimation(UAnimSequence* InAnimation);

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

private:
	TSharedRef<SWidget> BuildAssetBar();
	TSharedRef<SWidget> BuildTransportBar();

	FString GetAnimationPath() const;
	FString GetCompareAnimationPath() const;
	void OnAnimationPicked(const FAssetData& AssetData);
	void OnCompareAnimationPicked(const FAssetData& AssetData);

	/** 지금 프리뷰에 원본 영상을 연다(애니메이션은 그대로). 재생 위치는 영상이 준비되면 맞춰진다. */
	bool OpenVideo(const FString& VideoPath);
	FReply OnPickVideoClicked();
	FText GetVideoHintText() const;

	void SetPlaying(bool bInPlaying);
	void SeekTo(float Time);
	void StepFrames(int32 FrameDelta);
	bool IsVideoReady() const;

	FReply OnPlayPauseClicked();
	void OnScrubBegin();
	void OnScrubEnd();
	void OnScrubValueChanged(float Value);
	FText GetTimeText() const;

	TStrongObjectPtr<UAnimSequence> Animation;
	TStrongObjectPtr<UAnimSequence> CompareAnimation;
	TStrongObjectPtr<UMediaPlayer> MediaPlayer;
	TStrongObjectPtr<UMediaTexture> MediaTexture;
	TStrongObjectPtr<UFileMediaSource> MediaSource;

	TSharedPtr<SHMocapPreviewViewport> Viewport;
	TSharedPtr<SImage> VideoImage;
	FSlateBrush VideoBrush;
	FIntPoint VideoSize = FIntPoint(1920, 1080);
	FString LoadedVideoName;       // 비어 있으면 연결된 영상이 없다.
	FString LastVideoDirectory;

	float Duration = 0.f;          // 애니메이션 길이(초). 슬라이더 범위.
	float FrameInterval = 1.f / 30.f;
	float CurrentTime = 0.f;
	bool bPlaying = false;
	bool bLoop = true;
	bool bScrubbing = false;
	bool bResumeAfterScrub = false;

	// 영상은 비동기로 열린다. 준비된 뒤 한 번만 현재 위치로 맞춘다.
	bool bVideoConfigured = false;
	// Seek 직후에는 플레이어가 이전 시간을 잠깐 보고하므로, 이 시간 동안은 자체 시계를 쓴다.
	float VideoSyncHold = 0.f;
};
