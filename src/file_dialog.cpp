#include "file_dialog.h"

#ifdef _WIN32

#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>

namespace {

std::string toUtf8(const wchar_t* wide) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string text(size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, text.data(), size, nullptr, nullptr);
    return text;
}

std::wstring toWide(const std::string& text) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 1) return {};
    std::wstring wide(size - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), size);
    return wide;
}

}  // namespace

bool nativeDialogsAvailable() { return true; }

std::string openFileDialog(FileKind kind) {
    wchar_t buffer[MAX_PATH * 4] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = (DWORD)(sizeof(buffer) / sizeof(buffer[0]));
    ofn.lpstrFilter = kind == FileKind::Fbx ? L"FBX mesh (*.fbx)\0*.fbx\0All files\0*.*\0"
                      : kind == FileKind::Project
                          ? L"Player project (*.mcuv)\0*.mcuv\0All files\0*.*\0"
                          : L"Images (*.png;*.jpg;*.jpeg;*.tga;*.bmp)\0*.png;*.jpg;*.jpeg;*.tga;*.bmp\0All files\0*.*\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return {};
    return toUtf8(buffer);
}

std::string pickFolderDialog() {
    std::string result;
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dialog))))
        return result;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    if (SUCCEEDED(dialog->Show(nullptr))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = toUtf8(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

namespace {

std::string saveDialog(const std::string& suggestedName, const wchar_t* filter, const wchar_t* extension) {
    wchar_t buffer[MAX_PATH * 4] = L"";
    const std::wstring suggested = toWide(suggestedName);
    if (suggested.size() < sizeof(buffer) / sizeof(buffer[0])) wcscpy_s(buffer, suggested.c_str());
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = (DWORD)(sizeof(buffer) / sizeof(buffer[0]));
    ofn.lpstrFilter = filter;
    ofn.lpstrDefExt = extension;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return {};
    return toUtf8(buffer);
}

}  // namespace

std::string saveProjectDialog(const std::string& suggestedName) {
    return saveDialog(suggestedName, L"Player project (*.mcuv)\0*.mcuv\0", L"mcuv");
}

std::string savePngDialog(const std::string& suggestedName) {
    return saveDialog(suggestedName, L"PNG image (*.png)\0*.png\0", L"png");
}

#else

bool nativeDialogsAvailable() { return false; }
std::string openFileDialog(FileKind) { return {}; }
std::string pickFolderDialog() { return {}; }
// Without a dialog the PNG goes to the working folder under the suggested name.
std::string savePngDialog(const std::string& suggestedName) { return suggestedName; }
std::string saveProjectDialog(const std::string& suggestedName) { return suggestedName; }

#endif
