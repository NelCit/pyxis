# RFC 0006 — headless Kit automation to test/iterate the Pyxis viewport delegate
# WITHOUT a human in the loop. Run via:
#
#   kit.exe <pyxis.editor.kit> --no-window --exec _tools/omniverse/headless_render_check.py
#
# (env: PYXIS_HEADLESS_SCENE = abs path to a .usd, PYXIS_HEADLESS_OUT = output dir)
#
# It opens the scene, then for each renderer (rtx / pxr+Storm / pxr+Pyxis):
#   - switches the viewport's hydra engine + render-mode (the Render-menu action)
#   - warms up N frames
#   - captures the viewport to a PNG
#   - prints [HEADLESS] status lines (engine actually active, errors, capture ok)
# so the parent process can read the PNGs + log and iterate. Quits when done.

import asyncio
import os

import carb
import carb.settings
import omni.kit.app
import omni.usd

OUT_DIR = os.environ.get("PYXIS_HEADLESS_OUT", os.path.join(os.environ.get("TEMP", "."), "pyxis_headless"))
SCENE = os.environ.get("PYXIS_HEADLESS_SCENE", "")
CAMERA = os.environ.get("PYXIS_HEADLESS_CAMERA", "/World/Cameras/CamLobbyWide")
WARMUP = int(os.environ.get("PYXIS_HEADLESS_WARMUP", "180"))


def _out(msg: str) -> None:
    # Print to stdout (captured by the parent) AND the Kit log.
    print("[HEADLESS] " + msg, flush=True)
    carb.log_warn("[HEADLESS] " + msg)


async def _pump(app, n: int) -> None:
    for _ in range(n):
        await app.next_update_async()


async def run() -> None:
    app = omni.kit.app.get_app()
    os.makedirs(OUT_DIR, exist_ok=True)
    _out(f"start out_dir={OUT_DIR} scene={SCENE} camera={CAMERA} warmup={WARMUP}")

    # Let extensions finish starting up.
    await _pump(app, 60)

    ctx = omni.usd.get_context()
    if SCENE:
        ok, err = await ctx.open_stage_async(SCENE)
        _out(f"open_stage ok={ok} err={err}")
        await _pump(app, 120)

    try:
        from omni.kit.viewport.utility import capture_viewport_to_file, get_active_viewport
    except Exception as exc:  # noqa: BLE001
        _out(f"FATAL import viewport.utility: {exc}")
        app.post_quit(1)
        return

    viewport = get_active_viewport()
    _out(f"viewport={viewport}")
    if viewport is None:
        _out("FATAL no active viewport")
        app.post_quit(1)
        return

    # Log the STARTUP default (before any switch) — what the viewport auto-attached
    # to. This is what the user sees on launch; if it's not Pyxis, the .kit's
    # /pxr/rendermode isn't being applied at first attach.
    settings = carb.settings.get_settings()
    _out(f"STARTUP default: hydra_engine={viewport.hydra_engine} render_mode={viewport.render_mode} "
         f"/renderer/active={settings.get('/renderer/active')} /pxr/rendermode={settings.get('/pxr/rendermode')}")

    try:
        viewport.camera_path = CAMERA
    except Exception as exc:  # noqa: BLE001
        _out(f"camera set failed: {exc}")

    configs = [
        ("rtx", "rtx", None),
        ("storm", "pxr", "HdStormRendererPlugin"),
        ("pyxis", "pxr", "HdPyxisRendererPlugin"),
    ]
    # PYXIS_HEADLESS_ONLY="pyxis" runs only that renderer (tests whether Pyxis is
    # black on its own vs only after a Storm->Pyxis switch — the shared-pxr-engine
    # hypothesis). Comma-separated, order honored.
    only = os.environ.get("PYXIS_HEADLESS_ONLY", "").strip()
    if only == "default":
        # Don't switch anything: capture whatever the startup hook left active
        # (verifies the extension's default-Pyxis startup module).
        for _ in range(WARMUP):
            await app.next_update_async()
        _out(f"default: active engine={viewport.hydra_engine} render_mode={viewport.render_mode}")
        path = os.path.join(OUT_DIR, "render_default.png")
        if os.path.exists(path):
            os.remove(path)
        capture_viewport_to_file(viewport, file_path=path)
        for _ in range(30):
            await _pump(app, 10)
            if os.path.exists(path) and os.path.getsize(path) > 0:
                break
        _out(f"default: capture -> exists={os.path.exists(path)} "
             f"size={os.path.getsize(path) if os.path.exists(path) else 0}")
        _out("DONE")
        app.post_quit(0)
        return
    if only:
        wanted = [s.strip() for s in only.split(",") if s.strip()]
        by_name = {c[0]: c for c in configs}
        configs = [by_name[w] for w in wanted if w in by_name]
    for name, engine, mode in configs:
        _out(f"--- {name}: switching to engine={engine} mode={mode} ---")
        # PYXIS_RTX_MODE overrides the rtx render-mode (e.g. "RealTime" raster vs
        # the default "RealTimePathTracing"). Lets us test whether RTX renders at
        # all in a non-accumulating mode.
        rtx_mode = os.environ.get("PYXIS_RTX_MODE", "").strip()
        if name == "rtx" and rtx_mode:
            mode = rtx_mode
        try:
            # Mirror the real Render-menu apply() (renderer_menu_container.py):
            # set /renderer/active, then the engine's rendermode, then the engine.
            # This is what fires omni.hydra.pyxis's /renderer/active observer, so
            # the headless test faithfully reproduces selecting a renderer in the UI.
            settings.set_string("/renderer/active", engine)
            if mode:
                rm_path = f"/{engine}/rendermode" if engine != "iray" else "/rtx/iray/rendermode"
                settings.set_string(rm_path, mode)
                viewport.set_hd_engine(engine, mode)
            else:
                viewport.set_hd_engine(engine)
        except Exception as exc:  # noqa: BLE001
            _out(f"{name}: set_hd_engine EXCEPTION: {exc}")
            continue

        await _pump(app, WARMUP)
        _out(f"{name}: active engine={viewport.hydra_engine} render_mode={viewport.render_mode}")

        path = os.path.join(OUT_DIR, f"render_{name}.png")
        try:
            if os.path.exists(path):
                os.remove(path)
            capture_viewport_to_file(viewport, file_path=path)
            # Poll for the file (do NOT await the capture future — it can hang
            # headless if the engine never completes a frame). Pump in chunks.
            exists = False
            for _ in range(30):
                await _pump(app, 10)
                if os.path.exists(path) and os.path.getsize(path) > 0:
                    exists = True
                    break
            size = os.path.getsize(path) if exists else 0
            _out(f"{name}: capture -> exists={exists} size={size}")
        except Exception as exc:  # noqa: BLE001
            _out(f"{name}: capture EXCEPTION: {exc}")

    _out("DONE")
    await _pump(app, 10)
    app.post_quit(0)


asyncio.ensure_future(run())
