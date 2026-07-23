#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "FireHUDTypes.h"
#include "FireHUD.generated.h"

class UFireHUDWidget;

/**
 * MTBS Wildfire Simulation HUD — native canvas overlay.
 *
 * Renders: year bar, fire-info panel, state-filter panel (Tab), hover tooltip,
 * controls bar, and a text-search field inside the filter panel.
 *
 * Falls back gracefully when WBP_FireHUD Blueprint widget is unavailable.
 */
UCLASS()
class FIRESIMULATION2_API AFireHUD : public AHUD
{
	GENERATED_BODY()

public:
	AFireHUD();

	virtual void BeginPlay() override;
	virtual void DrawHUD() override;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD")
	TSubclassOf<UFireHUDWidget> HUDWidgetClass;

	UFUNCTION(BlueprintPure, Category = "HUD")
	UFireHUDWidget* GetHUDWidget() const { return HUDWidget; }

	// ── State API (called by AFireMapController) ──────────────────────────
	void UpdateYearState(int32 NewYear, int32 MinYear, int32 MaxYear,
		int32 FireCount, const FString& StateCode);
	void SetFocusedFire(const FFireHUDFireInfo& Info);
	void ClearFocusedFire();
	void SetHoveredFire(const FFireHUDFireInfo& Info, bool bHovered);
	void SetAvailableStates(const TArray<FString>& States);
	void SetSelectedState(const FString& State);
	void SetMouseSensitivity(float Sensitivity);
	void SetStateFireCounts(const TMap<FString,int32>& Counts);

	/** Call when a filter item is clicked — triggers a brief visual flash. */
	void NotifyFilterItemClicked(const FString& Item);

	// ── Filter panel ─────────────────────────────────────────────────────
	void ToggleFilterPanel();
	bool IsFilterPanelOpen() const { return bShowFilterPanel; }

	/** Shows/hides a centered loading overlay with 0..1 progress. */
	void SetLoadingOverlay(bool bVisible, float Progress01, const FString& Label);
	void ShowCenterPrompt(const FString& Message, float DurationSeconds = 1.8f);

	// ── Search bar (owned by HUD; input fed by FireMapController) ────────
	void ActivateSearchBar();
	void DeactivateSearchBar();
	bool IsSearchBarActive() const { return bSearchBarActive; }
	void AppendSearchChar(TCHAR Ch);
	void BackspaceSearch();
	void ClearSearch();
	FString GetSearchBuffer() const { return SearchBuffer; }

	/**
	 * Hit-tests the filter panel at the given screen position.
	 * Returns:
	 *   - A US state code (e.g. "CA") when a state cell is clicked.
	 *   - "__SENS_UP__" / "__SENS_DOWN__" for sensitivity +/- buttons.
	 *   - "__SEARCH_BAR__" when the search field is clicked.
	 *   - "__PANEL__" when inside the panel but no specific target.
	 *   - Empty string when outside the panel entirely.
	 */
	FString HitTestFilterPanel(float MouseX, float MouseY) const;

private:
	UPROPERTY(Transient)
	TObjectPtr<UFireHUDWidget> HUDWidget;

	// ── Year / filter state ───────────────────────────────────────────────
	int32   HUDYear      = 2020;
	int32   HUDMinYear   = 1984;
	int32   HUDMaxYear   = 2026;
	int32   HUDFireCount = 0;
	FString HUDStateCode = TEXT("ALL");

	// ── Fire panel ────────────────────────────────────────────────────────
	bool           bShowFirePanel  = false;
	FFireHUDFireInfo FocusedFireInfo;

	// ── Hover tooltip ─────────────────────────────────────────────────────
	bool           bShowHoverTooltip = false;
	FFireHUDFireInfo HoveredFireInfo;

	// ── Filter / search panel ─────────────────────────────────────────────
	bool            bShowFilterPanel   = true;   // open by default
	TArray<FString> AvailableStates;
	FString         SelectedState      = TEXT("ALL");
	float           HUDMouseSensitivity = 1.0f;

	// ── Interactive state: hover highlight + click flash ──────────────────
	FString HoveredFilterItem;             // re-computed every DrawHUD frame
	FString FilterClickFlashItem;          // item that was just clicked
	float   FilterClickFlashEndTime = -1.f;// world-time expiry of flash
	TMap<FString,int32> StateFireCounts;   // # fires per state for display

	bool   bSearchBarActive = false;
	FString SearchBuffer;

	// Startup loading overlay state.
	bool    bShowLoadingOverlay = false;
	float   LoadingProgress01 = 0.0f;
	FString LoadingLabel = TEXT("Loading wildfire data...");
	FString CenterPromptText;
	float   CenterPromptEndTime = -1.0f;

	// Cached viewport size for input hit-testing outside DrawHUD (Canvas can be null there).
	float CachedViewportSizeX = 0.0f;
	float CachedViewportSizeY = 0.0f;

	// ── DrawHUD sub-draws ─────────────────────────────────────────────────
	void DrawYearBar();
	void DrawControlsBar();
	void DrawBrandingPanel();
	void DrawFireInfoPanel();
	void DrawFilterPanel();
	void DrawHoverTooltip();
	void DrawLoadingOverlay();
	void DrawCenterPrompt();

	void DrawPanel(float X, float Y, float W, float H, const FLinearColor& BgColor);
	void DrawProgressBar(float X, float Y, float W, float H,
		float Progress, const FLinearColor& BgColor, const FLinearColor& FillColor);
};
