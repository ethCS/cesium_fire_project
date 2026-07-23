#include "WildfireVisualizationSubsystem.h"

#include "Algo/Sort.h"
#include "Camera/PlayerCameraManager.h"
#include "FireDataSubsystem.h"
#include "FireLocalRendererComponent.h"
#include "FireNationalRendererComponent.h"
#include "FireNiagaraRendererComponent.h"
#include "FireRegionalRendererComponent.h"
#include "FireVDBRendererComponent.h"
#include "GameFramework/PlayerController.h"
#include "firesimulation2.h"

namespace WildfireVisualizationInternal
{
	static float ComputeSeverityScore(const FFireEventAttributes& Attributes)
	{
		const float AcresScore = static_cast<float>(FMath::Clamp(FMath::LogX(10.0, FMath::Max(1.0, Attributes.Acres)), 0.0, 8.0));
		const float High = static_cast<float>(FMath::Max(1.0, Attributes.HighThreshold));
		const float Moderate = static_cast<float>(FMath::Max(1.0, Attributes.ModerateThreshold));
		const float ThresholdBlend = static_cast<float>(FMath::Clamp((Attributes.Acres / High) + (Attributes.Acres / Moderate), 0.0, 4.0));
		return AcresScore + ThresholdBlend;
	}
}

bool UWildfireVisualizationSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UWildfireVisualizationSubsystem::SetNationalRenderer(UFireNationalRendererComponent* InRenderer)
{
	NationalRenderer = InRenderer;
}

void UWildfireVisualizationSubsystem::SetRegionalRenderer(UFireRegionalRendererComponent* InRenderer)
{
	RegionalRenderer = InRenderer;
}

void UWildfireVisualizationSubsystem::SetLocalRenderer(UFireLocalRendererComponent* InRenderer)
{
	LocalRenderer = InRenderer;
}

void UWildfireVisualizationSubsystem::SetNiagaraRenderer(UFireNiagaraRendererComponent* InRenderer)
{
	NiagaraRenderer = InRenderer;
}

void UWildfireVisualizationSubsystem::SetVDBRenderer(UFireVDBRendererComponent* InRenderer)
{
	VDBRenderer = InRenderer;
}

bool UWildfireVisualizationSubsystem::SetFilters(const int32 InYear, const FString& InStateCode, const int32 InStartDayInclusive, const int32 InEndDayInclusive)
{
	if (InYear <= 0 || InStartDayInclusive < 1 || InStartDayInclusive > 366 || InEndDayInclusive < 1 || InEndDayInclusive > 366 || InStartDayInclusive > InEndDayInclusive)
	{
		return false;
	}

	const FString TrimmedStateCode = InStateCode.TrimStartAndEnd().ToUpper();
	if (TrimmedStateCode.IsEmpty())
	{
		return false;
	}

	Year = InYear;
	StateCode = TrimmedStateCode;
	StartDayInclusive = InStartDayInclusive;
	EndDayInclusive = InEndDayInclusive;
	return true;
}

bool UWildfireVisualizationSubsystem::RefreshView()
{
	const double RefreshStart = FPlatformTime::Seconds();
	UWorld* World = GetWorld();
	if (!World || !NationalRenderer)
	{
		return false;
	}

	UGameInstance* GameInstance = World->GetGameInstance();
	if (!GameInstance)
	{
		return false;
	}

	UFireDataSubsystem* FireDataSubsystem = GameInstance->GetSubsystem<UFireDataSubsystem>();
	if (!FireDataSubsystem || !FireDataSubsystem->IsDataLoaded())
	{
		UE_LOG(LogFireSimulation2, Warning, TEXT("WildfireVisualizationSubsystem: FireDataSubsystem unavailable or not loaded."));
		NationalRenderer->ClearNationalView();
		if (RegionalRenderer)
		{
			RegionalRenderer->ClearRegionalView();
		}
		if (LocalRenderer)
		{
			LocalRenderer->ClearLocalView();
		}
		if (NiagaraRenderer)
		{
			NiagaraRenderer->ClearFireNiagara();
		}
		if (VDBRenderer)
		{
			VDBRenderer->ClearFireVDB();
		}
		LastQueriedEvents.Reset();
		LastVisualStates.Reset();
		return false;
	}

	FVector CameraLocation = FVector::ZeroVector;
	bool bHasCamera = false;
	if (APlayerController* PC = World->GetFirstPlayerController())
	{
		if (APlayerCameraManager* CameraManager = PC->PlayerCameraManager)
		{
			CameraLocation = CameraManager->GetCameraLocation();
			bHasCamera = true;
		}
	}

	TArray<FFireEventWithGeometry> Events;
	const double QueryStart = FPlatformTime::Seconds();
	FireDataSubsystem->QueryEventsByDateRange(Year, StateCode, StartDayInclusive, EndDayInclusive, Events);
	const double QueryMs = (FPlatformTime::Seconds() - QueryStart) * 1000.0;
	LastQueriedEvents = Events;

	TArray<FFireEventWithGeometry> NationalEvents;
	TArray<FFireEventWithGeometry> RegionalEvents;
	TArray<FFireEventWithGeometry> LocalEvents;

	const int32 NationalRenderCount = FMath::Min(Events.Num(), MaxNationalMarkers);
	NationalEvents.Reserve(NationalRenderCount);
	for (int32 i = 0; i < NationalRenderCount; ++i)
	{
		NationalEvents.Add(Events[i]);
	}

	RegionalEvents.Reserve(FMath::Min(Events.Num(), MaxRegionalMeshes));
	LocalEvents.Reserve(FMath::Min(Events.Num(), MaxLocalFires));
	LastVisualStates.Reset();
	LastVisualStates.Reserve(FMath::Min(Events.Num(), MaxUpdatesPerFrame));

	int32 NationalCount = 0;
	int32 RegionalCount = 0;
	int32 LocalCount = 0;
	int32 CloseCount = 0;
	int32 RenderedCloseCount = 0;

	const int32 ProcessCount = FMath::Min(Events.Num(), MaxUpdatesPerFrame);
	for (int32 i = 0; i < ProcessCount; ++i)
	{
		const FFireEventWithGeometry& Event = Events[i];
		const FVector WorldLocation = FireDataSubsystem->LonLatToLocalWorld(Event.Attributes.Longitude, Event.Attributes.Latitude, 0.0);
		const double DistanceCm = bHasCamera ? FVector::Dist(WorldLocation, CameraLocation) : TNumericLimits<double>::Max();
		const float DistanceKm = static_cast<float>(DistanceCm / 100000.0);

		EFireViewLOD LOD = EFireViewLOD::National;
		if (DistanceKm < LocalLodMinDistanceKm)
		{
			LOD = EFireViewLOD::Close;
			if (RenderedCloseCount >= MaxCloseFires)
			{
				LOD = EFireViewLOD::Local;
			}
			else
			{
				++RenderedCloseCount;
			}
		}
		else if (DistanceKm < RegionalLodMinDistanceKm)
		{
			LOD = EFireViewLOD::Local;
		}
		else if (DistanceKm < NationalLodMinDistanceKm)
		{
			LOD = EFireViewLOD::Regional;
		}

		const bool bCanRegional = Event.Geometry.Rings.Num() > 0;
		const bool bRenderNational = true;
		const bool bRenderRegional = bCanRegional;
		const bool bRenderLocal = (LOD == EFireViewLOD::Local || LOD == EFireViewLOD::Close);

		FFireVisualState State;
		State.EventId = Event.Attributes.EventId;
		State.WorldLocation = WorldLocation;
		State.SeverityScore = WildfireVisualizationInternal::ComputeSeverityScore(Event.Attributes);
		State.LOD = LOD;
		State.bRenderNational = bRenderNational;
		State.bRenderRegional = bRenderRegional;
		State.bRenderLocal = bRenderLocal;
		LastVisualStates.Add(MoveTemp(State));

		switch (LOD)
		{
		case EFireViewLOD::National: ++NationalCount; break;
		case EFireViewLOD::Regional: ++RegionalCount; break;
		case EFireViewLOD::Local: ++LocalCount; break;
		case EFireViewLOD::Close: ++CloseCount; break;
		default: break;
		}
	}

	// Regional overlays (perimeter/heatmap) should persist independently of camera-distance LOD.
	RegionalEvents.Reset();
	if (RegionalRenderer)
	{
		const bool bAllStates = StateCode.IsEmpty() || StateCode.Equals(TEXT("ALL"), ESearchCase::IgnoreCase) || StateCode.Equals(TEXT("*"), ESearchCase::IgnoreCase);
		if (bAllStates)
		{
			TMap<FString, TArray<const FFireEventWithGeometry*>> RegionalEventsByState;
			for (const FFireEventWithGeometry& Event : Events)
			{
				if (Event.Geometry.Rings.Num() == 0)
				{
					continue;
				}

				FString EventState = Event.Attributes.StateCode.TrimStartAndEnd().ToUpper();
				if (EventState.IsEmpty())
				{
					EventState = TEXT("UNK");
				}
				RegionalEventsByState.FindOrAdd(EventState).Add(&Event);
			}

			for (TPair<FString, TArray<const FFireEventWithGeometry*>>& Pair : RegionalEventsByState)
			{
				Pair.Value.Sort([](const FFireEventWithGeometry& A, const FFireEventWithGeometry& B)
				{
					return A.Attributes.Acres > B.Attributes.Acres;
				});
			}

			TArray<FString> StateKeys;
			RegionalEventsByState.GetKeys(StateKeys);
			StateKeys.Sort();

			TMap<FString, int32> StateCursor;
			for (const FString& StateKey : StateKeys)
			{
				StateCursor.Add(StateKey, 0);
			}

			bool bAddedAny = true;
			while (RegionalEvents.Num() < MaxRegionalMeshes && bAddedAny)
			{
				bAddedAny = false;
				for (const FString& StateKey : StateKeys)
				{
					int32& Cursor = StateCursor.FindChecked(StateKey);
					const TArray<const FFireEventWithGeometry*>& StateEvents = RegionalEventsByState.FindChecked(StateKey);
					if (!StateEvents.IsValidIndex(Cursor))
					{
						continue;
					}

					RegionalEvents.Add(*StateEvents[Cursor]);
					++Cursor;
					bAddedAny = true;
					if (RegionalEvents.Num() >= MaxRegionalMeshes)
					{
						break;
					}
				}
			}
		}
		else
		{
			for (const FFireEventWithGeometry& Event : Events)
			{
				if (Event.Geometry.Rings.Num() == 0)
				{
					continue;
				}

				RegionalEvents.Add(Event);
				if (RegionalEvents.Num() >= MaxRegionalMeshes)
				{
					break;
				}
			}
		}
	}

	// Keep local budget tight; prefer higher severity near-camera fires.
	LastVisualStates.Sort([](const FFireVisualState& A, const FFireVisualState& B)
	{
		return A.SeverityScore > B.SeverityScore;
	});

	for (const FFireVisualState& State : LastVisualStates)
	{
		if (!State.bRenderLocal || LocalEvents.Num() >= MaxLocalFires)
		{
			continue;
		}

		const FFireEventWithGeometry* Match = Events.FindByPredicate([&State](const FFireEventWithGeometry& Event)
		{
			return Event.Attributes.EventId.Equals(State.EventId, ESearchCase::IgnoreCase);
		});
		if (Match)
		{
			LocalEvents.Add(*Match);
		}
	}

	NationalRenderer->RenderNationalView(NationalEvents, FireDataSubsystem);
	const double RegionalStart = FPlatformTime::Seconds();
	if (RegionalRenderer)
	{
		// Use Cesium height sampling up-front so regional heatmaps project onto streamed terrain
		// instead of relying on temporary high-elevation fallback geometry.
		RegionalRenderer->RenderRegionalViewWithCesiumHeights(RegionalEvents, FireDataSubsystem);
	}
	const double RegionalDispatchMs = (FPlatformTime::Seconds() - RegionalStart) * 1000.0;
	const double LocalStart = FPlatformTime::Seconds();
	if (LocalRenderer)
	{
		LocalRenderer->RenderLocalView(LocalEvents, FireDataSubsystem);
	}
	// Niagara fire effects (legacy — keep disabled when using VDB).
	if (NiagaraRenderer)
	{
		NiagaraRenderer->RenderFireNiagara(RegionalEvents, FireDataSubsystem, CameraLocation);
	}
	// VDB volumetric fire actors placed inside heatmap polygons near the camera.
	if (VDBRenderer)
	{
		VDBRenderer->RenderFireVDB(RegionalEvents, FireDataSubsystem, CameraLocation);
	}
	const double LocalAndFxMs = (FPlatformTime::Seconds() - LocalStart) * 1000.0;

	int32 EventsWithAnyRings = 0;
	int32 EventsWithRenderableRings = 0;
	int32 TotalRings = 0;
	int32 RenderableRings = 0;
	for (const FFireEventWithGeometry& Event : Events)
	{
		const int32 RingCount = Event.Geometry.Rings.Num();
		if (RingCount > 0)
		{
			++EventsWithAnyRings;
			TotalRings += RingCount;
		}

		int32 ValidInEvent = 0;
		for (const FFireRing& Ring : Event.Geometry.Rings)
		{
			if (Ring.LonLatVertices.Num() >= 3)
			{
				++ValidInEvent;
			}
		}
		if (ValidInEvent > 0)
		{
			++EventsWithRenderableRings;
			RenderableRings += ValidInEvent;
		}
	}

	UE_LOG(
		LogFireSimulation2,
		Display,
		TEXT("WildfireVisualizationSubsystem: queried=%d processed=%d national=%d regional=%d local=%d close=%d renderedNational=%d renderedRegional=%d renderedLocal=%d rings=%d renderableRings=%d camera=%s queryMs=%.2f regionalDispatchMs=%.2f localFxMs=%.2f totalRefreshMs=%.2f"),
		Events.Num(),
		ProcessCount,
		NationalCount,
		RegionalCount,
		LocalCount,
		CloseCount,
		NationalEvents.Num(),
		RegionalEvents.Num(),
		LocalEvents.Num(),
		TotalRings,
		RenderableRings,
		*CameraLocation.ToString(),
		QueryMs,
		RegionalDispatchMs,
		LocalAndFxMs,
		(FPlatformTime::Seconds() - RefreshStart) * 1000.0);

	return true;
}

bool UWildfireVisualizationSubsystem::RefreshNationalView()
{
	return RefreshView();
}

void UWildfireVisualizationSubsystem::RefreshLocalFireEffectsOnly()
{
	if (!GetWorld())
	{
		return;
	}

	UGameInstance* GameInstance = GetWorld()->GetGameInstance();
	UFireDataSubsystem* FireDataSubsystem = GameInstance
		? GameInstance->GetSubsystem<UFireDataSubsystem>()
		: nullptr;

	if (!FireDataSubsystem || !FireDataSubsystem->IsDataLoaded())
	{
		return;
	}

	FVector CameraLocation = FVector::ZeroVector;
	if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		if (APlayerCameraManager* CM = PC->PlayerCameraManager)
		{
			CameraLocation = CM->GetCameraLocation();
		}
	}

	// Keep fire-effect refresh local to the focused scene so global datasets do not
	// consume the fire-instance budget while distant tiles are still unloaded.
	constexpr float MaxDistanceCm = 5000000.0f; // 50 km
	TArray<FFireEventWithGeometry> NearbyEvents;
	NearbyEvents.Reserve(LastQueriedEvents.Num());
	for (const FFireEventWithGeometry& Event : LastQueriedEvents)
	{
		if (Event.Geometry.Rings.Num() == 0)
		{
			continue;
		}

		const FVector EventWorld = FireDataSubsystem->LonLatToLocalWorld(
			Event.Attributes.Longitude, Event.Attributes.Latitude, 0.0);
		if (FVector::Dist(EventWorld, CameraLocation) <= MaxDistanceCm)
		{
			NearbyEvents.Add(Event);
		}
	}

	if (NiagaraRenderer)
	{
		NiagaraRenderer->RenderFireNiagara(NearbyEvents, FireDataSubsystem, CameraLocation);
	}

	if (VDBRenderer)
	{
		VDBRenderer->RenderFireVDB(NearbyEvents, FireDataSubsystem, CameraLocation);
	}

	UE_LOG(LogFireSimulation2, Display,
		TEXT("WildfireVisualizationSubsystem: RefreshLocalFireEffectsOnly — nearbyEvents=%d / totalQueried=%d"),
		NearbyEvents.Num(), LastQueriedEvents.Num());
}

void UWildfireVisualizationSubsystem::RefreshVDBOnly()
{
	if (!VDBRenderer || !GetWorld())
	{
		return;
	}

	UGameInstance* GameInstance = GetWorld()->GetGameInstance();
	UFireDataSubsystem* FireDataSubsystem = GameInstance
		? GameInstance->GetSubsystem<UFireDataSubsystem>()
		: nullptr;

	if (!FireDataSubsystem || !FireDataSubsystem->IsDataLoaded())
	{
		return;
	}

	FVector CameraLocation = FVector::ZeroVector;
	if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		if (APlayerCameraManager* CM = PC->PlayerCameraManager)
		{
			CameraLocation = CM->GetCameraLocation();
		}
	}

	// Only include events within 50 km of the camera.
	// The camera is at fire-focus altitude after auto-descent — Cesium tiles are
	// only guaranteed loaded near the camera.  Passing all 800+ events causes the
	// instance budget to be consumed by distant fires (Alaska, etc.) whose tiles
	// aren't loaded, filling the budget with underground actors before ever
	// reaching the fire the user is actually looking at.
	constexpr float MaxDistanceCm = 5000000.0f; // 50 km
	TArray<FFireEventWithGeometry> NearbyEvents;
	for (const FFireEventWithGeometry& Event : LastQueriedEvents)
	{
		if (Event.Geometry.Rings.Num() == 0)
		{
			continue;
		}
		const FVector EventWorld = FireDataSubsystem->LonLatToLocalWorld(
			Event.Attributes.Longitude, Event.Attributes.Latitude, 0.0);
		if (FVector::Dist(EventWorld, CameraLocation) <= MaxDistanceCm)
		{
			NearbyEvents.Add(Event);
		}
	}

	VDBRenderer->ClearFireVDB();
	VDBRenderer->RenderFireVDB(NearbyEvents, FireDataSubsystem, CameraLocation);

	UE_LOG(LogFireSimulation2, Display,
		TEXT("WildfireVisualizationSubsystem: RefreshVDBOnly — nearbyEvents=%d / totalQueried=%d"),
		NearbyEvents.Num(), LastQueriedEvents.Num());
}

void UWildfireVisualizationSubsystem::RefreshRegionalOnly()
{
	if (!RegionalRenderer)
	{
		return;
	}
	// Architecture rule: regional meshes are dataset-driven and cached.
	// Camera/view transitions must not trigger regional mesh rebuilds.
	RegionalRenderer->SetHeatmapVisible(true);
}

int32 UWildfireVisualizationSubsystem::GetRenderedEventCount() const
{
	return NationalRenderer ? NationalRenderer->GetEventCount() : 0;
}

void UWildfireVisualizationSubsystem::LogRegionalDebugStats() const
{
	int32 EventsWithAnyRings = 0;
	int32 EventsWithRenderableRings = 0;
	int32 TotalRings = 0;
	int32 RenderableRings = 0;
	for (const FFireEventWithGeometry& Event : LastQueriedEvents)
	{
		if (Event.Geometry.Rings.Num() > 0)
		{
			++EventsWithAnyRings;
			TotalRings += Event.Geometry.Rings.Num();
		}

		int32 ValidInEvent = 0;
		for (const FFireRing& Ring : Event.Geometry.Rings)
		{
			if (Ring.LonLatVertices.Num() >= 3)
			{
				++ValidInEvent;
			}
		}
		if (ValidInEvent > 0)
		{
			++EventsWithRenderableRings;
			RenderableRings += ValidInEvent;
		}
	}

	UE_LOG(
		LogFireSimulation2,
		Display,
		TEXT("WildfireVisualizationSubsystem Debug: lastQueried=%d visualStates=%d rings=%d renderableRings=%d"),
		LastQueriedEvents.Num(),
		LastVisualStates.Num(),
		TotalRings,
		RenderableRings);

	if (RegionalRenderer)
	{
		RegionalRenderer->LogRegionalRenderStats();
	}
	if (LocalRenderer)
	{
		UE_LOG(LogFireSimulation2, Display, TEXT("LocalRenderer: renderedLocal=%d"), LocalRenderer->GetLastRenderedCount());
	}
}

bool UWildfireVisualizationSubsystem::DebugDrawPerimeters(const float DurationSeconds, const float SphereRadiusCm) const
{
	UWorld* World = GetWorld();
	if (!World || !RegionalRenderer || LastQueriedEvents.Num() == 0)
	{
		UE_LOG(LogFireSimulation2, Warning, TEXT("WildfireVisualizationSubsystem DebugDrawPerimeters: no world/renderer/events available."));
		return false;
	}

	UGameInstance* GameInstance = World->GetGameInstance();
	if (!GameInstance)
	{
		return false;
	}

	UFireDataSubsystem* FireDataSubsystem = GameInstance->GetSubsystem<UFireDataSubsystem>();
	if (!FireDataSubsystem || !FireDataSubsystem->IsDataLoaded())
	{
		UE_LOG(LogFireSimulation2, Warning, TEXT("WildfireVisualizationSubsystem DebugDrawPerimeters: FireDataSubsystem unavailable."));
		return false;
	}

	RegionalRenderer->DrawPerimeterDebug(LastQueriedEvents, FireDataSubsystem, DurationSeconds, SphereRadiusCm);
	return true;
}
