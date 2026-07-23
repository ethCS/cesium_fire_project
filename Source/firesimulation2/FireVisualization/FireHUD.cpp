// FireHUD.cpp -- MTBS Wildfire Simulation HUD  v1.2.0
// University of Montana -- Computer Science Department
// Native canvas: branding panel, state filter, fire info, controls bar.

#include "FireHUD.h"
#include "Blueprint/UserWidget.h"
#include "Engine/Canvas.h"
#include "FireHUDWidget.h"
#include "firesimulation2.h"

// Design tokens
namespace HUDStyle
{
	static const FLinearColor BgDeep    (0.012f,0.014f,0.048f,0.96f);
	static const FLinearColor BgMid     (0.024f,0.028f,0.072f,0.86f);
	static const FLinearColor BgGlass   (0.030f,0.038f,0.090f,0.80f);
	static const FLinearColor AccentBlue   (0.20f, 0.60f, 1.00f, 1.00f);
	static const FLinearColor AccentBlueDim(0.10f, 0.35f, 0.70f, 0.75f);
	static const FLinearColor AccentGold   (1.00f, 0.82f, 0.22f, 1.00f);
	static const FLinearColor AccentGreen  (0.20f, 1.00f, 0.55f, 1.00f);
	static const FLinearColor TextPrimary  (0.96f, 0.97f, 1.00f, 1.00f);
	static const FLinearColor TextMuted    (0.56f, 0.60f, 0.70f, 0.90f);
	static const FLinearColor TextDim      (0.36f, 0.38f, 0.50f, 0.76f);
	static const FLinearColor Separator    (0.18f, 0.20f, 0.32f, 0.40f);
	static const FLinearColor KeyBg        (0.09f, 0.11f, 0.19f, 0.96f);
	static const FLinearColor KeyBorder    (0.26f, 0.30f, 0.48f, 0.50f);
	static const FLinearColor Shadow       (0.00f, 0.00f, 0.00f, 0.22f);
	static const FLinearColor SevLow       (1.00f, 0.72f, 0.10f, 1.00f);
	static const FLinearColor SevHigh      (0.90f, 0.10f, 0.05f, 1.00f);
	static const FLinearColor FilterBg     (0.009f,0.013f,0.050f,0.98f);
	static const FLinearColor FilterBorder (0.20f, 0.58f, 1.00f, 0.38f);
	static const FLinearColor FilterNormal (0.055f,0.065f,0.120f,0.80f);
	static const FLinearColor FilterHover  (0.09f, 0.20f, 0.42f, 0.92f);
	static const FLinearColor FilterSel    (0.04f, 0.28f, 0.62f, 0.92f);
	static const FLinearColor FilterFlash  (0.22f, 0.58f, 1.00f, 1.00f);
	static const FLinearColor SearchActive (0.04f, 0.15f, 0.36f, 0.94f);
	static const FLinearColor SearchIdle   (0.055f,0.065f,0.130f,0.88f);
	static const FLinearColor UMMaroon    (0.29f, 0.03f, 0.08f, 1.00f);
	static const FLinearColor UMGold      (1.00f, 0.78f, 0.17f, 1.00f);
	static const FLinearColor UMGoldDim   (0.80f, 0.60f, 0.10f, 0.70f);
	static const FLinearColor UMBrandBg   (0.010f,0.010f,0.038f,0.97f);
	static constexpr float TopBarH    = 52.f;
	static constexpr float BottomBarH = 46.f;
	static constexpr float InfoPanelW = 296.f;
	static constexpr float InfoPanelX = 16.f;
	static constexpr float FilterW    = 240.f;
	static constexpr float BrandingW  = 274.f;
	static constexpr float BrandingH  = 74.f;
	static constexpr float Pad        = 12.f;
	static constexpr float SPad       =  7.f;
}

namespace
{
	static FString GetStateFullName(const FString& StateCode)
	{
		static const TMap<FString, FString> StateNames = {
			{TEXT("AL"),TEXT("ALABAMA")},{TEXT("AK"),TEXT("ALASKA")},{TEXT("AZ"),TEXT("ARIZONA")},
			{TEXT("AR"),TEXT("ARKANSAS")},{TEXT("CA"),TEXT("CALIFORNIA")},{TEXT("CO"),TEXT("COLORADO")},
			{TEXT("CT"),TEXT("CONNECTICUT")},{TEXT("DE"),TEXT("DELAWARE")},{TEXT("FL"),TEXT("FLORIDA")},
			{TEXT("GA"),TEXT("GEORGIA")},{TEXT("HI"),TEXT("HAWAII")},{TEXT("ID"),TEXT("IDAHO")},
			{TEXT("IL"),TEXT("ILLINOIS")},{TEXT("IN"),TEXT("INDIANA")},{TEXT("IA"),TEXT("IOWA")},
			{TEXT("KS"),TEXT("KANSAS")},{TEXT("KY"),TEXT("KENTUCKY")},{TEXT("LA"),TEXT("LOUISIANA")},
			{TEXT("ME"),TEXT("MAINE")},{TEXT("MD"),TEXT("MARYLAND")},{TEXT("MA"),TEXT("MASSACHUSETTS")},
			{TEXT("MI"),TEXT("MICHIGAN")},{TEXT("MN"),TEXT("MINNESOTA")},{TEXT("MS"),TEXT("MISSISSIPPI")},
			{TEXT("MO"),TEXT("MISSOURI")},{TEXT("MT"),TEXT("MONTANA")},{TEXT("NE"),TEXT("NEBRASKA")},
			{TEXT("NV"),TEXT("NEVADA")},{TEXT("NH"),TEXT("NEW HAMPSHIRE")},{TEXT("NJ"),TEXT("NEW JERSEY")},
			{TEXT("NM"),TEXT("NEW MEXICO")},{TEXT("NY"),TEXT("NEW YORK")},{TEXT("NC"),TEXT("NORTH CAROLINA")},
			{TEXT("ND"),TEXT("NORTH DAKOTA")},{TEXT("OH"),TEXT("OHIO")},{TEXT("OK"),TEXT("OKLAHOMA")},
			{TEXT("OR"),TEXT("OREGON")},{TEXT("PA"),TEXT("PENNSYLVANIA")},{TEXT("RI"),TEXT("RHODE ISLAND")},
			{TEXT("SC"),TEXT("SOUTH CAROLINA")},{TEXT("SD"),TEXT("SOUTH DAKOTA")},{TEXT("TN"),TEXT("TENNESSEE")},
			{TEXT("TX"),TEXT("TEXAS")},{TEXT("UT"),TEXT("UTAH")},{TEXT("VT"),TEXT("VERMONT")},
			{TEXT("VA"),TEXT("VIRGINIA")},{TEXT("WA"),TEXT("WASHINGTON")},{TEXT("WV"),TEXT("WEST VIRGINIA")},
			{TEXT("WI"),TEXT("WISCONSIN")},{TEXT("WY"),TEXT("WYOMING")}
		};
		if (const FString* Found = StateNames.Find(StateCode.ToUpper()))
		{
			return *Found;
		}
		return FString();
	}

	static bool StateMatchesSearch(const FString& StateCode, const FString& SearchBuffer)
	{
		if (SearchBuffer.IsEmpty())
		{
			return true;
		}
		if (StateCode.Contains(SearchBuffer, ESearchCase::IgnoreCase))
		{
			return true;
		}
		const FString FullName = GetStateFullName(StateCode);
		return !FullName.IsEmpty() && FullName.Contains(SearchBuffer, ESearchCase::IgnoreCase);
	}
}

AFireHUD::AFireHUD()
{
	static ConstructorHelpers::FClassFinder<UFireHUDWidget> Widget(
		TEXT("/Game/WBP_FireHUD.WBP_FireHUD_C"));
	if (Widget.Succeeded()) { HUDWidgetClass = Widget.Class; }
}

void AFireHUD::BeginPlay()
{
	Super::BeginPlay();
	if (!HUDWidgetClass) { UE_LOG(LogFireSimulation2, Display, TEXT("AFireHUD: native canvas HUD active.")); return; }
	HUDWidget = CreateWidget<UFireHUDWidget>(GetOwningPlayerController(), HUDWidgetClass);
	if (HUDWidget) { HUDWidget->AddToViewport(10); }
}

void AFireHUD::UpdateYearState(const int32 Y,const int32 Min,const int32 Max,const int32 Count,const FString& SC)
{
	HUDYear=Y; HUDMinYear=Min; HUDMaxYear=Max; HUDFireCount=Count; HUDStateCode=SC; SelectedState=SC;
	if (HUDWidget) { HUDWidget->OnYearChanged(Y,Min,Max,Count,SC); }
}

void AFireHUD::SetFocusedFire(const FFireHUDFireInfo& I) { bShowFirePanel=true; FocusedFireInfo=I; if (HUDWidget) HUDWidget->OnFireFocused(I); }
void AFireHUD::ClearFocusedFire()                        { bShowFirePanel=false; if (HUDWidget) HUDWidget->OnReturnedToOverview(); }
void AFireHUD::SetHoveredFire(const FFireHUDFireInfo& I,bool bH) { bShowHoverTooltip=bH; if(bH)HoveredFireInfo=I; if(HUDWidget)HUDWidget->OnFireHovered(I,bH); }
void AFireHUD::ToggleFilterPanel()      { bShowFilterPanel=!bShowFilterPanel; if(!bShowFilterPanel){bSearchBarActive=false;SearchBuffer.Empty();} }
void AFireHUD::SetAvailableStates(const TArray<FString>& S) { AvailableStates=S; }
void AFireHUD::SetSelectedState(const FString& S)           { SelectedState=S; HUDStateCode=S; }
void AFireHUD::SetMouseSensitivity(float V)                 { HUDMouseSensitivity=FMath::Clamp(V,0.1f,5.0f); }
void AFireHUD::SetStateFireCounts(const TMap<FString,int32>& C) { StateFireCounts=C; }
void AFireHUD::SetLoadingOverlay(const bool bVisible, const float Progress01, const FString& Label)
{
	bShowLoadingOverlay = bVisible;
	LoadingProgress01 = FMath::Clamp(Progress01, 0.0f, 1.0f);
	LoadingLabel = Label.IsEmpty() ? TEXT("Loading wildfire data...") : Label;
}
void AFireHUD::ShowCenterPrompt(const FString& Message, const float DurationSeconds)
{
	CenterPromptText = Message;
	CenterPromptEndTime = GetWorld() ? (GetWorld()->GetTimeSeconds() + FMath::Max(0.1f, DurationSeconds)) : -1.0f;
}
void AFireHUD::NotifyFilterItemClicked(const FString& Item)
{
	FilterClickFlashItem    = Item;
	FilterClickFlashEndTime = GetWorld() ? GetWorld()->GetTimeSeconds()+0.45f : -1.f;
}
void AFireHUD::ActivateSearchBar()   { bSearchBarActive=true; }
void AFireHUD::DeactivateSearchBar() { bSearchBarActive=false; }
void AFireHUD::BackspaceSearch()     { if(SearchBuffer.Len()>0) SearchBuffer=SearchBuffer.LeftChop(1); }
void AFireHUD::ClearSearch()         { SearchBuffer.Empty(); }
void AFireHUD::AppendSearchChar(const TCHAR Ch) { if(SearchBuffer.Len()<20) SearchBuffer.AppendChar(Ch); }

FString AFireHUD::HitTestFilterPanel(const float MX,const float MY) const
{
	if(!bShowFilterPanel) return FString();

	float ViewW = Canvas ? Canvas->SizeX : CachedViewportSizeX;
	float ViewH = Canvas ? Canvas->SizeY : CachedViewportSizeY;
	if ((ViewW <= 0.f || ViewH <= 0.f) && GetOwningPlayerController())
	{
		int32 VX = 0, VY = 0;
		GetOwningPlayerController()->GetViewportSize(VX, VY);
		ViewW = static_cast<float>(VX);
		ViewH = static_cast<float>(VY);
	}
	if (ViewW <= 0.f || ViewH <= 0.f) return FString();

	const float FX=ViewW-HUDStyle::FilterW-4.f, FY=HUDStyle::TopBarH+4.f;
	const float FH=ViewH-HUDStyle::TopBarH-HUDStyle::BottomBarH-8.f;
	if(MX<FX||MX>FX+HUDStyle::FilterW||MY<FY||MY>FY+FH) return FString();
	const float P=HUDStyle::SPad;
	const float TX=FX+2.5f+P;
	const float InW=HUDStyle::FilterW-2.5f-P*2.f;
	float CurY=FY+P+24.f+P;
	const float SrchH=28.f;
	if(MY>=CurY&&MY<=CurY+SrchH) {
		if(!SearchBuffer.IsEmpty()&&MX>=FX+HUDStyle::FilterW-P-22.f&&MX<=FX+HUDStyle::FilterW-P) return TEXT("__SEARCH_CLEAR__");
		return TEXT("__SEARCH_BAR__");
	}
	CurY+=SrchH+(bSearchBarActive?18.f:4.f)+P;
	if(MY>=CurY&&MY<=CurY+30.f) return TEXT("ALL");
	CurY+=30.f+P;
	constexpr int32 Cols=4; constexpr float CellH=28.f;
	const float CellW=InW/Cols;
	TArray<FString> F2;
	for(const FString& S:AvailableStates) if(StateMatchesSearch(S, SearchBuffer)) F2.Add(S);
	for(int32 i=0;i<F2.Num();++i) {
		const float CX=TX+(i%Cols)*CellW, CY=CurY+(i/Cols)*(CellH+3.f);
		if(CY+CellH>FY+FH-92.f) break;
		if(MX>=CX&&MX<=CX+CellW-3.f&&MY>=CY&&MY<=CY+CellH) return F2[i];
	}
	const int32 NR=(F2.Num()+Cols-1)/Cols;
	const float SBW=InW-64.f;
	const float SensY=CurY+NR*(CellH+3.f)+P+16.f+P;
	const float BPX=TX+SBW+6.f, BMX=BPX+24.f+4.f;
	if(MY>=SensY&&MY<=SensY+22.f) {
		if(MX>=BPX&&MX<=BPX+24.f)  return TEXT("__SENS_UP__");
		if(MX>=BMX&&MX<=BMX+24.f)  return TEXT("__SENS_DOWN__");
	}
	return TEXT("__PANEL__");
}

void AFireHUD::DrawHUD()
{
	Super::DrawHUD();
	if(HUDWidget||!Canvas) return;
	CachedViewportSizeX = Canvas->SizeX;
	CachedViewportSizeY = Canvas->SizeY;
	if(bShowFilterPanel&&GetOwningPlayerController()) {
		float MX=0.f,MY=0.f; GetOwningPlayerController()->GetMousePosition(MX,MY);
		HoveredFilterItem=HitTestFilterPanel(MX,MY);
	} else HoveredFilterItem.Empty();
	if(GetWorld()&&GetWorld()->GetTimeSeconds()>FilterClickFlashEndTime) FilterClickFlashItem.Empty();
	if(GetWorld()&&GetWorld()->GetTimeSeconds()>CenterPromptEndTime) { CenterPromptText.Empty(); }
	DrawYearBar();
	DrawControlsBar();
	DrawBrandingPanel();
	if(bShowFirePanel)   DrawFireInfoPanel();
	if(bShowFilterPanel) DrawFilterPanel();
	if(bShowHoverTooltip&&!bShowFilterPanel) DrawHoverTooltip();
	if(!CenterPromptText.IsEmpty()) DrawCenterPrompt();
	if(bShowLoadingOverlay) DrawLoadingOverlay();
}

void AFireHUD::DrawPanel(float X,float Y,float W,float H,const FLinearColor& C) { if(Canvas) DrawRect(C,X,Y,W,H); }
void AFireHUD::DrawProgressBar(float X,float Y,float W,float H,float P,const FLinearColor& Bg,const FLinearColor& Fill)
{ DrawPanel(X,Y,W,H,Bg); DrawPanel(X,Y,FMath::Max(1.f,W*FMath::Clamp(P,0.f,1.f)),H,Fill); }

void AFireHUD::DrawYearBar()
{
	if(!Canvas) return;
	const float W=Canvas->SizeX, H=HUDStyle::TopBarH, Pad=HUDStyle::Pad;
	DrawPanel(0.f,0.f,W,H*0.30f,FLinearColor(0.008f,0.010f,0.055f,0.98f));
	DrawPanel(0.f,H*0.30f,W,H*0.42f,HUDStyle::BgDeep);
	DrawPanel(0.f,H*0.72f,W,H*0.28f,FLinearColor(0.035f,0.040f,0.100f,0.62f));
	DrawPanel(0.f,H-1.5f,W,1.5f,HUDStyle::Separator);
	DrawPanel(0.f,0.f,W,1.5f,FLinearColor(0.20f,0.60f,1.00f,0.18f));
	const float YR=(float)FMath::Max(1,HUDMaxYear-HUDMinYear);
	DrawPanel(0.f,H-4.f,W,4.f,FLinearColor(0.04f,0.04f,0.10f,0.65f));
	DrawPanel(0.f,H-4.f,FMath::Max(2.f,W*(HUDYear-HUDMinYear)/YR),4.f,FLinearColor(0.20f,0.60f,1.00f,0.52f));
	const FString YS=FString::Printf(TEXT("< %d >"),HUDYear);
	float YW=0.f,YHt=0.f; GetTextSize(YS,YW,YHt,nullptr,1.55f);
	const float YX=(W-YW)*0.5f, YY=(H-YHt)*0.5f-3.f;
	DrawPanel(YX-12.f+2.f,YY-5.f+2.f,YW+24.f,YHt+10.f,FLinearColor(0,0,0,0.32f));
	DrawPanel(YX-12.f,YY-5.f,YW+24.f,YHt+10.f,FLinearColor(0.04f,0.12f,0.28f,0.84f));
	DrawPanel(YX-12.f,YY-5.f,2.5f,YHt+10.f,HUDStyle::AccentBlue);
	DrawPanel(YX+YW+9.5f,YY-5.f,2.5f,YHt+10.f,HUDStyle::AccentBlue);
	DrawPanel(YX-9.5f,YY-5.f,YW+19.f,1.f,FLinearColor(1,1,1,0.06f));
	DrawText(YS,HUDStyle::AccentGold,YX,YY,nullptr,1.55f);
	const FString FS=FString::Printf(TEXT("%d Fires"),HUDFireCount);
	DrawPanel(Pad,YY+(YHt-6.f)*0.5f,6.f,6.f,HUDStyle::AccentBlue);
	DrawText(FS,HUDStyle::TextPrimary,Pad+10.f,YY+2.f,nullptr,1.05f);
	DrawText(FString::Printf(TEXT("%d"),HUDMinYear),HUDStyle::TextDim,Pad,H-13.f,nullptr,0.76f);
	const FString MxS=FString::Printf(TEXT("%d"),HUDMaxYear);
	float MxW=0.f,MxH=0.f; GetTextSize(MxS,MxW,MxH,nullptr,0.76f);
	const float FilterLeft=bShowFilterPanel?(Canvas->SizeX-HUDStyle::FilterW-10.f):(Canvas->SizeX-Pad);
	DrawText(MxS,HUDStyle::TextDim,FMath::Min(FilterLeft-MxW-8.f,W-MxW-Pad),H-13.f,nullptr,0.76f);
	const FString SS=SelectedState.IsEmpty()?TEXT("ALL"):SelectedState;
	const FString BadgeStr=FString::Printf(TEXT(" %s "),*SS);
	float BW2=0.f,BH2=0.f; GetTextSize(BadgeStr,BW2,BH2,nullptr,1.00f);
	const float BadgeX=FilterLeft-BW2-10.f, BadgeY=YY;
	DrawPanel(BadgeX-2.f,BadgeY-3.f,BW2+4.f,BH2+6.f,FLinearColor(0.02f,0.18f,0.08f,0.78f));
	DrawPanel(BadgeX-2.f,BadgeY-3.f,2.f,BH2+6.f,HUDStyle::AccentGreen);
	DrawPanel(BadgeX-2.f,BadgeY-3.f,BW2+4.f,1.f,FLinearColor(0.2f,0.8f,0.4f,0.25f));
	DrawText(BadgeStr,HUDStyle::AccentGreen,BadgeX,BadgeY,nullptr,1.00f);
	if(!bShowFilterPanel) {
		const FString Hint=TEXT("[TAB] Filter");
		float HW=0.f,HH=0.f; GetTextSize(Hint,HW,HH,nullptr,0.82f);
		DrawPanel(BadgeX-HW-18.f,YY,HW+8.f,HH+4.f,FLinearColor(0.06f,0.10f,0.22f,0.70f));
		DrawText(Hint,HUDStyle::AccentBlue,BadgeX-HW-14.f,YY+2.f,nullptr,0.82f);
	}
}

void AFireHUD::DrawBrandingPanel()
{
	if(!Canvas) return;
	const float BX=8.f, BY=HUDStyle::TopBarH+6.f;
	const float BW=HUDStyle::BrandingW, BH=HUDStyle::BrandingH;
	const float P=HUDStyle::SPad+3.f;
	DrawPanel(BX-2.f,BY-2.f,BW+4.f,BH+4.f,FLinearColor(HUDStyle::UMMaroon.R*0.4f,0.f,0.f,0.16f));
	DrawPanel(BX+3.f,BY+4.f,BW,BH,FLinearColor(0,0,0,0.26f));
	DrawPanel(BX,BY,BW,BH,HUDStyle::UMBrandBg);
	DrawPanel(BX,BY,5.f,BH,HUDStyle::UMMaroon);
	DrawPanel(BX+5.f,BY,1.5f,BH,HUDStyle::UMGoldDim);
	DrawPanel(BX+5.f,BY,BW-5.f,1.5f,HUDStyle::UMGold);
	DrawPanel(BX+5.f,BY+BH-1.f,BW-5.f,1.f,HUDStyle::Separator);
	DrawPanel(BX+7.f,BY+2.f,BW-9.f,1.f,FLinearColor(1,1,1,0.04f));
	DrawPanel(BX+BW-4.f,BY,4.f,4.f,FLinearColor(0,0,0,0.75f));
	DrawPanel(BX+BW-4.f,BY+BH-4.f,4.f,4.f,FLinearColor(0,0,0,0.75f));
	const float TX=BX+5.f+P;
	float TY=BY+9.f;
	DrawText(TEXT("UNIVERSITY OF MONTANA"),HUDStyle::UMGold,TX,TY,nullptr,0.92f);
	TY+=18.f;
	DrawPanel(TX,TY,BW-5.f-P*2.f,1.f,FLinearColor(HUDStyle::UMGold.R,HUDStyle::UMGold.G,HUDStyle::UMGold.B,0.16f));
	TY+=5.f;
	DrawText(TEXT("Computer Science Department"),HUDStyle::TextMuted,TX,TY,nullptr,0.80f);
	TY+=15.f;
	DrawText(TEXT("3D Interactive UE5 Wildfire Simulation"),HUDStyle::AccentBlue,TX,TY,nullptr,0.78f);
	const FString VStr=TEXT("v1.2.0");
	float VW=0.f,VH=0.f; GetTextSize(VStr,VW,VH,nullptr,0.70f);
	DrawPanel(BX+BW-VW-P-4.f,BY+BH-VH-5.f,VW+8.f,VH+4.f,FLinearColor(0.06f,0.10f,0.22f,0.82f));
	DrawPanel(BX+BW-VW-P-4.f,BY+BH-VH-5.f,1.5f,VH+4.f,FLinearColor(0.20f,0.35f,0.70f,0.75f));
	DrawText(VStr,HUDStyle::TextDim,BX+BW-VW-P,BY+BH-VH-4.f,nullptr,0.70f);
	DrawText(TEXT("Data: USFS MTBS Program"),HUDStyle::TextDim,TX,BY+BH-VH-4.f,nullptr,0.68f);
}

void AFireHUD::DrawControlsBar()
{
	if(!Canvas) return;
	const float W=Canvas->SizeX, BH=HUDStyle::BottomBarH, BY=Canvas->SizeY-BH, P=10.f;
	DrawPanel(0.f,BY,W,1.5f,HUDStyle::Separator);
	DrawPanel(0.f,BY+1.5f,W,BH*0.36f,FLinearColor(0.035f,0.038f,0.095f,0.65f));
	DrawPanel(0.f,BY+BH*0.36f,W,BH-BH*0.36f,HUDStyle::BgDeep);
	DrawPanel(0.f,BY+BH-1.f,W,1.f,FLinearColor(0.20f,0.60f,1.00f,0.10f));
	auto Key=[&](const FString& L,float X,float Y,float Sc)->float {
		float KW=0.f,KH=0.f; GetTextSize(L,KW,KH,nullptr,Sc);
		DrawPanel(X-1.f,Y-2.f,KW+6.f,KH+4.f,HUDStyle::KeyBg);
		DrawPanel(X-1.f,Y-2.f,KW+6.f,1.f,HUDStyle::KeyBorder);
		DrawPanel(X-1.f,Y+KH+1.f,KW+6.f,1.f,FLinearColor(0,0,0,0.50f));
		DrawText(L,HUDStyle::TextPrimary,X+2.f,Y,nullptr,Sc);
		return KW+10.f;
	};
	const float Sc=0.80f, R1=BY+5.f, R2=BY+BH*0.50f+1.f;
	float CX=P;
	CX+=Key(TEXT("W A S D"),CX,R1,Sc); DrawText(TEXT("Move"),HUDStyle::TextMuted,CX,R1,nullptr,Sc); CX+=44.f;
	CX+=Key(TEXT("Mouse"),CX,R1,Sc);   DrawText(TEXT("Look"),HUDStyle::TextMuted,CX,R1,nullptr,Sc); CX+=38.f;
	CX+=Key(TEXT("Scroll/+/-"),CX,R1,Sc); DrawText(TEXT("Zoom"),HUDStyle::TextMuted,CX,R1,nullptr,Sc); CX+=42.f;
	CX+=Key(TEXT("TAB"),CX,R1,Sc); DrawText(TEXT("Filter States"),HUDStyle::AccentBlue,CX,R1,nullptr,Sc);
	CX=P;
	if(bShowFirePanel) {
		CX+=Key(TEXT(","),CX,R2,Sc); DrawText(TEXT("Prev  "),HUDStyle::TextMuted,CX,R2,nullptr,Sc); CX+=44.f;
		CX+=Key(TEXT("."),CX,R2,Sc); DrawText(TEXT("Next Fire  "),HUDStyle::TextMuted,CX,R2,nullptr,Sc); CX+=72.f;
		CX+=Key(TEXT("Esc"),CX,R2,Sc); DrawText(TEXT("Return to Globe"),HUDStyle::AccentBlue,CX,R2,nullptr,Sc);
	} else {
		CX+=Key(TEXT("< >"),CX,R2,Sc); DrawText(TEXT("Year  "),HUDStyle::TextMuted,CX,R2,nullptr,Sc); CX+=46.f;
		CX+=Key(TEXT("Click Pillar"),CX,R2,Sc); DrawText(TEXT("Descend  "),HUDStyle::AccentGold,CX,R2,nullptr,Sc); CX+=62.f;
		CX+=Key(TEXT(", ."),CX,R2,Sc); DrawText(TEXT("Browse Fires  "),HUDStyle::TextMuted,CX,R2,nullptr,Sc); CX+=90.f;
		CX+=Key(TEXT("Esc"),CX,R2,Sc); DrawText(TEXT("Overview"),HUDStyle::TextMuted,CX,R2,nullptr,Sc);
	}
}

void AFireHUD::DrawFireInfoPanel()
{
	if(!Canvas) return;
	const float PX=HUDStyle::InfoPanelX, PY=HUDStyle::TopBarH+HUDStyle::BrandingH+16.f;
	const float PW=HUDStyle::InfoPanelW, LH=22.f, P=HUDStyle::Pad;
	const FLinearColor& Sev=FocusedFireInfo.SeverityColor;
	const float PH=P+30.f+P+4.f+5*LH+P+14.f+P+18.f+P;
	DrawPanel(PX+3.f,PY+4.f,PW,PH,FLinearColor(0,0,0,0.28f));
	DrawPanel(PX,PY,PW,PH,HUDStyle::BgDeep);
	DrawPanel(PX,PY,PW,PH*0.35f,FLinearColor(0.04f,0.05f,0.14f,0.42f));
	DrawPanel(PX,PY,3.5f,PH,Sev);
	DrawPanel(PX+3.5f,PY,PW-3.5f,1.5f,FLinearColor(Sev.R,Sev.G,Sev.B,0.45f));
	DrawPanel(PX,PY+PH-1.f,PW,1.f,HUDStyle::Separator);
	DrawPanel(PX+PW-4.f,PY,4.f,4.f,FLinearColor(0,0,0,0.72f));
	const float TX=PX+3.5f+P;
	FString Name=FocusedFireInfo.Name; if(Name.Len()>26) Name=Name.Left(24)+TEXT("...");
	float NW=0.f,NHt=0.f; GetTextSize(Name,NW,NHt,nullptr,1.18f);
	DrawPanel(TX-4.f,PY+P-2.f,PW-3.5f-P-4.f,NHt+8.f,FLinearColor(Sev.R*0.10f,Sev.G*0.10f,Sev.B*0.10f,0.70f));
	DrawText(Name,Sev,TX,PY+P,nullptr,1.18f);
	float CurY=PY+P+NHt+7.f;
	DrawPanel(TX,CurY,PW-P*2.f,1.f,HUDStyle::Separator); CurY+=5.f;
	auto Field=[&](const FString& Lbl,const FString& Val,const FLinearColor& VC) {
		DrawText(Lbl,HUDStyle::TextDim,TX,CurY,nullptr,0.86f);
		float LW=0.f,LH2=0.f; GetTextSize(Lbl,LW,LH2,nullptr,0.86f);
		DrawText(Val,VC,TX+LW+3.f,CurY,nullptr,0.86f); CurY+=LH;
	};
	Field(TEXT("Date:  "),FocusedFireInfo.DateStr,HUDStyle::TextPrimary);
	Field(TEXT("State: "),FocusedFireInfo.StateCode,HUDStyle::TextPrimary);
	Field(TEXT("Area:  "),FocusedFireInfo.AcresStr,HUDStyle::AccentGold);
	Field(TEXT("Sev:   "),FocusedFireInfo.SeverityStr,Sev);
	Field(TEXT("Year:  "),FString::FromInt(FocusedFireInfo.Year),HUDStyle::TextMuted);
	CurY+=3.f;
	const float BW2=PW-P*2.5f;
	DrawProgressBar(TX,CurY,BW2,7.f,FocusedFireInfo.SeverityNorm,FLinearColor(0.06f,0.06f,0.10f,0.80f),Sev);
	DrawPanel(TX+BW2*0.33f,CurY-2.f,1.f,11.f,HUDStyle::Separator);
	DrawPanel(TX+BW2*0.66f,CurY-2.f,1.f,11.f,HUDStyle::Separator);
	CurY+=11.f;
	DrawText(TEXT("Low"),HUDStyle::SevLow,TX,CurY,nullptr,0.70f);
	DrawText(TEXT("High"),HUDStyle::SevHigh,TX+BW2-22.f,CurY,nullptr,0.70f);
	CurY+=14.f;
	DrawPanel(TX,CurY,PW-P*2.f,1.f,HUDStyle::Separator); CurY+=5.f;
	DrawText(TEXT("[,][.]  Prev / Next   [Esc]  Globe"),HUDStyle::TextDim,TX,CurY,nullptr,0.76f);
}

void AFireHUD::DrawFilterPanel()
{
	if(!Canvas) return;
	const float FW=HUDStyle::FilterW;
	const float FX=Canvas->SizeX-FW-4.f, FY=HUDStyle::TopBarH+4.f;
	const float FH=Canvas->SizeY-HUDStyle::TopBarH-HUDStyle::BottomBarH-8.f;
	const float P=HUDStyle::SPad;
	const float TX=FX+2.5f+P, InW=FW-2.5f-P*2.f;
	DrawPanel(FX+3.f,FY+4.f,FW,FH,FLinearColor(0,0,0,0.30f));
	DrawPanel(FX-1.5f,FY-1.5f,FW+3.f,FH+3.f,FLinearColor(0.20f,0.58f,1.00f,0.10f));
	DrawPanel(FX,FY,FW,FH,HUDStyle::FilterBg);
	DrawPanel(FX,FY,2.5f,FH,HUDStyle::AccentBlue);
	DrawPanel(FX+2.5f,FY,FW-2.5f,1.5f,HUDStyle::FilterBorder);
	DrawPanel(FX,FY+FH-1.f,FW,1.f,HUDStyle::FilterBorder);
	DrawPanel(FX+4.f,FY+1.5f,FW-6.f,1.f,FLinearColor(1,1,1,0.04f));
	DrawPanel(FX,FY,4.f,4.f,FLinearColor(0,0,0,0.75f));
	float CurY=FY+P;
	DrawText(TEXT("STATE FILTER"),HUDStyle::AccentBlue,TX,CurY,nullptr,0.92f);
	const FString CH=TEXT("[TAB]"); float CHW=0.f,CHH=0.f; GetTextSize(CH,CHW,CHH,nullptr,0.76f);
	DrawPanel(FX+FW-CHW-P-4.f,CurY,CHW+8.f,CHH+4.f,HUDStyle::KeyBg);
	DrawPanel(FX+FW-CHW-P-4.f,CurY,CHW+8.f,1.f,HUDStyle::KeyBorder);
	DrawText(CH,HUDStyle::TextDim,FX+FW-CHW-P,CurY+2.f,nullptr,0.76f);
	CurY+=24.f; DrawPanel(TX,CurY,InW,1.f,HUDStyle::Separator); CurY+=P;
	const float SrchH=28.f;
	const bool bSrchH=(HoveredFilterItem==TEXT("__SEARCH_BAR__"));
	DrawPanel(TX,CurY,InW,SrchH,bSearchBarActive?HUDStyle::SearchActive:(bSrchH?HUDStyle::FilterHover:HUDStyle::SearchIdle));
	DrawPanel(TX,CurY,2.f,SrchH,bSearchBarActive?HUDStyle::AccentBlue:(bSrchH?FLinearColor(0.10f,0.35f,0.70f,0.75f):HUDStyle::Separator));
	DrawPanel(TX,CurY,InW,1.f,FLinearColor(1,1,1,bSearchBarActive?0.06f:0.02f));
	const FString DT=SearchBuffer.IsEmpty()?(bSearchBarActive?TEXT("Type to search..."):TEXT("Search states...")):SearchBuffer+(bSearchBarActive?TEXT("_"):TEXT(""));
	DrawText(DT,SearchBuffer.IsEmpty()?HUDStyle::TextDim:HUDStyle::TextPrimary,TX+8.f,CurY+6.f,nullptr,0.86f);
	if(!SearchBuffer.IsEmpty()) {
		const bool bXH=(HoveredFilterItem==TEXT("__SEARCH_CLEAR__"));
		DrawPanel(FX+FW-P-22.f,CurY+4.f,20.f,20.f,bXH?FLinearColor(0.40f,0.10f,0.10f,0.90f):FLinearColor(0.22f,0.06f,0.06f,0.75f));
		DrawText(TEXT("X"),bXH?FLinearColor(1,0.5f,0.5f,1):FLinearColor(0.80f,0.35f,0.35f,1),FX+FW-P-16.f,CurY+5.f,nullptr,0.86f);
	}
	if(bSearchBarActive) DrawText(TEXT("[Esc] Cancel   [Enter] Confirm"),HUDStyle::TextDim,TX+6.f,CurY+SrchH+2.f,nullptr,0.70f);
	CurY+=SrchH+(bSearchBarActive?18.f:4.f)+P;
	const bool bAS=SelectedState.Equals(TEXT("ALL"),ESearchCase::IgnoreCase);
	const bool bAH=(HoveredFilterItem==TEXT("ALL")), bAF=(FilterClickFlashItem==TEXT("ALL"));
	const float AllH=30.f;
	FLinearColor ABg=bAF?HUDStyle::FilterFlash:bAS?HUDStyle::FilterSel:bAH?HUDStyle::FilterHover:HUDStyle::FilterNormal;
	DrawPanel(TX+2.f,CurY+2.f,InW,AllH,FLinearColor(0,0,0,0.20f));
	DrawPanel(TX,CurY,InW,AllH,ABg);
	DrawPanel(TX,CurY,2.f,AllH,bAS||bAF?HUDStyle::AccentBlue:HUDStyle::Separator);
	DrawPanel(TX,CurY,InW,1.f,FLinearColor(1,1,1,bAH?0.07f:0.02f));
	if(bAS) DrawPanel(TX+4.f,CurY+AllH*0.5f-3.f,6.f,6.f,HUDStyle::AccentGreen);
	DrawText(TEXT("ALL STATES"),bAF?FLinearColor(1,1,1,1):bAS?HUDStyle::AccentGold:bAH?HUDStyle::TextPrimary:HUDStyle::TextMuted,TX+(bAS?14.f:8.f),CurY+7.f,nullptr,0.88f);
	const FString AC=FString::Printf(TEXT("%d"),HUDFireCount); float ACW=0.f,ACH=0.f; GetTextSize(AC,ACW,ACH,nullptr,0.76f);
	DrawPanel(TX+InW-ACW-8.f,CurY+5.f,ACW+6.f,AllH-10.f,FLinearColor(0.08f,0.12f,0.25f,0.70f));
	DrawText(AC,bAS?HUDStyle::AccentBlue:HUDStyle::TextDim,TX+InW-ACW-5.f,CurY+6.f,nullptr,0.76f);
	CurY+=AllH+P;
	constexpr int32 Cols=4; constexpr float CellH=28.f;
	const float CellW=InW/Cols;
	TArray<FString> Filtered;
	for(const FString& S:AvailableStates) if(StateMatchesSearch(S, SearchBuffer)) Filtered.Add(S);
	if(Filtered.IsEmpty()) DrawText(TEXT("No matches"),HUDStyle::TextDim,TX+4.f,CurY+6.f,nullptr,0.80f);
	for(int32 i=0;i<Filtered.Num();++i) {
		const float CX=TX+(i%Cols)*CellW, CY=CurY+(i/Cols)*(CellH+3.f);
		if(CY+CellH>FY+FH-92.f) break;
		const FString& S=Filtered[i];
		const bool bSl=S.Equals(SelectedState,ESearchCase::IgnoreCase);
		const bool bHv=(HoveredFilterItem==S), bFl=(FilterClickFlashItem==S);
		DrawPanel(CX+1.f,CY+2.f,CellW-4.f,CellH,FLinearColor(0,0,0,0.22f));
		FLinearColor CB=bFl?HUDStyle::FilterFlash:bSl?HUDStyle::FilterSel:bHv?HUDStyle::FilterHover:HUDStyle::FilterNormal;
		DrawPanel(CX,CY,CellW-3.f,CellH,CB);
		DrawPanel(CX,CY,2.f,CellH,bFl||bSl?HUDStyle::AccentBlue:bHv?FLinearColor(0.10f,0.35f,0.70f,0.75f):FLinearColor(0,0,0,0));
		DrawPanel(CX,CY,CellW-3.f,1.f,FLinearColor(1,1,1,bHv?0.07f:0.02f));
		if(bSl&&!bFl) DrawPanel(CX+3.f,CY+3.f,4.f,4.f,HUDStyle::AccentGreen);
		FLinearColor TC=bFl?FLinearColor(1,1,1,1):bSl?HUDStyle::AccentGold:bHv?HUDStyle::TextPrimary:HUDStyle::TextMuted;
		float StW=0.f,StH=0.f; GetTextSize(S,StW,StH,nullptr,0.82f);
		DrawText(S,TC,CX+(CellW-StW)*0.5f-1.f,CY+(CellH-StH)*0.5f,nullptr,0.82f);
		if(StateFireCounts.Contains(S)) {
			const FString CS=FString::Printf(TEXT("%d"),StateFireCounts[S]);
			float CntW=0.f,CntH=0.f; GetTextSize(CS,CntW,CntH,nullptr,0.60f);
			DrawText(CS,bSl?FLinearColor(0.10f,0.35f,0.70f,0.75f):FLinearColor(0.45f,0.48f,0.62f,0.65f),CX+CellW-CntW-5.f,CY+CellH-CntH-2.f,nullptr,0.60f);
		}
	}
	const int32 NR=FMath::Max(1,(Filtered.Num()+Cols-1)/Cols);
	CurY+=NR*(CellH+3.f)+P;
	if(CurY+48.f<FY+FH-2.f) {
		DrawPanel(TX,CurY,InW,1.f,HUDStyle::Separator); CurY+=P;
		DrawText(TEXT("SENSITIVITY"),HUDStyle::TextDim,TX+2.f,CurY,nullptr,0.76f);
		DrawText(FString::Printf(TEXT("%.1fx"),HUDMouseSensitivity),HUDStyle::AccentGold,TX+InW-30.f,CurY,nullptr,0.82f);
		CurY+=16.f;
		const float SBW=InW-64.f;
		const float SP=(HUDMouseSensitivity-0.1f)/(5.f-0.1f);
		DrawPanel(TX+2.f,CurY+7.f,SBW,6.f,FLinearColor(0.06f,0.06f,0.12f,0.80f));
		DrawPanel(TX+2.f,CurY+7.f,FMath::Max(2.f,SBW*SP),6.f,HUDStyle::AccentBlue);
		const float ThX=TX+2.f+SBW*SP-4.f;
		DrawPanel(ThX,CurY+4.f,8.f,12.f,FLinearColor(1,1,1,0.90f));
		DrawPanel(ThX,CurY+4.f,8.f,1.f,HUDStyle::AccentBlue);
		const bool bPH=(HoveredFilterItem==TEXT("__SENS_UP__")), bMH=(HoveredFilterItem==TEXT("__SENS_DOWN__"));
		const float BPX=TX+SBW+6.f, BMX=BPX+24.f+4.f;
		DrawPanel(BPX,CurY,24.f,22.f,bPH?HUDStyle::FilterHover:HUDStyle::KeyBg);
		DrawPanel(BMX,CurY,24.f,22.f,bMH?HUDStyle::FilterHover:HUDStyle::KeyBg);
		DrawPanel(BPX,CurY,24.f,1.f,HUDStyle::KeyBorder); DrawPanel(BMX,CurY,24.f,1.f,HUDStyle::KeyBorder);
		DrawText(TEXT("+"),bPH?HUDStyle::TextPrimary:HUDStyle::TextMuted,BPX+7.f,CurY+3.f,nullptr,0.90f);
		DrawText(TEXT("-"),bMH?HUDStyle::TextPrimary:HUDStyle::TextMuted,BMX+8.f,CurY+3.f,nullptr,0.90f);
		CurY+=22.f+P;
	}
	if(CurY+20.f<FY+FH) {
		DrawPanel(TX,FY+FH-20.f,InW,1.f,HUDStyle::Separator);
		DrawText(TEXT("[Click] state  [+][-] sensitivity  [TAB] close"),HUDStyle::TextDim,TX+2.f,FY+FH-16.f,nullptr,0.68f);
	}
}

void AFireHUD::DrawHoverTooltip()
{
	if(!Canvas||!GetOwningPlayerController()) return;
	float MX=0.f,MY=0.f; GetOwningPlayerController()->GetMousePosition(MX,MY);
	FString L1=HoveredFireInfo.Name; if(L1.Len()>30) L1=L1.Left(28)+TEXT("...");
	const FString L2=FString::Printf(TEXT("%s   %s   %s"),*HoveredFireInfo.AcresStr,*HoveredFireInfo.SeverityStr,*HoveredFireInfo.StateCode);
	float L1W=0.f,L1H=0.f,L2W=0.f,L2H=0.f;
	GetTextSize(L1,L1W,L1H,nullptr,1.05f); GetTextSize(L2,L2W,L2H,nullptr,0.84f);
	const float BoxW=FMath::Max(L1W,L2W)+HUDStyle::Pad*2.f, BoxH=L1H+L2H+HUDStyle::Pad*2.f+6.f;
	const float BoxX=FMath::Clamp(MX+18.f,0.f,Canvas->SizeX-BoxW);
	const float BoxY=FMath::Clamp(MY-BoxH-10.f,HUDStyle::TopBarH+4.f,Canvas->SizeY-HUDStyle::BottomBarH-BoxH);
	const FLinearColor& SC=HoveredFireInfo.SeverityColor;
	DrawPanel(BoxX+2.f,BoxY+3.f,BoxW,BoxH,FLinearColor(0,0,0,0.28f));
	DrawPanel(BoxX-1.f,BoxY-1.f,BoxW+2.f,BoxH+2.f,FLinearColor(SC.R*0.35f,SC.G*0.35f,SC.B*0.35f,0.20f));
	DrawPanel(BoxX,BoxY,BoxW,BoxH,HUDStyle::BgDeep);
	DrawPanel(BoxX,BoxY,BoxW,2.f,FLinearColor(SC.R,SC.G,SC.B,0.90f));
	DrawPanel(BoxX,BoxY+2.f,2.f,BoxH-2.f,FLinearColor(SC.R,SC.G,SC.B,0.45f));
	DrawPanel(BoxX+2.f,BoxY+2.f,BoxW-2.f,1.f,FLinearColor(1,1,1,0.05f));
	DrawText(L1,HUDStyle::AccentGold,BoxX+HUDStyle::Pad,BoxY+HUDStyle::Pad,nullptr,1.05f);
	DrawText(L2,HUDStyle::TextMuted,BoxX+HUDStyle::Pad,BoxY+HUDStyle::Pad+L1H+4.f,nullptr,0.84f);
}

void AFireHUD::DrawLoadingOverlay()
{
	if(!Canvas) return;

	const float W = Canvas->SizeX;
	const float H = Canvas->SizeY;
	const float OverlayW = 520.f;
	const float OverlayH = 140.f;
	const float X = (W - OverlayW) * 0.5f;
	const float Y = (H - OverlayH) * 0.5f;

	DrawPanel(0.f, 0.f, W, H, FLinearColor(0.f, 0.f, 0.f, 0.38f));
	DrawPanel(X+4.f, Y+6.f, OverlayW, OverlayH, FLinearColor(0.f,0.f,0.f,0.30f));
	DrawPanel(X, Y, OverlayW, OverlayH, FLinearColor(0.015f,0.020f,0.060f,0.96f));
	DrawPanel(X, Y, OverlayW, 2.f, HUDStyle::AccentBlue);
	DrawPanel(X, Y+OverlayH-1.f, OverlayW, 1.f, HUDStyle::Separator);

	const FString Title = TEXT("Loading wildfire visualization...");
	float TW=0.f, TH=0.f; GetTextSize(Title, TW, TH, nullptr, 1.05f);
	DrawText(Title, HUDStyle::TextPrimary, X + (OverlayW-TW)*0.5f, Y + 16.f, nullptr, 1.05f);

	const float BarX = X + 24.f;
	const float BarY = Y + 62.f;
	const float BarW = OverlayW - 48.f;
	const float BarH = 20.f;
	DrawPanel(BarX, BarY, BarW, BarH, FLinearColor(0.06f,0.08f,0.16f,0.92f));
	DrawPanel(BarX, BarY, FMath::Max(2.f, BarW * LoadingProgress01), BarH, HUDStyle::AccentBlue);
	DrawPanel(BarX, BarY, BarW, 1.f, FLinearColor(1.f,1.f,1.f,0.08f));

	const FString Percent = FString::Printf(TEXT("%d%%"), FMath::RoundToInt(LoadingProgress01 * 100.0f));
	float PW=0.f, PH=0.f; GetTextSize(Percent, PW, PH, nullptr, 0.92f);
	DrawText(Percent, HUDStyle::AccentGold, X + (OverlayW-PW)*0.5f, BarY + 24.f, nullptr, 0.92f);

	const FString Msg = LoadingLabel.IsEmpty() ? TEXT("Preparing fires...") : LoadingLabel;
	float MW=0.f, MH=0.f; GetTextSize(Msg, MW, MH, nullptr, 0.82f);
	DrawText(Msg, HUDStyle::TextMuted, X + (OverlayW-MW)*0.5f, Y + OverlayH - MH - 12.f, nullptr, 0.82f);
}

void AFireHUD::DrawCenterPrompt()
{
	if(!Canvas || CenterPromptText.IsEmpty()) return;
	float TW=0.f,TH=0.f; GetTextSize(CenterPromptText, TW, TH, nullptr, 1.0f);
	const float W = FMath::Max(280.f, TW + 44.f);
	const float H = 52.f;
	const float X = (Canvas->SizeX - W) * 0.5f;
	const float Y = (Canvas->SizeY - H) * 0.5f + 96.f;
	DrawPanel(X+3.f,Y+4.f,W,H,FLinearColor(0,0,0,0.25f));
	DrawPanel(X,Y,W,H,FLinearColor(0.02f,0.08f,0.20f,0.92f));
	DrawPanel(X,Y,3.f,H,HUDStyle::AccentBlue);
	DrawPanel(X,Y,W,1.5f,FLinearColor(0.20f,0.60f,1.00f,0.40f));
	DrawText(CenterPromptText, HUDStyle::TextPrimary, X + (W-TW)*0.5f, Y + (H-TH)*0.5f - 1.f, nullptr, 1.0f);
}
