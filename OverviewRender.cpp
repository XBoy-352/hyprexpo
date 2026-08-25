#include "HyprlandConfigCompat.hpp"
#define HyprlandAPI CompatHyprlandAPI
#include "OverviewInternal.hpp"
#include "HyprexpoLogic.hpp"
#include "OverviewPassElement.hpp"
#define private   public
#define protected public
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/animation/WorkspaceAnimationController.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/target/WindowGroupTarget.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/state/WorkspacePlacementController.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#undef private
#undef protected
#include <hyprland/src/debug/log/Logger.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

void COverview::queueForeignRecalc(PHLMONITORREF mon) {
    if (!mon)
        return;

    for (const auto& m : pendingForeignRecalcs) {
        if (m.lock() == mon.lock())
            return;
    }

    pendingForeignRecalcs.push_back(mon);
}

void COverview::flushForeignRecalcs() {
    if (pendingForeignRecalcs.empty())
        return;

    const auto mons = pendingForeignRecalcs;
    pendingForeignRecalcs.clear();

    if (!g_layoutManager)
        return;

    for (const auto& m : mons) {
        if (const auto MON = m.lock())
            g_layoutManager->recalculateMonitor(MON);
    }
}

void COverview::redrawID(int id, bool forcelowres) {
    const auto MON = pMonitor.lock();
    if (!MON)
        return;

    if (MON->m_activeWorkspace != startedOn && !closing) {
        // likely user changed.
        onWorkspaceChange();
    }

    blockOverviewRendering = true;

    Render::GL::g_pHyprOpenGL->makeEGLCurrent();
    settleWorkspaceMoveAnimations();

    if (images.empty()) {
        blockOverviewRendering = false;
        return;
    }

    id = std::clamp(id, 0, (int)images.size() - 1);

    CBox monbox{0, 0, MON->m_pixelSize.x, MON->m_pixelSize.y};

    if (!forcelowres && (size->value() != MON->m_size || closing))
        monbox = {{0, 0}, MON->m_pixelSize};

    if (!ENABLE_LOWRES)
        monbox = {{0, 0}, MON->m_pixelSize};

    const auto savedTransform       = MON->m_transform;
    const auto savedTransformedSize = MON->m_transformedSize;
    const auto savedPixelSize       = MON->m_pixelSize;

    // Fix for rotated monitors: swap dimensions to match logical orientation
    if (isTransformRotated(savedTransform)) {
        monbox = {{0, 0}, {monbox.h, monbox.w}};

        // Override monitor state to disable rotation
        MON->m_transform       = WL_OUTPUT_TRANSFORM_NORMAL;
        MON->m_pixelSize       = {monbox.w, monbox.h};
        MON->m_transformedSize = {monbox.w, monbox.h};
    }

    auto& image = images[id];

    ensureFramebuffer(image, monbox, framebufferFormatWithAlpha(MON->m_output->state->state().drmFormat));

    CRegion fakeDamage{0, 0, INT16_MAX, INT16_MAX};
    g_pHyprRenderer->beginRender(MON, fakeDamage, Render::RENDER_MODE_FULL_FAKE, nullptr, image.fb);

    clearWithColor(CHyprColor{0, 0, 0, 1.0});

    // In all_monitors mode always re-resolve foreign cells by ID and never persist the strong ref:
    // a foreign workspace destroyed/recreated on its home monitor (same ID) must not keep rendering a
    // stale object, and we must not pin foreign workspaces alive for the overview's lifetime. Emptied
    // workspaces fall to the null/WORKSPACE_INVALID path below.
    PHLWORKSPACE PWORKSPACE;
    if (!m_allMonitors && image.pWorkspace) {
        PWORKSPACE = image.pWorkspace;
    } else {
        for (const auto& w : State::workspaceState()->workspacesCopy()) {
            if (w->m_id == image.workspaceID) {
                PWORKSPACE = w;
                break;
            }
        }
    }
    if (!m_allMonitors)
        image.pWorkspace = PWORKSPACE;

    const auto   restoreWorkspace = MON->m_activeWorkspace;
    PHLWORKSPACE openSpecial      = MON->m_activeSpecialWorkspace;
    if (openSpecial)
        MON->m_activeSpecialWorkspace.reset();

    startedOn->m_visible = false;

    if (PWORKSPACE) {
        // all_monitors: a FOREIGN workspace's windows are laid out in their home monitor's coordinate
        // space, so the layout/renderer leave them outside this monitor's frame and the cell renders
        // empty. Temporarily reparent the workspace onto MON so the preview activation recalcs its
        // space against MON's work area and lays it out in-frame for the capture. Everything
        // (workspace owner, each window's animated position/size, home-monitor layout) is fully
        // restored after the capture below.
        struct SForeignSave {
            PHLWINDOW w;
            Vector2D  posV, posG, sizeV, sizeG;
        };
        std::vector<SForeignSave> foreignSaved;
        PHLMONITORREF             foreignHome;
        const bool                reparentForeign = m_allMonitors && PWORKSPACE->m_monitor && PWORKSPACE->m_monitor.lock() != MON;
        if (reparentForeign) {
            // Only the WORKSPACE is reparented, never its windows. shouldRenderWindow gates tiled
            // windows on workspace->m_monitor == pMonitor (plus m_forceRendering, already set by the
            // preview state) and the layout recalc positions targets via the workspace's m_monitor,
            // so the per-window m_monitor writes (and their save/restore) are unnecessary. Window
            // geometry IS saved/restored pre-recalc: recalculate() only sets new layout GOALS, so
            // without this the windows would animate off toward the capture's monitor-frame geometry
            // afterwards.
            foreignHome = PWORKSPACE->m_monitor;
            if (PWORKSPACE->m_space) {
                // Per-workspace space target list instead of a full-compositor window scan.
                for (const auto& t : PWORKSPACE->m_space->targets()) {
                    if (!t)
                        continue;
                    if (t->type() == Layout::TARGET_TYPE_GROUP) {
                        auto* gt = static_cast<Layout::CWindowGroupTarget*>(t.get());
                        if (!gt || !gt->getGroup())
                            continue;
                        for (const auto& wref : gt->getGroup()->windows()) {
                            const auto w = wref.lock();
                            if (!windowVisibleOnWorkspace(w, PWORKSPACE))
                                continue;
                            foreignSaved.push_back({w, w->m_realPosition->value(), w->m_realPosition->goal(), w->m_realSize->value(), w->m_realSize->goal()});
                        }
                        continue;
                    }
                    const auto w = t->window();
                    if (!windowVisibleOnWorkspace(w, PWORKSPACE))
                        continue;
                    foreignSaved.push_back({w, w->m_realPosition->value(), w->m_realPosition->goal(), w->m_realSize->value(), w->m_realSize->goal()});
                }
            }
            PWORKSPACE->m_monitor = MON;
        }

        const auto previousWS    = activateWorkspaceForPreview(MON, PWORKSPACE);
        const auto previewStates = applyExclusiveWorkspacePreviewState(PWORKSPACE);
        // Local-only goal-restore exemption (see normalizeMonitorWorkspaceRenderState note): never skip
        // the relayout-undo for a foreign workspace, or its home layout corrupts. The added m_monitor
        // clause is gated behind m_allMonitors so the flag-OFF path stays byte-identical to stock
        // (PWORKSPACE == startedOn), which is the only behaviour that can hold without the feature.
        const auto windowState   = (PWORKSPACE == startedOn && (!m_allMonitors || PWORKSPACE->m_monitor == MON)) ? std::vector<SWindowPreviewState>{} : applyWorkspaceWindowGoalState(PWORKSPACE);

        if (PWORKSPACE == startedOn)
            MON->m_activeSpecialWorkspace = openSpecial;

        {
            CPinnedWindowPreviewGuard pinnedWindowPreviewGuard{showPinnedWindowsInPreview()};
            g_pHyprRenderer->renderWorkspace(MON, PWORKSPACE, Time::steadyNow(), monbox);
        }

        restoreWorkspaceWindowGoalState(windowState);
        restoreWorkspacePreviewStates(previewStates);
        restoreActiveWorkspaceAfterPreview(MON, previousWS);

        if (reparentForeign) {
            // Restore foreign workspace ownership and each window's animated pos/size, then re-lay
            // the home monitor so its real layout is untouched by the capture (runs last → wins).
            // Window m_monitor never changed (workspace-only reparent), so there is nothing to
            // restore there.
            PWORKSPACE->m_monitor = foreignHome;
            for (const auto& s : foreignSaved) {
                if (!s.w)
                    continue;
                s.w->m_realPosition->setValueAndWarp(s.posV);
                *s.w->m_realPosition = s.posG;
                s.w->m_realSize->setValueAndWarp(s.sizeV);
                *s.w->m_realSize = s.sizeG;
            }
            // Deferred: recalculating the home monitor's layout here is a full relayout of every
            // window on it. Several cells in the same redrawAll/flushQueuedRedraws batch typically
            // share the same foreign home monitor (e.g. 5 workspaces on one external display), so
            // queue it and recalc each distinct monitor once via flushForeignRecalcs() at the end
            // of the batch instead of once per cell.
            if (foreignHome)
                queueForeignRecalc(foreignHome);
        }

        if (PWORKSPACE == startedOn)
            MON->m_activeSpecialWorkspace.reset();
    } else {
        CPinnedWindowPreviewGuard pinnedWindowPreviewGuard{showPinnedWindowsInPreview()};
        g_pHyprRenderer->renderWorkspace(MON, PWORKSPACE, Time::steadyNow(), monbox);
    }

    g_pHyprRenderer->m_renderData.blockScreenShader = true;
    g_pHyprRenderer->endRender();

    // Restore the original monitor state after capture
    MON->m_transform       = savedTransform;
    MON->m_pixelSize       = savedPixelSize;
    MON->m_transformedSize = savedTransformedSize;

    // Capture normalizes rotated monitor geometry; Hyprland's output path adds one more half-turn.
    if (const auto texture = image.fb->getTexture(); texture)
        texture->m_transform = isTransformRotated(savedTransform) ? HYPRUTILS_TRANSFORM_180 : HYPRUTILS_TRANSFORM_NORMAL;

    MON->m_activeSpecialWorkspace = openSpecial;

    const auto activeWorkspace = restoreWorkspace ? restoreWorkspace : startedOn;
    MON->m_activeWorkspace = activeWorkspace;
    if (activeWorkspace) {
        activeWorkspace->m_visible = true;
        if (activeWorkspace == startedOn)
            Animation::Workspace::startAnimation(activeWorkspace, Animation::Workspace::ANIMATION_TYPE_IN, true, true);
    }

    blockOverviewRendering = false;
}

void COverview::redrawAll(bool forcelowres) {
    const auto MON = pMonitor.lock();
    if (!MON)
        return;

    for (size_t i = 0; i < images.size(); ++i) {
        redrawID(i, forcelowres);
    }

    flushForeignRecalcs();
}

void COverview::damage() {
    const auto MON = pMonitor.lock();
    if (!MON)
        return;

    blockDamageReporting = true;
    g_pHyprRenderer->damageMonitor(MON);
    blockDamageReporting = false;
}

void COverview::onDamageReported() {
    const auto MON = pMonitor.lock();
    if (!MON)
        return;

    damageDirty = true;

    Vector2D SIZE = size->value();

    const auto GAPSIZE = (closing ? (1.0 - size->getPercent()) : size->getPercent()) * GAP_WIDTH;
    const auto OUTER = currentOuterInset();
    CBox texbox = tileBoxForIndex(openedID, SIZE, GAPSIZE, OUTER, true).translate(MON->m_position);

    damage();

    blockDamageReporting = true;
    g_pHyprRenderer->damageBox(texbox);
    blockDamageReporting = false;
    MON->scheduleFrame();
}

void COverview::close(bool switchToSelection) {
    if (closing)
        return;

    // The teardown animation is now committed; lock out further swipe input so a
    // re-grabbed gesture can't rewind it (issue #57 follow-up: close replay).
    m_closeCommitted = true;

    const auto MON = pMonitor.lock();
    if (!MON) {
        closing = true;
        g_pOverview.reset();
        return;
    }

    resetSubmapIfNeeded();

    if (images.empty()) {
        g_pOverview.reset();
        return;
    }

    const int   ID = closeOnID == -1 ? openedID : closeOnID;

    const int   SAFEID = std::clamp(ID, 0, (int)images.size() - 1);
    const auto& TILE   = images[SAFEID];

    const auto targetSize = zoomSizeForCurrentGrid(MON->m_size);
    *size = targetSize;
    *pos  = -(tilePosForID(SAFEID, targetSize, 0.0) * MON->m_scale);

    closing = true;

    // all_monitors: no more progressive streaming — redrawAll() below re-captures every cell in one
    // batch, so the deferred queue is done.
    if (m_allMonitors)
        pendingForeignCaptures.clear();

    redrawAll();

    if (switchToSelection && TILE.workspaceID != MON->activeWorkspaceID()) {
        bool handled = false;

        if (m_allMonitors) {
            // Re-resolve by ID; never trust a cached home monitor across frames.
            PHLWORKSPACE WS;
            for (const auto& w : State::workspaceState()->workspacesCopy()) {
                if (w->m_id == TILE.workspaceID) {
                    WS = w;
                    break;
                }
            }
            const auto HOME = WS ? WS->m_monitor.lock() : nullptr;

            // TOCTOU re-validation: an unplug between open and click can reassign WS->m_monitor to a
            // survivor. Confirm HOME is still the owner AND present in the live monitor list before we
            // focus it; otherwise fall through to the local-switch fallback.
            bool homeValid = false;
            if (HOME) {
                const bool stillOwner = HOME == WS->m_monitor.lock();
                const auto& MONITORS  = State::monitorState()->monitors();
                const bool  live      = std::find(MONITORS.begin(), MONITORS.end(), HOME) != MONITORS.end();
                homeValid             = stillOwner && live;
            }

            if (m_pullToCurrent && WS) {
                // path (b): PULL the workspace onto the current monitor and switch to it here.
                MON->setSpecialWorkspace(0);
                const auto OLDWS = MON->m_activeWorkspace;
                State::workspacePlacementController()->moveWorkspaceToMonitor(WS, MON, false);
                const auto CHANGE = Config::Actions::changeWorkspaceOnCurrentMonitor(WS);
                if (!CHANGE)
                    Log::logger->log(Log::ERR, "[hyprexpo] failed to pull workspace: {}", CHANGE.error().message);

                // MON's IN/OUT (animation targets MON — correct for path b).
                Animation::Workspace::startAnimation(MON->m_activeWorkspace, Animation::Workspace::ANIMATION_TYPE_IN, true, true);
                Animation::Workspace::startAnimation(OLDWS, Animation::Workspace::ANIMATION_TYPE_OUT, false, true);

                startedOn = MON->m_activeWorkspace;
                handled   = true;
            } else if (WS && homeValid && HOME != MON) {
                // path (a): FOCUS the owner monitor and switch there. MON stays on startedOn so the
                // overview tears down cleanly via the unconditional end-callback below
                // (shouldRenderOverviewForMonitor keeps rendering while MON->m_activeWorkspace == startedOn).
                const auto HOMEOLDWS = HOME->m_activeWorkspace;
                const auto FOCUS     = Config::Actions::focusMonitor(HOME);
                if (!FOCUS) {
                    // Leave handled=false so the local-switch fallback below runs instead of
                    // switching/animating a monitor the user is not looking at.
                    Log::logger->log(Log::ERR, "[hyprexpo] failed to focus owner monitor: {}", FOCUS.error().message);
                } else {
                    // Clear the owner's special workspace unconditionally: when the clicked workspace
                    // is already HOME's active one, focusMonitor alone lands us there and a lingering
                    // special workspace would otherwise stay open over it.
                    HOME->setSpecialWorkspace(0);

                    // If the clicked workspace is already HOME's active one, focusMonitor alone lands us
                    // there — re-switching (and animating an already-active workspace) is redundant/wrong.
                    if (HOMEOLDWS != WS) {
                        const auto CHANGE = Config::Actions::changeWorkspace(WS->getConfigName());
                        if (!CHANGE)
                            Log::logger->log(Log::ERR, "[hyprexpo] failed to change workspace: {}", CHANGE.error().message);

                        // Animate HOME's old/new workspaces, NOT MON's.
                        Animation::Workspace::startAnimation(WS, Animation::Workspace::ANIMATION_TYPE_IN, true, true);
                        if (HOMEOLDWS)
                            Animation::Workspace::startAnimation(HOMEOLDWS, Animation::Workspace::ANIMATION_TYPE_OUT, false, true);
                    }

                    handled = true;
                }
            }
        }

        if (!handled) {
            MON->setSpecialWorkspace(0);

            // If this tile's workspace was WORKSPACE_INVALID, move to the next
            // empty workspace. This should only happen if skip_empty is on, in
            // which case some tiles will be left with this ID intentionally.
            const int  NEWID = TILE.workspaceID == WORKSPACE_INVALID ? getWorkspaceIDNameFromString("emptynm").id : TILE.workspaceID;

            PHLWORKSPACE NEWIDWS;
            for (const auto& w : State::workspaceState()->workspacesCopy()) {
                if (w->m_id == NEWID) {
                    NEWIDWS = w;
                    break;
                }
            }

            const auto OLDWS = MON->m_activeWorkspace;

            const auto CHANGE = !NEWIDWS ? Config::Actions::changeWorkspace(std::to_string(NEWID)) : Config::Actions::changeWorkspace(NEWIDWS->getConfigName());
            if (!CHANGE)
                Log::logger->log(Log::ERR, "[hyprexpo] failed to change workspace: {}", CHANGE.error().message);

            Animation::Workspace::startAnimation(MON->m_activeWorkspace, Animation::Workspace::ANIMATION_TYPE_IN, true, true);
            Animation::Workspace::startAnimation(OLDWS, Animation::Workspace::ANIMATION_TYPE_OUT, false, true);

            startedOn = MON->m_activeWorkspace;
        }
    }

    size->setCallbackOnEnd(removeOverview);
}

void COverview::onPreRender() {
    // all_monitors: stream deferred foreign-cell captures across frames (bounded per tick) so the
    // open doesn't stall on N synchronous reparent → render → restore passes. damage() keeps frames
    // ticking (addDamage hooks → scheduleFrame) until the queue drains, and runs before the grid
    // pass of this frame so each freshly captured cell is drawn the same frame.
    if (m_allMonitors && !pendingForeignCaptures.empty()) {
        for (size_t n = 0; n < FOREIGN_CAPTURES_PER_FRAME && !pendingForeignCaptures.empty(); ++n) {
            const int id = pendingForeignCaptures.front();
            pendingForeignCaptures.erase(pendingForeignCaptures.begin());
            redrawID(id);
        }
        flushForeignRecalcs();
        damage();
    }

    if (damageDirty) {
        damageDirty = false;
        redrawID(closing ? (closeOnID == -1 ? openedID : closeOnID) : openedID);
        flushForeignRecalcs();
    }
}

void COverview::onWorkspaceChange() {
    const auto MON = pMonitor.lock();
    if (!MON)
        return;

    if (valid(startedOn))
        Animation::Workspace::startAnimation(startedOn, Animation::Workspace::ANIMATION_TYPE_OUT, false, true);
    else
        startedOn = MON->m_activeWorkspace;

    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i].workspaceID != MON->activeWorkspaceID())
            continue;

        openedID = i;
        break;
    }

    closeOnID = openedID;
    close();
}

void COverview::render() {
    g_pHyprRenderer->m_renderPass.add(makeUnique<COverviewPassElement>());
}

bool COverview::shouldRenderOverviewForMonitor(const PHLMONITOR& monitor) const {
    if (pMonitor != monitor)
        return false;

    const auto MON = pMonitor.lock();
    if (!MON)
        return false;

    if (closing && (externalWorkspaceMoveDuringClose || MON->m_activeWorkspace != startedOn))
        return false;

    return true;
}


void COverview::fullRender() {
    const auto MON = pMonitor.lock();
    if (!MON)
        return;

    if (MON->m_activeWorkspace != startedOn && !closing) {
        // likely user changed.
        onWorkspaceChange();
    }

    Vector2D SIZE = size->value();

    const auto GAPSIZE = (closing ? (1.0 - size->getPercent()) : size->getPercent()) * GAP_WIDTH;
    const auto OUTER   = currentOuterInset();
    const auto SHAPE   = currentGridShape();

    clearWithColor(BG_COLOR.stripA());
    if (wallpaperBg && MON->m_background) {
        CRegion backgroundDamage{0, 0, INT16_MAX, INT16_MAX};
        CBox    backgroundBox{{0, 0}, MON->m_transformedSize};
        Render::GL::g_pHyprOpenGL->renderTextureInternal(MON->m_background, backgroundBox, {.damage = &backgroundDamage, .a = 1.0f});
        Render::GL::g_pHyprOpenGL->renderRect(backgroundBox, CHyprColor{0x00000066}, {});
    }

    static auto* const* PTILEROUND  = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:tile_rounding")->getDataStaticPtr();
    static auto* const* PTOUNDPWR   = (Hyprlang::FLOAT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:tile_rounding_power")->getDataStaticPtr();
    static auto* const* PTILEROUNDF = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:tile_rounding_focus")->getDataStaticPtr();
    static auto* const* PTILEROUNDC = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:tile_rounding_current")->getDataStaticPtr();
    static auto* const* PTILEROUNDH = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:tile_rounding_hover")->getDataStaticPtr();

    const int   BASE_ROUND_SCALED    = std::max(0, (int)std::lround((double)**PTILEROUND * MON->m_scale));
    const int   FOCUS_ROUND_SCALED   = **PTILEROUNDF >= 0 ? std::max(0, (int)std::lround((double)**PTILEROUNDF * MON->m_scale)) : BASE_ROUND_SCALED;
    const int   CURRENT_ROUND_SCALED = **PTILEROUNDC >= 0 ? std::max(0, (int)std::lround((double)**PTILEROUNDC * MON->m_scale)) : BASE_ROUND_SCALED;
    const int   HOVER_ROUND_SCALED   = **PTILEROUNDH >= 0 ? std::max(0, (int)std::lround((double)**PTILEROUNDH * MON->m_scale)) : BASE_ROUND_SCALED;
    const float ROUND_PWR            = **PTOUNDPWR;

    std::vector<CBox> tileBoxes(images.size());
    const bool        entryAnimationActive = animateEntry && !closing;
    bool              entryAnimationPending = false;
    const double      entryElapsed = entryAnimationActive ? std::chrono::duration<double>(std::chrono::steady_clock::now() - createdAt).count() : 0.0;

    for (size_t y = 0; y < (size_t)SHAPE.rows; ++y) {
        for (size_t x = 0; x < (size_t)SHAPE.cols; ++x) {
            const int id = x + y * SHAPE.cols;
            if (id < 0 || id >= (int)images.size())
                continue;
            CBox texbox = tileBoxForIndex(id, SIZE, GAPSIZE, OUTER, true);
            texbox.scale(MON->m_scale).translate(pos->value());
            texbox.round();
            tileBoxes[id] = texbox;

            int tileRound = BASE_ROUND_SCALED;
            if ((int)id == kbFocusID)
                tileRound = FOCUS_ROUND_SCALED;
            else if ((int)id == openedID)
                tileRound = CURRENT_ROUND_SCALED;
            else if ((int)id == hoveredID)
                tileRound = HOVER_ROUND_SCALED;

            const int maxCornerPx = std::max(0, (int)std::floor(std::min(texbox.w, texbox.h) / 2.0));
            tileRound = std::min(tileRound, maxCornerPx);

            float alpha = 1.0f;
            if (entryAnimationActive) {
                const double delay = (double)id * 0.05;
                const double raw   = std::clamp((entryElapsed - delay) / 0.2, 0.0, 1.0);
                alpha              = (float)(raw * raw * (3.0 - 2.0 * raw));
                if (raw < 1.0)
                    entryAnimationPending = true;
            }

            CRegion damage{0, 0, INT16_MAX, INT16_MAX};
            Render::GL::g_pHyprOpenGL->renderTextureInternal(images[id].fb->getTexture(), texbox, {.damage = &damage, .a = alpha, .round = tileRound, .roundingPower = ROUND_PWR});
        }
    }

    // overlays: labels and borders
    static auto* const* PLABELEN    = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_enable")->getDataStaticPtr();
    static auto* const* PLABELSIZE  = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_font_size")->getDataStaticPtr();
    static auto const*  PLABELPOS   = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_position")->getDataStaticPtr();
    static auto const*  PLABELPOSL  = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_pos")->getDataStaticPtr();
    static auto* const* PLABELSIZEL = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_size")->getDataStaticPtr();
    static auto* const* PACTCOL     = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:active_highlight_col")->getDataStaticPtr();
    static auto* const* PHOVCOL     = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:hover_highlight_col")->getDataStaticPtr();
    static auto const*  PLABELMODE  = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_text_mode")->getDataStaticPtr();
    static auto const*  PTOKENMAP   = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_token_map")->getDataStaticPtr();
    static auto* const* PLABELOX    = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_offset_x")->getDataStaticPtr();
    static auto* const* PLABELOY    = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_offset_y")->getDataStaticPtr();
    static auto const*  PLABELSHOW  = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_show")->getDataStaticPtr();
    static auto* const* PLCOLDEF    = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_color_default")->getDataStaticPtr();
    static auto* const* PLCOLHOV    = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_color_hover")->getDataStaticPtr();
    static auto* const* PLCOLFOC    = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_color_focus")->getDataStaticPtr();
    static auto* const* PLCOLCUR    = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_color_current")->getDataStaticPtr();
    static auto* const* PWSNUMCOL   = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:workspace_number_color")->getDataStaticPtr();
    static auto* const* PLSCALEH    = (Hyprlang::FLOAT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_scale_hover")->getDataStaticPtr();
    static auto* const* PLSCALEF    = (Hyprlang::FLOAT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_scale_focus")->getDataStaticPtr();
    static auto* const* PLBGEN      = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_bg_enable")->getDataStaticPtr();
    static auto* const* PLBGCOL     = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_bg_color")->getDataStaticPtr();
    static auto* const* PLBGROUND   = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_bg_rounding")->getDataStaticPtr();
    static auto const*  PLBGSHAPE   = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_bg_shape")->getDataStaticPtr();
    static auto* const* PLBGPAD     = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_padding")->getDataStaticPtr();

    static auto* const* PBWIDTH     = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:border_width")->getDataStaticPtr();
    static auto const*  PBCOLCUR    = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:border_color_current")->getDataStaticPtr();
    static auto const*  PBCOLFOC    = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:border_color_focus")->getDataStaticPtr();
    static auto const*  PBCOLHOV    = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:border_color_hover")->getDataStaticPtr();
    static auto const*  PBGRCUR     = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:border_grad_current")->getDataStaticPtr();
    static auto const*  PBGREFOC    = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:border_grad_focus")->getDataStaticPtr();
    static auto const*  PBGREHOV    = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:border_grad_hover")->getDataStaticPtr();

    static auto* const* PDRAGPROXYCOL    = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:drag_drop_proxy_color")->getDataStaticPtr();
    static auto* const* PDRAGPROXYACTCOL = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:drag_drop_proxy_active_color")->getDataStaticPtr();
    static auto const*  PDRAGPROXYBORDER = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:drag_drop_proxy_border_color")->getDataStaticPtr();
    static auto* const* PDRAGPROXYBWIDTH = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:drag_drop_proxy_border_width")->getDataStaticPtr();
    static auto* const* PDRAGPROXYROUND  = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:drag_drop_proxy_rounding")->getDataStaticPtr();
    static auto const*  PDRAGSOURCEBORDER = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:drag_drop_source_border_color")->getDataStaticPtr();
    static auto* const* PDRAGSOURCEBWIDTH = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:drag_drop_source_border_width")->getDataStaticPtr();

    static auto* const* PSELECTEN   = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:selection_label_enable")->getDataStaticPtr();
    static auto const*  PSELECTMAP  = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:selection_label_token_map")->getDataStaticPtr();
    static auto const*  PSELECTPOS  = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:selection_label_position")->getDataStaticPtr();
    static auto* const* PSELECTOX   = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:selection_label_offset_x")->getDataStaticPtr();
    static auto* const* PSELECTOY   = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:selection_label_offset_y")->getDataStaticPtr();
    static auto* const* PSELECTCOL  = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:selection_label_color")->getDataStaticPtr();

    auto normalizeAnchor = [](std::string anchor) {
        std::replace(anchor.begin(), anchor.end(), '_', '-');
        return anchor;
    };

    auto resolveWorkspaceName = [&](size_t id) -> std::string {
        if (id >= images.size())
            return {};

        const auto& image = images[id];
        if (image.pWorkspace && !image.pWorkspace->m_name.empty())
            return image.pWorkspace->m_name;

        for (const auto& workspace : State::workspaceState()->workspacesCopy()) {
            if (!workspace || workspace->m_id != image.workspaceID)
                continue;

            if (!workspace->m_name.empty())
                return workspace->m_name;
            break;
        }

        return std::to_string(image.workspaceID);
    };

    auto renderLabel = [&](SP<Render::ITexture>& tex, Vector2D& sz, const std::string& label, const CHyprColor& col, float scaleMul, const CBox& tile, const std::string& anchor,
                           int offsetX, int offsetY, int fontSize) {
        if (label.empty())
            return;

        const int baseF = std::max(8, fontSize);
        if (!tex || tex->m_texID == 0) {
            const int fsz = std::max(8, (int)std::round(baseF * scaleMul));
            Vector2D  buf{std::max(32, fsz * std::max(2, (int)label.size())), std::max(24, fsz + 8)};
            sz  = buf;
            tex = renderNumberTexture(label, col, buf, MON->m_scale, fsz);
        }

        if (!tex || tex->m_texID == 0)
            return;

        static auto* const* PLPIXELSNAP = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_pixel_snap")->getDataStaticPtr();

        auto placeBox = [&](const CBox& tileBox, const Vector2D& size, const std::string& anchorName, int offsetX2, int offsetY2) -> CBox {
            double x = tileBox.x, y = tileBox.y;
            if (anchorName == "top-left") {
                x += offsetX2;
                y += offsetY2;
            } else if (anchorName == "top-right") {
                x += tileBox.w - size.x - offsetX2;
                y += offsetY2;
            } else if (anchorName == "bottom-left") {
                x += offsetX2;
                y += tileBox.h - size.y - offsetY2;
            } else if (anchorName == "bottom-right") {
                x += tileBox.w - size.x - offsetX2;
                y += tileBox.h - size.y - offsetY2;
            } else {
                x += (tileBox.w - size.x) / 2.0;
                y += (tileBox.h - size.y) / 2.0;
            }
            return CBox{x, y, (double)size.x, (double)size.y};
        };

        auto drawWithBG = [&]() {
            const int pad = **PLBGPAD;
            Vector2D  bgSize = {sz.x + pad * 2, sz.y + pad * 2};
            const std::string shape{*PLBGSHAPE};
            int roundPx = **PLBGROUND;
            if (shape == "circle" || shape == "square") {
                const double side = std::max(bgSize.x, bgSize.y);
                bgSize            = {side, side};
                roundPx           = (shape == "circle") ? std::lround(side / 2.0) : 0;
            }
            CBox bg = placeBox(tile, bgSize, anchor, offsetX, offsetY);
            CBox lb{bg.x + (bg.w - sz.x) / 2.0, bg.y + (bg.h - sz.y) / 2.0, (double)sz.x, (double)sz.y};
            if (**PLPIXELSNAP) {
                bg.round();
                lb.round();
            }
            Render::GL::g_pHyprOpenGL->renderRect(bg, CHyprColor{(uint64_t)**PLBGCOL}, {.round = roundPx});
            Render::GL::g_pHyprOpenGL->renderTexture(tex, lb, {.a = 1.0});
        };

        auto drawNoBG = [&]() {
            CBox lb = placeBox(tile, sz, anchor, offsetX, offsetY);
            if (**PLPIXELSNAP)
                lb.round();
            Render::GL::g_pHyprOpenGL->renderTexture(tex, lb, {.a = 1.0});
        };

        if (**PLBGEN)
            drawWithBG();
        else
            drawNoBG();
    };

    auto drawBorderForID = [&](int id, const std::string& borderSpec, const std::string& deprecatedGradSpec, int roundScaled, int borderWidthOverride = -1) {
        if (!isTileValid(id) || id < 0 || id >= (int)tileBoxes.size())
            return;
        if (borderWidthOverride == 0)
            return;

        const CBox& box = tileBoxes[id];
        if (box.w <= 0.0 || box.h <= 0.0)
            return;

        const int BWIDTH = borderWidthOverride > 0 ? borderWidthOverride : (int)**PBWIDTH;
        if (BWIDTH <= 0)
            return;

        const std::string effectiveSpec = Hyprexpo::resolveBorderSpec(borderSpec, deprecatedGradSpec);

        if (isGradientBorderSpec(effectiveSpec)) {
            const auto spec = parseGradientSpec(effectiveSpec);
            if (spec.valid) {
                Config::CGradientValueData grad;
                grad.m_colors.clear();
                grad.m_colors.push_back(spec.c1);
                grad.m_colors.push_back(spec.c2);
                grad.m_angle = spec.angleDeg * (float)M_PI / 180.f;
                grad.updateColorsOk();
                Render::GL::g_pHyprOpenGL->renderBorder(box, grad, {.round = roundScaled, .roundingPower = ROUND_PWR, .borderSize = BWIDTH});
            }
        } else if (!effectiveSpec.empty()) {
            Hyprexpo::SColorRGBA parsedColor;
            if (Hyprexpo::parseSolidColorSpec(effectiveSpec, parsedColor)) {
                CHyprColor color{parsedColor.r, parsedColor.g, parsedColor.b, parsedColor.a};
                Render::GL::g_pHyprOpenGL->renderBorder(box, color, {.round = roundScaled, .roundingPower = ROUND_PWR, .borderSize = BWIDTH});
            } else {
                Log::logger->log(Log::ERR, "[hyprexpo] invalid border color config: {}", effectiveSpec);
            }
        }
    };

    auto drawProxyBorder = [&](const CBox& proxy, int round, int borderWidth, const std::string& borderSpec, const std::string& fallbackSpec) {
        if (borderWidth <= 0)
            return;

        const std::string effectiveSpec = borderSpec.empty() ? fallbackSpec : borderSpec;
        if (effectiveSpec.empty())
            return;

        if (isGradientBorderSpec(effectiveSpec)) {
            const auto spec = parseGradientSpec(effectiveSpec);
            if (!spec.valid)
                return;

            Config::CGradientValueData grad;
            grad.m_colors = {spec.c1, spec.c2};
            grad.m_angle  = spec.angleDeg * (float)M_PI / 180.f;
            grad.updateColorsOk();
            Render::GL::g_pHyprOpenGL->renderBorder(proxy, grad, {.round = round, .roundingPower = ROUND_PWR, .borderSize = borderWidth});
            return;
        }

        Hyprexpo::SColorRGBA parsedColor;
        if (!Hyprexpo::parseSolidColorSpec(effectiveSpec, parsedColor)) {
            Log::logger->log(Log::ERR, "[hyprexpo] invalid drag_drop_proxy_border_color config: {}", effectiveSpec);
            return;
        }

        Config::CGradientValueData grad{CHyprColor{parsedColor.r, parsedColor.g, parsedColor.b, parsedColor.a}};
        grad.updateColorsOk();
        Render::GL::g_pHyprOpenGL->renderBorder(proxy, grad, {.round = round, .roundingPower = ROUND_PWR, .borderSize = borderWidth});
    };

    std::vector<std::string> selectionTokens;
    if (!std::string{*PSELECTMAP}.empty())
        selectionTokens = splitCommaList(std::string{*PSELECTMAP});

    if (**PLABELEN || **PSELECTEN || showWorkspaceNumbers) {
        const int labelHoveredID = closing ? -1 : hoveredID;
        const std::string modernAnchor = Hyprexpo::trimString(std::string{*PLABELPOS});
        const std::string labelAnchor  = normalizeAnchor(modernAnchor.empty() ? std::string{*PLABELPOSL} : modernAnchor);
        const int labelFontSize = **PLABELSIZE > 0 ? **PLABELSIZE : std::max(8, (int)**PLABELSIZEL / 2);

        auto resolveState = [&](int id) -> int {
            if (id == kbFocusID)
                return 2;
            if (id == openedID)
                return 3;
            if (id == labelHoveredID)
                return 1;
            return 0;
        };

        std::vector<std::string> labelTokens;
        if (!std::string{*PTOKENMAP}.empty())
            labelTokens = splitCommaList(std::string{*PTOKENMAP});

        int tokenCounter = 0;
        for (size_t id = 0; id < images.size(); ++id) {
            const auto& image = images[id];
            const auto& tile  = tileBoxes[id];

            if (image.workspaceID == WORKSPACE_INVALID || tile.w <= 0.0 || tile.h <= 0.0)
                continue;

            const bool labelEnabled = **PLABELEN || showWorkspaceNumbers;
            const std::string labelShow = showWorkspaceNumbers ? "always" : std::string{*PLABELSHOW};
            if (Hyprexpo::shouldShowWorkspaceLabel(labelEnabled, labelShow, (int)id == labelHoveredID, (int)id == kbFocusID, (int)id == openedID)) {
                std::string label;
                const std::string mode = showWorkspaceNumbers ? std::string{"id"} : std::string{*PLABELMODE};
                if (dynamicGrid && showWorkspaceNames) {
                    label = resolveWorkspaceName(id);
                } else if (mode == "token") {
                    if (tokenCounter < (int)labelTokens.size() && !labelTokens[tokenCounter].empty())
                        label = labelTokens[tokenCounter];
                    else
                        label = fallbackTokenForVisibleIndex(tokenCounter);
                } else if (mode == "index") {
                    label = std::to_string(tokenCounter + 1);
                } else {
                    label = std::to_string(images[id].workspaceID);
                }

                const int st = resolveState((int)id);
                if (!label.empty()) {
                    if (showWorkspaceNumbers)
                        renderLabel(images[id].labelTexDefault, images[id].labelSizeDefault, label, CHyprColor{(uint64_t)**PWSNUMCOL}, 1.0f, tile, labelAnchor, **PLABELOX, **PLABELOY,
                                    labelFontSize);
                    else if (st == 1)
                        renderLabel(images[id].labelTexHover, images[id].labelSizeHover, label, CHyprColor{(uint64_t)**PLCOLHOV}, **PLSCALEH, tile, labelAnchor, **PLABELOX,
                                    **PLABELOY, labelFontSize);
                    else if (st == 2)
                        renderLabel(images[id].labelTexFocus, images[id].labelSizeFocus, label, CHyprColor{(uint64_t)**PLCOLFOC}, **PLSCALEF, tile, labelAnchor, **PLABELOX,
                                    **PLABELOY, labelFontSize);
                    else if (st == 3)
                        renderLabel(images[id].labelTexCurrent, images[id].labelSizeCurrent, label, CHyprColor{(uint64_t)**PLCOLCUR}, 1.0f, tile, labelAnchor, **PLABELOX,
                                    **PLABELOY, labelFontSize);
                    else
                        renderLabel(images[id].labelTexDefault, images[id].labelSizeDefault, label, CHyprColor{(uint64_t)**PLCOLDEF}, 1.0f, tile, labelAnchor, **PLABELOX,
                                    **PLABELOY, labelFontSize);
                }
            }

            if (**PSELECTEN && tokenCounter < (int)selectionTokens.size() && !selectionTokens[tokenCounter].empty())
                renderLabel(images[id].selectionLabelTex, images[id].selectionLabelSize, selectionTokens[tokenCounter], CHyprColor{(uint64_t)**PSELECTCOL}, 1.0f, tile,
                            std::string{*PSELECTPOS}, **PSELECTOX, **PSELECTOY, **PLABELSIZE);

            ++tokenCounter;
        }
    }

    const int RND_CUR = CURRENT_ROUND_SCALED;
    const int RND_FOC = FOCUS_ROUND_SCALED;
    const int RND_HOV = HOVER_ROUND_SCALED;

    auto legacyColorSpec = [](uint64_t color) {
        constexpr char HEX[] = "0123456789abcdef";
        std::string    spec  = "0x00000000";
        for (int index = 0; index < 8; ++index)
            spec[9 - index] = HEX[(color >> (index * 4)) & 0xF];
        return spec;
    };

    const std::string currentLegacySpec = dynamicGrid ? legacyColorSpec((uint64_t)**PACTCOL) : std::string{};
    const std::string hoverLegacySpec   = dynamicGrid ? legacyColorSpec((uint64_t)**PHOVCOL) : std::string{};
    const std::string currentFallback   = Hyprexpo::resolveBorderSpec(std::string{*PBGRCUR}, currentLegacySpec);
    const std::string hoverFallback     = Hyprexpo::resolveBorderSpec(std::string{*PBGREHOV}, hoverLegacySpec);

    if (hoveredID != -1 && hoveredID != openedID && hoveredID != kbFocusID)
        drawBorderForID(hoveredID, std::string{*PBCOLHOV}, hoverFallback, RND_HOV);
    drawBorderForID(openedID, std::string{*PBCOLCUR}, currentFallback, RND_CUR);
    if (kbFocusID != -1)
        drawBorderForID(kbFocusID, std::string{*PBCOLFOC}, std::string{*PBGREFOC}, RND_FOC);

    if (dragMoved && dragSourceID != -1) {
        const std::string sourceBorder = std::string{*PDRAGSOURCEBORDER}.empty() ? std::string{*PBCOLFOC} : std::string{*PDRAGSOURCEBORDER};
        const int         sourceWidth  = **PDRAGSOURCEBWIDTH >= 0 ? **PDRAGSOURCEBWIDTH : (int)**PBWIDTH;
        drawBorderForID(dragSourceID, sourceBorder, std::string{*PBGREFOC}, RND_FOC, sourceWidth);
    }

    dropIntent         = {};
    dropIntentTargetID = -1;

    if (dragWindow && isTileValid(dragSourceID)) {
        const auto windowBox = dragWindow->getWindowMainSurfaceBox();
        if (windowBox.w > 0 && windowBox.h > 0) {
            const CBox& sourceBox = tileBoxes[dragSourceID];
            const double scaleX   = sourceBox.w / MON->m_size.x;
            const double scaleY   = sourceBox.h / MON->m_size.y;
            const double minW     = std::min(sourceBox.w, 24.0 * MON->m_scale);
            const double minH     = std::min(sourceBox.h, 24.0 * MON->m_scale);

            CBox proxy{
                lastMousePosLocal.x * MON->m_scale - dragGrabOffset.x * scaleX,
                lastMousePosLocal.y * MON->m_scale - dragGrabOffset.y * scaleY,
                std::clamp(windowBox.w * scaleX, minW, sourceBox.w),
                std::clamp(windowBox.h * scaleY, minH, sourceBox.h),
            };
            proxy.round();

            const int maxProxyRound = std::max(0, (int)std::floor(std::min(proxy.w, proxy.h) / 2.0));
            const int autoRound     = std::min(RND_FOC, maxProxyRound);
            const int round        = **PDRAGPROXYROUND >= 0 ? std::min(std::max(0, (int)std::lround((double)**PDRAGPROXYROUND * MON->m_scale)), maxProxyRound) : autoRound;

            if (dragMoved && hoveredID != -1 && hoveredID != dragSourceID && isTileValid(hoveredID)) {
                const auto targetTileBox = tileBoxForIndex(hoveredID, SIZE, GAPSIZE, OUTER, true);
                const Hyprexpo::SRect targetTileLocal{targetTileBox.x, targetTileBox.y, targetTileBox.w, targetTileBox.h};
                dropIntent = Hyprexpo::computeDropIntentGeometry({
                    .targetValid     = true,
                    .pointerLocal    = {lastMousePosLocal.x, lastMousePosLocal.y},
                    .targetTileLocal = targetTileLocal,
                    .workspaceSize   = {MON->m_size.x, MON->m_size.y},
                    .windowSize      = {windowBox.w, windowBox.h},
                    .grabOffset      = {dragGrabOffset.x, dragGrabOffset.y},
                });
                dropIntentTargetID = dropIntent.valid ? hoveredID : -1;
            }

            if (dropIntent.valid) {
                CBox targetProxy{
                    dropIntent.targetProxyLocal.x,
                    dropIntent.targetProxyLocal.y,
                    dropIntent.targetProxyLocal.w,
                    dropIntent.targetProxyLocal.h,
                };
                targetProxy.scale(MON->m_scale).translate(pos->value());
                targetProxy.round();

                const int targetMaxRound = std::max(0, (int)std::floor(std::min(targetProxy.w, targetProxy.h) / 2.0));
                const int targetRound    = **PDRAGPROXYROUND >= 0 ? std::min(std::max(0, (int)std::lround((double)**PDRAGPROXYROUND * MON->m_scale)), targetMaxRound) : std::min(RND_FOC, targetMaxRound);
                Render::GL::g_pHyprOpenGL->renderRect(targetProxy, CHyprColor{(uint64_t)**PDRAGPROXYACTCOL}, {.round = targetRound, .roundingPower = ROUND_PWR});

                const int   borderWidth   = **PDRAGPROXYBWIDTH >= 0 ? **PDRAGPROXYBWIDTH : std::max(2, (int)**PBWIDTH + 1);
                const std::string effectiveSpec = std::string{*PDRAGPROXYBORDER}.empty() ? std::string{*PBCOLFOC} : std::string{*PDRAGPROXYBORDER};
                drawProxyBorder(targetProxy, targetRound, borderWidth, effectiveSpec, std::string{*PBGREFOC});
            }

            Render::GL::g_pHyprOpenGL->renderRect(proxy, CHyprColor{(uint64_t)(dragMoved ? **PDRAGPROXYACTCOL : **PDRAGPROXYCOL)}, {.round = round, .roundingPower = ROUND_PWR});

            const int   borderWidth   = **PDRAGPROXYBWIDTH >= 0 ? **PDRAGPROXYBWIDTH : std::max(2, (int)**PBWIDTH + 1);
            std::string effectiveSpec = std::string{*PDRAGPROXYBORDER}.empty() ? std::string{*PBCOLFOC} : std::string{*PDRAGPROXYBORDER};
            if (effectiveSpec.empty())
                effectiveSpec = std::string{*PBGREFOC};
            drawProxyBorder(proxy, round, borderWidth, effectiveSpec, std::string{*PBGREFOC});
        }
    }

    if (entryAnimationPending)
        damage();
}
