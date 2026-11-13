// Copyright Epic Games, Inc. All Rights Reserved.

#include "HMocapRetarget.h"
#include "HMocapToolModule.h"
#include "Retargeter/IKRetargeter.h"
#include "Retargeter/IKRetargetProcessor.h"
#include "Rig/IKRigDefinition.h"
#include "RetargetEditor/IKRetargetBatchOperation.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimEnums.h"
#include "Animation/Skeleton.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "AssetRegistry/AssetData.h"
#include "Engine/SkeletalMesh.h"
#include "ReferenceSkeleton.h"
#include "FileHelpers.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FrameRate.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "HMocapRetarget"

namespace
{
	const TCHAR* RetargetSection = TEXT("HMocapTool.Retarget");

	/** 본 하나의 로컬 트랜스폼을 키마다 읽는다. 트랙이 없는 본은 참조 포즈로 채운다. */
	TArray<FTransform> ReadLocalKeys(const IAnimationDataModel& Model, const FReferenceSkeleton& Ref, int32 BoneIndex, int32 NumKeys)
	{
		TArray<FTransform> Keys;
		const FName BoneName = Ref.GetBoneName(BoneIndex);
		if (Model.IsValidBoneTrackName(BoneName))
		{
			Model.GetBoneTrackTransforms(BoneName, Keys);
			if (Keys.Num() != NumKeys)
			{
				Keys.Reset(NumKeys);
				for (int32 Key = 0; Key < NumKeys; ++Key)
				{
					Keys.Add(Model.GetBoneTrackTransform(BoneName, FFrameNumber(Key)));
				}
			}
		}
		else
		{
			Keys.Init(Ref.GetRefBonePose()[BoneIndex], NumKeys);
		}
		return Keys;
	}

	void WriteLocalKeys(IAnimationDataController& Controller, const IAnimationDataModel& Model, FName BoneName, const TArray<FTransform>& Keys)
	{
		TArray<FVector> Positions;
		TArray<FQuat> Rotations;
		TArray<FVector> Scales;
		Positions.Reserve(Keys.Num());
		Rotations.Reserve(Keys.Num());
		Scales.Reserve(Keys.Num());
		for (const FTransform& Key : Keys)
		{
			Positions.Add(Key.GetLocation());
			Rotations.Add(Key.GetRotation().GetNormalized());
			Scales.Add(Key.GetScale3D());
		}

		constexpr bool bShouldTransact = false;
		if (!Model.IsValidBoneTrackName(BoneName))
		{
			Controller.AddBoneCurve(BoneName, bShouldTransact);
		}
		Controller.SetBoneTrackKeys(BoneName, Positions, Rotations, Scales, bShouldTransact);
	}

	/**
	 * 골반에 들어 있는 수평 이동을 root 뼈로 옮긴다.
	 *
	 * 새 root는 골반 바로 아래 바닥을 따라간다(높이·회전은 원래 root 그대로, bAlignStartRotation이면 시작 방향만큼 돌린다).
	 * root의 자식 본들은 컴포넌트 공간 위치가 그대로 유지되도록 로컬 값을 다시 계산하므로 몸의 포즈는 바뀌지 않는다.
	 * 경로를 다듬어도 마찬가지다 - root와 골반 사이의 상대 위치가 달라질 뿐이다.
	 */
	bool ApplyRootMotion(UAnimSequence& Animation, FName PelvisBone, const FHMocapRetargetSettings& Settings, FText& OutError)
	{
		const USkeleton* Skeleton = Animation.GetSkeleton();
		const IAnimationDataModel* Model = Animation.GetDataModel();
		if (!Skeleton || !Model)
		{
			OutError = LOCTEXT("NoAnimationData", "애니메이션 데이터를 읽을 수 없습니다.");
			return false;
		}

		const FReferenceSkeleton& Ref = Skeleton->GetReferenceSkeleton();
		int32 PelvisIndex = Ref.FindBoneIndex(PelvisBone);
		if (PelvisIndex == INDEX_NONE)
		{
			PelvisIndex = Ref.FindBoneIndex(TEXT("pelvis"));
		}
		if (PelvisIndex <= 0)
		{
			OutError = LOCTEXT("NoPelvis", "대상 스켈레톤에서 골반 뼈를 찾지 못했습니다. 대상 IK 릭의 Pelvis 설정을 확인하세요.");
			return false;
		}

		const int32 NumKeys = Model->GetNumberOfKeys();
		if (NumKeys < 2)
		{
			OutError = LOCTEXT("TooShort", "애니메이션이 너무 짧습니다.");
			return false;
		}

		const TArray<FTransform>& RefPose = Ref.GetRefBonePose();
		const TArray<FTransform> OldRoot = ReadLocalKeys(*Model, Ref, 0, NumKeys);

		// root의 직계 자식(골반, IK 본 등). 이 본들의 로컬 값만 다시 쓰면 된다.
		TArray<int32> RootChildren;
		TArray<TArray<FTransform>> ChildKeys;
		for (int32 BoneIndex = 1; BoneIndex < Ref.GetNum(); ++BoneIndex)
		{
			if (Ref.GetParentIndex(BoneIndex) == 0)
			{
				RootChildren.Add(BoneIndex);
				ChildKeys.Add(ReadLocalKeys(*Model, Ref, BoneIndex, NumKeys));
			}
		}

		// 골반의 컴포넌트 공간 트랜스폼 (애니메이션, 참조 포즈).
		TArray<FTransform> PelvisComponent = ReadLocalKeys(*Model, Ref, PelvisIndex, NumKeys);
		FTransform RefPelvisComponent = RefPose[PelvisIndex];
		for (int32 Parent = Ref.GetParentIndex(PelvisIndex); Parent != INDEX_NONE; Parent = Ref.GetParentIndex(Parent))
		{
			const TArray<FTransform> ParentKeys = (Parent == 0) ? OldRoot : ReadLocalKeys(*Model, Ref, Parent, NumKeys);
			for (int32 Key = 0; Key < NumKeys; ++Key)
			{
				PelvisComponent[Key] = PelvisComponent[Key] * ParentKeys[Key];
			}
			RefPelvisComponent = RefPelvisComponent * RefPose[Parent];
		}

		// 이동 경로(XY) 다듬기. 양 끝으로 갈수록 창을 줄여서 시작점과 끝점은 원래 값을 유지한다.
		const int32 LastKey = NumKeys - 1;
		const int32 Radius = FMath::Max(0, FMath::RoundToInt32(Settings.SmoothSeconds * Model->GetFrameRate().AsDecimal() * 0.5));
		TArray<FVector2D> Path;
		Path.Reserve(NumKeys);
		for (int32 Key = 0; Key < NumKeys; ++Key)
		{
			const int32 Window = FMath::Min3(Radius, Key, LastKey - Key);
			FVector2D Sum = FVector2D::ZeroVector;
			for (int32 Other = Key - Window; Other <= Key + Window; ++Other)
			{
				const FVector Location = PelvisComponent[Other].GetLocation();
				Sum += FVector2D(Location.X, Location.Y);
			}
			Path.Add(Sum / static_cast<double>(2 * Window + 1));
		}

		// 시작 방향: 첫 프레임의 골반이 참조 포즈보다 수직축으로 얼마나 돌아가 있는지.
		FQuat StartYaw = FQuat::Identity;
		if (Settings.bAlignStartRotation)
		{
			const FQuat Delta =PelvisComponent[0].GetRotation() * RefPelvisComponent.GetRotation().Inverse();
			FQuat Swing;
			FQuat Twist;
			Delta.ToSwingTwist(FVector::UpVector, Swing, Twist);
			StartYaw = Twist.GetNormalized();
		}

		// 컴포넌트 공간에서의 새 root.
		TArray<FTransform> NewRoot;
		NewRoot.Reserve(NumKeys);
		for (int32 Key = 0; Key < NumKeys; ++Key)
		{
			const FTransform& Old = OldRoot[Key];
			NewRoot.Add(FTransform(
				(StartYaw * Old.GetRotation()).GetNormalized(),
				FVector(Path[Key].X, Path[Key].Y, Old.GetLocation().Z),
				Old.GetScale3D()));
		}

		// 자식 본: 컴포넌트 공간 위치를 유지하도록 새 root 기준 로컬 값으로 바꾼다.
		for (int32 ChildIndex = 0; ChildIndex < RootChildren.Num(); ++ChildIndex)
		{
			TArray<FTransform>& Keys = ChildKeys[ChildIndex];
			for (int32 Key = 0; Key < NumKeys; ++Key)
			{
				const FTransform Component = Keys[Key] * OldRoot[Key];
				Keys[Key] = Component.GetRelativeTransform(NewRoot[Key]);
			}
		}

		// 시작 정렬: 첫 프레임의 root가 참조 포즈의 위치(원점)·방향(정면) 중 고른 것에 오도록 전체를 같은 만큼 옮긴다.
		// 방향만 맞추면 시작 위치를 축으로 돌고, 위치만 맞추면 돌리지 않고 평행 이동한다.
		TArray<FTransform> RootKeys = NewRoot;
		if (Settings.bAlignStartLocation || Settings.bAlignStartRotation)
		{
			const FTransform StartTarget(
				Settings.bAlignStartRotation ? RefPose[0].GetRotation() : NewRoot[0].GetRotation(),
				Settings.bAlignStartLocation ? RefPose[0].GetLocation() : NewRoot[0].GetLocation(),
				NewRoot[0].GetScale3D());
			const FTransform ToStart = NewRoot[0].Inverse() * StartTarget;
			for (FTransform& Key : RootKeys)
			{
				Key = Key * ToStart;
			}
		}

		IAnimationDataController& Controller = Animation.GetController();
		constexpr bool bShouldTransact = false;
		Controller.OpenBracket(LOCTEXT("RootMotionBracket", "골반 이동을 루트 모션으로 변환"), bShouldTransact);
		WriteLocalKeys(Controller, *Model, Ref.GetBoneName(0), RootKeys);
		for (int32 ChildIndex = 0; ChildIndex < RootChildren.Num(); ++ChildIndex)
		{
			WriteLocalKeys(Controller, *Model, Ref.GetBoneName(RootChildren[ChildIndex]), ChildKeys[ChildIndex]);
		}
		Controller.CloseBracket(bShouldTransact);

		Animation.bEnableRootMotion = true;
		Animation.RootMotionRootLock = ERootMotionRootLock::RefPose;
		Animation.bForceRootLock = false;
		Animation.MarkPackageDirty();

		const FVector Travel = RootKeys[LastKey].GetLocation() - RootKeys[0].GetLocation();
		UE_LOG(LogHMocapTool, Log, TEXT("루트 모션 변환: %s, 골반 '%s', 키 %d개, 다듬기 반경 %d프레임, 시작 맞춤(위치 %d, 방향 %d), 총 이동 (%.1f, %.1f) cm"),
			*Animation.GetName(), *Ref.GetBoneName(PelvisIndex).ToString(), NumKeys, Radius,
			Settings.bAlignStartLocation ? 1 : 0, Settings.bAlignStartRotation ? 1 : 0, Travel.X, Travel.Y);
		return true;
	}
}

void FHMocapRetargetSettings::Load()
{
	if (!GConfig)
	{
		return;
	}
	FString Path;
	GConfig->GetBool(RetargetSection, TEXT("bEnabled"), bEnabled, GEditorPerProjectIni);
	if (GConfig->GetString(RetargetSection, TEXT("Retargeter"), Path, GEditorPerProjectIni))
	{
		Retargeter = FSoftObjectPath(Path);
	}
	if (GConfig->GetString(RetargetSection, TEXT("TargetMesh"), Path, GEditorPerProjectIni))
	{
		TargetMesh = FSoftObjectPath(Path);
	}
	GConfig->GetString(RetargetSection, TEXT("OutputFolder"), OutputFolder, GEditorPerProjectIni);
	GConfig->GetBool(RetargetSection, TEXT("bRootMotion"), bRootMotion, GEditorPerProjectIni);
	GConfig->GetBool(RetargetSection, TEXT("bAlignStartLocation"), bAlignStartLocation, GEditorPerProjectIni);
	GConfig->GetBool(RetargetSection, TEXT("bAlignStartRotation"), bAlignStartRotation, GEditorPerProjectIni);
	GConfig->GetFloat(RetargetSection, TEXT("SmoothSeconds"), SmoothSeconds, GEditorPerProjectIni);
}

void FHMocapRetargetSettings::Save() const
{
	if (!GConfig)
	{
		return;
	}
	GConfig->SetBool(RetargetSection, TEXT("bEnabled"), bEnabled, GEditorPerProjectIni);
	GConfig->SetString(RetargetSection, TEXT("Retargeter"), *Retargeter.ToString(), GEditorPerProjectIni);
	GConfig->SetString(RetargetSection, TEXT("TargetMesh"), *TargetMesh.ToString(), GEditorPerProjectIni);
	GConfig->SetString(RetargetSection, TEXT("OutputFolder"), *OutputFolder, GEditorPerProjectIni);
	GConfig->SetBool(RetargetSection, TEXT("bRootMotion"), bRootMotion, GEditorPerProjectIni);
	GConfig->SetBool(RetargetSection, TEXT("bAlignStartLocation"), bAlignStartLocation, GEditorPerProjectIni);
	GConfig->SetBool(RetargetSection, TEXT("bAlignStartRotation"), bAlignStartRotation, GEditorPerProjectIni);
	GConfig->SetFloat(RetargetSection, TEXT("SmoothSeconds"), SmoothSeconds, GEditorPerProjectIni);
	GConfig->Flush(false, GEditorPerProjectIni);
}

UAnimSequence* HMocapRetarget::Convert(UAnimSequence* Source, const FHMocapRetargetSettings& Settings, FText& OutError)
{
	OutError = FText::GetEmpty();
	if (!Source || !Source->GetSkeleton())
	{
		OutError = LOCTEXT("NoSource", "변환할 애니메이션이 없습니다.");
		return nullptr;
	}

	UIKRetargeter* Retargeter = Cast<UIKRetargeter>(Settings.Retargeter.TryLoad());
	if (!Retargeter)
	{
		OutError = LOCTEXT("NoRetargeter", "IK 리타게터 에셋을 지정하세요.");
		return nullptr;
	}

	USkeletalMesh* TargetMesh = Cast<USkeletalMesh>(Settings.TargetMesh.TryLoad());
	if (!TargetMesh)
	{
		TargetMesh = Retargeter->GetPreviewMesh(ERetargetSourceOrTarget::Target);
	}
	if (!TargetMesh || !TargetMesh->GetSkeleton())
	{
		OutError = LOCTEXT("NoTargetMesh", "대상 메쉬를 지정하세요. (리타게터의 대상 IK 릭에 프리뷰 메쉬가 없습니다.)");
		return nullptr;
	}
	if (TargetMesh->GetSkeleton() == Source->GetSkeleton())
	{
		OutError = LOCTEXT("AlreadyTarget", "이미 대상 캐릭터의 스켈레톤으로 된 애니메이션입니다. 모캡 결과 애니메이션을 고르세요.");
		return nullptr;
	}

	// 리타게터의 원본 쪽 릭이 이 애니메이션의 스켈레톤과 맞는지 가볍게 확인한다.
	const UIKRigDefinition* SourceRig = Retargeter->GetIKRig(ERetargetSourceOrTarget::Source);
	const UIKRigDefinition* TargetRig = Retargeter->GetIKRig(ERetargetSourceOrTarget::Target);
	if (!SourceRig || !TargetRig)
	{
		OutError = LOCTEXT("NoRigs", "리타게터에 원본·대상 IK 릭이 모두 지정되어 있어야 합니다.");
		return nullptr;
	}
	if (Source->GetSkeleton()->GetReferenceSkeleton().FindBoneIndex(SourceRig->GetPelvis()) == INDEX_NONE)
	{
		OutError = FText::Format(LOCTEXT("SourceRigMismatch", "리타게터의 원본 IK 릭({0})이 이 애니메이션의 스켈레톤({1})과 맞지 않습니다."),
			FText::FromString(SourceRig->GetName()), FText::FromString(Source->GetSkeleton()->GetName()));
		return nullptr;
	}

	// 원본 메쉬는 애니메이션 자신의 프리뷰 메쉬를 우선한다(이 애니메이션을 확실히 재생할 수 있는 메쉬).
	USkeletalMesh* SourceMesh = Source->GetPreviewMesh(true);
	if (!SourceMesh)
	{
		SourceMesh = Source->GetSkeleton()->GetPreviewMesh(true);
	}
	if (!SourceMesh)
	{
		SourceMesh = Retargeter->GetPreviewMesh(ERetargetSourceOrTarget::Source);
	}
	if (!SourceMesh)
	{
		OutError = LOCTEXT("NoSourceMesh", "원본 메쉬를 찾지 못했습니다. 모캡 결과 스켈레톤에 프리뷰 메쉬를 지정하세요.");
		return nullptr;
	}

	FString Folder = Settings.OutputFolder;
	Folder.TrimStartAndEndInline();
	while (Folder.EndsWith(TEXT("/")))
	{
		Folder.LeftChopInline(1);
	}
	if (!Folder.IsEmpty() && !FPackageName::IsValidLongPackageName(Folder, true))
	{
		OutError = FText::Format(LOCTEXT("BadFolder", "변환 결과 폴더가 올바른 콘텐츠 경로가 아닙니다: {0}"), FText::FromString(Folder));
		return nullptr;
	}

	FIKRetargetBatchOperationInputs Inputs;
	Inputs.AssetsToRetarget.Add(FAssetData(Source));
	Inputs.SourceMesh = SourceMesh;
	Inputs.TargetMesh = TargetMesh;
	Inputs.IKRetargetAsset = Retargeter;
	Inputs.Suffix = TEXT("_RT");
	Inputs.TargetPath = Folder;
	Inputs.bUseSourcePath = Folder.IsEmpty();
	Inputs.bIncludeReferencedAssets = false;
	Inputs.bOverwriteExistingFiles = false;

	const TArray<FAssetData> Results = UIKRetargetBatchOperation::RunBatchRetarget(Inputs);
	UAnimSequence* Result = Results.IsEmpty() ? nullptr : Cast<UAnimSequence>(Results[0].GetAsset());
	if (!IsValid(Result) || Result == Source || Result->GetSkeleton() != TargetMesh->GetSkeleton())
	{
		OutError = LOCTEXT("RetargetFailed", "리타깃에 실패했거나 취소되었습니다. 출력 로그를 확인하세요.");
		return nullptr;
	}

	if (Settings.bRootMotion)
	{
		// 실패해도 리타깃 결과는 남긴다. 이유는 OutError로 알린다.
		ApplyRootMotion(*Result, TargetRig->GetPelvis(), Settings, OutError);
	}

	// 변환 결과만 골라도 프리뷰가 원본 영상을 찾을 수 있게 이어 준다.
	HMocapSourceVideo::Remember(Result->GetPathName(), HMocapSourceVideo::Find(Source->GetPathName()));

	if (!UEditorLoadingAndSavingUtils::SavePackages(TArray<UPackage*>{ Result->GetPackage() }, false) && OutError.IsEmpty())
	{
		OutError = LOCTEXT("SaveFailed", "변환한 애니메이션을 저장하지 못했습니다. 콘텐츠 브라우저에서 저장 상태를 확인하세요.");
	}
	return Result;
}

#undef LOCTEXT_NAMESPACE
