#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "FireHUDTypes.h"
#include "FireHUDWidget.generated.h"

/**
 * C++ base for the WBP_FireHUD Blueprint widget.
 *
 * All data delivery uses BlueprintImplementableEvent so designers can bind
 * widget properties however they like without touching C++.
 *
 * Usage:
 *   1. Create a Widget Blueprint (WBP_FireHUD) with parent class UFireHUDWidget.
 *   2. Implement the On* events in the Blueprint event graph or widget bindings.
 *   3. Assign WBP_FireHUD to AFireHUD::HUDWidgetClass (via BP_FireHUD or DefaultProperties).
 *
 * A minimal on-screen debug fallback is automatically active when no Blueprint
 * widget is assigned — the AFireHUD will warn and the game still functions.
 */
UCLASS(Abstract, BlueprintType, Blueprintable)
class FIRESIMULATION2_API UFireHUDWidget : public UUserWidget
{
	GENERATED_BODY()

public:

	// -------------------------------------------------------------------------
	// Year / filter state
	// -------------------------------------------------------------------------

	/**
	 * Called immediately after the active year changes.
	 * @param NewYear     The newly selected year.
	 * @param MinYear     Oldest available year in the loaded dataset (1984).
	 * @param MaxYear     Most recent available year (2026).
	 * @param FireCount   Number of fires visible in the current filter.
	 * @param StateCode   Active state filter ("ALL" = nationwide).
	 */
	UFUNCTION(BlueprintImplementableEvent, Category = "Fire|HUD")
	void OnYearChanged(int32 NewYear, int32 MinYear, int32 MaxYear, int32 FireCount, const FString& StateCode);

	// -------------------------------------------------------------------------
	// Fire focus
	// -------------------------------------------------------------------------

	/** Called when a fire is focused (clicked or navigated to via ,/. keys). */
	UFUNCTION(BlueprintImplementableEvent, Category = "Fire|HUD")
	void OnFireFocused(const FFireHUDFireInfo& Info);

	/** Called when the camera returns to global overview (Esc). */
	UFUNCTION(BlueprintImplementableEvent, Category = "Fire|HUD")
	void OnReturnedToOverview();

	// -------------------------------------------------------------------------
	// Hover tooltip
	// -------------------------------------------------------------------------

	/**
	 * Called every tick while the cursor hovers over a fire pillar.
	 * bHovered = false when the cursor leaves all pillars.
	 */
	UFUNCTION(BlueprintImplementableEvent, Category = "Fire|HUD")
	void OnFireHovered(const FFireHUDFireInfo& Info, bool bHovered);
};
