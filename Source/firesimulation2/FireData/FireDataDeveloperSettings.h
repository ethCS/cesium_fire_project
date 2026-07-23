// config for the fire data system. shows up in unreal editor under project settings → project → fire simulation data.
// the paths here tell the subsystem where to find the generated data folders (catalog, index, geometry).
// the transform settings (OriginLatitude, OriginLongitude, MetersPerUnrealUnit) are used as the fallback
// coordinate conversion when cesium isn't in the scene. if fires are appearing in the wrong place on the map,
// this is the first place to look.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "FireDataDeveloperSettings.generated.h"

UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Fire Simulation Data"))
class FIRESIMULATION2_API UFireDataDeveloperSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UFireDataDeveloperSettings();

	virtual FName GetCategoryName() const override { return TEXT("Project"); }

	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Paths")
	FDirectoryPath GeneratedDataRoot;

	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Paths")
	FString CatalogRelativePath;

	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Paths")
	FString IndexRelativePath;

	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Paths")
	FString GeometryFolderRelativePath;

	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Transform")
	double OriginLatitude;

	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Transform")
	double OriginLongitude;

	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Transform")
	double MetersPerUnrealUnit;
};

