# ProjectComp

A 5v5 tactical first-person shooter prototype built in **Unreal Engine 5.7** with **C++**. The focus is competitive integrity: server-authoritative gameplay, client-side prediction and lag compensation.

Solo personal project, built with agentic AI coding in VS Code.

## 🎮 Game format
- 5v5 Attackers vs Defenders with a bomb plant/defuse objective
- First to 13 rounds, with a halftime side swap and two lives per player per round
- **The Loom**, an original mechanic: dead players draft and cast team abilities (9 abilities across Aggro, Control and Midrange pillars) from a shared team cooldown pool, with one cast per player per round
- Buy phase with credits for four throwable grenades: flash, info scan, vulnerable and smoke

## ⚙️ Technical highlights
- **Server-authoritative hitscan weapons** with hit zones, deterministic recoil patterns, movement-based spread and tickrate-independent fire
- **Lag compensation**: a 32-snapshot history buffer (500 ms at 64 Hz) for server-side rewind
- **Custom predicted movement component**: tactical walk, air strafing, an air-speed cap and crouch-jumping, with custom saved moves for client prediction
- **Anti-cheat by design**: aim-rate clamping, and hit endpoints are never broadcast to clients
- **Round state machine** in the GameMode (Loom selection → buy phase → live round → post-round), with the GameState only replicating state
- **C++ UMG HUD**: scoreboard, kill feed, minimap and settings
- **Bandwidth-conscious replication** using quantised vectors and compact integer types

## 📁 Structure
| Path | Contents |
|---|---|
| `Source/ProjectComp/` | C++ gameplay code (~40 classes) |
| `Config/` | Engine, input and game config |
| `Docs/MovementTuning.md` | Reference for every movement parameter |
| `SETUP.md` | Build and contributor setup |

## 🔧 Building
Requires Unreal Engine 5.7, Visual Studio 2022+ with the C++ game development workload, and Git LFS. See [SETUP.md](SETUP.md).

## 🚧 Status
Playable prototype, in active development.
