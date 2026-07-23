#include "FireNiagaraRendererComponent.h"

#include "Engine/World.h"
#include "FireDataSubsystem.h"
#include "Materials/MaterialInterface.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "UObject/ConstructorHelpers.h"
#include "firesimulation2.h"

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

UFireNiagaraRendererComponent::UFireNiagaraRendererComponent()
{
	PrimaryComponentTick.bCanEverTick = false;

	// Default to the project's wildfire Niagara system so no manual assignment is required.
	static ConstructorHelpers::FObjectFinder<UNiagaraSystem> FireAsset(TEXT("/Game/FX/NS_WildFire"));
	if (FireAsset.Succeeded())
	{
		FireNiagaraSystem = FireAsset.Object;
	}
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void UFireNiagaraRendererComponent::InitializeRenderer(USceneComponent* InAttachRoot)
{
	AttachRoot = InAttachRoot;

	// Runtime fallback: if the constructor-time asset finder failed (editor reload, etc.),
	// try a synchronous load now.  This covers the first PIE session after the asset is created.
	if (!FireNiagaraSystem)
	{
		FireNiagaraSystem = Cast<UNiagaraSystem>(
			StaticLoadObject(UNiagaraSystem::StaticClass(), nullptr, TEXT("/Game/FX/NS_WildFire")));

		if (FireNiagaraSystem)
		{
			UE_LOG(LogFireSimulation2, Display,
				TEXT("FireNiagaraRenderer: loaded NS_WildFire via runtime fallback."));
		}
		else
		{
			UE_LOG(LogFireSimulation2, Warning,
				TEXT("FireNiagaraRenderer: NS_WildFire not found — fire Niagara effects will be skipped."));
		}
	}

	// Load additive fire override material so Niagara sprites use additive blending
	// and emit vibrant HDR orange glow visible from any viewing distance.
	FireOverrideMaterial = Cast<UMaterialInterface>(
		StaticLoadObject(UMaterialInterface::StaticClass(), nullptr, TEXT("/Game/FX/M_FireAdditive")));

	if (FireOverrideMaterial)
	{
		UE_LOG(LogFireSimulation2, Display,
			TEXT("FireNiagaraRenderer: loaded M_FireAdditive override material."));
	}
	else
	{
		UE_LOG(LogFireSimulation2, Warning,
			TEXT("FireNiagaraRenderer: M_FireAdditive not found — fire will use default Niagara material."));
	}
}

void UFireNiagaraRendererComponent::Configure(
	const int32  InMaxInstancesPerEvent,
	const int32  InMaxTotalInstances,
	const float  InMinAcresForNiagara,
	const float  InSampleSpacingKm,
	const float  InNiagaraScaleBase,
	const float  InActivationDistanceKm)
{
	MaxInstancesPerEvent   = FMath::Max(1,    InMaxInstancesPerEvent);
	MaxTotalInstances      = FMath::Max(1,    InMaxTotalInstances);
	MinAcresForNiagara     = FMath::Max(0.0f, InMinAcresForNiagara);
	SampleSpacingKm        = FMath::Max(0.1f, InSampleSpacingKm);
	NiagaraScaleBase       = FMath::Max(0.01f, InNiagaraScaleBase);
	ActivationDistanceKm   = FMath::Max(0.1f, InActivationDistanceKm);
}

// ---------------------------------------------------------------------------
// Pool management
// ---------------------------------------------------------------------------

UNiagaraComponent* UFireNiagaraRendererComponent::AcquireFromPool()
{
	if (!FireNiagaraSystem || !GetOwner())
	{
		return nullptr;
	}

	// Drain stale (garbage-collected) entries and look for a live one.
	UNiagaraComponent* Reused = nullptr;
	while (ComponentPool.Num() > 0)
	{
		TObjectPtr<UNiagaraComponent> Candidate = ComponentPool.Pop(EAllowShrinking::No);
		if (IsValid(Candidate.Get()))
		{
			Reused = Candidate.Get();
			break;
		}
	}

	if (!Reused)
	{
		// Pool is empty — allocate a new component.
		Reused = NewObject<UNiagaraComponent>(GetOwner());
		if (!Reused)
		{
			return nullptr;
		}

		USceneComponent* Parent = AttachRoot.Get()
			? AttachRoot.Get()
			: GetOwner()->GetRootComponent();

		Reused->SetupAttachment(Parent);
		Reused->RegisterComponent();
		Reused->SetAutoActivate(false);
		// bAutoDestroy is private in UE5 — looping fire systems won't auto-destroy;
		// for non-looping assets, the Deactivate() call on pool return prevents issues.
	}

	// (Re)apply the asset every time so pool components survive asset swaps in the editor.
	Reused->SetAsset(FireNiagaraSystem);

	// Override sprite renderer material slot 0 with the additive fire glow material.
	// UNiagaraComponent inherits UPrimitiveComponent::SetMaterial(), so slot 0 maps to
	// the first renderer's material — identical to how skeletal mesh overrides work.
	if (FireOverrideMaterial)
	{
		Reused->SetMaterial(0, FireOverrideMaterial);
	}

	ActiveComponents.Add(Reused);
	return Reused;
}

void UFireNiagaraRendererComponent::ReturnToPool(UNiagaraComponent* Component)
{
	if (!IsValid(Component))
	{
		return;
	}
	Component->Deactivate();
	Component->ResetSystem();
	ComponentPool.Add(Component);
}

// ---------------------------------------------------------------------------
// Render / Clear
// ---------------------------------------------------------------------------

void UFireNiagaraRendererComponent::ClearFireNiagara()
{
	for (TObjectPtr<UNiagaraComponent>& Comp : ActiveComponents)
	{
		ReturnToPool(Comp.Get());
	}
	ActiveComponents.Reset();
	LastRenderedInstanceCount = 0;
	LastRenderedEventCount    = 0;
}

void UFireNiagaraRendererComponent::RenderFireNiagara(
	const TArray<FFireEventWithGeometry>& Events,
	const UFireDataSubsystem*             DataSubsystem,
	const FVector&                        CameraLocation)
{
	ClearFireNiagara();

	UE_LOG(LogFireSimulation2, Display,
		TEXT("FireNiagaraRenderer: RenderFireNiagara called — events=%d system=%s material=%s"),
		Events.Num(),
		FireNiagaraSystem ? *FireNiagaraSystem->GetName() : TEXT("NULL"),
		FireOverrideMaterial ? *FireOverrideMaterial->GetName() : TEXT("NULL"));

	if (!FireNiagaraSystem || !DataSubsystem || !GetOwner())
	{
		UE_LOG(LogFireSimulation2, Warning,
			TEXT("FireNiagaraRenderer: SKIPPED — FireNiagaraSystem=%s DataSubsystem=%s Owner=%s"),
			FireNiagaraSystem ? TEXT("OK") : TEXT("NULL — NS_WildFire missing or failed to load"),
			DataSubsystem    ? TEXT("OK") : TEXT("NULL"),
			GetOwner()       ? TEXT("OK") : TEXT("NULL"));
		return;
	}

	UWorld* World = GetWorld();

	FCollisionQueryParams TraceParams(FName(TEXT("NiagaraFireTerrainSample")), /*bTraceComplex=*/false);
	TraceParams.AddIgnoredActor(GetOwner());

	// Convert the configurable kilometre spacing to degrees of latitude/longitude.
	// 1 degree of latitude ≈ 111.32 km everywhere; longitude degrees vary by cos(lat)
	// but across CONUS (~25–50 °N) the variation is small enough that the latitude
	// approximation gives adequate grid uniformity for visual purposes.
	constexpr float KmPerDegree = 111.32f;
	const float SpacingDegrees = SampleSpacingKm / KmPerDegree;

	int32 TotalInstances  = 0;
	int32 RenderedEvents  = 0;

	for (const FFireEventWithGeometry& Event : Events)
	{
		if (TotalInstances >= MaxTotalInstances)
		{
			break;
		}

		// ---- Eligibility checks ----------------------------------------

		if (Event.Attributes.Acres < MinAcresForNiagara)
		{
			continue;
		}
		if (Event.Geometry.Rings.Num() == 0)
		{
			continue;
		}

		// Camera-distance gate: skip events whose attribute centroid is too far away.
		// This early-out avoids sampling thousands of interior points for off-screen fires.
		const FVector EventEllipsoid = DataSubsystem->LonLatToLocalWorld(
			Event.Attributes.Longitude, Event.Attributes.Latitude, 0.0);
		const float DistanceKm =
			static_cast<float>(FVector::Dist(EventEllipsoid, CameraLocation)) / 100000.0f;

		if (DistanceKm > ActivationDistanceKm)
		{
			continue;
		}

		// ---- Severity-driven parameters --------------------------------

		const float Intensity = ComputeFireIntensity(Event.Attributes);

		// Spawn rate: 10 particles/s for small fires → 100 for the largest.
		const float SpawnRate = 10.0f + Intensity * 90.0f;

		// Scale: base × [0.5, 1.0] so intense fires look proportionally larger.
		const float ScaleValue = NiagaraScaleBase * (0.5f + Intensity * 0.5f);

		// Colour: orange (low) → deep red (high) interpolated in HSV space.
		const FLinearColor FlameColor = FLinearColor::LerpUsingHSV(
			FLinearColor(1.0f, 0.55f, 0.0f, 1.0f),   // orange
			FLinearColor(1.0f, 0.08f, 0.02f, 1.0f),  // deep red
			Intensity);

		// ---- Interior sampling ------------------------------------------

		// Only the outer ring (index 0) defines the burn boundary.  Subsequent rings
		// are interior holes (unburned islands) — we intentionally skip them so fire
		// effects are never placed inside a void.
		const FFireRing& OuterRing = Event.Geometry.Rings[0];
		if (OuterRing.LonLatVertices.Num() < 3)
		{
			continue;
		}

		TArray<FVector2D> SamplePoints;
		SampleInteriorPoints(
			OuterRing.LonLatVertices,
			SpacingDegrees,
			MaxInstancesPerEvent,
			SamplePoints);

		// If the ring is too small for even one grid cell, fall back to the attribute
		// centroid so large-but-compact fires still get at least one Niagara instance.
		if (SamplePoints.Num() == 0)
		{
			SamplePoints.Add(FVector2D(
				static_cast<float>(Event.Attributes.Longitude),
				static_cast<float>(Event.Attributes.Latitude)));
		}

		// Severity-stratified placement: sort samples by ascending distance from the
		// ring centroid so that inner-zone points (which proxy high-severity burn cores
		// where dNBR values are highest) are placed first.  When MaxInstancesPerEvent
		// caps placement, the budget is spent on the most severe interior areas rather
		// than on fringe pixels at the perimeter.
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

		// ---- Place Niagara instances ------------------------------------

		bool bAnyPlacedForEvent = false;

		for (const FVector2D& LonLat : SamplePoints)
		{
			if (TotalInstances >= MaxTotalInstances)
			{
				break;
			}

			// Ground each sample point to the Cesium tile surface via world-Z trace.
			// We use world-Z (not LocalUp) for the same reason as the heatmap renderer:
			// LocalUp at off-reference CONUS locations has horizontal components that
			// would displace fire instances away from their geographic column.
			const FVector EllipsoidPos =
				DataSubsystem->LonLatToLocalWorld(LonLat.X, LonLat.Y, 0.0);

			FVector SpawnLocation(EllipsoidPos.X, EllipsoidPos.Y, EllipsoidPos.Z);

			if (World)
			{
				const FVector TraceStart(EllipsoidPos.X, EllipsoidPos.Y, EllipsoidPos.Z + 1000000.0f);
				const FVector TraceEnd  (EllipsoidPos.X, EllipsoidPos.Y, EllipsoidPos.Z - 1000000.0f);

				FHitResult Hit;
				if (World->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_Visibility, TraceParams))
				{
					// Place the fire emitter directly on the terrain surface with a tiny
					// clearance.  The Niagara system is scaled very large so particles will
					// rise far above the ground and be clearly visible when looking down
					// from pillar-top altitude.
					SpawnLocation = FVector(Hit.Location.X, Hit.Location.Y, Hit.Location.Z + 200.0f);
				}
				// Tile not loaded yet — fall back to the ellipsoid surface position.
				// This may place emitters slightly underground on elevated terrain, but
				// NS_Flame_4 particles emit upward so they still appear above ground.
				// The auto-refresh timers triggered by MovePlayerToPillarTop will
				// re-run this function once tiles have streamed in and snap emitters
				// to exact terrain height.
				else
				{
					SpawnLocation = FVector(EllipsoidPos.X, EllipsoidPos.Y, EllipsoidPos.Z + 200.0f);
				}
			}

			// Acquire a pooled (or freshly created) Niagara component.
			UNiagaraComponent* Comp = AcquireFromPool();
			if (!Comp)
			{
				// Pool allocation failed (system unset mid-render, or OOM).
				break;
			}

			// Position the emitter on the terrain.
			Comp->SetWorldLocation(SpawnLocation);
			Comp->SetWorldRotation(FRotator::ZeroRotator);

			// Scale the component in world space so particles are physically large enough
			// to be visible from the camera distances used in fire-focus mode.
			// NiagaraScaleBase is treated as a dimensionless multiplier (1.0 = default size).
			const float WorldScaleFactor = FMath::Max(0.01f, ScaleValue);
			Comp->SetRelativeScale3D(FVector(WorldScaleFactor));

			// Also attempt to push values into Niagara user parameters if the asset exposes them.
			Comp->SetVariableFloat(SpawnRateParameterName, SpawnRate);
			Comp->SetVariableFloat(ScaleParameterName,     ScaleValue);
			Comp->SetVariableLinearColor(ColorParameterName, FlameColor);

			// bReset=true restarts looping emitters cleanly each time an instance is reused.
			Comp->Activate(/*bReset=*/true);

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
		TEXT("FireNiagaraRenderer: renderedEvents=%d instances=%d poolSize=%d"),
		LastRenderedEventCount, LastRenderedInstanceCount, ComponentPool.Num());
}

// ---------------------------------------------------------------------------
// Geometry helpers
// ---------------------------------------------------------------------------

bool UFireNiagaraRendererComponent::IsPointInRing(
	const FVector2D& Point, const TArray<FVector2D>& Ring)
{
	// Standard ray-casting algorithm.  Cast a horizontal ray along +X from Point and count
	// how many ring edges it crosses.  An odd count means the point is inside.
	bool bInside = false;
	const int32 N = Ring.Num();
	for (int32 i = 0, j = N - 1; i < N; j = i++)
	{
		const FVector2D& Vi = Ring[i];
		const FVector2D& Vj = Ring[j];

		// Does the edge [Vj, Vi] straddle the horizontal ray from Point?
		if (((Vi.Y > Point.Y) != (Vj.Y > Point.Y)) &&
		    (Point.X < (Vj.X - Vi.X) * (Point.Y - Vi.Y) / (Vj.Y - Vi.Y) + Vi.X))
		{
			bInside = !bInside;
		}
	}
	return bInside;
}

void UFireNiagaraRendererComponent::SampleInteriorPoints(
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

	// Compute the axis-aligned lon/lat bounding box of the ring.
	FVector2D MinBound(TNumericLimits<float>::Max(),  TNumericLimits<float>::Max());
	FVector2D MaxBound(-TNumericLimits<float>::Max(), -TNumericLimits<float>::Max());
	for (const FVector2D& V : Ring)
	{
		MinBound.X = FMath::Min(MinBound.X, V.X);
		MinBound.Y = FMath::Min(MinBound.Y, V.Y);
		MaxBound.X = FMath::Max(MaxBound.X, V.X);
		MaxBound.Y = FMath::Max(MaxBound.Y, V.Y);
	}

	// Reserve a rough upper bound to avoid repeated reallocations.
	const int32 ColsEstimate = FMath::CeilToInt((MaxBound.X - MinBound.X) / SpacingDegrees) + 1;
	const int32 RowsEstimate = FMath::CeilToInt((MaxBound.Y - MinBound.Y) / SpacingDegrees) + 1;
	OutPoints.Reserve(FMath::Min(MaxPoints, ColsEstimate * RowsEstimate));

	// Walk the grid in lat rows then lon columns.  The half-spacing offset centres each
	// sample cell so the first and last columns/rows aren't on the boundary itself.
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
// Severity helpers
// ---------------------------------------------------------------------------

float UFireNiagaraRendererComponent::ComputeFireIntensity(const FFireEventAttributes& Attributes)
{
	// Size component — log₁₀ acres: 100 ac → 0.0, 10 000 ac → 0.5, 1 000 000 ac → 1.0.
	const float AcresScore = FMath::Clamp(
		(static_cast<float>(FMath::LogX(10.0, FMath::Max(1.0, Attributes.Acres))) - 2.0f) / 4.0f,
		0.0f, 1.0f);

	// Severity component from MTBS raster-derived dNBR thresholds.
	//
	// HighThreshold is the dNBR value above which a pixel is classified "high severity."
	// A LOWER threshold means high-severity classification kicks in sooner → more pixels
	// qualify → the fire burned more intensely across a larger fraction of its area.
	// MTBS uses 9999 as a sentinel when no high-severity pixels were detected.
	//
	// Empirical range from MTBS data: ~100 (very intense) to ~660 (standard cutoff).
	// We invert and normalise so low-threshold fires score close to 1.0.
	const float HighT = static_cast<float>(Attributes.HighThreshold);
	const float SeverityScore = (HighT > 0.0f && HighT < 9000.0f)
		? FMath::Clamp(1.0f - (HighT - 100.0f) / 560.0f, 0.0f, 1.0f)
		: 0.0f;

	// Equal blend of size and severity: a small but intensely classified fire scores
	// higher than a vast but lightly classified one, and vice-versa.
	return FMath::Clamp(AcresScore * 0.5f + SeverityScore * 0.5f, 0.0f, 1.0f);
}
