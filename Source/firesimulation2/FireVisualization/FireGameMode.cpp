// FireGameMode.cpp
// Sets HUDClass to AFireHUD so the wildfire HUD overlay is always active.

#include "FireGameMode.h"

#include "FireHUD.h"

AFireGameMode::AFireGameMode()
{
	HUDClass = AFireHUD::StaticClass();
}
