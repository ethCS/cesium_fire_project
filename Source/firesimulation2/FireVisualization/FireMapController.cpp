#include "FireMapController.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/GameInstance.h"
#include "EngineUtils.h"
#include "FireDataSubsystem.h"
#include "FireGameMode.h"
#include "FireHUD.h"
#include "FireHUDWidget.h"
#include "FireNationalRendererComponent.h"
#include "Cesium3DTileset.h"
#include "CesiumSampleHeightResult.h"
#include "Kismet/GameplayStatics.h"
#include "FireLocalRendererComponent.h"
#include "FireNiagaraRendererComponent.h"
#include "FireRegionalRendererComponent.h"
#include "FireVDBRendererComponent.h"
#include "WildfireVisualizationSubsystem.h"
#include "firesimulation2.h"
#include "GameFramework/DefaultPawn.h"
#include "GameFramework/FloatingPawnMovement.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Camera/CameraComponent.h"
#include "HAL/IConsoleManager.h"
#include "InputCoreTypes.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/PlatformTLS.h"

namespace FireMapControllerConsole
{
	AFireMapController* FindController(UWorld* World)
	{
		if (!World)
		{
			return nullptr;
		}

		for (TActorIterator<AFireMapController> It(World); It; ++It)
		{
			return *It;
		}

		return nullptr;
	}

	void SetYearCommand(const TArray<FString>& Args, UWorld* World)
	{
		AFireMapController* Controller = FindController(World);
		if (!Controller)
		{
			UE_LOG(LogFireSimulation2, Error, TEXT("Fire.SetYear failed: no FireMapController found in world."));
			return;
		}

		if (Args.Num() < 1)
		{
			UE_LOG(LogFireSimulation2, Error, TEXT("Usage: Fire.SetYear <year>"));
			return;
		}

		const int32 ParsedYear = FCString::Atoi(*Args[0]);
		if (ParsedYear <= 0)
		{
			UE_LOG(LogFireSimulation2, Error, TEXT("Fire.SetYear failed: invalid year '%s'."), *Args[0]);
			return;
		}

		Controller->SetYearAndRefresh(ParsedYear);
	}

	void SetFilterCommand(const TArray<FString>& Args, UWorld* World)
	{
		AFireMapController* Controller = FindController(World);
		if (!Controller)
		{
			UE_LOG(LogFireSimulation2, Error, TEXT("Fire.SetFilter failed: no FireMapController found in world."));
			return;
		}

		if (Args.Num() < 2)
		{
			UE_LOG(LogFireSimulation2, Error, TEXT("Usage: Fire.SetFilter <year> <stateCode> [startDay] [endDay]"));
			return;
		}

		const int32 ParsedYear = FCString::Atoi(*Args[0]);
		const FString ParsedStateCode = Args[1].TrimStartAndEnd().ToUpper();
		const int32 ParsedStartDay = Args.Num() > 2 ? FCString::Atoi(*Args[2]) : 1;
		const int32 ParsedEndDay = Args.Num() > 3 ? FCString::Atoi(*Args[3]) : 365;

		const bool bApplied = Controller->SetFiltersAndRefresh(ParsedYear, ParsedStateCode, ParsedStartDay, ParsedEndDay);
		if (!bApplied)
		{
			UE_LOG(LogFireSimulation2, Error, TEXT("Fire.SetFilter failed: invalid filter values."));
		}
	}

	void FocusCommand(const TArray<FString>& Args, UWorld* World)
	{
		AFireMapController* Controller = FindController(World);
		if (!Controller)
		{
			UE_LOG(LogFireSimulation2, Error, TEXT("Fire.Focus failed: no FireMapController found in world."));
			return;
		}

		Controller->FocusAndSpawnCurrentFire();
	}

	void DebugPerimetersCommand(const TArray<FString>& Args, UWorld* World)
	{
		AFireMapController* Controller = FindController(World);
		if (!Controller)
		{
			UE_LOG(LogFireSimulation2, Error, TEXT("Fire.DebugPerimeters failed: no FireMapController found in world."));
			return;
		}

		const float DurationSeconds = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 20.0f;
		const float SphereRadiusCm = Args.Num() > 1 ? FCString::Atof(*Args[1]) : 3000.0f;
		Controller->DebugDrawPerimeters(DurationSeconds, SphereRadiusCm);
	}

	void TeleportFirstPillarCommand(const TArray<FString>& Args, UWorld* World)
	{
		AFireMapController* Controller = FindController(World);
		if (!Controller)
		{
			UE_LOG(LogFireSimulation2, Error, TEXT("Fire.TeleportFirst failed: no FireMapController found in world."));
			return;
		}

		Controller->DebugTeleportToFirstPillar();
	}
}

static FAutoConsoleCommandWithWorldAndArgs GFireSetYearCommand(
	TEXT("Fire.SetYear"),
	TEXT("Set active wildfire visualization year. Usage: Fire.SetYear <year>"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&FireMapControllerConsole::SetYearCommand));

static FAutoConsoleCommandWithWorldAndArgs GFireSetFilterCommand(
	TEXT("Fire.SetFilter"),
	TEXT("Set wildfire filters. Usage: Fire.SetFilter <year> <stateCode> [startDay] [endDay]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&FireMapControllerConsole::SetFilterCommand));

static FAutoConsoleCommandWithWorldAndArgs GFireFocusCommand(
	TEXT("Fire.Focus"),
	TEXT("Focus camera on current fire point."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&FireMapControllerConsole::FocusCommand));

static FAutoConsoleCommandWithWorldAndArgs GFireDebugPerimetersCommand(
	TEXT("Fire.DebugPerimeters"),
	TEXT("Draw wildfire perimeter debug geometry. Usage: Fire.DebugPerimeters [durationSeconds] [sphereRadiusCm]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&FireMapControllerConsole::DebugPerimetersCommand));

static FAutoConsoleCommandWithWorldAndArgs GFireTeleportFirstCommand(
	TEXT("Fire.TeleportFirst"),
	TEXT("Teleport to first pillar and run cancelable auto-descent into fire view."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&FireMapControllerConsole::TeleportFirstPillarCommand));

AFireMapController::AFireMapController()
{
	PrimaryActorTick.bCanEverTick = true;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);
	NationalRenderer = CreateDefaultSubobject<UFireNationalRendererComponent>(TEXT("NationalRenderer"));
	RegionalRenderer = CreateDefaultSubobject<UFireRegionalRendererComponent>(TEXT("RegionalRenderer"));
	LocalRenderer    = CreateDefaultSubobject<UFireLocalRendererComponent>(TEXT("LocalRenderer"));
	NiagaraRenderer  = CreateDefaultSubobject<UFireNiagaraRendererComponent>(TEXT("NiagaraRenderer"));
	VDBRenderer      = CreateDefaultSubobject<UFireVDBRendererComponent>(TEXT("VDBRenderer"));
}

void AFireMapController::BeginPlay()
{
	Super::BeginPlay();

	// Migrate stale per-state defaults from old map instances to nationwide view.
	if (StateCode.Equals(TEXT("CA"), ESearchCase::IgnoreCase) ||
		StateCode.Equals(TEXT("AZ"), ESearchCase::IgnoreCase))
	{
		StateCode = TEXT("ALL");
	}

	NationalRenderer->InitializeRenderer(Root);
	NationalRenderer->Configure(PerimeterZOffset, PointMarkerHeightOffset, PointMarkerPillarHeightCm, PointMarkerDiameterCm, bShowPointInstances);
	RegionalRenderer->InitializeRenderer(Root);
	RegionalRenderer->Configure(RegionalPerimeterOffsetCm, RegionalHeatmapOffsetCm, RegionalMaxRenderedEvents, RegionalMinAcresForPerimeter, RegionalPerimeterExaggerationScale);
	LocalRenderer->InitializeRenderer(Root);
	LocalRenderer->Configure(LocalMaxActiveFires, LocalSmokeColumnHeightCm, LocalSmokeColumnDiameterCm, LocalSmokeHeightOffsetCm);
	NiagaraRenderer->InitializeRenderer(Root);
	NiagaraRenderer->Configure(NiagaraMaxInstancesPerEvent, NiagaraMaxTotalInstances, NiagaraMinAcresForFire, NiagaraSampleSpacingKm, NiagaraScaleBase, NiagaraActivationDistanceKm);
	VDBRenderer->InitializeRenderer(Root);
	VDBRenderer->Configure(VDBMaxInstancesPerEvent, VDBMaxTotalInstances, VDBMinAcresForFire, VDBSampleSpacingKm, VDBScaleXY, VDBScaleZ, VDBActivationDistanceKm);

	if (UWildfireVisualizationSubsystem* VisualizationSubsystem = GetWorld() ? GetWorld()->GetSubsystem<UWildfireVisualizationSubsystem>() : nullptr)
	{
		VisualizationSubsystem->SetNationalRenderer(NationalRenderer);
		VisualizationSubsystem->SetRegionalRenderer(bEnableRegionalPerimeters ? RegionalRenderer.Get() : nullptr);
		VisualizationSubsystem->SetLocalRenderer(bEnableLocalView ? LocalRenderer.Get() : nullptr);
		VisualizationSubsystem->SetNiagaraRenderer(bEnableNiagaraFire ? NiagaraRenderer.Get() : nullptr);
		VisualizationSubsystem->SetVDBRenderer(bEnableVDBFire ? VDBRenderer.Get() : nullptr);
		VisualizationSubsystem->SetFilters(Year, StateCode, StartDayOfYear, EndDayOfYear);
	}

	SetupRuntimeCameraAndMovement();
	// Force heterogeneous volume rendering path on for VDB fire.
	if (IConsoleVariable* CVarHV = IConsoleManager::Get().FindConsoleVariable(TEXT("r.HeterogeneousVolumes")))
	{
		CVarHV->Set(1, ECVF_SetByCode);
	}
	if (IConsoleVariable* CVarHVEnable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.HeterogeneousVolumes.Enable")))
	{
		CVarHVEnable->Set(1, ECVF_SetByCode);
	}
	if (IConsoleVariable* CVarHVTrans = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Translucency.HeterogeneousVolumes")))
	{
		CVarHVTrans->Set(1, ECVF_SetByCode);
	}

	if (APlayerController* PlayerController = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
	{
		OverviewViewTarget = PlayerController->GetPawn() ? static_cast<AActor*>(PlayerController->GetPawn()) : PlayerController->GetViewTarget();
	}

	SetupRuntimeYearInput();
	AcquireHUDWidget();
	NotifyYearChanged();

	// Lock user interaction during initial load to avoid confusing empty/partial state.
	if (APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
	{
		PC->SetIgnoreMoveInput(true);
		PC->SetIgnoreLookInput(true);
		if (AFireHUD* FireHUD = Cast<AFireHUD>(PC->GetHUD()))
		{
			FireHUD->SetLoadingOverlay(true, 0.0f, TEXT("Loading wildfire data..."));
		}
	}
	// IMPORTANT: do not run the heavy visualization build inside BeginPlay or the first
	// HUD frame cannot render until after all startup mesh/data work completes.
	TWeakObjectPtr<AFireMapController> WeakThisInit(this);
	GetWorldTimerManager().SetTimerForNextTick(FTimerDelegate::CreateLambda([WeakThisInit]()
	{
		AFireMapController* Self = WeakThisInit.Get();
		if (!Self)
		{
			return;
		}
		Self->RefreshVisualization();
		// Avoid a second expensive startup query; use first render result as target count.
		Self->StartupExpectedFireCount = Self->NationalRenderer ? Self->NationalRenderer->GetEventCount() : Self->RenderedEvents.Num();
		Self->bStartupDeferredRefreshDone = true;
	}));
}

void AFireMapController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		FTimerManager& TM = World->GetTimerManager();
		TM.ClearTimer(StartupRefreshHandle);
		TM.ClearTimer(YearDebounceHandle);
		TM.ClearTimer(LocalFireSnapHandle);
		TM.ClearTimer(DescentFireSnapHandle);
	}
	Super::EndPlay(EndPlayReason);
}

void AFireMapController::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	UpdateStartupLoadingState(DeltaSeconds);
	if (bStartupLoading)
	{
		return;
	}

	UpdateHoverTooltip();

	if (bSearchBarActive) { TickSearchInput(); }

	if (bIsAutoDescending)
	{
		TickAutoDescend(DeltaSeconds);
	}

	// Allow instant cancel back to overview even if input bindings are bypassed.
	if (bInFireView)
	{
		if (APlayerController* PlayerController = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
		{
			if (PlayerController->IsInputKeyDown(EKeys::Escape))
			{
				ReturnToOverviewView();
			}
		}
	}
}

void AFireMapController::UpdateStartupLoadingState(const float DeltaSeconds)
{
	if (!bStartupLoading)
	{
		return;
	}

	StartupLoadingElapsed += FMath::Max(0.0f, DeltaSeconds);

	int32 CurrentCount = 0;
	if (NationalRenderer)
	{
		CurrentCount = NationalRenderer->GetEventCount();
	}

	const int32 TargetCount = FMath::Max(StartupExpectedFireCount, 1);
	const float CountProgress = FMath::Clamp(static_cast<float>(CurrentCount) / static_cast<float>(TargetCount), 0.0f, 1.0f);

	const bool bTimedOut = StartupLoadingElapsed >= StartupLoadingMaxSeconds;

	// Multi-stage smooth progress — driven by elapsed time, NOT timer query (which
	// returns -1 once fired, causing a false snap to 85% and a stuck bar).
	// Stage 1 (0–85%): national fire render count.
	// Stage 2 (85–99%): wall-clock progress toward the deferred regional refresh.
	// Stage 3 (99→100%): snap immediately when deferred refresh finishes.
	float TargetProgress = CountProgress * 0.85f;
	if (CountProgress >= 1.0f && !bStartupDeferredRefreshDone && !bTimedOut)
	{
		const float DeferredPhase = FMath::Clamp(
			StartupLoadingElapsed / FMath::Max(StartupDeferredRefreshDelaySeconds, 1.0f),
			0.0f, 1.0f);
		TargetProgress = 0.85f + DeferredPhase * 0.14f; // 85% → 99%
	}
	if (bStartupDeferredRefreshDone || bTimedOut)
	{
		// Snap display to 100% immediately — no slow crawl from 99%.
		StartupDisplayedProgress01 = 1.0f;
		TargetProgress = 1.0f;
	}
	else
	{
		const float MaxStep = FMath::Max(0.01f, StartupProgressRatePerSecond) * FMath::Max(0.0f, DeltaSeconds);
		StartupDisplayedProgress01 = FMath::Min(TargetProgress, StartupDisplayedProgress01 + MaxStep);
	}

	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (AFireHUD* FireHUD = PC ? Cast<AFireHUD>(PC->GetHUD()) : nullptr)
	{
		const FString Label = bStartupDeferredRefreshDone
			? FString::Printf(TEXT("Loaded %d fires — ready! (%d%%)"), CurrentCount, FMath::FloorToInt(StartupDisplayedProgress01 * 100.0f))
			: FString::Printf(TEXT("Loaded %d / %d fires (%d%%)"), CurrentCount, TargetCount, FMath::FloorToInt(StartupDisplayedProgress01 * 100.0f));
		FireHUD->SetLoadingOverlay(true, StartupDisplayedProgress01, Label);
	}

	if ((bStartupDeferredRefreshDone || bTimedOut) && StartupDisplayedProgress01 >= 1.0f)
	{
		bStartupLoading = false;
		if (PC)
		{
			PC->SetIgnoreMoveInput(false);
			PC->SetIgnoreLookInput(false);
			if (AFireHUD* FireHUD = Cast<AFireHUD>(PC->GetHUD()))
			{
				FireHUD->SetLoadingOverlay(false, 1.0f, TEXT("Ready"));
			}
		}

		UE_LOG(LogFireSimulation2, Display,
			TEXT("FireMapController: startup loading complete. current=%d expected=%d deferredRefreshDone=%d timedOut=%d"),
			CurrentCount, TargetCount, bStartupDeferredRefreshDone ? 1 : 0, bTimedOut ? 1 : 0);
	}
}

void AFireMapController::TickAutoDescend(float DeltaSeconds)
{
	APlayerController* PlayerController = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PlayerController)
	{
		bIsAutoDescending = false;
		return;
	}

	APawn* Pawn = PlayerController->GetPawn();
	if (!Pawn)
	{
		bIsAutoDescending = false;
		return;
	}

	// Cancel if the user provides any movement input.
	if (Pawn->GetPendingMovementInputVector().SizeSquared() > KINDA_SMALL_NUMBER)
	{
		bIsAutoDescending = false;

		// Hand control to the player at the current location instead of snapping back to overview.
		bInFireView = true;
		SetViewModeLit();
		if (UWildfireVisualizationSubsystem* Sub = GetWorld()
			? GetWorld()->GetSubsystem<UWildfireVisualizationSubsystem>()
			: nullptr)
		{
			Sub->RefreshLocalFireEffectsOnly();
		}
		if (APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
		{
			if (AFireHUD* FireHUD = Cast<AFireHUD>(PC->GetHUD()))
			{
				FireHUD->ShowCenterPrompt(TEXT("Fire animations active. Heatmap remains terrain overlay. WASD to move, Esc to return."), 2.5f);
			}
		}
		UE_LOG(LogFireSimulation2, Display, TEXT("AutoDescend: interrupted by movement input. Staying in fire view with manual WASD control."));
		return;
	}

	// Advance at a fixed rate — total descent duration is ~5 seconds (1/0.2 = 5).
	constexpr float DescentSpeed = 0.2f;
	AutoDescendAlpha = FMath::Clamp(AutoDescendAlpha + DeltaSeconds * DescentSpeed, 0.0f, 1.0f);

	// Cubic ease-in-out: slow start, fast middle, slow arrival.
	const float Smooth = FMath::InterpEaseInOut(0.0f, 1.0f, AutoDescendAlpha, 3.0f);

	const FVector NewLocation = FMath::Lerp(AutoDescendStartLocation, AutoDescendTargetLocation, Smooth);

	// Slerp rotation — ensure short-arc path to prevent mid-descent camera flip.
	FQuat StartQ = AutoDescendStartRotation.Quaternion();
	FQuat EndQ   = AutoDescendTargetRotation.Quaternion();
	if ((StartQ | EndQ) < 0.0f) { EndQ = -EndQ; }  // force same hemisphere
	const FRotator NewRotation = FQuat::Slerp(StartQ, EndQ, Smooth).Rotator();

	Pawn->SetActorLocation(NewLocation, false, nullptr, ETeleportType::TeleportPhysics);
	PlayerController->SetControlRotation(NewRotation);

	if (AutoDescendAlpha >= 1.0f)
	{
		bIsAutoDescending = false;
		bInFireView = true;
		SetViewModeLit();

		// Hide the regional heatmap — it's a flat mesh that floods the camera at ground level.
		// Camera is now at fire level. Place VDB/Niagara fire effects.
		if (UWildfireVisualizationSubsystem* Sub = GetWorld()
			? GetWorld()->GetSubsystem<UWildfireVisualizationSubsystem>()
			: nullptr)
		{
			Sub->RefreshLocalFireEffectsOnly();
		}
		if (APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
		{
			if (AFireHUD* FireHUD = Cast<AFireHUD>(PC->GetHUD()))
			{
				FireHUD->ShowCenterPrompt(TEXT("Fire animations active. Heatmap remains terrain overlay. WASD to move, Esc to return."), 2.5f);
			}
		}

		// 3-second snap: re-place fire effects once Cesium tiles finish streaming at ground level.
		TWeakObjectPtr<AFireMapController> WeakThis(this);
		GetWorldTimerManager().ClearTimer(DescentFireSnapHandle);
		GetWorldTimerManager().SetTimer(DescentFireSnapHandle, [WeakThis]()
		{
			AFireMapController* Self = WeakThis.Get();
			if (!Self)
			{
				return;
			}
			if (UWildfireVisualizationSubsystem* Sub = Self->GetWorld()
				? Self->GetWorld()->GetSubsystem<UWildfireVisualizationSubsystem>()
				: nullptr)
			{
				Sub->RefreshLocalFireEffectsOnly();
			}
		}, 3.0f, false);

		UE_LOG(LogFireSimulation2, Display, TEXT("AutoDescend: arrived at fire level, fire effects placed, terrain heatmap remains active."));
	}
}

void AFireMapController::RefreshVisualization()
{
	const double RefreshStart = FPlatformTime::Seconds();
	ClearVisualization();
	NationalRenderer->Configure(PerimeterZOffset, PointMarkerHeightOffset, PointMarkerPillarHeightCm, PointMarkerDiameterCm, bShowPointInstances);
	RegionalRenderer->Configure(RegionalPerimeterOffsetCm, RegionalHeatmapOffsetCm, RegionalMaxRenderedEvents, RegionalMinAcresForPerimeter, RegionalPerimeterExaggerationScale);
	LocalRenderer->Configure(LocalMaxActiveFires, LocalSmokeColumnHeightCm, LocalSmokeColumnDiameterCm, LocalSmokeHeightOffsetCm);
	NiagaraRenderer->Configure(NiagaraMaxInstancesPerEvent, NiagaraMaxTotalInstances, NiagaraMinAcresForFire, NiagaraSampleSpacingKm, NiagaraScaleBase, NiagaraActivationDistanceKm);
	VDBRenderer->Configure(VDBMaxInstancesPerEvent, VDBMaxTotalInstances, VDBMinAcresForFire, VDBSampleSpacingKm, VDBScaleXY, VDBScaleZ, VDBActivationDistanceKm);

	if (UWildfireVisualizationSubsystem* VisualizationSubsystem = GetWorld() ? GetWorld()->GetSubsystem<UWildfireVisualizationSubsystem>() : nullptr)
	{
		VisualizationSubsystem->SetRegionalRenderer(bEnableRegionalPerimeters ? RegionalRenderer.Get() : nullptr);
		VisualizationSubsystem->SetLocalRenderer(bEnableLocalView ? LocalRenderer.Get() : nullptr);
		VisualizationSubsystem->SetNiagaraRenderer(bEnableNiagaraFire ? NiagaraRenderer.Get() : nullptr);
		VisualizationSubsystem->SetVDBRenderer(bEnableVDBFire ? VDBRenderer.Get() : nullptr);
		if (VisualizationSubsystem->SetFilters(Year, StateCode, StartDayOfYear, EndDayOfYear))
		{
			VisualizationSubsystem->RefreshView();
		}
	}

	SyncRenderedCachesFromRenderer();
	UE_LOG(
		LogFireSimulation2,
		Display,
		TEXT("FireMapController: RefreshVisualization complete year=%d state=%s dayRange=%d-%d rendered=%d tookMs=%.2f"),
		Year,
		*StateCode,
		StartDayOfYear,
		EndDayOfYear,
		RenderedEvents.Num(),
		(FPlatformTime::Seconds() - RefreshStart) * 1000.0);
}

bool AFireMapController::SetYearAndRefresh(int32 NewYear)
{
	if (NewYear <= 0 || MinAvailableYear > MaxAvailableYear)
	{
		UE_LOG(LogFireSimulation2, Error, TEXT("SetYearAndRefresh failed: invalid year %d"), NewYear);
		return false;
	}

	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!bStartupLoading)
	{
		if (AFireHUD* FireHUD = PC ? Cast<AFireHUD>(PC->GetHUD()) : nullptr)
		{
			FireHUD->SetLoadingOverlay(true, 0.10f, FString::Printf(TEXT("Switching to %d wildfire data..."), NewYear));
		}
	}

	Year = FMath::Clamp(NewYear, MinAvailableYear, MaxAvailableYear);
	RefreshVisualization();

	if (!bStartupLoading)
	{
		if (AFireHUD* FireHUD = PC ? Cast<AFireHUD>(PC->GetHUD()) : nullptr)
		{
			FireHUD->SetLoadingOverlay(false, 1.0f, TEXT("Ready"));
		}
	}
	return true;
}

bool AFireMapController::SetFiltersAndRefresh(int32 NewYear, const FString& NewStateCode, int32 NewStartDayOfYear, int32 NewEndDayOfYear)
{
	if (NewYear <= 0 || NewStartDayOfYear < 1 || NewStartDayOfYear > 366 || NewEndDayOfYear < 1 || NewEndDayOfYear > 366 || NewStartDayOfYear > NewEndDayOfYear)
	{
		return false;
	}

	const FString TrimmedStateCode = NewStateCode.TrimStartAndEnd().ToUpper();
	if (TrimmedStateCode.IsEmpty())
	{
		return false;
	}

	Year = NewYear;
	StateCode = TrimmedStateCode;
	StartDayOfYear = NewStartDayOfYear;
	EndDayOfYear = NewEndDayOfYear;
	RefreshVisualization();
	return true;
}

void AFireMapController::FocusAndSpawnCurrentFire()
{
	FocusFireByOffset(0);
}

void AFireMapController::DebugTeleportToFirstPillar()
{
	if (RenderedEventCenters.Num() == 0)
	{
		UE_LOG(LogFireSimulation2, Warning, TEXT("DebugTeleportToFirstPillar: no rendered events."));
		return;
	}

	FocusedEventIndex = 0;
	MovePlayerToPillarTop(FocusedEventIndex);
}

void AFireMapController::DebugDrawPerimeters(const float DurationSeconds, const float SphereRadiusCm)
{
	if (UWildfireVisualizationSubsystem* VisualizationSubsystem = GetWorld() ? GetWorld()->GetSubsystem<UWildfireVisualizationSubsystem>() : nullptr)
	{
		VisualizationSubsystem->LogRegionalDebugStats();
		VisualizationSubsystem->DebugDrawPerimeters(DurationSeconds, SphereRadiusCm);
	}
}

void AFireMapController::DebugRunCesiumSampleHeightProbe(const double ProbeLongitude, const double ProbeLatitude)
{
	const bool bUseDefaultBase = FMath::IsNearlyZero(ProbeLongitude, KINDA_SMALL_NUMBER) &&
		FMath::IsNearlyZero(ProbeLatitude, KINDA_SMALL_NUMBER);
	const double EffectiveProbeLongitude = bUseDefaultBase ? -105.0 : ProbeLongitude;
	const double EffectiveProbeLatitude = bUseDefaultBase ? 40.0 : ProbeLatitude;

	UWorld* World = GetWorld();
	if (!World)
	{
		UE_LOG(LogFireSimulation2, Warning, TEXT("CesiumProbe: no world."));
		return;
	}

	ACesium3DTileset* Tileset = nullptr;
	for (TActorIterator<ACesium3DTileset> It(World); It; ++It)
	{
		Tileset = *It;
		break;
	}
	if (!Tileset)
	{
		UE_LOG(LogFireSimulation2, Warning, TEXT("CesiumProbe: no ACesium3DTileset found."));
		return;
	}

	CesiumProbeTileset = Tileset;
	CesiumProbeBaseLonLat = FVector2D(static_cast<float>(EffectiveProbeLongitude), static_cast<float>(EffectiveProbeLatitude));
	CesiumProbeCounts = {1, 10, 100, 1000, 5000, 10000, 20000, 40000};
	CesiumProbeRequestIndex = 0;
	bCesiumProbeActive = true;

	TArray<FString> CountStrings;
	CountStrings.Reserve(CesiumProbeCounts.Num());
	for (const int32 Count : CesiumProbeCounts)
	{
		CountStrings.Add(FString::FromInt(Count));
	}

	UE_LOG(
		LogFireSimulation2,
		Display,
		TEXT("CesiumProbe: starting tileset=%s baseLonLat=(%.6f, %.6f) defaultedFromZero=%d counts=[%s]"),
		*GetNameSafe(Tileset),
		EffectiveProbeLongitude,
		EffectiveProbeLatitude,
		bUseDefaultBase ? 1 : 0,
		*FString::Join(CountStrings, TEXT(",")));

	DispatchCesiumSampleProbeRequest();
}

void AFireMapController::DispatchCesiumSampleProbeRequest()
{
	if (!bCesiumProbeActive)
	{
		return;
	}

	ACesium3DTileset* Tileset = CesiumProbeTileset.Get();
	if (!Tileset)
	{
		UE_LOG(LogFireSimulation2, Warning, TEXT("CesiumProbe: aborted reason=tileset_invalid"));
		bCesiumProbeActive = false;
		return;
	}

	if (!CesiumProbeCounts.IsValidIndex(CesiumProbeRequestIndex))
	{
		UE_LOG(LogFireSimulation2, Display, TEXT("CesiumProbe: complete all requests dispatched and callbacks received."));
		bCesiumProbeActive = false;
		return;
	}

	const int32 RequestId = CesiumProbeRequestIndex + 1;
	const int32 PointCount = CesiumProbeCounts[CesiumProbeRequestIndex];
	const double DispatchTs = FPlatformTime::Seconds();
	const uint32 DispatchThreadId = FPlatformTLS::GetCurrentThreadId();

	TArray<FVector> Positions;
	Positions.Reserve(PointCount);
	const double BaseLon = static_cast<double>(CesiumProbeBaseLonLat.X);
	const double BaseLat = static_cast<double>(CesiumProbeBaseLonLat.Y);
	for (int32 i = 0; i < PointCount; ++i)
	{
		const int32 GridX = i % 32;
		const int32 GridY = i / 32;
		const double Lon = BaseLon + static_cast<double>(GridX) * 0.002;
		const double Lat = BaseLat + static_cast<double>(GridY) * 0.002;
		Positions.Add(FVector(Lon, Lat, 0.0));
	}

	UE_LOG(
		LogFireSimulation2,
		Display,
		TEXT("CesiumProbe: request_dispatched id=%d points=%d ts=%.3f thread=%u onGameThread=%d"),
		RequestId,
		PointCount,
		DispatchTs,
		DispatchThreadId,
		IsInGameThread() ? 1 : 0);

	Tileset->SampleHeightMostDetailed(
		Positions,
		FCesiumSampleHeightMostDetailedCallback::CreateLambda(
			[this, DispatchTs, RequestId, PointCount](
				ACesium3DTileset* InTileset,
				const TArray<FCesiumSampleHeightResult>& Results,
				const TArray<FString>& Warnings)
			{
				const double CallbackTs = FPlatformTime::Seconds();
				const uint32 CallbackThreadId = FPlatformTLS::GetCurrentThreadId();
				int32 Successes = 0;
				for (const FCesiumSampleHeightResult& R : Results)
				{
					if (R.SampleSuccess)
					{
						++Successes;
					}
				}

				UE_LOG(
					LogFireSimulation2,
					Display,
					TEXT("CesiumProbe: callback_received id=%d points=%d results=%d success=%d warnings=%d ts=%.3f latencyMs=%.2f thread=%u onGameThread=%d tileset=%s"),
					RequestId,
					PointCount,
					Results.Num(),
					Successes,
					Warnings.Num(),
					CallbackTs,
					(CallbackTs - DispatchTs) * 1000.0,
					CallbackThreadId,
					IsInGameThread() ? 1 : 0,
					*GetNameSafe(InTileset));

				for (const FString& Warning : Warnings)
				{
					UE_LOG(LogFireSimulation2, Warning, TEXT("CesiumProbe: warning id=%d %s"), RequestId, *Warning);
				}

				++CesiumProbeRequestIndex;
				DispatchCesiumSampleProbeRequest();
			}));
}

void AFireMapController::SetupRuntimeYearInput()
{
	APlayerController* PlayerController = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PlayerController)
	{
		UE_LOG(LogFireSimulation2, Warning, TEXT("FireMapController: No player controller found. Runtime shortcuts are disabled."));
		return;
	}

	EnableInput(PlayerController);
	if (!InputComponent)
	{
		UE_LOG(LogFireSimulation2, Warning, TEXT("FireMapController: Failed to create input component for runtime shortcuts."));
		return;
	}

	// Year filters
	InputComponent->BindKey(EKeys::Right, IE_Pressed, this, &AFireMapController::SelectNextYear);
	InputComponent->BindKey(EKeys::Left, IE_Pressed, this, &AFireMapController::SelectPreviousYear);
	InputComponent->BindKey(EKeys::Down, IE_Pressed, this, &AFireMapController::SelectFirstYear);
	InputComponent->BindKey(EKeys::Up, IE_Pressed, this, &AFireMapController::SelectLastYear);

	// Zoom
	InputComponent->BindKey(EKeys::MouseScrollUp, IE_Pressed, this, &AFireMapController::ZoomIn);
	InputComponent->BindKey(EKeys::MouseScrollDown, IE_Pressed, this, &AFireMapController::ZoomOut);
	InputComponent->BindKey(EKeys::Equals, IE_Pressed, this, &AFireMapController::ZoomIn);
	InputComponent->BindKey(EKeys::Hyphen, IE_Pressed, this, &AFireMapController::ZoomOut);

	// Fire focus navigation
	InputComponent->BindKey(EKeys::Period, IE_Pressed, this, &AFireMapController::FocusNextFire);
	InputComponent->BindKey(EKeys::Comma, IE_Pressed, this, &AFireMapController::FocusPreviousFire);
	InputComponent->BindKey(EKeys::LeftMouseButton, IE_Pressed, this, &AFireMapController::HandleSelectFireAtCursor);
	InputComponent->BindKey(EKeys::Escape, IE_Pressed, this, &AFireMapController::ReturnToOverviewView);
	InputComponent->BindKey(EKeys::Tab,    IE_Pressed, this, &AFireMapController::ToggleFilterPanel);
	InputComponent->BindKey(EKeys::F,      IE_Pressed, this, &AFireMapController::ToggleFilterPanel);
	InputComponent->BindKey(EKeys::F6,     IE_Pressed, this, &AFireMapController::SetViewModeLit);
	InputComponent->BindKey(EKeys::F7,     IE_Pressed, this, &AFireMapController::SetViewModeWireframe);

	UE_LOG(LogFireSimulation2, Display, TEXT("Controls: Arrows=year, MouseWheel/+/−=zoom, click fire points to move on top of selected pillar, Esc=return to overview, ,/.=focus previous/next fire."));
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(1002, 15.0f, FColor::Cyan, TEXT("Wildfire controls: Arrows=year | MouseWheel=zoom | Hover=details | Click plot=on top of pillar | Esc=overview"));
	}
}

void AFireMapController::SetupRuntimeCameraAndMovement()
{
	APlayerController* PlayerController = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PlayerController)
	{
		return;
	}

	PlayerController->bShowMouseCursor = true;
	PlayerController->bEnableClickEvents = true;
	PlayerController->bEnableMouseOverEvents = true;
	FInputModeGameAndUI InputMode;
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	PlayerController->SetInputMode(InputMode);

	APawn* Pawn = PlayerController->GetPawn();
	if (!Pawn)
	{
		return;
	}

	if (UFloatingPawnMovement* Move = Pawn->FindComponentByClass<UFloatingPawnMovement>())
	{
		Move->MaxSpeed = PawnMaxSpeed;
		Move->Acceleration = PawnAcceleration;
		Move->Deceleration = PawnDeceleration;
	}

	if (APlayerCameraManager* CameraManager = PlayerController->PlayerCameraManager)
	{
		CameraManager->SetFOV(90.0f);
	}

	ApplySensitivityToController();

	if (UCameraComponent* Camera = Pawn->FindComponentByClass<UCameraComponent>())
	{
		Camera->SetFieldOfView(90.0f);
	}

	// Force lit shading for normal gameplay; wireframe remains available on F7.
	SetViewModeLit();
}

void AFireMapController::ZoomIn()
{
	APlayerController* PlayerController = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (PlayerController && PlayerController->PlayerCameraManager)
	{
		float CurrentFov = PlayerController->PlayerCameraManager->GetFOVAngle();
		if (APawn* Pawn = PlayerController->GetPawn())
		{
			if (UCameraComponent* Camera = Pawn->FindComponentByClass<UCameraComponent>())
			{
				CurrentFov = Camera->FieldOfView;
			}
		}
		const float NewFov = FMath::Clamp(CurrentFov - ZoomStepFov, MinZoomFov, MaxZoomFov);
		PlayerController->PlayerCameraManager->SetFOV(NewFov);
		if (APawn* Pawn = PlayerController->GetPawn())
		{
			if (UCameraComponent* Camera = Pawn->FindComponentByClass<UCameraComponent>())
			{
				Camera->SetFieldOfView(NewFov);
			}
		}
	}
}

void AFireMapController::ZoomOut()
{
	APlayerController* PlayerController = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (PlayerController && PlayerController->PlayerCameraManager)
	{
		float CurrentFov = PlayerController->PlayerCameraManager->GetFOVAngle();
		if (APawn* Pawn = PlayerController->GetPawn())
		{
			if (UCameraComponent* Camera = Pawn->FindComponentByClass<UCameraComponent>())
			{
				CurrentFov = Camera->FieldOfView;
			}
		}
		const float NewFov = FMath::Clamp(CurrentFov + ZoomStepFov, MinZoomFov, MaxZoomFov);
		PlayerController->PlayerCameraManager->SetFOV(NewFov);
		if (APawn* Pawn = PlayerController->GetPawn())
		{
			if (UCameraComponent* Camera = Pawn->FindComponentByClass<UCameraComponent>())
			{
				Camera->SetFieldOfView(NewFov);
			}
		}
	}
}

void AFireMapController::FocusNextFire()
{
	FocusFireByOffset(1);
}

void AFireMapController::FocusPreviousFire()
{
	FocusFireByOffset(-1);
}

void AFireMapController::MovePlayerToPillarTop(const int32 EventIndex)
{
	if (!NationalRenderer || !RenderedEventCenters.IsValidIndex(EventIndex) || !RenderedEventGroundCenters.IsValidIndex(EventIndex))
	{
		return;
	}

	APlayerController* PlayerController = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PlayerController)
	{
		UE_LOG(LogFireSimulation2, Warning, TEXT("MovePlayerToPillarTop: no PlayerController found."));
		return;
	}

	APawn* ControlledPawn = PlayerController->GetPawn();
	if (!ControlledPawn)
	{
		UE_LOG(LogFireSimulation2, Warning, TEXT("MovePlayerToPillarTop: no controlled pawn found."));
		return;
	}

	// Capture overview camera state before any pillar-descent rotation is applied.
	if (!bHasOverviewViewState)
	{
		AActor* ActiveViewTarget = PlayerController->GetViewTarget();
		CacheOverviewViewState(PlayerController, ActiveViewTarget ? ActiveViewTarget : static_cast<AActor*>(ControlledPawn));
	}

	// Helper: build a camera rotator from a look direction with the local surface normal
	// as the horizon reference, so the camera never appears rolled/upside-down on the globe.
	auto MakeSurfaceAlignedRotation = [](const FVector& LookDir, const FVector& LocalUp) -> FRotator
	{
		const FVector SafeLook = LookDir.GetSafeNormal();
		const FVector SafeUp   = LocalUp.GetSafeNormal();
		// When looking nearly straight down the surface normal, pick a stable fallback right.
		if (FMath::Abs(FVector::DotProduct(SafeLook, SafeUp)) > 0.999f)
		{
			// Camera is looking directly along the up axis — pick global X as right reference.
			const FVector Right = FVector::CrossProduct(SafeLook, FVector(1,0,0)).GetSafeNormal();
			const FVector Up2   = FVector::CrossProduct(Right, SafeLook).GetSafeNormal();
			return FRotationMatrix::MakeFromXZ(SafeLook, Up2).Rotator();
		}
		return FRotationMatrix::MakeFromXZ(SafeLook, SafeUp).Rotator();
	};

	UInstancedStaticMeshComponent* PointInstances = NationalRenderer->GetPointInstances();
	if (!PointInstances)
	{
		const FVector FallbackPillarUp = (RenderedEventCenters[EventIndex] - RenderedEventGroundCenters[EventIndex]).GetSafeNormal();
		const FVector SafeUp = FallbackPillarUp.IsNearlyZero() ? FVector::UpVector : FallbackPillarUp;
		const float EffectiveLiftCm = FMath::Max(PillarTopCameraLiftCm, 150000.0f);
		const FVector SpawnLocation = RenderedEventCenters[EventIndex] + (SafeUp * ((PointMarkerPillarHeightCm * 0.5f) + EffectiveLiftCm));
		const FVector LookDir = (RenderedEventGroundCenters[EventIndex] - SpawnLocation).GetSafeNormal();
		const FRotator LookDownRotation = MakeSurfaceAlignedRotation(LookDir, SafeUp);
		ControlledPawn->SetActorLocation(SpawnLocation, false, nullptr, ETeleportType::TeleportPhysics);
		ControlledPawn->SetActorRotation(LookDownRotation, ETeleportType::TeleportPhysics);
		PlayerController->SetControlRotation(LookDownRotation);
		PlayerController->SetViewTarget(ControlledPawn);
		return;
	}

	FTransform InstanceTransform;
	if (!PointInstances->GetInstanceTransform(EventIndex, InstanceTransform, true))
	{
		const FVector FallbackPillarUp = (RenderedEventCenters[EventIndex] - RenderedEventGroundCenters[EventIndex]).GetSafeNormal();
		const FVector SafeUp = FallbackPillarUp.IsNearlyZero() ? FVector::UpVector : FallbackPillarUp;
		const float EffectiveLiftCm = FMath::Max(PillarTopCameraLiftCm, 150000.0f);
		const FVector SpawnLocation = RenderedEventCenters[EventIndex] + (SafeUp * ((PointMarkerPillarHeightCm * 0.5f) + EffectiveLiftCm));
		const FVector LookDir = (RenderedEventGroundCenters[EventIndex] - SpawnLocation).GetSafeNormal();
		const FRotator LookDownRotation = MakeSurfaceAlignedRotation(LookDir, SafeUp);
		ControlledPawn->SetActorLocation(SpawnLocation, false, nullptr, ETeleportType::TeleportPhysics);
		ControlledPawn->SetActorRotation(LookDownRotation, ETeleportType::TeleportPhysics);
		PlayerController->SetControlRotation(LookDownRotation);
		PlayerController->SetViewTarget(ControlledPawn);
		return;
	}

	float MeshHalfHeight = 100.0f;
	if (UStaticMesh* MarkerMesh = PointInstances->GetStaticMesh())
	{
		MeshHalfHeight = MarkerMesh->GetBounds().BoxExtent.Z;
	}

	// PillarAxis is the instance Z-axis = the local outward surface normal at this globe position.
	const FVector PillarAxis = InstanceTransform.GetUnitAxis(EAxis::Z).GetSafeNormal();
	const FVector PillarTop  = InstanceTransform.TransformPosition(FVector(0.0f, 0.0f, MeshHalfHeight));
	const float PawnHalfHeight  = FMath::Max(ControlledPawn->GetSimpleCollisionHalfHeight(), 0.0f);
	const float EffectiveLiftCm = FMath::Max(PillarTopCameraLiftCm, 150000.0f);
	// Lift camera radially outward along PillarAxis only — no global-Z offset.
	const FVector SpawnLocation = PillarTop + (PillarAxis * (PawnHalfHeight + EffectiveLiftCm));
	// Look radially inward (down toward the ground) with surface normal as camera-up.
	const FVector LookDirDown   = -PillarAxis;
	const FRotator LookDownRotation = MakeSurfaceAlignedRotation(LookDirDown, PillarAxis);

	ControlledPawn->SetActorLocation(SpawnLocation, false, nullptr, ETeleportType::TeleportPhysics);
	ControlledPawn->SetActorRotation(LookDownRotation, ETeleportType::TeleportPhysics);
	PlayerController->SetControlRotation(LookDownRotation);
	PlayerController->SetViewTarget(ControlledPawn);

	UE_LOG(LogFireSimulation2, Display, TEXT("MovePlayerToPillarTop: moved pawn to pillar top at %s (event %d/%d). Starting auto-descent."), *SpawnLocation.ToString(), EventIndex + 1, RenderedEventCenters.Num());

	// Descent target: 3000 m above fire, rotated with local surface normal as horizon reference.
	FVector TargetCameraPos = SpawnLocation;
	FRotator TargetRotation = LookDownRotation;

	if (RenderedEvents.IsValidIndex(EventIndex))
	{
		UGameInstance* GI = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
		if (UFireDataSubsystem* FDS = GI ? GI->GetSubsystem<UFireDataSubsystem>() : nullptr)
		{
			const FFireEventAttributes& Attrs = RenderedEvents[EventIndex].Attributes;
			const FVector Above3km  = FDS->LonLatToLocalWorld(Attrs.Longitude, Attrs.Latitude, 3000.0);
			const FVector LookAt    = FDS->LonLatToLocalWorld(Attrs.Longitude, Attrs.Latitude, 0.0);
			// Local surface normal: direction from surface to sky at this lat/lon.
			const FVector HighPoint = FDS->LonLatToLocalWorld(Attrs.Longitude, Attrs.Latitude, 13000.0);
			const FVector LocalUp   = (HighPoint - Above3km).GetSafeNormal();
			// Place camera above 3 km along the local surface normal.
			TargetCameraPos = Above3km + LocalUp * FocusHeightOffset;
			const FVector ToFire    = (LookAt - TargetCameraPos).GetSafeNormal();
			TargetRotation  = MakeSurfaceAlignedRotation(ToFire, LocalUp);
		}
	}

	AutoDescendStartLocation  = SpawnLocation;
	AutoDescendStartRotation  = LookDownRotation;
	AutoDescendTargetLocation = TargetCameraPos;
	AutoDescendTargetRotation = TargetRotation;
	AutoDescendAlpha          = 0.0f;
	bIsAutoDescending         = true;
}

void AFireMapController::CacheOverviewViewState(APlayerController* PlayerController, AActor* ViewTarget)
{
	if (!PlayerController || !ViewTarget)
	{
		return;
	}

	if (ViewTarget == PlayerController)
	{
		if (APawn* Pawn = PlayerController->GetPawn())
		{
			ViewTarget = Pawn;
		}
	}

	OverviewViewTarget = ViewTarget;
	OverviewViewLocation = ViewTarget->GetActorLocation();
	// Cache controller-facing rotation and force zero roll so returning from fire view
	// cannot restore a sideways camera orientation.
	OverviewViewRotation = PlayerController->GetControlRotation();
	OverviewViewRotation.Roll = 0.0f;
	OverviewViewFov = PlayerController->PlayerCameraManager ? PlayerController->PlayerCameraManager->GetFOVAngle() : 90.0f;
	if (UCameraComponent* TargetCamera = ViewTarget->FindComponentByClass<UCameraComponent>())
	{
		OverviewViewFov = TargetCamera->FieldOfView;
	}
	bHasOverviewViewState = true;
}

void AFireMapController::FocusFireByOffset(int32 Offset)
{
	if (RenderedEventCenters.Num() == 0)
	{
		return;
	}

	if (FocusedEventIndex == INDEX_NONE)
	{
		FocusedEventIndex = 0;
	}
	else
	{
		const int32 Num = RenderedEventCenters.Num();
		FocusedEventIndex = (FocusedEventIndex + Offset + Num) % Num;
	}

	APlayerController* PlayerController = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PlayerController)
	{
		UE_LOG(LogFireSimulation2, Warning, TEXT("FocusFireByOffset: no PlayerController found."));
		return;
	}
	PlayerController->SetCinematicMode(false, false, false, true, true);
	APawn* ControlledPawn = PlayerController->GetPawn();
	if (!ControlledPawn)
	{
		UE_LOG(LogFireSimulation2, Warning, TEXT("FocusFireByOffset: no controlled pawn found."));
		return;
	}

	FVector GroundCenter = RenderedEventGroundCenters.IsValidIndex(FocusedEventIndex) ? RenderedEventGroundCenters[FocusedEventIndex] : RenderedEventCenters[FocusedEventIndex];
	FVector GroundNormal = FVector::UpVector;
	ResolveFocusedGroundAnchor(GroundCenter, GroundNormal);
	const FVector TerrainUp = FVector::UpVector;
	FVector HorizontalViewDir = ControlledPawn->GetActorForwardVector();
	HorizontalViewDir.Z = 0.0f;
	if (HorizontalViewDir.SizeSquared() < KINDA_SMALL_NUMBER)
	{
		HorizontalViewDir = FVector::ForwardVector;
	}
	HorizontalViewDir = HorizontalViewDir.GetSafeNormal();

	// Keep both fire and camera strictly above the selected terrain point.
	const float FireLiftCm = FMath::Max(FireSpawnHeightOffset + FireMinTerrainLiftCm, 5000.0f);
	FVector FirePos = GroundCenter + (TerrainUp * FireLiftCm);
	FVector CameraPos = FirePos + (TerrainUp * FocusHeightOffset) - (HorizontalViewDir * FocusHorizontalDistanceCm);

	// Hard safety clamp in world-Z so focus cannot end under terrain.
	{
		FCollisionQueryParams GroundProbeParams(SCENE_QUERY_STAT(FocusTerrainClearance), false);
		GroundProbeParams.AddIgnoredActor(ControlledPawn);
		GroundProbeParams.AddIgnoredActor(this);
		if (NationalRenderer && NationalRenderer->GetPointInstances())
		{
			GroundProbeParams.AddIgnoredComponent(NationalRenderer->GetPointInstances());
		}

		const float ExtraSafetyLiftCm = FMath::Max(FocusMinTerrainClearanceCm, 10000.0f);
		FHitResult GroundAtEventHit;
		if (GetWorld()->LineTraceSingleByChannel(
			GroundAtEventHit,
			GroundCenter + FVector(0.0f, 0.0f, 500000.0f),
			GroundCenter - FVector(0.0f, 0.0f, 500000.0f),
			ECC_Visibility,
			GroundProbeParams))
		{
			GroundCenter = GroundAtEventHit.ImpactPoint;
			FirePos = GroundCenter + FVector(0.0f, 0.0f, FireLiftCm);
			const float MinCameraZAtEvent = GroundAtEventHit.ImpactPoint.Z + FocusHeightOffset + ExtraSafetyLiftCm;
			CameraPos.Z = FMath::Max(CameraPos.Z, MinCameraZAtEvent);
		}

		FHitResult GroundAtCameraHit;
		if (GetWorld()->LineTraceSingleByChannel(
			GroundAtCameraHit,
			CameraPos + FVector(0.0f, 0.0f, 500000.0f),
			CameraPos - FVector(0.0f, 0.0f, 500000.0f),
			ECC_Visibility,
			GroundProbeParams))
		{
			const float MinSafeCameraZ = GroundAtCameraHit.ImpactPoint.Z + ExtraSafetyLiftCm;
			CameraPos.Z = FMath::Max(CameraPos.Z, MinSafeCameraZ);
		}
	}

	const FRotator LookRotation = (FirePos - CameraPos).Rotation();

	AActor* ActiveViewTarget = PlayerController->GetViewTarget();
	if (!bHasOverviewViewState)
	{
		CacheOverviewViewState(PlayerController, ActiveViewTarget ? ActiveViewTarget : static_cast<AActor*>(ControlledPawn));
	}

	// Click behavior: only travel the player to a terrain-level fire viewpoint.
	ControlledPawn->SetActorLocation(CameraPos, false, nullptr, ETeleportType::TeleportPhysics);
	ControlledPawn->SetActorRotation(LookRotation, ETeleportType::TeleportPhysics);
	PlayerController->SetControlRotation(LookRotation);
	PlayerController->SetViewTarget(ControlledPawn);
	bInFireView = true;
	SetViewModeLit();

	if (UCameraComponent* PawnCamera = ControlledPawn->FindComponentByClass<UCameraComponent>())
	{
		PawnCamera->SetFieldOfView(65.0f);
		PawnCamera->SetConstraintAspectRatio(false);
	}
	if (APlayerCameraManager* CameraManager = PlayerController->PlayerCameraManager)
	{
		CameraManager->SetFOV(65.0f);
		CameraManager->bDefaultConstrainAspectRatio = false;
	}
	NotifyFireFocused(FocusedEventIndex);
	UE_LOG(
		LogFireSimulation2,
		Display,
		TEXT("FocusFireByOffset: moved pawn to fire view at %s (GroundZ=%.1f FireZ=%.1f CameraZ=%.1f) | fire %d/%d"),
		*CameraPos.ToString(),
		GroundCenter.Z,
		FirePos.Z,
		CameraPos.Z,
		FocusedEventIndex + 1,
		RenderedEventCenters.Num());

	// Ensure close-range 3D fire effects are refreshed when focus mode is entered
	// from keyboard/console paths (which do not pass through click auto-descent).
	if (UWildfireVisualizationSubsystem* Sub = GetWorld()
		? GetWorld()->GetSubsystem<UWildfireVisualizationSubsystem>()
		: nullptr)
	{
		Sub->RefreshLocalFireEffectsOnly();
	}

	TWeakObjectPtr<AFireMapController> WeakThis(this);
	GetWorldTimerManager().ClearTimer(LocalFireSnapHandle);
	GetWorldTimerManager().SetTimer(LocalFireSnapHandle, [WeakThis]()
	{
		AFireMapController* Self = WeakThis.Get();
		if (!Self)
		{
			return;
		}
		if (UWildfireVisualizationSubsystem* Sub = Self->GetWorld()
			? Self->GetWorld()->GetSubsystem<UWildfireVisualizationSubsystem>()
			: nullptr)
		{
			Sub->RefreshLocalFireEffectsOnly();
		}
	}, 3.0f, false);
}

void AFireMapController::ReturnToOverviewView()
{
	APlayerController* PlayerController = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PlayerController)
	{
		return;
	}

	// Always stop any in-flight descent so it cannot re-hide the heatmap after overview return.
	bIsAutoDescending = false;
	AutoDescendAlpha = 0.0f;
	if (bSearchBarActive)
	{
		DeactivateSearchBar();
	}

	PlayerController->SetCinematicMode(false, false, false, true, true);
	PlayerController->ResetIgnoreMoveInput();
	PlayerController->ResetIgnoreLookInput();
	PlayerController->bShowMouseCursor = true;
	PlayerController->bEnableClickEvents = true;
	PlayerController->bEnableMouseOverEvents = true;
	FInputModeGameAndUI InputMode;
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	PlayerController->SetInputMode(InputMode);

	AActor* Target = OverviewViewTarget.Get();
	if (!Target)
	{
		Target = PlayerController->GetPawn();
	}
	if (Target && bHasOverviewViewState)
	{
		OverviewViewRotation.Roll = 0.0f;
		Target->SetActorLocation(OverviewViewLocation, false, nullptr, ETeleportType::TeleportPhysics);
		Target->SetActorRotation(OverviewViewRotation, ETeleportType::TeleportPhysics);
		PlayerController->SetControlRotation(OverviewViewRotation);
		PlayerController->SetViewTarget(Target);

		if (UCameraComponent* TargetCamera = Target->FindComponentByClass<UCameraComponent>())
		{
			TargetCamera->SetFieldOfView(OverviewViewFov);
			TargetCamera->SetConstraintAspectRatio(false);
		}
		if (APlayerCameraManager* CameraManager = PlayerController->PlayerCameraManager)
		{
			CameraManager->SetFOV(OverviewViewFov);
			CameraManager->bDefaultConstrainAspectRatio = false;
		}
	}
	bHasOverviewViewState = false;
	bInFireView = false;

	// Restore heatmap visibility when returning to orbital view.
	if (RegionalRenderer)
	{
		RegionalRenderer->SetHeatmapVisible(true);
	}

	if (NiagaraRenderer)
	{
		NiagaraRenderer->ClearFireNiagara();
	}
	if (VDBRenderer)
	{
		VDBRenderer->ClearFireVDB();
	}

	NotifyReturnedToOverview();
}

void AFireMapController::SetViewModeLit()
{
	if (APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
	{
		PC->ConsoleCommand(TEXT("viewmode lit"), true);
		UE_LOG(LogFireSimulation2, Display, TEXT("View mode set to Lit (F6)."));
	}
}

void AFireMapController::SetViewModeWireframe()
{
	if (APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
	{
		PC->ConsoleCommand(TEXT("viewmode wireframe"), true);
		UE_LOG(LogFireSimulation2, Display, TEXT("View mode set to Wireframe (F7)."));
	}
}

void AFireMapController::SelectNextYear()
{
	PendingYear = FMath::Clamp((PendingYear > 0 ? PendingYear : Year) + 1, MinAvailableYear, MaxAvailableYear);
	if (APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
	{
		if (AFireHUD* FireHUD = Cast<AFireHUD>(PC->GetHUD()))
		{
			FireHUD->ShowCenterPrompt(FString::Printf(TEXT("Switching Year to %d..."), PendingYear), 1.5f);
		}
	}
	GetWorldTimerManager().SetTimer(YearDebounceHandle, this, &AFireMapController::DebouncedYearRefresh, YearDebounceSeconds, false);
}

void AFireMapController::SelectPreviousYear()
{
	PendingYear = FMath::Clamp((PendingYear > 0 ? PendingYear : Year) - 1, MinAvailableYear, MaxAvailableYear);
	if (APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
	{
		if (AFireHUD* FireHUD = Cast<AFireHUD>(PC->GetHUD()))
		{
			FireHUD->ShowCenterPrompt(FString::Printf(TEXT("Switching Year to %d..."), PendingYear), 1.5f);
		}
	}
	GetWorldTimerManager().SetTimer(YearDebounceHandle, this, &AFireMapController::DebouncedYearRefresh, YearDebounceSeconds, false);
}

void AFireMapController::PrePlaceFireActors()
{
	// Load the Blueprint class
	const FSoftClassPath FirePath(TEXT("/Game/GroundFire01/BP/BP_GroundFIre_01.BP_GroundFIre_01_C"));
	UClass* FireClass = FirePath.TryLoadClass<AActor>();
	if (!FireClass || !GetWorld())
	{
		UE_LOG(LogFireSimulation2, Warning, TEXT("PrePlaceFireActors: could not load BP_GroundFIre_01."));
		return;
	}

	// Destroy previously placed fire actors so re-running is safe.
	TArray<AActor*> Existing;
	UGameplayStatics::GetAllActorsOfClass(GetWorld(), FireClass, Existing);
	for (AActor* Actor : Existing)
	{
		Actor->Destroy();
	}

	// Spawn underground — PIE duplicates these into the game world where
	// UHeterogeneousVolumeComponent initialises correctly.
	const FVector ParkLocation(0.0f, 0.0f, -999999999.0f);
	int32 Placed = 0;
	for (int32 i = 0; i < VDBMaxTotalInstances; ++i)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		if (AActor* Actor = GetWorld()->SpawnActor<AActor>(FireClass, ParkLocation, FRotator::ZeroRotator, Params))
		{
			Actor->SetActorEnableCollision(false);
			++Placed;
		}
	}

	UE_LOG(LogFireSimulation2, Display,
		TEXT("PrePlaceFireActors: placed %d BP_GroundFIre_01 actors underground. Save the level then press Play."),
		Placed);
}

void AFireMapController::SelectFirstYear()
{
	if (APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
	{
		if (AFireHUD* FireHUD = Cast<AFireHUD>(PC->GetHUD()))
		{
			FireHUD->ShowCenterPrompt(FString::Printf(TEXT("Switching Year to %d..."), MinAvailableYear), 1.5f);
		}
	}
	SetYearAndRefresh(MinAvailableYear);
	NotifyYearChanged();
}

void AFireMapController::SelectLastYear()
{
	if (APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
	{
		if (AFireHUD* FireHUD = Cast<AFireHUD>(PC->GetHUD()))
		{
			FireHUD->ShowCenterPrompt(FString::Printf(TEXT("Switching Year to %d..."), MaxAvailableYear), 1.5f);
		}
	}
	SetYearAndRefresh(MaxAvailableYear);
	NotifyYearChanged();
}

void AFireMapController::SyncRenderedCachesFromRenderer()
{
	RenderedEvents.Reset();
	RenderedEventGroundCenters.Reset();
	RenderedEventCenters.Reset();

	if (!NationalRenderer)
	{
		return;
	}

	RenderedEvents = NationalRenderer->GetRenderedEvents();
	RenderedEventGroundCenters = NationalRenderer->GetRenderedEventGroundCenters();
	RenderedEventCenters = NationalRenderer->GetRenderedEventCenters();
	if (RenderedEventCenters.Num() > 0 && FocusedEventIndex == INDEX_NONE)
	{
		FocusedEventIndex = 0;
	}

	// Build available state list for HUD filter panel.
	TSet<FString> StateSet;
	for (const FFireEventWithGeometry& E : RenderedEvents)
	{
		if (!E.Attributes.StateCode.IsEmpty())
		{
			StateSet.Add(E.Attributes.StateCode.ToUpper());
		}
	}
	TArray<FString> StateList = StateSet.Array();
	StateList.Sort();

	// Build per-state fire counts for HUD display.
	TMap<FString,int32> StateCounts;
	for (const FFireEventWithGeometry& E : RenderedEvents)
	{
		if (!E.Attributes.StateCode.IsEmpty())
		{
			StateCounts.FindOrAdd(E.Attributes.StateCode.ToUpper())++;
		}
	}

	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (AFireHUD* FireHUD = PC ? Cast<AFireHUD>(PC->GetHUD()) : nullptr)
	{
		FireHUD->SetAvailableStates(StateList);
		FireHUD->SetSelectedState(StateCode);
		FireHUD->SetMouseSensitivity(MouseSensitivity);
		FireHUD->SetStateFireCounts(StateCounts);
	}
}

void AFireMapController::ClearVisualization()
{
	if (NationalRenderer)
	{
		NationalRenderer->ClearNationalView();
	}
	if (RegionalRenderer)
	{
		RegionalRenderer->ClearRegionalView();
	}
	if (LocalRenderer)
	{
		LocalRenderer->ClearLocalView();
	}
	if (NiagaraRenderer)
	{
		NiagaraRenderer->ClearFireNiagara();
	}
	if (VDBRenderer)
	{
		VDBRenderer->ClearFireVDB();
	}
	FlushPersistentDebugLines(GetWorld());
	RenderedEvents.Reset();
	RenderedEventGroundCenters.Reset();
	RenderedEventCenters.Reset();
	FocusedEventIndex = INDEX_NONE;
	HoveredEventIndex = INDEX_NONE;

	if (bInFireView)
	{
		ReturnToOverviewView();
	}
	else
	{
		bInFireView = false;
	}

	if (GEngine)
	{
		GEngine->RemoveOnScreenDebugMessage(1001);
	}
}

void AFireMapController::HandleSelectFireAtCursor()
{
	if (bStartupLoading)
	{
		return;
	}

	// Single-click escape from fire view back to overview (restores heatmap immediately).
	if (bInFireView)
	{
		ReturnToOverviewView();
		return;
	}

	APlayerController* PlayerController = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	UWorld* World = GetWorld();
	if (!PlayerController)
	{
		return;
	}

	// Check if user clicked inside the filter panel first.
	float MouseX=0.f, MouseY=0.f;
	PlayerController->GetMousePosition(MouseX, MouseY);

	// Clicking anywhere in the top bar toggles the filter panel.
	if (MouseY < 52.f)
	{
		ToggleFilterPanel();
		return;
	}

	if (AFireHUD* FireHUD = Cast<AFireHUD>(PlayerController->GetHUD()))
	{
		const FString HitResult = FireHUD->HitTestFilterPanel(MouseX, MouseY);
		if (!HitResult.IsEmpty())
		{
			if (HitResult == TEXT("__SEARCH_BAR__"))      { ActivateSearchBar(); return; }
			if (HitResult == TEXT("__SEARCH_CLEAR__"))    { if (AFireHUD* H2=FireHUD) { H2->ClearSearch(); } return; }
			if (HitResult == TEXT("__SENS_UP__"))         { IncreaseSensitivity(); return; }
			if (HitResult == TEXT("__SENS_DOWN__"))       { DecreaseSensitivity(); return; }
			if (HitResult != TEXT("__PANEL__"))           { ApplyStateFilter(HitResult); return; }
			return; // consumed by panel
		}

		// Clicking outside an open filter panel closes it and restores movement input.
		if (FireHUD->IsFilterPanelOpen())
		{
			if (bSearchBarActive)
			{
				DeactivateSearchBar();
			}
			FireHUD->ToggleFilterPanel();
			return;
		}
	}

	UInstancedStaticMeshComponent* PointInstances = NationalRenderer ? NationalRenderer->GetPointInstances() : nullptr;
	if (!PointInstances || !World)
	{
		return;
	}

	int32 BestIndex = INDEX_NONE;
	FHitResult CursorHit;
	const bool bHasCursorHit = PlayerController->GetHitResultUnderCursor(ECC_Visibility, false, CursorHit);
	if (bHasCursorHit &&
		CursorHit.Component.Get() == PointInstances &&
		CursorHit.Item != INDEX_NONE &&
		RenderedEventCenters.IsValidIndex(CursorHit.Item))
	{
		BestIndex = CursorHit.Item;
	}

	FVector ImpactFallbackPoint = FVector::ZeroVector;
	bool bHasImpactFallbackPoint = false;
	if (BestIndex == INDEX_NONE)
	{
		FVector RayOrigin;
		FVector RayDirection;
		if (PlayerController->DeprojectMousePositionToWorld(RayOrigin, RayDirection))
		{
			const FVector TraceStart = RayOrigin;
			const FVector TraceEnd = RayOrigin + (RayDirection.GetSafeNormal() * FMath::Max(ClickSelectMaxDistanceCm, 50000000.0f));

			FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(FirePointClickTrace), false);
			TraceParams.AddIgnoredActor(this);
			if (APawn* ControlledPawn = PlayerController->GetPawn())
			{
				TraceParams.AddIgnoredActor(ControlledPawn);
			}

			TArray<FHitResult> Hits;
			if (World->LineTraceMultiByChannel(Hits, TraceStart, TraceEnd, ECC_Visibility, TraceParams))
			{
				double BestHitDistanceSq = TNumericLimits<double>::Max();
				for (const FHitResult& Hit : Hits)
				{
					if (Hit.Component.Get() != PointInstances)
					{
						continue;
					}

					if (Hit.Item != INDEX_NONE && RenderedEventCenters.IsValidIndex(Hit.Item))
					{
						const double DistanceSq = FVector::DistSquared(TraceStart, Hit.ImpactPoint);
						if (DistanceSq < BestHitDistanceSq)
						{
							BestHitDistanceSq = DistanceSq;
							BestIndex = Hit.Item;
						}
					}
					else if (!bHasImpactFallbackPoint)
					{
						ImpactFallbackPoint = Hit.ImpactPoint;
						bHasImpactFallbackPoint = true;
					}
				}
			}
		}
	}

	if (BestIndex == INDEX_NONE && bHasCursorHit && CursorHit.Component.Get() == PointInstances)
	{
		ImpactFallbackPoint = CursorHit.ImpactPoint;
		bHasImpactFallbackPoint = true;
	}

	if (BestIndex == INDEX_NONE && bHasImpactFallbackPoint)
	{
		const double MaxFallbackDistanceSq = FMath::Square(FMath::Max(PointMarkerDiameterCm * 1.5, 50000.0f));
		double BestFallbackDistanceSq = MaxFallbackDistanceSq;
		for (int32 i = 0; i < RenderedEventCenters.Num(); ++i)
		{
			const double DistanceSq = FVector::DistSquared(RenderedEventCenters[i], ImpactFallbackPoint);
			if (DistanceSq < BestFallbackDistanceSq)
			{
				BestFallbackDistanceSq = DistanceSq;
				BestIndex = i;
			}
		}
	}

	if (BestIndex == INDEX_NONE && NationalRenderer)
	{
		FVector RayOrigin;
		FVector RayDirection;
		if (PlayerController->DeprojectMousePositionToWorld(RayOrigin, RayDirection))
		{
			double StrictDistanceSq = TNumericLimits<double>::Max();
			const double StrictMaxDistanceCm = FMath::Max(PointMarkerDiameterCm * 2.0, 100000.0);
			const int32 StrictIndex = NationalRenderer->FindNearestEventToRay(RayOrigin, RayDirection, StrictMaxDistanceCm, StrictDistanceSq);
			if (StrictIndex != INDEX_NONE && RenderedEventCenters.IsValidIndex(StrictIndex))
			{
				BestIndex = StrictIndex;
			}
		}
	}

	if (BestIndex != INDEX_NONE)
	{
		FocusedEventIndex = BestIndex;
		MovePlayerToPillarTop(BestIndex);
	}
}

bool AFireMapController::ResolveFocusedGroundAnchor(FVector& OutGroundAnchor, FVector& OutGroundNormal) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	const FVector TraceStart = OutGroundAnchor + FVector(0.0f, 0.0f, 4000000.0f);
	const FVector TraceEnd = OutGroundAnchor - FVector(0.0f, 0.0f, 4000000.0f);
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(FireGroundTrace), false);
	if (NationalRenderer && NationalRenderer->GetPointInstances())
	{
		QueryParams.AddIgnoredComponent(NationalRenderer->GetPointInstances());
	}
	FHitResult Hit;
	if (World->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_Visibility, QueryParams))
	{
		OutGroundAnchor = Hit.ImpactPoint;
		OutGroundNormal = Hit.ImpactNormal.GetSafeNormal();
		return true;
	}

	// Fallback oblique trace around local-up if world-Z probe misses.
	FVector UpDir = OutGroundAnchor.GetSafeNormal();
	if (UpDir.IsNearlyZero())
	{
		UpDir = FVector::UpVector;
	}
	const FVector FallbackStart = OutGroundAnchor + (UpDir * 3000000.0f);
	const FVector FallbackEnd = OutGroundAnchor - (UpDir * 3000000.0f);
	if (World->LineTraceSingleByChannel(Hit, FallbackStart, FallbackEnd, ECC_Visibility, QueryParams))
	{
		OutGroundAnchor = Hit.ImpactPoint;
		OutGroundNormal = Hit.ImpactNormal.GetSafeNormal();
		return true;
	}

	return false;
}

void AFireMapController::HidePointMarkerByIndex(const int32 EventIndex)
{
	if (NationalRenderer)
	{
		NationalRenderer->HideMarkerByIndex(EventIndex);
	}
}

int32 AFireMapController::FindNearestEventToCursor(double& OutDistanceSq) const
{
	OutDistanceSq = TNumericLimits<double>::Max();

	APlayerController* PlayerController = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PlayerController || RenderedEventCenters.Num() == 0)
	{
		return INDEX_NONE;
	}

	FVector RayOrigin;
	FVector RayDirection;
	if (!PlayerController->DeprojectMousePositionToWorld(RayOrigin, RayDirection))
	{
		return INDEX_NONE;
	}

	if (!NationalRenderer)
	{
		return INDEX_NONE;
	}

	return NationalRenderer->FindNearestEventToRay(RayOrigin, RayDirection, ClickSelectMaxDistanceCm, OutDistanceSq);
}

void AFireMapController::UpdateHoverTooltip()
{
	double HoverDistanceSq = TNumericLimits<double>::Max();
	const int32 CandidateIndex = FindNearestEventToCursor(HoverDistanceSq);
	if (CandidateIndex == INDEX_NONE || !RenderedEvents.IsValidIndex(CandidateIndex) || !RenderedEventCenters.IsValidIndex(CandidateIndex))
	{
		if (HoveredEventIndex != INDEX_NONE)
		{
			// Notify HUD that hover ended.
				APlayerController* PC2 = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
				if (AFireHUD* FireHUD = PC2 ? Cast<AFireHUD>(PC2->GetHUD()) : nullptr)
				{
					FFireHUDFireInfo EmptyInfo;
					FireHUD->SetHoveredFire(EmptyInfo, false);
				}
				if (CachedHUDWidget)
				{
					FFireHUDFireInfo EmptyInfo;
					CachedHUDWidget->OnFireHovered(EmptyInfo, false);
				}
			}
		HoveredEventIndex = INDEX_NONE;
		LastHoveredBroadcastIndex = INDEX_NONE;
		if (GEngine)
		{
			GEngine->RemoveOnScreenDebugMessage(1001);
		}
		return;
	}

	HoveredEventIndex = CandidateIndex;
	const FFireEventAttributes& Attr = RenderedEvents[HoveredEventIndex].Attributes;
	const FString HoverText = FString::Printf(
		TEXT("Fire: %s | State: %s | Year: %d | Acres: %.0f | Type: %s | Date: %s | Click to zoom to event"),
		*Attr.Name,
		*Attr.StateCode,
		Attr.Year,
		Attr.Acres,
		*Attr.IncidentType,
		*Attr.DateIso);

	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(1001, 0.0f, FColor::Yellow, HoverText);
	}

	// Broadcast to HUD widget (only when the hovered fire changes to avoid per-tick spam).
	if (HoveredEventIndex != LastHoveredBroadcastIndex)
	{
		LastHoveredBroadcastIndex = HoveredEventIndex;
		const FFireHUDFireInfo HoverInfo = BuildFireInfo(Attr);

		APlayerController* PC2 = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
		if (AFireHUD* FireHUD = PC2 ? Cast<AFireHUD>(PC2->GetHUD()) : nullptr)
		{
			FireHUD->SetHoveredFire(HoverInfo, true);
		}
		if (CachedHUDWidget)
		{
			CachedHUDWidget->OnFireHovered(HoverInfo, true);
		}
	}

	const FVector HoverUp = RenderedEventGroundCenters.IsValidIndex(HoveredEventIndex)
		? RenderedEventGroundCenters[HoveredEventIndex].GetSafeNormal()
		: RenderedEventCenters[HoveredEventIndex].GetSafeNormal();
	const FVector LabelWorld = RenderedEventCenters[HoveredEventIndex] + (HoverUp * (EventColumnHeight * 1.1f));
	DrawDebugString(GetWorld(), LabelWorld, HoverText, nullptr, FColor::Yellow, 0.0f, true);
}

// =============================================================================
// HUD helpers
// =============================================================================

void AFireMapController::AcquireHUDWidget()
{
	CachedHUDWidget = nullptr;
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PC)
	{
		return;
	}
	if (AFireHUD* FireHUD = Cast<AFireHUD>(PC->GetHUD()))
	{
		CachedHUDWidget = FireHUD->GetHUDWidget();
		if (CachedHUDWidget)
		{
			UE_LOG(LogFireSimulation2, Display, TEXT("FireMapController: HUD widget '%s' acquired."),
				*CachedHUDWidget->GetClass()->GetName());
		}
		else
		{
			UE_LOG(LogFireSimulation2, Display,
				TEXT("FireMapController: AFireHUD present — using native canvas HUD."));
		}
	}
	else
	{
		UE_LOG(LogFireSimulation2, Warning,
			TEXT("FireMapController: HUD is not AFireHUD — HUD bridge inactive. ")
			TEXT("Ensure AFireGameMode or level World Settings uses AFireGameMode/BP_FireGameMode."));
	}
}

FFireHUDFireInfo AFireMapController::BuildFireInfo(const FFireEventAttributes& Attr)
{
	FFireHUDFireInfo Info;
	Info.EventId   = Attr.EventId;
	Info.Name      = Attr.Name.IsEmpty() ? TEXT("Unknown Fire") : Attr.Name;
	Info.DateStr   = Attr.DateIso;
	Info.StateCode = Attr.StateCode;
	Info.Year      = Attr.Year;
	Info.Acres     = static_cast<float>(Attr.Acres);

	if (Info.Acres >= 1000000.0f)
	{
		Info.AcresStr = FString::Printf(TEXT("%.1fM acres"), Info.Acres / 1000000.0f);
	}
	else if (Info.Acres >= 1000.0f)
	{
		Info.AcresStr = FString::Printf(TEXT("%.1fK acres"), Info.Acres / 1000.0f);
	}
	else
	{
		Info.AcresStr = FString::Printf(TEXT("%.0f acres"), Info.Acres);
	}

	// Severity: use DnbrVal/HighThreshold ratio if available, else log10(acres).
	float SeverityNorm = 0.0f;
	if (Attr.DnbrVal > 0.0f && Attr.HighThreshold > 0.0f)
	{
		SeverityNorm = FMath::Clamp(Attr.DnbrVal / static_cast<float>(Attr.HighThreshold), 0.0f, 1.0f);
	}
	else
	{
		SeverityNorm = FMath::Clamp(
			(static_cast<float>(FMath::LogX(10.0, FMath::Max(1.0, Attr.Acres))) - 2.0f) / 5.0f,
			0.0f, 1.0f);
	}
	Info.SeverityNorm = SeverityNorm;

	if (SeverityNorm < 0.33f)      Info.SeverityStr = TEXT("Low Severity");
	else if (SeverityNorm < 0.66f) Info.SeverityStr = TEXT("Moderate Severity");
	else                            Info.SeverityStr = TEXT("High Severity");

	Info.SeverityColor = FLinearColor::LerpUsingHSV(
		FLinearColor(1.0f, 0.55f, 0.0f, 1.0f),   // orange
		FLinearColor(1.0f, 0.08f, 0.02f, 1.0f),  // deep red
		SeverityNorm);

	return Info;
}

void AFireMapController::NotifyYearChanged()
{
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (AFireHUD* FireHUD = PC ? Cast<AFireHUD>(PC->GetHUD()) : nullptr)
	{
		FireHUD->UpdateYearState(Year, MinAvailableYear, MaxAvailableYear,
			RenderedEvents.Num(), StateCode);
	}
	if (!CachedHUDWidget)
	{
		return;
	}
	CachedHUDWidget->OnYearChanged(Year, MinAvailableYear, MaxAvailableYear,
		RenderedEvents.Num(), StateCode);
}

void AFireMapController::NotifyFireFocused(const int32 EventIndex)
{
	if (!RenderedEvents.IsValidIndex(EventIndex))
	{
		return;
	}
	const FFireHUDFireInfo Info = BuildFireInfo(RenderedEvents[EventIndex].Attributes);

	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (AFireHUD* FireHUD = PC ? Cast<AFireHUD>(PC->GetHUD()) : nullptr)
	{
		FireHUD->SetFocusedFire(Info);
	}
	if (CachedHUDWidget)
	{
		CachedHUDWidget->OnFireFocused(Info);
	}
}

void AFireMapController::NotifyReturnedToOverview()
{
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (AFireHUD* FireHUD = PC ? Cast<AFireHUD>(PC->GetHUD()) : nullptr)
	{
		FireHUD->ClearFocusedFire();
	}
	if (!CachedHUDWidget)
	{
		return;
	}
	CachedHUDWidget->OnReturnedToOverview();
}

// =============================================================================
// New methods: debounce, filter panel, sensitivity, search bar
// =============================================================================

void AFireMapController::DebouncedYearRefresh()
{
	const int32 TargetYear = PendingYear > 0 ? PendingYear : Year;
	PendingYear = 0;
	SetYearAndRefresh(TargetYear);
	NotifyYearChanged();
}

void AFireMapController::ToggleFilterPanel()
{
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (AFireHUD* FireHUD = PC ? Cast<AFireHUD>(PC->GetHUD()) : nullptr)
	{
		const bool bWasOpen = FireHUD->IsFilterPanelOpen();
		if (bWasOpen && bSearchBarActive)
		{
			DeactivateSearchBar();
		}
		FireHUD->ToggleFilterPanel();
	}
}

void AFireMapController::ApplyStateFilter(const FString& NewState)
{
	const FString RequestedState = NewState.Equals(TEXT("ALL"), ESearchCase::IgnoreCase)
		? TEXT("ALL")
		: NewState.ToUpper();
	const bool bClickedSameStateToClear =
		!RequestedState.Equals(TEXT("ALL"), ESearchCase::IgnoreCase) &&
		RequestedState.Equals(StateCode, ESearchCase::IgnoreCase);
	StateCode = bClickedSameStateToClear ? TEXT("ALL") : RequestedState;

	// Immediately update HUD visuals (flash + selection) before the slower refresh.
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (AFireHUD* FireHUD = PC ? Cast<AFireHUD>(PC->GetHUD()) : nullptr)
	{
		FireHUD->SetSelectedState(StateCode);
		FireHUD->NotifyFilterItemClicked(StateCode);
		FireHUD->ShowCenterPrompt(FString::Printf(TEXT("[%s] Filter Applied"), *StateCode), 2.0f);
	}

	SetFiltersAndRefresh(Year, StateCode, StartDayOfYear, EndDayOfYear);
	NotifyYearChanged();
}

void AFireMapController::IncreaseSensitivity()
{
	MouseSensitivity = FMath::Clamp(MouseSensitivity + 0.25f, 0.1f, 5.0f);
	ApplySensitivityToController();
}

void AFireMapController::DecreaseSensitivity()
{
	MouseSensitivity = FMath::Clamp(MouseSensitivity - 0.25f, 0.1f, 5.0f);
	ApplySensitivityToController();
}

void AFireMapController::ApplySensitivityToController()
{
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PC) { return; }

	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	PC->InputYawScale_DEPRECATED   =  MouseSensitivity * 2.5f;
	PC->InputPitchScale_DEPRECATED = -MouseSensitivity * 1.765f;
	PRAGMA_ENABLE_DEPRECATION_WARNINGS

	if (APawn* Pawn = PC->GetPawn())
	{
		if (UFloatingPawnMovement* Move = Pawn->FindComponentByClass<UFloatingPawnMovement>())
		{
			Move->MaxSpeed = PawnMaxSpeed * MouseSensitivity;
		}
	}

	if (AFireHUD* FireHUD = Cast<AFireHUD>(PC->GetHUD()))
	{
		FireHUD->SetMouseSensitivity(MouseSensitivity);
	}
}

void AFireMapController::ActivateSearchBar()
{
	if (bSearchBarActive)
	{
		return;
	}
	bSearchBarActive = true;
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (PC)
	{
		PC->ResetIgnoreMoveInput();
		PC->SetIgnoreMoveInput(true);
	}
	if (AFireHUD* FireHUD = PC ? Cast<AFireHUD>(PC->GetHUD()) : nullptr)
	{
		if (!FireHUD->IsFilterPanelOpen()) { FireHUD->ToggleFilterPanel(); }
		FireHUD->ActivateSearchBar();
	}
}

void AFireMapController::DeactivateSearchBar()
{
	if (!bSearchBarActive)
	{
		return;
	}
	bSearchBarActive = false;
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (PC && !bStartupLoading)
	{
		PC->ResetIgnoreMoveInput();
	}
	if (AFireHUD* FireHUD = PC ? Cast<AFireHUD>(PC->GetHUD()) : nullptr)
	{
		FireHUD->DeactivateSearchBar();
	}
}

void AFireMapController::TickSearchInput()
{
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PC) { return; }
	AFireHUD* FireHUD = Cast<AFireHUD>(PC->GetHUD());
	if (!FireHUD) { return; }

	// Close/confirm
	if (PC->WasInputKeyJustPressed(EKeys::Escape) || PC->WasInputKeyJustPressed(EKeys::Tab))
	{
		DeactivateSearchBar();
		return;
	}
	if (PC->WasInputKeyJustPressed(EKeys::BackSpace)) { FireHUD->BackspaceSearch(); return; }

	// Letter keys A-Z
	static const TPair<FKey,TCHAR> KeyMap[] =
	{
		{EKeys::A,'A'},{EKeys::B,'B'},{EKeys::C,'C'},{EKeys::D,'D'},{EKeys::E,'E'},
		{EKeys::F,'F'},{EKeys::G,'G'},{EKeys::H,'H'},{EKeys::I,'I'},{EKeys::J,'J'},
		{EKeys::K,'K'},{EKeys::L,'L'},{EKeys::M,'M'},{EKeys::N,'N'},{EKeys::O,'O'},
		{EKeys::P,'P'},{EKeys::Q,'Q'},{EKeys::R,'R'},{EKeys::S,'S'},{EKeys::T,'T'},
		{EKeys::U,'U'},{EKeys::V,'V'},{EKeys::W,'W'},{EKeys::X,'X'},{EKeys::Y,'Y'},
		{EKeys::Z,'Z'},
	};
	for (const auto& KV : KeyMap)
	{
		if (PC->WasInputKeyJustPressed(KV.Key)) { FireHUD->AppendSearchChar(KV.Value); return; }
	}
}
