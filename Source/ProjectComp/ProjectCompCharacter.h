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
class UProjectCompHealthComponent;
class AProjectCompHitscanWeapon;
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

	/** Health container — replicated; routes damage into the GameMode death flow. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta = (AllowPrivateAccess = "true"))
	UProjectCompHealthComponent* HealthComponent;

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

	/** Hold-mouse1: fire equipped weapon. */
	UPROPERTY(EditAnywhere, Category ="Input")
	UInputAction* FireAction;

	/** Weapon class spawned for every freshly-possessed character. Set in BP defaults. */
	UPROPERTY(EditDefaultsOnly, Category = "Weapon")
	TSubclassOf<AProjectCompHitscanWeapon> DefaultWeaponClass;

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

	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoFireStart();

	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoFireEnd();

	/** Server: forward trigger state from client RPC down to the equipped weapon. */
	UFUNCTION(Server, Reliable)
	void Server_SetTriggerHeld(bool bHeld);

	/** Server: spawn + equip the DefaultWeaponClass for this character. Called on possession. */
	virtual void Server_EquipDefaultWeapon();

	// ---- Engine overrides --------------------------------------------------

	virtual bool CanJumpInternal_Implementation() const override;
	virtual void Landed(const FHitResult& Hit) override;
	virtual void OnStartCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust) override;
	virtual void OnEndCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust) override;
	virtual void Tick(float DeltaTime) override;
	virtual void SetupPlayerInputComponent(UInputComponent* InputComponent) override;
	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;
	virtual void PossessedBy(AController* NewController) override;
	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Console command for testing damage flow without weapons. Type "SelfDamage 100" in PIE console. */
	UFUNCTION(Exec)
	void SelfDamage(float Amount);

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
	UProjectCompHealthComponent* GetHealthComponent() const { return HealthComponent; }
	EMovementState GetMovementState() const { return MovementState; }

protected:

	/** Local-only movement state — anim BP reads it via the getter; not replicated. */
	EMovementState MovementState;

	/** Reserved (unused). */
	float CrouchEyeHeight;

	/** Reserved (unused). */
	float DefaultEyeHeight;

	/** Server-spawned weapon actor currently equipped. Replicated so clients can render it. */
	UPROPERTY(Replicated)
	TObjectPtr<AProjectCompHitscanWeapon> CurrentWeapon;

	/**
	 * Per-client mesh visibility refresh. Runs on every client (including listen
	 * server) on a 0.1s timer. When this character is dead (looming), hide the
	 * body mesh from opponent viewers but keep it visible to teammates and the
	 * looming player themselves. When alive, ensure mesh is visible to everyone.
	 */
	void UpdateMeshVisibilityForLocalViewer();

	FTimerHandle VisibilityRefreshTimer;
};

