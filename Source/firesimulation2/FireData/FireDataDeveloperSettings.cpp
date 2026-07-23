#include "FireDataDeveloperSettings.h"

UFireDataDeveloperSettings::UFireDataDeveloperSettings()
{
	GeneratedDataRoot.Path = TEXT("FireData/Generated");
	CatalogRelativePath = TEXT("catalog/events.ndjson");
	IndexRelativePath = TEXT("index/year_state_day_index.json");
	GeometryFolderRelativePath = TEXT("geometry");
	OriginLatitude = 39.8283;   // Approx geographic center of CONUS.
	OriginLongitude = -98.5795;
	MetersPerUnrealUnit = 1.0;  // 1 uu == 1 m
}
