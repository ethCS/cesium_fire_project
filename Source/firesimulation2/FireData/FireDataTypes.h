//SHAPE OF THE DATA!!!!!

// all the data structs live here. if you want to know what a "fire event" looks like in memory — the fields,
// the geometry, the combined result you actually work with — this is the file. nothing runs here, it's just
// the shape of the data. start here before reading anything else.

#pragma once

#include "CoreMinimal.h"
#include "FireDataTypes.generated.h"

USTRUCT(BlueprintType)
struct FFireEventAttributes
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire")
	FString EventId;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire")
	FString Name;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire")
	FString IncidentType;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire")
	FString Program;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire")
	FString AssessmentType;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire")
	FString StateCode;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire")
	int32 Year = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire")
	int32 DayOfYear = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire")
	FString DateIso;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire")
	double Acres = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire")
	double Latitude = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire")
	double Longitude = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire|Severity")
	double LowThreshold = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire|Severity")
	double ModerateThreshold = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire|Severity")
	double HighThreshold = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire|Severity")
	double NodataThreshold = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire|Severity")
	double GreennessThreshold = 0.0;

	/** dNBR offset value applied to normalize the severity index. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire|Severity")
	double DnbrVal = 0.0;

	/** Standard deviation of dNBR within the fire boundary — indicates intra-fire severity variability. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire|Severity")
	double DnbrStddev = 0.0;

	/** Pre-fire Normalized Burn Ratio. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire|Severity")
	double PreNbrVal = 0.0;

	/** Post-fire Normalized Burn Ratio. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire|Severity")
	double PosNbrVal = 0.0;

	/** Composite Burn Index — field-measured severity score (0 = unburned, 3 = high severity). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire|Severity")
	double Cbi = 0.0;

	/** IRWIN system cross-reference ID. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire|Metadata")
	FString IrwinId;

	/** Landsat scene ID used as the pre-fire image. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire|Metadata")
	FString PreImageId;

	/** Landsat scene ID used as the post-fire image. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire|Metadata")
	FString PostImageId;
};

USTRUCT(BlueprintType)
struct FFireRing
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire")
	TArray<FVector2D> LonLatVertices;
};

USTRUCT(BlueprintType)
struct FFireGeometry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire")
	FString EventId;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire")
	TArray<FFireRing> Rings;
};

USTRUCT(BlueprintType)
struct FFireEventWithGeometry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire")
	FFireEventAttributes Attributes;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire")
	FFireGeometry Geometry;
};

UENUM(BlueprintType)
enum class EFireViewLOD : uint8
{
	National = 0 UMETA(DisplayName = "National"),
	Regional = 1 UMETA(DisplayName = "Regional"),
	Local = 2 UMETA(DisplayName = "Local"),
	Close = 3 UMETA(DisplayName = "Close")
};

USTRUCT(BlueprintType)
struct FFireVisualState
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire")
	FString EventId;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire")
	FVector WorldLocation = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire")
	float SeverityScore = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire")
	EFireViewLOD LOD = EFireViewLOD::National;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire")
	bool bRenderNational = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire")
	bool bRenderRegional = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fire")
	bool bRenderLocal = false;
};
