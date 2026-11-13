// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "UObject/StrongObjectPtr.h"

class FSpawnTabArgs;
class SDockTab;
class UHMocapJobRunner;

DECLARE_LOG_CATEGORY_EXTERN(LogHMocapTool, Log, All)

/**
 * 애니메이션 에셋이 어느 영상에서 나왔는지 기억해 둔다(프리뷰에서 원본 영상을 다시 찾는 용도).
 * 영상은 각자 PC의 로컬 파일이라 에셋이 아니라 개인 설정(EditorPerProjectUserSettings)에 적는다.
 */
namespace HMocapSourceVideo
{
	void Remember(const FString& AnimationPath, const FString& VideoPath);
	/** 기록이 없으면 빈 문자열. 파일이 아직 있는지는 확인하지 않는다. */
	FString Find(const FString& AnimationPath);
}

class FHMocapToolModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	virtual bool SupportsDynamicReloading() override { return false; }

	static FHMocapToolModule& Get();
	UHMocapJobRunner& GetRunner();

private:
	void RegisterMenus();
	TSharedRef<SDockTab> SpawnMocapTab(const FSpawnTabArgs& Args);

	TStrongObjectPtr<UHMocapJobRunner> Runner;
};
