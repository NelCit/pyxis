# Minimal Kit capture: open scene, warm up the (startup-default) Pyxis viewport,
# capture ONE PNG. No engine switching — fast (<40s) for the headless-vs-Kit
# color-parity check. Env: PYXIS_HEADLESS_SCENE, PYXIS_HEADLESS_OUT,
# PYXIS_HEADLESS_CAMERA, PYXIS_HEADLESS_WARMUP.
import asyncio
import os

import carb
import omni.kit.app
import omni.usd

OUT = os.environ.get("PYXIS_HEADLESS_OUT", os.path.join(os.environ.get("TEMP", "."), "pyxis_cap"))
SCENE = os.environ.get("PYXIS_HEADLESS_SCENE", "")
CAMERA = os.environ.get("PYXIS_HEADLESS_CAMERA", "/World/Cameras/CamLobbyWide")
WARMUP = int(os.environ.get("PYXIS_HEADLESS_WARMUP", "150"))


def _out(m):
    print("[CAP] " + m, flush=True)
    carb.log_warn("[CAP] " + m)


async def _pump(app, n):
    for _ in range(n):
        await app.next_update_async()


async def run():
    app = omni.kit.app.get_app()
    os.makedirs(OUT, exist_ok=True)
    await _pump(app, 60)
    ctx = omni.usd.get_context()
    if SCENE:
        ok, err = await ctx.open_stage_async(SCENE)
        _out(f"open_stage ok={ok} err={err}")
        await _pump(app, 120)
    from omni.kit.viewport.utility import capture_viewport_to_file, get_active_viewport
    vp = get_active_viewport()
    _out(f"viewport={vp} engine={getattr(vp,'hydra_engine',None)} mode={getattr(vp,'render_mode',None)}")
    try:
        vp.camera_path = CAMERA
    except Exception as exc:  # noqa: BLE001
        _out(f"camera set failed: {exc}")
    await _pump(app, WARMUP)
    path = os.path.join(OUT, "render_pyxis.png")
    cap = capture_viewport_to_file(vp, path)
    # capture is async-ish; pump until the file lands.
    for _ in range(120):
        await app.next_update_async()
        if os.path.exists(path) and os.path.getsize(path) > 0:
            break
    _out(f"capture -> exists={os.path.exists(path)} size={os.path.getsize(path) if os.path.exists(path) else 0}")
    app.post_quit(0)


asyncio.ensure_future(run())
