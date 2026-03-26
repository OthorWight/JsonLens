#include "utils.h"
#include "arena_json.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif

size_t GetArenaRegionCount(const Arena* a) {
    size_t count = 0;
    ArenaRegion* curr = a->begin;
    while (curr) {
        count++;
        curr = curr->next;
    }
    return count;
}

size_t GetArenaMemoryUsage(const Arena* a) {
    size_t total = 0;
    ArenaRegion* curr = a->begin;
    while (curr) {
        total += curr->capacity + sizeof(ArenaRegion);
        curr = curr->next;
    }
    return total;
}

std::string FormatTime(double ms) {
    char buf[64];
    if (ms >= 1000.0) {
        snprintf(buf, sizeof(buf), "%.2f s", ms / 1000.0);
    } else if (ms >= 1.0) {
        snprintf(buf, sizeof(buf), "%.2f ms", ms);
    } else if (ms > 0.0) {
        snprintf(buf, sizeof(buf), "%.2f us", ms * 1000.0);
    } else {
        snprintf(buf, sizeof(buf), "0 ms");
    }
    return std::string(buf);
}

std::string FormatMemory(size_t bytes) {
    char buf[64];
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        snprintf(buf, sizeof(buf), "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ULL * 1024ULL) {
        snprintf(buf, sizeof(buf), "%.2f MB", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024ULL) {
        snprintf(buf, sizeof(buf), "%.2f KB", (double)bytes / 1024.0);
    } else {
        snprintf(buf, sizeof(buf), "%zu B", bytes);
    }
    return std::string(buf);
}

std::string ShowOpenFileDialog(const std::string& default_dir) {
#ifdef _WIN32
    char filename[MAX_PATH] = {0};
    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFilter = "JSON Files\0*.json\0All Files\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrDefExt = "json";
    if (!default_dir.empty()) ofn.lpstrInitialDir = default_dir.c_str();
    if (GetOpenFileNameA(&ofn)) return std::string(filename);
#else
    char buffer[1024];
    std::string result = "";
#ifdef __APPLE__
    std::string cmd = "osascript -e 'POSIX path of (choose file";
    if (!default_dir.empty()) cmd += " default location \"" + default_dir + "\"";
    cmd += ")' 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
#else
    std::string cmd = "zenity --file-selection --title=\"Open JSON\" --file-filter=\"*.json\"";
    if (!default_dir.empty()) cmd += " --filename=\"" + default_dir + "/\"";
    cmd += " 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) return "";
    if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result = buffer;
        if (!result.empty() && result.back() == '\n') result.pop_back();
    }
    pclose(pipe);
    return result;
#endif
    return "";
}

std::string ShowSaveFileDialog() {
#ifdef _WIN32
    char filename[MAX_PATH] = {0};
    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFilter = "JSON Files\0*.json\0All Files\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
    ofn.lpstrDefExt = "json";
    if (GetSaveFileNameA(&ofn)) return std::string(filename);
#else
    char buffer[1024];
    std::string result = "";
#ifdef __APPLE__
    FILE* pipe = popen("osascript -e 'POSIX path of (choose file name)' 2>/dev/null", "r");
#else
    FILE* pipe = popen("zenity --file-selection --save --confirm-overwrite --title=\"Save JSON\" --file-filter=\"*.json\" 2>/dev/null", "r");
#endif
    if (!pipe) return "";
    if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result = buffer;
        if (!result.empty() && result.back() == '\n') result.pop_back();
    }
    pclose(pipe);
    return result;
#endif
    return "";
}