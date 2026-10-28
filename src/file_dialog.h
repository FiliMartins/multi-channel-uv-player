#pragma once

#include <string>

enum class FileKind { Fbx, Image, Project };

// Native Windows dialogs. Each returns a UTF-8 path, or an empty string when the
// user cancels. On other platforms (used only for development) they return empty
// and the UI offers a path field instead.
bool nativeDialogsAvailable();
std::string openFileDialog(FileKind kind);
std::string pickFolderDialog();
// Asks where to save a PNG, starting from `suggestedName`.
std::string savePngDialog(const std::string& suggestedName);
// Asks where to save a project (.mcuv), starting from `suggestedName`.
std::string saveProjectDialog(const std::string& suggestedName);
