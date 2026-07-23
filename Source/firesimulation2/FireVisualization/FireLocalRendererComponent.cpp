#include "FireLocalRendererComponent.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "FireDataSubsystem.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"

UFireLocalRendererComponent::UFireLocalRendererComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UFireLocalRendererComponent::InitializeRenderer(USceneComponent* AttachParent)
{
	if (LocalSmokeInstances || !GetOwner())
	{
		return;
	}

	LocalSmokeInstances = NewObject<UInstancedStaticMeshComponent>(GetOwner(), TEXT("LocalSmokeInstances"));
	if (!LocalSmokeInstances)
	{
		return;
	}

	LocalSmokeInstances->SetupAttachment(AttachParent ? AttachParent : GetOwner()->GetRootComponent());
	LocalSmokeInstances->RegisterComponent();
	LocalSmokeInstances->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	LocalSmokeInstances->SetVisibility(true);
	LocalSmokeInstances->SetHiddenInGame(false);
	LocalSmokeInstances->CastShadow = false;

	if (UStaticMesh* MarkerMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder")))
	{
		LocalSmokeInstances->SetStaticMesh(MarkerMesh);
	}

	if (UMaterialInterface* BaseMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")))
	{
		UMaterialInstanceDynamic* SmokeMID = UMaterialInstanceDynamic::Create(BaseMaterial, this);
		if (SmokeMID)
		{
			SmokeMID->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.30f, 0.30f, 0.30f, 0.65f));
			LocalSmokeInstances->SetMaterial(0, SmokeMID);
		}
	}
}

void UFireLocalRendererComponent::Configure(const int32 InMaxActiveLocalFires, const float InColumnHeightCm, const float InColumnDiameterCm, const float InHeightOffsetCm)
{
	MaxActiveLocalFires = FMath::Clamp(InMaxActiveLocalFires, 1, 100);
	ColumnHeightCm = FMath::Max(InColumnHeightCm, 1000.0f);
	ColumnDiameterCm = FMath::Max(InColumnDiameterCm, 1000.0f);
	HeightOffsetCm = FMath::Max(InHeightOffsetCm, 100.0f);
}

void UFireLocalRendererComponent::RenderLocalView(const TArray<FFireEventWithGeometry>& Events, const UFireDataSubsystem* DataSubsystem)
{
	ClearLocalView();
	if (!DataSubsystem || !LocalSmokeInstances || Events.Num() == 0)
	{
		return;
	}

	TArray<int32> SortedIndices;
	SortedIndices.Reserve(Events.Num());
	for (int32 i = 0; i < Events.Num(); ++i)
	{
		SortedIndices.Add(i);
	}

	SortedIndices.Sort([&Events, this](const int32 A, const int32 B)
	{
		return ComputeLocalIntensityScore(Events[A].Attributes) > ComputeLocalIntensityScore(Events[B].Attributes);
	});

	const int32 ToRender = FMath::Min(MaxActiveLocalFires, SortedIndices.Num());
	LastRenderedCount = 0;
	for (int32 i = 0; i < ToRender; ++i)
	{
		const FFireEventWithGeometry& Event = Events[SortedIndices[i]];
		const FVector Ground = DataSubsystem->LonLatToLocalWorld(Event.Attributes.Longitude, Event.Attributes.Latitude, 0.0);
		FVector UpDir = Ground.GetSafeNormal();
		if (UpDir.IsNearlyZero())
		{
			UpDir = FVector::UpVector;
		}

		const FVector Location = Ground + (UpDir * (HeightOffsetCm + ColumnHeightCm * 0.5f));
		const float HeightScale = FMath::Max(1.0f, ColumnHeightCm / 200.0f);
		const float DiameterScale = FMath::Max(1.0f, (ColumnDiameterCm * 0.5f) / 50.0f);
		LocalSmokeInstances->AddInstance(FTransform(FRotator::ZeroRotator, Location, FVector(DiameterScale, DiameterScale, HeightScale)));
		++LastRenderedCount;
	}
}

void UFireLocalRendererComponent::ClearLocalView()
{
	if (LocalSmokeInstances)
	{
		LocalSmokeInstances->ClearInstances();
	}
	LastRenderedCount = 0;
}

float UFireLocalRendererComponent::ComputeLocalIntensityScore(const FFireEventAttributes& Attributes) const
{
	const float AcresScore = static_cast<float>(FMath::Clamp(FMath::LogX(10.0, FMath::Max(1.0, Attributes.Acres)), 0.0, 8.0));
	const float ThresholdScore = static_cast<float>(FMath::Clamp(Attributes.HighThreshold, 0.0, 1000000.0) > 0.0
		? Attributes.Acres / FMath::Max(1.0, Attributes.HighThreshold)
		: 0.0);
	return AcresScore + ThresholdScore;
}
