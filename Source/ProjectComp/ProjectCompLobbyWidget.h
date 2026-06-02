// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "ProjectCompLobbyWidget.generated.h"

class UBorder;
class UButton;
class UCanvasPanel;
class UTextBlock;
class UVerticalBox;
class APlayerState;

/**
 * Pre-match team-select lobby (full-screen, C++-built UMG). Shown only while the
 * round phase is None (the pre-match lobby). Players pick Attackers/Defenders in
 * real time (free swap, any size) and see every player's ping (whole ms, refreshed
 * every 3s). The listen-server host additionally gets a "Start Match" button.
 *
 * Pure state-reader + RPC-caller: rosters come from GameState->PlayerArray, ping
 * from APlayerState::ExactPing; team changes / start route through the owning
 * PlayerController's Server RPCs.
 */
UCLASS(Abstract, BlueprintType)
class PROJECTCOMP_API UProjectCompLobbyWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	// ---- Tunables (BP defaults) -----------------------------------------
	UPROPERTY(EditDefaultsOnly, Category = "Lobby")
	FLinearColor ScreenDimColor = FLinearColor(0.02f, 0.02f, 0.04f, 0.92f);

	UPROPERTY(EditDefaultsOnly, Category = "Lobby")
	FLinearColor AttackersColor = FLinearColor(0.95f, 0.62f, 0.25f, 1.0f);

	UPROPERTY(EditDefaultsOnly, Category = "Lobby")
	FLinearColor DefendersColor = FLinearColor(0.30f, 0.62f, 0.95f, 1.0f);

	/** How often (s) the roster membership/names are rebuilt — kept brisk so team
	 *  swaps read as real-time. */
	UPROPERTY(EditDefaultsOnly, Category = "Lobby", meta = (ClampMin = "0.1"))
	float RosterRefreshInterval = 0.4f;

	/** How often (s) each player's displayed ping is re-sampled. Per spec: 3s. */
	UPROPERTY(EditDefaultsOnly, Category = "Lobby", meta = (ClampMin = "0.5"))
	float PingRefreshInterval = 3.0f;

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;

	void BuildLayout();

	/** Re-sample every player's ExactPing into PingCache (whole ms). */
	void RefreshPingCache();

	/** Rebuild the two team lists from GameState->PlayerArray using PingCache. */
	void RebuildRoster();

	/** Show the Start button only on the host's local widget. */
	/** True if the local player is the current lobby leader. */
	bool IsLocalLeader() const;

	/** Position + show the "Make Lobby Leader" context box at the cursor. */
	void OpenContextMenuAt(const FVector2D& AbsScreenPos);

	UFUNCTION() void OnJoinAttackersClicked();
	UFUNCTION() void OnJoinDefendersClicked();
	UFUNCTION() void OnStartMatchClicked();
	UFUNCTION() void OnMakeLeaderClicked();

	// ---- Widget tree ----------------------------------------------------
	UPROPERTY() TObjectPtr<UCanvasPanel> RootCanvas;
	UPROPERTY() TObjectPtr<UTextBlock> AttackerHeader;
	UPROPERTY() TObjectPtr<UTextBlock> DefenderHeader;
	UPROPERTY() TObjectPtr<UVerticalBox> AttackerList;
	UPROPERTY() TObjectPtr<UVerticalBox> DefenderList;
	UPROPERTY() TObjectPtr<UButton> JoinAttackersButton;
	UPROPERTY() TObjectPtr<UButton> JoinDefendersButton;
	UPROPERTY() TObjectPtr<UButton> StartMatchButton;
	UPROPERTY() TObjectPtr<UTextBlock> StartMatchLabel;
	/** Right-click "Make Lobby Leader" box; collapsed unless open. */
	UPROPERTY() TObjectPtr<UBorder> ContextMenu;
	UPROPERTY() TObjectPtr<UButton> MakeLeaderButton;

private:
	float RosterTimer = 0.0f;
	float PingTimer = 0.0f;

	/** Last-sampled ping per player (whole ms), updated every PingRefreshInterval. */
	TMap<TWeakObjectPtr<APlayerState>, int32> PingCache;

	/** A roster row's text widget + the player it represents — for right-click hit
	 *  testing. Rebuilt each roster refresh; weak so it never dangles. */
	struct FRosterRow
	{
		TWeakObjectPtr<UTextBlock> Widget;
		TWeakObjectPtr<APlayerState> PS;
	};
	TArray<FRosterRow> RosterRows;

	/** The player a freshly-opened context menu would promote to leader. */
	TWeakObjectPtr<APlayerState> ContextTargetPS;

	/** Build one roster row into a team list (also tracked in RosterRows). */
	void AddRosterRow(UVerticalBox* List, APlayerState* PS, const FString& Name, int32 PingMs, bool bIsLocal, bool bIsLeader, const FLinearColor& TeamColor);
};
