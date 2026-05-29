// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProjectCompFulfillButton.h"

UProjectCompFulfillButton::UProjectCompFulfillButton()
{
	// Wire the underlying UButton click into our payload-carrying broadcaster
	// once at construction. AddDynamic is safe here even before the widget
	// finishes building — UButton's OnClicked is a UFUNCTION-bound multicast,
	// not the SButton click directly.
	OnClicked.AddDynamic(this, &UProjectCompFulfillButton::HandleNativeClicked);
}

void UProjectCompFulfillButton::HandleNativeClicked()
{
	// Pass `this` so listeners can read RequesterPC + Kind + ItemId off the
	// broadcasting button without needing a per-button handler.
	OnFulfillClicked.Broadcast(this);
}
