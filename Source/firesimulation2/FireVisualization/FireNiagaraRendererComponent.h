#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "FireDataTypes.h"
#include "FireNiagaraRendererComponent.generated.h"

class UFireDataSubsystem;
class UMaterialInterface;
class UNiagaraComponent;
class UNiagaraSystem;
class USceneComponent;

/**
 * Renders animated Niagara fire effects grounded to the Cesium terrain surface at interior
 * sample points within each fire perimeter polygon.
 *
 * For each active event the component:
 *   1. Samples a regular lon/lat grid across the outer perimeter ring and retains only
 *      points that pass an O(n) ray-casting point-in-polygon test.
 *   2. Fires a world-Z terrain line trace at each valid sample to find the true Cesium tile
 *      surface height (matching the approach used by the pillar and heatmap renderers).
 *   3. Places a pooled UNiagaraComponent at the resolved terrain point and drives per-instance
 *      Niagara user parameters from severity data so larger, hotter fires emit denser flames.
 *
 * The component pools Niagara instances (deactivate rather than destroy) to avoid per-frame
 * GC pressure when years are cycled.
 *
 * Parameter wiring — set these UPROPERTY names to match your Niagara asset's User variables:
 *   SpawnRateParameterName   (float)        particles spawned per second
 *   ScaleParameterName       (float)        uniform scale multiplier applied to the emitter
 *   ColorParameterName       (LinearColor)  base flame tint blended from orange→red by severity
 *
 * Assign FireNiagaraSystem in the editor (or in a Blueprint subclass) before PIE.
 */
UCLASS(ClassGroup = (Fire), meta = (BlueprintSpawnableComponent))
class FIRESIMULATION2_API UFireNiagaraRendererComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UFireNiagaraRendererComponent();

	// -------------------------------------------------------------------------
	// Lifecycle
	// -------------------------------------------------------------------------

	/** Called from AFireMapController::BeginPlay after all renderers are created. */
	void InitializeRenderer(USceneComponent* InAttachRoot);

	/**
	 * Updates runtime configuration.  Safe to call any time; takes effect on the next
	 * RenderFireNiagara call.
	 */
	void Configure(
		int32  InMaxInstancesPerEvent,
		int32  InMaxTotalInstances,
		float  InMinAcresForNiagara,
		float  InSampleSpacingKm,
		float  InNiagaraScaleBase,
		float  InActivationDistanceKm);

	// -------------------------------------------------------------------------
	// Render / Clear
	// -------------------------------------------------------------------------

	/**
	 * Places (or reactivates) Niagara fire instances for all events that are within
	 * ActivationDistanceKm of CameraLocation and exceed MinAcresForNiagara.
	 * Calls ClearFireNiagara internally before placing new instances.
	 */
	void RenderFireNiagara(
		const TArray<FFireEventWithGeometry>& Events,
		const UFireDataSubsystem*             DataSubsystem,
		const FVector&                        CameraLocation);

	/** Deactivates all active Niagara instances and returns them to the pool. */
	void ClearFireNiagara();

	// -------------------------------------------------------------------------
	// Diagnostics
	// -------------------------------------------------------------------------

	int32 GetLastRenderedInstanceCount() const { return LastRenderedInstanceCount; }
	int32 GetLastRenderedEventCount()    const { return LastRenderedEventCount; }

	// -------------------------------------------------------------------------
	// Configuration — exposed to the editor so designers can tune in-session
	// -------------------------------------------------------------------------

	/** Niagara fire system asset.  Assign this in the Details panel. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Niagara Fire|Asset")
	TObjectPtr<UNiagaraSystem> FireNiagaraSystem;

	/** Maximum Niagara instances to place within a single fire event's perimeter. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Niagara Fire|Sampling", meta = (ClampMin = "1"))
	int32 MaxInstancesPerEvent = 15;

	/** Hard cap on the total number of active Niagara instances across all events. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Niagara Fire|Sampling", meta = (ClampMin = "1"))
	int32 MaxTotalInstances = 200;

	/** Fires smaller than this (in acres) receive no Niagara effect. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Niagara Fire|Sampling", meta = (ClampMin = "0.0"))
	float MinAcresForNiagara = 500.0f;

	/**
	 * Grid spacing used when sampling interior points from each perimeter ring (kilometres).
	 * Smaller values produce denser coverage but more instances.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Niagara Fire|Sampling", meta = (ClampMin = "0.1"))
	float SampleSpacingKm = 1.5f;

	/** Niagara effects are only placed for fires within this distance of the camera (km). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Niagara Fire|Sampling", meta = (ClampMin = "0.1"))
	float ActivationDistanceKm = 20.0f;

	/** Base scale multiplier applied to every Niagara instance before severity scaling. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Niagara Fire|Appearance", meta = (ClampMin = "0.01"))
	float NiagaraScaleBase = 1.0f;

	/** Niagara user parameter name for particles-per-second spawn rate. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Niagara Fire|Parameters")
	FName SpawnRateParameterName = FName(TEXT("User.SpawnRate"));

	/** Niagara user parameter name for uniform emitter scale. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Niagara Fire|Parameters")
	FName ScaleParameterName = FName(TEXT("User.Scale"));

	/** Niagara user parameter name for flame colour tint. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Niagara Fire|Parameters")
	FName ColorParameterName = FName(TEXT("User.Color"));

private:
	// -------------------------------------------------------------------------
	// Component pool
	// -------------------------------------------------------------------------

	/**
	 * Returns a Niagara component ready for placement: either a recycled one from the pool
	 * or a newly created one.  Always (re)sets the asset so the pool survives asset swaps.
	 * Returns nullptr if FireNiagaraSystem is unset or allocation fails.
	 */
	UNiagaraComponent* AcquireFromPool();

	/** Deactivates a component and moves it back to ComponentPool. */
	void ReturnToPool(UNiagaraComponent* Component);

	// -------------------------------------------------------------------------
	// Geometry helpers
	// -------------------------------------------------------------------------

	/**
	 * Ray-casting point-in-polygon test on a lon/lat ring.
	 * Works on simple (non-self-intersecting) polygons; adequate for MTBS perimeters.
	 */
	static bool IsPointInRing(const FVector2D& Point, const TArray<FVector2D>& Ring);

	/**
	 * Fills OutPoints with lon/lat positions from a regular grid over the ring's bounding
	 * box, retaining only those that pass IsPointInRing.
	 *
	 * @param Ring            The outer perimeter ring in lon/lat (X=lon, Y=lat).
	 * @param SpacingDegrees  Grid cell size in degrees.
	 * @param MaxPoints       Hard cap; sampling stops once reached.
	 * @param OutPoints       Interior sample positions.
	 */
	static void SampleInteriorPoints(
		const TArray<FVector2D>& Ring,
		float                    SpacingDegrees,
		int32                    MaxPoints,
		TArray<FVector2D>&       OutPoints);

	// -------------------------------------------------------------------------
	// Severity helpers
	// -------------------------------------------------------------------------

	/**
	 * Maps fire severity attributes to a normalised intensity in [0, 1].
	 * Uses log10(acres) so the full range from small (100 ac) to mega fires (1M ac)
	 * maps gracefully across the output range.
	 */
	static float ComputeFireIntensity(const FFireEventAttributes& Attributes);

	// -------------------------------------------------------------------------
	// State
	// -------------------------------------------------------------------------

	/** Inactive Niagara components available for reuse. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UNiagaraComponent>> ComponentPool;

	/** Currently active Niagara components (one per placed fire sample point). */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UNiagaraComponent>> ActiveComponents;

	/** Root component that all Niagara instances attach to. */
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> AttachRoot;

	/**
	 * Additive fire glow material loaded at runtime from /Game/FX/M_FireAdditive.
	 * Overrides the sprite renderer's default material on every Niagara component so
	 * all particles use additive blending and emit vibrant HDR orange/red light that
	 * is visible from extreme viewing distances.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> FireOverrideMaterial;

	int32 LastRenderedInstanceCount = 0;
	int32 LastRenderedEventCount    = 0;
};
