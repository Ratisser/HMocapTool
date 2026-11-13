// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "UObject/Object.h"
#include "UObject/SoftObjectPath.h"
#include "HMocapRetarget.h"
#include "HMocapJobRunner.generated.h"

class UFootageCaptureData;
class UMetaHumanPerformance;
namespace UE::MetaHuman::Pipeline { class FPipelineData; }

UENUM()
enum class EHMocapJobState : uint8
{
	Pending, Ingesting, Processing, Exporting, Done, Failed, Cancelled
};

struct FHMocapJob
{
	FString VideoPath;
	FString Slate;
	EHMocapJobState State = EHMocapJobState::Pending;
	FText Message;
	int32 StagesDone = 0;
	int32 FrameStart = 0;
	int32 FrameEnd = 0;
	int32 FrameCurrent = 0;
	FSoftObjectPath AnimSequencePath;
	/** 우리 캐릭터로 변환한 애니메이션. 변환을 켜지 않았거나 실패했으면 비어 있다. */
	FSoftObjectPath RetargetedAnimPath;
	/** 완료는 했지만 알려야 할 문제(변환 실패 등). */
	FText Warning;
};

struct FHMocapSettings
{
	FString OutputFolder = TEXT("/Game/CaptureManager/Mocap");
	bool bFootLocking = true;
	bool bAutoBodyHeight = true;
	float BodyHeightCm = 175.f;
	FHMocapRetargetSettings Retarget;
};

DECLARE_MULTICAST_DELEGATE_OneParam(FOnHMocapLog, const FText&);

UCLASS()
class UHMocapJobRunner : public UObject
{
	GENERATED_BODY()

public:
	const TArray<TSharedPtr<FHMocapJob>>& GetJobs() const { return Jobs; }
	void AddVideos(const TArray<FString>& Paths);
	void RemoveJob(TSharedPtr<FHMocapJob> Job);
	void ClearFinished();
	bool Start(const FHMocapSettings& InSettings);
	void Cancel();
	bool IsRunning() const { return bRunning; }
	static bool IsBodyTrackerAvailable();
	void Shutdown();
	virtual void BeginDestroy() override;

	FSimpleMulticastDelegate OnJobsChanged;
	FOnHMocapLog OnLog;

private:
	enum class EStep : uint8 { Next, WaitIngest, Process, WaitProcess, Export };
	bool TickQueue(float DeltaTime);
	void BeginIngest();
	void BeginProcessing();
	void ExportAnimation();
	void Finish(EHMocapJobState State, const FText& Message);
	void UnbindPerformance();
	void Log(const FText& Message);
	bool RequestCancelIngest();
	int32 FindTakeNumber();

	UFUNCTION()
	void OnIngestSuccess(int32 IngestId, uint8 IngestType, UFootageCaptureData* FootageCaptureData);
	UFUNCTION()
	void OnIngestFailure(int32 IngestId, uint8 IngestType, FText ErrorMessage);
	void OnFrameProcessed(int32 Frame);
	void OnStageFinished(int32 Stage);
	void OnProcessingFinished(TSharedPtr<const UE::MetaHuman::Pipeline::FPipelineData> Data);

	UPROPERTY(Transient)
	TObjectPtr<UMetaHumanPerformance> Performance;
	UPROPERTY(Transient)
	TObjectPtr<UFootageCaptureData> CaptureData;

	TArray<TSharedPtr<FHMocapJob>> Jobs;
	TSharedPtr<FHMocapJob> CurrentJob;
	FHMocapSettings Settings;
	FTSTicker::FDelegateHandle TickerHandle;
	EStep Step = EStep::Next;
	bool bRunning = false;
	bool bCancelRequested = false;
	bool bCallbackReady = false;
	bool bCallbackSuccess = false;
	bool bCallbackAborted = false;
	FText CallbackError;
	int32 ActiveIngestId = INDEX_NONE;
	double LastProgressNotification = 0.0;
};
