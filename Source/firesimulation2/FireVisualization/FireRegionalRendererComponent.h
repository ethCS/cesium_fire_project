#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "FireDataTypes.h"
#include "ProceduralMeshComponent.h"
#include "FireRegionalRendererComponent.generated.h"

class UFireDataSubsystem;
class UMaterialInterface;
class UProceduralMeshComponent;
class USceneComponent;
class UWorld;
class ACesium3DTileset;

UCLASS(ClassGroup = (Fire), meta = (BlueprintSpawnableComponent))
class FIRESIMULATION2_API UFireRegionalRendererComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UFireRegionalRendererComponent();

	void InitializeRenderer(USceneComponent* AttachParent);
	void Configure(float InPerimeterBaseOffsetCm, float InHeatmapOffsetCm, int32 InMaxRenderedEvents, double InMinAcresForRegional, float InPerimeterExaggerationScale);
	void RenderRegionalView(const TArray<FFireEventWithGeometry>& Events, const UFireDataSubsystem* DataSubsystem, bool bEllipsoidOnly = false);

	/**
	 * Progressive Cesium-height version: renders an immediate ellipsoid placeholder, then
	 * samples terrain heights in asynchronous batches and updates heatmap mesh sections
	 * incrementally until the dataset converges to terrain-conforming geometry.
	 */
	void RenderRegionalViewWithCesiumHeights(const TArray<FFireEventWithGeometry>& Events, const UFireDataSubsystem* DataSubsystem);
	void ClearRegionalView(bool bResetDatasetCache = true);
	void SetHeatmapVisible(bool bVisible);
	void LogRegionalRenderStats() const;
	void DrawPerimeterDebug(const TArray<FFireEventWithGeometry>& Events, const UFireDataSubsystem* DataSubsystem, float DurationSeconds, float SphereRadiusCm) const;

	int32 GetLastSourceEventCount() const { return LastSourceEventCount; }
	int32 GetLastEventsWithAnyRings() const { return LastEventsWithAnyRings; }
	int32 GetLastEventsWithRenderableRings() const { return LastEventsWithRenderableRings; }
	int32 GetLastTotalRings() const { return LastTotalRings; }
	int32 GetLastRenderableRings() const { return LastRenderableRings; }
	int32 GetLastRenderedEventCount() const { return LastRenderedEventCount; }
	int32 GetLastRenderedRingCount() const { return LastRenderedRingCount; }
	int32 GetLastPerimeterSectionCount() const { return LastPerimeterSectionCount; }
	int32 GetLastHeatmapSectionCount() const { return LastHeatmapSectionCount; }
	int32 GetLastPerimeterVertexCount() const { return LastPerimeterVertexCount; }
	int32 GetLastHeatmapVertexCount() const { return LastHeatmapVertexCount; }

	// Returns one of 10 visually distinct colors from the ColorBrewer Set1 palette.
	// Cycling by rendered-event rank ensures adjacent/overlapping fires never share a hue.
	static FLinearColor GetPaletteColor(int32 RenderedRank);

private:
	struct FRegionalRenderMetrics
	{
		int64 RebuildCount = 0;
		int64 CreateMeshSectionCalls = 0;
		int64 ClearAllMeshSectionsCalls = 0;
		int64 TerrainSamplePoints = 0;
		int64 TerrainTraceCalls = 0;
		int64 TerrainTraceSuccesses = 0;
		int64 TerrainTraceMisses = 0;
		int64 TerrainFallbackVertices = 0;
		int64 CesiumBatchRequests = 0;
		int64 CesiumBatchCompleted = 0;
		int64 CesiumBatchFailed = 0;
		int64 CesiumRequestedVertices = 0;
		int64 CesiumReturnedResults = 0;
		int64 CesiumFailedResults = 0;
		int64 HeatmapTriangleCount = 0;
		int64 PerimeterTriangleCount = 0;
		double LastBuildMs = 0.0;
		double LastTriangulationMs = 0.0;
		double LastTerrainProjectionMs = 0.0;
		FVector LastCameraLocation = FVector::ZeroVector;
	};

	float ComputeHeatAlpha(const FFireEventAttributes& Attributes) const;
	ACesium3DTileset* FindCesiumTileset() const;
	void EnsureCesiumTerrainCollision(ACesium3DTileset* Tileset) const;

	/**
	 * Enumerates interior grid vertices for a ring in the same row-major order used by
	 * BuildTerrainProjectedHeatmapMeshData and BuildHeatmapMeshFromCesiumHeights.
	 * Call this to build the flat vertex list passed to SampleHeightMostDetailed.
	 * Returns false if the ring produces no usable grid points.
	 */
	bool CollectRingGridLonLat(
		const FFireRing& Ring,
		TArray<FVector2D>& OutVerticesLonLat,
		bool& bOutUsedBoundaryFallback) const;

	/**
	 * Builds the heatmap mesh section using pre-sampled Cesium heights instead of
	 * physics traces.  CesiumHeightsM must be indexed identically to the output of
	 * CollectRingGridLonLat for the same ring (same grid step, same boundary).
	 * Heights are in metres above the WGS84 ellipsoid (FCesiumSampleHeightResult::Z).
	 */
	bool BuildHeatmapMeshFromCesiumHeights(
		const FFireRing& Ring,
		const UFireDataSubsystem* DataSubsystem,
		float TerrainClearanceCm,
		float HeatAlpha,
		TArrayView<const float> CesiumHeightsM,
		bool bUsedBoundaryFallback,
		TArray<FVector>& OutVertices,
		TArray<int32>& OutTriangles,
		TArray<FVector>& OutNormals,
		TArray<FVector2D>& OutUV0,
		TArray<FLinearColor>& OutVertexColors,
		TArray<FProcMeshTangent>& OutTangents) const;

	/**
	 * Rebuilds the regional mesh using Cesium-sampled heights.
	 * Called from the SampleHeightMostDetailed async callback once all heights are known.
	 * HeightsMeters is a flat array — vertices for ring 0 first, then ring 1, etc.
	 * RingVertexCounts[i] is the vertex count for ring i, matching CollectRingGridLonLat output.
	 * RingBoundaryFallback[i] records whether ring i used boundary-fallback enumeration.
	 */
	void RenderRegionalViewWithHeights(
		const TArray<FFireEventWithGeometry>& Events,
		const UFireDataSubsystem* DataSubsystem,
		const TArray<float>& HeightsMeters,
		const TArray<int32>& RingVertexCounts,
		const TArray<bool>& RingBoundaryFallback);
	void DispatchNextCesiumHeightBatch(ACesium3DTileset* Tileset, uint64 Generation);

	bool BuildTerrainProjectedHeatmapMeshData(
		const FFireRing& Ring,
		const UFireDataSubsystem* DataSubsystem,
		UWorld* InWorld,
		float TerrainClearanceCm,
		float HeatAlpha,
		TArray<FVector>& OutVertices,
		TArray<int32>& OutTriangles,
		TArray<FVector>& OutNormals,
		TArray<FVector2D>& OutUV0,
		TArray<FLinearColor>& OutVertexColors,
		TArray<FProcMeshTangent>& OutTangents,
		bool bEllipsoidOnly = false) const;
	void BuildRingMeshData(
		const FFireRing& Ring,
		const UFireDataSubsystem* DataSubsystem,
		UWorld* InWorld,
		float TerrainClearanceCm,
		const FLinearColor& VertexColor,
		TArray<FVector>& OutVertices,
		TArray<int32>& OutTriangles,
		TArray<FVector>& OutNormals,
		TArray<FVector2D>& OutUV0,
		TArray<FLinearColor>& OutVertexColors,
		TArray<FProcMeshTangent>& OutTangents) const;

	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> PerimeterMesh;

	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> HeatmapMesh;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> PerimeterMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> HeatmapMaterial;

	struct FPendingCesiumRingSample
	{
		FFireRing Ring;
		int32 SectionIndex = INDEX_NONE;
		float HeatAlpha = 0.5f;
		float HeatmapClearanceCm = 220.0f;
		bool bUsedBoundaryFallback = false;
		int32 BucketLonIndex = 0;
		int32 BucketLatIndex = 0;
		double CentroidLon = 0.0;
		double CentroidLat = 0.0;
		int32 NextSampleOffset = 0;
		int32 CompletedSampleCount = 0;
		TArray<FVector2D> SampleLonLat;
		TArray<float> SampledHeightsMeters;
	};

	TArray<FPendingCesiumRingSample> PendingCesiumRings;
	int32 PendingCesiumNextRingIndex = 0;
	uint64 ActiveCesiumGeneration = 0;
	int32 PendingCesiumBatchRequests = 0;
	int32 PendingCesiumBatchFailures = 0;
	int64 PendingCesiumRequestedVertices = 0;
	double PendingCesiumStartSeconds = 0.0;
	TMap<int32, double> PendingCesiumBatchDispatchSeconds;
	TWeakObjectPtr<ACesium3DTileset> PendingCesiumTileset;

	uint32 LastDatasetHash = 0;
	int32 LastDatasetEventCount = -1;
	bool bHasValidCachedDataset = false;

	float PerimeterBaseOffsetCm = 1200.0f;
	float HeatmapOffsetCm = 220.0f;
	float HeatmapGridMinStepCm = 2500.0f;
	int32 HeatmapMaxGridVerticesPerRing = 3000;
	int32 MaxRenderedEvents = 500;
	double MinAcresForRegional = 150.0;
	float PerimeterExaggerationScale = 1.0f;

	int32 LastSourceEventCount = 0;
	int32 LastEventsWithAnyRings = 0;
	int32 LastEventsWithRenderableRings = 0;
	int32 LastTotalRings = 0;
	int32 LastRenderableRings = 0;
	int32 LastRenderedEventCount = 0;
	int32 LastRenderedRingCount = 0;
	int32 LastPerimeterSectionCount = 0;
	int32 LastHeatmapSectionCount = 0;
	int32 LastPerimeterVertexCount = 0;
	int32 LastHeatmapVertexCount = 0;
	mutable FRegionalRenderMetrics Metrics;
};
