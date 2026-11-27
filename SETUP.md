# ProjectComp — Contributor Setup

A 5v5 tactical FPS in **Unreal Engine 5.7** (C++ + Blueprints). This guide gets a
new machine cloning, building, and working on a branch.

> ⚠️ This is a **C++ project**. The compiled editor module is **not** in git
> (`Binaries/` is ignored), so after cloning you **must build once** before the
> editor will open — even if you only do art/Blueprints. That needs Visual Studio.

---

## 1. Prerequisites (install these first)

| Tool | Notes |
|------|-------|
| **Unreal Engine 5.7** | Exact version, via the Epic Games Launcher. Opening with a different version converts assets and breaks the project for everyone. |
| **Visual Studio 2022 or 2026** | With the **"Game development with C++"** workload (includes MSVC + the .NET bits). Required to compile the editor module. |
| **Git** | https://git-scm.com |
| **Git LFS** | https://git-lfs.com — run `git lfs install` once after installing. **Required** (binary assets are stored in LFS). |

Verify Git LFS is ready:
```
git lfs version        # should print a version
git lfs install        # one-time, sets up the filters
```

---

## 2. Clone

In VS Code: `Ctrl+Shift+P` → **Git: Clone** → paste the URL → pick a folder → sign in to GitHub.

Or CLI:
```
git clone https://github.com/nicholasalanbassett/ProjectComp.git
cd ProjectComp
git lfs pull          # only needed if LFS wasn't installed before cloning
```
With git-lfs installed, the real binary assets download automatically. If you see
tiny text "pointer" files where `.uasset`/textures should be, run `git lfs pull`.

---

## 3. First build (the step people miss)

Pick one:

**A. Via Visual Studio**
1. Right-click `ProjectComp.uproject` → **Generate Visual Studio project files**.
2. Open the generated `ProjectComp.sln`.
3. Set configuration to **Development Editor** / **Win64** → **Build** (Ctrl+Shift+B).
4. Launch the editor from VS (F5) or by opening the `.uproject`.

**B. Let the editor build it**
1. Double-click `ProjectComp.uproject`.
2. When prompted *"modules are missing or out of date, rebuild?"* → **Yes**
   (this uses the VS toolchain under the hood).

After the first successful build, day-to-day you can just open the `.uproject`.

---

## 4. Working on a branch

Never commit straight to `main`. Branch, push, open a Pull Request.

```
git checkout main
git pull                              # get latest
git checkout -b feature/<your-thing>  # e.g. feature/buy-menu-wbp
# ...work, then...
git add <your files>
git commit -m "Short description of the change"
git push -u origin feature/<your-thing>
```
Then on GitHub: open a **Pull Request** into `main`. The owner reviews + merges.
Pull `main` regularly so your branch doesn't drift.

In VS Code you can do all of this from the Source Control panel + the branch
indicator in the status bar (Create Branch / Publish Branch / Sync).

---

## 5. Asset rules (read this — it prevents lost work)

Unreal `.uasset` / `.umap` files are **binary — git cannot merge them**. If two
people edit the same asset on different branches, merging silently keeps only one
side. To avoid losing work:

- **Divide ownership.** Agree who owns which assets. Don't both edit the same
  `.uasset` at once — especially shared ones (`BP_FirstPersonCharacter`, the
  GameMode BP, maps).
- **Lock a hot asset before editing it** (LFS file locking):
  ```
  git lfs lock   Content/FirstPerson/Blueprints/BP_FirstPersonCharacter.uasset
  # ...edit, commit, push...
  git lfs unlock Content/FirstPerson/Blueprints/BP_FirstPersonCharacter.uasset
  ```
  See current locks with `git lfs locks`.
- **Keep branches short-lived and merge often** — the longer a branch lives, the
  more likely an asset collision.
- **Everyone on UE 5.7.** Don't open with another version.
- **Don't commit** `Binaries/`, `Intermediate/`, `Saved/`, `DerivedDataCache/`
  (already handled by `.gitignore`).

### C++ vs content split
- **C++** lives in `Source/ProjectComp/` (`.h`/`.cpp`). Editing it means a rebuild.
- **Content** lives in `Content/` (`.uasset`). No rebuild, but binary (see above).
- If you need a new C++ hook for your Blueprint/anim work, ask — don't hand-edit
  the C++ unless that's your area.

---

## 6. Quick reference

```
git status                 # what changed
git pull                   # update current branch
git checkout -b feature/x  # new branch
git push -u origin feature/x
git lfs locks              # who has what locked
git lfs lock <path>        # claim an asset
git lfs unlock <path>      # release it
```

Questions about the C++ side (abilities, weapons, HUD plumbing, netcode): ask the
project owner.
