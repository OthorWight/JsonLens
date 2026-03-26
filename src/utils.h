#ifndef JSONLENS_UTILS_H
#define JSONLENS_UTILS_H

#include <string>
#include <stddef.h>

struct Arena; // Forward declaration

size_t GetArenaRegionCount(const Arena* a);
size_t GetArenaMemoryUsage(const Arena* a);
std::string FormatTime(double ms);
std::string FormatMemory(size_t bytes);
std::string ShowOpenFileDialog(const std::string& default_dir = "");
std::string ShowSaveFileDialog();

#endif // JSONLENS_UTILS_H