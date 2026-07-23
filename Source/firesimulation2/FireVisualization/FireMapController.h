#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "FireDataTypes.h"
#include "FireHUDTypes.h"
#include "FireMapController.generated.h"

class UFireNationalRendererComponent;
class UFireRegionalRendererComponent;
class UFireLocalRendererComponent;
class UFireNiagaraRendererComponent;
class UFireVDBRendererComponent;
class UFireHUDWidget;
class APlayerController;
class ACesium3DTileset;

UCLASS()
class FIRESIMULATION2_API AFireMapController : public AActor
{
	GENERATED_BODY()

public:
	AFireMapController();

	UFUNCTION(BlueprintCallable, Category = "Fire View")
	void RefreshVisualization();

	UFUNCTION(BlueprintCallable, Category = "Fire View")
	bool SetYearAndRefresh(int32 NewYear);

	UFUNCTION(BlueprintCallable, Category = "Fire View")
	bool SetFiltersAndRefresh(int32 NewYear, const FString& NewStateCode, int32 NewStartDayOfYear, int32 NewEndDayOfYear);

	UFUNCTION(BlueprintCallable, Category = "Fire View")
	void FocusAndSpawnCurrentFire();

	UFUNCTION(BlueprintCallable, Category = "Debug")
	void DebugTeleportToFirstPillar();

	UFUNCTION(BlueprintCallable, Category = "Debug")
	void DebugDrawPerimeters(float DurationSeconds = 20.0f, float SphereRadiusCm = 3000.0f);

	/** Minimal Cesium API probe: submits escalating SampleHeightMostDetailed requests sequentially and logs callback lifecycle. */
	UFUNCTION(BlueprintCallable, Category = "Debug")
	void DebugRunCesiumSampleHeightProbe(double ProbeLongitude = -105.0, double ProbeLatitude = 40.0);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Filter")
	int32 Year = 2020;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Filter")
	FString StateCode = TEXT("ALL");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Filter")
	int32 StartDayOfYear = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Filter")
	int32 EndDayOfYear = 365;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Filter")
	int32 MinAvailableYear = 1984;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Filter")
	int32 MaxAvailableYear = 2026;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Style")
	FLinearColor PerimeterColor = FLinearColor(1.0f, 0.5f, 0.0f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Style")
	float PerimeterZOffset = 60.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Style")
	bool bShowPoints = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Style")
	bool bShowPointInstances = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Style")
	float EventDebugRadius = 500000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Style")
	float PointMarkerHeightOffset = 500000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Style")
	float PointMarkerDiameterCm = 90000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Style")
	float PointMarkerPillarHeightCm = 9000000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Style")
	float EventDebugThickness = 7000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Style")
	float EventColumnHeight = 1800000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Style")
	bool bShowPerimeterMarkers = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Style")
	float EventMarkerScale = 350000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Style")
	float PerimeterMarkerScale = 120000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Style")
	int32 PerimeterMarkerStride = 16;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Regional View")
	bool bEnableRegionalPerimeters = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Regional View")
	float RegionalPerimeterOffsetCm = 1200.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Regional View")
	float RegionalHeatmapOffsetCm = 220.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Regional View")
	double RegionalMinAcresForPerimeter = 150.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Regional View")
	int32 RegionalMaxRenderedEvents = 500;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Regional View")
	float RegionalPerimeterExaggerationScale = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local View")
	bool bEnableLocalView = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local View")
	int32 LocalMaxActiveFires = 20;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local View")
	float LocalSmokeColumnHeightCm = 250000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local View")
	float LocalSmokeColumnDiameterCm = 50000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local View")
	float LocalSmokeHeightOffsetCm = 3000.0f;

	/**
	 * Places VDBMaxTotalInstances BP_GroundFIre_01 actors underground in the editor level.
	 * Call this ONCE from the Details panel before PIE — click the button, save the level,
	 * then press Play.  UHeterogeneousVolumeComponent only renders when the actor was in the
	 * level before PIE started (editor duplication initialises the HV render proxy correctly;
	 * runtime SpawnActor does not).
	 */
	UFUNCTION(CallInEditor, Category = "VDB Fire")
	void PrePlaceFireActors();

	// ---- VDB Fire Effects --------------------------------------------------
	// Spawns BP_GroundFIre_01 actor instances at interior sample points within
	// each fire polygon.  Assign FireActorClass on the VDBRenderer component in
	// the Details panel.  Disable bEnableNiagaraFire when using VDB to avoid
	// rendering both simultaneously.

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VDB Fire")
	bool bEnableVDBFire = true;

	/** Maximum VDB fire actors per event perimeter. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VDB Fire", meta = (ClampMin = "1"))
	int32 VDBMaxInstancesPerEvent = 10;

	/** Hard cap on total active VDB fire actors across all events. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VDB Fire", meta = (ClampMin = "1"))
	int32 VDBMaxTotalInstances = 80;

	/** Fires smaller than this (acres) receive no VDB fire effect. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VDB Fire", meta = (ClampMin = "0.0"))
	float VDBMinAcresForFire = 500.0f;

	/** Interior grid spacing in kilometres. Smaller = denser coverage, more actors. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VDB Fire", meta = (ClampMin = "0.1"))
	float VDBSampleSpacingKm = 1.5f;

	/**
	 * XY (footprint) scale for each VDB fire actor. Default 3000 ≈ 600 m wide column.
	 * Match or exceed VDBScaleZ so fires appear as wide columns, not slivers.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VDB Fire", meta = (ClampMin = "0.01"))
	float VDBScaleXY = 3000.0f;

	/**
	 * Z (height) scale for each VDB fire actor before severity scaling.
	 * Default 3000 produces ~600 m tall flame columns visible from fire-focus altitude.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VDB Fire", meta = (ClampMin = "0.01"))
	float VDBScaleZ = 3000.0f;

	/** VDB fire effects are only placed within this camera distance (km). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VDB Fire", meta = (ClampMin = "0.1"))
	float VDBActivationDistanceKm = 50.0f;

	// ---- Niagara Fire Effects -----------------------------------------------
	// These control Niagara particle fire effects that are placed at interior sample
	// points within each heatmap polygon when the camera is within ActivationDistanceKm.
	// Assign a UNiagaraSystem asset to the NiagaraRenderer component in the Details panel.

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Niagara Fire")
	bool bEnableNiagaraFire = true;		// additive glow particles — vibrant fire visible from any altitude

	/** Maximum Niagara instances placed within a single fire event's perimeter. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Niagara Fire", meta = (ClampMin = "1"))
	int32 NiagaraMaxInstancesPerEvent = 15;

	/** Hard cap on total active Niagara instances across all events at once. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Niagara Fire", meta = (ClampMin = "1"))
	int32 NiagaraMaxTotalInstances = 500;

	/** Fires smaller than this (acres) receive no Niagara fire effect. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Niagara Fire", meta = (ClampMin = "0.0"))
	float NiagaraMinAcresForFire = 500.0f;

	/** Grid spacing used to sample interior fire points (kilometres). Smaller = denser. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Niagara Fire", meta = (ClampMin = "0.1"))
	float NiagaraSampleSpacingKm = 5.0f;

	/** Base scale multiplier applied to every Niagara instance before severity scaling. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Niagara Fire", meta = (ClampMin = "0.01"))
	float NiagaraScaleBase = 10000.0f;

	/** Fire effects are only rendered for events within this distance of the camera (km). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Niagara Fire", meta = (ClampMin = "0.1"))
	float NiagaraActivationDistanceKm = 5000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Navigation")
	float FocusHeightOffset = 80000.0f;	// 800 m above fire — close enough to see VDB flame columns

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Navigation")
	float FocusHorizontalDistanceCm = 45000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Navigation")
	float FocusMinTerrainClearanceCm = 12000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Navigation")
	float ZoomStepFov = 6.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Navigation")
	float MinZoomFov = 20.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Navigation")
	float MaxZoomFov = 95.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Navigation")
	float PawnMaxSpeed = 100000000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Navigation")
	float PawnAcceleration = 150000000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Navigation")
	float PawnDeceleration = 150000000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Navigation")
	float ClickSelectMaxDistanceCm = 3500000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Navigation")
	float FireSpawnHeightOffset = 5000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Navigation")
	float FireMinTerrainLiftCm = 15000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Navigation")
	float PillarTopCameraLiftCm = 150000.0f;

	/** Mouse look sensitivity multiplier applied to yaw/pitch. Default 1.0 = engine default. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Navigation", meta = (ClampMin = "0.1", ClampMax = "5.0"))
	float MouseSensitivity = 1.0f;

	/** Seconds to debounce rapid year-arrow presses before triggering a full refresh. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Performance", meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float YearDebounceSeconds = 0.15f;

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UFireNationalRendererComponent> NationalRenderer;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UFireRegionalRendererComponent> RegionalRenderer;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UFireLocalRendererComponent> LocalRenderer;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UFireNiagaraRendererComponent> NiagaraRenderer;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UFireVDBRendererComponent> VDBRenderer;

	void ClearVisualization();
	void SetupRuntimeYearInput();
	void SetupRuntimeCameraAndMovement();
	void HandleSelectFireAtCursor();
	void ZoomIn();
	void ZoomOut();
	void FocusNextFire();
	void FocusPreviousFire();
	void FocusFireByOffset(int32 Offset);
	void MovePlayerToPillarTop(int32 EventIndex);
	void ReturnToOverviewView();
	void CacheOverviewViewState(APlayerController* PlayerController, AActor* ViewTarget);
	void SelectNextYear();
	void SelectPreviousYear();
	void SelectFirstYear();
	void SelectLastYear();
	void SyncRenderedCachesFromRenderer();
	void TickAutoDescend(float DeltaSeconds);

	/** Debounced callback: fires after YearDebounceSeconds of no further year key presses. */
	void DebouncedYearRefresh();

	/** Toggles state-filter panel visibility on the HUD. */
	void ToggleFilterPanel();

	/** Applies a state filter and rebuilds visualization. */
	void ApplyStateFilter(const FString& NewState);

	/** Increases mouse sensitivity by a fixed step and applies it. */
	void IncreaseSensitivity();

	/** Decreases mouse sensitivity by a fixed step and applies it. */
	void DecreaseSensitivity();

	/** Writes current MouseSensitivity to the PlayerController input scaling. */
	void ApplySensitivityToController();

	/** Polls key presses each tick when search bar is active and routes to HUD. */
	void TickSearchInput();
	void DispatchCesiumSampleProbeRequest();
	void SetViewModeLit();
	void SetViewModeWireframe();
	void UpdateStartupLoadingState(float DeltaSeconds);

	/** Activates the search bar: blocks game input, clears buffer. */
	void ActivateSearchBar();

	/** Deactivates the search bar and restores game input. */
	void DeactivateSearchBar();

	// Debounce timer for year changes
	FTimerHandle YearDebounceHandle;
	int32        PendingYear = 0;

	// Search-bar input state
	bool bSearchBarActive = false;

	// Startup load gate: blocks interaction while the initial fire set is being prepared.
	bool bStartupLoading = true;
	float StartupLoadingElapsed = 0.0f;
	float StartupLoadingMinSeconds = 1.0f;
	float StartupLoadingMaxSeconds = 12.0f;
	float StartupDisplayedProgress01 = 0.0f;
	float StartupProgressRatePerSecond = 0.35f;
	bool bStartupDeferredRefreshDone = false;
	int32 StartupExpectedFireCount = 0;
	FTimerHandle StartupRefreshHandle;
	float StartupDeferredRefreshDelaySeconds = 10.0f;
	FTimerHandle LocalFireSnapHandle;
	FTimerHandle DescentFireSnapHandle;

	// Cesium SampleHeight API probe state
	bool bCesiumProbeActive = false;
	int32 CesiumProbeRequestIndex = 0;
	FVector2D CesiumProbeBaseLonLat = FVector2D(-105.0f, 40.0f);
	TArray<int32> CesiumProbeCounts;
	double CesiumProbeRequestDispatchTs = 0.0;
	TWeakObjectPtr<ACesium3DTileset> CesiumProbeTileset;

	// Auto-descent state — animated camera travel from pillar top to fire level.
	bool    bIsAutoDescending          = false;
	float   AutoDescendAlpha           = 0.0f;   // progress 0→1
	FVector AutoDescendStartLocation   = FVector::ZeroVector;
	FVector AutoDescendTargetLocation  = FVector::ZeroVector;
	FRotator AutoDescendStartRotation  = FRotator::ZeroRotator;
	FRotator AutoDescendTargetRotation = FRotator::ZeroRotator;

	UPROPERTY(Transient)
	TArray<FFireEventWithGeometry> RenderedEvents;

	UPROPERTY(Transient)
	TArray<FVector> RenderedEventGroundCenters;

	UPROPERTY(Transient)
	TArray<FVector> RenderedEventCenters;

	int32 FocusedEventIndex = INDEX_NONE;
	int32 HoveredEventIndex = INDEX_NONE;

	UPROPERTY(Transient)
	TObjectPtr<AActor> OverviewViewTarget;

	bool bHasOverviewViewState = false;
	bool bInFireView = false;
	FVector OverviewViewLocation = FVector::ZeroVector;
	FRotator OverviewViewRotation = FRotator::ZeroRotator;
	float OverviewViewFov = 90.0f;

	bool ResolveFocusedGroundAnchor(FVector& OutGroundAnchor, FVector& OutGroundNormal) const;
	void HidePointMarkerByIndex(int32 EventIndex);
	void UpdateHoverTooltip();
	int32 FindNearestEventToCursor(double& OutDistanceSq) const;

	// ---- HUD widget helpers -------------------------------------------------
	/** Fetches and caches the UFireHUDWidget from the active AFireHUD. */
	void AcquireHUDWidget();

	/** Builds an FFireHUDFireInfo summary from raw fire attributes. */
	static FFireHUDFireInfo BuildFireInfo(const FFireEventAttributes& Attr);

	/** Broadcasts the current year/filter state to the HUD widget (if present). */
	void NotifyYearChanged();

	/** Broadcasts fire-focused info for the given rendered-event index. */
	void NotifyFireFocused(int32 EventIndex);

	/** Broadcasts that the camera has returned to the overview globe view. */
	void NotifyReturnedToOverview();

	/** Cached pointer to the UFireHUDWidget created by AFireHUD. */
	UPROPERTY(Transient)
	TObjectPtr<UFireHUDWidget> CachedHUDWidget;

	/** Last hovered event index sent to the HUD to avoid redundant broadcasts. */
	int32 LastHoveredBroadcastIndex = INDEX_NONE;
};
