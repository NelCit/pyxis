// Pyxis app — shared Windows save-file picker implementation.
//
// Single COM-dialog implementation reused by the EditorPanel AOV
// save flow + the Performance panel profile-JSON save flow.

#include "SaveFilePicker.h"

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #include <objbase.h>
    #include <shobjidl.h>
#endif

namespace pyxis::app {

#if defined(_WIN32)

std::string SaveFilePickerDialog(const SaveFilePickerSpec& spec) noexcept {
  // STA on the calling thread for the duration of the call. Returns
  // RPC_E_CHANGED_MODE if another component already initialised COM
  // here with a different model — fine, the dialog still works; we
  // skip the matching CoUninitialize so we don't undo their state.
  const HRESULT initResult =
      CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  const bool weInitedCom = SUCCEEDED(initResult);

  IFileSaveDialog* dialog = nullptr;
  HRESULT comResult = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_ALL,
                                       IID_IFileSaveDialog,
                                       reinterpret_cast<void**>(&dialog));
  if (FAILED(comResult) || !dialog)
  {
    if (weInitedCom)
      CoUninitialize();
    return {};
  }

  // Filter spec — only one entry is set; the COM API takes a C-array
  // pointer + count so we wrap a single COMDLG_FILTERSPEC literal.
  // Lifetime: the label/glob pointers must outlive the SetFileTypes
  // call but COM internally copies before Show(), so stack lifetime
  // is safe.
  const std::wstring labelOwned{spec.filterLabel};
  const std::wstring globOwned{spec.filterGlob};
  const COMDLG_FILTERSPEC filterSpec[] = {{labelOwned.c_str(), globOwned.c_str()}};
  dialog->SetFileTypes(static_cast<UINT>(std::size(filterSpec)), filterSpec);

  if (!spec.title.empty())
  {
    const std::wstring titleOwned{spec.title};
    dialog->SetTitle(titleOwned.c_str());
  }
  if (!spec.defaultExtension.empty())
  {
    const std::wstring extOwned{spec.defaultExtension};
    dialog->SetDefaultExtension(extOwned.c_str());
  }
  if (!spec.suggestedFileName.empty())
  {
    const std::wstring nameOwned{spec.suggestedFileName};
    dialog->SetFileName(nameOwned.c_str());
  }

  comResult = dialog->Show(nullptr);
  std::string result;
  if (SUCCEEDED(comResult))
  {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item)) && item)
    {
      PWSTR widePath = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &widePath)) && widePath)
      {
        const int byteCount =
            WideCharToMultiByte(CP_UTF8, 0, widePath, -1, nullptr, 0, nullptr, nullptr);
        if (byteCount > 1)
        {
          result.resize(static_cast<std::size_t>(byteCount - 1));
          WideCharToMultiByte(CP_UTF8, 0, widePath, -1, result.data(), byteCount, nullptr,
                              nullptr);
        }
        CoTaskMemFree(widePath);
      }
      item->Release();
    }
  }
  dialog->Release();
  if (weInitedCom)
    CoUninitialize();
  return result;
}

#else  // !_WIN32

std::string SaveFilePickerDialog(const SaveFilePickerSpec& /*spec*/) noexcept {
  return {};
}

#endif

}  // namespace pyxis::app
