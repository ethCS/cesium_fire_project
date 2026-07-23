#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "FireDataTypes.h"
#include "Materials/MaterialInterface.h"
#include "FireVDBRendererComponent.generated.h"

class UFireDataSubsystem;
class USceneComponent;

/**
 * Renders 3D volumetric fire by pooling instances of a Blueprint actor (BP_GroundFIre_01)
 * that drives a VDB sparse volumetric texture via HeterogeneousVolumes.
 *
 * Placement mirrors FireNiagaraRendererComponent: a regular lon/lat grid is sampled
 * inside each fire perimeter ring, samples are sorted centroid-inward so the dense
 * burn core is filled first, then each point is terrain-traced to the Cesium surface.
 *
 * Actors are pooled (hidden rather than destroyed) to avoid per-cycle spawn cost.
 * Assign BP_GroundFIre_01 to FireActorClass in the Details panel before PIE.
 */
UCLASS(ClassGroup = (Fire), meta = (BlueprintSpawnableComponent))
class FIRESIMULATION2_API UFireVDBRendererComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UFireVDBRendererComponent();

    void InitializeRenderer(USceneComponent* InAttachRoot);

    void Configure(
        int32 InMaxInstancesPerEvent,
        int32 InMaxTotalInstances,
        float InMinAcresForFire,
        float InSampleSpacingKm,
        float InScaleXY,
        float InScaleZ,
        float InActivationDistanceKm);

    void RenderFireVDB(
        const TArray<FFireEventWithGeometry>& Events,
        const UFireDataSubsystem*             DataSubsystem,
        const FVector&                        CameraLocation);

    void ClearFireVDB();

    int32 GetLastRenderedInstanceCount() const { return LastRenderedInstanceCount; }
    int32 GetLastRenderedEventCount()    const { return LastRenderedEventCount; }

    // -------------------------------------------------------------------------
    // Configuration
    // -------------------------------------------------------------------------

    /**
     * Blueprint actor class to spawn per fire sample point. Assign BP_GroundFIre_01.
     *
     * TSoftClassPtr stores only the asset path — the class is NOT loaded or constructed
     * until the first spawn, which avoids the CDO crash that occurs when assigning a
     * Blueprint containing HeterogeneousVolumes via TSubclassOf in the Details panel.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VDB Fire|Asset")
    TSoftClassPtr<AActor> FireActorClass;

    /** Maximum VDB fire actors placed within a single fire event's perimeter. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VDB Fire|Sampling", meta = (ClampMin = "1"))
    int32 MaxInstancesPerEvent = 10;

    /** Hard cap on total active fire actors across all events. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VDB Fire|Sampling", meta = (ClampMin = "1"))
    int32 MaxTotalInstances = 80;

    /** Fires smaller than this (acres) receive no VDB fire effect. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VDB Fire|Sampling", meta = (ClampMin = "0.0"))
    float MinAcresForFire = 500.0f;

    /** Interior grid spacing in kilometres. Smaller = denser coverage, more actors. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VDB Fire|Sampling", meta = (ClampMin = "0.1"))
    float SampleSpacingKm = 1.5f;

    /** VDB fire effects are only placed within this camera distance (km). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VDB Fire|Sampling", meta = (ClampMin = "0.1"))
    float ActivationDistanceKm = 20.0f;

    /**
     * XY (footprint) scale applied to each fire actor.
     * For wildfires visible from fire-focus altitude (~3 km), default 3000 produces
     * ~600 m wide fire columns — proportional to the Z height.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VDB Fire|Appearance", meta = (ClampMin = "0.01"))
    float ScaleXY = 3000.0f;

    /**
     * Z (height) scale applied to each fire actor before severity scaling.
     * Default 3000 produces ~600 m tall flame columns visible from fire-focus altitude.
     * Increase for fires that need to be seen from greater distances.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VDB Fire|Appearance", meta = (ClampMin = "0.01"))
    float ScaleZ = 3000.0f;


private:
    AActor* AcquireFromPool();
    void    ReturnToPool(AActor* Actor);

    static bool IsPointInRing(const FVector2D& Point, const TArray<FVector2D>& Ring);
    static void SampleInteriorPoints(
        const TArray<FVector2D>& Ring,
        float                    SpacingDegrees,
        int32                    MaxPoints,
        TArray<FVector2D>&       OutPoints);
    static float ComputeFireIntensity(const FFireEventAttributes& Attributes);

    UPROPERTY(Transient)
    TArray<TObjectPtr<AActor>> ActorPool;

    UPROPERTY(Transient)
    TArray<TObjectPtr<AActor>> ActiveActors;

    UPROPERTY(Transient)
    TObjectPtr<USceneComponent> AttachRoot;

    /** Cached reference to MI_Fire — used as DMI parent to avoid MID-of-MID chains. */
    UPROPERTY(Transient)
    TObjectPtr<UMaterialInterface> FireBaseMaterial;

    int32 LastRenderedInstanceCount = 0;
    int32 LastRenderedEventCount    = 0;
};
