#include "FireVDBRendererComponent.h"

#include "Components/PointLightComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FireDataSubsystem.h"
#include "Components/HeterogeneousVolumeComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "firesimulation2.h"

UFireVDBRendererComponent::UFireVDBRendererComponent()
{
    PrimaryComponentTick.bCanEverTick = false;

    // Default to the project's VDB fire Blueprint so no manual assignment is required.
    FireActorClass = TSoftClassPtr<AActor>(
        FSoftObjectPath(TEXT("/Game/GroundFire01/BP/BP_GroundFIre_01.BP_GroundFIre_01_C")));
}

void UFireVDBRendererComponent::InitializeRenderer(USceneComponent* InAttachRoot)
{
    AttachRoot = InAttachRoot;

    // Cache the base fire material — always use this as the DMI parent to avoid
    // the MID-of-MID chain bug where each RenderFireVDB call wraps the previous DMI.
    // MI_Fire is a MaterialInstanceConstant (valid DMI parent); MID-of-MID is rejected
    // by UE and silently breaks all parameter overrides.
    FireBaseMaterial = Cast<UMaterialInterface>(StaticLoadObject(
        UMaterialInterface::StaticClass(), nullptr,
        TEXT("/Game/GroundFire01/Materials/MI_Fire")));
    if (!FireBaseMaterial)
    {
        UE_LOG(LogFireSimulation2, Warning,
            TEXT("FireVDBRenderer: could not load MI_Fire — fire material will use component defaults."));
    }
    // Collect pre-placed BP_GroundFIre_01 actors from the level into the pool.
    // These must exist in the level BEFORE PIE — use the PrePlaceFireActors button
    // on AFireMapController in the Details panel, then save and press Play.
    // UHeterogeneousVolumeComponent only works when the actor was part of the level
    // at PIE start; runtime SpawnActor produces invisible fire.
    UClass* FireClass = FireActorClass.LoadSynchronous();
    if (FireClass && GetWorld())
    {
        ActorPool.Reset();
        for (TActorIterator<AActor> It(GetWorld(), FireClass); It; ++It)
        {
            AActor* Actor = *It;
            if (IsValid(Actor))
            {
                Actor->SetActorEnableCollision(false);
                Actor->SetActorLocation(FVector(0.0f, 0.0f, -999999999.0f));
                ActorPool.Add(Actor);
            }
        }
        UE_LOG(LogFireSimulation2, Display,
            TEXT("FireVDBRenderer: found %d pre-placed fire actors in level pool."),
            ActorPool.Num());

        if (ActorPool.Num() == 0)
        {
            UE_LOG(LogFireSimulation2, Warning,
                TEXT("FireVDBRenderer: NO pre-placed fire actors found. ")
                TEXT("Select AFireMapController → Details → 'Pre Place Fire Actors', save level, re-run PIE."));
        }
    }
}

void UFireVDBRendererComponent::Configure(
    const int32 InMaxInstancesPerEvent,
    const int32 InMaxTotalInstances,
    const float InMinAcresForFire,
    const float InSampleSpacingKm,
    const float InScaleXY,
    const float InScaleZ,
    const float InActivationDistanceKm)
{
    MaxInstancesPerEvent  = FMath::Max(1,     InMaxInstancesPerEvent);
    MaxTotalInstances     = FMath::Max(1,     InMaxTotalInstances);
    MinAcresForFire       = FMath::Max(0.0f,  InMinAcresForFire);
    SampleSpacingKm       = FMath::Max(0.1f,  InSampleSpacingKm);
    ScaleXY               = FMath::Max(0.01f, InScaleXY);
    ScaleZ                = FMath::Max(0.01f, InScaleZ);
    ActivationDistanceKm  = FMath::Max(0.1f,  InActivationDistanceKm);
}

// ---------------------------------------------------------------------------
// Pool management
// ---------------------------------------------------------------------------

AActor* UFireVDBRendererComponent::AcquireFromPool()
{
    // Only use pre-placed actors — never spawn at runtime.
    // UHeterogeneousVolumeComponent's render proxy is only initialised during
    // the editor→PIE duplication process, not via SpawnActor at runtime.
    while (ActorPool.Num() > 0)
    {
        TObjectPtr<AActor> Candidate = ActorPool.Pop(EAllowShrinking::No);
        if (IsValid(Candidate.Get()))
        {
            ActiveActors.Add(Candidate.Get());
            return Candidate.Get();
        }
    }
    return nullptr;
}

void UFireVDBRendererComponent::ReturnToPool(AActor* Actor)
{
    if (!IsValid(Actor))
    {
        return;
    }
    // Move far underground instead of hiding — hiding can deactivate the
    // HeterogeneousVolumeComponent in ways that prevent it re-rendering later.
    Actor->SetActorLocation(FVector(0.0f, 0.0f, -999999999.0f));
    ActorPool.Add(Actor);
}

// ---------------------------------------------------------------------------
// Render / Clear
// ---------------------------------------------------------------------------

void UFireVDBRendererComponent::ClearFireVDB()
{
    for (TObjectPtr<AActor>& Actor : ActiveActors)
    {
        ReturnToPool(Actor.Get());
    }
    ActiveActors.Reset();
    LastRenderedInstanceCount = 0;
    LastRenderedEventCount    = 0;
}

void UFireVDBRendererComponent::RenderFireVDB(
    const TArray<FFireEventWithGeometry>& Events,
    const UFireDataSubsystem*             DataSubsystem,
    const FVector&                        CameraLocation)
{
    ClearFireVDB();

    if (FireActorClass.IsNull() || !DataSubsystem || !GetOwner())
    {
        UE_LOG(LogFireSimulation2, Warning,
            TEXT("FireVDBRenderer: skipped — FireActorClass is %s, DataSubsystem is %s"),
            FireActorClass.IsNull() ? TEXT("NULL (not assigned)") : TEXT("set"),
            DataSubsystem ? TEXT("valid") : TEXT("NULL"));
        return;
    }

    UE_LOG(LogFireSimulation2, Display,
        TEXT("FireVDBRenderer: RenderFireVDB called — events=%d MinAcres=%.0f"),
        Events.Num(), MinAcresForFire);

    UWorld* World = GetWorld();

    FCollisionQueryParams TraceParams(FName(TEXT("VDBFireTerrainSample")), /*bTraceComplex=*/false);
    TraceParams.AddIgnoredActor(GetOwner());

    constexpr float KmPerDegree  = 111.32f;
    const float     SpacingDegrees = SampleSpacingKm / KmPerDegree;

    // Pre-pass: find the best terrain Z from any event in the batch so trace-miss events
    // can use a geographically close fallback instead of a fixed 2000m elevation.
    float BestTerrainZ = TNumericLimits<float>::Max();
    if (World)
    {
        for (const FFireEventWithGeometry& Event : Events)
        {
            if (Event.Attributes.Acres < MinAcresForFire || Event.Geometry.Rings.Num() == 0) continue;
            const FVector Ell = DataSubsystem->LonLatToLocalWorld(Event.Attributes.Longitude, Event.Attributes.Latitude, 0.0);
            FHitResult PreHit;
            if (World->LineTraceSingleByChannel(PreHit,
                FVector(Ell.X, Ell.Y, Ell.Z + 5000000.0f),
                FVector(Ell.X, Ell.Y, Ell.Z - 500000.0f),
                ECC_Visibility, TraceParams))
            {
                // Pick the hit closest to the camera (highest Z when looking down) for the fallback.
                if (BestTerrainZ == TNumericLimits<float>::Max() || FMath::Abs(PreHit.Location.Z - CameraLocation.Z) < FMath::Abs(BestTerrainZ - CameraLocation.Z))
                {
                    BestTerrainZ = PreHit.Location.Z;
                }
            }
        }
    }
    // Last resort: 0m elevation (sea level) — better than 2000m for coastal/lowland fires.
    const FVector FallbackAt0m = Events.Num() > 0
        ? DataSubsystem->LonLatToLocalWorld(Events[0].Attributes.Longitude, Events[0].Attributes.Latitude, 0.0)
        : FVector::ZeroVector;
    if (BestTerrainZ == TNumericLimits<float>::Max())
    {
        BestTerrainZ = FallbackAt0m.Z;
    }

    int32 TotalInstances = 0;
    int32 RenderedEvents = 0;

    for (const FFireEventWithGeometry& Event : Events)
    {
        if (TotalInstances >= MaxTotalInstances)
        {
            break;
        }

        if (Event.Attributes.Acres < MinAcresForFire)
        {
            continue;
        }
        if (Event.Geometry.Rings.Num() == 0)
        {
            continue;
        }

        // Severity-driven scale.
        const float Intensity    = ComputeFireIntensity(Event.Attributes);
        const float ScaleValueXY = FMath::Max(0.01f, ScaleXY * (0.5f + Intensity * 0.5f));
        const float ScaleValueZ  = FMath::Max(0.01f, ScaleZ  * (0.5f + Intensity * 0.5f));

        // ---- Centroid terrain trace ----------------------------------------
        // Cesium tiles are only loaded near the camera. Sample points spread across
        // a large fire polygon will fail terrain traces if they're far from the camera.
        // Trace the fire's own centroid first (camera is near it after auto-descent)
        // and use the resulting Z as the authoritative fallback for all sample points.
        const FVector CentroidEllipsoid = DataSubsystem->LonLatToLocalWorld(
            Event.Attributes.Longitude, Event.Attributes.Latitude, 0.0);

        // Fallback: use the best terrain Z hit from the pre-pass (nearest successfully traced
        // fire in the same region), which keeps fires near the actual terrain elevation.
        float CentroidTerrainZ = BestTerrainZ;

        if (World)
        {
            FHitResult CentroidHit;
            if (World->LineTraceSingleByChannel(
                CentroidHit,
                FVector(CentroidEllipsoid.X, CentroidEllipsoid.Y, CentroidEllipsoid.Z + 5000000.0f),
                FVector(CentroidEllipsoid.X, CentroidEllipsoid.Y, CentroidEllipsoid.Z - 500000.0f),
                ECC_Visibility, TraceParams))
            {
                CentroidTerrainZ = CentroidHit.Location.Z;
                UE_LOG(LogFireSimulation2, Display,
                    TEXT("FireVDBRenderer: centroid terrain HIT Z=%.0f for %s"),
                    CentroidTerrainZ, *Event.Attributes.EventId);
            }
            else
            {
                UE_LOG(LogFireSimulation2, Warning,
                    TEXT("FireVDBRenderer: centroid trace missed for %s — using best-match terrain fallback Z=%.0f"),
                    *Event.Attributes.EventId, CentroidTerrainZ);
            }
        }

        // ---- Sample interior points ----------------------------------------
        const FFireRing& OuterRing = Event.Geometry.Rings[0];
        if (OuterRing.LonLatVertices.Num() < 3)
        {
            continue;
        }

        TArray<FVector2D> SamplePoints;
        SampleInteriorPoints(OuterRing.LonLatVertices, SpacingDegrees, MaxInstancesPerEvent, SamplePoints);

        if (SamplePoints.Num() == 0)
        {
            SamplePoints.Add(FVector2D(
                static_cast<float>(Event.Attributes.Longitude),
                static_cast<float>(Event.Attributes.Latitude)));
        }

        // Sort centroid-inward — highest-severity interior first.
        if (SamplePoints.Num() > 1)
        {
            FVector2D RingCentroid = FVector2D::ZeroVector;
            for (const FVector2D& V : OuterRing.LonLatVertices)
            {
                RingCentroid += V;
            }
            RingCentroid /= static_cast<float>(OuterRing.LonLatVertices.Num());
            SamplePoints.Sort([&RingCentroid](const FVector2D& A, const FVector2D& B)
            {
                return FVector2D::DistSquared(A, RingCentroid) < FVector2D::DistSquared(B, RingCentroid);
            });
        }

        bool bAnyPlacedForEvent = false;

        for (const FVector2D& LonLat : SamplePoints)
        {
            if (TotalInstances >= MaxTotalInstances)
            {
                break;
            }

            const FVector EllipsoidPos = DataSubsystem->LonLatToLocalWorld(LonLat.X, LonLat.Y, 0.0);
            FVector SpawnLocation(EllipsoidPos.X, EllipsoidPos.Y, CentroidTerrainZ + 50.0f);

            if (World)
            {
                FHitResult Hit;
                if (World->LineTraceSingleByChannel(
                    Hit,
                    FVector(EllipsoidPos.X, EllipsoidPos.Y, CentroidTerrainZ + 500000.0f),
                    FVector(EllipsoidPos.X, EllipsoidPos.Y, CentroidTerrainZ - 500000.0f),
                    ECC_Visibility, TraceParams))
                {
                    SpawnLocation = FVector(Hit.Location.X, Hit.Location.Y, Hit.Location.Z + 50.0f);
                }
                // Else: CentroidTerrainZ is already in SpawnLocation — fire appears at
                // the same elevation as the fire centroid, which is correct for mountain terrain.
            }

            AActor* FireActor = AcquireFromPool();
            if (!FireActor) { break; }

            FireActor->SetActorLocation(SpawnLocation);
            FireActor->SetActorScale3D(FVector(ScaleValueXY, ScaleValueXY, ScaleValueZ));
            FireActor->SetActorHiddenInGame(false);
            FireActor->SetActorEnableCollision(false);
            FireActor->SetActorTickEnabled(true);

            // Apply fire material parameters directly to the existing HeterogeneousVolume MID.
            // Creating a new MID on every refresh can destabilize sparse-volume rendering and
            // can produce visible volume bounds instead of filled flames.
            TArray<UActorComponent*> Comps;
            FireActor->GetComponents(Comps);
            for (UActorComponent* Comp : Comps)
            {
                if (UPrimitiveComponent* PrimComp = Cast<UPrimitiveComponent>(Comp))
                {
                    if (UHeterogeneousVolumeComponent* HV = Cast<UHeterogeneousVolumeComponent>(PrimComp))
                    {
                        HV->SetVisibility(true, true);
                        HV->SetHiddenInGame(false);
                        HV->SetComponentTickEnabled(true);
                        HV->Activate(true);
                        UMaterialInterface* Mat = HV->GetMaterial(0);
                        UMaterialInstanceDynamic* DMI = Cast<UMaterialInstanceDynamic>(Mat);
                        if (!DMI)
                        {
                            UMaterialInterface* Parent = FireBaseMaterial.Get();
                            if (Parent && !Parent->IsA<UMaterialInstanceDynamic>())
                            {
                                DMI = UMaterialInstanceDynamic::Create(Parent, HV);
                                if (DMI)
                                {
                                    HV->SetMaterial(0, DMI);
                                }
                            }
                        }

                        if (DMI)
                        {
                            // Heavier density + brighter blackbody gives a clearly visible
                            // orange flame core from overview descent, not a transparent box.
                            DMI->SetScalarParameterValue(TEXT("Brightness"),            2800.0f);
                            DMI->SetScalarParameterValue(TEXT("Density"),               0.35f);
                            DMI->SetScalarParameterValue(TEXT("BlackBody Temperature"), 4600.0f);
                            DMI->SetScalarParameterValue(TEXT("Temperature Min"),       0.45f);
                            DMI->SetScalarParameterValue(TEXT("Temperature Max"),       2.4f);
                            DMI->SetVectorParameterValue(TEXT("Fire Color"),
                                FLinearColor(1.0f, 0.30f, 0.02f, 1.0f));
                        }
                    }
                    PrimComp->SetVisibility(true, true);
                    PrimComp->SetHiddenInGame(false);
                    PrimComp->SetComponentTickEnabled(true);
                    PrimComp->Activate(true);
                    PrimComp->SetActive(true, /*bReset=*/true);
                }
                else if (USceneComponent* SceneComp = Cast<USceneComponent>(Comp))
                {
                    SceneComp->SetVisibility(true, true);
                    SceneComp->SetHiddenInGame(false);
                    SceneComp->SetComponentTickEnabled(true);
                    SceneComp->Activate(true);
                }
            }

            // Fix white point lights that cause the VDB volume to scatter white light.
            // The Blueprint has two white PointLightComponents; change them to vivid orange
            // so the scattered light matches the fire emission color.
            TArray<UPointLightComponent*> Lights;
            FireActor->GetComponents<UPointLightComponent>(Lights);
            for (UPointLightComponent* Light : Lights)
            {
                Light->SetLightColor(FLinearColor(1.0f, 0.35f, 0.06f, 1.0f)); // vivid orange
                Light->SetIntensity(2000.0f);
                Light->SetAttenuationRadius(50000.0f);
            }

            ++TotalInstances;
            bAnyPlacedForEvent = true;
        }

        if (bAnyPlacedForEvent)
        {
            ++RenderedEvents;
        }
    }

    LastRenderedInstanceCount = TotalInstances;
    LastRenderedEventCount    = RenderedEvents;

    UE_LOG(LogFireSimulation2, Display,
        TEXT("FireVDBRenderer: renderedEvents=%d instances=%d poolSize=%d"),
        LastRenderedEventCount, LastRenderedInstanceCount, ActorPool.Num());
}

// ---------------------------------------------------------------------------
// Geometry helpers
// ---------------------------------------------------------------------------

bool UFireVDBRendererComponent::IsPointInRing(const FVector2D& Point, const TArray<FVector2D>& Ring)
{
    bool bInside = false;
    const int32 N = Ring.Num();
    for (int32 i = 0, j = N - 1; i < N; j = i++)
    {
        const FVector2D& Vi = Ring[i];
        const FVector2D& Vj = Ring[j];
        if (((Vi.Y > Point.Y) != (Vj.Y > Point.Y)) &&
            (Point.X < (Vj.X - Vi.X) * (Point.Y - Vi.Y) / (Vj.Y - Vi.Y) + Vi.X))
        {
            bInside = !bInside;
        }
    }
    return bInside;
}

void UFireVDBRendererComponent::SampleInteriorPoints(
    const TArray<FVector2D>& Ring,
    const float              SpacingDegrees,
    const int32              MaxPoints,
    TArray<FVector2D>&       OutPoints)
{
    OutPoints.Reset();
    if (Ring.Num() < 3 || SpacingDegrees <= 0.0f || MaxPoints <= 0)
    {
        return;
    }

    FVector2D MinBound( TNumericLimits<float>::Max(),  TNumericLimits<float>::Max());
    FVector2D MaxBound(-TNumericLimits<float>::Max(), -TNumericLimits<float>::Max());
    for (const FVector2D& V : Ring)
    {
        MinBound.X = FMath::Min(MinBound.X, V.X);
        MinBound.Y = FMath::Min(MinBound.Y, V.Y);
        MaxBound.X = FMath::Max(MaxBound.X, V.X);
        MaxBound.Y = FMath::Max(MaxBound.Y, V.Y);
    }

    const int32 ColsEst = FMath::CeilToInt((MaxBound.X - MinBound.X) / SpacingDegrees) + 1;
    const int32 RowsEst = FMath::CeilToInt((MaxBound.Y - MinBound.Y) / SpacingDegrees) + 1;
    OutPoints.Reserve(FMath::Min(MaxPoints, ColsEst * RowsEst));

    const float StartLat = MinBound.Y + SpacingDegrees * 0.5f;
    const float StartLon = MinBound.X + SpacingDegrees * 0.5f;

    for (float Lat = StartLat; Lat < MaxBound.Y && OutPoints.Num() < MaxPoints; Lat += SpacingDegrees)
    {
        for (float Lon = StartLon; Lon < MaxBound.X && OutPoints.Num() < MaxPoints; Lon += SpacingDegrees)
        {
            if (IsPointInRing(FVector2D(Lon, Lat), Ring))
            {
                OutPoints.Add(FVector2D(Lon, Lat));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Severity helper
// ---------------------------------------------------------------------------

float UFireVDBRendererComponent::ComputeFireIntensity(const FFireEventAttributes& Attributes)
{
    // Size component: log₁₀ acres [100 ac → 0, 1M ac → 1]
    const float AcresScore = FMath::Clamp(
        (static_cast<float>(FMath::LogX(10.0, FMath::Max(1.0, Attributes.Acres))) - 2.0f) / 4.0f,
        0.0f, 1.0f);

    // Severity component: lower HighThreshold → more pixels burned severely → higher score.
    const float HighT = static_cast<float>(Attributes.HighThreshold);
    const float SeverityScore = (HighT > 0.0f && HighT < 9000.0f)
        ? FMath::Clamp(1.0f - (HighT - 100.0f) / 560.0f, 0.0f, 1.0f)
        : 0.0f;

    return FMath::Clamp(AcresScore * 0.5f + SeverityScore * 0.5f, 0.0f, 1.0f);
}
