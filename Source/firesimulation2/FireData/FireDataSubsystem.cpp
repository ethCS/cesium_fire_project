// this is where all the actual parsing and querying happens. here's the order things run:
//   1. initialize() fires on game start → calls LoadAllCoreData()
//   2. LoadCatalog() reads every fire event from events.ndjson into a hashmap (event id → attributes)
//   3. LoadIndex() reads the year/state/day index so queries are O(1) instead of scanning everything
//   4. when a query comes in, EnsureGeometryYearLoaded() lazy-loads that year's polygon file if needed
//   5. the query joins attributes + geometry and returns FFireEventWithGeometry structs
//   6. LonLatToLocalWorld() converts GPS coords to unreal world space (tries cesium first, falls back to math)
//
// if something isn't loading, check the paths in FireDataDeveloperSettings and look at the output log —
// there are UE_LOG calls after every major load step that tell you exactly what succeeded or failed.

#include "FireDataSubsystem.h"

#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "FireDataDeveloperSettings.h"
#include "JsonObjectConverter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "EngineUtils.h"
#include "firesimulation2.h"

namespace FireDataInternal
{
	static FString NormalizeEventId(const FString& InEventId)
	{
		return InEventId.TrimStartAndEnd().ToUpper();
	}

	static bool TryCesiumTransform(UWorld* World, const double Longitude, const double Latitude, const double HeightMeters, FVector& OutWorld)
	{
		if (!World)
		{
			return false;
		}

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor || !Actor->GetClass()->GetName().Contains(TEXT("CesiumGeoreference")))
			{
				continue;
			}

			static const TArray<FName> CandidateFunctions = {
				TEXT("TransformLongitudeLatitudeHeightToUnreal"),
				TEXT("TransformLongitudeLatitudeHeightPositionToUnreal")
			};

			for (const FName FuncName : CandidateFunctions)
			{
				if (UFunction* Fn = Actor->FindFunction(FuncName))
				{
					struct FCesiumTransformParams
					{
						FVector LongitudeLatitudeHeight;
						FVector ReturnValue;
					};

					FCesiumTransformParams Params;
					Params.LongitudeLatitudeHeight = FVector(Longitude, Latitude, HeightMeters);
					Params.ReturnValue = FVector::ZeroVector;
					Actor->ProcessEvent(Fn, &Params);
					OutWorld = Params.ReturnValue;
					return true;
				}
			}
		}

		return false;
	}

	static int32 ClampDay(int32 DayOfYear)
	{
		return FMath::Clamp(DayOfYear, 1, 366);
	}

	static double ParseNumberField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key)
	{
		double Number = 0.0;
		if (Obj->TryGetNumberField(Key, Number))
		{
			return Number;
		}

		FString Raw;
		if (Obj->TryGetStringField(Key, Raw))
		{
			return FCString::Atod(*Raw);
		}

		return 0.0;
	}

	static FString ParseStringField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key)
	{
		FString Value;
		if (Obj->TryGetStringField(Key, Value))
		{
			return Value;
		}

		double Number = 0.0;
		if (Obj->TryGetNumberField(Key, Number))
		{
			return FString::SanitizeFloat(Number);
		}

		return FString();
	}
}

void UFireDataSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	bLoaded = LoadAllCoreData();
	if (!bLoaded)
	{
		UE_LOG(LogFireSimulation2, Error, TEXT("FireDataSubsystem failed to initialize. Check generated data paths and ETL output."));
	}
}

void UFireDataSubsystem::QueryEventsByDay(const int32 Year, const FString& StateCode, const int32 DayOfYear, TArray<FFireEventWithGeometry>& OutEvents)
{
	QueryEventsByDateRange(Year, StateCode, DayOfYear, DayOfYear, OutEvents);
}

void UFireDataSubsystem::QueryEventsByDateRange(const int32 Year, const FString& StateCode, const int32 StartDayInclusive, const int32 EndDayInclusive, TArray<FFireEventWithGeometry>& OutEvents)
{
	OutEvents.Reset();
	if (!bLoaded)
	{
		return;
	}

	const FString NormalizedState = StateCode.ToUpper();
	const TMap<FString, TMap<int32, TArray<FString>>>* YearMap = YearStateDayIndex.Find(Year);
	if (!YearMap)
	{
		return;
	}

	EnsureGeometryYearLoaded(Year);
	const TMap<FString, FFireGeometry>* GeometryMap = GeometryByYear.Find(Year);

	TSet<FString> UniqueIds;
	const int32 StartDay = FireDataInternal::ClampDay(StartDayInclusive);
	const int32 EndDay = FireDataInternal::ClampDay(EndDayInclusive);

	const bool bAllStates = NormalizedState.IsEmpty() || NormalizedState.Equals(TEXT("ALL"), ESearchCase::IgnoreCase) || NormalizedState.Equals(TEXT("*"), ESearchCase::IgnoreCase);
	if (bAllStates)
	{
		for (const TPair<FString, TMap<int32, TArray<FString>>>& StatePair : *YearMap)
		{
			const TMap<int32, TArray<FString>>& DayMap = StatePair.Value;
			for (int32 Day = StartDay; Day <= EndDay; ++Day)
			{
				if (const TArray<FString>* EventIds = DayMap.Find(Day))
				{
					for (const FString& Id : *EventIds)
					{
						UniqueIds.Add(Id);
					}
				}
			}
		}
	}
	else
	{
		const TMap<int32, TArray<FString>>* StateMap = YearMap->Find(NormalizedState);
		if (!StateMap)
		{
			return;
		}

		for (int32 Day = StartDay; Day <= EndDay; ++Day)
		{
			if (const TArray<FString>* EventIds = StateMap->Find(Day))
			{
				for (const FString& Id : *EventIds)
				{
					UniqueIds.Add(Id);
				}
			}
		}
	}

	OutEvents.Reserve(UniqueIds.Num());
	int32 MissingAttrCount = 0;
	int32 MissingGeometryCount = 0;
	int32 JoinedGeometryCount = 0;
	int32 JoinedRingCount = 0;
	for (const FString& Id : UniqueIds)
	{
		const FString NormalizedId = FireDataInternal::NormalizeEventId(Id);
		const FFireEventAttributes* Attr = EventById.Find(NormalizedId);
		if (!Attr)
		{
			++MissingAttrCount;
			continue;
		}

		FFireEventWithGeometry Combined;
		Combined.Attributes = *Attr;
		if (GeometryMap)
		{
			if (const FFireGeometry* Geometry = GeometryMap->Find(NormalizedId))
			{
				Combined.Geometry = *Geometry;
				++JoinedGeometryCount;
				JoinedRingCount += Geometry->Rings.Num();
			}
			else
			{
				++MissingGeometryCount;
			}
		}
		OutEvents.Add(MoveTemp(Combined));
	}

	UE_LOG(
		LogFireSimulation2,
		Display,
		TEXT("FireDataSubsystem Query: year=%d state=%s days=%d-%d uniqueIds=%d outEvents=%d joinedGeometry=%d joinedRings=%d missingGeometry=%d missingAttributes=%d"),
		Year,
		*NormalizedState,
		StartDay,
		EndDay,
		UniqueIds.Num(),
		OutEvents.Num(),
		JoinedGeometryCount,
		JoinedRingCount,
		MissingGeometryCount,
		MissingAttrCount);
}

FVector UFireDataSubsystem::LonLatToLocalWorld(const double Longitude, const double Latitude, const double HeightMeters) const
{
	FVector CesiumWorld;
	if (FireDataInternal::TryCesiumTransform(GetWorld(), Longitude, Latitude, HeightMeters, CesiumWorld))
	{
		return CesiumWorld;
	}

	const UFireDataDeveloperSettings* Settings = GetDefault<UFireDataDeveloperSettings>();
	const double OriginLatRad = FMath::DegreesToRadians(Settings->OriginLatitude);
	const double LatRad = FMath::DegreesToRadians(Latitude);
	const double LonDeltaRad = FMath::DegreesToRadians(Longitude - Settings->OriginLongitude);
	const double LatDeltaRad = LatRad - OriginLatRad;

	constexpr double EarthRadiusMeters = 6378137.0;
	const double EastMeters = EarthRadiusMeters * LonDeltaRad * FMath::Cos(OriginLatRad);
	const double NorthMeters = EarthRadiusMeters * LatDeltaRad;

	const double Scale = Settings->MetersPerUnrealUnit <= 0.0 ? 1.0 : Settings->MetersPerUnrealUnit;
	return FVector(NorthMeters / Scale, EastMeters / Scale, HeightMeters / Scale);
}

bool UFireDataSubsystem::LoadAllCoreData()
{
	const UFireDataDeveloperSettings* Settings = GetDefault<UFireDataDeveloperSettings>();
	DataRootAbs = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Settings->GeneratedDataRoot.Path);

	return LoadCatalog() && LoadIndex();
}

bool UFireDataSubsystem::LoadCatalog()
{
	const UFireDataDeveloperSettings* Settings = GetDefault<UFireDataDeveloperSettings>();
	const FString CatalogPath = FPaths::Combine(DataRootAbs, Settings->CatalogRelativePath);

	TArray<TSharedPtr<FJsonObject>> Rows;
	if (!ReadJsonLinesFile(CatalogPath, Rows))
	{
		UE_LOG(LogFireSimulation2, Error, TEXT("Failed to read catalog NDJSON: %s"), *CatalogPath);
		return false;
	}

	EventById.Reset();
	for (const TSharedPtr<FJsonObject>& Row : Rows)
	{
		FFireEventAttributes Event;
		Event.EventId = FireDataInternal::ParseStringField(Row, TEXT("event_id")).TrimStartAndEnd();
		Event.Name = FireDataInternal::ParseStringField(Row, TEXT("name"));
		Event.IncidentType = FireDataInternal::ParseStringField(Row, TEXT("incident_type"));
		Event.Program = FireDataInternal::ParseStringField(Row, TEXT("map_prog"));
		Event.AssessmentType = FireDataInternal::ParseStringField(Row, TEXT("asmnt_type"));
		Event.StateCode = FireDataInternal::ParseStringField(Row, TEXT("state"));
		Event.DateIso = FireDataInternal::ParseStringField(Row, TEXT("ig_date"));
		Event.Year = static_cast<int32>(FireDataInternal::ParseNumberField(Row, TEXT("year")));
		Event.DayOfYear = static_cast<int32>(FireDataInternal::ParseNumberField(Row, TEXT("day_of_year")));
		Event.Acres = FireDataInternal::ParseNumberField(Row, TEXT("burnbndac"));
		Event.Latitude = FireDataInternal::ParseNumberField(Row, TEXT("burnbndlat"));
		Event.Longitude = FireDataInternal::ParseNumberField(Row, TEXT("burnbndlon"));
		Event.LowThreshold = FireDataInternal::ParseNumberField(Row, TEXT("low_t"));
		Event.ModerateThreshold = FireDataInternal::ParseNumberField(Row, TEXT("mod_t"));
		Event.HighThreshold = FireDataInternal::ParseNumberField(Row, TEXT("high_t"));
		Event.NodataThreshold = FireDataInternal::ParseNumberField(Row, TEXT("nodata_t"));
		Event.GreennessThreshold = FireDataInternal::ParseNumberField(Row, TEXT("greenness_t"));
		Event.DnbrVal = FireDataInternal::ParseNumberField(Row, TEXT("dnbr_val"));
		Event.DnbrStddev = FireDataInternal::ParseNumberField(Row, TEXT("dnbr_stddev"));
		Event.PreNbrVal = FireDataInternal::ParseNumberField(Row, TEXT("prenbr_val"));
		Event.PosNbrVal = FireDataInternal::ParseNumberField(Row, TEXT("posnbr_val"));
		Event.Cbi = FireDataInternal::ParseNumberField(Row, TEXT("cbi"));
		Event.IrwinId = FireDataInternal::ParseStringField(Row, TEXT("irwinid"));
		Event.PreImageId = FireDataInternal::ParseStringField(Row, TEXT("pre_id"));
		Event.PostImageId = FireDataInternal::ParseStringField(Row, TEXT("post_id"));
		const FString NormalizedEventId = FireDataInternal::NormalizeEventId(Event.EventId);
		EventById.Add(NormalizedEventId, MoveTemp(Event));
	}

	UE_LOG(LogFireSimulation2, Log, TEXT("Loaded %d fire events from catalog."), EventById.Num());
	return EventById.Num() > 0;
}

bool UFireDataSubsystem::LoadIndex()
{
	const UFireDataDeveloperSettings* Settings = GetDefault<UFireDataDeveloperSettings>();
	const FString IndexPath = FPaths::Combine(DataRootAbs, Settings->IndexRelativePath);

	TSharedPtr<FJsonObject> Root;
	if (!ReadJsonObjectFile(IndexPath, Root))
	{
		UE_LOG(LogFireSimulation2, Error, TEXT("Failed to read index JSON: %s"), *IndexPath);
		return false;
	}

	YearStateDayIndex.Reset();
	for (const TPair<FString, TSharedPtr<FJsonValue>>& YearPair : Root->Values)
	{
		const int32 Year = FCString::Atoi(*YearPair.Key);
		const TSharedPtr<FJsonObject>* StateObj;
		if (!YearPair.Value->TryGetObject(StateObj))
		{
			continue;
		}

		TMap<FString, TMap<int32, TArray<FString>>> StateMap;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& StatePair : (*StateObj)->Values)
		{
			const FString State = StatePair.Key.ToUpper();
			const TSharedPtr<FJsonObject>* DayObj;
			if (!StatePair.Value->TryGetObject(DayObj))
			{
				continue;
			}

			TMap<int32, TArray<FString>> DayMap;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& DayPair : (*DayObj)->Values)
			{
				const int32 Day = FCString::Atoi(*DayPair.Key);
				const TArray<TSharedPtr<FJsonValue>>* JsonIds;
				if (!DayPair.Value->TryGetArray(JsonIds))
				{
					continue;
				}

				TArray<FString> Ids;
				Ids.Reserve(JsonIds->Num());
				for (const TSharedPtr<FJsonValue>& IdValue : *JsonIds)
				{
					Ids.Add(FireDataInternal::NormalizeEventId(IdValue->AsString()));
				}
				DayMap.Add(Day, MoveTemp(Ids));
			}

			StateMap.Add(State, MoveTemp(DayMap));
		}

		YearStateDayIndex.Add(Year, MoveTemp(StateMap));
	}

	UE_LOG(LogFireSimulation2, Log, TEXT("Loaded index for %d years."), YearStateDayIndex.Num());
	return YearStateDayIndex.Num() > 0;
}

bool UFireDataSubsystem::EnsureGeometryYearLoaded(const int32 Year)
{
	if (LoadedGeometryYears.Contains(Year))
	{
		return true;
	}

	const UFireDataDeveloperSettings* Settings = GetDefault<UFireDataDeveloperSettings>();
	const FString GeometryPath = FPaths::Combine(DataRootAbs, Settings->GeometryFolderRelativePath, FString::FromInt(Year) + TEXT(".ndjson"));

	TArray<TSharedPtr<FJsonObject>> Rows;
	if (!ReadJsonLinesFile(GeometryPath, Rows))
	{
		UE_LOG(LogFireSimulation2, Warning, TEXT("No geometry file for year %d at %s"), Year, *GeometryPath);
		LoadedGeometryYears.Add(Year);
		return false;
	}

	TMap<FString, FFireGeometry> YearMap;
	for (const TSharedPtr<FJsonObject>& Row : Rows)
	{
		FFireGeometry Geometry;
		Geometry.EventId = FireDataInternal::NormalizeEventId(Row->GetStringField(TEXT("event_id")));

		const TArray<TSharedPtr<FJsonValue>>* RingsJson = nullptr;
		if (Row->TryGetArrayField(TEXT("rings"), RingsJson))
		{
			for (const TSharedPtr<FJsonValue>& RingValue : *RingsJson)
			{
				const TArray<TSharedPtr<FJsonValue>>* VerticesJson = nullptr;
				if (!RingValue->TryGetArray(VerticesJson))
				{
					continue;
				}

				FFireRing Ring;
				Ring.LonLatVertices.Reserve(VerticesJson->Num());
				for (const TSharedPtr<FJsonValue>& VertexValue : *VerticesJson)
				{
					const TArray<TSharedPtr<FJsonValue>>* PairJson = nullptr;
					if (!VertexValue->TryGetArray(PairJson) || PairJson->Num() < 2)
					{
						continue;
					}

					const double Lon = (*PairJson)[0]->AsNumber();
					const double Lat = (*PairJson)[1]->AsNumber();
					Ring.LonLatVertices.Add(FVector2D(Lon, Lat));
				}

				if (Ring.LonLatVertices.Num() >= 3)
				{
					Geometry.Rings.Add(MoveTemp(Ring));
				}
			}
		}

		YearMap.Add(Geometry.EventId, MoveTemp(Geometry));
	}

	GeometryByYear.Add(Year, MoveTemp(YearMap));
	LoadedGeometryYears.Add(Year);
	UE_LOG(LogFireSimulation2, Log, TEXT("Loaded geometry for year %d."), Year);
	return true;
}

bool UFireDataSubsystem::ReadJsonLinesFile(const FString& AbsolutePath, TArray<TSharedPtr<FJsonObject>>& OutObjects)
{
	OutObjects.Reset();
	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *AbsolutePath))
	{
		return false;
	}

	TArray<FString> Lines;
	Content.ParseIntoArrayLines(Lines, true);
	OutObjects.Reserve(Lines.Num());
	for (const FString& Line : Lines)
	{
		if (Line.IsEmpty())
		{
			continue;
		}

		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
		TSharedPtr<FJsonObject> Obj;
		if (FJsonSerializer::Deserialize(Reader, Obj) && Obj.IsValid())
		{
			OutObjects.Add(Obj);
		}
	}

	return true;
}

bool UFireDataSubsystem::ReadJsonObjectFile(const FString& AbsolutePath, TSharedPtr<FJsonObject>& OutObject)
{
	OutObject.Reset();
	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *AbsolutePath))
	{
		return false;
	}

	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Content);
	return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
}
