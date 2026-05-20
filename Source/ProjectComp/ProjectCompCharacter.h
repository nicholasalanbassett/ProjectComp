// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Logging/LogMacros.h"
#include "ProjectCompCharacter.generated.h"

class UInputComponent;
class USkeletalMeshComponent;
class UCameraComponent;
class UInputAction;
struct FInputActionValue;

/** High-level movement state for animation/gameplay code to read. */
UENUM(BlueprintType)
enum class EMovementState : uint8
{
	Walking = 0,    // On the ground (run, walk, or crouch — anim BP differentiates by speed/bIsCrouched)
	Sprinting = 1,  // Reserved; currently unused
	Airborne = 2,   // In the air (jumping or falling)
	Landed = 3      // Reserved; currently unused
};

/**
 * First-person tac-FPS character.
 *
 * Movement tuning (high accel, dead-stop braking, heavy gravity) lives in the
 * constructor. Prediction-safe walk modifier and air-speed cap live in
 * UProjectCompCharacterMovementComponent.
 */
UCLASS(abstract)
class AProjectCompCharacter : public ACharacter
{
	GENERATED_BODY()

	/** First-person arms mesh, parented to the camera (owner-only visibility). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta = (AllowPrivateAccess = "true"))
	USkeletalMeshComponent* FirstPersonMesh;

	/** First-person camera, parented to the capsule (world-aligned, no head-bone sway). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta = (AllowPrivateAccess = "true"))
	UCameraComponent* FirstPersonCameraComponent;

protected:

	// ---- Input actions (assigned in the BP) -------------------------------

	UPROPERTY(EditAnywhere, Category ="Input")
	UInputAction* JumpAction;

	UPROPERTY(EditAnywhere, Category ="Input")
	UInputAction* MoveAction;

	UPROPERTY(EditAnywhere, Category ="Input")
	UInputAction* LookAction;

	UPROPERTY(EditAnywhere, Category ="Input")
	UInputAction* MouseLookAction;

	/** Hold-shift: tactical walk (slower). */
	UPROPERTY(EditAnywhere, Category ="Input")
	UInputAction* WalkAction;

	/** Hold-ctrl: crouch. */
	UPROPERTY(EditAnywhere, Category ="Input")
	UInputAction* CrouchAction;

public:
	AProjectCompCharacter(const FObjectInitializer& ObjectInitializer);

protected:

	// ---- Input thunks (bound by SetupPlayerInputComponent) ----------------

	void MoveInput(const FInputActionValue& Value);
	void LookInput(const FInputActionValue& Value);

	// ---- Action handlers (BP-callable so UI/AI can drive them) ------------

	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoAim(float Yaw, float Pitch);

	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoMove(float Right, float Forward);

	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoJumpStart();

	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoJumpEnd();

	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoWalkStart();

	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoWalkEnd();

	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoCrouchStart();

	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoCrouchEnd();

	// ---- Engine overrides --------------------------------------------------

	virtual void Landed(const FHitResult& Hit) override;
	virtual void OnStartCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust) override;
	virtual void OnEndCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust) override;
	virtual void Tick(float DeltaTime) override;
	virtual void SetupPlayerInputComponent(UInputComponent* InputComponent) override;

	// ---- Camera tuning (exposed to BP) ------------------------------------

	/** Eye Z relative to the capsule center while standing (cm). */
	UPROPERTY(EditDefaultsOnly, Category="Camera")
	float StandingCameraZ = 64.0f;

	/** FInterpTo speed for the crouch camera ease (~10 ≈ 0.2s, higher = snappier). */
	UPROPERTY(EditDefaultsOnly, Category="Camera")
	float CrouchInterpSpeed = 10.0f;

	// ---- Crouch transition state (not for editing) ------------------------

	/** Current visual progress: 0 = standing, 1 = crouched. Decoupled from the engine's bIsCrouched so the capsule snaps for hit-reg while the view eases. */
	float CrouchVisualAlpha = 0.0f;

	/** What CrouchVisualAlpha is easing toward (set by OnStartCrouch/OnEndCrouch). */
	float TargetCrouchAlpha = 0.0f;

	/** (DefaultHalfHeight − CrouchedHalfHeight). Cached from the value the engine passes into OnStartCrouch so Tick doesn't recompute it. */
	float CachedCrouchHalfHeightDiff = 0.0f;

	/** Single landing site for animation-state side effects (currently a stub). */
	void UpdateAnimationState();

public:

	USkeletalMeshComponent* GetFirstPersonMesh() const { return FirstPersonMesh; }
	UCameraComponent* GetFirstPersonCameraComponent() const { return FirstPersonCameraComponent; }
	EMovementState GetMovementState() const { return MovementState; }

protected:

	/** Local-only movement state — anim BP reads it via the getter; not replicated. */
	EMovementState MovementState;

	/** Reserved (unused). */
	float CrouchEyeHeight;

	/** Reserved (unused). */
	float DefaultEyeHeight;
};

