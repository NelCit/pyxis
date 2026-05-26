# omni.hydra.pyxis — Render Settings WINDOW panel (RFC 0006).
#
# Adds a "Pyxis" section to Kit's Render Settings window (alongside RTX),
# exposing the persistent-engine toggle. Mirrors the proven Synopsys/ANSYS
# AVxcelerate idiom at D:/AssetPreparation_Editor/source/extensions/
# avx.ap.lpe.settings (lpe_settings_stack.py: LPESettingsStack +
# AmbientOcclusionSettingsFrame).
#
# The toggle is bound to the carb path that the pxr engine bridges to the
# delegate's GetRenderSetting(TfToken("pyxis:persistEngine")):
#   /persistent/app/hydra/delegates/<PLUGIN_ID>/settings/<token-colon-as-slash>/value
# (AVxcelerate's settings_definitions.format_path: s.replace(":", "/") prefixed
# with /persistent/app/hydra/delegates/<PLUGIN_ID>/settings/ + /value.)
#
# Module-level imports of omni.rtx.window.settings are guarded so a bare kit.exe
# without that extension still imports this package (registration just no-ops).

import omni.ui as ui

try:
    from omni.kit.widget.settings import SettingType
    from omni.rtx.window.settings.rtx_settings_stack import RTXSettingsStack
    from omni.rtx.window.settings.settings_collection_frame import SettingsCollectionFrame

    _RTX_SETTINGS_AVAILABLE = True
except Exception:  # noqa: BLE001
    # Bare kit.exe (no omni.rtx.window.settings): keep the package importable.
    SettingType = None  # type: ignore[assignment]
    RTXSettingsStack = object  # type: ignore[assignment,misc]
    SettingsCollectionFrame = object  # type: ignore[assignment,misc]
    _RTX_SETTINGS_AVAILABLE = False

# Carb path -> delegate GetRenderSetting(TfToken("pyxis:persistEngine")).
_PERSIST_PATH = "/persistent/app/hydra/delegates/HdPyxisRendererPlugin/settings/pyxis/persistEngine/value"

_PERSIST_TOOLTIP = (
    "Persist Pyxis's engine (its Vulkan device + texture cache + scene) when you "
    "switch to another renderer, so switching back is instant (no texture/geometry "
    "re-upload). Costs idle VRAM: the full scene stays resident on Pyxis's own "
    "device even while another renderer is active, so turn this OFF for very large "
    "scenes. Applies on the next renderer switch."
)


class PyxisSettingsFrame(SettingsCollectionFrame):
    def _build_ui(self):
        self._add_setting(
            setting_type=SettingType.BOOL,
            path=_PERSIST_PATH,
            name="Keep renderer resident on switch",
            tooltip=_PERSIST_TOOLTIP,
            cleartext_path="Keep renderer resident on switch",
            has_reset=True,
        )


class PyxisSettingsStack(RTXSettingsStack):
    def __init__(self) -> None:
        self._stack = ui.VStack(spacing=10, identifier=__class__.__name__)
        with self._stack:
            PyxisSettingsFrame("Pyxis", parent=self, collapsed=False)
            ui.Spacer()

    def destroy(self):
        super().destroy()
