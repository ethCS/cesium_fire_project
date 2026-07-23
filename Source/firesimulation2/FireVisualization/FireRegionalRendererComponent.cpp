#include "FireRegionalRendererComponent.h"

#include "Cesium3DTileset.h"
#include "CesiumSampleHeightResult.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FireDataSubsystem.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/PrimitiveComponent.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"
#include "Async/Async.h"
#include "firesimulation2.h"
#include "Algo/Reverse.h"
#include "HAL/IConsoleManager.h"
#include "UObject/UnrealType.h"

namespace FireRegionalDebug
{
	static TAutoConsoleVariable<int32> CVarHeatmapDebugProjection(
		TEXT("firesim.heatmap.debugprojection"),
		0,
		TEXT("Draws regional heatmap projection debug (geo points, rays, hits, final verts, local-up vectors). 0=off, 1=on."),
		ECVF_Default);

	static TAutoConsoleVariable<int32> CVarHeatmapDebugMaxDraws(
		TEXT("firesim.heatmap.debugprojection.maxdraws"),
		256,
		TEXT("Maximum number of projected vertices to visualize per regional rebuild."),
		ECVF_Default);

	// When > 0, log a geometry audit entry for every N-th Cesium-height vertex.
	// Set to 1 for every vertex (expensive), 50 for a representative sample.
	static TAutoConsoleVariable<int32> CVarHeatmapAuditRate(
		TEXT("firesim.heatmap.auditrate"),
		50,
		TEXT("Log a CesiumHeightAudit entry for every N-th vertex in the accurate pass. 0 = disabled."),
		ECVF_Default);
}

namespace FireRegionalTriangulation
{
	static double SignedArea2D(const TArray<FVector2D>& Points)
	{
		if (Points.Num() < 3)
		{
			return 0.0;
		}

		double Area = 0.0;
		for (int32 i = 0; i < Points.Num(); ++i)
		{
			const int32 Next = (i + 1) % Points.Num();
			Area += (static_cast<double>(Points[i].X) * static_cast<double>(Points[Next].Y)) - (static_cast<double>(Points[Next].X) * static_cast<double>(Points[i].Y));
		}
		return Area * 0.5;
	}

	static bool IsPointInsideTriangle(const FVector2D& P, const FVector2D& A, const FVector2D& B, const FVector2D& C)
	{
		const FVector2D V0 = C - A;
		const FVector2D V1 = B - A;
		const FVector2D V2 = P - A;

		const float Dot00 = FVector2D::DotProduct(V0, V0);
		const float Dot01 = FVector2D::DotProduct(V0, V1);
		const float Dot02 = FVector2D::DotProduct(V0, V2);
		const float Dot11 = FVector2D::DotProduct(V1, V1);
		const float Dot12 = FVector2D::DotProduct(V1, V2);

		const float Denom = (Dot00 * Dot11) - (Dot01 * Dot01);
		if (FMath::IsNearlyZero(Denom))
		{
			return false;
		}

		const float InvDenom = 1.0f / Denom;
		const float U = ((Dot11 * Dot02) - (Dot01 * Dot12)) * InvDenom;
		const float V = ((Dot00 * Dot12) - (Dot01 * Dot02)) * InvDenom;
		return U >= 0.0f && V >= 0.0f && (U + V) <= 1.0f;
	}

	static bool TriangulateSimplePolygon(const TArray<FVector2D>& InPoints, TArray<int32>& OutTriangles)
	{
		OutTriangles.Reset();
		if (InPoints.Num() < 3)
		{
			return false;
		}

		TArray<int32> Indices;
		Indices.Reserve(InPoints.Num());
		for (int32 i = 0; i < InPoints.Num(); ++i)
		{
			Indices.Add(i);
		}

		// Ensure consistent CCW winding for ear clipping.
		if (SignedArea2D(InPoints) < 0.0)
		{
			Algo::Reverse(Indices);
		}

		int32 Guard = 0;
		while (Indices.Num() > 3 && Guard < 8192)
		{
			++Guard;
			bool bCutEar = false;
			for (int32 i = 0; i < Indices.Num(); ++i)
			{
				const int32 Prev = Indices[(i - 1 + Indices.Num()) % Indices.Num()];
				const int32 Curr = Indices[i];
				const int32 Next = Indices[(i + 1) % Indices.Num()];

				const FVector2D& A = InPoints[Prev];
				const FVector2D& B = InPoints[Curr];
				const FVector2D& C = InPoints[Next];
				const float Cross = ((B.X - A.X) * (C.Y - A.Y)) - ((B.Y - A.Y) * (C.X - A.X));
				if (Cross <= KINDA_SMALL_NUMBER)
				{
					continue;
				}

				bool bAnyInside = false;
				for (int32 j = 0; j < Indices.Num(); ++j)
				{
					const int32 Test = Indices[j];
					if (Test == Prev || Test == Curr || Test == Next)
					{
						continue;
					}

					if (IsPointInsideTriangle(InPoints[Test], A, B, C))
					{
						bAnyInside = true;
						break;
					}
				}

				if (bAnyInside)
				{
					continue;
				}

				OutTriangles.Add(Prev);
				OutTriangles.Add(Curr);
				OutTriangles.Add(Next);
				Indices.RemoveAt(i);
				bCutEar = true;
				break;
			}

			if (!bCutEar)
			{
				break;
			}
		}

		if (Indices.Num() == 3)
		{
			OutTriangles.Add(Indices[0]);
			OutTriangles.Add(Indices[1]);
			OutTriangles.Add(Indices[2]);
		}

		return OutTriangles.Num() >= 3;
	}

	static bool IsPointInPolygon2D(const FVector2D& Point, const TArray<FVector2D>& Polygon)
	{
		bool bInside = false;
		const int32 N = Polygon.Num();
		if (N < 3)
		{
			return false;
		}

		for (int32 i = 0, j = N - 1; i < N; j = i++)
		{
			const FVector2D& A = Polygon[i];
			const FVector2D& B = Polygon[j];
			const bool bCrosses = ((A.Y > Point.Y) != (B.Y > Point.Y));
			if (bCrosses)
			{
				const float IntersectX = (B.X - A.X) * (Point.Y - A.Y) / (B.Y - A.Y) + A.X;
				if (Point.X < IntersectX)
				{
					bInside = !bInside;
				}
			}
		}
		return bInside;
	}
}

namespace
{
	uint32 ComputeRegionalDatasetHash(const TArray<FFireEventWithGeometry>& Events)
	{
		uint32 Hash = 2166136261u;
		auto Mix = [&Hash](const uint32 Value)
		{
			Hash ^= Value;
			Hash *= 16777619u;
		};
		for (const FFireEventWithGeometry& Event : Events)
		{
			const FTCHARToUTF8 EventIdUtf8(*Event.Attributes.EventId);
			const uint8* Bytes = reinterpret_cast<const uint8*>(EventIdUtf8.Get());
			for (int32 i = 0; i < EventIdUtf8.Length(); ++i)
			{
				Mix(static_cast<uint32>(Bytes[i]));
			}
			Mix(static_cast<uint32>(Event.Attributes.Year));
			Mix(static_cast<uint32>(Event.Geometry.Rings.Num()));
			for (const FFireRing& Ring : Event.Geometry.Rings)
			{
				Mix(static_cast<uint32>(Ring.LonLatVertices.Num()));
				if (Ring.LonLatVertices.Num() > 0)
				{
					const FVector2D& First = Ring.LonLatVertices[0];
					const FVector2D& Last = Ring.LonLatVertices.Last();
					Mix(static_cast<uint32>(FMath::RoundToInt(First.X * 10000.0f)));
					Mix(static_cast<uint32>(FMath::RoundToInt(First.Y * 10000.0f)));
					Mix(static_cast<uint32>(FMath::RoundToInt(Last.X * 10000.0f)));
					Mix(static_cast<uint32>(FMath::RoundToInt(Last.Y * 10000.0f)));
				}
			}
		}
		return Hash == 0u ? 1u : Hash;
	}
}

UFireRegionalRendererComponent::UFireRegionalRendererComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UFireRegionalRendererComponent::InitializeRenderer(USceneComponent* AttachParent)
{
	if (!GetOwner())
	{
		return;
	}

	if (!PerimeterMesh)
	{
		PerimeterMesh = NewObject<UProceduralMeshComponent>(GetOwner(), TEXT("RegionalPerimeterMesh"));
		if (PerimeterMesh)
		{
			PerimeterMesh->SetupAttachment(AttachParent ? AttachParent : GetOwner()->GetRootComponent());
			PerimeterMesh->RegisterComponent();
			PerimeterMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			PerimeterMesh->SetCastShadow(false);
			PerimeterMesh->bUseAsyncCooking = true;
			PerimeterMesh->SetVisibility(true);
			PerimeterMesh->SetHiddenInGame(false);
			PerimeterMesh->bNeverDistanceCull = true;
			PerimeterMesh->SetBoundsScale(50.0f);
			// Heatmap geometry covers all of CONUS — exclude from ray tracing to stay
			// within the RT geometry memory budget (would otherwise exceed 700 MiB).
			PerimeterMesh->bVisibleInRayTracing = false;
		}
	}

	if (!HeatmapMesh)
	{
		HeatmapMesh = NewObject<UProceduralMeshComponent>(GetOwner(), TEXT("RegionalHeatmapMesh"));
		if (HeatmapMesh)
		{
			HeatmapMesh->SetupAttachment(AttachParent ? AttachParent : GetOwner()->GetRootComponent());
			HeatmapMesh->RegisterComponent();
			HeatmapMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			HeatmapMesh->SetCastShadow(false);
			HeatmapMesh->bUseAsyncCooking = true;
			HeatmapMesh->SetVisibility(true);
			HeatmapMesh->SetHiddenInGame(false);
			HeatmapMesh->bNeverDistanceCull = true;
			HeatmapMesh->SetBoundsScale(50.0f);
			HeatmapMesh->bVisibleInRayTracing = false;
			HeatmapMesh->TranslucencySortPriority = 10;
		}
	}

	if (!PerimeterMaterial)
	{
		PerimeterMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineDebugMaterials/VertexColorMaterial.VertexColorMaterial"));
	}

	if (!HeatmapMaterial)
	{
		HeatmapMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineDebugMaterials/VertexColorMaterial.VertexColorMaterial"));
	}
}

void UFireRegionalRendererComponent::Configure(const float InPerimeterBaseOffsetCm, const float InHeatmapOffsetCm, const int32 InMaxRenderedEvents, const double InMinAcresForRegional, const float InPerimeterExaggerationScale)
{
	PerimeterBaseOffsetCm = FMath::Max(InPerimeterBaseOffsetCm, 50.0f);
	// Heatmap should cling to terrain, not float above it. Keep a small but non-trivial
	// clearance to avoid Z-fighting/flicker against terrain.
	HeatmapOffsetCm = FMath::Clamp(InHeatmapOffsetCm, 120.0f, 600.0f);
	MaxRenderedEvents = FMath::Max(1, InMaxRenderedEvents);
	MinAcresForRegional = FMath::Max(0.0, InMinAcresForRegional);
	PerimeterExaggerationScale = FMath::Max(1.0f, InPerimeterExaggerationScale);
}

void UFireRegionalRendererComponent::RenderRegionalView(const TArray<FFireEventWithGeometry>& Events, const UFireDataSubsystem* DataSubsystem, const bool bEllipsoidOnly)
{
	const double BuildStart = FPlatformTime::Seconds();
	++Metrics.RebuildCount;
	Metrics.TerrainSamplePoints = 0;
	Metrics.TerrainTraceCalls = 0;
	Metrics.TerrainTraceSuccesses = 0;
	Metrics.TerrainTraceMisses = 0;
	Metrics.TerrainFallbackVertices = 0;
	Metrics.CesiumBatchRequests = 0;
	Metrics.CesiumBatchCompleted = 0;
	Metrics.CesiumBatchFailed = 0;
	Metrics.CesiumRequestedVertices = 0;
	Metrics.CesiumReturnedResults = 0;
	Metrics.CesiumFailedResults = 0;
	Metrics.HeatmapTriangleCount = 0;
	Metrics.PerimeterTriangleCount = 0;
	Metrics.LastTriangulationMs = 0.0;
	Metrics.LastTerrainProjectionMs = 0.0;
	if (const UWorld* World = GetWorld())
	{
		if (const APlayerController* PC = World->GetFirstPlayerController())
		{
			if (const APlayerCameraManager* PCM = PC->PlayerCameraManager)
			{
				Metrics.LastCameraLocation = PCM->GetCameraLocation();
			}
		}
	}

	ClearRegionalView(false);
	if (!DataSubsystem || !PerimeterMesh || !HeatmapMesh)
	{
		return;
	}

	// Sort largest-first so big fire scars sit on the lowest Z layer.
	// Smaller fires (including those nested inside larger ones) float on top, which
	// matches real-world burn-scar layering and avoids the most common overlap case.
	TArray<FFireEventWithGeometry> SortedEvents = Events;
	SortedEvents.Sort([](const FFireEventWithGeometry& A, const FFireEventWithGeometry& B)
	{
		return A.Attributes.Acres > B.Attributes.Acres;
	});

	LastSourceEventCount = SortedEvents.Num();
	LastEventsWithAnyRings = 0;
	LastEventsWithRenderableRings = 0;
	LastTotalRings = 0;
	LastRenderableRings = 0;
	LastRenderedEventCount = 0;
	LastRenderedRingCount = 0;
	LastPerimeterSectionCount = 0;
	LastHeatmapSectionCount = 0;
	LastPerimeterVertexCount = 0;
	LastHeatmapVertexCount = 0;

	int32 RenderedEventCount = 0;
	int32 SectionIndex = 0;

	for (const FFireEventWithGeometry& Event : SortedEvents)
	{
		const int32 RingCount = Event.Geometry.Rings.Num();
		if (RingCount > 0)
		{
			++LastEventsWithAnyRings;
			LastTotalRings += RingCount;
		}

		int32 RenderableRingsForEvent = 0;
		for (const FFireRing& Ring : Event.Geometry.Rings)
		{
			if (Ring.LonLatVertices.Num() >= 3)
			{
				++RenderableRingsForEvent;
			}
		}
		if (RenderableRingsForEvent > 0)
		{
			++LastEventsWithRenderableRings;
			LastRenderableRings += RenderableRingsForEvent;
		}

		if (RenderedEventCount >= MaxRenderedEvents || Event.Attributes.Acres < MinAcresForRegional || Event.Geometry.Rings.Num() == 0)
		{
			continue;
		}

		// Each rendered event gets a unique palette hue so overlapping fires are
		// immediately distinguishable. Alpha stays severity-driven: larger fires
		// are more opaque, smaller ones more transparent.
		const FLinearColor PaletteColor = GetPaletteColor(RenderedEventCount);
		const float HeatAlpha = ComputeHeatAlpha(Event.Attributes);

		// Stagger Z clearance by 1 m per rendered rank, cycling every 50 events.
		// This prevents Z-fighting between polygons that share the same terrain cell
		// without pushing any layer more than 50 m above the terrain surface.
		const float LayerOffsetCm = static_cast<float>(RenderedEventCount % 50) * 100.0f;
		const float PerimeterClearance = PerimeterBaseOffsetCm + LayerOffsetCm;
		const float HeatmapClearance   = HeatmapOffsetCm + LayerOffsetCm;

		for (const FFireRing& Ring : Event.Geometry.Rings)
		{
			if (Ring.LonLatVertices.Num() < 3)
			{
				continue;
			}

			TArray<FVector> PerimeterVertices;
			TArray<int32> PerimeterTriangles;
			TArray<FVector> PerimeterNormals;
			TArray<FVector2D> PerimeterUV0;
			TArray<FLinearColor> PerimeterVertexColors;
			TArray<FProcMeshTangent> PerimeterTangents;
			BuildRingMeshData(
				Ring, DataSubsystem, GetWorld(), PerimeterClearance,
				FLinearColor(PaletteColor.R, PaletteColor.G, PaletteColor.B, 0.90f),
				PerimeterVertices, PerimeterTriangles, PerimeterNormals,
				PerimeterUV0, PerimeterVertexColors, PerimeterTangents);

			if (PerimeterVertices.Num() >= 3 && PerimeterTriangles.Num() >= 3)
			{
				PerimeterMesh->CreateMeshSection_LinearColor(
					SectionIndex, PerimeterVertices, PerimeterTriangles,
					PerimeterNormals, PerimeterUV0, PerimeterVertexColors, PerimeterTangents, false);
				++Metrics.CreateMeshSectionCalls;
				Metrics.PerimeterTriangleCount += (PerimeterTriangles.Num() / 3);
				if (PerimeterMaterial)
				{
					PerimeterMesh->SetMaterial(SectionIndex, PerimeterMaterial);
				}
				++LastPerimeterSectionCount;
				LastPerimeterVertexCount += PerimeterVertices.Num();
			}

			TArray<FVector> HeatVertices;
			TArray<int32> HeatTriangles;
			TArray<FVector> HeatNormals;
			TArray<FVector2D> HeatUV0;
			TArray<FLinearColor> HeatVertexColors;
			TArray<FProcMeshTangent> HeatTangents;
			const bool bBuiltTerrainHeatmap = BuildTerrainProjectedHeatmapMeshData(
				Ring, DataSubsystem, GetWorld(), HeatmapClearance,
				HeatAlpha,
				HeatVertices, HeatTriangles, HeatNormals,
					HeatUV0, HeatVertexColors, HeatTangents,
					bEllipsoidOnly);

			if (HeatVertices.Num() >= 3 && HeatTriangles.Num() >= 3)
			{
				HeatmapMesh->CreateMeshSection_LinearColor(
					SectionIndex, HeatVertices, HeatTriangles,
					HeatNormals, HeatUV0, HeatVertexColors, HeatTangents, false);
				++Metrics.CreateMeshSectionCalls;
				Metrics.HeatmapTriangleCount += (HeatTriangles.Num() / 3);
				if (HeatmapMaterial)
				{
					HeatmapMesh->SetMaterial(SectionIndex, HeatmapMaterial);
				}
				++LastHeatmapSectionCount;
				LastHeatmapVertexCount += HeatVertices.Num();
			}
			else if (!bBuiltTerrainHeatmap)
			{
				UE_LOG(LogFireSimulation2, Warning, TEXT("RegionalRenderer: terrain heatmap build failed for one ring; skipping planar fallback."));
			}

			++LastRenderedRingCount;
			++SectionIndex;
		}

		++RenderedEventCount;
	}
	LastRenderedEventCount = RenderedEventCount;
	Metrics.LastBuildMs = (FPlatformTime::Seconds() - BuildStart) * 1000.0;
	LogRegionalRenderStats();
}

void UFireRegionalRendererComponent::ClearRegionalView(const bool bResetDatasetCache)
{
	++Metrics.ClearAllMeshSectionsCalls;
	if (PerimeterMesh)
	{
		PerimeterMesh->ClearAllMeshSections();
	}
	if (HeatmapMesh)
	{
		HeatmapMesh->ClearAllMeshSections();
	}
	if (bResetDatasetCache)
	{
		bHasValidCachedDataset = false;
		LastDatasetHash = 0;
		LastDatasetEventCount = -1;
	}
}

void UFireRegionalRendererComponent::SetHeatmapVisible(bool bVisible)
{
	if (HeatmapMesh)
	{
		HeatmapMesh->SetVisibility(bVisible);
		HeatmapMesh->SetHiddenInGame(!bVisible);
	}
	if (PerimeterMesh)
	{
		PerimeterMesh->SetVisibility(bVisible);
		PerimeterMesh->SetHiddenInGame(!bVisible);
	}
}

void UFireRegionalRendererComponent::LogRegionalRenderStats() const
{
	const FBoxSphereBounds PerimeterBounds = PerimeterMesh ? PerimeterMesh->Bounds : FBoxSphereBounds(ForceInit);
	const FBoxSphereBounds HeatBounds = HeatmapMesh ? HeatmapMesh->Bounds : FBoxSphereBounds(ForceInit);
	UE_LOG(
		LogFireSimulation2,
		Display,
		TEXT("RegionalRenderer: sourceEvents=%d eventsWithRings=%d eventsWithRenderableRings=%d totalRings=%d renderableRings=%d renderedEvents=%d renderedRings=%d perimeterSections=%d heatmapSections=%d perimeterVerts=%d heatmapVerts=%d perimeterTris=%lld heatmapTris=%lld perimeterVisible=%d heatmapVisible=%d perimeterBoundsOrigin=%s perimeterBoundsExtent=%s heatBoundsOrigin=%s heatBoundsExtent=%s datasetHash=%u rebuildCount=%lld buildMs=%.2f triMs=%.2f terrainMs=%.2f createCalls=%lld clearCalls=%lld samples=%lld traceCalls=%lld traceSuccess=%lld traceMisses=%lld fallbackVerts=%lld cesiumBatches=%lld/%lld cesiumBatchFailures=%lld cesiumRequestedVerts=%lld cesiumResults=%lld cesiumResultFailures=%lld camera=%s"),
		LastSourceEventCount,
		LastEventsWithAnyRings,
		LastEventsWithRenderableRings,
		LastTotalRings,
		LastRenderableRings,
		LastRenderedEventCount,
		LastRenderedRingCount,
		LastPerimeterSectionCount,
		LastHeatmapSectionCount,
		LastPerimeterVertexCount,
		LastHeatmapVertexCount,
		Metrics.PerimeterTriangleCount,
		Metrics.HeatmapTriangleCount,
		PerimeterMesh ? (PerimeterMesh->IsVisible() ? 1 : 0) : 0,
		HeatmapMesh ? (HeatmapMesh->IsVisible() ? 1 : 0) : 0,
		*PerimeterBounds.Origin.ToString(),
		*PerimeterBounds.BoxExtent.ToString(),
		*HeatBounds.Origin.ToString(),
		*HeatBounds.BoxExtent.ToString(),
		LastDatasetHash,
		Metrics.RebuildCount,
		Metrics.LastBuildMs,
		Metrics.LastTriangulationMs,
		Metrics.LastTerrainProjectionMs,
		Metrics.CreateMeshSectionCalls,
		Metrics.ClearAllMeshSectionsCalls,
		Metrics.TerrainSamplePoints,
		Metrics.TerrainTraceCalls,
		Metrics.TerrainTraceSuccesses,
		Metrics.TerrainTraceMisses,
		Metrics.TerrainFallbackVertices,
		Metrics.CesiumBatchCompleted,
		Metrics.CesiumBatchRequests,
		Metrics.CesiumBatchFailed,
		Metrics.CesiumRequestedVertices,
		Metrics.CesiumReturnedResults,
		Metrics.CesiumFailedResults,
		*Metrics.LastCameraLocation.ToString());
}

void UFireRegionalRendererComponent::DrawPerimeterDebug(const TArray<FFireEventWithGeometry>& Events, const UFireDataSubsystem* DataSubsystem, const float DurationSeconds, const float SphereRadiusCm) const
{
	if (!DataSubsystem || !GetWorld())
	{
		return;
	}

	int32 DrawnRings = 0;
	int32 DrawnVertices = 0;
	for (const FFireEventWithGeometry& Event : Events)
	{
		for (const FFireRing& Ring : Event.Geometry.Rings)
		{
			const int32 VertexCount = Ring.LonLatVertices.Num();
			if (VertexCount < 2)
			{
				continue;
			}

			TArray<FVector> VerticesWorld;
			VerticesWorld.Reserve(VertexCount);
			for (const FVector2D& LonLat : Ring.LonLatVertices)
			{
				const FVector Ground = DataSubsystem->LonLatToLocalWorld(LonLat.X, LonLat.Y, 0.0);
				const FVector UpDir = Ground.GetSafeNormal();
				const FVector DebugPos = Ground + (UpDir * PerimeterBaseOffsetCm);
				VerticesWorld.Add(DebugPos);
				DrawDebugSphere(GetWorld(), DebugPos, SphereRadiusCm, 8, FColor::Green, false, DurationSeconds, 0, 200.0f);
				++DrawnVertices;
			}

			for (int32 i = 0; i < VerticesWorld.Num(); ++i)
			{
				const int32 NextIndex = (i + 1) % VerticesWorld.Num();
				DrawDebugLine(GetWorld(), VerticesWorld[i], VerticesWorld[NextIndex], FColor::Red, false, DurationSeconds, 0, 300.0f);
			}
			++DrawnRings;
		}
	}

	UE_LOG(LogFireSimulation2, Display, TEXT("RegionalRenderer DebugDraw: rings=%d vertices=%d duration=%.1fs sphereRadius=%.1fcm"), DrawnRings, DrawnVertices, DurationSeconds, SphereRadiusCm);
}

FLinearColor UFireRegionalRendererComponent::GetPaletteColor(const int32 RenderedRank)
{
	// ColorBrewer Set1 (9-class) — designed for maximum visual distinction on map backgrounds.
	// All colors are fully saturated and legible against satellite terrain imagery.
	static const FLinearColor Palette[] =
	{
		FLinearColor(0.894f, 0.102f, 0.110f),  // 0 Red
		FLinearColor(0.216f, 0.494f, 0.722f),  // 1 Blue
		FLinearColor(0.302f, 0.686f, 0.290f),  // 2 Green
		FLinearColor(0.596f, 0.306f, 0.639f),  // 3 Purple
		FLinearColor(1.000f, 0.498f, 0.000f),  // 4 Orange
		FLinearColor(0.651f, 0.337f, 0.157f),  // 5 Brown
		FLinearColor(0.969f, 0.506f, 0.749f),  // 6 Pink
		FLinearColor(0.400f, 0.761f, 0.647f),  // 7 Teal
		FLinearColor(0.902f, 0.670f, 0.008f),  // 8 Gold
	};
	constexpr int32 PaletteSize = UE_ARRAY_COUNT(Palette);
	return Palette[FMath::Abs(RenderedRank) % PaletteSize];
}

float UFireRegionalRendererComponent::ComputeHeatAlpha(const FFireEventAttributes& Attributes) const
{
	const float AcresLog = static_cast<float>(FMath::LogX(10.0, FMath::Max(1.0, Attributes.Acres)));
	return FMath::Clamp((AcresLog - 1.5f) / 4.0f, 0.25f, 0.95f);
}

bool UFireRegionalRendererComponent::BuildTerrainProjectedHeatmapMeshData(
	const FFireRing& Ring,
	const UFireDataSubsystem* DataSubsystem,
	UWorld* InWorld,
	const float TerrainClearanceCm,
	const float HeatAlpha,
	TArray<FVector>& OutVertices,
	TArray<int32>& OutTriangles,
	TArray<FVector>& OutNormals,
	TArray<FVector2D>& OutUV0,
	TArray<FLinearColor>& OutVertexColors,
	TArray<FProcMeshTangent>& OutTangents,
	const bool bEllipsoidOnly) const
{
	const double TerrainStart = FPlatformTime::Seconds();
	OutVertices.Reset();
	OutTriangles.Reset();
	OutNormals.Reset();
	OutUV0.Reset();
	OutVertexColors.Reset();
	OutTangents.Reset();

	const int32 VertexCount = Ring.LonLatVertices.Num();
	if (!DataSubsystem || VertexCount < 3)
	{
		return false;
	}

	TArray<FVector2D> BoundaryLonLat;
	BoundaryLonLat.Reserve(VertexCount);
	for (const FVector2D& LonLat : Ring.LonLatVertices)
	{
		BoundaryLonLat.Add(LonLat);
	}

	if (BoundaryLonLat.Num() >= 4 && BoundaryLonLat[0].Equals(BoundaryLonLat.Last(), 0.000001f))
	{
		BoundaryLonLat.Pop();
	}
	if (BoundaryLonLat.Num() < 3)
	{
		return false;
	}

	FVector2D MinBound(TNumericLimits<float>::Max(), TNumericLimits<float>::Max());
	FVector2D MaxBound(-TNumericLimits<float>::Max(), -TNumericLimits<float>::Max());
	for (const FVector2D& P : BoundaryLonLat)
	{
		MinBound.X = FMath::Min(MinBound.X, P.X);
		MinBound.Y = FMath::Min(MinBound.Y, P.Y);
		MaxBound.X = FMath::Max(MaxBound.X, P.X);
		MaxBound.Y = FMath::Max(MaxBound.Y, P.Y);
	}

	const float WidthDegrees = FMath::Max(0.00001f, MaxBound.X - MinBound.X);
	const float HeightDegrees = FMath::Max(0.00001f, MaxBound.Y - MinBound.Y);

	// ---- Centroid for per-vertex thermal gradient ----
	FVector2D Centroid2D(0.0f, 0.0f);
	for (const FVector2D& P : BoundaryLonLat)
		Centroid2D += P;
	Centroid2D /= static_cast<float>(BoundaryLonLat.Num());

	// Lon distance is compressed by cos(lat); use this scale for isotropic gradient distances.
	const float CosLatScale = FMath::Cos(FMath::DegreesToRadians(Centroid2D.Y));
	float MaxBoundaryDistLonLat = 0.0001f;
	for (const FVector2D& P : BoundaryLonLat)
	{
		const float dX = (P.X - Centroid2D.X) * CosLatScale;
		const float dY = P.Y - Centroid2D.Y;
		MaxBoundaryDistLonLat = FMath::Max(MaxBoundaryDistLonLat, FMath::Sqrt(dX * dX + dY * dY));
	}

	// Gradient colors: bright yellow-orange center → dark red at boundary edge
	static const FLinearColor HotColor (1.0f, 0.65f, 0.00f, 1.0f);
	static const FLinearColor WarmColor(0.88f, 0.15f, 0.00f, 1.0f);
	static const FLinearColor CoolColor(0.40f, 0.00f, 0.00f, 1.0f);

	auto ComputeHeatVertexColor = [&](float Lon, float Lat) -> FLinearColor
	{
		const float dX = (Lon - Centroid2D.X) * CosLatScale;
		const float dY = Lat - Centroid2D.Y;
		const float DistFrac = FMath::Clamp(FMath::Sqrt(dX * dX + dY * dY) / MaxBoundaryDistLonLat, 0.0f, 1.0f);
		FLinearColor GradColor;
		if (DistFrac < 0.5f)
			GradColor = FMath::Lerp(HotColor, WarmColor, DistFrac * 2.0f);
		else
			GradColor = FMath::Lerp(WarmColor, CoolColor, (DistFrac - 0.5f) * 2.0f);
		// Liquid-like alpha: full opacity at center, fades to ~0 at the boundary ring
		const float EdgeFade = 1.0f - FMath::SmoothStep(0.5f, 1.0f, DistFrac);
		const float FinalAlpha = FMath::Max(HeatAlpha * EdgeFade, HeatAlpha * 0.18f);
		return FLinearColor(GradColor.R, GradColor.G, GradColor.B, FinalAlpha);
	};
	// ---- End centroid gradient setup ----

	const float BaseGridStepDegrees = FMath::Max(0.0001f, HeatmapGridMinStepCm / 11132000.0f);
	float GridStepDegrees = BaseGridStepDegrees;

	int32 GridCols = FMath::Max(2, FMath::CeilToInt(WidthDegrees / GridStepDegrees) + 1);
	int32 GridRows = FMath::Max(2, FMath::CeilToInt(HeightDegrees / GridStepDegrees) + 1);
	const int32 MaxGridVertices = FMath::Max(128, HeatmapMaxGridVerticesPerRing);
	if (GridCols * GridRows > MaxGridVertices)
	{
		const float RequiredStep = FMath::Sqrt((WidthDegrees * HeightDegrees) / static_cast<float>(MaxGridVertices));
		GridStepDegrees = FMath::Max(GridStepDegrees, RequiredStep);
		GridCols = FMath::Max(2, FMath::CeilToInt(WidthDegrees / GridStepDegrees) + 1);
		GridRows = FMath::Max(2, FMath::CeilToInt(HeightDegrees / GridStepDegrees) + 1);
	}

	FCollisionQueryParams TraceParams(FName(TEXT("FireHeatmapTerrainSample")), false);
	if (GetOwner())
	{
		TraceParams.AddIgnoredActor(GetOwner());
	}
	const float ContactOffsetCm = FMath::Clamp(TerrainClearanceCm, 120.0f, 600.0f);
	FCollisionObjectQueryParams ObjectQueryParams;
	ObjectQueryParams.AddObjectTypesToQuery(ECC_WorldStatic);
	ObjectQueryParams.AddObjectTypesToQuery(ECC_WorldDynamic);
	const bool bDebugProjection = FireRegionalDebug::CVarHeatmapDebugProjection.GetValueOnAnyThread() > 0;
	const int32 MaxDebugDraws = FMath::Max(0, FireRegionalDebug::CVarHeatmapDebugMaxDraws.GetValueOnAnyThread());
	int32 DebugDrawCount = 0;
	bool bLoggedVertexAudit = false;
	const float DebugDrawSeconds = 20.0f;
	auto ProjectLonLatToTerrain = [&](const float Lon, const float Lat, bool& bHasLastTerrainHit, FVector& LastTerrainHit, FVector& LastTerrainNormal, FVector& OutVertex, FVector& OutNormal)
	{
		++Metrics.TerrainSamplePoints;
		const FVector EllipsoidPt = DataSubsystem->LonLatToLocalWorld(Lon, Lat, 0.0);
		const FVector LocalUp = EllipsoidPt.GetSafeNormal().IsNearlyZero() ? FVector::UpVector : EllipsoidPt.GetSafeNormal();
		OutVertex = EllipsoidPt + LocalUp * ContactOffsetCm;
		OutNormal = LocalUp;

		// Ellipsoid-only path: skip all physics traces (used for fast placeholder renders)
		if (bEllipsoidOnly || !InWorld)
		{
			return;
		}

		FHitResult Hit;
		const FVector LocalTraceStart = EllipsoidPt + LocalUp * 4000000.0f;
		const FVector LocalTraceEnd = EllipsoidPt - LocalUp * 4000000.0f;

		if (bDebugProjection && InWorld && DebugDrawCount < MaxDebugDraws)
		{
			DrawDebugPoint(InWorld, EllipsoidPt, 12.0f, FColor::Cyan, false, DebugDrawSeconds, 0);
			DrawDebugDirectionalArrow(InWorld, EllipsoidPt, EllipsoidPt + LocalUp * 50000.0f, 1500.0f, FColor::Blue, false, DebugDrawSeconds, 0, 100.0f);
			DrawDebugLine(InWorld, LocalTraceStart, LocalTraceEnd, FColor::Yellow, false, DebugDrawSeconds, 0, 80.0f);
		}

		Metrics.TerrainTraceCalls += 3;
		const bool bHitLocal = InWorld->LineTraceSingleByObjectType(Hit, LocalTraceStart, LocalTraceEnd, ObjectQueryParams, TraceParams) ||
			InWorld->LineTraceSingleByChannel(Hit, LocalTraceStart, LocalTraceEnd, ECC_Visibility, TraceParams) ||
			InWorld->LineTraceSingleByChannel(Hit, LocalTraceStart, LocalTraceEnd, ECC_WorldStatic, TraceParams);
		if (bHitLocal)
		{
			++Metrics.TerrainTraceSuccesses;
			OutNormal = Hit.ImpactNormal.GetSafeNormal();
			if (OutNormal.IsNearlyZero())
			{
				OutNormal = LocalUp;
			}
			OutVertex = Hit.Location + OutNormal * ContactOffsetCm;
			bHasLastTerrainHit = true;
			LastTerrainHit = Hit.Location;
			LastTerrainNormal = OutNormal;

			if (bDebugProjection && InWorld && DebugDrawCount < MaxDebugDraws)
			{
				DrawDebugPoint(InWorld, Hit.Location, 16.0f, FColor::Green, false, DebugDrawSeconds, 0);
				DrawDebugPoint(InWorld, OutVertex, 14.0f, FColor::Red, false, DebugDrawSeconds, 0);
				++DebugDrawCount;
			}

			if (!bLoggedVertexAudit)
			{
				UE_LOG(
					LogFireSimulation2,
					Display,
					TEXT("RegionalRenderer VertexAudit: lon=%.8f lat=%.8f ellipsoid=%s localUp=%s traceStart=%s traceEnd=%s terrainHit=%s hitNormal=%s finalVertex=%s"),
					Lon,
					Lat,
					*EllipsoidPt.ToString(),
					*LocalUp.ToString(),
					*LocalTraceStart.ToString(),
					*LocalTraceEnd.ToString(),
					*Hit.Location.ToString(),
					*OutNormal.ToString(),
					*OutVertex.ToString());
				bLoggedVertexAudit = true;
			}
			return;
		}

		if (bHasLastTerrainHit)
		{
			OutNormal = LastTerrainNormal.IsNearlyZero() ? LocalUp : LastTerrainNormal;
			OutVertex = LastTerrainHit + OutNormal * ContactOffsetCm;
			++Metrics.TerrainFallbackVertices;
			if (bDebugProjection && InWorld && DebugDrawCount < MaxDebugDraws)
			{
				DrawDebugPoint(InWorld, OutVertex, 14.0f, FColor::Magenta, false, DebugDrawSeconds, 0);
				++DebugDrawCount;
			}
			return;
		}
		++Metrics.TerrainTraceMisses;
	};

	TMap<FIntPoint, int32> GridToVertexIndex;
	GridToVertexIndex.Reserve(GridCols * GridRows);
	OutVertices.Reserve(GridCols * GridRows);
	OutNormals.Reserve(GridCols * GridRows);
	OutUV0.Reserve(GridCols * GridRows);
	OutVertexColors.Reserve(GridCols * GridRows);
	OutTangents.Reserve(GridCols * GridRows);

	int32 InsideCount = 0;
	for (int32 Row = 0; Row < GridRows; ++Row)
	{
		const float Lat = MinBound.Y + static_cast<float>(Row) * GridStepDegrees;
		for (int32 Col = 0; Col < GridCols; ++Col)
		{
			const float Lon = MinBound.X + static_cast<float>(Col) * GridStepDegrees;
			const FVector2D Sample2D(Lon, Lat);
			if (!FireRegionalTriangulation::IsPointInPolygon2D(Sample2D, BoundaryLonLat))
			{
				continue;
			}
			++InsideCount;
		}
	}

	// If the grid step is too coarse for the ring's shape (thin/elongated polygons),
	// no interior samples land inside the boundary. Halve the step and retry once.
	if (InsideCount < 3 && GridStepDegrees > BaseGridStepDegrees * 0.5f)
	{
		GridStepDegrees *= 0.5f;
		GridCols = FMath::Max(2, FMath::CeilToInt(WidthDegrees / GridStepDegrees) + 1);
		GridRows = FMath::Max(2, FMath::CeilToInt(HeightDegrees / GridStepDegrees) + 1);
		InsideCount = 0;
		for (int32 Row = 0; Row < GridRows; ++Row)
		{
			const float Lat2 = MinBound.Y + static_cast<float>(Row) * GridStepDegrees;
			for (int32 Col = 0; Col < GridCols; ++Col)
			{
				const float Lon2 = MinBound.X + static_cast<float>(Col) * GridStepDegrees;
				if (FireRegionalTriangulation::IsPointInPolygon2D(FVector2D(Lon2, Lat2), BoundaryLonLat))
				{
					++InsideCount;
				}
			}
		}
	}

	// If still no interior samples, fall back to using the ring's own vertices as the
	// mesh grid — this always produces valid terrain-projected geometry.
	const bool bUseBoundaryFallback = (InsideCount < 3);

	if (bUseBoundaryFallback)
	{
		bool bHasLastTerrainHit = false;
		FVector LastTerrainHit = FVector::ZeroVector;
		FVector LastTerrainNormal = FVector::UpVector;
		for (int32 i = 0; i < BoundaryLonLat.Num(); ++i)
		{
			const float Lon = BoundaryLonLat[i].X;
			const float Lat = BoundaryLonLat[i].Y;
			FVector Vertex = FVector::ZeroVector;
			FVector Normal = FVector::UpVector;
			ProjectLonLatToTerrain(Lon, Lat, bHasLastTerrainHit, LastTerrainHit, LastTerrainNormal, Vertex, Normal);
			OutVertices.Add(Vertex);
			OutNormals.Add(Normal);
			OutUV0.Add(FVector2D(
				(WidthDegrees > KINDA_SMALL_NUMBER) ? (Lon - MinBound.X) / WidthDegrees : 0.0f,
				(HeightDegrees > KINDA_SMALL_NUMBER) ? (Lat - MinBound.Y) / HeightDegrees : 0.0f));
				OutVertexColors.Add(ComputeHeatVertexColor(Lon, Lat));
			OutTangents.Add(FProcMeshTangent(FVector::ForwardVector, false));
		}
		if (OutVertices.Num() >= 3)
		{
			TArray<FVector2D> Points2DFB;
			for (const FVector& V : OutVertices)
			{
				const FVector Rel = V - OutVertices[0];
				Points2DFB.Add(FVector2D(Rel.X, Rel.Y));
			}
			FireRegionalTriangulation::TriangulateSimplePolygon(Points2DFB, OutTriangles);
		}
		if (OutTriangles.Num() < 3) { return false; }
		Metrics.LastTriangulationMs += (FPlatformTime::Seconds() - TerrainStart) * 1000.0;
		const int32 FrontCountFB = OutTriangles.Num();
		const TArray<int32> FrontFB = OutTriangles;
		for (int32 i = 0; i < FrontCountFB; i += 3)
		{
			OutTriangles.Add(FrontFB[i + 2]);
			OutTriangles.Add(FrontFB[i + 1]);
			OutTriangles.Add(FrontFB[i]);
		}
		return true;
	}

	for (int32 Row = 0; Row < GridRows; ++Row)
	{
		bool bHasLastTerrainHit = false;
		FVector LastTerrainHit = FVector::ZeroVector;
		FVector LastTerrainNormal = FVector::UpVector;
		const float Lat = MinBound.Y + static_cast<float>(Row) * GridStepDegrees;
		for (int32 Col = 0; Col < GridCols; ++Col)
		{
			const float Lon = MinBound.X + static_cast<float>(Col) * GridStepDegrees;
			const FVector2D Sample2D(Lon, Lat);
			if (!FireRegionalTriangulation::IsPointInPolygon2D(Sample2D, BoundaryLonLat))
			{
				continue;
			}

			FVector Vertex = FVector::ZeroVector;
			FVector Normal = FVector::UpVector;
			ProjectLonLatToTerrain(Lon, Lat, bHasLastTerrainHit, LastTerrainHit, LastTerrainNormal, Vertex, Normal);

			const int32 VertexIndex = OutVertices.Num();
			GridToVertexIndex.Add(FIntPoint(Col, Row), VertexIndex);
			OutVertices.Add(Vertex);
			OutNormals.Add(Normal);
			OutUV0.Add(FVector2D(
				(WidthDegrees > KINDA_SMALL_NUMBER) ? (Lon - MinBound.X) / WidthDegrees : 0.0f,
				(HeightDegrees > KINDA_SMALL_NUMBER) ? (Lat - MinBound.Y) / HeightDegrees : 0.0f));
			OutVertexColors.Add(ComputeHeatVertexColor(Lon, Lat));
			OutTangents.Add(FProcMeshTangent(FVector::ForwardVector, false));
		}
	}

	if (OutVertices.Num() < 3)
	{
		return false;
	}

	for (int32 Row = 0; Row < GridRows - 1; ++Row)
	{
		for (int32 Col = 0; Col < GridCols - 1; ++Col)
		{
			const int32* A = GridToVertexIndex.Find(FIntPoint(Col, Row));
			const int32* B = GridToVertexIndex.Find(FIntPoint(Col + 1, Row));
			const int32* C = GridToVertexIndex.Find(FIntPoint(Col, Row + 1));
			const int32* D = GridToVertexIndex.Find(FIntPoint(Col + 1, Row + 1));

			if (A && C && B)
			{
				OutTriangles.Add(*A);
				OutTriangles.Add(*C);
				OutTriangles.Add(*B);
			}
			if (B && C && D)
			{
				OutTriangles.Add(*B);
				OutTriangles.Add(*C);
				OutTriangles.Add(*D);
			}
		}
	}

	if (OutTriangles.Num() < 3)
	{
		return false;
	}

	// Smooth normal averaging: accumulate face normals from front-face triangles so the
	// mesh shades as Phong (smooth) instead of flat-faceted.
	{
		TArray<FVector> AccumNormals;
		AccumNormals.SetNumZeroed(OutVertices.Num());
		for (int32 t = 0; t < OutTriangles.Num(); t += 3)
		{
			const int32 Ai = OutTriangles[t];
			const int32 Bi = OutTriangles[t + 1];
			const int32 Ci = OutTriangles[t + 2];
			if (Ai >= OutVertices.Num() || Bi >= OutVertices.Num() || Ci >= OutVertices.Num()) continue;
			const FVector FaceN = FVector::CrossProduct(
				OutVertices[Bi] - OutVertices[Ai],
				OutVertices[Ci] - OutVertices[Ai]);
			AccumNormals[Ai] += FaceN;
			AccumNormals[Bi] += FaceN;
			AccumNormals[Ci] += FaceN;
		}
		for (int32 i = 0; i < OutVertices.Num(); ++i)
		{
			if (!AccumNormals[i].IsNearlyZero())
				OutNormals[i] = AccumNormals[i].GetSafeNormal();
		}
	}

	const int32 FrontCount = OutTriangles.Num();
	const TArray<int32> FrontTriangles = OutTriangles;
	OutTriangles.Reserve(FrontCount * 2);
	for (int32 i = 0; i < FrontCount; i += 3)
	{
		OutTriangles.Add(FrontTriangles[i + 2]);
		OutTriangles.Add(FrontTriangles[i + 1]);
		OutTriangles.Add(FrontTriangles[i]);
	}

	Metrics.LastTerrainProjectionMs += (FPlatformTime::Seconds() - TerrainStart) * 1000.0;
	return true;
}

// ---------------------------------------------------------------------------
// Cesium height-based mesh building
// ---------------------------------------------------------------------------

bool UFireRegionalRendererComponent::CollectRingGridLonLat(
	const FFireRing& Ring,
	TArray<FVector2D>& OutVerticesLonLat,
	bool& bOutUsedBoundaryFallback) const
{
	OutVerticesLonLat.Reset();
	bOutUsedBoundaryFallback = false;

	const int32 VertexCount = Ring.LonLatVertices.Num();
	if (VertexCount < 3) return false;

	TArray<FVector2D> BoundaryLonLat;
	BoundaryLonLat.Reserve(VertexCount);
	for (const FVector2D& V : Ring.LonLatVertices) BoundaryLonLat.Add(V);
	if (BoundaryLonLat.Num() >= 4 && BoundaryLonLat[0].Equals(BoundaryLonLat.Last(), 0.000001f))
		BoundaryLonLat.Pop();
	if (BoundaryLonLat.Num() < 3) return false;

	FVector2D MinBound(TNumericLimits<float>::Max(), TNumericLimits<float>::Max());
	FVector2D MaxBound(-TNumericLimits<float>::Max(), -TNumericLimits<float>::Max());
	for (const FVector2D& P : BoundaryLonLat)
	{
		MinBound.X = FMath::Min(MinBound.X, P.X);
		MinBound.Y = FMath::Min(MinBound.Y, P.Y);
		MaxBound.X = FMath::Max(MaxBound.X, P.X);
		MaxBound.Y = FMath::Max(MaxBound.Y, P.Y);
	}
	const float WidthDegrees  = FMath::Max(0.00001f, MaxBound.X - MinBound.X);
	const float HeightDegrees = FMath::Max(0.00001f, MaxBound.Y - MinBound.Y);

	const float BaseGridStepDegrees = FMath::Max(0.0001f, HeatmapGridMinStepCm / 11132000.0f);
	float GridStepDegrees = BaseGridStepDegrees;
	int32 GridCols = FMath::Max(2, FMath::CeilToInt(WidthDegrees  / GridStepDegrees) + 1);
	int32 GridRows = FMath::Max(2, FMath::CeilToInt(HeightDegrees / GridStepDegrees) + 1);
	const int32 MaxGridVertices = FMath::Max(128, HeatmapMaxGridVerticesPerRing);
	if (GridCols * GridRows > MaxGridVertices)
	{
		const float RequiredStep = FMath::Sqrt((WidthDegrees * HeightDegrees) / static_cast<float>(MaxGridVertices));
		GridStepDegrees = FMath::Max(GridStepDegrees, RequiredStep);
		GridCols = FMath::Max(2, FMath::CeilToInt(WidthDegrees  / GridStepDegrees) + 1);
		GridRows = FMath::Max(2, FMath::CeilToInt(HeightDegrees / GridStepDegrees) + 1);
	}

	// Count interior points (mirrors BuildTerrainProjectedHeatmapMeshData exactly)
	int32 InsideCount = 0;
	for (int32 Row = 0; Row < GridRows; ++Row)
	{
		const float Lat = MinBound.Y + static_cast<float>(Row) * GridStepDegrees;
		for (int32 Col = 0; Col < GridCols; ++Col)
		{
			const float Lon = MinBound.X + static_cast<float>(Col) * GridStepDegrees;
			if (FireRegionalTriangulation::IsPointInPolygon2D(FVector2D(Lon, Lat), BoundaryLonLat))
				++InsideCount;
		}
	}
	if (InsideCount < 3 && GridStepDegrees > BaseGridStepDegrees * 0.5f)
	{
		GridStepDegrees *= 0.5f;
		GridCols = FMath::Max(2, FMath::CeilToInt(WidthDegrees  / GridStepDegrees) + 1);
		GridRows = FMath::Max(2, FMath::CeilToInt(HeightDegrees / GridStepDegrees) + 1);
		InsideCount = 0;
		for (int32 Row = 0; Row < GridRows; ++Row)
		{
			const float Lat = MinBound.Y + static_cast<float>(Row) * GridStepDegrees;
			for (int32 Col = 0; Col < GridCols; ++Col)
			{
				const float Lon = MinBound.X + static_cast<float>(Col) * GridStepDegrees;
				if (FireRegionalTriangulation::IsPointInPolygon2D(FVector2D(Lon, Lat), BoundaryLonLat))
					++InsideCount;
			}
		}
	}

	if (InsideCount < 3)
	{
		// Boundary fallback: use ring vertices directly
		bOutUsedBoundaryFallback = true;
		for (const FVector2D& V : BoundaryLonLat)
			OutVerticesLonLat.Add(V);
		return OutVerticesLonLat.Num() >= 3;
	}

	// Grid interior vertices in row-major order (same as BuildTerrainProjectedHeatmapMeshData)
	for (int32 Row = 0; Row < GridRows; ++Row)
	{
		const float Lat = MinBound.Y + static_cast<float>(Row) * GridStepDegrees;
		for (int32 Col = 0; Col < GridCols; ++Col)
		{
			const float Lon = MinBound.X + static_cast<float>(Col) * GridStepDegrees;
			if (FireRegionalTriangulation::IsPointInPolygon2D(FVector2D(Lon, Lat), BoundaryLonLat))
				OutVerticesLonLat.Add(FVector2D(Lon, Lat));
		}
	}
	return OutVerticesLonLat.Num() >= 3;
}

bool UFireRegionalRendererComponent::BuildHeatmapMeshFromCesiumHeights(
	const FFireRing& Ring,
	const UFireDataSubsystem* DataSubsystem,
	const float TerrainClearanceCm,
	const float HeatAlpha,
	const TArrayView<const float> CesiumHeightsM,
	const bool bUsedBoundaryFallback,
	TArray<FVector>& OutVertices,
	TArray<int32>& OutTriangles,
	TArray<FVector>& OutNormals,
	TArray<FVector2D>& OutUV0,
	TArray<FLinearColor>& OutVertexColors,
	TArray<FProcMeshTangent>& OutTangents) const
{
	OutVertices.Reset(); OutTriangles.Reset(); OutNormals.Reset();
	OutUV0.Reset(); OutVertexColors.Reset(); OutTangents.Reset();

	if (!DataSubsystem || CesiumHeightsM.Num() < 3) return false;

	const int32 VertexCount = Ring.LonLatVertices.Num();
	if (VertexCount < 3) return false;

	TArray<FVector2D> BoundaryLonLat;
	BoundaryLonLat.Reserve(VertexCount);
	for (const FVector2D& V : Ring.LonLatVertices) BoundaryLonLat.Add(V);
	if (BoundaryLonLat.Num() >= 4 && BoundaryLonLat[0].Equals(BoundaryLonLat.Last(), 0.000001f))
		BoundaryLonLat.Pop();
	if (BoundaryLonLat.Num() < 3) return false;

	FVector2D MinBound(TNumericLimits<float>::Max(), TNumericLimits<float>::Max());
	FVector2D MaxBound(-TNumericLimits<float>::Max(), -TNumericLimits<float>::Max());
	for (const FVector2D& P : BoundaryLonLat)
	{
		MinBound.X = FMath::Min(MinBound.X, P.X); MinBound.Y = FMath::Min(MinBound.Y, P.Y);
		MaxBound.X = FMath::Max(MaxBound.X, P.X); MaxBound.Y = FMath::Max(MaxBound.Y, P.Y);
	}
	const float WidthDegrees  = FMath::Max(0.00001f, MaxBound.X - MinBound.X);
	const float HeightDegrees = FMath::Max(0.00001f, MaxBound.Y - MinBound.Y);

	// Thermal gradient setup (mirrors BuildTerrainProjectedHeatmapMeshData)
	FVector2D Centroid2D(0.0f, 0.0f);
	for (const FVector2D& P : BoundaryLonLat) Centroid2D += P;
	Centroid2D /= static_cast<float>(BoundaryLonLat.Num());
	const float CosLatScale = FMath::Cos(FMath::DegreesToRadians(Centroid2D.Y));
	float MaxBoundaryDistLonLat = 0.0001f;
	for (const FVector2D& P : BoundaryLonLat)
	{
		const float dX = (P.X - Centroid2D.X) * CosLatScale;
		const float dY = P.Y - Centroid2D.Y;
		MaxBoundaryDistLonLat = FMath::Max(MaxBoundaryDistLonLat, FMath::Sqrt(dX * dX + dY * dY));
	}
	static const FLinearColor HotColor (1.0f, 0.65f, 0.00f, 1.0f);
	static const FLinearColor WarmColor(0.88f, 0.15f, 0.00f, 1.0f);
	static const FLinearColor CoolColor(0.40f, 0.00f, 0.00f, 1.0f);
	auto ComputeHeatVertexColor = [&](float Lon, float Lat) -> FLinearColor
	{
		const float dX = (Lon - Centroid2D.X) * CosLatScale;
		const float dY = Lat - Centroid2D.Y;
		const float DistFrac = FMath::Clamp(FMath::Sqrt(dX * dX + dY * dY) / MaxBoundaryDistLonLat, 0.0f, 1.0f);
		FLinearColor GradColor = (DistFrac < 0.5f)
			? FMath::Lerp(HotColor, WarmColor, DistFrac * 2.0f)
			: FMath::Lerp(WarmColor, CoolColor, (DistFrac - 0.5f) * 2.0f);
		const float EdgeFade  = 1.0f - FMath::SmoothStep(0.5f, 1.0f, DistFrac);
		const float FinalAlpha = FMath::Max(HeatAlpha * EdgeFade, HeatAlpha * 0.18f);
		return FLinearColor(GradColor.R, GradColor.G, GradColor.B, FinalAlpha);
	};

	const float ContactOffsetCm = FMath::Clamp(TerrainClearanceCm, 120.0f, 600.0f);

	if (bUsedBoundaryFallback)
	{
		// Boundary fallback: CesiumHeightsM maps 1:1 onto BoundaryLonLat
		const int32 Count = FMath::Min(CesiumHeightsM.Num(), BoundaryLonLat.Num());
		OutVertices.Reserve(Count); OutNormals.Reserve(Count);
		OutUV0.Reserve(Count); OutVertexColors.Reserve(Count); OutTangents.Reserve(Count);
		for (int32 i = 0; i < Count; ++i)
		{
			const float Lon = BoundaryLonLat[i].X;
			const float Lat = BoundaryLonLat[i].Y;
			const double HeightM = static_cast<double>(CesiumHeightsM[i]);
			const FVector EllipsoidPt = DataSubsystem->LonLatToLocalWorld(Lon, Lat, HeightM);
			const FVector LocalUp    = EllipsoidPt.GetSafeNormal().IsNearlyZero() ? FVector::UpVector : EllipsoidPt.GetSafeNormal();
			OutVertices.Add(EllipsoidPt + LocalUp * ContactOffsetCm);
			OutNormals.Add(LocalUp);
			OutUV0.Add(FVector2D(
				(WidthDegrees > KINDA_SMALL_NUMBER) ? (Lon - MinBound.X) / WidthDegrees : 0.0f,
				(HeightDegrees > KINDA_SMALL_NUMBER) ? (Lat - MinBound.Y) / HeightDegrees : 0.0f));
			OutVertexColors.Add(ComputeHeatVertexColor(Lon, Lat));
			OutTangents.Add(FProcMeshTangent(FVector::ForwardVector, false));
		}
		if (OutVertices.Num() < 3) return false;
		TArray<FVector2D> Points2DFB;
		for (const FVector& V : OutVertices)
		{
			const FVector Rel = V - OutVertices[0];
			Points2DFB.Add(FVector2D(Rel.X, Rel.Y));
		}
		FireRegionalTriangulation::TriangulateSimplePolygon(Points2DFB, OutTriangles);
		if (OutTriangles.Num() < 3) return false;
		const int32 FBFront = OutTriangles.Num();
		const TArray<int32> FBTriCopy = OutTriangles;
		for (int32 i = 0; i < FBFront; i += 3)
		{
			OutTriangles.Add(FBTriCopy[i + 2]);
			OutTriangles.Add(FBTriCopy[i + 1]);
			OutTriangles.Add(FBTriCopy[i]);
		}
		return true;
	}

	// Grid path: reconstruct the same grid used by CollectRingGridLonLat
	const float BaseGridStepDegrees = FMath::Max(0.0001f, HeatmapGridMinStepCm / 11132000.0f);
	float GridStepDegrees = BaseGridStepDegrees;
	int32 GridCols = FMath::Max(2, FMath::CeilToInt(WidthDegrees  / GridStepDegrees) + 1);
	int32 GridRows = FMath::Max(2, FMath::CeilToInt(HeightDegrees / GridStepDegrees) + 1);
	const int32 MaxGridVertices = FMath::Max(128, HeatmapMaxGridVerticesPerRing);
	if (GridCols * GridRows > MaxGridVertices)
	{
		const float RequiredStep = FMath::Sqrt((WidthDegrees * HeightDegrees) / static_cast<float>(MaxGridVertices));
		GridStepDegrees = FMath::Max(GridStepDegrees, RequiredStep);
		GridCols = FMath::Max(2, FMath::CeilToInt(WidthDegrees  / GridStepDegrees) + 1);
		GridRows = FMath::Max(2, FMath::CeilToInt(HeightDegrees / GridStepDegrees) + 1);
	}
	// Replicate the halve-step retry to ensure same grid as CollectRingGridLonLat
	{
		int32 InsideCount = 0;
		for (int32 Row = 0; Row < GridRows; ++Row)
		{
			const float Lat = MinBound.Y + static_cast<float>(Row) * GridStepDegrees;
			for (int32 Col = 0; Col < GridCols; ++Col)
			{
				const float Lon = MinBound.X + static_cast<float>(Col) * GridStepDegrees;
				if (FireRegionalTriangulation::IsPointInPolygon2D(FVector2D(Lon, Lat), BoundaryLonLat))
					++InsideCount;
			}
		}
		if (InsideCount < 3 && GridStepDegrees > BaseGridStepDegrees * 0.5f)
		{
			GridStepDegrees *= 0.5f;
			GridCols = FMath::Max(2, FMath::CeilToInt(WidthDegrees  / GridStepDegrees) + 1);
			GridRows = FMath::Max(2, FMath::CeilToInt(HeightDegrees / GridStepDegrees) + 1);
		}
	}

	// Build vertex array in the exact same row-major, polygon-filtered order
	TMap<FIntPoint, int32> GridToVertexIndex;
	int32 HeightIdx = 0;
	const bool bDebug = FireRegionalDebug::CVarHeatmapDebugProjection.GetValueOnAnyThread() > 0;
	const int32 MaxDebugDraws = FMath::Max(0, FireRegionalDebug::CVarHeatmapDebugMaxDraws.GetValueOnAnyThread());
	const int32 AuditRate = FireRegionalDebug::CVarHeatmapAuditRate.GetValueOnAnyThread();
	int32 DebugDrawCount = 0;
	const float DebugSecs = 25.0f;
	UWorld* DebugWorld = (bDebug) ? GetWorld() : nullptr;
	// Accumulate stats for this ring
	float MinHeightM = TNumericLimits<float>::Max();
	float MaxHeightM = -TNumericLimits<float>::Max();
	double SumHeightM = 0.0;
	int32 HeightOverrunCount = 0;

	for (int32 Row = 0; Row < GridRows; ++Row)
	{
		const float Lat = MinBound.Y + static_cast<float>(Row) * GridStepDegrees;
		for (int32 Col = 0; Col < GridCols; ++Col)
		{
			const float Lon = MinBound.X + static_cast<float>(Col) * GridStepDegrees;
			if (!FireRegionalTriangulation::IsPointInPolygon2D(FVector2D(Lon, Lat), BoundaryLonLat))
				continue;
			if (HeightIdx >= CesiumHeightsM.Num())
			{
				// Ran out of height data — shouldn't happen if CollectRingGridLonLat matched
				UE_LOG(LogFireSimulation2, Warning, TEXT("BuildHeatmapMeshFromCesiumHeights: height index overrun at (%d,%d)"), Col, Row);
				++HeightOverrunCount;
				break;
			}
			const float HeightM = CesiumHeightsM[HeightIdx];
			++HeightIdx;

			MinHeightM = FMath::Min(MinHeightM, HeightM);
			MaxHeightM = FMath::Max(MaxHeightM, HeightM);
			SumHeightM += static_cast<double>(HeightM);

			const FVector EllipsoidPt = DataSubsystem->LonLatToLocalWorld(Lon, Lat, static_cast<double>(HeightM));
			const FVector LocalUp    = EllipsoidPt.GetSafeNormal().IsNearlyZero() ? FVector::UpVector : EllipsoidPt.GetSafeNormal();
			const FVector FinalVertex = EllipsoidPt + LocalUp * ContactOffsetCm;

			const int32 VertIdx = OutVertices.Num();
			GridToVertexIndex.Add(FIntPoint(Col, Row), VertIdx);
			OutVertices.Add(FinalVertex);
			OutNormals.Add(LocalUp);
			OutUV0.Add(FVector2D(
				(WidthDegrees > KINDA_SMALL_NUMBER) ? (Lon - MinBound.X) / WidthDegrees : 0.0f,
				(HeightDegrees > KINDA_SMALL_NUMBER) ? (Lat - MinBound.Y) / HeightDegrees : 0.0f));
			OutVertexColors.Add(ComputeHeatVertexColor(Lon, Lat));
			OutTangents.Add(FProcMeshTangent(FVector::ForwardVector, false));

			// Debug draw: Orange = Cesium terrain point (EllipsoidPt), White = final mesh vertex
			if (bDebug && DebugWorld && DebugDrawCount < MaxDebugDraws)
			{
				DrawDebugPoint(DebugWorld, EllipsoidPt, 14.0f, FColor::Orange, false, DebugSecs, 0);
				DrawDebugPoint(DebugWorld, FinalVertex, 10.0f, FColor::White,  false, DebugSecs, 0);
				++DebugDrawCount;
			}

			// Geometry audit — sample every N-th vertex
			if (AuditRate > 0 && (VertIdx % AuditRate == 0))
			{
				// Height error is structurally zero in this path: Cesium sample Z is fed directly
				// into LonLatToLocalWorld(lon, lat, sampledHeightM) to produce EllipsoidPt.
				const float HeightErrorCm = 0.0f;
				const float FinalClearanceCm = FVector::DotProduct(FinalVertex - EllipsoidPt, LocalUp);
				const float ClearanceErrorCm = FinalClearanceCm - ContactOffsetCm;
				UE_LOG(LogFireSimulation2, Display,
					TEXT("CesiumHeightAudit[%d]: lon=%.6f lat=%.6f cesiumHeightM=%.2f ellipsoidPt=%s finalVertex=%s heightErrorCm=%.2f clearanceErrorCm=%.2f"),
					VertIdx, Lon, Lat, HeightM,
					*EllipsoidPt.ToString(), *FinalVertex.ToString(), HeightErrorCm, ClearanceErrorCm);
			}
		}
	}
	if (HeightOverrunCount > 0)
	{
		UE_LOG(LogFireSimulation2, Warning,
			TEXT("BuildHeatmapMeshFromCesiumHeights: %d height overruns (ring had %d vertices, heights provided=%d)"),
			HeightOverrunCount, OutVertices.Num() + HeightOverrunCount, CesiumHeightsM.Num());
	}
	if (OutVertices.Num() > 0)
	{
		UE_LOG(LogFireSimulation2, Display,
			TEXT("CesiumRingHeightStats: verts=%d heightMin=%.1fm heightMax=%.1fm heightAvg=%.1fm overruns=%d"),
			OutVertices.Num(), MinHeightM, MaxHeightM,
			(OutVertices.Num() > 0) ? static_cast<float>(SumHeightM / OutVertices.Num()) : 0.0f,
			HeightOverrunCount);
	}

	if (OutVertices.Num() < 3) return false;

	// Same grid triangulation as BuildTerrainProjectedHeatmapMeshData
	for (int32 Row = 0; Row < GridRows - 1; ++Row)
	{
		for (int32 Col = 0; Col < GridCols - 1; ++Col)
		{
			const int32* A = GridToVertexIndex.Find(FIntPoint(Col,     Row));
			const int32* B = GridToVertexIndex.Find(FIntPoint(Col + 1, Row));
			const int32* C = GridToVertexIndex.Find(FIntPoint(Col,     Row + 1));
			const int32* D = GridToVertexIndex.Find(FIntPoint(Col + 1, Row + 1));
			if (A && C && B) { OutTriangles.Add(*A); OutTriangles.Add(*C); OutTriangles.Add(*B); }
			if (B && C && D) { OutTriangles.Add(*B); OutTriangles.Add(*C); OutTriangles.Add(*D); }
		}
	}
	if (OutTriangles.Num() < 3) return false;

	// Smooth normal averaging
	{
		TArray<FVector> AccumNormals;
		AccumNormals.SetNumZeroed(OutVertices.Num());
		for (int32 t = 0; t < OutTriangles.Num(); t += 3)
		{
			const int32 Ai = OutTriangles[t], Bi = OutTriangles[t + 1], Ci = OutTriangles[t + 2];
			if (Ai >= OutVertices.Num() || Bi >= OutVertices.Num() || Ci >= OutVertices.Num()) continue;
			const FVector FaceN = FVector::CrossProduct(OutVertices[Bi] - OutVertices[Ai], OutVertices[Ci] - OutVertices[Ai]);
			AccumNormals[Ai] += FaceN; AccumNormals[Bi] += FaceN; AccumNormals[Ci] += FaceN;
		}
		for (int32 i = 0; i < OutVertices.Num(); ++i)
			if (!AccumNormals[i].IsNearlyZero()) OutNormals[i] = AccumNormals[i].GetSafeNormal();
	}

	// Back-face triangles
	const int32 FrontCount = OutTriangles.Num();
	const TArray<int32> FrontTrisCopy = OutTriangles;
	OutTriangles.Reserve(FrontCount * 2);
	for (int32 i = 0; i < FrontCount; i += 3)
	{
		OutTriangles.Add(FrontTrisCopy[i + 2]);
		OutTriangles.Add(FrontTrisCopy[i + 1]);
		OutTriangles.Add(FrontTrisCopy[i]);
	}

	++Metrics.TerrainTraceSuccesses; // count this ring as a success in the accurate path
	return true;
}

void UFireRegionalRendererComponent::RenderRegionalViewWithHeights(
	const TArray<FFireEventWithGeometry>& Events,
	const UFireDataSubsystem* DataSubsystem,
	const TArray<float>& HeightsMeters,
	const TArray<int32>& RingVertexCounts,
	const TArray<bool>& RingBoundaryFallback)
{
	const double BuildStart = FPlatformTime::Seconds();
	++Metrics.RebuildCount;
	Metrics.TerrainSamplePoints = HeightsMeters.Num();
	Metrics.TerrainTraceCalls = 0;
	Metrics.TerrainTraceSuccesses = 0;
	Metrics.TerrainTraceMisses = 0;
	Metrics.TerrainFallbackVertices = 0;
	Metrics.CesiumBatchRequests = 0;
	Metrics.CesiumBatchCompleted = 0;
	Metrics.CesiumBatchFailed = 0;
	Metrics.CesiumRequestedVertices = 0;
	Metrics.CesiumReturnedResults = 0;
	Metrics.CesiumFailedResults = 0;
	Metrics.HeatmapTriangleCount = 0;
	Metrics.PerimeterTriangleCount = 0;
	Metrics.LastTriangulationMs = 0.0;
	Metrics.LastTerrainProjectionMs = 0.0;
	if (const UWorld* World = GetWorld())
	{
		if (const APlayerController* PC = World->GetFirstPlayerController())
			if (const APlayerCameraManager* PCM = PC->PlayerCameraManager)
				Metrics.LastCameraLocation = PCM->GetCameraLocation();
	}

	ClearRegionalView(false);
	if (!DataSubsystem || !PerimeterMesh || !HeatmapMesh) return;

	TArray<FFireEventWithGeometry> SortedEvents = Events;
	SortedEvents.Sort([](const FFireEventWithGeometry& A, const FFireEventWithGeometry& B)
	{ return A.Attributes.Acres > B.Attributes.Acres; });

	LastSourceEventCount = SortedEvents.Num();
	LastEventsWithAnyRings = 0; LastEventsWithRenderableRings = 0;
	LastTotalRings = 0; LastRenderableRings = 0;
	LastRenderedEventCount = 0; LastRenderedRingCount = 0;
	LastPerimeterSectionCount = 0; LastHeatmapSectionCount = 0;
	LastPerimeterVertexCount = 0; LastHeatmapVertexCount = 0;

	int32 RenderedEventCount = 0;
	int32 SectionIndex = 0;
	int32 HeightFlatIdx = 0;  // cursor into HeightsMeters flat array
	int32 RingDataIdx = 0;    // cursor into RingVertexCounts / RingBoundaryFallback

	for (const FFireEventWithGeometry& Event : SortedEvents)
	{
		const int32 RingCount = Event.Geometry.Rings.Num();
		if (RingCount > 0)
		{
			++LastEventsWithAnyRings;
			LastTotalRings += RingCount;
			bool bAnyRenderable = false;
			for (const FFireRing& Ring : Event.Geometry.Rings)
				if (Ring.LonLatVertices.Num() >= 3) bAnyRenderable = true;
			if (bAnyRenderable)
			{
				++LastEventsWithRenderableRings;
				for (const FFireRing& Ring : Event.Geometry.Rings)
					if (Ring.LonLatVertices.Num() >= 3) ++LastRenderableRings;
			}
		}

		if (RenderedEventCount >= MaxRenderedEvents || Event.Attributes.Acres < MinAcresForRegional || Event.Geometry.Rings.Num() == 0)
		{
			// Skip event but advance ring cursor so height indices stay aligned
			for (const FFireRing& Ring : Event.Geometry.Rings)
			{
				if (Ring.LonLatVertices.Num() >= 3 && RingDataIdx < RingVertexCounts.Num())
				{
					HeightFlatIdx += RingVertexCounts[RingDataIdx];
					++RingDataIdx;
				}
			}
			continue;
		}

		const FLinearColor PaletteColor = GetPaletteColor(RenderedEventCount);
		const float HeatAlpha = ComputeHeatAlpha(Event.Attributes);
		const float LayerOffsetCm = static_cast<float>(RenderedEventCount % 50) * 100.0f;
		const float PerimeterClearance = PerimeterBaseOffsetCm + LayerOffsetCm;
		const float HeatmapClearance   = HeatmapOffsetCm + LayerOffsetCm;

		for (const FFireRing& Ring : Event.Geometry.Rings)
		{
			if (Ring.LonLatVertices.Num() < 3)
			{
				// No height data expected for rings that CollectRingGridLonLat would have skipped
				continue;
			}

			// Perimeter mesh — same as RenderRegionalView (physics trace based, this is fine)
			TArray<FVector> PerimeterVertices, PerimeterNormals, PerimeterUV0_vec;
			TArray<int32> PerimeterTriangles;
			TArray<FVector2D> PerimeterUV0;
			TArray<FLinearColor> PerimeterVertexColors;
			TArray<FProcMeshTangent> PerimeterTangents;
			BuildRingMeshData(
				Ring, DataSubsystem, GetWorld(), PerimeterClearance,
				FLinearColor(PaletteColor.R, PaletteColor.G, PaletteColor.B, 0.90f),
				PerimeterVertices, PerimeterTriangles, PerimeterNormals,
				PerimeterUV0, PerimeterVertexColors, PerimeterTangents);

			if (PerimeterVertices.Num() >= 3 && PerimeterTriangles.Num() >= 3)
			{
				PerimeterMesh->CreateMeshSection_LinearColor(
					SectionIndex, PerimeterVertices, PerimeterTriangles,
					PerimeterNormals, PerimeterUV0, PerimeterVertexColors, PerimeterTangents, false);
				++Metrics.CreateMeshSectionCalls;
				Metrics.PerimeterTriangleCount += (PerimeterTriangles.Num() / 3);
				if (PerimeterMaterial) PerimeterMesh->SetMaterial(SectionIndex, PerimeterMaterial);
				++LastPerimeterSectionCount;
				LastPerimeterVertexCount += PerimeterVertices.Num();
			}

			// Heatmap — use Cesium heights
			TArray<FVector> HeatVertices, HeatNormals;
			TArray<int32> HeatTriangles;
			TArray<FVector2D> HeatUV0;
			TArray<FLinearColor> HeatVertexColors;
			TArray<FProcMeshTangent> HeatTangents;

			bool bBuiltHeatmap = false;
			if (RingDataIdx < RingVertexCounts.Num())
			{
				const int32 VCount = RingVertexCounts[RingDataIdx];
				const bool bFallback = RingBoundaryFallback[RingDataIdx];
				++RingDataIdx;

				if (VCount > 0 && HeightFlatIdx + VCount <= HeightsMeters.Num())
				{
					TArrayView<const float> HeightSlice(HeightsMeters.GetData() + HeightFlatIdx, VCount);
					HeightFlatIdx += VCount;
					bBuiltHeatmap = BuildHeatmapMeshFromCesiumHeights(
						Ring, DataSubsystem, HeatmapClearance, HeatAlpha,
						HeightSlice, bFallback,
						HeatVertices, HeatTriangles, HeatNormals,
						HeatUV0, HeatVertexColors, HeatTangents);
				}
				else
				{
					HeightFlatIdx += VCount;
				}
			}

			if (HeatVertices.Num() >= 3 && HeatTriangles.Num() >= 3)
			{
				HeatmapMesh->CreateMeshSection_LinearColor(
					SectionIndex, HeatVertices, HeatTriangles,
					HeatNormals, HeatUV0, HeatVertexColors, HeatTangents, false);
				++Metrics.CreateMeshSectionCalls;
				Metrics.HeatmapTriangleCount += (HeatTriangles.Num() / 3);
				if (HeatmapMaterial) HeatmapMesh->SetMaterial(SectionIndex, HeatmapMaterial);
				++LastHeatmapSectionCount;
				LastHeatmapVertexCount += HeatVertices.Num();
			}
			else if (!bBuiltHeatmap)
			{
				UE_LOG(LogFireSimulation2, Warning, TEXT("RegionalRenderer (Cesium heights): heatmap build failed for one ring."));
			}

			++LastRenderedRingCount;
			++SectionIndex;
		}
		++RenderedEventCount;
	}

	LastRenderedEventCount = RenderedEventCount;
	Metrics.LastBuildMs = (FPlatformTime::Seconds() - BuildStart) * 1000.0;
	UE_LOG(LogFireSimulation2, Display, TEXT("RegionalRenderer CesiumHeightPass: renderedRings=%d heightsUsed=%d traceSuccess=%lld"),
		LastRenderedRingCount, HeightFlatIdx, Metrics.TerrainTraceSuccesses);
	LogRegionalRenderStats();
}

void UFireRegionalRendererComponent::BuildRingMeshData(
	const FFireRing& Ring,
	const UFireDataSubsystem* DataSubsystem,
	UWorld* InWorld,
	const float TerrainClearanceCm,
	const FLinearColor& VertexColor,
	TArray<FVector>& OutVertices,
	TArray<int32>& OutTriangles,
	TArray<FVector>& OutNormals,
	TArray<FVector2D>& OutUV0,
	TArray<FLinearColor>& OutVertexColors,
	TArray<FProcMeshTangent>& OutTangents) const
{
	const double PerimeterStart = FPlatformTime::Seconds();
	OutVertices.Reset();
	OutTriangles.Reset();
	OutNormals.Reset();
	OutUV0.Reset();
	OutVertexColors.Reset();
	OutTangents.Reset();

	const int32 VertexCount = Ring.LonLatVertices.Num();
	OutVertices.Reserve(VertexCount);
	OutNormals.Reserve(VertexCount);
	OutUV0.Reserve(VertexCount);
	OutVertexColors.Reserve(VertexCount);
	OutTangents.Reserve(VertexCount);
	OutTriangles.Reserve(FMath::Max(0, (VertexCount - 2) * 3));

	FVector RingCenter = FVector::ZeroVector;
	for (const FVector2D& LonLat : Ring.LonLatVertices)
	{
		RingCenter += DataSubsystem->LonLatToLocalWorld(LonLat.X, LonLat.Y, 0.0);
	}
	RingCenter /= static_cast<float>(VertexCount);
	FVector UpDir = RingCenter.GetSafeNormal();
	if (UpDir.IsNearlyZero())
	{
		UpDir = FVector::UpVector;
	}
	FVector TangentX = FVector::CrossProduct(UpDir, FVector::UpVector);
	if (TangentX.IsNearlyZero())
	{
		TangentX = FVector::CrossProduct(UpDir, FVector::RightVector);
	}
	TangentX.Normalize();
	const FVector TangentY = FVector::CrossProduct(UpDir, TangentX).GetSafeNormal();

	// Collision query params shared across all vertex traces — ignore only the owning actor so
	// the Fire mesh components don't block their own traces.
	FCollisionQueryParams TraceParams(FName(TEXT("FireTerrainSample")), false);
	if (GetOwner())
	{
		TraceParams.AddIgnoredActor(GetOwner());
	}

	// When no Cesium tile is hit (tiles not yet streamed in), fall back to the WGS84
	// ellipsoid surface at 500 m elevation.  This keeps perimeter vertices above most
	// CONUS terrain (average elevation ~750 m, but the fallback at 500 m is good enough
	// to avoid clipping through low-altitude terrain while tiles are loading).
	// Once the camera descends and RefreshRegionalOnly() is called, the next render
	// will snap to the actual terrain surface.
	constexpr float TerrainFallbackOffsetCm = 0.0f;

	for (const FVector2D& LonLat : Ring.LonLatVertices)
	{
		const FVector Ground = DataSubsystem->LonLatToLocalWorld(LonLat.X, LonLat.Y, 0.0);
		const FVector LocalUp = Ground.GetSafeNormal();
		const FVector ExaggeratedGround = RingCenter + ((Ground - RingCenter) * PerimeterExaggerationScale);

		FVector Vertex;
		if (InWorld)
		{
			// Trace straight down in world-Z — identical to the approach used by
			// FocusFireByOffset (which successfully hits Cesium terrain via ECC_Visibility).
			// Tracing along the globe normal instead would tilt the ray off-axis by up to
			// ~38° at the far edges of CONUS, causing it to miss narrow tile meshes.
			const FVector TraceStart(ExaggeratedGround.X, ExaggeratedGround.Y, ExaggeratedGround.Z + 4000000.0f);
			const FVector TraceEnd  (ExaggeratedGround.X, ExaggeratedGround.Y, ExaggeratedGround.Z - 4000000.0f);

			FHitResult Hit;
			if (InWorld->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_Visibility, TraceParams) ||
				InWorld->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_WorldStatic, TraceParams))
			{
				// Sit the mesh TerrainClearanceCm above the terrain hit in pure world-Z.
				// DO NOT use LocalUp here — at off-center CONUS locations LocalUp has
				// significant X/Y components, adding up to ~1.3 km of horizontal drift per
				// 5 km of offset.  That displaces heatmap vertices away from the correct
				// geographic column, breaking alignment with the pillar markers.
				Vertex = FVector(Hit.Location.X, Hit.Location.Y, Hit.Location.Z + TerrainClearanceCm);
			}
			else
			{
				// Tile not yet loaded — use 500m elevation fallback to keep the mesh
				// above low-altitude terrain while awaiting RefreshRegionalOnly().
				const FVector Fallback500m = DataSubsystem->LonLatToLocalWorld(LonLat.X, LonLat.Y, 3000.0);
				Vertex = FVector(Fallback500m.X, Fallback500m.Y, Fallback500m.Z + TerrainFallbackOffsetCm);
			}
		}
		else
		{
			const FVector Fallback500m = DataSubsystem->LonLatToLocalWorld(LonLat.X, LonLat.Y, 3000.0);
			Vertex = FVector(Fallback500m.X, Fallback500m.Y, Fallback500m.Z + TerrainFallbackOffsetCm);
		}

		OutVertices.Add(Vertex);
		OutNormals.Add(LocalUp);
		const FVector Relative = Vertex - RingCenter;
		const float U = FVector::DotProduct(Relative, TangentX) * 0.00005f + 0.5f;
		const float V = FVector::DotProduct(Relative, TangentY) * 0.00005f + 0.5f;
		OutUV0.Add(FVector2D(U, V));
		OutVertexColors.Add(VertexColor);
		OutTangents.Add(FProcMeshTangent(TangentX, false));
	}

	TArray<FVector2D> Points2D;
	Points2D.Reserve(VertexCount);
	for (const FVector& Vertex : OutVertices)
	{
		const FVector Relative = Vertex - RingCenter;
		const float X = FVector::DotProduct(Relative, TangentX);
		const float Y = FVector::DotProduct(Relative, TangentY);
		Points2D.Add(FVector2D(X, Y));
	}

	// Remove duplicate closing vertex if source ring is explicitly closed.
	if (Points2D.Num() >= 4 && Points2D[0].Equals(Points2D.Last(), 0.01f))
	{
		Points2D.Pop();
		OutVertices.Pop();
		OutNormals.Pop();
		OutUV0.Pop();
		OutVertexColors.Pop();
		OutTangents.Pop();
	}

	TArray<int32> FrontFaceTriangles;
	if (!FireRegionalTriangulation::TriangulateSimplePolygon(Points2D, FrontFaceTriangles))
	{
		return;
	}
	Metrics.LastTriangulationMs += (FPlatformTime::Seconds() - PerimeterStart) * 1000.0;

	OutTriangles = FrontFaceTriangles;
	// Emit back-face triangles too to avoid one-sided material invisibility.
	const int32 FrontCount = FrontFaceTriangles.Num();
	OutTriangles.Reserve(FrontCount * 2);
	for (int32 i = 0; i < FrontCount; i += 3)
	{
		OutTriangles.Add(FrontFaceTriangles[i + 2]);
		OutTriangles.Add(FrontFaceTriangles[i + 1]);
		OutTriangles.Add(FrontFaceTriangles[i]);
	}
}

// ---------------------------------------------------------------------------
// Cesium height-sampling helpers
// ---------------------------------------------------------------------------

ACesium3DTileset* UFireRegionalRendererComponent::FindCesiumTileset() const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}
	for (TActorIterator<ACesium3DTileset> It(World); It; ++It)
	{
		return *It;
	}
	return nullptr;
}

void UFireRegionalRendererComponent::EnsureCesiumTerrainCollision(ACesium3DTileset* Tileset) const
{
	if (!Tileset)
	{
		return;
	}

	Tileset->SetActorEnableCollision(true);

	auto ForceBoolProperty = [](UObject* Obj, const TCHAR* PropertyName, bool bValue)
	{
		if (!Obj)
		{
			return false;
		}
		if (FBoolProperty* BoolProp = FindFProperty<FBoolProperty>(Obj->GetClass(), PropertyName))
		{
			BoolProp->SetPropertyValue_InContainer(Obj, bValue);
			return true;
		}
		return false;
	};

	ForceBoolProperty(Tileset, TEXT("CreatePhysicsMeshes"), true);
	ForceBoolProperty(Tileset, TEXT("EnableCollision"), true);

	TArray<UPrimitiveComponent*> PrimitiveComponents;
	Tileset->GetComponents<UPrimitiveComponent>(PrimitiveComponents, true);
	for (UPrimitiveComponent* Prim : PrimitiveComponents)
	{
		if (!Prim)
		{
			continue;
		}
		Prim->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Prim->SetCollisionResponseToAllChannels(ECR_Ignore);
		Prim->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
		Prim->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
		Prim->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
		Prim->SetCollisionResponseToChannel(ECC_Camera, ECR_Block);
	}
}

void UFireRegionalRendererComponent::RenderRegionalViewWithCesiumHeights(
	const TArray<FFireEventWithGeometry>& Events,
	const UFireDataSubsystem* DataSubsystem)
{
	const uint32 IncomingDatasetHash = ComputeRegionalDatasetHash(Events);
	if (bHasValidCachedDataset &&
		LastDatasetHash == IncomingDatasetHash &&
		LastDatasetEventCount == Events.Num())
	{
		return;
	}

	// Per-request vertex budget for SampleHeightMostDetailed. Requests are queued in
	// batches so large datasets stay responsive and converge progressively.
	static TAutoConsoleVariable<int32> CVarCesiumBatchVertices(
		TEXT("firesim.heatmap.cesium.batchvertices"),
		12000,
		TEXT("Target vertex count per SampleHeightMostDetailed request in progressive regional batching."),
		ECVF_Default);
	static TAutoConsoleVariable<float> CVarCesiumBatchBucketDegrees(
		TEXT("firesim.heatmap.cesium.bucketdegrees"),
		0.5f,
		TEXT("Spatial bucket size in degrees for progressive Cesium sampling. Batches stay within a single bucket."),
		ECVF_Default);

	LastDatasetHash = IncomingDatasetHash;
	LastDatasetEventCount = Events.Num();
	bHasValidCachedDataset = true;
	++ActiveCesiumGeneration;
	const uint64 ThisGeneration = ActiveCesiumGeneration;

	// --- Immediate synchronous render using ellipsoid heights (no physics traces, instant) ---
	// This provides immediate visual output while Cesium loads accurate tile heights.
	RenderRegionalView(Events, DataSubsystem, /*bEllipsoidOnly=*/true);

	ACesium3DTileset* Tileset = FindCesiumTileset();
	if (!Tileset)
	{
		return;
	}
	EnsureCesiumTerrainCollision(Tileset);

	// Sort events largest-first, same as RenderRegionalView, so ring indices align
	TArray<FFireEventWithGeometry> SortedEvents = Events;
	SortedEvents.Sort([](const FFireEventWithGeometry& A, const FFireEventWithGeometry& B)
	{ return A.Attributes.Acres > B.Attributes.Acres; });

	// Build per-ring Cesium sampling jobs aligned to section indices.
	PendingCesiumRings.Reset();
	PendingCesiumNextRingIndex = 0;
	PendingCesiumBatchRequests = 0;
	PendingCesiumBatchFailures = 0;
	PendingCesiumRequestedVertices = 0;
	PendingCesiumStartSeconds = FPlatformTime::Seconds();
	PendingCesiumBatchDispatchSeconds.Reset();
	PendingCesiumTileset = Tileset;

	int32 RenderedEventCount = 0;
	int32 SectionIndex = 0;
	for (const FFireEventWithGeometry& Event : SortedEvents)
	{
		if (RenderedEventCount >= MaxRenderedEvents || Event.Attributes.Acres < MinAcresForRegional || Event.Geometry.Rings.Num() == 0)
		{
			continue;
		}

		const float HeatAlpha = ComputeHeatAlpha(Event.Attributes);
		const float LayerOffsetCm = static_cast<float>(RenderedEventCount % 50) * 100.0f;
		const float HeatmapClearance = HeatmapOffsetCm + LayerOffsetCm;

		for (const FFireRing& Ring : Event.Geometry.Rings)
		{
			if (Ring.LonLatVertices.Num() < 3)
			{
				++SectionIndex;
				continue;
			}

			TArray<FVector2D> RingLonLat;
			bool bFallback = false;
			CollectRingGridLonLat(Ring, RingLonLat, bFallback);
			if (RingLonLat.Num() >= 3)
			{
				FPendingCesiumRingSample& Pending = PendingCesiumRings.AddDefaulted_GetRef();
				Pending.Ring = Ring;
				Pending.SectionIndex = SectionIndex;
				Pending.HeatAlpha = HeatAlpha;
				Pending.HeatmapClearanceCm = HeatmapClearance;
				Pending.bUsedBoundaryFallback = bFallback;
				Pending.SampleLonLat = MoveTemp(RingLonLat);
				double SumLon = 0.0;
				double SumLat = 0.0;
				int32 FiniteCount = 0;
				for (const FVector2D& LonLat : Pending.SampleLonLat)
				{
					const double Lon = static_cast<double>(LonLat.X);
					const double Lat = static_cast<double>(LonLat.Y);
					if (!FMath::IsFinite(Lon) || !FMath::IsFinite(Lat))
					{
						continue;
					}
					SumLon += Lon;
					SumLat += Lat;
					++FiniteCount;
				}
				if (FiniteCount > 0)
				{
					Pending.CentroidLon = SumLon / static_cast<double>(FiniteCount);
					Pending.CentroidLat = SumLat / static_cast<double>(FiniteCount);
				}
				Pending.NextSampleOffset = 0;
				Pending.CompletedSampleCount = 0;
				Pending.SampledHeightsMeters.SetNumZeroed(Pending.SampleLonLat.Num());
				PendingCesiumRequestedVertices += Pending.SampleLonLat.Num();
			}

			++SectionIndex;
		}
		++RenderedEventCount;
	}

	if (PendingCesiumRings.IsEmpty())
	{
		return;
	}

	const double BucketDegrees = FMath::Max(0.05, static_cast<double>(CVarCesiumBatchBucketDegrees.GetValueOnAnyThread()));
	TSet<uint64> UniqueBuckets;
	for (FPendingCesiumRingSample& Pending : PendingCesiumRings)
	{
		Pending.BucketLonIndex = FMath::FloorToInt(Pending.CentroidLon / BucketDegrees);
		Pending.BucketLatIndex = FMath::FloorToInt(Pending.CentroidLat / BucketDegrees);
		const uint32 LonBits = static_cast<uint32>(Pending.BucketLonIndex);
		const uint32 LatBits = static_cast<uint32>(Pending.BucketLatIndex);
		const uint64 BucketKey = (static_cast<uint64>(LonBits) << 32) | static_cast<uint64>(LatBits);
		UniqueBuckets.Add(BucketKey);
	}
	PendingCesiumRings.Sort([](const FPendingCesiumRingSample& A, const FPendingCesiumRingSample& B)
	{
		if (A.BucketLonIndex != B.BucketLonIndex)
		{
			return A.BucketLonIndex < B.BucketLonIndex;
		}
		if (A.BucketLatIndex != B.BucketLatIndex)
		{
			return A.BucketLatIndex < B.BucketLatIndex;
		}
		if (!FMath::IsNearlyEqual(A.CentroidLon, B.CentroidLon))
		{
			return A.CentroidLon < B.CentroidLon;
		}
		if (!FMath::IsNearlyEqual(A.CentroidLat, B.CentroidLat))
		{
			return A.CentroidLat < B.CentroidLat;
		}
		return A.SectionIndex < B.SectionIndex;
	});

	UE_LOG(LogFireSimulation2, Display,
		TEXT("RegionalRenderer: queued progressive Cesium height sampling datasetHash=%u generation=%llu rings=%d vertices=%lld batchTarget=%d bucketDegrees=%.2f uniqueBuckets=%d queuedTs=%.3f"),
		IncomingDatasetHash,
		ThisGeneration,
		PendingCesiumRings.Num(),
		PendingCesiumRequestedVertices,
		FMath::Max(1024, CVarCesiumBatchVertices.GetValueOnAnyThread()),
		BucketDegrees,
		UniqueBuckets.Num(),
		PendingCesiumStartSeconds);

	Metrics.CesiumBatchRequests = 0;
	Metrics.CesiumBatchCompleted = 0;
	Metrics.CesiumBatchFailed = 0;
	Metrics.CesiumRequestedVertices = PendingCesiumRequestedVertices;
	Metrics.CesiumReturnedResults = 0;
	Metrics.CesiumFailedResults = 0;

	DispatchNextCesiumHeightBatch(Tileset, ThisGeneration);
}

void UFireRegionalRendererComponent::DispatchNextCesiumHeightBatch(ACesium3DTileset* Tileset, const uint64 Generation)
{
	if (Generation != ActiveCesiumGeneration)
	{
		UE_LOG(
			LogFireSimulation2,
			Display,
			TEXT("RegionalRenderer: DispatchNextCesiumHeightBatch discarded generation mismatch requestedGen=%llu activeGen=%llu datasetHash=%u"),
			Generation,
			ActiveCesiumGeneration,
			LastDatasetHash);
		return;
	}
	if (!bHasValidCachedDataset)
	{
		UE_LOG(
			LogFireSimulation2,
			Display,
			TEXT("RegionalRenderer: DispatchNextCesiumHeightBatch aborted (no valid cached dataset) generation=%llu datasetHash=%u"),
			Generation,
			LastDatasetHash);
		return;
	}
	if (LastDatasetHash == 0u)
	{
		UE_LOG(
			LogFireSimulation2,
			Display,
			TEXT("RegionalRenderer: DispatchNextCesiumHeightBatch aborted (dataset hash is zero) generation=%llu"),
			Generation);
		return;
	}
	if (!Tileset)
	{
		Tileset = PendingCesiumTileset.Get();
	}
	if (!Tileset)
	{
		++PendingCesiumBatchFailures;
		UE_LOG(
			LogFireSimulation2,
			Warning,
			TEXT("RegionalRenderer: DispatchNextCesiumHeightBatch failed (no tileset) datasetHash=%u generation=%llu"),
			LastDatasetHash,
			Generation);
		return;
	}

	int32 BatchVertexBudget = 12000;
	if (IConsoleVariable* BatchVar = IConsoleManager::Get().FindConsoleVariable(TEXT("firesim.heatmap.cesium.batchvertices")))
	{
		BatchVertexBudget = FMath::Max(1024, BatchVar->GetInt());
	}

	if (PendingCesiumNextRingIndex >= PendingCesiumRings.Num())
	{
		const double TotalMs = (FPlatformTime::Seconds() - PendingCesiumStartSeconds) * 1000.0;
		UE_LOG(
			LogFireSimulation2,
			Display,
			TEXT("RegionalRenderer: Cesium progressive sampling complete datasetHash=%u generation=%llu batches=%d failedBatches=%d requestedVerts=%lld returnedResults=%lld resultFailures=%lld totalMs=%.2f completionEventEmitted=1"),
			LastDatasetHash,
			Generation,
			PendingCesiumBatchRequests,
			PendingCesiumBatchFailures,
			PendingCesiumRequestedVertices,
			Metrics.CesiumReturnedResults,
			Metrics.CesiumFailedResults,
			TotalMs);
		LogRegionalRenderStats();
		return;
	}

	TArray<int32> BatchRingIndices;
	TArray<FVector> BatchSamplePoints;
	BatchRingIndices.Reserve(32);
	BatchSamplePoints.Reserve(BatchVertexBudget + 256);

	int64 AccumulatedVerts = 0;
	int32 Cursor = PendingCesiumNextRingIndex;
	int32 BatchBucketLon = 0;
	int32 BatchBucketLat = 0;
	bool bHasBatchBucket = false;
	while (Cursor < PendingCesiumRings.Num())
	{
		const FPendingCesiumRingSample& RingSample = PendingCesiumRings[Cursor];
		const int32 RingVerts = RingSample.SampleLonLat.Num();
		if (RingVerts <= 0)
		{
			++Cursor;
			continue;
		}
		if (!bHasBatchBucket)
		{
			BatchBucketLon = RingSample.BucketLonIndex;
			BatchBucketLat = RingSample.BucketLatIndex;
			bHasBatchBucket = true;
		}
		else if (RingSample.BucketLonIndex != BatchBucketLon || RingSample.BucketLatIndex != BatchBucketLat)
		{
			break;
		}

		const bool bWouldOverflow = (AccumulatedVerts > 0) && (AccumulatedVerts + RingVerts > BatchVertexBudget);
		if (bWouldOverflow)
		{
			break;
		}

		BatchRingIndices.Add(Cursor);
		AccumulatedVerts += RingVerts;
		for (const FVector2D& LonLat : RingSample.SampleLonLat)
		{
			BatchSamplePoints.Add(FVector(LonLat.X, LonLat.Y, 0.0));
		}
		++Cursor;
	}

	if (BatchRingIndices.IsEmpty() || BatchSamplePoints.IsEmpty())
	{
		// Defensive progress to avoid infinite loops on malformed input.
		++PendingCesiumNextRingIndex;
		DispatchNextCesiumHeightBatch(Tileset, Generation);
		return;
	}
	PendingCesiumNextRingIndex = Cursor;
	++PendingCesiumBatchRequests;
	Metrics.CesiumBatchRequests = PendingCesiumBatchRequests;

	const int32 BatchRequestIndex = PendingCesiumBatchRequests;
	const int32 BatchRingCount = BatchRingIndices.Num();
	const int32 BatchVertexCount = BatchSamplePoints.Num();
	const int32 BatchRingStart = BatchRingIndices[0];
	const int32 BatchRingEnd = BatchRingIndices.Last();
	double MinLon = TNumericLimits<double>::Max();
	double MaxLon = TNumericLimits<double>::Lowest();
	double MinLat = TNumericLimits<double>::Max();
	double MaxLat = TNumericLimits<double>::Lowest();
	int32 InvalidFiniteCount = 0;
	int32 InvalidRangeCount = 0;
	for (const FVector& SamplePoint : BatchSamplePoints)
	{
		const double Lon = static_cast<double>(SamplePoint.X);
		const double Lat = static_cast<double>(SamplePoint.Y);
		if (!FMath::IsFinite(Lon) || !FMath::IsFinite(Lat))
		{
			++InvalidFiniteCount;
			continue;
		}
		MinLon = FMath::Min(MinLon, Lon);
		MaxLon = FMath::Max(MaxLon, Lon);
		MinLat = FMath::Min(MinLat, Lat);
		MaxLat = FMath::Max(MaxLat, Lat);
		if (Lon < -180.0 || Lon > 180.0 || Lat < -90.0 || Lat > 90.0)
		{
			++InvalidRangeCount;
		}
	}
	const double BatchDispatchTs = FPlatformTime::Seconds();
	PendingCesiumBatchDispatchSeconds.Add(BatchRequestIndex, BatchDispatchTs);
	const uint32 DatasetHashAtDispatch = LastDatasetHash;
	UE_LOG(
		LogFireSimulation2,
		Display,
		TEXT("RegionalRenderer: Cesium batch request datasetHash=%u generation=%llu batch=%d ringRange=%d-%d rings=%d vertices=%d bucket=(%d,%d) lonRange=[%.6f,%.6f] latRange=[%.6f,%.6f] invalidFinite=%d invalidRange=%d dispatchedTs=%.3f nextRing=%d/%d"),
		DatasetHashAtDispatch,
		Generation,
		BatchRequestIndex,
		BatchRingStart,
		BatchRingEnd,
		BatchRingCount,
		BatchVertexCount,
		bHasBatchBucket ? BatchBucketLon : 0,
		bHasBatchBucket ? BatchBucketLat : 0,
		BatchVertexCount > InvalidFiniteCount ? MinLon : 0.0,
		BatchVertexCount > InvalidFiniteCount ? MaxLon : 0.0,
		BatchVertexCount > InvalidFiniteCount ? MinLat : 0.0,
		BatchVertexCount > InvalidFiniteCount ? MaxLat : 0.0,
		InvalidFiniteCount,
		InvalidRangeCount,
		BatchDispatchTs,
		PendingCesiumNextRingIndex,
		PendingCesiumRings.Num());

	TWeakObjectPtr<UFireRegionalRendererComponent> WeakThis(this);
	TWeakObjectPtr<ACesium3DTileset> WeakTileset(Tileset);
	Tileset->SampleHeightMostDetailed(
		BatchSamplePoints,
		FCesiumSampleHeightMostDetailedCallback::CreateLambda(
			[WeakThis, WeakTileset, Generation, DatasetHashAtDispatch, BatchRingIndices, BatchRequestIndex, BatchVertexCount, BatchRingStart, BatchRingEnd](
				ACesium3DTileset* /*InTileset*/,
				const TArray<FCesiumSampleHeightResult>& Results,
				const TArray<FString>& Warnings)
			{
				const double CallbackTs = FPlatformTime::Seconds();
				const uint32 CallbackThreadId = FPlatformTLS::GetCurrentThreadId();
				const bool bCallbackOnGameThread = IsInGameThread();
				UE_LOG(
					LogFireSimulation2,
					Display,
					TEXT("RegionalRenderer: Cesium batch callback received datasetHash=%u generation=%llu batch=%d ringRange=%d-%d vertices=%d callbackTs=%.3f callbackThreadId=%u callbackOnGameThread=%d warnings=%d results=%d"),
					DatasetHashAtDispatch,
					Generation,
					BatchRequestIndex,
					BatchRingStart,
					BatchRingEnd,
					BatchVertexCount,
					CallbackTs,
					CallbackThreadId,
					bCallbackOnGameThread ? 1 : 0,
					Warnings.Num(),
					Results.Num());

				AsyncTask(ENamedThreads::GameThread, [WeakThis, WeakTileset, Generation, DatasetHashAtDispatch, BatchRingIndices, BatchRequestIndex, BatchVertexCount, Results, Warnings]()
				{
					UFireRegionalRendererComponent* Self = WeakThis.Get();
					if (!Self)
					{
						UE_LOG(
							LogFireSimulation2,
							Display,
							TEXT("RegionalRenderer: Cesium batch callback discarded batch=%d reason=renderer_destroyed"),
							BatchRequestIndex);
						return;
					}
					if (Generation != Self->ActiveCesiumGeneration)
					{
						UE_LOG(
							LogFireSimulation2,
							Display,
							TEXT("RegionalRenderer: Cesium batch callback discarded datasetHash=%u batch=%d reason=stale_generation callbackGen=%llu activeGen=%llu"),
							Self->LastDatasetHash,
							BatchRequestIndex,
							Generation,
							Self->ActiveCesiumGeneration);
						return;
					}
					if (!Self->bHasValidCachedDataset)
					{
						UE_LOG(
							LogFireSimulation2,
							Display,
							TEXT("RegionalRenderer: Cesium batch callback discarded datasetHash=%u batch=%d reason=no_valid_cached_dataset"),
							Self->LastDatasetHash,
							BatchRequestIndex);
						return;
					}
					if (Self->LastDatasetHash != DatasetHashAtDispatch)
					{
						UE_LOG(
							LogFireSimulation2,
							Display,
							TEXT("RegionalRenderer: Cesium batch callback discarded batch=%d reason=dataset_mismatch callbackHash=%u activeHash=%u"),
							BatchRequestIndex,
							DatasetHashAtDispatch,
							Self->LastDatasetHash);
						return;
					}

					const double CallbackHandledTs = FPlatformTime::Seconds();
					double DispatchTs = 0.0;
					if (const double* DispatchTsPtr = Self->PendingCesiumBatchDispatchSeconds.Find(BatchRequestIndex))
					{
						DispatchTs = *DispatchTsPtr;
					}
					UE_LOG(
						LogFireSimulation2,
						Display,
						TEXT("RegionalRenderer: Cesium batch callback accepted datasetHash=%u generation=%llu batch=%d dispatchTs=%.3f callbackHandledTs=%.3f latencyMs=%.2f"),
						Self->LastDatasetHash,
						Generation,
						BatchRequestIndex,
						DispatchTs,
						CallbackHandledTs,
						DispatchTs > 0.0 ? (CallbackHandledTs - DispatchTs) * 1000.0 : -1.0);

					for (const FString& W : Warnings)
					{
						UE_LOG(LogFireSimulation2, Warning, TEXT("RegionalRenderer SampleHeight warning (batch %d): %s"), BatchRequestIndex, *W);
					}

					if (Results.Num() != BatchVertexCount)
					{
						UE_LOG(
							LogFireSimulation2,
							Warning,
							TEXT("RegionalRenderer: batch %d returned %d results for %d requests."),
							BatchRequestIndex,
							Results.Num(),
							BatchVertexCount);
					}

					const UFireDataSubsystem* DataSub = nullptr;
					if (UWorld* World = Self->GetWorld())
					{
						if (UGameInstance* GI = World->GetGameInstance())
						{
							DataSub = GI->GetSubsystem<UFireDataSubsystem>();
						}
					}
					if (!DataSub || !Self->HeatmapMesh)
					{
						++Self->PendingCesiumBatchFailures;
						Self->Metrics.CesiumBatchFailed = Self->PendingCesiumBatchFailures;
						UE_LOG(
							LogFireSimulation2,
							Warning,
							TEXT("RegionalRenderer: Cesium batch callback early-return datasetHash=%u batch=%d reason=missing_datasub_or_heatmap dataSub=%d heatmap=%d"),
							Self->LastDatasetHash,
							BatchRequestIndex,
							DataSub ? 1 : 0,
							Self->HeatmapMesh ? 1 : 0);
						return;
					}

					int32 ResultOffset = 0;
					int32 BatchFailures = 0;
					for (const int32 RingIndex : BatchRingIndices)
					{
						if (!Self->PendingCesiumRings.IsValidIndex(RingIndex))
						{
							++BatchFailures;
							continue;
						}

						const FPendingCesiumRingSample& RingSample = Self->PendingCesiumRings[RingIndex];
						const int32 RingVertCount = RingSample.SampleLonLat.Num();
						if (RingVertCount < 3 || ResultOffset + RingVertCount > Results.Num())
						{
							++BatchFailures;
							ResultOffset += RingVertCount;
							continue;
						}

						TArray<float> HeightsMeters;
						HeightsMeters.Reserve(RingVertCount);
						for (int32 i = 0; i < RingVertCount; ++i)
						{
							const FCesiumSampleHeightResult& R = Results[ResultOffset + i];
							const bool bSuccess = R.SampleSuccess;
							HeightsMeters.Add(bSuccess ? static_cast<float>(R.LongitudeLatitudeHeight.Z) : 0.0f);
							++Self->Metrics.CesiumReturnedResults;
							if (!bSuccess)
							{
								++Self->Metrics.CesiumFailedResults;
							}
						}
						ResultOffset += RingVertCount;

						TArray<FVector> HeatVertices;
						TArray<int32> HeatTriangles;
						TArray<FVector> HeatNormals;
						TArray<FVector2D> HeatUV0;
						TArray<FLinearColor> HeatVertexColors;
						TArray<FProcMeshTangent> HeatTangents;
						const bool bBuilt = Self->BuildHeatmapMeshFromCesiumHeights(
							RingSample.Ring,
							DataSub,
							RingSample.HeatmapClearanceCm,
							RingSample.HeatAlpha,
							TArrayView<const float>(HeightsMeters),
							RingSample.bUsedBoundaryFallback,
							HeatVertices,
							HeatTriangles,
							HeatNormals,
							HeatUV0,
							HeatVertexColors,
							HeatTangents);

						if (bBuilt && HeatVertices.Num() >= 3 && HeatTriangles.Num() >= 3)
						{
							Self->HeatmapMesh->CreateMeshSection_LinearColor(
								RingSample.SectionIndex,
								HeatVertices,
								HeatTriangles,
								HeatNormals,
								HeatUV0,
								HeatVertexColors,
								HeatTangents,
								false);
							if (Self->HeatmapMaterial)
							{
								Self->HeatmapMesh->SetMaterial(RingSample.SectionIndex, Self->HeatmapMaterial);
							}
						}
						else
						{
							++BatchFailures;
						}
					}

					if (BatchFailures > 0)
					{
						Self->PendingCesiumBatchFailures += BatchFailures;
					}
					Self->Metrics.CesiumBatchFailed = Self->PendingCesiumBatchFailures;
					++Self->Metrics.CesiumBatchCompleted;
					UE_LOG(
						LogFireSimulation2,
						Display,
						TEXT("RegionalRenderer: Cesium batch complete datasetHash=%u generation=%llu batch=%d ringRange=%d-%d rings=%d vertices=%d failures=%d returned=%d"),
						Self->LastDatasetHash,
						Generation,
						BatchRequestIndex,
						BatchRingIndices[0],
						BatchRingIndices.Last(),
						BatchRingIndices.Num(),
						BatchVertexCount,
						BatchFailures,
						Results.Num());

					UE_LOG(
						LogFireSimulation2,
						Display,
						TEXT("RegionalRenderer: invoking DispatchNextCesiumHeightBatch datasetHash=%u generation=%llu afterBatch=%d"),
						Self->LastDatasetHash,
						Generation,
						BatchRequestIndex);
					Self->DispatchNextCesiumHeightBatch(WeakTileset.Get(), Generation);
				});
			}));
}
