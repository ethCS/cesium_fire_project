#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "FireGameMode.generated.h"

/**
 * Default game mode for the wildfire simulation.
 * Sets HUDClass = AFireHUD so the fire HUD overlay is always present.
 *
 * Override in a Blueprint subclass (BP_FireGameMode) to:
 *   - Set HUDClass = BP_FireHUD (if you created a Blueprint subclass of AFireHUD)
 *   - Assign the default Pawn class or PlayerController if needed
 */
UCLASS()
class FIRESIMULATION2_API AFireGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AFireGameMode();
};
