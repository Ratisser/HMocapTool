// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class HMocapTool : ModuleRules
{
	public HMocapTool(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine",

			// 에디터 UI
			"Slate", "SlateCore",
			"InputCore",   // SListView 템플릿이 EKeys를 참조한다
			"UnrealEd", "ToolMenus", "DesktopPlatform",
			"AssetTools", "AssetRegistry", "ContentBrowser",

			// 프리뷰: 원본 영상 재생, 애니메이션 에셋 선택 위젯
			"MediaAssets", "PropertyEditor",

			// 영상 -> 애니메이션 파이프라인 (엔진 플러그인)
			"MetaHumanPerformance", "MetaHumanPipelineCore",
			"CaptureDataCore", "CaptureManagerEditorSettings",

			// 우리 캐릭터로 리타깃 (IK 리타게터 일괄 변환)
			"IKRig", "IKRigEditor",

			// MetaHumanPerformance.h가 include하지만 그쪽에서는 Private 의존성이라 전파되지 않는 모듈
			"NNE", "ControlRig",

			// MetaHumanBodyTracker(Fab 플러그인)에는 의존하지 않는다. 없는 PC에서도 컴파일돼야 한다.
		});
	}
}
