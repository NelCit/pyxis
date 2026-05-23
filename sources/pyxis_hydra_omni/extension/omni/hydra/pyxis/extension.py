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
        # Smoke-test marker (survives default log filtering).
        print(
            f"PYXIS_C2 registered={plugin_dir} type_found={bool(plugin_type)}",
            flush=True,
        )

    def on_shutdown(self) -> None:
        # USD has no unregister-plugins API; the delegate simply stops being
        # selectable when the process exits. Nothing to do here.
        pass
