"""RFC 0004 — registers the prebuilt Pyxis Hydra delegate with USD's plugin
registry on extension startup, so "Pyxis" appears in the Kit viewport's
renderer (Hydra engine) list.

The native delegate (pyxis_hydra_omni.dll + resources/plugInfo.json) is built
out-of-tree by _tools/omniverse/build.ps1 and staged into <ext>/bin/. We do not
build it inside Kit's premake; we only point USD's PlugRegistry at the staged
plugInfo so Hydra discovers HdPyxisOmniRendererPlugin.
"""

import os

import carb
import omni.ext
from pxr import Plug


class PyxisHydraExtension(omni.ext.IExt):
    def on_startup(self, ext_id: str) -> None:
        manager = omni.kit.app.get_app().get_extension_manager()
        ext_path = manager.get_extension_path(ext_id)
        # plugInfo.json is staged under <ext>/bin/resources (see build.ps1); the
        # plugin's Root=".." in plugInfo resolves LibraryPath to <ext>/bin/<dll>.
        plugin_dir = os.path.join(ext_path, "bin", "resources")
        if not os.path.isfile(os.path.join(plugin_dir, "plugInfo.json")):
            carb.log_warn(
                f"[omni.hydra.pyxis] no plugInfo.json at {plugin_dir}; "
                "run _tools/omniverse/build.ps1 to build + stage the delegate."
            )
            return
        Plug.Registry().RegisterPlugins(plugin_dir)
        # Confirm the delegate type is now discoverable (proves plugInfo ingested).
        plugin_type = Plug.Registry.FindTypeByName("HdPyxisOmniRendererPlugin")
        carb.log_warn(
            f"[omni.hydra.pyxis] Registered Pyxis Hydra delegate from {plugin_dir}; "
            f"type discoverable = {bool(plugin_type)}"
        )

        # RFC 0004 (ISimpleEngine test): enumerate HdRendererPlugin types the way
        # Kit's renderer menu does (hd_renderer_plugins.py: Tf.Type("HdRendererPlugin")
        # .GetAllDerivedTypes()). If "HdPyxisOmniRendererPlugin" appears, Kit's
        # renderer system + the usdrt SimpleEngine (setRendererPlugin) can target it.
        try:
            from pxr import Tf

            base = Tf.Type.FindByName("HdRendererPlugin")
            names = [t.typeName for t in base.GetAllDerivedTypes()] if base else []
            pyxis_seen = any("Pyxis" in n for n in names)
            carb.log_warn(
                f"[omni.hydra.pyxis] Kit enumerates {len(names)} HdRendererPlugin types: "
                f"{names}; Pyxis enumerable = {pyxis_seen}"
            )
            print(
                f"PYXIS_ENUM count={len(names)} pyxis={pyxis_seen} plugins={names}",
                flush=True,
            )
        except Exception as exc:  # noqa: BLE001 - diagnostic probe
            carb.log_warn(f"[omni.hydra.pyxis] HdRendererPlugin enumeration probe failed: {exc}")

    def on_shutdown(self) -> None:
        # USD has no unregister-plugins API; the delegate simply stops being
        # selectable when the process exits. Nothing to do here.
        pass
