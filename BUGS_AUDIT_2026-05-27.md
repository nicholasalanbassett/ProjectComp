# Playtest Bug Audit — 2026-05-27

Captured the evening of the first multiplayer playtest. Five issues to tackle tomorrow.

---

## Issue 1 — FPS spread (host 120, friend A ~60, friend B ~5)

### Diagnosis

**Most likely: hardware/driver disparity, not code.** A 12x gap between two clients on the same build, on the same server, with identical replication traffic almost certainly means very different hardware. The game's per-frame server-side work is small (lag-comp ring snapshot, phase countdown — see [ProjectCompGameMode.cpp:61-121](Source/ProjectComp/ProjectCompGameMode.cpp#L61-L121)) and replicated state is tiny (uint8 HP, uint16 deciseconds timer, 2-team loadout struct).

### Things to verify with friend B (the 5fps one)

1. **GPU**: dedicated vs integrated graphics. Onboard Intel UHD will tank UE 5 at any quality.
2. **Resolution**: native 4K with no UI to drop it = guaranteed pain on weak GPUs. We have no in-game resolution scale toggle yet.
3. **Driver age**: outdated GPU drivers cost 20-50% on UE 5 routinely.
4. **Background processes**: streaming software (OBS), Chrome with 80 tabs, anti-virus full scan.
5. **Shader compilation stall**: first-launch on a packaged build can hitch hard while shaders compile. Should recover after ~30s of play.
6. **Thermal throttling**: laptop on lap or in stand without airflow.

### Code-side contributing factors (smaller magnitude, but real)

- **`DrawDebugSphere/Line` calls on host's machine drawing many shapes** — VULN zone redraws every frame at 24+10 segments. Multiple active zones + flashes = real cost. **Friend B likely sees no debug shapes (server-only for FLASH/INFO) so this isn't their bottleneck, but is worth knowing.**
- **No quality presets / scalability**: project doesn't ship a scalability config, so the engine defaults run on weak GPUs as if they were beefy. Adding `r.ScreenPercentage` toggle + low/med/high preset would help friend B immediately.

### Fix priorities for tomorrow

1. Have friend B run `stat unit` in console mid-game. If GPU is the bottleneck (`GPU` line dominant), it's their hardware. If `Game thread` is dominant, it's CPU.
2. Add a simple resolution-scale CVar binding accessible via console: friends can type `r.ScreenPercentage 75` (or 50) for a quick perf boost.
3. Add scalability presets (`sg.ResolutionQuality 0..4`, `sg.PostProcessQuality 0..4`, etc.) — UE has these baked in. We just need to document the commands.

---

## Issue 2 — Game freezes when enabling second monitor

### Diagnosis

**This is a UE-engine RHI behavior, not our code.** When Windows changes display topology (monitor enabled/disabled, resolution changed, primary monitor switched), Windows sends `WM_DISPLAYCHANGE`. UE's D3D11/D3D12 RHI may receive a device-reset notification and stall the render thread while it rebuilds swap chains. The game thread keeps running (hence console works), but the rendering pipeline is wedged and inputs visually appear "frozen."

### What we know from the symptom

- Console still works → game thread alive, input still reaching the world
- Player frozen → either input not propagating to pawn, OR the visible frame is stale and the world is actually moving (would need to check log to confirm)
- Persists indefinitely → not a transient stall; the RHI didn't recover

### Likely root causes

1. **Borderless windowed mode + display change**: UE's borderless mode handles `WM_DISPLAYCHANGE` weakly. Switching to exclusive fullscreen usually avoids this. Try `r.FullscreenMode 0` (or 2 for windowed-fullscreen).
2. **D3D12 + display topology change**: D3D12 in UE 5.7 has had reported issues with hot-plug monitor events. Switching to D3D11 RHI is a workaround (`-d3d11` command-line flag, or set RHI in project settings).
3. **Multi-GPU systems**: if the integrated GPU is doing the desktop while a dedicated GPU runs the game, enabling a second monitor can force a GPU reassignment.

### Not our code

I checked `SetInputMode`/`bShowMouseCursor` references — only the Loom minimap touches input mode, and that doesn't trigger on display change. We don't override any focus handling that could explain this.

### Fix priorities for tomorrow

1. **Quickest test**: run the packaged build with `-d3d11` command-line arg (`ProjectComp.exe Substrate?listen -d3d11`). If the freeze stops, it's a D3D12 RHI issue → switch project RHI to D3D11 or use the flag.
2. **Try fullscreen modes**: switch between exclusive fullscreen, borderless windowed, and windowed via `r.FullscreenMode` to find one that survives display topology changes.
3. If neither works, document as "don't enable a second monitor mid-game" and move on — it's an edge case.

---

## Issue 3 — All debug viz visible to clients (NetMulticast wrap)

### Confirmed by playtest

Friends see their **own** VULN zone (matches code — VULN ticks on every machine via replicated `bIsZoneActive`). Friends do NOT see FLASH or INFO viz when they throw — confirms the diagnosis: those DrawDebug calls execute inside server-only paths.

### What needs wrapping

| Source | Currently runs on | Needs |
|---|---|---|
| [FlashGrenade::HandleDetonationGameplay](Source/ProjectComp/ProjectCompFlashGrenade.cpp) — radius sphere + per-target rays + center marker | Server only | Multicast |
| [InfoGrenade::OnEnterActivePhase](Source/ProjectComp/ProjectCompInfoGrenade.cpp) — scan dome + count text + per-target rays | Server only | Multicast |
| [ThrowableGrenadeBase::Tick](Source/ProjectComp/ProjectCompThrowableGrenadeBase.cpp) — projectile tracer + wall-phase markers + bounce markers | Server only (Tick early-returns on clients) | Multicast |
| [VulnerableGrenade::Tick](Source/ProjectComp/ProjectCompVulnerableGrenade.cpp) — neon coral zone | All machines ✅ | No change |

### Implementation plan

Add `NetMulticast, Unreliable` RPCs that broadcast the debug payloads. Each subclass calls its multicast at the relevant moment (impact, detonate, scan-complete, etc.); the implementation does the `DrawDebug*` calls locally on every machine that receives it.

Approximate API:

```cpp
// ThrowableGrenadeBase.h
UFUNCTION(NetMulticast, Unreliable)
void Multicast_DebugTracer(FVector A, FVector B, FColor C);

UFUNCTION(NetMulticast, Unreliable)
void Multicast_DebugPhaseMarkers(FVector Entry, FVector Exit);

UFUNCTION(NetMulticast, Unreliable)
void Multicast_DebugBounceMarker(FVector At, FVector Normal);
```

Then in Tick (server side), replace direct `DrawDebugXxx` calls with the multicast calls. Implementation body does the actual draw.

Tracer is per-tick at potentially 60Hz × N projectiles → could be heavy on the wire. Two mitigations:
1. **Unreliable** (already specified) — drops if congested.
2. **Throttle**: only multicast every 2nd or 3rd tick. Visual fidelity loss is minor; bandwidth halves.
3. **Alternative**: replicate projectile spawn + velocity once, have clients tick locally and draw their own tracer. Cheaper on the wire but more code.

FLASH and INFO detonation are single events (not per-tick), so a single multicast per detonation is fine — minimal bandwidth.

### Effort

~45 min of work. Three files modified. One repackage + redistribute.

---

## Issue 4 — Nausea after ~1 hour

### Diagnosis

A real concern — usually means one of: low FOV, mouse smoothing, frame pacing, or motion-mismatch (camera vs. capsule motion). I checked the code and the most likely culprits in priority order:

### Suspect 1 (HIGH) — FOV is too low

Line 60 of [ProjectCompCharacter.cpp:60](Source/ProjectComp/ProjectCompCharacter.cpp#L60): `FirstPersonFieldOfView = 70.0f`.

**70 degrees is on the low end and is a known nausea trigger for many players.** Tac FPS norms:
- CS2: ~90 degrees default, adjustable
- Valorant: 103 degrees fixed
- Apex/COD: 90-110 default, 80-120 adjustable

A 70° FOV simulates a narrower field, which the brain interprets as "I'm moving fast through a tunnel" and triggers vestibular conflict — classic motion sickness path.

**Fix**: bump default FOV to 90, expose as an in-game setting (or just a CVar binding to `FirstPersonCameraComponent->SetFieldOfView` from the console). Will likely solve most of the nausea.

### Suspect 2 (MEDIUM) — Crouch camera ease + capsule snap mismatch

[ProjectCompCharacter.cpp:1321-1337](Source/ProjectComp/ProjectCompCharacter.cpp#L1321-L1337): `CrouchVisualAlpha` lerps separately from `bIsCrouched` (engine state, capsule snaps). Camera Z eases over ~0.2s while the world (collision, view-from-capsule baseline) snapped instantly. That ~0.2s of mismatch can read as "the world is moving but I'm not" — small but cumulative over an hour.

**Fix**: try snapping the camera with the capsule (no ease) and see if it feels worse — if not, leave snap-only. Alternatively reduce `CrouchInterpSpeed` from 10 to 15-20 to shorten the ease window further.

### Suspect 3 (MEDIUM) — Recoil recovery rate too slow

`RecoilRecoveryRate = 4.0f` deg/sec — that's *very* slow. A burst that kicks 10° up takes 2.5s to recover. During those 2.5s the camera is constantly drifting. Continuous slow camera motion you didn't initiate is a strong nausea contributor.

**Fix**: bump `RecoilRecoveryRate` to 15-20 deg/sec for a snappier recovery. Or expose as a tunable on the weapon BP. Test with `stat raw` to verify recovery feels right.

### Suspect 4 (LOW) — No frame pacing / VSync handling

If frame times are uneven (host 120, friend B 5), the 5fps friend especially is getting nausea from frame stutter. Once we fix Issue 1 (give them a way to dial graphics down) this should improve naturally.

### Suspect 5 (LOW) — Mouse input handling

I didn't see `bEnableMouseSmoothing` or `bEnableFOVScaling` overrides. UE defaults to mouse smoothing OFF in modern versions, but worth checking `DefaultInput.ini`:
- `bEnableMouseSmoothing=False` (default in UE 5)
- `bEnableFOVScaling=False`

If either is true in your input config, smoothing creates that "floaty mouse" feeling which is nauseating over time.

### Fix priorities for tomorrow

1. **FOV to 90 default** — single line change, most likely to fix nausea.
2. **Recoil recovery rate up to ~15** — second highest impact.
3. **Verify `bEnableMouseSmoothing=False`** in `DefaultInput.ini`.
4. Lower `CrouchInterpSpeed` (or remove crouch camera ease entirely).
5. Add an FOV slider to settings so individual players can tune.

---

## Issue 5 — Friends can see VULN viz but not FLASH/INFO

Same root cause as Issue 3. VULN's per-tick draw runs on every client because:
1. `bIsZoneActive` is replicated
2. `SetActorTickEnabled(true)` is called in BeginPlay (runs on every machine)
3. Tick draws `DrawDebugSphere` locally on each client whose `bIsZoneActive` is true

FLASH and INFO don't follow this pattern — their DrawDebug calls live inside server-only handlers. **The fix in Issue 3 resolves this entirely.**

---

## Tomorrow's priority order

1. **FOV bump 70 → 90** (single-line, biggest QOL win, fixes nausea for most players)
2. **NetMulticast debug viz** (fixes Issue 3+5, friends see flashes/scans)
3. **Recoil recovery rate bump** (continued nausea fix)
4. **D3D11 / fullscreen test for monitor freeze** (try `-d3d11` flag)
5. **Add resolution scale CVar binding** for friend B's perf (`r.ScreenPercentage` documented)
6. Repackage, redistribute, retest.

Should be a 2-3 hour session tomorrow. Goodnight.
