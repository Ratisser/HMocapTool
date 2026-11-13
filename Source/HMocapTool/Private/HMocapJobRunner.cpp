// Copyright Epic Games, Inc. All Rights Reserved.

#include "HMocapJobRunner.h"
#include "HMocapToolModule.h"
#include "MetaHumanPerformance.h"
#include "MetaHumanPerformanceExportUtils.h"
#include "Pipeline/PipelineData.h"
#include "CaptureData.h"
#include "Settings/CaptureManagerEditorSettings.h"
#include "Features/IModularFeatures.h"
#include "Modules/ModuleManager.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Factories/Factory.h"
#include "Animation/AnimSequence.h"
#include "FileHelpers.h"
#include "HAL/PlatformTime.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "HMocapJobRunner"

namespace
{
	bool IsFinished(EHMocapJobState State)
	{
		return State == EHMocapJobState::Done || State == EHMocapJobState::Failed || State == EHMocapJobState::Cancelled;
	}

	UClass* FindIngestLibrary()
	{
		const TCHAR* ClassPath = TEXT("/Script/CaptureManagerIngestBlueprint.CaptureManagerIngestBlueprintLibrary");
		UClass* Library = FindObject<UClass>(nullptr, ClassPath);
		if (!Library && FModuleManager::LoadModulePtr<IModuleInterface>("CaptureManagerIngestBlueprint"))
		{
			Library = FindObject<UClass>(nullptr, ClassPath);
		}
		return Library;
	}
}

bool UHMocapJobRunner::IsBodyTrackerAvailable()
{
	return IModularFeatures::Get().IsModularFeatureAvailable(FName(TEXT("MetaHumanBodyTrackerInterface")));
}

void UHMocapJobRunner::Log(const FText& Message)
{
	UE_LOG(LogHMocapTool, Log, TEXT("%s"), *Message.ToString());
	OnLog.Broadcast(Message);
}

void UHMocapJobRunner::AddVideos(const TArray<FString>& Paths)
{
	for (const FString& Path : Paths)
	{
		FString AbsolutePath = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(AbsolutePath);
		const FString Extension = FPaths::GetExtension(AbsolutePath).ToLower();
		if (!FPaths::FileExists(AbsolutePath) || (Extension != TEXT("mp4") && Extension != TEXT("mov")))
		{
			Log(FText::Format(LOCTEXT("InvalidVideo", "영상 파일을 추가할 수 없습니다: {0}"), FText::FromString(Path)));
			continue;
		}
		if (Jobs.ContainsByPredicate([&AbsolutePath](const TSharedPtr<FHMocapJob>& Job)
			{ return !IsFinished(Job->State) && Job->VideoPath.Equals(AbsolutePath, ESearchCase::IgnoreCase); }))
		{
			continue;
		}
		TSharedPtr<FHMocapJob> Job = MakeShared<FHMocapJob>();
		Job->VideoPath = AbsolutePath;
		Job->Slate = FPaths::GetBaseFilename(AbsolutePath);
		for (TCHAR& Character : Job->Slate)
		{
			if (!((Character >= 'A' && Character <= 'Z') || (Character >= 'a' && Character <= 'z')
				|| (Character >= '0' && Character <= '9') || Character == '_'))
			{
				Character = '_';
			}
		}
		if (Job->Slate.IsEmpty())
		{
			Job->Slate = TEXT("Video");
		}
		Jobs.Add(Job);
		Log(FText::Format(LOCTEXT("Queued", "영상 추가: {0}"), FText::FromString(FPaths::GetCleanFilename(AbsolutePath))));
	}
	OnJobsChanged.Broadcast();
}

void UHMocapJobRunner::RemoveJob(TSharedPtr<FHMocapJob> Job)
{
	if (Job != CurrentJob)
	{
		Jobs.Remove(Job);
		OnJobsChanged.Broadcast();
	}
}

void UHMocapJobRunner::ClearFinished()
{
	Jobs.RemoveAll([this](const TSharedPtr<FHMocapJob>& Job) { return Job != CurrentJob && IsFinished(Job->State); });
	OnJobsChanged.Broadcast();
}

bool UHMocapJobRunner::Start(const FHMocapSettings& InSettings)
{
	if (bRunning)
	{
		Log(LOCTEXT("AlreadyRunning", "이미 작업을 처리하고 있습니다."));
		return false;
	}
	if (!IsBodyTrackerAvailable())
	{
		Log(LOCTEXT("TrackerMissing", "바디 트래커 기능을 사용할 수 없습니다. 플러그인 설치와 활성화를 확인하세요."));
		return false;
	}
	if (!Jobs.ContainsByPredicate([](const TSharedPtr<FHMocapJob>& Job) { return Job->State == EHMocapJobState::Pending; }))
	{
		Log(LOCTEXT("NoPendingJobs", "대기 중인 영상이 없습니다."));
		return false;
	}
	if (!InSettings.OutputFolder.StartsWith(TEXT("/Game/")) || !FPackageName::IsValidLongPackageName(InSettings.OutputFolder))
	{
		Log(LOCTEXT("InvalidOutput", "출력 폴더는 /Game/으로 시작하는 유효한 콘텐츠 경로여야 합니다."));
		return false;
	}
	if (!FMath::IsFinite(InSettings.BodyHeightCm) || InSettings.BodyHeightCm < 100.f || InSettings.BodyHeightCm > 230.f)
	{
		Log(LOCTEXT("InvalidHeight", "키는 100~230cm 범위로 입력하세요."));
		return false;
	}
	Settings = InSettings;
	bRunning = true;
	bCancelRequested = false;
	Step = EStep::Next;
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UHMocapJobRunner::TickQueue));
	Log(LOCTEXT("QueueStarted", "영상 모캡 처리를 시작합니다."));
	OnJobsChanged.Broadcast();
	return true;
}

bool UHMocapJobRunner::TickQueue(float DeltaTime)
{
	if (!bRunning)
	{
		return false;
	}
	if (bCancelRequested && Step != EStep::WaitIngest && Step != EStep::WaitProcess)
	{
		Finish(EHMocapJobState::Cancelled, LOCTEXT("Cancelled", "취소됨"));
	}
	else if ((Step == EStep::WaitIngest || Step == EStep::WaitProcess) && bCallbackReady)
	{
		bCallbackReady = false;
		if (bCancelRequested || bCallbackAborted)
		{
			Finish(EHMocapJobState::Cancelled, LOCTEXT("Cancelled", "취소됨"));
		}
		else if (!bCallbackSuccess)
		{
			Finish(EHMocapJobState::Failed, CallbackError);
		}
		else
		{
			Step = Step == EStep::WaitIngest ? EStep::Process : EStep::Export;
		}
	}
	else if (Step == EStep::Next)
	{
		const TSharedPtr<FHMocapJob>* Pending = Jobs.FindByPredicate(
			[](const TSharedPtr<FHMocapJob>& Job) { return Job->State == EHMocapJobState::Pending; });
		if (!Pending)
		{
			bRunning = false;
			Log(LOCTEXT("QueueFinished", "대기 영상 처리가 끝났습니다."));
			OnJobsChanged.Broadcast();
		}
		else
		{
			CurrentJob = *Pending;
			BeginIngest();
		}
	}
	else if (Step == EStep::Process)
	{
		BeginProcessing();
	}
	else if (Step == EStep::Export)
	{
		ExportAnimation();
	}
	return bRunning;
}

int32 UHMocapJobRunner::FindTakeNumber()
{
	UCaptureManagerEditorSettings* CaptureSettings = GetMutableDefault<UCaptureManagerEditorSettings>();
	FString ImportTemplate = CaptureSettings ? CaptureSettings->GetVerifiedImportDirectory() : FString();
	if (ImportTemplate.IsEmpty())
	{
		ImportTemplate = TEXT("/Game/CaptureManager/Imports/{slate}_{take}");
	}
	// 비동기 MonoVideo 경로는 FAssetNamingStrategy에 device와 id를 빈 문자열로 넘긴다.
	ImportTemplate.ReplaceInline(TEXT("{device}"), TEXT(""), ESearchCase::CaseSensitive);
	ImportTemplate.ReplaceInline(TEXT("{id}"), TEXT(""), ESearchCase::CaseSensitive);
	ImportTemplate.ReplaceInline(TEXT("{slate}"), *CurrentJob->Slate, ESearchCase::CaseSensitive);
	FPaths::RemoveDuplicateSlashes(ImportTemplate);
	const bool bHasTakeToken = ImportTemplate.Contains(TEXT("{take}"), ESearchCase::CaseSensitive);
	FString TokenCheck = ImportTemplate.Replace(TEXT("{take}"), TEXT("1"), ESearchCase::CaseSensitive);
	FPaths::RemoveDuplicateSlashes(TokenCheck);
	if (TokenCheck.Contains(TEXT("{")) || !FPackageName::IsValidLongPackageName(TokenCheck, true))
	{
		// 엔진의 사용자 정의 토큰을 임의로 추측하면 기존 가져오기를 덮어쓸 수 있다.
		Log(LOCTEXT("UnsupportedImportTemplate", "가져오기 경로의 사용자 정의 토큰을 해석할 수 없습니다. Capture Manager 가져오기 경로에 기본 토큰만 사용하세요."));
		return INDEX_NONE;
	}
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	for (int32 Take = 1; Take <= 999; ++Take)
	{
		FString Candidate = bHasTakeToken
			? ImportTemplate.Replace(TEXT("{take}"), *FString::FromInt(Take), ESearchCase::CaseSensitive)
			: ImportTemplate;
		FPaths::RemoveDuplicateSlashes(Candidate);
		Registry.ScanPathsSynchronous(TArray<FString>{ Candidate });
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPath(FName(*Candidate), Assets, true);
		if (Assets.IsEmpty())
		{
			return Take;
		}
		if (!bHasTakeToken)
		{
			return INDEX_NONE;
		}
	}
	return INDEX_NONE;
}

void UHMocapJobRunner::BeginIngest()
{
	CurrentJob->State = EHMocapJobState::Ingesting;
	CurrentJob->Message = LOCTEXT("Ingesting", "가져오는 중");
	OnJobsChanged.Broadcast();
	Log(FText::Format(LOCTEXT("IngestStarted", "가져오기 시작: {0}"), FText::FromString(CurrentJob->VideoPath)));
	const int32 Take = FindTakeNumber();
	if (Take == INDEX_NONE)
	{
		Finish(EHMocapJobState::Failed, LOCTEXT("NoFreeTake", "사용 가능한 가져오기 경로가 없습니다. 경로 설정과 테이크 1~999를 확인하세요."));
		return;
	}
	UClass* Library = FindIngestLibrary();
	UFunction* Function = Library ? Library->FindFunctionByName(TEXT("IngestMonoVideo")) : nullptr;
	if (!Function)
	{
		Finish(EHMocapJobState::Failed, LOCTEXT("IngestFunctionMissing", "영상 가져오기 함수를 찾을 수 없습니다. Capture Manager 모듈을 확인하세요."));
		return;
	}
	FStrProperty* Video = CastField<FStrProperty>(Function->FindPropertyByName(TEXT("VideoFilePath")));
	FStrProperty* Audio = CastField<FStrProperty>(Function->FindPropertyByName(TEXT("AudioFilePath")));
	FStrProperty* Slate = CastField<FStrProperty>(Function->FindPropertyByName(TEXT("Slate")));
	FIntProperty* TakeNumber = CastField<FIntProperty>(Function->FindPropertyByName(TEXT("TakeNumber")));
	FStructProperty* Conversion = CastField<FStructProperty>(Function->FindPropertyByName(TEXT("Params")));
	FDelegateProperty* Success = CastField<FDelegateProperty>(Function->FindPropertyByName(TEXT("OnSuccess")));
	FDelegateProperty* Failure = CastField<FDelegateProperty>(Function->FindPropertyByName(TEXT("OnFailure")));
	FIntProperty* Result = CastField<FIntProperty>(Function->FindPropertyByName(TEXT("ReturnValue")));
	if (!Video || !Audio || !Slate || !TakeNumber || !Conversion || !Conversion->Struct || !Success || !Failure || !Result)
	{
		Finish(EHMocapJobState::Failed, LOCTEXT("IngestSignatureMismatch", "영상 가져오기 함수의 매개변수가 엔진과 일치하지 않습니다."));
		return;
	}
	FStructOnScope ParamsScope(Function);
	uint8* Memory = ParamsScope.GetStructMemory();
	Video->SetPropertyValue_InContainer(Memory, CurrentJob->VideoPath);
	Audio->SetPropertyValue_InContainer(Memory, FString());
	Slate->SetPropertyValue_InContainer(Memory, CurrentJob->Slate);
	TakeNumber->SetPropertyValue_InContainer(Memory, Take);
	FScriptDelegate OnSuccess;
	OnSuccess.BindUFunction(this, GET_FUNCTION_NAME_CHECKED(UHMocapJobRunner, OnIngestSuccess));
	FScriptDelegate OnFailure;
	OnFailure.BindUFunction(this, GET_FUNCTION_NAME_CHECKED(UHMocapJobRunner, OnIngestFailure));
	Success->SetPropertyValue_InContainer(Memory, OnSuccess);
	Failure->SetPropertyValue_InContainer(Memory, OnFailure);
	bCallbackReady = false;
	bCallbackAborted = false;
	ActiveIngestId = INDEX_NONE;
	Step = EStep::WaitIngest;
	Library->GetDefaultObject()->ProcessEvent(Function, Memory);
	ActiveIngestId = Result->GetPropertyValue_InContainer(Memory);
	if (ActiveIngestId < 0 && !bCallbackReady)
	{
		Finish(EHMocapJobState::Failed, LOCTEXT("InvalidIngestId", "영상 가져오기 요청을 시작하지 못했습니다."));
	}
}

void UHMocapJobRunner::OnIngestSuccess(int32 IngestId, uint8 IngestType, UFootageCaptureData* FootageCaptureData)
{
	if (!bRunning || Step != EStep::WaitIngest || (ActiveIngestId != INDEX_NONE && ActiveIngestId != IngestId))
	{
		return;
	}
	CaptureData = FootageCaptureData;
	bCallbackSuccess = FootageCaptureData != nullptr;
	CallbackError = LOCTEXT("MissingCaptureData", "가져온 영상 데이터가 없습니다.");
	bCallbackReady = true;
}

void UHMocapJobRunner::OnIngestFailure(int32 IngestId, uint8 IngestType, FText ErrorMessage)
{
	if (!bRunning || Step != EStep::WaitIngest || (ActiveIngestId != INDEX_NONE && ActiveIngestId != IngestId))
	{
		return;
	}
	bCallbackSuccess = false;
	CallbackError = FText::Format(LOCTEXT("IngestFailed", "가져오기 실패: {0}"), ErrorMessage);
	bCallbackReady = true;
}

void UHMocapJobRunner::BeginProcessing()
{
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	FString PackageName;
	FString AssetName;
	AssetTools.CreateUniqueAssetName(Settings.OutputFolder / (TEXT("PERF_") + CurrentJob->Slate), TEXT(""), PackageName, AssetName);
	UClass* FactoryClass = FindObject<UClass>(nullptr, TEXT("/Script/MetaHumanPerformance.MetaHumanPerformanceFactoryNew"));
	if (FactoryClass && FactoryClass->IsChildOf(UFactory::StaticClass()))
	{
		UFactory* Factory = NewObject<UFactory>(GetTransientPackage(), FactoryClass);
		Performance = Cast<UMetaHumanPerformance>(AssetTools.CreateAsset(AssetName,
			FPackageName::GetLongPackagePath(PackageName), UMetaHumanPerformance::StaticClass(), Factory));
	}
	else
	{
		UPackage* Package = CreatePackage(*PackageName);
		Performance = NewObject<UMetaHumanPerformance>(Package, FName(*AssetName), RF_Public | RF_Standalone);
		if (Performance)
		{
			FAssetRegistryModule::AssetCreated(Performance);
		}
	}
	if (!Performance || !CaptureData)
	{
		Finish(EHMocapJobState::Failed, LOCTEXT("PerformanceCreationFailed", "퍼포먼스 에셋을 만들 수 없거나 영상 데이터가 없습니다."));
		return;
	}
	Performance->SetInputType(EDataInputType::MonoFootage);
	Performance->SetFootageCaptureData(CaptureData);
	Performance->bFaceTracking = false;
	Performance->SetBodyTracking(true);
	Performance->bEnableFootLocking = Settings.bFootLocking;
	Performance->bAutoBodyHeight = Settings.bAutoBodyHeight;
	Performance->BodyHeight = Settings.BodyHeightCm;
	Performance->MarkPackageDirty();
	if (!Performance->CanProcess())
	{
		Finish(EHMocapJobState::Failed, FText::Format(LOCTEXT("CannotProcess", "처리할 수 없습니다: {0}"), Performance->GetCannotProcessTooltipText()));
		return;
	}
	CurrentJob->State = EHMocapJobState::Processing;
	CurrentJob->Message = LOCTEXT("Processing", "처리 중");
	CurrentJob->FrameStart = static_cast<int32>(Performance->StartFrameToProcess);
	CurrentJob->FrameEnd = static_cast<int32>(Performance->EndFrameToProcess);
	CurrentJob->FrameCurrent = CurrentJob->FrameStart;
	CurrentJob->StagesDone = 0;
	LastProgressNotification = 0.0;
	Performance->OnFrameProcessed().AddUObject(this, &UHMocapJobRunner::OnFrameProcessed);
	Performance->OnStageProcessingFinished().AddUObject(this, &UHMocapJobRunner::OnStageFinished);
	Performance->OnProcessingFinished().AddUObject(this, &UHMocapJobRunner::OnProcessingFinished);
	Performance->SetBlockingProcessing(false);
	Step = EStep::WaitProcess;
	bCallbackReady = false;
	bCallbackAborted = false;
	Log(LOCTEXT("ProcessingStarted", "바디 애니메이션 처리를 시작합니다."));
	OnJobsChanged.Broadcast();
	const EStartPipelineErrorType Error = Performance->StartPipeline(false);
	if (Error != EStartPipelineErrorType::None)
	{
		if (!bCallbackReady)
		{
			CallbackError = FText::Format(LOCTEXT("PipelineStartFailed", "처리를 시작하지 못했습니다. 오류 코드: {0}"), static_cast<int32>(Error));
		}
		bCallbackSuccess = false;
		bCallbackReady = true;
	}
}

void UHMocapJobRunner::OnFrameProcessed(int32 Frame)
{
	// 진행 콜백은 UI 갱신으로 이어지므로 게임 스레드가 아니면 무시한다.
	if (!ensure(IsInGameThread()) || !CurrentJob || Step != EStep::WaitProcess)
	{
		return;
	}
	CurrentJob->FrameCurrent = Frame;
	const double Now = FPlatformTime::Seconds();
	if (Now - LastProgressNotification >= 0.1)
	{
		LastProgressNotification = Now;
		OnJobsChanged.Broadcast();
	}
}

void UHMocapJobRunner::OnStageFinished(int32 Stage)
{
	if (ensure(IsInGameThread()) && CurrentJob && Step == EStep::WaitProcess)
	{
		CurrentJob->StagesDone = FMath::Min(CurrentJob->StagesDone + 1, 5);
		CurrentJob->FrameCurrent = CurrentJob->FrameStart;
		OnJobsChanged.Broadcast();
	}
}

void UHMocapJobRunner::OnProcessingFinished(TSharedPtr<const UE::MetaHuman::Pipeline::FPipelineData> Data)
{
	// 설정 실패 시 StartPipeline() 안에서 바로 호출될 수 있다. 여기서는 결과만 기록하고 다음 틱에 진행한다.
	if (!ensure(IsInGameThread()) || !bRunning || Step != EStep::WaitProcess)
	{
		return;
	}
	using namespace UE::MetaHuman::Pipeline;
	bCallbackSuccess = Data.IsValid() && Data->GetExitStatus() == EPipelineExitStatus::Ok;
	bCallbackAborted = Data.IsValid() && Data->GetExitStatus() == EPipelineExitStatus::Aborted;
	CallbackError = FText::Format(LOCTEXT("PipelineFailed", "바디 처리 실패: {0}"),
		Data.IsValid() ? FText::FromString(Data->GetErrorMessage()) : LOCTEXT("MissingPipelineResult", "처리 결과가 없습니다."));
	bCallbackReady = true;
}

void UHMocapJobRunner::ExportAnimation()
{
	UnbindPerformance();
	CurrentJob->State = EHMocapJobState::Exporting;
	CurrentJob->Message = LOCTEXT("Exporting", "내보내는 중");
	OnJobsChanged.Broadcast();
	if (!Performance || !UMetaHumanPerformanceExportUtils::CanExportBodyAnimation(Performance))
	{
		Finish(EHMocapJobState::Failed, LOCTEXT("CannotExport", "내보낼 바디 애니메이션 데이터가 없습니다."));
		return;
	}
	UMetaHumanPerformanceExportAnimationSettings* ExportSettings = NewObject<UMetaHumanPerformanceExportAnimationSettings>();
	ExportSettings->bShowExportDialog = false;
	ExportSettings->bAutoSaveAnimSequence = true;
	ExportSettings->ExportRange = EPerformanceExportRange::WholeSequence;
	ExportSettings->bExportFace = false;
	ExportSettings->bExportBody = true;
	ExportSettings->ExportSkeleton = EPerformanceExportSkeleton::PerformerSkeleton;
	FString PackageName;
	FString AssetName;
	FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get().CreateUniqueAssetName(
		Settings.OutputFolder / FString::Printf(TEXT("AS_%s_Body"), *CurrentJob->Slate), TEXT(""), PackageName, AssetName);
	ExportSettings->PackagePath = FPackageName::GetLongPackagePath(PackageName);
	ExportSettings->AssetName = AssetName;
	UAnimSequence* Animation = UMetaHumanPerformanceExportUtils::ExportAnimationSequence(Performance, ExportSettings);
	if (!Animation)
	{
		Finish(EHMocapJobState::Failed, LOCTEXT("ExportFailed", "바디 애니메이션 내보내기에 실패했습니다. 엔진 출력 로그를 확인하세요."));
		return;
	}
	Performance->MarkPackageDirty();
	if (!UEditorLoadingAndSavingUtils::SavePackages(TArray<UPackage*>{ Performance->GetPackage(), Animation->GetPackage() }, false))
	{
		Finish(EHMocapJobState::Failed, LOCTEXT("SaveFailed", "생성한 에셋을 저장하지 못했습니다. 콘텐츠 브라우저에서 저장 상태를 확인하세요."));
		return;
	}
	CurrentJob->AnimSequencePath = FSoftObjectPath(Animation);
	// 나중에 이 애니메이션만 골라도 프리뷰가 원본 영상을 찾을 수 있게 한다.
	HMocapSourceVideo::Remember(Animation->GetPathName(), CurrentJob->VideoPath);

	// 우리 캐릭터로 변환(리타깃 + 루트 모션). 실패해도 모캡 결과는 이미 있으므로 작업은 완료로 둔다.
	if (Settings.Retarget.bEnabled)
	{
		Log(LOCTEXT("RetargetStarted", "우리 캐릭터로 변환하는 중..."));
		FText RetargetError;
		if (UAnimSequence* Converted = HMocapRetarget::Convert(Animation, Settings.Retarget, RetargetError))
		{
			CurrentJob->RetargetedAnimPath = FSoftObjectPath(Converted);
			Log(FText::Format(LOCTEXT("RetargetDone", "변환 완료: {0}"), FText::FromString(Converted->GetPathName())));
		}
		if (!RetargetError.IsEmpty())
		{
			CurrentJob->Warning = RetargetError;
			Log(FText::Format(LOCTEXT("RetargetProblem", "변환 문제: {0}"), RetargetError));
		}
	}
	Finish(EHMocapJobState::Done, LOCTEXT("Done", "완료"));
}

void UHMocapJobRunner::UnbindPerformance()
{
	if (Performance)
	{
		Performance->OnFrameProcessed().RemoveAll(this);
		Performance->OnStageProcessingFinished().RemoveAll(this);
		Performance->OnProcessingFinished().RemoveAll(this);
	}
}

void UHMocapJobRunner::Finish(EHMocapJobState State, const FText& Message)
{
	UnbindPerformance();
	if (CurrentJob)
	{
		CurrentJob->State = State;
		CurrentJob->Message = Message;
		Log(FText::Format(LOCTEXT("JobResult", "{0}: {1}"),
			FText::FromString(FPaths::GetCleanFilename(CurrentJob->VideoPath)), Message));
	}
	CurrentJob.Reset();
	Performance = nullptr;
	CaptureData = nullptr;
	ActiveIngestId = INDEX_NONE;
	bCallbackReady = false;
	bCallbackAborted = false;
	Step = EStep::Next;
	if (State == EHMocapJobState::Cancelled)
	{
		bRunning = false;
	}
	OnJobsChanged.Broadcast();
}

bool UHMocapJobRunner::RequestCancelIngest()
{
	UClass* Library = FindIngestLibrary();
	UFunction* Function = Library ? Library->FindFunctionByName(TEXT("CancelIngest")) : nullptr;
	FIntProperty* Id = Function ? CastField<FIntProperty>(Function->FindPropertyByName(TEXT("IngestId"))) : nullptr;
	FBoolProperty* Result = Function ? CastField<FBoolProperty>(Function->FindPropertyByName(TEXT("ReturnValue"))) : nullptr;
	if (!Id || !Result || ActiveIngestId == INDEX_NONE)
	{
		Log(LOCTEXT("CancelIngestUnavailable", "가져오기 취소 함수를 사용할 수 없습니다."));
		return false;
	}
	FStructOnScope ParamsScope(Function);
	Id->SetPropertyValue_InContainer(ParamsScope.GetStructMemory(), ActiveIngestId);
	Library->GetDefaultObject()->ProcessEvent(Function, ParamsScope.GetStructMemory());
	return Result->GetPropertyValue_InContainer(ParamsScope.GetStructMemory());
}

void UHMocapJobRunner::Cancel()
{
	if (!bRunning || bCancelRequested)
	{
		return;
	}
	bCancelRequested = true;
	Log(LOCTEXT("CancelRequested", "취소를 요청했습니다. 현재 작업이 종료될 때까지 기다려 주세요."));
	if (Step == EStep::WaitIngest && !bCallbackReady)
	{
		if (!RequestCancelIngest())
		{
			Log(LOCTEXT("CancelIngestPending", "가져오기 취소를 확인하지 못했습니다. 완료 콜백을 기다린 뒤 대기열을 중지합니다."));
		}
	}
	else if (Step == EStep::WaitProcess && Performance && !bCallbackReady)
	{
		Performance->CancelPipeline();
	}
	OnJobsChanged.Broadcast();
}

void UHMocapJobRunner::Shutdown()
{
	Cancel();
	FTSTicker::RemoveTicker(TickerHandle);
	TickerHandle.Reset();
	UnbindPerformance();
	if (CurrentJob)
	{
		Finish(EHMocapJobState::Cancelled, LOCTEXT("ShutdownCancelled", "도구 종료로 취소됨"));
	}
	bRunning = false;
}

void UHMocapJobRunner::BeginDestroy()
{
	// 여기서는 Performance 등 다른 UObject가 이미 파괴됐을 수 있다. 정리는 모듈이 Shutdown()으로 미리 한다.
	FTSTicker::RemoveTicker(TickerHandle);
	TickerHandle.Reset();
	Super::BeginDestroy();
}

#undef LOCTEXT_NAMESPACE
