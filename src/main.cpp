#include "GlassDecoration.hpp"
#include "GlassLayerCompositeElement.hpp"
#include "GlassLayerPassElement.hpp"
#include "GlassLayerSurface.hpp"
#include "GlassRenderer.hpp"
#include "Globals.hpp"
#include "PluginConfig.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/state/ViewState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/render/decorations/DecorationPositioner.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/event/EventBus.hpp>

#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <sstream>

static void clearLayerGlassOnClose(PHLLS layerSurface) {
    if (!g_pGlobalState || !layerSurface)
        return;

    // Drop cached layer glass immediately. Otherwise the previous glass output
    // can remain in the damage history while Hyprland switches to its close
    // snapshot path, showing stale/black pixels for a frame.
    std::erase_if(g_pGlobalState->layerSurfaces, [&](const auto& pair) {
        return pair.first == layerSurface.get() || pair.second->getLayerSurface() == layerSurface;
    });

    if (auto monitor = layerSurface->m_monitor.lock())
        g_pHyprRenderer->damageMonitor(monitor);
}

static void onNewWindow(PHLWINDOW window) {
    if (std::ranges::any_of(window->m_windowDecorations,
                            [](const auto& decoration) { return decoration->getDisplayName() == "HyprGlass"; }))
        return;

    auto decoration = makeUnique<CGlassDecoration>(window);
    g_pGlobalState->decorations.emplace_back(decoration);
    decoration->m_self = decoration;
    HyprlandAPI::addWindowDecoration(PHANDLE, window, std::move(decoration));
}

static void onCloseWindow(PHLWINDOW window) {
    // Prune EXPIRED entries ONLY — never live ones. "close" fires on UNMAP,
    // and XWayland windows (Steam is full of them) unmap and remap without
    // ever being destroyed. The old version also erased the entry whose owner
    // matched the closing window, which FORGOT a still-attached decoration:
    // PLUGIN_EXIT could then no longer remove it, dlclose unmapped its code,
    // and the window's eventual REAL destruction called a virtual destructor
    // through a vtable pointing into freed pages — a compositor SEGV that
    // fired on "exit Steam" (2026-08-03). When a window truly dies, its
    // decoration dies with it and the entry expires; the prune below catches
    // it on the next close event. Tracking a live decoration twice as long is
    // free; forgetting it once was fatal.
    (void)window;
    std::erase_if(g_pGlobalState->decorations, [](const auto& decoration) {
        return !decoration.get();
    });
}

// ── Layer surface support ────────────────────────────────────────────────────

// Parse comma-separated config string into a set of trimmed values.
static void parseCommaSeparated(StringConfigPtr configPtr, std::unordered_set<std::string>& out) {
    out.clear();
    const auto raw = readStringConfig(configPtr);
    if (raw.empty()) return;

    std::istringstream stream{std::string(raw)};
    std::string token;
    while (std::getline(stream, token, ',')) {
        auto start = token.find_first_not_of(" \t");
        auto end   = token.find_last_not_of(" \t");
        if (start != std::string::npos)
            out.insert(token.substr(start, end - start + 1));
    }
}

// Parse comma-separated "key<sep>value" pairs. The callback receives (key, valueStr) for each pair.
template <typename Fn>
static void parseKeyValuePairs(StringConfigPtr configPtr, char separator, Fn&& callback) {
    const auto raw = readStringConfig(configPtr);
    if (raw.empty()) return;

    std::istringstream stream{std::string(raw)};
    std::string token;
    while (std::getline(stream, token, ',')) {
        auto sepPos = token.rfind(separator);
        if (sepPos == std::string::npos) continue;

        auto kStart = token.find_first_not_of(" \t");
        auto kEnd   = token.find_last_not_of(" \t", sepPos - 1);
        auto vStart = token.find_first_not_of(" \t", sepPos + 1);
        auto vEnd   = token.find_last_not_of(" \t");

        if (kStart != std::string::npos && kEnd != std::string::npos &&
            vStart != std::string::npos && vEnd != std::string::npos && kStart <= kEnd && vStart <= vEnd) {
            callback(token.substr(kStart, kEnd - kStart + 1),
                     token.substr(vStart, vEnd - vStart + 1));
        }
    }
}

static void parseLayerNamespaceFilters() {
    const auto& config = g_pGlobalState->config;
    parseCommaSeparated(config.layersNamespaces, g_pGlobalState->layerNamespaceFilter);
    parseCommaSeparated(config.layersExcludeNamespaces, g_pGlobalState->layerNamespaceExclude);

    g_pGlobalState->layerNamespacePresets.clear();
    parseKeyValuePairs(config.layersNamespacePresets, ':', [&](const std::string& ns, const std::string& preset) {
        g_pGlobalState->layerNamespacePresets.emplace(ns, preset);
    });

    g_pGlobalState->layerNamespaceMaskThresholds.clear();
    parseKeyValuePairs(config.layersNamespaceMaskThresholds, '=', [&](const std::string& ns, const std::string& val) {
        try { g_pGlobalState->layerNamespaceMaskThresholds.emplace(ns, std::stof(val)); } catch (...) {}
    });
}

static bool shouldGlassLayer(PHLLS layerSurface) {
    if (!layerSurface)
        return false;

    const auto& ns = layerSurface->m_namespace;

    // Exclusion takes priority
    if (g_pGlobalState->layerNamespaceExclude.contains(ns))
        return false;

    const auto& include = g_pGlobalState->layerNamespaceFilter;
    if (include.empty())
        return true;

    return include.contains(ns);
}

using renderLayerFn = void (*)(Render::IHyprRenderer*, PHLLS, PHLMONITOR, const Time::steady_tp&, bool, bool);

// Debug-armed forensic tap: every window render, with the decorate flag and
// pass mode. The glass is a DECORATION - any render that comes through here
// with decorate=false paints the window with NO water at all, and one such
// frame parked in a swapchain buffer is exactly one frame of the unfocused-
// window strobe. This names the subsystem doing it.
using renderWindowFn = void (*)(Render::IHyprRenderer*, PHLWINDOW, PHLMONITOR, const Time::steady_tp&, bool, int, bool, bool);

static void hkRenderWindow(Render::IHyprRenderer* thisptr, PHLWINDOW window, PHLMONITOR monitor,
                           const Time::steady_tp& now, bool decorate, int mode, bool ignorePosition, bool standalone) {
    GlassRenderer::DBG_LOG("RENDERWIN win=%lx dec=%d mode=%d standalone=%d ignpos=%d\n",
                           reinterpret_cast<uintptr_t>(window.get()), decorate ? 1 : 0, mode,
                           standalone ? 1 : 0, ignorePosition ? 1 : 0);
    ((renderWindowFn)g_pGlobalState->renderWindowHook->m_original)(thisptr, window, monitor, now, decorate, mode, ignorePosition, standalone);
}

static void hkRenderLayer(Render::IHyprRenderer* thisptr, PHLLS layerSurface, PHLMONITOR monitor,
                           const Time::steady_tp& now, bool popups, bool lockscreen) {
    const auto& config = g_pGlobalState->config;

    // Hyprland renders closing layers from snapshots. Do not inject the glass
    // pipeline while that snapshot is being captured: the snapshot framebuffer
    // starts transparent/black, so sampling it as a background can bake a black
    // rectangle into the fade-out snapshot.
    if (g_pHyprRenderer->m_bRenderingSnapshot) {
        ((renderLayerFn)g_pGlobalState->renderLayerHook->m_original)(thisptr, layerSurface, monitor, now, popups, lockscreen);
        return;
    }

    // Prune dead layer surfaces whose weak_ptr has expired (layer was destroyed
    // but never got a replacement at the same raw pointer address)
    std::erase_if(g_pGlobalState->layerSurfaces, [](const auto& pair) {
        return !pair.second->getLayerSurface();
    });

    // Only inject glass on the main surface pass, not popups
    if (!popups && config.layersEnabled && **config.layersEnabled && shouldGlassLayer(layerSurface)) {
        // Lazy-create per-layer state, replacing stale entries whose weak ref died
        // (can happen when a new CLayerSurface is allocated at the same address)
        auto* rawPtr = layerSurface.get();
        auto& layerStates = g_pGlobalState->layerSurfaces;
        auto it = layerStates.find(rawPtr);
        if (it != layerStates.end() && !it->second->getLayerSurface()) {
            it->second = std::make_shared<CGlassLayerSurface>(layerSurface);
        } else if (it == layerStates.end()) {
            it = layerStates.emplace(rawPtr, std::make_shared<CGlassLayerSurface>(layerSurface)).first;
        }

        if (!layerSurface->m_mapped) {   // 0.56: fading-out layers render via CFadingOutState, not here
            ((renderLayerFn)g_pGlobalState->renderLayerHook->m_original)(thisptr, layerSurface, monitor, now, popups, lockscreen);
            return;
        }

        float alpha = layerSurface->alpha().value();
        if (alpha < 0.001f) {
            ((renderLayerFn)g_pGlobalState->renderLayerHook->m_original)(thisptr, layerSurface, monitor, now, popups, lockscreen);
            return;
        }

        // Pre-surface: sample+blur background, redirect currentFB → temp FBO
        CGlassLayerPassElement::SGlassLayerPassData preData{it->second, alpha};
        g_pHyprRenderer->m_renderPass.add(makeUnique<CGlassLayerPassElement>(preData));

        // Original renderLayer: surface renders into the redirected temp FBO
        ((renderLayerFn)g_pGlobalState->renderLayerHook->m_original)(thisptr, layerSurface, monitor, now, popups, lockscreen);

        // Post-surface: restore currentFB, apply glass masked by temp FBO alpha, blit surface
        CGlassLayerCompositeElement::SGlassLayerCompositeData postData{it->second, alpha};
        g_pHyprRenderer->m_renderPass.add(makeUnique<CGlassLayerCompositeElement>(postData));

        it->second->damageIfMoved();
        return;
    }

    // Call the original renderLayer
    ((renderLayerFn)g_pGlobalState->renderLayerHook->m_original)(thisptr, layerSurface, monitor, now, popups, lockscreen);
}



// ============================================================================
//  SHIMMER CLOCK
//
//  hyprwater is normally damage-driven: it redraws when something changes.
//  An animated caustic has nothing to change, so without a heartbeat the wave
//  field would be frozen at whatever t the last unrelated redraw happened to
//  land on. This timer supplies that heartbeat, and ONLY while the shimmer is
//  actually on — when it is off the timer idles at half-second granularity and
//  damages nothing, so a user who never enables it pays for two wakeups a
//  second and no rendering at all.
//
//  This is also precisely why the effects governor exists: while this is
//  running the compositor is drawing continuously rather than on demand, which
//  is exactly the cost a game does not want to share.
// ============================================================================
static SP<CEventLoopTimer> g_shimmerTimer;

static void shimmerTick(SP<CEventLoopTimer> self, void*) {
    if (!g_pGlobalState) {
        self->updateTimeout(std::chrono::milliseconds(500));
        return;
    }

    const auto& cfg = g_pGlobalState->config;
    const bool  on  = cfg.shimmerEnabled && **cfg.shimmerEnabled != 0
                   && cfg.shimmerIntensity && **cfg.shimmerIntensity > 0.0;

    if (on && g_pCompositor && g_pHyprRenderer) {
        // FORCE-DAMAGE EVERY GLASSED WINDOW, not just the monitors.
        //
        // Hyprland re-renders only what changed. Damaging the monitor schedules a
        // frame, but each window still redraws only its own damaged region — so
        // while the water advances every tick, one window last redrew at step 100
        // and its neighbour at step 150, and a small box around some updating
        // text redrew this instant. The result is several different moments of
        // the same simulation on screen at once: the "multiple layers", the hard
        // cut-offs, and the invisible boxes around text that move when you resize
        // (they are damage rectangles, and resizing redraws different ones).
        //
        // forceFull=true makes each glassed window redraw whole, so every surface
        // shows the same instant of water.
        for (auto& deco : g_pGlobalState->decorations) {
            if (!deco) continue;
            auto w = deco->getOwner();
            if (w && w->m_isMapped)
                g_pHyprRenderer->damageWindow(w, true);
        }

        // 0.56 moved the monitor list off CCompositor into the state tracker;
        // g_pCompositor->m_monitors no longer exists.
        for (const auto& monitor : State::monitorState()->monitors()) {
            // Only monitors that are actually presenting. Damaging a DPMS-off
            // or unplugged output would keep it awake for an effect nobody can
            // see, and on a laptop that is a battery leak with no visible cause.
            if (monitor && monitor->m_enabled)
                g_pHyprRenderer->damageMonitor(monitor);
        }
    }

    self->updateTimeout(std::chrono::milliseconds(on ? 16 : 500));
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(PHANDLE,
            std::format("[{}] Version mismatch!", PLUGIN_NAME),
            CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("Version mismatch");
    }

    g_pGlobalState = std::make_unique<SGlobalState>();

    static auto onOpen = Event::bus()->m_events.window.open.listen([&](PHLWINDOW w) { onNewWindow(w); });

    static auto onClose = Event::bus()->m_events.window.close.listen([&](PHLWINDOW w) { onCloseWindow(w); });

    static auto onLayerClosed = Event::bus()->m_events.layer.closed.listen([&](PHLLS layerSurface) { clearLayerGlassOnClose(layerSurface); });

    // Z-order / visibility changes invalidate layer glass caches on the affected monitor only.
    // Per-monitor to avoid triggering re-samples on idle monitors (feedback loop).
    auto bumpWindowMonitor = [&](PHLWINDOW w) {
        if (w) if (auto mon = w->m_monitor.lock()) g_pGlobalState->bumpSceneGeneration(mon.get());
    };
    static auto onWindowActive = Event::bus()->m_events.window.active.listen(
        [=](PHLWINDOW w, Desktop::eFocusReason) { bumpWindowMonitor(w); });
    static auto onWindowFullscreen = Event::bus()->m_events.window.fullscreen.listen(
        [=](PHLWINDOW w) { bumpWindowMonitor(w); });
    static auto onWindowMoveToWorkspace = Event::bus()->m_events.window.moveToWorkspace.listen(
        [=](PHLWINDOW w, PHLWORKSPACE) { bumpWindowMonitor(w); });
    static auto onWorkspaceActive = Event::bus()->m_events.workspace.active.listen(
        [&](PHLWORKSPACE ws) {
            if (ws) if (auto mon = ws->m_monitor.lock()) g_pGlobalState->bumpSceneGeneration(mon.get());
        });

    // Every mouse press taps the water at the cursor (queueClickSplash gates
    // itself on shimmer + the mouse slider, so this is a cheap no-op when off).
    static auto onMouseButton = Event::bus()->m_events.input.mouse.button.listen(
        [&](IPointer::SButtonEvent e, Event::SCallbackInfo&) {
            if (e.state == WL_POINTER_BUTTON_STATE_PRESSED)
                GlassRenderer::queueClickSplash();
        });

    // Clear pending presets/layers before config re-parse, commit after
    static auto onPreConfigReload = Event::bus()->m_events.config.preReload.listen([&]() {
        clearPendingPresets();
        clearPendingLayers();
    });

    static auto onConfigReloaded = Event::bus()->m_events.config.reloaded.listen([&]() {
        initConfigPointers(PHANDLE, g_pGlobalState->config);
        commitPendingPresets();
        parseLayerNamespaceFilters();
        commitPendingLayers(); // merge Lua layer() calls on top of string config
        validateConfig();
    });


    registerConfig(PHANDLE);
    initConfigPointers(PHANDLE, g_pGlobalState->config);

    // Shadows must be enabled for the glass effect to sample the correct background.
    // Force-enable if the user has disabled them.
    const auto shadowEnabled = Config::mgr()->getConfigValue("decoration:shadow:enabled");
    auto* const PSHADOWENABLED = reinterpret_cast<Hyprlang::INT* const*>(shadowEnabled.dataptr);
    if (PSHADOWENABLED && !**PSHADOWENABLED) {
        HyprlandAPI::invokeHyprctlCommand("keyword", "decoration:shadow:enabled true");
    }

    for (auto& window : Desktop::viewState()->windows()) {   // 0.56: window list lives on the desktop view state
        if (window->isHidden() || !window->m_isMapped)
            continue;
        onNewWindow(window);
    }

    // Hook renderLayer for layer surface glass support
    auto renderLayerMatches = HyprlandAPI::findFunctionsByName(PHANDLE, "renderLayer");
    for (const auto& match : renderLayerMatches) {
        // Match the overload: Render::IHyprRenderer::renderLayer(PHLLS, PHLMONITOR, steady_tp, bool, bool)
        if (match.demangled.contains("renderLayer") && match.demangled.contains("LayerSurface")) {
            g_pGlobalState->renderLayerHook = HyprlandAPI::createFunctionHook(PHANDLE, match.address, (void*)hkRenderLayer);
            if (g_pGlobalState->renderLayerHook)
                g_pGlobalState->renderLayerHook->hook();
            break;
        }
    }

    auto renderWindowMatches = HyprlandAPI::findFunctionsByName(PHANDLE, "renderWindow");
    GlassRenderer::DBG_LOG("HOOKSCAN renderWindow: %zu candidates\n", renderWindowMatches.size());
    for (const auto& match : renderWindowMatches) {
        GlassRenderer::DBG_LOG("HOOKSCAN candidate: %s\n", match.demangled.c_str());
        if (match.demangled.contains("IHyprRenderer") && match.demangled.contains("renderWindow")) {
            g_pGlobalState->renderWindowHook = HyprlandAPI::createFunctionHook(PHANDLE, match.address, (void*)hkRenderWindow);
            if (g_pGlobalState->renderWindowHook) {
                const bool ok = g_pGlobalState->renderWindowHook->hook();
                GlassRenderer::DBG_LOG("HOOKSCAN hooked=%d\n", ok ? 1 : 0);
            } else
                GlassRenderer::DBG_LOG("HOOKSCAN createFunctionHook returned null\n");
            break;
        }
    }

    if (!g_pGlobalState->renderLayerHook) {
        HyprlandAPI::addNotificationV2(PHANDLE, {
            {"text", std::string("[hyprwater] Could not hook renderLayer — layer glass disabled")},
            {"time", (uint64_t)5000},
            {"color", CHyprColor{1.0, 0.8, 0.2, 1.0}},
        });
    }

    HyprlandAPI::reloadConfig();
    initConfigPointers(PHANDLE, g_pGlobalState->config);
    commitPendingPresets();
    parseLayerNamespaceFilters();
    commitPendingLayers();
    validateConfig();

    g_shimmerTimer = makeShared<CEventLoopTimer>(std::chrono::milliseconds(500), shimmerTick, nullptr);
    g_pEventLoopManager->addTimer(g_shimmerTimer);

    return {std::string(PLUGIN_NAME), std::string(PLUGIN_DESCRIPTION), std::string(PLUGIN_AUTHOR), std::string(PLUGIN_VERSION)};
}

APICALL EXPORT void PLUGIN_EXIT() {
    // Cancel BEFORE the early return: the timer holds a callback into this
    // shared object, so leaving it armed while the .so unloads means the event
    // loop eventually calls into freed code. That is a compositor crash on
    // plugin unload, and it would happen whether or not global state exists.
    if (g_shimmerTimer) {
        // cancel() only disarms it — the event loop manager STILL HOLDS the
        // shared pointer, so the timer (and its callback into this .so) survives
        // the unload and the loop eventually calls into freed code. It must be
        // removed from the manager as well. Verified: with cancel() alone,
        // `hyprctl plugin unload` SIGSEGVs the compositor every time.
        if (g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(g_shimmerTimer);
        g_shimmerTimer->cancel();
        g_shimmerTimer.reset();
    }

    if (!g_pGlobalState)
        return;

    // Release the wave-simulation framebuffers explicitly. They are GPU
    // resources created by the renderer and held in this plugin's global state;
    // dropping the .so without releasing them leaves the compositor holding
    // framebuffers whose owning code is gone. Reloading the plugin is exactly
    // when that bites, and a reload is what preceded the SIGSEGV on 2026-08-02.
    g_pGlobalState->waveFb[0].reset();
    g_pGlobalState->waveFb[1].reset();
    g_pGlobalState->waveCurrent   = 0;
    g_pGlobalState->waveStepCount = 0;
    // Same rule for the currents field.
    g_pGlobalState->fluidVelFb[0].reset();
    g_pGlobalState->fluidVelFb[1].reset();
    g_pGlobalState->fluidPrsFb[0].reset();
    g_pGlobalState->fluidPrsFb[1].reset();
    g_pGlobalState->fluidDivFb.reset();
    g_pGlobalState->fluidVelCurrent = 0;
    g_pGlobalState->fluidPrsCurrent = 0;
    g_pGlobalState->trailFb.reset();
    g_pGlobalState->causticFb.reset();
    g_pGlobalState->causticTmpFb.reset();

    g_pHyprRenderer->m_renderPass.removeAllOfType("CGlassPassElement");
    g_pHyprRenderer->m_renderPass.removeAllOfType("CGlassLayerPassElement");
    g_pHyprRenderer->m_renderPass.removeAllOfType("CGlassLayerCompositeElement");

    // Destroy every decoration SYNCHRONOUSLY, RIGHT NOW, by erasing it from
    // its window's decoration vector ourselves. Neither official path can be
    // trusted here: HyprlandAPI::removeWindowDecoration resolves the owner
    // through the WINDOW REGISTRY, and an unmap-hidden window (Steam is full
    // of them) is not in it — silent no-op. CWindow::removeWindowDeco defers
    // to a queue drained when the window next renders — an unmapped window
    // never renders, so the decoration outlives dlclose either way. Its
    // vtable then points into freed pages and the window's eventual REAL
    // destruction calls straight into them (2026-08-03, "exited Steam";
    // both official paths re-crashed the nested repro before this version).
    // The positioner must be told first — the base-class destructor does NOT
    // uncache, and a stale positioner entry is just the same bomb elsewhere.
    auto stripGlassDeco = [](PHLWINDOW w) {
        if (!w)
            return;
        for (const auto& d : w->m_windowDecorations)
            if (d && d->getDisplayName() == "HyprGlass" && g_pDecorationPositioner)
                g_pDecorationPositioner->uncacheDecoration(d.get());
        std::erase_if(w->m_windowDecorations, [](const auto& d) {
            return d && d->getDisplayName() == "HyprGlass";
        });
    };
    // Tracked decorations first (their owners may live OUTSIDE the registry:
    // unmap-hidden or fading out)...
    for (auto& decoration : g_pGlobalState->decorations) {
        if (auto* deco = decoration.get())
            stripGlassDeco(deco->getOwner());
    }
    g_pGlobalState->decorations.clear();
    // ...then every registry window, so a lost tracking entry can never
    // strand a decoration on a mapped window either.
    if (Desktop::windowState())
        for (const auto& w : Desktop::windowState()->windows())
            stripGlassDeco(w);

    if (g_pGlobalState->renderWindowHook)
        HyprlandAPI::removeFunctionHook(PHANDLE, g_pGlobalState->renderWindowHook);
    if (g_pGlobalState->renderLayerHook) {
        HyprlandAPI::removeFunctionHook(PHANDLE, g_pGlobalState->renderLayerHook);
        g_pGlobalState->renderLayerHook = nullptr;
    }

    g_pGlobalState->layerSurfaces.clear();
    g_pGlobalState->shaderManager.destroy();
    g_pGlobalState.reset();
}
