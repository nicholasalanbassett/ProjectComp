// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "ProjectCompGameTypes.h"
#include "ProjectCompPlayerController.generated.h"

class UInputAction;
class UInputMappingContext;
class UProjectCompMinimapWidget;
class UProjectCompCornerMinimapWidget;
class UProjectCompSettingsWidget;
class UProjectCompLobbyWidget;
class APlayerState;

/** Fired when the buy-menu toggle (B key / ToggleBuyMenu) is pressed. A WBP HUD
 *  binds to this to flip its own buy-panel visibility (the C++ HUD path uses its
 *  own internal toggle). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnBuyMenuToggled);

/** Fired when the hold-Tab scoreboard is shown/hidden (true = shown). A WBP HUD
 *  binds this; the C++ HUD is poked directly. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnScoreboardToggled, bool, bOpen);

/**
 *  Player controller for the tac-FPS character.
 *
 *  Owns:
 *    - Input mapping contexts (added to the Enhanced Input subsystem on login)
 *    - Custom camera manager class assignment
 *    - Client-to-server RPC entry points for the Loom system
 *      (ability draft votes + cast requests). RPCs validate cheaply on the
 *      client side then forward to the GameMode for full authoritative checks.
 */
UCLASS(abstract, config="Game")
class PROJECTCOMP_API AProjectCompPlayerController : public APlayerController
{
	GENERATED_BODY()

public:

	AProjectCompPlayerController();

	// ---- BP-callable helpers that issue the RPCs ---------------------------

	/** Owning-client entry point: submit your two votes for this PreRound's draft. */
	UFUNCTION(BlueprintCallable, Category = "Ability")
	void SubmitAbilityVote(EAbilityType First, EAbilityType Second);

	/** Owning-client entry point: request a Loom ability cast (player must be looming). */
	UFUNCTION(BlueprintCallable, Category = "Ability")
	void RequestAbilityCast(EAbilityType Type, FVector TargetLocation);

	/** Owning-client entry point: change your team in the pre-match lobby. */
	UFUNCTION(BlueprintCallable, Category = "Lobby")
	void RequestTeamChange(ETeamID NewTeam);

	/** Owning-client (lobby leader) entry point: start the match from the lobby. */
	UFUNCTION(BlueprintCallable, Category = "Lobby")
	void RequestStartMatch();

	/** Owning-client (lobby leader) entry point: return to the lobby from the
	 *  end-of-match results screen. */
	UFUNCTION(BlueprintCallable, Category = "Lobby")
	void RequestReturnToLobby();

	/** Owning-client (lobby leader) entry point: hand lobby leadership to Target. */
	UFUNCTION(BlueprintCallable, Category = "Lobby")
	void RequestSetLobbyLeader(APlayerState* Target);

	/** Owning-client entry point: claim a Loom ability during PreRound selection window.
	 *  First-come-first-served per team (no duplicates). Server arbitrates. */
	UFUNCTION(BlueprintCallable, Category = "Loom")
	void ClaimLoomAbility(EAbilityType Ability);

	/** Owning-client entry point: purchase a grenade during PreRound selection window. */
	UFUNCTION(BlueprintCallable, Category = "Grenade")
	void BuyGrenade(EGrenadeType Type);

	/** Owning-client entry point: purchase a weapon during PreRound selection window.
	 *  Server validates slot/class pairing (Pistol → Secondary, SMG/Rifle → Primary). */
	UFUNCTION(BlueprintCallable, Category = "Loadout")
	void BuyWeapon(EWeaponClass Weapon);

	/** Owning-client entry point: purchase armour during PreRound selection window. */
	UFUNCTION(BlueprintCallable, Category = "Loadout")
	void BuyArmour(EArmourLevel Level);

	// ---- Buy-phase requests (right-click in buy panel) -----------------

	UFUNCTION(BlueprintCallable, Category = "Loadout")
	void RequestWeapon(EWeaponClass Weapon);

	UFUNCTION(BlueprintCallable, Category = "Loadout")
	void RequestArmour(EArmourLevel Level);

	UFUNCTION(BlueprintCallable, Category = "Loadout")
	void RequestGrenade(EGrenadeType Type);

	// ---- Teammate fulfillment (click in teammates panel) ---------------

	UFUNCTION(BlueprintCallable, Category = "Loadout")
	void FulfillWeaponRequest(APlayerController* Requester, EWeaponClass Weapon);

	UFUNCTION(BlueprintCallable, Category = "Loadout")
	void FulfillArmourRequest(APlayerController* Requester, EArmourLevel Level);

	UFUNCTION(BlueprintCallable, Category = "Loadout")
	void FulfillGrenadeRequest(APlayerController* Requester, EGrenadeType Type);

	/** Owning-client entry point: use the grenade currently in EquippedGrenadeTypes[SlotIndex].
	 *  No-op if the slot is empty (player didn't buy 2 grenades). Resolves origin/direction
	 *  from the pawn's camera and forwards to Server_UseGrenade. Slot index 0 or 1. */
	UFUNCTION(BlueprintCallable, Category = "Grenade")
	void UseGrenadeInSlot(int32 SlotIndex, bool bAltFire = false);

	/** Owning-client entry: GLOBAL DROP the grenade in slot SlotIndex at world
	 *  Target (resolved from the minimap cursor). Forwards to the server RPC. */
	UFUNCTION(BlueprintCallable, Category = "Grenade")
	void RequestGlobalDrop(int32 SlotIndex, FVector Target);

	/** Open the minimap overlay in grenade-placement mode for SlotIndex (called
	 *  from the character when the slot key is held past the threshold). Unlike
	 *  the Loom minimap this is allowed while alive. */
	void OpenGrenadePlacement(int32 SlotIndex, const TArray<FKey>& CloseKeys);

	/** Close the grenade-placement overlay (the util is placed by the map click
	 *  itself, or this is a right-click/cancel). Idempotent. */
	void CloseGrenadePlacement();

	/** True while the grenade global-placement overlay is up. */
	bool IsGrenadePlacementOpen() const { return bGrenadePlacementMode; }

	/** True while EITHER full-screen map is up (Loom cast or grenade placement).
	 *  The always-on corner minimap hides itself while this is true. */
	bool IsFullscreenMapOpen() const { return bMinimapOpen || bGrenadePlacementMode; }

	// ---- Dev/test console hooks (PIE listen-server convenience) -----------
	// Use the UE console (~) and call e.g. `BuyNade 1` to buy a Flash.
	// Indices for BuyNade: 1=Flash 2=Info 3=Vulnerable 4=Smoke.
	// Indices for ClaimLoom: 1=AA 2=AC 3=AM 4=CC 5=CA 6=CM 7=MM 8=MA 9=MC.

	UFUNCTION(Exec)
	void BuyNade(int32 TypeIndex);

	UFUNCTION(Exec)
	void ClaimLoom(int32 AbilityIndex);

	/** CHEAT: server-only-effective credit top-up for testing the buy phase. */
	UFUNCTION(Exec)
	void GiveCredits(int32 Amount);

	/** Logs a one-line snapshot of phase, selection window, and the calling
	 *  player's economy/Loom/grenade state to the Output Log. */
	UFUNCTION(Exec)
	void DumpEcon();

	/** One-shot performance snapshot. Dumps frame timing, draw call count,
	 *  primitive/light/PSC counts, texture streaming pool state, and key CVars
	 *  to the Output Log + on-screen. Use this to sanity-check perf without
	 *  babysitting a stat overlay. */
	UFUNCTION(Exec)
	void PerfCheck();

	/** Apply a single-call scalability preset. Friend-friendly wrapper for the
	 *  built-in sg.* commands so weak-hardware players can dial down with one
	 *  console line. Levels:
	 *    0 = Lowest  (ResolutionScale 50%, everything else off)
	 *    1 = Low     (ResolutionScale 75%, others Low)
	 *    2 = Medium  (ResolutionScale 90%, others Medium)
	 *    3 = High    (full resolution, others High)
	 *    4 = Epic    (full resolution, others Epic — UE default) */
	UFUNCTION(Exec)
	void Perf(int32 Level);

	/** Dev exec: buy a weapon. Indices: 1=Pistol 2=SMG 3=Rifle. */
	UFUNCTION(Exec)
	void BuyW(int32 ClassIndex);

	/** Dev exec: buy armour. Indices: 1=Light 2=Heavy. */
	UFUNCTION(Exec)
	void BuyA(int32 LevelIndex);

	/** CHEAT: force-end the current RoundActive with the given team as winner.
	 *  TeamIndex: 0=Attackers 1=Defenders. */
	UFUNCTION(Exec)
	void EndRound(int32 TeamIndex);

	/** CHEAT: toggle pausing the match/phase timer so the current round never
	 *  advances — handy for testing. Type PauseTimer again to resume. */
	UFUNCTION(Exec)
	void PauseTimer();

	/** CHEAT: end the current phase and start the next immediately (from the lobby,
	 *  starts the match). */
	UFUNCTION(Exec)
	void NextPhase();

	/** Throw the equipped grenade of the given type from the caller's pawn
	 *  (origin = camera location, direction = camera forward). Indices:
	 *  1=Flash 2=Info 3=Vulnerable 4=Smoke. RoundActive only.
	 *  AltFire (0/1, default 0): right-click straight throw vs left-click arc. */
	UFUNCTION(Exec)
	void UseNade(int32 TypeIndex, int32 AltFire = 0);

	/** Owning-client entry point: toggle the Loom cast minimap. Only effective
	 *  while looming — alive callers are silently ignored. Bound to the
	 *  MinimapAction input but BP-callable for menus / cheats. */
	UFUNCTION(BlueprintCallable, Category = "Loom")
	void ToggleMinimap();

	/** Owning-client entry point: open/close the PreRound buy menu HUD panel.
	 *  Bound to the B key. Only meaningful during PreRound (the widget itself
	 *  gates rendering on phase) — pressing it any other time silently flips
	 *  state, which is reset to closed on the next PreRound entry anyway. */
	UFUNCTION(BlueprintCallable, Category = "HUD")
	void ToggleBuyMenu();

	/** Broadcast each time ToggleBuyMenu fires. A designer WBP HUD binds here to
	 *  show/hide its buy panel; the C++ HUD ignores it and uses its own state. */
	UPROPERTY(BlueprintAssignable, Category = "HUD")
	FOnBuyMenuToggled OnBuyMenuToggled;

	/** Hold-Tab scoreboard: show on Tab press, hide on release. Bound via BindKey. */
	void ShowScoreboard();
	void HideScoreboard();

	/** Broadcast when the scoreboard shows/hides (true = shown). WBP HUDs bind here. */
	UPROPERTY(BlueprintAssignable, Category = "HUD")
	FOnScoreboardToggled OnScoreboardToggled;

	/** Owning-client entry point: open/close the performance settings overlay.
	 *  Bound to the P key. Game continues running behind it; input mode goes
	 *  to mouse-locked GameAndUI while open so clicks land on the buttons. */
	UFUNCTION(BlueprintCallable, Category = "HUD")
	void ToggleSettings();

	/** Returns the last Perf preset level applied via Perf() (0=Lowest .. 4=Epic).
	 *  -1 if nothing has been applied this session. The Settings widget reads
	 *  this to draw the active highlight. */
	int32 GetCurrentPerfLevel() const { return CurrentPerfLevel; }

protected:

	UPROPERTY(EditAnywhere, Category="Input")
	TArray<UInputMappingContext*> DefaultMappingContexts;

	/** Press to toggle the Loom cast minimap. Looming players only. */
	UPROPERTY(EditAnywhere, Category="Input")
	UInputAction* MinimapAction;

	/** Subclass of UProjectCompMinimapWidget to instantiate for this PC. Set
	 *  to BP_MinimapWidget on the PlayerController BP defaults so designers
	 *  can per-level swap textures + world bounds without touching C++. */
	UPROPERTY(EditDefaultsOnly, Category="Loom")
	TSubclassOf<UProjectCompMinimapWidget> MinimapWidgetClass;

	/** Always-on top-left HUD minimap. Assign a BP_CornerMinimap (same MapTexture +
	 *  bounds as BP_MinimapWidget); falls back to the C++ class if left unset. */
	UPROPERTY(EditDefaultsOnly, Category="HUD")
	TSubclassOf<UProjectCompCornerMinimapWidget> CornerMinimapClass;

	/** Subclass of the team-select lobby widget, shown during the pre-match None
	 *  phase. Assign BP_LobbyWidget in defaults. */
	UPROPERTY(EditDefaultsOnly, Category="HUD")
	TSubclassOf<UProjectCompLobbyWidget> LobbyWidgetClass;

	/** Subclass of UProjectCompSettingsWidget to instantiate. Defaults to the
	 *  C++ class; designers can override with a BP subclass if they need
	 *  custom layout, but the C++ class is fully self-contained. */
	UPROPERTY(EditDefaultsOnly, Category="HUD")
	TSubclassOf<UProjectCompSettingsWidget> SettingsWidgetClass;

	virtual void SetupInputComponent() override;
	virtual void OnPossess(APawn* InPawn) override;
	virtual void PlayerTick(float DeltaTime) override;

	/** Per-frame: pick the right input mode based on the current ERoundPhase
	 *  + selection window. LoomSelection → GameAndUI with mouse locked to
	 *  viewport (player can't drag out, can still WASD). PreRound buy window
	 *  → GameAndUI with mouse hover-free (so defenders can position toward
	 *  their site while buying). Everything else → GameOnly. Skips if the
	 *  Loom minimap is open (that input mode takes priority). */
	void UpdateInputModeForPhase();

	/** Cached marker so UpdateInputModeForPhase only calls SetInputMode on a
	 *  real transition. Avoids per-frame mode reapplication that would steal
	 *  focus or reset cursor position. */
	enum class EAppliedInputMode : uint8 { GameOnly, GameAndUILocked, GameAndUIHoverFree, Unknown };
	EAppliedInputMode LastAppliedInputMode = EAppliedInputMode::Unknown;

	/** Show/hide the minimap. Stricter version of ToggleMinimap that doesn't
	 *  toggle — used by lifecycle events (death/respawn) to force a state.
	 *  bFocusWidget: give the widget keyboard focus (needed for the Loom 1/2 slot
	 *  keys). Pass false for the grenade global-drop map so the game keeps focus —
	 *  focusing the widget flushes held keys (the player stops moving) and is only
	 *  needed for keyboard-driven map UI, which global drop isn't. */
	void SetMinimapOpen(bool bOpen, bool bFocusWidget = true);

	/** Lazily create the widget instance on first open. Returns null on failure. */
	UProjectCompMinimapWidget* EnsureMinimapWidget();

	/** Lazily create + show the always-on corner minimap (once, local player). */
	void EnsureCornerMinimap();

	/** Show/hide the pre-match team-select lobby overlay (driven by the None phase). */
	void SetLobbyOpen(bool bOpen);

	/** Lazily create the lobby widget instance. Returns null on failure. */
	UProjectCompLobbyWidget* EnsureLobbyWidget();

	// ---- Server RPCs (declared with full _Validate + _Implementation) ------

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_SubmitAbilityVote(EAbilityType First, EAbilityType Second);

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_RequestAbilityCast(EAbilityType Type, FVector TargetLocation);

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_ClaimLoomAbility(EAbilityType Ability);

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_BuyGrenade(EGrenadeType Type);

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_BuyWeapon(EWeaponClass Weapon);

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_BuyArmour(EArmourLevel Level);

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_RequestWeapon(EWeaponClass Weapon);

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_RequestArmour(EArmourLevel Level);

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_RequestGrenade(EGrenadeType Type);

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_FulfillWeaponRequest(APlayerController* Requester, EWeaponClass Weapon);

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_FulfillArmourRequest(APlayerController* Requester, EArmourLevel Level);

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_FulfillGrenadeRequest(APlayerController* Requester, EGrenadeType Type);

	/** CHEAT path: tops up the caller's credits on the server. Unconditional —
	 *  this is dev-only and should be stripped or gated for production. */
	UFUNCTION(Server, Reliable, WithValidation)
	void Server_CheatAddCredits(int32 Amount);

	/** CHEAT path: force-end the current RoundActive with the given winner. */
	UFUNCTION(Server, Reliable, WithValidation)
	void Server_CheatEndRound(ETeamID WinningTeam);

	/** CHEAT path: toggle the GameMode phase-timer freeze (PauseTimer exec). */
	UFUNCTION(Server, Reliable, WithValidation)
	void Server_CheatToggleFreezeTimer();

	/** CHEAT path: advance to the next phase (NextPhase exec). */
	UFUNCTION(Server, Reliable, WithValidation)
	void Server_CheatAdvancePhase();

	/** Server RPC: forward a grenade-use request to the pawn's GrenadeComponent.
	 *  Origin and Direction are computed client-side from the pawn pose, then
	 *  re-validated server-side inside the component.
	 *  bAltFire: false = LMB overhand throw; true = the standardized underhand soft lob. */
	UFUNCTION(Server, Reliable, WithValidation)
	void Server_UseGrenade(EGrenadeType Type, bool bAltFire, FVector Origin, FVector Direction);

	/** Server RPC: GLOBAL DROP — deploy the grenade at world Target via the
	 *  pawn's GrenadeComponent (floor-resolved, no flight). Re-validated server-side. */
	UFUNCTION(Server, Reliable, WithValidation)
	void Server_GlobalDropGrenade(EGrenadeType Type, FVector Target);

	/** Server RPC: change the caller's team in the lobby (free swap, any size). */
	UFUNCTION(Server, Reliable, WithValidation)
	void Server_RequestTeamChange(ETeamID NewTeam);

	/** Server RPC: lobby-leader-only — start the match from the lobby. */
	UFUNCTION(Server, Reliable, WithValidation)
	void Server_StartMatch();

	/** Server RPC: lobby-leader-only — return to the lobby from MatchEnded. */
	UFUNCTION(Server, Reliable, WithValidation)
	void Server_ReturnToLobby();

	/** Server RPC: leader-only — transfer lobby leadership to Target. */
	UFUNCTION(Server, Reliable, WithValidation)
	void Server_SetLobbyLeader(APlayerState* Target);

private:

	/** Lazily-created instance of the minimap widget. Owned by this PC, hidden
	 *  by default, added/removed from viewport via SetMinimapOpen. */
	UPROPERTY()
	TObjectPtr<UProjectCompMinimapWidget> MinimapInstance;

	/** Lazily-created always-on corner minimap (top-left HUD). */
	UPROPERTY()
	TObjectPtr<UProjectCompCornerMinimapWidget> CornerMinimapInstance;

	/** True while MinimapInstance is on the viewport. */
	bool bMinimapOpen = false;

	/** Lazily-created team-select lobby widget. */
	UPROPERTY()
	TObjectPtr<UProjectCompLobbyWidget> LobbyInstance;

	/** True while LobbyInstance is on the viewport. */
	bool bLobbyOpen = false;

	/** True while the minimap is open specifically for grenade GLOBAL placement
	 *  (vs the dead-only Loom cast). Allowed while alive; close routes through
	 *  CloseGrenadePlacement. */
	bool bGrenadePlacementMode = false;

	/** Lazily-created instance of the perf settings overlay. */
	UPROPERTY()
	TObjectPtr<UProjectCompSettingsWidget> SettingsInstance;

	/** True while SettingsInstance is on the viewport. Drives input mode. */
	bool bSettingsOpen = false;

	/** Last Perf preset level applied (0..4). -1 = nothing yet. The Settings
	 *  widget reads this to draw the active highlight. */
	int32 CurrentPerfLevel = -1;

	/** Lazily create the settings widget instance on first toggle. */
	UProjectCompSettingsWidget* EnsureSettingsWidget();
};
