// Copyright Epic Games, Inc. All Rights Reserved.

#include "HMocapToolModule.h"
#include "Modules/ModuleManager.h"
#include "Misc/ConfigCacheIni.h"
#include "ToolMenus.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "Styling/AppStyle.h"
#include "UObject/UObjectBase.h"
#include "HMocapJobRunner.h"
#include "SHMocapToolTab.h"

#define LOCTEXT_NAMESPACE "HMocapToolModule"

IMPLEMENT_MODULE(FHMocapToolModule, HMocapTool)
DEFINE_LOG_CATEGORY(LogHMocapTool)

namespace HMocapTabs
{
	static const FName Mocap(TEXT("HMocapTool"));
}

namespace HMocapSourceVideo
{
	static const TCHAR* Section = TEXT("HMocapTool.SourceVideos");

	void Remember(const FString& AnimationPath, const FString& VideoPath)
	{
		if (!GConfig || AnimationPath.IsEmpty() || VideoPath.IsEmpty())
		{
			return;
		}
		GConfig->SetString(Section, *AnimationPath, *VideoPath, GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
	}

	FString Find(const FString& AnimationPath)
	{
		FString VideoPath;
		if (GConfig && !AnimationPath.IsEmpty())
		{
			GConfig->GetString(Section, *AnimationPath, VideoPath, GEditorPerProjectIni);
		}
		return VideoPath;
	}
}

FHMocapToolModule& FHMocapToolModule::Get()
{
	return FModuleManager::LoadModuleChecked<FHMocapToolModule>("HMocapTool");
}

UHMocapJobRunner& FHMocapToolModule::GetRunner()
{
	if (!Runner.IsValid())
	{
		Runner.Reset(NewObject<UHMocapJobRunner>());
	}
	return *Runner;
}

void FHMocapToolModule::StartupModule()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(HMocapTabs::Mocap,
		FOnSpawnTab::CreateRaw(this, &FHMocapToolModule::SpawnMocapTab))
		.SetDisplayName(LOCTEXT("TabTitle", "영상 모캡"))
		.SetMenuType(ETabSpawnerMenuType::Hidden);

	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FHMocapToolModule::RegisterMenus));
}

void FHMocapToolModule::ShutdownModule()
{
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(HMocapTabs::Mocap);
	}
	if (UObjectInitialized() && Runner.IsValid())
	{
		Runner->Shutdown();
	}
	Runner.Reset();
}

void FHMocapToolModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	// 메뉴바의 HTools는 ProjectHIEditor 모듈이 만든다. 그 모듈이 없는 프로젝트에서도 항목이 보이도록 없으면 직접 만든다.
	// (ProjectHIEditor가 나중에 같은 이름으로 다시 추가해도 항목은 하나로 유지된다.)
	UToolMenu* MainMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu");
	FToolMenuSection& MainSection = MainMenu->FindOrAddSection(NAME_None);
	if (!MainSection.FindEntry("HTools"))
	{
		FToolMenuEntry& HToolsEntry = MainSection.AddSubMenu(
			"HTools",
			LOCTEXT("HToolsMenu", "HTools"),
			LOCTEXT("HToolsMenuTooltip", "에디터 툴"),
			FNewToolMenuChoice());
		HToolsEntry.InsertPosition = FToolMenuInsert("Help", EToolMenuInsertType::After);
	}

	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.HTools");
	FToolMenuSection& Section = Menu->FindOrAddSection("Animation", LOCTEXT("Animation", "애니메이션"));
	Section.AddMenuEntry("HMocapTool",
		LOCTEXT("OpenTool", "영상 모캡"),
		LOCTEXT("OpenToolTooltip", "영상에서 바디 애니메이션을 생성합니다."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Viewports"),
		FUIAction(FExecuteAction::CreateLambda([]()
		{
			FGlobalTabmanager::Get()->TryInvokeTab(HMocapTabs::Mocap);
		})));
}

TSharedRef<SDockTab> FHMocapToolModule::SpawnMocapTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SHMocapToolTab)
		];
}

#undef LOCTEXT_NAMESPACE
