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
class UProjectCompGrenadeComponent;
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
 * Server-side transform snapshot used by the lag-compensation rewind system.
 * One entry per server tick, kept in a small ring buffer on each character so
 * a fire trace can rewind the target's hitbox to where it was when the firer's
 * client took the shot (ServerTime - RTT/2 - InterpDelay).
 */
struct FCharacterTransformSnapshot
{
	float ServerTime = 0.0f;
	FVector Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	float CapsuleHalfHeight = 96.0f;
	/** Pawn velocity at snapshot time. Used (separately from lag-comp) to give
	 *  grenade throws a forgiving "peak velocity over a recent window" for
	 *  jump/strafe/lineup throws — see GetWindowedThrowVelocity. */
	FVector Velocity = FVector::ZeroVector;
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

	/** Server-side dispatcher for grenade use (validates kit + cooldown, spawns the
	 *  AProjectCompGrenadeBase subclass, stamps PlayerState cooldown end-time). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta = (AllowPrivateAccess = "true"))
	UProjectCompGrenadeComponent* GrenadeComponent;

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

	/** Hold-E (default): plant/defuse the bomb while inside a bomb-site trigger. */
	UPROPERTY(EditAnywhere, Category ="Input")
	UInputAction* UseAction;

	/** Press 2: cast team ability slot 2 (mid-round ability). Slot 1 is cast via
	 *  the minimap UI, not a character input — see UProjectCompMinimapWidget. */
	UPROPERTY(EditAnywhere, Category ="Input")
	UInputAction* AbilitySlot2Action;

	/** Press R: reload the equipped weapon if the mag isn't full and reserves > 0. */
	UPROPERTY(EditAnywhere, Category ="Input")
	UInputAction* ReloadAction;

	/** EQUIP the grenade in PlayerState slot 0 (LMB/RMB then throws it).
	 *  Defaults to E in the IMC. Re-pressing while already equipped to slot 0
	 *  is a no-op per design — use SwitchToWeaponAction to disarm. */
	UPROPERTY(EditAnywhere, Category ="Input")
	UInputAction* GrenadeSlot1Action;

	/** EQUIP the grenade in PlayerState slot 1. Defaults to Q in the IMC. */
	UPROPERTY(EditAnywhere, Category ="Input")
	UInputAction* GrenadeSlot2Action;

	/** Switch back to the primary weapon (un-equip any held grenade). Defaults
	 *  to "1" in the IMC. */
	UPROPERTY(EditAnywhere, Category ="Input")
	UInputAction* SwitchToWeaponAction;

	/** Right mouse button. When a grenade is equipped, throws it in alt-fire
	 *  mode (RMB throw). When the weapon is equipped, currently a no-op —
	 *  reserved for future ADS / scope binding. */
	UPROPERTY(EditAnywhere, Category ="Input")
	UInputAction* AltFireAction;

	/** Weapon class spawned for every freshly-possessed character. Set in BP defaults. */
	UPROPERTY(EditDefaultsOnly, Category = "Weapon")
	TSubclassOf<AProjectCompHitscanWeapon> DefaultWeaponClass;

	/** Optional socket on FirstPersonMesh to attach the weapon to, so reload anims
	 *  can move the gun with the hands. NAME_None (default) = attach to the camera
	 *  with the current fixed offset (existing behavior). */
	UPROPERTY(EditDefaultsOnly, Category = "Weapon")
	FName WeaponAttachSocket = NAME_None;

	/** Max distance (cm) the ability aim-trace reaches from the camera before
	 *  falling back to the end of the ray. Tuned per-design later. */
	UPROPERTY(EditDefaultsOnly, Category = "Ability", meta = (ClampMin = "100.0"))
	float AbilityAimDistance = 5000.0f;

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

	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoUseStart();

	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoCastAbilitySlot2();

	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoReload();

	/** Input handler (slot-key PRESS): equip the grenade in PlayerState slot 0
	 *  and start the hold-to-global-place timer. */
	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoEquipGrenadeSlot1();

	/** Input handler (slot-key PRESS): equip the grenade in PlayerState slot 1. */
	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoEquipGrenadeSlot2();

	/** Input handler (slot-key RELEASE): cancels the hold timer; if the global
	 *  placement overlay opened during the hold, confirms the drop at the cursor. */
	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoReleaseGrenadeSlot1();

	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoReleaseGrenadeSlot2();

	/** Input handler: un-equip any held grenade, return to primary weapon. */
	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoSwitchToWeapon();

	/** Input handler: key 3 — equip the carried bomb so G can drop it. No-op unless
	 *  this player is the current bomb carrier. */
	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoEquipBomb();

	/** Input handler: G — drop the currently-equipped item (bomb or weapon).
	 *  Grenades are permanent kit for the round and can't be dropped. */
	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoDropEquipped();

	/** Input handler: F — interact. Picks up a dropped weapon or the bomb you're
	 *  looking at, within close range (Valorant/CS feel). Forwards to the server. */
	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoInteract();

	/** Input handler: RMB press. If a grenade is equipped, ARMS the self-toss
	 *  (hold-to-wind-up); the throw fires on release in DoAltFireEnd — mirrors the
	 *  LMB throw. No grenade equipped → no-op (reserved for ADS / scope). */
	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoAltFireStart();

	/** Input handler: RMB release. Throws the wound-up self-toss (alt-fire) if one
	 *  was armed by DoAltFireStart, then returns to the weapon. */
	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoAltFireEnd();

	/** Common ability-cast plumbing: resolve the slot to the team's drafted
	 *  ability via GameState, aim-trace from the camera to compute a world
	 *  TargetLocation, then forward to the controller's RequestAbilityCast RPC.
	 *  Server validates phase, looming state, pool, and team membership — the
	 *  client side is intentionally naive. */
	void RequestAbilityCastForSlot(int32 SlotIndex);

	/** On respawn, the new pawn's held-input booleans (bUseHeld on this character,
	 *  bTriggerHeld on the freshly-equipped weapon) initialise to false. If the
	 *  player never released Fire / Use across death, Enhanced Input's edge-
	 *  triggered "Started" event won't re-fire on the new pawn — so the held
	 *  intent silently drops until they release+re-press. This helper polls the
	 *  current Enhanced Input action state one frame after possession and
	 *  re-emits Started for any action that's still physically held. */
	void ReconcileHeldInputAfterPossession();

	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoUseEnd();

	/** Maximum yaw change per second the server will accept (degrees). 1440 deg/s
	 *  = 4 full revolutions/second — fast enough for any legitimate flick,
	 *  too slow for an aimbot's snap. */
	UPROPERTY(EditDefaultsOnly, Category = "AntiCheat", meta = (ClampMin = "60.0"))
	float MaxYawRateDegPerSec = 1440.0f;

	/** Maximum pitch change per second the server will accept (degrees). */
	UPROPERTY(EditDefaultsOnly, Category = "AntiCheat", meta = (ClampMin = "60.0"))
	float MaxPitchRateDegPerSec = 720.0f;

	/** Absolute pitch limit (degrees). UE default is 89 — clients can be tampered
	 *  past that. Server enforces here. */
	UPROPERTY(EditDefaultsOnly, Category = "AntiCheat", meta = (ClampMin = "0.0", ClampMax = "90.0"))
	float MaxAbsolutePitch = 89.0f;

	/** Server: forward trigger state from client RPC down to the equipped weapon.
	 *  WithValidation so the engine drops connections sending nonsense, and the
	 *  implementation gates on alive + RoundActive + weapon-present so an
	 *  out-of-context trigger held is silently rejected on the server. */
	UFUNCTION(Server, Reliable, WithValidation)
	void Server_SetTriggerHeld(bool bHeld);

	/** Server: track Use-key held state for plant/defuse interactions. */
	UFUNCTION(Server, Reliable, WithValidation)
	void Server_SetUseHeld(bool bHeld);

	/** Server: drop the carried bomb at the player's feet (G with the bomb equipped). */
	UFUNCTION(Server, Reliable, WithValidation)
	void Server_DropBomb();

	/** Server: drop the equipped weapon onto the ground (G with the weapon equipped). */
	UFUNCTION(Server, Reliable, WithValidation)
	void Server_DropWeapon();

	/** Server: F-interact — pick up the bomb or a dropped weapon the player is
	 *  looking at within InteractRange. Server traces from the validated aim. */
	UFUNCTION(Server, Reliable, WithValidation)
	void Server_TryInteract();

	/** Max distance (cm) for an F-interact pickup. Close range, Valorant/CS-like. */
	UPROPERTY(EditDefaultsOnly, Category = "Interact", meta = (ClampMin = "50.0"))
	float InteractRange = 200.0f;

	/** Min dot(aim, toTarget) for a pickup to count as "looking at it" (~0.7 ≈ 45°). */
	UPROPERTY(EditDefaultsOnly, Category = "Interact", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float InteractConeDot = 0.7f;

	/** Server: forward an R-key press down to the equipped weapon's
	 *  Server_StartReload. Server validates eligibility (alive, mag not
	 *  full, reserves > 0) and silently rejects otherwise. */
	UFUNCTION(Server, Reliable)
	void Server_RequestReload();

public:
	/** Server-callable (NOT an RPC — bomb site is server-authoritative): clear
	 *  the held interact state on both server and owning client when a plant
	 *  or defuse completes. Without this, the player has to release F after
	 *  a successful plant/defuse before they can move / fire again, because
	 *  bUseHeld stays true and the lock holds. Public so AProjectCompBombSite
	 *  can invoke it directly on the active interactor. */
	void Server_NotifyInteractionComplete();

protected:
	/** Mirrors Server_NotifyInteractionComplete on the owning client so its
	 *  local bUseHeld + interact lock match the server. Otherwise the client
	 *  would still predict MaxWalkSpeed=0 until F is released. */
	UFUNCTION(Client, Reliable)
	void Client_ForceReleaseInteractLock();

	/** Server: spawn + equip the DefaultWeaponClass for this character. Called on possession. */
	virtual void Server_EquipDefaultWeapon();

	/** Server: attach a weapon actor to the FP view and make it CurrentWeapon.
	 *  Shared by initial equip and ground pickup. */
	void AttachWeaponToView(AProjectCompHitscanWeapon* Weapon);

	/** Server: detach a weapon, clear its owner (marks it as droppable loot), and
	 *  leave it on the ground in front of the player. Does NOT touch CurrentWeapon. */
	void Server_DropWeaponInternal(AProjectCompHitscanWeapon* Weapon);

public:
	/** Server: equip a weapon lying on the ground. bAllowSwap=false (walk-over
	 *  auto-pickup) only fills an empty slot; true (F-interact) swaps the held
	 *  weapon for the new one. Called from the weapon's pickup overlap + interact. */
	void EquipGroundWeapon(AProjectCompHitscanWeapon* Weapon, bool bAllowSwap);

	/** Server: drop the equipped weapon onto the ground (called on death so the
	 *  gun can be looted). No-op if unarmed. */
	void DropCurrentWeaponToGround();

protected:

	// ---- Engine overrides --------------------------------------------------

	virtual bool CanJumpInternal_Implementation() const override;
	virtual void Landed(const FHitResult& Hit) override;
	virtual bool IsNetRelevantFor(const AActor* RealViewer, const AActor* ViewTarget, const FVector& SrcLocation) const override;
	virtual void OnStartCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust) override;
	virtual void OnEndCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust) override;
	virtual void Tick(float DeltaTime) override;
	virtual void SetupPlayerInputComponent(UInputComponent* InputComponent) override;
	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;
	virtual void PossessedBy(AController* NewController) override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Console command for testing damage flow without weapons. Type "SelfDamage 100"
	 *  in PIE console. Compiled to a no-op in shipping builds (see implementation). */
	UFUNCTION(Exec)
	void SelfDamage(float Amount);

	/** PIE cheat: directly set BOTH teams' drafted abilities + refill pools, bypassing
	 *  the vote/tally path. Runs only on the listen-server host (HasAuthority gate).
	 *  Usage in PIE console: `ForceDraft CA AA`. Two-letter short codes for each
	 *  EAbilityType slot. No-op in shipping. */
	UFUNCTION(Exec)
	void ForceDraft(const FString& Slot1, const FString& Slot2);

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
	UProjectCompGrenadeComponent* GetGrenadeComponent() const { return GrenadeComponent; }

	/** Owning-client equip state: -1 = primary weapon, 0/1 = grenade slot index.
	 *  Read by HUD to highlight the active loadout entry. */
	UFUNCTION(BlueprintPure, Category="Grenade")
	int32 GetEquippedGrenadeSlot() const { return EquippedGrenadeSlot; }
	AProjectCompHitscanWeapon* GetCurrentWeapon() const { return CurrentWeapon; }

	/** Cosmetic: fired when CurrentWeapon changes (equip / draw / swap; NewWeapon
	 *  null on holster/drop). FP + TP draw montages hook this. Fires on the owning
	 *  client + remotes via OnRep_CurrentWeapon, and on the authority from the
	 *  equip path so the listen host's own pawn animates too. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Weapon")
	void OnWeaponEquipped(AProjectCompHitscanWeapon* NewWeapon);
	/** True while the player is locked into a plant/defuse interaction. Read by
	 *  the weapon to block fire + reload during the interaction. */
	bool IsInteractLocked() const { return bInteractLockActive; }
	EMovementState GetMovementState() const { return MovementState; }

	/** Server: true while the Use key is currently held. Polled by ABombSite for plant/defuse. */
	bool IsUseHeld() const { return bUseHeld; }

	/** Deprecated shim: prefer OnBombSiteOverlapChanged(bool). Kept for any external
	 *  callers still using the old name — forwards to the counter-based path with
	 *  a defensive recount. */
	void NotifyBombSiteOverlapChanged();

	/**
	 * Apply one shot's recoil kick to the local control rotation and seed the
	 * recovery state. Called by the weapon's Multicast_OnFired handler on the
	 * owning-client side — no per-shot Client RPC needed (the multicast is
	 * already on the wire for tracers/muzzle flash). Server math runs against
	 * the same deterministic pattern, so client prediction and server state stay
	 * in sync without ack overhead.
	 */
	void ApplyRecoilKickLocal(float PitchKick, float YawKick, float RecoveryRate, float RecoveryDelay);

	/**
	 * Server-only: returns the player's control rotation clamped to the
	 * configured pitch range and yaw/pitch rate budget. Use this — not the
	 * raw GetControlRotation() — for any authoritative trace (hit reg,
	 * line-of-sight, etc.) so a tampered client cannot snap to perfect
	 * headshots between RPCs. Mutates state (writes the clamped rotation
	 * back to the controller and updates last-validated bookkeeping).
	 */
	FRotator Server_GetValidatedAimRotation();

	// ---- Lag compensation (server-only) -----------------------------------
	/** Push the current pose into the history ring. Called once per server
	 *  tick by the GameMode so the ring stays aligned to the server tick rate. */
	void Server_RecordTransformSnapshot(float ServerTime);

	/** Server-only: the highest-magnitude velocity this pawn had within the last
	 *  WindowSeconds (falls back to the current velocity). Grenade throws use this
	 *  instead of the instantaneous release-frame velocity so jump / strafe /
	 *  lineup throws are forgiving — your peak movement still counts even if the
	 *  release is a few frames off the exact peak. Reads the lag-comp ring, which
	 *  the GameMode fills every server tick regardless of this actor's own Tick. */
	FVector Server_GetWindowedThrowVelocity(float WindowSeconds) const;

	/** Interpolate within the ring to the requested server time. Returns false
	 *  if the requested time is older than what the ring holds. */
	bool Server_GetHistoricalSnapshot(float TargetServerTime, FCharacterTransformSnapshot& Out) const;

	/** Move the pawn back to a historical pose; pair with Server_RestoreFromRewind.
	 *  Mesh follows via attachment, so a single capsule move covers the hitbox. */
	void Server_RewindTo(float TargetServerTime);

	/** Undo Server_RewindTo and put the pawn back where it was. */
	void Server_RestoreFromRewind();

	bool IsCurrentlyRewound() const { return bIsRewound; }

protected:

	/** Local-only movement state — anim BP reads it via the getter; not replicated. */
	EMovementState MovementState;

	/** Reserved (unused). */
	float CrouchEyeHeight;

	/** Reserved (unused). */
	float DefaultEyeHeight;

	/** Server-spawned weapon actor currently equipped. Replicated so clients can
	 *  render it; RepNotify drives the OnWeaponEquipped draw hook on remotes. */
	UPROPERTY(ReplicatedUsing = OnRep_CurrentWeapon)
	TObjectPtr<AProjectCompHitscanWeapon> CurrentWeapon;

	/** RepNotify: fire the OnWeaponEquipped cosmetic hook on clients. */
	UFUNCTION()
	void OnRep_CurrentWeapon();

	/** Server-only: tracked Use-key state for plant/defuse interactions. */
	bool bUseHeld = false;

	/** Owning-client-local state. -1 = weapon equipped (default). 0 = holding
	 *  the grenade in PlayerState::EquippedGrenadeTypes[0]. 1 = holding slot 1.
	 *  Drives LMB/RMB routing: while >= 0, fire input throws the grenade
	 *  instead of firing the weapon; weapon fire + reload are gated off.
	 *
	 *  Not replicated — client predicts equip locally, and the actual grenade
	 *  spawn (the only server-authoritative event) goes through the existing
	 *  Server_UseGrenade path which does its own validation. */
	int32 EquippedGrenadeSlot = -1;

	/** Owning-client-local: true while the carried bomb is the equipped item (key 3).
	 *  Gates G to drop the bomb; cleared when switching to the weapon or a grenade. */
	bool bBombEquipped = false;

	/** Owning-client-local: true while LMB is held down with a grenade equipped,
	 *  i.e. a throw is "wound up" and waiting for release. Set on fire-press
	 *  (DoFireStart) while EquippedGrenadeSlot >= 0; the actual throw fires on
	 *  release (DoFireEnd). Cleared on throw or any equip change so a stale
	 *  wind-up can't fire. Not replicated — pure input timing. */
	bool bGrenadeThrowArmed = false;

	/** Owning-client-local: when a throw is wound up (bGrenadeThrowArmed), which
	 *  button armed it — true = RMB self-toss (alt-fire), false = LMB throw. Lets
	 *  each release handler throw only its own mode, so holding one button then
	 *  pressing the other can't cross-fire. Not replicated. */
	bool bArmedThrowIsAlt = false;

	/** Hold a grenade slot key past this many seconds to open the global-drop
	 *  minimap overlay (vs a quick tap, which just equips). Delegated tuning —
	 *  0.5s reads as a deliberate hold while staying snappy. */
	UPROPERTY(EditDefaultsOnly, Category = "Grenade", meta = (ClampMin = "0.1"))
	float GrenadeGlobalHoldThreshold = 0.5f;

	/** Owning-client-local hold state for the tap-vs-hold slot keys. The held
	 *  slot tracks which key's timer is live; placement-open state itself lives
	 *  on the PlayerController (single source of truth). */
	int32 HeldGrenadeSlot = -1;
	FTimerHandle GrenadeHoldTimer;

	/** Shared press/release logic for the two grenade slot keys. */
	void HandleGrenadeSlotPressed(int32 Slot);
	void HandleGrenadeSlotReleased(int32 Slot);

	/** Walk speed defaults cached in BeginPlay. Restored when Use is released. */
	float DefaultMaxWalkSpeed = 560.0f;
	float DefaultMaxWalkSpeedCrouched = 200.0f;

	// ---- Recoil (owning client only) ---------------------------------------
	// Tracks outstanding kick degrees for intra-spray smooth recovery.
	// Reset at the start of each new spray; recovery only runs while firing.
	float AccumulatedRecoilPitch = 0.0f;
	float AccumulatedRecoilYaw = 0.0f;
	float RecoilRecoveryRate = 15.0f;
	float TimeUntilRecoilRecovery = 0.0f;
	bool bIsFiring = false;


	/** Player's desired crouch state to apply when the interact lock releases.
	 *  Seeded from bWantsToCrouch when the lock engages, then kept in sync with
	 *  Ctrl press/release input during the lock (the lock itself owns the live
	 *  crouch state — input only updates this snapshot). On lock release, this
	 *  drives whether the player stands or stays crouched. */
	bool bWasCrouchingBeforeInteract = false;

	/** True while the interaction-imposed movement lock is active.
	 *  Set on DoUseStart / Server_SetUseHeld(true) only when overlapping a bomb site;
	 *  cleared on the matching end. Without this, releasing E outside a site would
	 *  spuriously uncrouch a Ctrl-crouched player and reset MaxWalkSpeed. */
	bool bInteractLockActive = false;

	/** Number of bomb-site triggers currently overlapping this capsule. Updated by
	 *  AProjectCompBombSite begin/end overlap callbacks. Avoids per-call array
	 *  allocation in GetOverlappingActors. */
	int32 BombSiteOverlapCount = 0;

	/** Number of enemy AProjectCompVulnerableGrenade AOEs currently overlapping
	 *  this character. Server-only counter — not replicated; the damage
	 *  multiplier is applied authoritatively in TakeDamage where this is read. */
	int32 VulnerableZoneOverlapCount = 0;

	/** Multiplier applied to incoming damage while VulnerableZoneOverlapCount > 0.
	 *  Design memory says "2x damage taken" — exposed as UPROPERTY for BP tuning. */
	UPROPERTY(EditDefaultsOnly, Category = "Vulnerable", meta = (ClampMin = "1.0", ClampMax = "10.0"))
	float VulnerableDamageMultiplier = 2.0f;

	// ---- Aim-rate clamp bookkeeping (server-only) -------------------------
	/** Last server-time Server_GetValidatedAimRotation ran. Used to compute the
	 *  per-call max-delta budget. -1 = first call, seed without clamping. */
	float LastAimValidationTime = -1.0f;

	/** Last accepted yaw / pitch. Subsequent calls clamp the delta from this. */
	float LastValidatedYaw = 0.0f;
	float LastValidatedPitch = 0.0f;

	// ---- Lag compensation ring (server-only) ------------------------------
	/** Ring depth; 32 entries at 64Hz server = 500ms backlog. Covers worst-case
	 *  competitive RTT (~150ms each way + 50ms interp delay). */
	static constexpr int32 kHistorySize = 32;
	FCharacterTransformSnapshot HistoryBuf[kHistorySize];
	int32 HistoryHead = 0;
	int32 HistoryCount = 0;

	/** Saved pose during an active rewind. Restored by Server_RestoreFromRewind. */
	FVector PreRewindLocation = FVector::ZeroVector;
	FRotator PreRewindRotation = FRotator::ZeroRotator;
	bool bIsRewound = false;

public:
	/** Returns true when the capsule currently overlaps any AProjectCompBombSite trigger.
	 *  Backed by BombSiteOverlapCount which the bomb site updates on begin/end overlap;
	 *  avoids the per-call GetOverlappingActors allocation. */
	bool IsOverlappingBombSite() const { return BombSiteOverlapCount > 0; }

	/** Called by AProjectCompBombSite when its trigger begin/end-overlaps this pawn.
	 *  Maintains the overlap counter that backs IsOverlappingBombSite() and then
	 *  re-evaluates the interact lock state. */
	void OnBombSiteOverlapChanged(bool bEntered);

	/** True if this character is currently inside one or more enemy
	 *  AProjectCompVulnerableGrenade AOEs. Drives the incoming-damage
	 *  multiplier applied in TakeDamage. Server-side counter. */
	bool IsInVulnerableZone() const { return VulnerableZoneOverlapCount > 0; }

	/** Called by AProjectCompVulnerableGrenade on enemy begin/end-overlap.
	 *  Server-only — clamps to non-negative. */
	void OnVulnerableZoneOverlapChanged(bool bEntered);

	/** True if this character is standing inside an enemy AA (Acoustic Deafening)
	 *  zone — a zone whose caster is on the opposing team. Used CLIENT-SIDE by the
	 *  weapon's cosmetic fire hook to mute the gunfire SFX for a deafened listener. */
	bool IsInsideEnemyAcousticZone() const;

	/** True if this character is standing inside an enemy CM (Interact Jammer)
	 *  zone. Consulted SERVER-SIDE to reject F-interact (plant/defuse/pickup). */
	bool IsInsideEnemyInteractJammer() const;
protected:

	/** Apply or release the interact movement lock based on bUseHeld + bomb site overlap.
	 *  Called from input edges, RPC, and overlap-changed events. Idempotent — safe to
	 *  call repeatedly; only transitions when the desired state differs from current. */
	void UpdateInteractLock();

	/** True only when the local player can currently plant (bomb carrier in a site)
	 *  or defuse (defender in range of the planted bomb). Gates the interact lock so
	 *  a plain F-press inside a site no longer auto-crouches. Reads GameState's bomb. */
	bool CanInteractWithBombNow() const;

	/**
	 * Per-client mesh visibility refresh. Runs on every client (including listen
	 * server) on a 0.1s timer. When this character is dead (looming), hide the
	 * body mesh from opponent viewers but keep it visible to teammates and the
	 * looming player themselves. When alive, ensure mesh is visible to everyone.
	 */
	void UpdateMeshVisibilityForLocalViewer();

	FTimerHandle VisibilityRefreshTimer;
};

