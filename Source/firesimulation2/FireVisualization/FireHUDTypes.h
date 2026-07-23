#pragma once

#include "CoreMinimal.h"
#include "FireHUDTypes.generated.h"

/**
 * Compact summary of a fire event passed to UFireHUDWidget Blueprint events.
 * Built from FFireEventAttributes by AFireMapController before broadcasting
 * to the HUD widget so the widget never touches raw data subsystems.
 */
USTRUCT(BlueprintType)
struct FIRESIMULATION2_API FFireHUDFireInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Fire|HUD")
	FString EventId;

	UPROPERTY(BlueprintReadOnly, Category = "Fire|HUD")
	FString Name;

	/** ISO-8601 date string, e.g. "2020-07-12". */
	UPROPERTY(BlueprintReadOnly, Category = "Fire|HUD")
	FString DateStr;

	UPROPERTY(BlueprintReadOnly, Category = "Fire|HUD")
	FString StateCode;

	/** Pre-formatted acres string, e.g. "42.5K acres" or "1.2M acres". */
	UPROPERTY(BlueprintReadOnly, Category = "Fire|HUD")
	FString AcresStr;

	/** Human-readable severity label: "Low Severity" / "Moderate Severity" / "High Severity". */
	UPROPERTY(BlueprintReadOnly, Category = "Fire|HUD")
	FString SeverityStr;

	UPROPERTY(BlueprintReadOnly, Category = "Fire|HUD")
	int32 Year = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Fire|HUD")
	float Acres = 0.0f;

	/** Normalised severity 0..1 computed from DnbrVal or log10(acres). */
	UPROPERTY(BlueprintReadOnly, Category = "Fire|HUD")
	float SeverityNorm = 0.0f;

	/** Interpolated orange→red color from severity, ready for widget binding. */
	UPROPERTY(BlueprintReadOnly, Category = "Fire|HUD")
	FLinearColor SeverityColor = FLinearColor(1.0f, 0.55f, 0.0f, 1.0f);
};
