# omni.hydra.pyxis — viewport helper (RFC 0006).
#
# The native plugin (omni.hydra.pyxis.plugin.dll) registers the Pyxis
# HdRendererPlugin so "Pyxis" appears in the Render menu under the 'pxr' engine
# (NVIDIA's omni.hydra.pxr, which hosts any registered USD HdRendererPlugin —
# the same path Aurora/V-Ray/Octane/Redshift/Cycles and Synopsys/ANSYS
# AVxcelerate all use; no third-party native viewport engine exists, the SDK's
# IHydraEngineFactory is forward-declared only).
#
#   THE PROBLEM: the closed omni.hydra.pxr engine ALWAYS boots on Pixar Storm
#   (hardcoded — it ignores plugInfo priority AND /pxr/rendermode at boot; removing
#   Storm yields "Could not find HdStormRendererPlugin render-delegate" + a black
#   viewport). It only adopts another delegate via an explicit set_hd_engine() on
#   the already-live engine. Kit's render menu sets /pxr/rendermode then BOOTS the
#   engine, so selecting Pyxis straight from another engine boots Storm and stays
#   there. Synopsys/ANSYS AVxcelerate (D:/AssetPreparation_Editor) documents the
#   exact same thing: "two activations are required — the first boots Pixar Storm
#   (via the .kit), the second activates the delegate via set_hd_engine()".
#
#   THE FIX (this module = our RendererActivator): when 'pxr' becomes the active
#   engine (or a stage's assets finish loading while pxr is active), do the SECOND
#   activation once — viewport.set_hd_engine("pxr", <selected delegate>). Notes:
#     * It MUST go through set_hd_engine() (the official viewport API). A raw
#       /pxr/rendermode settings write does NOT switch the live viewport.
#     * SINGLE call, fired at most ONCE per pxr session (guarded). The earlier
#       Storm->Pyxis *toggle* needlessly rebooted the Pyxis engine on every select
#       (two `PyxisEngine: initialised`); a single guarded activation does not.
#     * Selecting Storm needs no activation (Storm is the boot delegate).
#
#   FLASH: with renderer.active = "pxr" + content.emptyStageOnStart = true, Storm
#   boots on the EMPTY stage (nothing visible) and we activate Pyxis before any
#   real content loads — so the user never sees Storm render a scene. This is how
#   AVxcelerate hides it.
#
# Event-driven (no continuous polling). Guarded so a bare kit.exe (no viewport
# stack) still loads the extension for plugin registration.

import asyncio
import os

import carb
import carb.settings
import omni.ext
import omni.kit.app
import omni.usd

_STORM_PLUGIN = "HdStormRendererPlugin"
_PYXIS_PLUGIN = "HdPyxisRendererPlugin"
_RENDERER_ACTIVE = "/renderer/active"
_PXR_RENDERMODE = "/pxr/rendermode"

# Persistent-engine render-settings (RFC 0006 "Persistent engine across pxr
# renderer switches"). The delegate reads these via HdRenderDelegate::
# GetRenderSetting, and now ADVERTISES them via GetRenderSettingDescriptors() so
# the pxr engine bridges carb <-> Get/SetRenderSetting using those descriptors.
# The carb path that maps to a delegate render-setting token T is the convention
# Synopsys/ANSYS AVxcelerate uses (settings_definitions.format_path: replace ":"
# with "/", prefix /persistent/app/hydra/delegates/<PLUGIN_ID>/settings/, suffix
# /value):
#   /persistent/app/hydra/delegates/<PLUGIN_ID>/settings/<T-colon-as-slash>/value
# We ALSO stamp a /pxr/<PLUGIN_ID>/settings/<slashed>/value root as a secondary
# fallback (the exact carb->delegate propagation path is not 100% confirmed
# without a live Kit run). Whichever reaches GetRenderSetting wins; the other is
# a harmless no-op. LIVE-TEST: PYXIS_OMNI_DBG prints the delegate-side persist +
# stageToken decisions.
_DELEGATE_SETTINGS_ROOTS = (
    f"/persistent/app/hydra/delegates/{_PYXIS_PLUGIN}/settings",
    f"/pxr/{_PYXIS_PLUGIN}/settings",
)
_PERSIST_ENGINE_TOKEN = "pyxis:persistEngine"
_STAGE_TOKEN = "pyxis:stageToken"


def _delegate_setting_keys(token: str):
    """Carb keys for a delegate render-setting token, AVxcelerate-style:
    <root>/<token-with-colon-as-slash>/value, for every settings root."""
    slashed = token.replace(":", "/")
    return [f"{root}/{slashed}/value" for root in _DELEGATE_SETTINGS_ROOTS]

# The delegate the pxr engine hardcodes as its boot delegate. Selecting it needs
# no second activation; selecting anything else does.
_PXR_BOOT_DELEGATE = _STORM_PLUGIN


class _PyxisViewportExt(omni.ext.IExt):
    def on_startup(self, ext_id: str) -> None:  # noqa: D401
        self._running = True
        self._settings = carb.settings.get_settings()
        self._activated = False  # already did the 2nd activation for this pxr session
        self._task = None
        self._active_sub = self._settings.subscribe_to_node_change_events(
            _RENDERER_ACTIVE, self._on_active_changed
        )
        # AVxcelerate-style trigger: re-activate when a stage's assets finish
        # loading while pxr is active (covers the empty-start -> open-scene flow).
        try:
            self._stage_sub = (
                omni.usd.get_context()
                .get_stage_event_stream()
                .create_subscription_to_pop(self._on_stage_event, name="omni.hydra.pyxis.activator")
            )
        except Exception:  # noqa: BLE001
            self._stage_sub = None
        carb.log_info("[omni.hydra.pyxis] activator armed (/renderer/active + ASSETS_LOADED)")
        # Default the persist toggle ON if unset (the delegate reads it; absent ->
        # default ON anyway, but stamping a value makes the toggle inspectable).
        self._init_persist_default()
        # Cover renderer.active = "pxr" at startup (Storm boots on the empty stage).
        self._startup_task = asyncio.ensure_future(self._activate_after_settle())
        # Pre-warm RTX in the background on the empty startup stage (like Storm
        # boots on pxr) so selecting RTX later is fast + quiet instead of a long
        # async-init with the 'unable to find suitable engine' retry spam.
        self._rtx_preload_task = asyncio.ensure_future(self._preload_rtx())
        # Add the "Pyxis" section to the Render Settings window (alongside RTX).
        self._render_settings_registered = False
        self._register_render_settings()

    # --- pre-warm RTX (RFC 0006) ------------------------------------------

    async def _preload_rtx(self) -> None:
        """ATTACH/create the RTX hydra engine on the context at startup so its
        pipeline warms in the background. Does NOT change the active viewport
        renderer (the activator keeps Pyxis); RTX just becomes ready so a later
        switch to it is fast. Best-effort — guarded for a bare kit.exe."""
        app = omni.kit.app.get_app()
        for _ in range(180):  # let the renderer stack + USD context settle first
            if not self._running:
                return
            await app.next_update_async()
        if not self._running:
            return
        try:
            ctx = omni.usd.get_context()
            try:
                attached = list(ctx.get_attached_hydra_engine_names())
            except Exception:  # noqa: BLE001
                attached = []
            if "rtx" not in attached:
                omni.usd.create_hydra_engine("rtx", ctx)
                carb.log_info("[omni.hydra.pyxis] pre-warmed RTX hydra engine at startup")
        except Exception as exc:  # noqa: BLE001
            carb.log_warn(f"[omni.hydra.pyxis] RTX preload failed: {exc}")

    # --- Render Settings window panel (RFC 0006) --------------------------

    def _register_render_settings(self) -> None:
        """Register the "Pyxis" Render Settings section. Mirrors AVxcelerate's
        avx.ap.lpe.settings extension.py on_startup (register_renderer +
        register_stack + build_ui). Guarded so a bare kit.exe (no
        omni.rtx.window.settings) still loads the extension."""
        try:
            from omni.rtx.window.settings import RendererSettingsFactory

            from .render_settings import PyxisSettingsStack

            RendererSettingsFactory.register_renderer(_PYXIS_PLUGIN, ["Pyxis"])
            RendererSettingsFactory.register_stack("Pyxis", PyxisSettingsStack)
            RendererSettingsFactory.build_ui()
            self._render_settings_registered = True
            carb.log_info("[omni.hydra.pyxis] registered Pyxis render-settings panel")
        except Exception as exc:  # noqa: BLE001
            carb.log_warn(f"[omni.hydra.pyxis] render-settings panel register failed: {exc}")

    def _unregister_render_settings(self) -> None:
        """Mirror AVxcelerate's on_shutdown: unregister_renderer +
        unregister_stack + build_ui (avx.ap.lpe.settings/extension.py:70-72)."""
        if not getattr(self, "_render_settings_registered", False):
            return
        try:
            from omni.rtx.window.settings import RendererSettingsFactory

            RendererSettingsFactory.unregister_renderer(_PYXIS_PLUGIN)
            RendererSettingsFactory.unregister_stack("Pyxis")
            RendererSettingsFactory.build_ui()
        except Exception as exc:  # noqa: BLE001
            carb.log_warn(f"[omni.hydra.pyxis] render-settings panel unregister failed: {exc}")
        finally:
            self._render_settings_registered = False

    # --- persistent-engine render-settings (RFC 0006) ---------------------

    def _init_persist_default(self) -> None:
        """Stamp pyxis:persistEngine = true at startup if no value exists yet.
        Keys are the AVxcelerate form:
        /persistent/app/hydra/delegates/HdPyxisRendererPlugin/settings/pyxis/persistEngine/value
        (+ the /pxr/... fallback)."""
        for key in _delegate_setting_keys(_PERSIST_ENGINE_TOKEN):
            try:
                if self._settings.get(key) is None:
                    self._settings.set(key, True)
            except Exception as exc:  # noqa: BLE001
                carb.log_warn(f"[omni.hydra.pyxis] persist-default set failed ({key}): {exc}")

    def _stamp_stage_token(self) -> None:
        """Stamp the current stage identity so the delegate can detect a stage
        change and reset the persisted engine (best-effort)."""
        try:
            context = omni.usd.get_context()
            token = ""
            try:
                token = context.get_stage_url() or ""
            except Exception:  # noqa: BLE001
                token = ""
            if not token:
                try:
                    token = str(context.get_stage_id())
                except Exception:  # noqa: BLE001
                    token = ""
            if not token:
                return
            # Keys: .../settings/pyxis/stageToken/value (+ the /pxr/... fallback).
            for key in _delegate_setting_keys(_STAGE_TOKEN):
                self._settings.set(key, token)
        except Exception as exc:  # noqa: BLE001
            carb.log_warn(f"[omni.hydra.pyxis] stage-token stamp failed: {exc}")

    # --- triggers ---------------------------------------------------------

    def _on_active_changed(self, item, event_type) -> None:  # noqa: ANN001
        try:
            if str(self._settings.get(_RENDERER_ACTIVE)) == "pxr":
                self._maybe_activate()
            else:
                # Left pxr (e.g. switched to RTX) — re-arm so returning re-activates.
                self._activated = False
                # Persist OFF: free Pyxis now. Kit keeps the pxr delegate ALIVE on
                # an engine switch (so the C++ destructor never runs on its own),
                # which is why the checkbox "did nothing". Destroying the pxr engine
                # here forces the delegate's destruction -> the C++ dtor releases the
                # process-static PyxisEngine (device + VRAM). Kit recreates pxr +
                # Pyxis rebuilds when the user switches back.
                if not self._persist_enabled():
                    self._destroy_pxr_engine()
        except Exception as exc:  # noqa: BLE001
            carb.log_warn(f"[omni.hydra.pyxis] active-change handler error: {exc}")

    def _persist_enabled(self) -> bool:
        """Read the persist toggle (default ON). PYXIS_OMNI_NO_PERSIST forces OFF.
        Reads the same carb render-setting path the panel writes + the delegate
        reads (whichever root carries a value)."""
        if os.environ.get("PYXIS_OMNI_NO_PERSIST"):
            return False
        for key in _delegate_setting_keys(_PERSIST_ENGINE_TOKEN):
            value = self._settings.get(key)
            if value is not None:
                return bool(value)
        return True  # default ON

    def _destroy_pxr_engine(self) -> None:
        """Destroy the attached 'pxr' hydra engine by uid (Kit API, cf.
        omni.usd.tests.test_usd_utils): enumerate uids, match engine_type_name
        == 'pxr', destroy. Frees Pyxis's resident device/VRAM. Best-effort."""
        try:
            ctx = omni.usd.get_context()
            for uid in ctx.get_attached_hydra_engine_uids():
                try:
                    desc = ctx.get_attached_hydra_engine_description(uid)
                except Exception:  # noqa: BLE001
                    continue
                if getattr(desc, "engine_type_name", "") == "pxr":
                    carb.log_warn(
                        f"[omni.hydra.pyxis] persist OFF -> destroying pxr engine "
                        f"uid={uid} to free Pyxis"
                    )
                    omni.usd.destroy_hydra_engine(uid)
                    break
        except Exception as exc:  # noqa: BLE001
            carb.log_warn(f"[omni.hydra.pyxis] destroy pxr engine failed: {exc}")

    def _on_stage_event(self, event) -> None:  # noqa: ANN001
        try:
            # Stamp the stage identity on OPENED and ASSETS_LOADED so the delegate
            # can reset the persisted engine when a DIFFERENT scene opens (step 5).
            # OPENED fires before geometry syncs, so the token is in place before
            # the first EmitToScene of the new stage reads it.
            if event.type in (int(omni.usd.StageEventType.OPENED),
                              int(omni.usd.StageEventType.ASSETS_LOADED)):
                self._stamp_stage_token()
            if event.type == int(omni.usd.StageEventType.ASSETS_LOADED):
                if str(self._settings.get(_RENDERER_ACTIVE)) == "pxr":
                    self._maybe_activate()
        except Exception as exc:  # noqa: BLE001
            carb.log_warn(f"[omni.hydra.pyxis] stage-event handler error: {exc}")

    def _maybe_activate(self) -> None:
        if self._activated:
            return  # one activation per pxr session — never reboot on reselect
        if self._task is not None and not self._task.done():
            return  # one in flight
        self._task = asyncio.ensure_future(self._activate())

    async def _activate_after_settle(self) -> None:
        app = omni.kit.app.get_app()
        for _ in range(120):
            if not self._running:
                return
            await app.next_update_async()
        if self._running and str(self._settings.get(_RENDERER_ACTIVE)) == "pxr":
            self._maybe_activate()

    # --- the actual (second) activation -----------------------------------

    def _get_viewport(self):
        try:
            from omni.kit.viewport.utility import get_active_viewport
            return get_active_viewport()
        except Exception:  # noqa: BLE001
            return None

    async def _wait_for_live_pxr(self, app):
        """Wait until the pxr hydra engine is live; return the viewport."""
        for _ in range(300):
            if not self._running:
                return None
            viewport = self._get_viewport()
            if viewport is not None:
                try:
                    if viewport.hydra_engine == "pxr":
                        return viewport
                except Exception:  # noqa: BLE001
                    pass
            await app.next_update_async()
        return None

    async def _activate(self) -> None:
        app = omni.kit.app.get_app()
        viewport = await self._wait_for_live_pxr(app)
        if viewport is None:
            return

        desired = str(self._settings.get(_PXR_RENDERMODE) or "")
        if not desired or desired == _PXR_BOOT_DELEGATE:
            # Storm IS the boot delegate — nothing to switch to.
            self._activated = True
            return

        # Let Storm finish its boot so the switch lands as a real delegate change,
        # then do the single second activation via the official viewport API.
        for _ in range(30):
            if not self._running:
                return
            await app.next_update_async()
        if not self._running:
            return
        carb.log_warn(
            f"[omni.hydra.pyxis] second activation -> set_hd_engine('pxr', '{desired}')"
        )
        try:
            viewport.set_hd_engine("pxr", desired)
            self._activated = True
        except Exception as exc:  # noqa: BLE001
            carb.log_warn(f"[omni.hydra.pyxis] second activation failed: {exc}")

    def on_shutdown(self) -> None:
        self._running = False
        self._unregister_render_settings()
        if getattr(self, "_active_sub", None) is not None:
            self._settings.unsubscribe_to_change_events(self._active_sub)
            self._active_sub = None
        self._stage_sub = None
        for attr in ("_startup_task", "_task", "_rtx_preload_task"):
            task = getattr(self, attr, None)
            if task is not None and not task.done():
                task.cancel()
            setattr(self, attr, None)
