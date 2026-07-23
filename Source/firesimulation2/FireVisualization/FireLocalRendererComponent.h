#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "FireDataTypes.h"
#include "FireLocalRendererComponent.generated.h"

class UFireDataSubsystem;
class UInstancedStaticMeshComponent;
class USceneComponent;

UCLASS(ClassGroup = (Fire), meta = (BlueprintSpawnableComponent))
class FIRESIMULATION2_API UFireLocalRendererComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UFireLocalRendererComponent();

	void InitializeRenderer(USceneComponent* AttachParent);
	void Configure(int32 InMaxActiveLocalFires, float InColumnHeightCm, float InColumnDiameterCm, float InHeightOffsetCm);
	void RenderLocalView(const TArray<FFireEventWithGeometry>& Events, const UFireDataSubsystem* DataSubsystem);
	void ClearLocalView();

	int32 GetLastRenderedCount() const { return LastRenderedCount; }

private:
	float ComputeLocalIntensityScore(const FFireEventAttributes& Attributes) const;

	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> LocalSmokeInstances;

	int32 MaxActiveLocalFires = 20;
	float ColumnHeightCm = 200000.0f;
	float ColumnDiameterCm = 30000.0f;
	float HeightOffsetCm = 2000.0f;
	int32 LastRenderedCount = 0;
};
