#include "FireNationalRendererComponent.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "FireDataSubsystem.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"

namespace FireNationalRendererInternal
{
	static bool TryGetGeometryCentroidLonLat(const FFireEventWithGeometry& Event, double& OutLongitude, double& OutLatitude)
	{
		double SumLon = 0.0;
		double SumLat = 0.0;
		int32 Count = 0;

		for (const FFireRing& Ring : Event.Geometry.Rings)
		{
			for (const FVector2D& LonLat : Ring.LonLatVertices)
			{
				SumLon += LonLat.X;
				SumLat += LonLat.Y;
				++Count;
			}
		}

		if (Count <= 0)
		{
			return false;
		}

		OutLongitude = SumLon / static_cast<double>(Count);
		OutLatitude = SumLat / static_cast<double>(Count);
		return true;
	}
}

UFireNationalRendererComponent::UFireNationalRendererComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UFireNationalRendererComponent::InitializeRenderer(USceneComponent* AttachParent)
{
	if (PointInstances || !GetOwner())
	{
		return;
	}

	PointInstances = NewObject<UInstancedStaticMeshComponent>(GetOwner(), TEXT("NationalPointInstances"));
	if (!PointInstances)
	{
		return;
	}

	PointInstances->SetupAttachment(AttachParent ? AttachParent : GetOwner()->GetRootComponent());
	PointInstances->RegisterComponent();
	PointInstances->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	PointInstances->SetCollisionObjectType(ECC_WorldDynamic);
	PointInstances->SetCollisionResponseToAllChannels(ECR_Ignore);
	PointInstances->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
	PointInstances->SetVisibility(true);
	PointInstances->SetHiddenInGame(false);
	PointInstances->CastShadow = false;

	if (UStaticMesh* MarkerMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder")))
	{
		PointInstances->SetStaticMesh(MarkerMesh);
	}

	if (UMaterialInterface* BaseMarkerMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")))
	{
		UMaterialInstanceDynamic* MarkerMID = UMaterialInstanceDynamic::Create(BaseMarkerMaterial, this);
		if (MarkerMID)
		{
			MarkerMID->SetVectorParameterValue(TEXT("Color"), FLinearColor(1.0f, 0.08f, 0.02f, 1.0f));
			PointInstances->SetMaterial(0, MarkerMID);
		}
	}
}

void UFireNationalRendererComponent::Configure(float InPerimeterZOffset, float InPointMarkerHeightOffset, float InPointMarkerPillarHeightCm, float InPointMarkerDiameterCm, bool bInShowPointInstances)
{
	PerimeterZOffset = InPerimeterZOffset;
	PointMarkerHeightOffset = InPointMarkerHeightOffset;
	PointMarkerPillarHeightCm = InPointMarkerPillarHeightCm;
	PointMarkerDiameterCm = InPointMarkerDiameterCm;
	bShowPointInstances = bInShowPointInstances;
}

void UFireNationalRendererComponent::RenderNationalView(const TArray<FFireEventWithGeometry>& Events, const UFireDataSubsystem* DataSubsystem)
{
	ClearNationalView();
	if (!DataSubsystem)
	{
		return;
	}

	UWorld* World = GetWorld();
	FCollisionQueryParams TraceParams(FName(TEXT("PillarTerrainSample")), false);
	if (GetOwner())
	{
		TraceParams.AddIgnoredActor(GetOwner());
	}

	RenderedEvents.Reserve(Events.Num());
	RenderedEventGroundCenters.Reserve(Events.Num());
	RenderedEventCenters.Reserve(Events.Num());

	for (const FFireEventWithGeometry& Event : Events)
	{
		double AnchorLon = Event.Attributes.Longitude;
		double AnchorLat = Event.Attributes.Latitude;
		FireNationalRendererInternal::TryGetGeometryCentroidLonLat(Event, AnchorLon, AnchorLat);

		// PerimeterZOffset keeps the ellipsoid point exactly where the old code had it —
		// the XY of this point is the authoritative geographic position for this marker.
		const FVector EllipsoidGround = DataSubsystem->LonLatToLocalWorld(AnchorLon, AnchorLat, PerimeterZOffset);

		// World-Z terrain probe anchors the pillar bottom at the real surface height.
		// IMPORTANT: all subsequent offsets use FVector::UpVector (world-Z), NOT LocalUp.
		// The cylinder instances are placed with FRotator::ZeroRotator so their height
		// axis IS world-Z.  Using LocalUp here would add horizontal components at any
		// location whose globe-normal tilts from world-Z — which is every CONUS point
		// that isn't the Cesium reference — and would displace markers off their correct
		// geographic position by up to ~15 km.
		FVector PillarBottom;
		if (World)
		{
			const FVector TraceStart(EllipsoidGround.X, EllipsoidGround.Y, EllipsoidGround.Z + 1000000.0f);
			const FVector TraceEnd  (EllipsoidGround.X, EllipsoidGround.Y, EllipsoidGround.Z - 1000000.0f);
			FHitResult Hit;
			if (World->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_Visibility, TraceParams))
			{
				// XY from hit (same geographic column, just resolved to the tile surface).
				// Z clearance in world-Z only — no horizontal drift.
				PillarBottom = FVector(Hit.Location.X, Hit.Location.Y, Hit.Location.Z + PerimeterZOffset);
			}
			else
			{
				// Tile not yet loaded — match the heatmap's fallback height (TerrainFallbackOffsetCm
				// = 5 km = PointMarkerHeightOffset) so pillar bottom and heatmap surface sit at the
				// same world-Z level when tiles haven't streamed in yet.
				PillarBottom = FVector(EllipsoidGround.X, EllipsoidGround.Y, EllipsoidGround.Z + PointMarkerHeightOffset);
			}
		}
		else
		{
			PillarBottom = FVector(EllipsoidGround.X, EllipsoidGround.Y, EllipsoidGround.Z + PointMarkerHeightOffset);
		}

		// Center = bottom + half height, pure world-Z — matches the unrotated cylinder.
		const FVector PillarCenter(PillarBottom.X, PillarBottom.Y, PillarBottom.Z + PointMarkerPillarHeightCm * 0.5f);

		RenderedEvents.Add(Event);
		RenderedEventGroundCenters.Add(PillarBottom);
		RenderedEventCenters.Add(PillarCenter);

		if (bShowPointInstances && PointInstances)
		{
			const float RadiusScale = FMath::Max(1.0f, (PointMarkerDiameterCm * 0.5f) / 50.0f);
			const float HeightScale = FMath::Max(1.0f, PointMarkerPillarHeightCm / 200.0f);
			PointInstances->AddInstance(FTransform(FRotator::ZeroRotator, PillarCenter, FVector(RadiusScale, RadiusScale, HeightScale)));
		}
	}
}

void UFireNationalRendererComponent::ClearNationalView()
{
	if (PointInstances)
	{
		PointInstances->ClearInstances();
	}

	RenderedEvents.Reset();
	RenderedEventGroundCenters.Reset();
	RenderedEventCenters.Reset();
}

bool UFireNationalRendererComponent::IsValidEventIndex(const int32 EventIndex) const
{
	return RenderedEvents.IsValidIndex(EventIndex) && RenderedEventGroundCenters.IsValidIndex(EventIndex) && RenderedEventCenters.IsValidIndex(EventIndex);
}

void UFireNationalRendererComponent::HideMarkerByIndex(const int32 EventIndex)
{
	if (!PointInstances || EventIndex < 0 || EventIndex >= PointInstances->GetInstanceCount())
	{
		return;
	}

	FTransform InstanceTransform;
	if (!PointInstances->GetInstanceTransform(EventIndex, InstanceTransform, true))
	{
		return;
	}

	InstanceTransform.SetScale3D(FVector(0.001f, 0.001f, 0.001f));
	PointInstances->UpdateInstanceTransform(EventIndex, InstanceTransform, true, true, true);
}

int32 UFireNationalRendererComponent::FindNearestEventToRay(const FVector& RayOrigin, const FVector& RayDirection, const double MaxDistanceCm, double& OutDistanceSq) const
{
	OutDistanceSq = TNumericLimits<double>::Max();
	if (RenderedEventCenters.Num() == 0)
	{
		return INDEX_NONE;
	}

	int32 BestIndex = INDEX_NONE;
	const FVector RayDirNorm = RayDirection.GetSafeNormal();
	for (int32 i = 0; i < RenderedEventCenters.Num(); ++i)
	{
		const FVector ToPoint = RenderedEventCenters[i] - RayOrigin;
		const double AlongRay = FVector::DotProduct(ToPoint, RayDirNorm);
		if (AlongRay <= 0.0)
		{
			continue;
		}

		const FVector ClosestPointOnRay = RayOrigin + RayDirNorm * AlongRay;
		const double DistanceSq = FVector::DistSquared(RenderedEventCenters[i], ClosestPointOnRay);
		const double AdaptiveThreshold = FMath::Max<double>(MaxDistanceCm, AlongRay * 0.1);
		if (DistanceSq <= AdaptiveThreshold * AdaptiveThreshold && DistanceSq < OutDistanceSq)
		{
			OutDistanceSq = DistanceSq;
			BestIndex = i;
		}
	}

	return BestIndex;
}
