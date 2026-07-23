// this is the public api for fire data — what blueprints and other systems call into.
// it loads the catalog and index on startup, then lazy-loads geometry per year only when
// something actually queries that year. if you're trying to get fire events out of the system,
// QueryEventsByDay and QueryEventsByDateRange are your two entry points.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "FireDataTypes.h"
#include "FireDataSubsystem.generated.h"

UCLASS()
class FIRESIMULATION2_API UFireDataSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	UFUNCTION(BlueprintCallable, Category = "Fire Data")
	bool IsDataLoaded() const { return bLoaded; }

	UFUNCTION(BlueprintCallable, Category = "Fire Data")
	int32 GetEventCount() const { return EventById.Num(); }

	UFUNCTION(BlueprintCallable, Category = "Fire Data")
	void QueryEventsByDay(int32 Year, const FString& StateCode, int32 DayOfYear, TArray<FFireEventWithGeometry>& OutEvents);

	UFUNCTION(BlueprintCallable, Category = "Fire Data")
	void QueryEventsByDateRange(int32 Year, const FString& StateCode, int32 StartDayInclusive, int32 EndDayInclusive, TArray<FFireEventWithGeometry>& OutEvents);

	UFUNCTION(BlueprintCallable, Category = "Fire Data")
	FVector LonLatToLocalWorld(double Longitude, double Latitude, double HeightMeters = 0.0) const;

private:
	bool bLoaded = false;
	FString DataRootAbs;
	TMap<FString, FFireEventAttributes> EventById;
	TMap<int32, TMap<FString, TMap<int32, TArray<FString>>>> YearStateDayIndex;
	TMap<int32, TMap<FString, FFireGeometry>> GeometryByYear;
	TSet<int32> LoadedGeometryYears;

	bool LoadAllCoreData();
	bool LoadCatalog();
	bool LoadIndex();
	bool EnsureGeometryYearLoaded(int32 Year);

	static bool ReadJsonLinesFile(const FString& AbsolutePath, TArray<TSharedPtr<FJsonObject>>& OutObjects);
	static bool ReadJsonObjectFile(const FString& AbsolutePath, TSharedPtr<FJsonObject>& OutObject);
};
