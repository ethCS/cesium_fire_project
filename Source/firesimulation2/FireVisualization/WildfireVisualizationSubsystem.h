#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "FireDataTypes.h"
#include "WildfireVisualizationSubsystem.generated.h"

class UFireNationalRendererComponent;
class UFireRegionalRendererComponent;
class UFireLocalRendererComponent;
class UFireNiagaraRendererComponent;
class UFireVDBRendererComponent;

UCLASS()
class FIRESIMULATION2_API UWildfireVisualizationSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

	UFUNCTION(BlueprintCallable, Category = "Wildfire Visualization")
	void SetNationalRenderer(UFireNationalRendererComponent* InRenderer);

	UFUNCTION(BlueprintCallable, Category = "Wildfire Visualization")
	void SetRegionalRenderer(UFireRegionalRendererComponent* InRenderer);

	UFUNCTION(BlueprintCallable, Category = "Wildfire Visualization")
	void SetLocalRenderer(UFireLocalRendererComponent* InRenderer);

	UFUNCTION(BlueprintCallable, Category = "Wildfire Visualization")
	void SetNiagaraRenderer(UFireNiagaraRendererComponent* InRenderer);

	UFUNCTION(BlueprintCallable, Category = "Wildfire Visualization")
	void SetVDBRenderer(UFireVDBRendererComponent* InRenderer);

	UFUNCTION(BlueprintCallable, Category = "Wildfire Visualization")
	bool SetFilters(int32 InYear, const FString& InStateCode, int32 InStartDayInclusive, int32 InEndDayInclusive);

	UFUNCTION(BlueprintCallable, Category = "Wildfire Visualization")
	bool RefreshView();

	/**
	 * Re-runs close-range fire effect placement (Niagara and/or VDB) using the last queried
	 * event set without clearing map-level visualization or moving the camera.
	 */
	UFUNCTION(BlueprintCallable, Category = "Wildfire Visualization")
	void RefreshLocalFireEffectsOnly();

	/**
	 * Re-runs only VDB fire placement using the last queried event set.
	 * Does NOT call ClearVisualization or ReturnToOverviewView, so it is safe
	 * to call when the camera is already at fire level (e.g. after auto-descent).
	 */
	UFUNCTION(BlueprintCallable, Category = "Wildfire Visualization")
	void RefreshVDBOnly();

	/**
	 * Regional meshes are dataset-driven and cached; this call only restores
	 * regional visibility without rebuilding procedural sections.
	 */
	UFUNCTION(BlueprintCallable, Category = "Wildfire Visualization")
	void RefreshRegionalOnly();

	UFUNCTION(BlueprintCallable, Category = "Wildfire Visualization")
	bool RefreshNationalView();

	UFUNCTION(BlueprintCallable, Category = "Wildfire Visualization")
	int32 GetRenderedEventCount() const;

	UFUNCTION(BlueprintCallable, Category = "Wildfire Visualization|Debug")
	void LogRegionalDebugStats() const;

	UFUNCTION(BlueprintCallable, Category = "Wildfire Visualization|Debug")
	bool DebugDrawPerimeters(float DurationSeconds = 20.0f, float SphereRadiusCm = 3000.0f) const;

private:
	UPROPERTY(Transient)
	TObjectPtr<UFireNationalRendererComponent> NationalRenderer;

	UPROPERTY(Transient)
	TObjectPtr<UFireRegionalRendererComponent> RegionalRenderer;

	UPROPERTY(Transient)
	TObjectPtr<UFireLocalRendererComponent> LocalRenderer;

	UPROPERTY(Transient)
	TObjectPtr<UFireNiagaraRendererComponent> NiagaraRenderer;

	UPROPERTY(Transient)
	TObjectPtr<UFireVDBRendererComponent> VDBRenderer;

	UPROPERTY(Transient)
	TArray<FFireEventWithGeometry> LastQueriedEvents;

	UPROPERTY(Transient)
	TArray<FFireVisualState> LastVisualStates;

	float NationalLodMinDistanceKm = 200.0f;
	float RegionalLodMinDistanceKm = 20.0f;
	float LocalLodMinDistanceKm = 2.0f;

	int32 MaxUpdatesPerFrame = 200;
	int32 MaxNationalMarkers = 10000;
	int32 MaxRegionalMeshes = 500;
	int32 MaxLocalFires = 20;
	int32 MaxCloseFires = 1;

	int32 Year = 2020;
	FString StateCode = TEXT("ALL");
	int32 StartDayInclusive = 1;
	int32 EndDayInclusive = 365;
};
