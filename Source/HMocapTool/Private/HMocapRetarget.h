// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPath.h"

class UAnimSequence;

/** 모캡 결과(MetaHuman 스켈레톤)를 우리 캐릭터용 애니메이션으로 바꾸는 설정. 개인 설정에 저장된다. */
struct FHMocapRetargetSettings
{
	/** 영상 처리가 끝나면 자동으로 변환까지 한다. */
	bool bEnabled = false;

	/** UIKRetargeter. 원본 = 모캡 결과 스켈레톤, 대상 = 우리 캐릭터. */
	FSoftObjectPath Retargeter;

	/** USkeletalMesh. 비우면 리타게터의 대상 프리뷰 메쉬를 쓴다. */
	FSoftObjectPath TargetMesh;

	FString OutputFolder = TEXT("/Game/CaptureManager/Mocap/Retargeted");

	/** 골반에 들어 있는 수평 이동을 root 뼈로 옮긴다(루트 모션). */
	bool bRootMotion = true;

	/** 첫 프레임의 root가 원점에 오도록 애니메이션 전체를 옮긴다. */
	bool bAlignStartLocation = true;

	/** 첫 프레임의 몸이 정면을 보도록 애니메이션 전체를 시작 위치를 축으로 돌린다. */
	bool bAlignStartRotation = true;

	/** root 이동 경로를 다듬는 시간 폭(초). 0이면 다듬지 않는다. 몸의 포즈는 바뀌지 않는다. */
	float SmoothSeconds = 0.3f;

	void Load();
	void Save() const;
};

namespace HMocapRetarget
{
	/**
	 * Source를 복제해 리타깃하고, 설정에 따라 루트 모션으로 바꾼 뒤 저장한다. Source는 건드리지 않는다.
	 * 실패하면 nullptr와 OutError.
	 * 리타깃은 됐지만 루트 모션 변환만 실패하면 결과를 반환하면서 OutError에 이유를 적는다.
	 * 진행 창(취소 가능)이 뜨는 동기 작업이다.
	 */
	UAnimSequence* Convert(UAnimSequence* Source, const FHMocapRetargetSettings& Settings, FText& OutError);
}
