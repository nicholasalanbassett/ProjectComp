// Copyright Epic Games, Inc. All Rights Reserved.


#include "ProjectCompCameraManager.h"

AProjectCompCameraManager::AProjectCompCameraManager()
{
	// Full vertical look — straight up and down. Kept at ±89 (just shy of 90 to
	// avoid gimbal flip) to match the server-side anti-cheat clamp
	// (AProjectCompCharacter::MaxAbsolutePitch = 89), so the view never exceeds
	// what the server validates for hit detection.
	ViewPitchMin = -89.0f;
	ViewPitchMax = 89.0f;
}
