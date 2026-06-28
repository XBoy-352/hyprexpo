# Unified all-workspaces overview — implementation plan (feature branch)

> Opt-in mode `plugin:hyprexpo:all_monitors`: on the invoking monitor, render the first N workspaces as live thumbnails regardless of which physical monitor owns each one. The grid keeps hyprexpo's existing square layout — N = `columns` × `columns` (so the default `columns = 3` gives a 3×3 grid of workspaces 1..9); a different `columns` value scales the cell count accordingly. View-only (no window dragging). Click semantics: plain click → focus the owner monitor and switch there; modifier click (Shift or right button) → pull that workspace onto the current monitor. Default off ⇒ stock behaviour byte-identical.

---

## 1. Feasibility verdict

**feasible-with-caveats (confidence: medium).** No new renderer API is required, but the click-routing primitives in the draft were wrong and are corrected below, and a **critical lifecycle gap (foreign-monitor unplug while the grid is open) must be closed or the feature can crash**.

**Capture approach (unchanged pipeline, fed foreign IDs).** Per cell, the existing offscreen path runs:
`beginRender(MON=pMonitor.lock(), …, image.fb)` → `g_pHyprRenderer->renderWorkspace(MON, PWORKSPACE, Time::steadyNow(), monbox)` → `endRender()` → sample `image.fb->getTexture()`. `IHyprRenderer::renderWorkspace` (Renderer.hpp:250) takes monitor and workspace as **independent** args and does **not** require `workspace->m_monitor == MON`, so a foreign-monitor workspace renders into a local cell with no renderer change. This already happens today in `redrawID` (OverviewRender.cpp:84-98).

**The "skip-guard" in the brief does not exist in the capture loop.** The capture path picks cells purely by workspace ID and has no `workspace->m_monitor != monitor` guard. The only ownership guards are in **teardown**: `normalizeMonitorWorkspaceRenderState` at `Overview.cpp:426` and `Overview.cpp:441`. **Therefore the single load-bearing source change is cell-ID enumeration, not rendering** — the contiguous-from-active-workspace logic must be branched to enumerate global IDs `1..N`.

**Home-display non-disruption is already structural and applies to foreign workspaces:** `activateWorkspaceForPreview` (Overview.cpp:573-587) only swaps the **local** monitor's `m_activeWorkspace` and never touches `workspace->m_monitor`; each cell render is wrapped by `applyWorkspaceWindowGoalState`/`restoreWorkspaceWindowGoalState` (OverviewRender.cpp:86,96) and `applyExclusiveWorkspacePreviewState`/`restoreWorkspacePreviewStates` (OverviewRender.cpp:85,97), which run **unconditionally** for every cell.

**Residual unknowns requiring runtime verification (not blockers):** (1) whether `renderWorkspace` correctly translates windows carrying home-monitor-relative coordinates into `monbox` at origin on a different-resolution/transform local monitor; (2) multi-monitor flicker from per-cell visibility toggling; (3) faithfulness of foreign thumbnails for differing aspect/rotation (the plan **squashes** to local aspect — see §6).

---

## 2. Architecture / where the change lives

Five concerns, each isolated:

1. **Cell-ID enumeration** — the only structurally single-monitor decision. Branch in `COverview::COverview` so the all_monitors path enumerates global IDs `1..N`. (Overview.cpp ctor, the `getWorkspaceMethodForMonitor` + three branches region.)
2. **Foreign-workspace liveness & lifetime safety** — do **not** cache strong `image.pWorkspace` for foreign cells; re-resolve by ID each `redrawID`; close/invalidate on monitor add/remove. (New, addresses critical + high reviewer findings.)
3. **Click routing** — `onCursorSelect` (Overview.cpp:914-939) reads the modifier/button and sets an intent member; `close()` (OverviewRender.cpp:179-233) branches path (a) vs (b) using the **correct** ConfigActions primitives (§4).
4. **Move/drag hard-disable** — view-only mode must gate every drag/move path so `ensureWorkspaceForTile` can never create a duplicate-global-ID workspace.
5. **Pure decisions** — enumeration, open-index, and click-intent resolution go into `HyprexpoLogic.{hpp,cpp}` (Hyprland-header-free, unit-tested).

Flag off ⇒ every one of these is bypassed and the existing code runs verbatim.

---

## 3. File-by-file changes

### 3a. `Overview.cpp` — ctor cell-ID enumeration (the core change)
- **Current:** ctor calls `getWorkspaceMethodForMonitor(pMonitor.lock())` then runs one of three branches (max_workspace / center / first) filling `images[i].workspaceID` contiguously from the cursor monitor's active workspace; the `first` branch calls `CWorkspace::create(currentID, pMonitor.lock(), …)` and mutates `pMonitor->m_activeWorkspace`.
- **New:** read the flag with the cached-pointer idiom (mirroring the existing `static auto* const*` reads near Overview.cpp:672-677). When `allMonitors` is true, **bypass `getWorkspaceMethodForMonitor` and all three branches entirely** and fill `images[i].workspaceID = (int64_t)i + 1`. Do **not** call `CWorkspace::create` and do **not** reassign `pMonitor->m_activeWorkspace`. Leave the entire else-path verbatim for flag-off.
- **Why:** "which workspace ID belongs in cell i" is the only single-monitor decision; everything downstream already works by ID.
- **`currentid` seed:** the existing capture loop already sets `currentid` via `if (PWORKSPACE == startedOn) currentid = i;` and `currentid` defaults to `0`. Per the reviewer correction this is **not load-bearing** — the in-range case is handled by the loop, the out-of-range case falls back to 0 automatically. We still route it through the tested helper `allMonitorsOpenIndex` (§7) for clarity, but it is not required for correctness.

### 3b. `OverviewRender.cpp:73-74` — do not cache strong foreign workspace (HIGH, reviewer-found)
- **Current:** `const auto PWORKSPACE = image.pWorkspace ? image.pWorkspace : g_pCompositor->getWorkspaceByID(image.workspaceID); image.pWorkspace = PWORKSPACE;` — caches a **strong** `PHLWORKSPACE` (Overview.hpp:70) and prefers it on every redraw.
- **New (all_monitors only):** for foreign cells, **always re-resolve by ID** and do not persist a strong ref: `const auto PWORKSPACE = allMonitors ? g_pCompositor->getWorkspaceByID(image.workspaceID) : (image.pWorkspace ? image.pWorkspace : g_pCompositor->getWorkspaceByID(image.workspaceID));` and skip the `image.pWorkspace = PWORKSPACE;` write when `allMonitors`. (Alternatively change `SWorkspaceImage::pWorkspace` to `PHLWORKSPACEREF` and lock per-frame; the local-only-reresolve is the smaller, lower-blast-radius change.)
- **Why:** in all_monitors mode every one of the N cells would otherwise pin a **foreign** workspace alive for the overview's whole lifetime, and a foreign workspace destroyed/recreated on its home monitor (same ID) would keep rendering the stale object. Re-lookup by ID each frame keeps cells live and lets emptied workspaces fall to the `WORKSPACE_INVALID`/null path.

### 3c. `OverviewRender.cpp:86` — never extend the `startedOn` goal-restore exemption to foreign workspaces (HIGH correctness)
- **Current:** `const auto windowState = PWORKSPACE == startedOn ? std::vector<SWindowPreviewState>{} : applyWorkspaceWindowGoalState(PWORKSPACE);` — skips goal save/restore when the cell is `startedOn`.
- **New:** tighten the exemption so it can only ever apply to a **local** workspace: `PWORKSPACE == startedOn && PWORKSPACE->m_monitor == MON ? {} : applyWorkspaceWindowGoalState(PWORKSPACE)`. In all_monitors mode `startedOn` is still local so behaviour is unchanged today; the guard prevents a future edit from skipping the relayout-undo for a foreign workspace (which would corrupt its home layout). Add a comment at `Overview.cpp:420` documenting that foreign-workspace correctness depends on per-cell restore, **not** teardown (`normalizeMonitorWorkspaceRenderState` skips `m_monitor != MON` at 426/441), so nobody optimizes the per-cell restore away.

### 3d. `OverviewInteraction.cpp` — hard-gate every move/drag path (MEDIUM, reviewer-found corruption vector)
- **Current:** `selectHoveredWorkspace()` (16-22) sets only `closeOnID`; the drag path (`beginWindowDrag`/`updateWindowDrag`/`finishWindowDrag`) feeds `ensureWorkspaceForTile` (197-214) which at line 211 calls `createNewWorkspace(image.workspaceID, MON->m_id, …)`.
- **New:** when `allMonitors` is true, **bypass the drag branch in `onCursorSelect`** (do not call `beginWindowDrag`/`finishWindowDrag`) and add `assert(!allMonitors)` (or an early `return`/log) at the top of `ensureWorkspaceForTile` so it can never be reached. 
- **Why:** in all_monitors mode `image.workspaceID` is a global ID that **already exists** on another monitor; `createNewWorkspace` with that ID bound to MON produces a duplicate-ID / dangling `CWorkspace`.

### 3e. `Overview.cpp:914-939` — `onCursorSelect` intent capture (see §4)
- **Current:** on PRESSED → `beginWindowDrag(); return;`; on RELEASE → `finishWindowDrag()` then `selectHoveredWorkspace(); close();`.
- **New (all_monitors):** skip the drag branch; on RELEASE compute `pull = (event.button == BTN_RIGHT) || shiftHeld()`, set new member `m_pullToCurrent = pull`, then `selectHoveredWorkspace(); close();`.

### 3f. `OverviewRender.cpp:179-233` — `close()` path (a)/(b) branch (see §4)

---

## 4. Click routing — both behaviours

**Insertion point:** `onCursorSelect` (Overview.cpp:914-939) reads the intent; `close(bool switchToSelection)` (OverviewRender.cpp:179-233) executes it. Thread the intent via a new bool member `m_pullToCurrent` set before `close()`.

**Reading the modifier (justified):** `IPointer::SButtonEvent` carries `button` + `state` but **no modifier mask**. Two signals:
- **Right button** — `event.button == BTN_RIGHT (0x111)`, self-contained. *Confirm `linux/input-event-codes.h` is transitively included in the TU before relying on the constant; if not, include it or hard-code with a named constant.*
- **Shift** — query live keyboard state, mirroring the cancel-key code (`g_pSeatManager->m_keyboard.lock()` → `xkb_state` modifier query).

Policy (tested via `resolveClickIntent`, §7): **right button OR Shift+left ⇒ PullToCurrent (b); plain left ⇒ FocusOwner (a).**

**CORRECTED PRIMITIVES (the draft assigned the wrong action to both paths).** Verified against `config/shared/actions/ConfigActions.hpp`:
- `changeWorkspace(PHLWORKSPACE|string)` — line 68/69 (follows the workspace to **its** monitor; does **not** pull)
- `moveToMonitor(PHLWORKSPACE ws, PHLMONITOR mon)` — **line 71**
- `changeWorkspaceOnCurrentMonitor(PHLWORKSPACE ws)` — **line 72**
- `focusMonitor(PHLMONITOR mon)` — **line 75**
- `g_pCompositor->moveWorkspaceToMonitor(PHLWORKSPACE, PHLMONITOR, bool)` — **Compositor.hpp:133**

`g_pCompositor->setActiveMonitor(...)` **does not exist** — the draft would not compile. Use `focusMonitor`.

> The existing `close()` block (OverviewRender.cpp:210-230) calls `Config::Actions::changeWorkspace(...)`. The existence of distinct `moveToMonitor` / `changeWorkspaceOnCurrentMonitor` / `focusMonitor` actions means plain `changeWorkspace` does **not** pull a foreign workspace to the current monitor — for a workspace living on monitor B it focuses/follows B. So today's stock close already behaves path-(a)-like for foreign workspaces. **Verify `changeWorkspace`'s cross-monitor body in Hyprland's keybind .cpp before finalizing**, since its source body isn't in the headers.

**`close()` branch (replace the single block at OverviewRender.cpp:210-230):**

```text
WS   = g_pCompositor->getWorkspaceByID(TILE.workspaceID)   // re-resolve, do not trust a cache
HOME = WS ? WS->m_monitor.lock() : nullptr

// TOCTOU re-validation (HIGH, reviewer-found): an unplug between open and click
// can reassign WS->m_monitor to a survivor. Confirm HOME is still the owner AND live.
homeValid = HOME && HOME == WS->m_monitor.lock() && monitorIsInCompositorList(HOME)

if (allMonitors && m_pullToCurrent) {                       // path (b) PULL onto current
    g_pCompositor->moveWorkspaceToMonitor(WS, MON, /*noFocus=*/false);   // or Actions::moveToMonitor(WS, MON)
    Config::Actions::changeWorkspaceOnCurrentMonitor(WS);
    // animate MON's IN/OUT as today (lines 226-227 target MON — correct for path b)
}
else if (allMonitors && homeValid && HOME != MON) {        // path (a) FOCUS OWNER + switch there
    Config::Actions::focusMonitor(HOME);
    HOME->setSpecialWorkspace(0);
    Config::Actions::changeWorkspace(WS->getConfigName());  // switches on the now-focused HOME
    // animate HOME's old/new workspaces, NOT MON's (reviewer correction):
    g_pDesktopAnimationManager->startAnimation(WS, ANIMATION_TYPE_IN,  true,  true);
    // OUT against HOME's previously-active ws captured before the switch
}
else {
    // existing local-switch block, verbatim (also the fallback when HOME==MON / null / invalid)
}
size->setCallbackOnEnd(removeOverview);   // unchanged — teardown ALWAYS fires
```

**Animation correctness (reviewer corrections folded in):**
- The draft's worry that path-(a) teardown "may not fire" is **wrong**: `size->setCallbackOnEnd(removeOverview)` (OverviewRender.cpp:232) fires unconditionally; and since path (a) leaves `MON->m_activeWorkspace == startedOn`, `shouldRenderOverviewForMonitor` keeps the overview rendering to completion. The **real** bug is that the IN/OUT `startAnimation` calls at 226-227 animate **MON's** workspaces; for path (a) they must animate **HOME's** old/new workspaces.
- For path (a) the overview-monitor workspace-change hooks (`onWorkspaceChange`, `onWindowMoveToWorkspace`'s `movedOnOverviewMonitor` guard at OverviewInteraction.cpp:521, and `externalWorkspaceMoveDuringClose`) will **not** trigger — that is fine because teardown is driven by the end-callback, but it means `shouldRenderOverviewForMonitor` will not early-abort. Confirm the overview's own pass tears down cleanly without the abort path.

---

## 5. Config flag wiring (this fork's exact pattern)

Three coordinated edits (the compat map is not auto-derived):

1. **`HyprexpoConfig.hpp`** (alongside `SKIP_EMPTY_DEFAULT`): add `inline constexpr int ALL_MONITORS_DEFAULT = 0;`. **Do not** add `ALL_MONITORS_PULL_BUTTON_DEFAULT` unless it is actually registered (the draft introduced a dangling, unreadable constant — reviewer gap). If a configurable pull button is wanted, register it fully in steps 2+3; otherwise omit it and hard-code the right-button/Shift policy.
2. **`PluginConfig.cpp`** `registerHyprexpoConfigValues()` (near the other grid flags, ~line 28): `addConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyprexpo:all_monitors", "consolidated all-workspaces overview", HyprexpoConfig::ALL_MONITORS_DEFAULT));` (bool flags are `CIntValue` 0/1 in this fork — no bool type exists).
3. **`Overview.cpp`** `intDefault()` DEFAULTS map (119-169): add `{"plugin:hyprexpo:all_monitors", HyprexpoConfig::ALL_MONITORS_DEFAULT},`.

Runtime read uses the cached `static auto* const*` indirection (Overview.cpp:672-677). No `createCancelKeyConfig`-style wrapper and no `PluginConfig.hpp` change needed. **Default off ⇒ stock behaviour byte-identical**, guaranteed by leaving every flag-off path verbatim.

---

## 6. Risks & mitigations

| Risk | Severity | Mitigation |
|---|---|---|
| **Foreign monitor unplugged while grid is open.** On unplug Hyprland reassigns `workspace->m_monitor` to a survivor; a cached cell would otherwise render a migrated workspace and a path-(a) click would `focusMonitor` the **wrong** survivor. | **critical** | **Implemented** (§6 / `Overview.cpp` ctor): in all_monitors mode the overview installs `monitorAddedHook`/`monitorRemovedHook` (`Event::bus()->m_events.monitor.added/removed`) for its lifetime. A foreign-monitor add/remove closes the overview synchronously (`close(false)`); an unplug of the overview's **own** monitor defers `close(false)` via `g_pEventLoopManager->doLater`, scoped to this instance (guards against resetting a newer overview), so we never reset during the monitor signal emit. Reinforced by §3b (re-resolve every cell by ID each frame — no cached home monitor) and §4 (TOCTOU re-validate `HOME` against the live monitor list before `focusMonitor`). |
| **Strong `image.pWorkspace` pins/staleness.** `SWorkspaceImage::pWorkspace` is a strong `PHLWORKSPACE` (Overview.hpp:70) preferred over live lookup (OverviewRender.cpp:73). In all_monitors mode it pins every foreign workspace alive and renders stale objects after foreign-side destroy/recreate. | **high** | §3b: in all_monitors mode re-resolve by ID every `redrawID`, never persist the strong ref (or switch the field to `PHLWORKSPACEREF`). Null any cache on monitor/workspace change. |
| **Path-(a) TOCTOU on vanished/reassigned `HOME`.** `WS->m_monitor.lock()` may point at a reassigned survivor between open and click. | **high** | §4: re-validate `HOME == WS->m_monitor.lock()` AND `HOME` is in the live monitor list before `focusMonitor`; else fall through to local-switch. |
| **Move/drag corruption via `ensureWorkspaceForTile`.** `createNewWorkspace(image.workspaceID, MON->m_id, …)` (OverviewInteraction.cpp:211) with a global ID that already exists ⇒ duplicate-ID workspace bound to MON. | **medium** | §3d: bypass the drag branch when `allMonitors`; `assert`/guard that `ensureWorkspaceForTile` is unreachable with the flag on. |
| **Wrong click primitive.** Draft used `changeWorkspace` for the pull path and non-existent `setActiveMonitor` for focus. | **high (compile + behaviour)** | §4: pull = `moveToMonitor`/`moveWorkspaceToMonitor` + `changeWorkspaceOnCurrentMonitor`; focus = `focusMonitor` + `changeWorkspace`. Verify `changeWorkspace`'s cross-monitor body in Hyprland keybinds .cpp. |
| **Multi-monitor flicker.** `applyExclusiveWorkspacePreviewState` (Overview.cpp:376-410) forces every non-target workspace `m_visible=false`/`alpha=0` on **every** redraw tick (not just open: fires from `setCallbackOnEnd`/`onPreRender`→`redrawID`), while monitors B/C present live content. | **medium** | Scope the visibility toggle to skip workspaces whose `m_monitor` is a **different live** monitor, OR confirm capture is synchronous within one render pass with no intervening present to B/C — verify on the dual-monitor rig. |
| **Teardown leaves a foreign workspace invisible.** `normalizeMonitorWorkspaceRenderState` only normalizes `m_monitor==MON` (Overview.cpp:426/441). If a redraw returns early between apply and restore, a foreign workspace stays `alpha=0` on its home monitor with nothing in `~COverview` to fix it. | **medium** | Ensure no early-return between `applyExclusiveWorkspacePreviewState` and `restoreWorkspacePreviewStates` in the all_monitors path; consider a scope-guard (RAII) so restore runs on every exit path. |
| **Path-(a) animates wrong monitor.** IN/OUT at OverviewRender.cpp:226-227 target MON. | **low** | §4: issue IN/OUT against HOME's old/new workspaces for path (a); leave `removeOverview` wiring untouched. |
| **Foreign aspect/resolution/rotation squashed.** Capture relays the foreign workspace to MON's geometry and `monbox = MON->m_pixelSize`; transform normalization (OverviewRender.cpp:50-62,116-117) uses **only** MON's transform. A portrait/rotated/differing-aspect foreign monitor shows MON-shaped, possibly mis-rotated content — not a faithful thumbnail. | **medium** | **Decision required (see Open unknowns).** Default plan: accept and **document** the squash as a known limitation. Faithful capture (render each foreign workspace against its **home** monitor geometry/transform into a home-sized FB, then rescale into the cell box) is real additional work — estimate +1 commit if chosen. |
| **Pulling a monitor-bound workspace.** A workspace with a `monitor:` rule may be snapped back by `ensureWorkspacesOnAssignedMonitors` (Compositor.hpp:168). | **low** | Test pull against bound/persistent workspaces; document that bound workspaces may not stay pulled. |
| **`BTN_RIGHT` constant availability in-TU.** | **low** | Confirm `linux/input-event-codes.h` is transitively included; include explicitly or use a named constant otherwise. |

---

## 7. Tests

**`HyprexpoLogic.{hpp,cpp}`** (Hyprland-header-free, the testable layer) — add pure helpers:
- `std::vector<int64_t> allMonitorsCellWorkspaceIDs(int tileCount);` → `{1..tileCount}` (pins the enumeration contract; central home for future skip-empty/N-cap logic).
- `int allMonitorsOpenIndex(int64_t startedOnID, int tileCount);` → index of `startedOnID` in 1..tileCount else 0 (clarity helper for the `currentid` seed; **not** correctness-load-bearing per §3a).
- `enum class EClickIntent { FocusOwner, PullToCurrent };` + `EClickIntent resolveClickIntent(bool rightButton, bool shiftHeld);` → "right OR shift ⇒ PullToCurrent".

**`tests/HyprexpoLogicTests.cpp`** (hand-rolled `expect()` harness, `main` at line 30) — add:
```cpp
expect(allMonitorsCellWorkspaceIDs(9) == std::vector<int64_t>{1,2,3,4,5,6,7,8,9}, "all_monitors enumerates 1..9");
expect(allMonitorsOpenIndex(5, 9) == 4, "open index maps startedOn ws id to cell");
expect(allMonitorsOpenIndex(99, 9) == 0, "out-of-range startedOn falls back to 0");
expect(resolveClickIntent(true,false)  == EClickIntent::PullToCurrent, "right button pulls");
expect(resolveClickIntent(false,true)  == EClickIntent::PullToCurrent, "shift pulls");
expect(resolveClickIntent(false,false) == EClickIntent::FocusOwner,    "plain click focuses owner");
```
Linked by Makefile / CMakeLists.txt / meson.build (all reference `HyprexpoLogic.cpp`).

**Do-not-break:** `tests/OverviewSourceTests.cpp:57-67` greps for the exact line `const auto MON = g_pOverview->pMonitor.lock();` and the lock→reset→damage→schedule ordering, and for the `onCursorMove`/`removeOverview` crash guard (Overview.cpp:903-905 / OverviewRender.cpp:24-26). Preserve those regions verbatim.

**Manual matrix (dual-monitor rig — single-monitor will not exercise the feature):**
1. Flag off → grid identical to stock (regression).
2. Flag on, all 1..9 render live; foreign workspaces keep displaying correctly on their home monitor during and after overview (no layout corruption).
3. Plain click on a foreign-owned cell → focus jumps to owner monitor and switches there; animation smooth.
4. Right-click / Shift+click on a foreign cell → workspace pulled onto current monitor; animation smooth.
5. Differing-resolution and **portrait/rotated** foreign monitor → confirm squash behaviour matches the documented limitation (or faithful render if §6 faithful option chosen).
6. **Unplug the foreign monitor while the grid is open** → overview closes (or re-resolves) cleanly, no crash; subsequent click does not focus a wrong survivor.
7. Foreign workspace emptied/auto-destroyed while grid open → cell falls to empty/invalid, no stale render, no crash.
8. Pull a monitor-bound workspace → observe whether it snaps back; matches documented behaviour.
9. Watch monitors B/C for flicker during open and during sustained-open redraw ticks.

---

## 8. Build / branch / reload steps

```bash
# 1. Branch off master (never commit to master directly — user git rules)
git -C /home/aditya/learn/hyprexpo-sandwichfarm checkout -b feat/all-monitors-overview

# 2. Fast unit-test loop (no Hyprland needed) — builds HyprexpoLogicTests + OverviewSourceTests, -Werror
make -C /home/aditya/learn/hyprexpo-sandwichfarm test

# 3. Build the plugin .so
make -C /home/aditya/learn/hyprexpo-sandwichfarm           # -> hyprexpo.so

# 4. Stage into hyprpm cache + live-reload (NOT hyprpm, NOT sudo)
make -C /home/aditya/learn/hyprexpo-sandwichfarm dev-reload # unload + hyprctl plugin load of the fresh .so
```

**Caveats:**
- `install-to-cache.sh` hardcodes `SRC=/home/aditya/hyprexpo-sandwichfarm/hyprexpo.so` (line 7) but the repo is at **`/home/aditya/learn/hyprexpo-sandwichfarm`** — that script is stale for this checkout. Prefer `make dev-reload` (which `readlink -f`s the dev target), or fix the path first. It pkexec-stages; **the user runs it, not the agent.**
- **Plugin reload is the user's job.** The agent never runs `sudo hyprpm` or `hyprpm reload`. After an ABI/header mismatch the user may need a full `hyprpm reload` cycle — a manual, non-sudo step they own.
- All runtime risks (foreign render correctness §6, flicker, unplug) are **dual-monitor-only**.

---

## 9. Effort estimate & suggested commit breakdown

Roughly **2–3 focused days** including dual-monitor verification.

1. **Config flag wiring** (§5) + logic helpers & tests (§7) — small, fully unit-testable. *(~2–3h)*
2. **Cell-ID enumeration branch** in ctor (§3a) — small, behind flag. *(~2h)*
3. **Liveness/lifetime safety** — re-resolve-by-ID for foreign cells (§3b), goal-restore guard (§3c). *(~3h)*
4. **Monitor add/remove listener** that closes/invalidates the open overview (§6 critical). *(~3–4h, new event wiring)*
5. **Move/drag hard-disable** + `ensureWorkspaceForTile` assert (§3d). *(~1–2h)*
6. **Click routing** path (a)/(b) with corrected primitives + modifier capture + TOCTOU re-validation + per-path animation (§4). *(~half day, the most error-prone)*
7. *(optional)* **Faithful foreign-aspect capture** if the squash limitation is rejected (§6). *(+1 day)*
8. **Dual-monitor manual verification & docs** (§7 matrix; document squash + bound-workspace behaviour). *(~half day)*

Keep commits 1–6 each behind the flag so master behaviour stays byte-identical until the feature is complete.

---

## Open unknowns (honest residuals)

1. **`changeWorkspace` cross-monitor semantics** — verified the *action set* (`changeWorkspace` vs `moveToMonitor` vs `changeWorkspaceOnCurrentMonitor` vs `focusMonitor`) from `ConfigActions.hpp:68-75`, but the actual bodies live in Hyprland's keybind .cpp (not in headers). Confirm `changeWorkspace`'s foreign-workspace behaviour before relying on path (a)/(b) wiring.
2. **`renderWorkspace` coordinate fidelity** for windows with home-monitor-relative positions drawn into `monbox` at origin on a different-resolution/transform local monitor — source-unverifiable; visual test required.
3. **Faithful thumbnails for heterogeneous monitors** — the plan squashes foreign content to local aspect and uses MON's transform only. This **does not** fully satisfy the goal's "handle scale/resolution differences" for portrait/rotated foreign monitors. **Product decision needed:** document the squash as a known limitation (default) vs. implement per-home-monitor capture (commit 7).
4. **Per-tick flicker** on monitors B/C from `applyExclusiveWorkspacePreviewState` — must be observed on the real dual-monitor rig; mitigation (skip different-live-monitor workspaces) is straightforward if it manifests.