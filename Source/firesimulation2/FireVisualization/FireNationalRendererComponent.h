#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "FireDataTypes.h"
#include "FireNationalRendererComponent.generated.h"

class UFireDataSubsystem;
class UInstancedStaticMeshComponent;
class USceneComponent;

UCLASS(ClassGroup = (Fire), meta = (BlueprintSpawnableComponent))
class FIRESIMULATION2_API UFireNationalRendererComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UFireNationalRendererComponent();

	void InitializeRenderer(USceneComponent* AttachParent);
	void Configure(float InPerimeterZOffset, float InPointMarkerHeightOffset, float InPointMarkerPillarHeightCm, float InPointMarkerDiameterCm, bool bInShowPointInstances);
	void RenderNationalView(const TArray<FFireEventWithGeometry>& Events, const UFireDataSubsystem* DataSubsystem);
	void ClearNationalView();

	UInstancedStaticMeshComponent* GetPointInstances() const { return PointInstances; }
	const TArray<FFireEventWithGeometry>& GetRenderedEvents() const { return RenderedEvents; }
	const TArray<FVector>& GetRenderedEventGroundCenters() const { return RenderedEventGroundCenters; }
	const TArray<FVector>& GetRenderedEventCenters() const { return RenderedEventCenters; }

	int32 GetEventCount() const { return RenderedEvents.Num(); }
	bool IsValidEventIndex(int32 EventIndex) const;
	void HideMarkerByIndex(int32 EventIndex);
	int32 FindNearestEventToRay(const FVector& RayOrigin, const FVector& RayDirection, double MaxDistanceCm, double& OutDistanceSq) const;

private:
	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> PointInstances;

	UPROPERTY(Transient)
	TArray<FFireEventWithGeometry> RenderedEvents;

	UPROPERTY(Transient)
	TArray<FVector> RenderedEventGroundCenters;

	UPROPERTY(Transient)
	TArray<FVector> RenderedEventCenters;

	float PerimeterZOffset = 60.0f;
	float PointMarkerHeightOffset = 500000.0f;
	float PointMarkerPillarHeightCm = 9000000.0f;
	float PointMarkerDiameterCm = 90000.0f;
	bool bShowPointInstances = true;
};
