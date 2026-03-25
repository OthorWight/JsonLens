#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <algorithm>
#include <map>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif

// Include the custom C JSON Parser (implemented and compiled via arena_json.c)
#include "arena_json.h"

// Helper to compute memory used across all chunks in a custom Arena
size_t GetArenaMemoryUsage(const Arena* a) {
    size_t total = 0;
    ArenaRegion* curr = a->begin;
    while (curr) {
        total += curr->capacity + sizeof(ArenaRegion);
        curr = curr->next;
    }
    return total;
}

// Helper to format time dynamically
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

// Helper to format memory size dynamically
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

enum class UndoActionType {
    SetNode,
    InsertNode,
    RemoveNode,
    TextReplace
};

enum class GraphNodeType {
    Normal,
    PrevPage,
    NextPage
};

struct GraphNode {
    std::string label;
    std::vector<GraphNode*> children;
    float x = 0, y = 0;
    float drawn_width = 0;
    size_t offset = 0;
    GraphNode* parent = nullptr;
    GraphNodeType type = GraphNodeType::Normal;
    JsonValue* source_val = nullptr;
    size_t page_step = 2000;

    ~GraphNode() { for (auto child : children) delete child; }
};

struct TextReplaceDelta {
    size_t pos;
    std::string old_str;
    std::string new_str;
};

struct UndoRecord {
    UndoActionType action;
    std::vector<size_t> path;
    JsonNode saved_node;
    std::vector<TextReplaceDelta> text_deltas;
};

JsonValue* GetParentValueFromPath(JsonValue* root, const std::vector<size_t>& path) {
    if (path.empty()) return nullptr;
    JsonValue* curr = root;
    for (size_t i = 0; i < path.size() - 1; i++) {
        size_t idx = path[i];
        if (!curr || (curr->type != JSON_OBJECT && curr->type != JSON_ARRAY)) return nullptr;
        if (idx >= curr->as.list.count) return nullptr;
        curr = &curr->as.list.items[idx].value;
    }
    return curr;
}

void JsonRemoveAtIndex(JsonValue* val, size_t index) {
    if (!val || (val->type != JSON_OBJECT && val->type != JSON_ARRAY)) return;
    if (index >= val->as.list.count) return;
    size_t items_to_move = val->as.list.count - index - 1;
    if (items_to_move > 0) memmove(&val->as.list.items[index], &val->as.list.items[index+1], items_to_move * sizeof(JsonNode));
    val->as.list.count--;
}

void JsonInsertAtIndex(Arena* a, JsonValue* parent, size_t index, const JsonNode& node) {
    if (!a || !parent || (parent->type != JSON_OBJECT && parent->type != JSON_ARRAY)) return;
    if (index > parent->as.list.count) index = parent->as.list.count;
    size_t old_count = parent->as.list.count;
    JsonNode* new_items = (JsonNode*)arena_alloc(a, (old_count + 1) * sizeof(JsonNode));
    if (index > 0) memcpy(new_items, parent->as.list.items, index * sizeof(JsonNode));
    if (index < old_count) memcpy(new_items + index + 1, parent->as.list.items + index, (old_count - index) * sizeof(JsonNode));
    new_items[index] = node;
    parent->as.list.items = new_items;
    parent->as.list.count = old_count + 1;
}

// Minimal cross-platform file dialog using native system calls
std::string ShowOpenFileDialog(const std::string& default_dir = "") {
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
        if (!result.empty() && result.back() == '\n') result.pop_back(); // Remove trailing newline
    }
    pclose(pipe);
    return result;
#endif
    return "";
}

// Minimal cross-platform save dialog using native system calls
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
        if (!result.empty() && result.back() == '\n') result.pop_back(); // Remove trailing newline
    }
    pclose(pipe);
    return result;
#endif
    return "";
}

struct AppSettings {
    float zoom = 1.0f;
    std::vector<std::string> recent_files;
    std::string last_folder;
    int graph_goto_target = 0; // 0 = Text View, 1 = Tree View
    bool allow_comments = true;
    bool use_tabs = false;
    int indent_size = 2;
    int pagination_size = 2000;

    std::string GetSettingsPath() {
        char* pref_path = SDL_GetPrefPath("JsonLens", "JsonLens");
        std::string path = pref_path ? std::string(pref_path) + "settings.json" : "settings.json";
        if (pref_path) SDL_free(pref_path);
        return path;
    }

    void Load() {
        std::string path = GetSettingsPath();
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return;
        fseek(f, 0, SEEK_END);
        size_t size = ftell(f);
        fseek(f, 0, SEEK_SET);
        char* data = (char*)malloc(size + 1);
        fread(data, 1, size, f);
        data[size] = '\0';
        fclose(f);

        Arena arena, scratch;
        arena_init(&arena);
        arena_init(&scratch);
        JsonError err;
        JsonValue* root = json_parse(&arena, &scratch, data, size, JSON_PARSE_ALLOW_COMMENTS, &err);
        if (root && root->type == JSON_OBJECT) {
            zoom = (float)json_get_number(root, "zoom", 1.0);
            JsonValue* recents = json_get(root, "recent_files");
            if (recents && recents->type == JSON_ARRAY) {
                for (size_t i = 0; i < recents->as.list.count; i++) {
                    if (recents->as.list.items[i].value.type == JSON_STRING) {
                        recent_files.push_back(recents->as.list.items[i].value.as.string);
                    }
                }
            }
            const char* folder = json_get_string(root, "last_folder", nullptr);
            if (folder) last_folder = folder;
            graph_goto_target = (int)json_get_number(root, "graph_goto_target", 0.0);
            allow_comments = json_get_bool(root, "allow_comments", true);
            use_tabs = json_get_bool(root, "use_tabs", false);
            indent_size = (int)json_get_number(root, "indent_size", 2.0);
            pagination_size = (int)json_get_number(root, "pagination_size", 2000.0);
        }
        arena_free(&arena);
        arena_free(&scratch);
        free(data);
    }

    void Save() {
        std::string path = GetSettingsPath();
        Arena arena;
        arena_init(&arena);
        JsonValue* root = json_create_object(&arena);
        json_add_number(&arena, root, "zoom", zoom);
        
        JsonValue* recents = json_create_array(&arena);
        for (const auto& file : recent_files) {
            json_append_string(&arena, recents, file.c_str());
        }
        json_add(&arena, root, "recent_files", recents);
        
        if (!last_folder.empty()) 
            json_add_string(&arena, root, "last_folder", last_folder.c_str());
        json_add_number(&arena, root, "graph_goto_target", (double)graph_goto_target);
        json_add_bool(&arena, root, "allow_comments", allow_comments);
        json_add_bool(&arena, root, "use_tabs", use_tabs);
        json_add_number(&arena, root, "indent_size", (double)indent_size);
        json_add_number(&arena, root, "pagination_size", (double)pagination_size);

        char* str = json_to_string(&arena, root, true, false, 4);
        if (str) {
            FILE* f = fopen(path.c_str(), "wb");
            if (f) {
                fwrite(str, 1, strlen(str), f);
                fclose(f);
            }
        }
        arena_free(&arena);
    }

    void AddRecentFile(std::string path) {
        for (auto it = recent_files.begin(); it != recent_files.end(); ++it) {
            if (*it == path) {
                recent_files.erase(it);
                break;
            }
        }
        recent_files.insert(recent_files.begin(), std::move(path));
        if (recent_files.size() > 10) recent_files.resize(10);
        Save();
    }
};

struct LargeTextFile {
    char* data = nullptr;
    size_t size = 0;
    size_t data_capacity = 0;
    std::vector<size_t> line_offsets;
    
    size_t select_start = (size_t)-1;
    size_t select_end = (size_t)-1;

    std::vector<UndoRecord> undo_stack;
    std::vector<UndoRecord> redo_stack;

    GraphNode* graph_root = nullptr;
    float graph_total_width = 0;
    float graph_total_height = 0;
    bool graph_dirty = true;
    
    std::map<JsonValue*, size_t> graph_pagination;

    bool tree_dirty = false;
    bool is_pretty = true;

    double load_time_ms = 0;
    double parse_time_ms = 0;
    double index_time_ms = 0;
    double format_time_ms = 0;
    size_t parse_memory_bytes = 0;
    
    int pagination_size = 2000;

    Arena main_arena;
    Arena scratch_arena;
    JsonValue* root_json = nullptr;
    JsonError last_err = {};

    LargeTextFile() {
        arena_init(&main_arena);
        arena_init(&scratch_arena);
    }

    ~LargeTextFile() {
        ClearHistory();
        ClearGraph();
        select_start = select_end = (size_t)-1;
        if (data) free(data);
        arena_free(&main_arena);
        arena_free(&scratch_arena);
    }

    void BuildLineOffsets() {
        line_offsets.clear();
        if (!data) return;
        line_offsets.push_back(0);
        size_t line_len = 0;
        for (size_t i = 0; i < size; i++) {
            line_len++;
            if (data[i] == '\n' || line_len >= 4096) { // Artificially wrap massive single-line minified files to prevent UI freeze
                line_offsets.push_back(i + 1);
                line_len = 0;
            }
        }
    }

    int GetLineFromOffset(size_t off) const {
        if (line_offsets.empty()) return 0;
        auto it = std::upper_bound(line_offsets.begin(), line_offsets.end(), off);
        return (int)std::distance(line_offsets.begin(), it) - 1;
    }

    void ClearHistory() {
        undo_stack.clear();
        redo_stack.clear();
    }

    void ClearAstHistory() {
        auto is_ast = [](const UndoRecord& r) { return r.action != UndoActionType::TextReplace; };
        undo_stack.erase(std::remove_if(undo_stack.begin(), undo_stack.end(), is_ast), undo_stack.end());
        redo_stack.erase(std::remove_if(redo_stack.begin(), redo_stack.end(), is_ast), redo_stack.end());
    }

    void ClearGraph() {
        if (graph_root) {
            delete graph_root;
            graph_root = nullptr;
        }
        graph_total_width = graph_total_height = 0;
    }

    void ClearTextHistory() {
        auto is_text = [](const UndoRecord& r) { return r.action == UndoActionType::TextReplace; };
        undo_stack.erase(std::remove_if(undo_stack.begin(), undo_stack.end(), is_text), undo_stack.end());
        redo_stack.erase(std::remove_if(redo_stack.begin(), redo_stack.end(), is_text), redo_stack.end());
    }

    size_t GetHistoryMemoryUsage() const {
        size_t total = 0;
        for (const auto& r : undo_stack) {
            total += sizeof(UndoRecord) + r.path.capacity() * sizeof(size_t) + r.text_deltas.capacity() * sizeof(TextReplaceDelta);
            for (const auto& d : r.text_deltas) total += d.old_str.capacity() + d.new_str.capacity();
        }
        for (const auto& r : redo_stack) {
            total += sizeof(UndoRecord) + r.path.capacity() * sizeof(size_t) + r.text_deltas.capacity() * sizeof(TextReplaceDelta);
            for (const auto& d : r.text_deltas) total += d.old_str.capacity() + d.new_str.capacity();
        }
        return total;
    }

    void Load(const char* filepath, bool allow_comments) {
        ClearHistory();
        ClearGraph();
        if (data) {
            free(data);
            data = nullptr;
        }
        tree_dirty = false;
        graph_dirty = true;
        arena_free(&main_arena);
        arena_free(&scratch_arena);
        arena_init(&main_arena);
        arena_init(&scratch_arena);
        root_json = nullptr;
        load_time_ms = parse_time_ms = index_time_ms = format_time_ms = 0;
        parse_memory_bytes = 0;
        
        Uint64 t_freq = SDL_GetPerformanceFrequency();
        Uint64 t0 = SDL_GetPerformanceCounter();
        
        FILE* f = fopen(filepath, "rb");
        if (!f) return;
        
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fseek(f, 0, SEEK_SET);
        
        data_capacity = size + 1024 * 1024; // Provide 1MB breathing room for edits
        data = (char*)malloc(data_capacity);
        fread(data, 1, size, f);
        data[size] = '\0';
        fclose(f);

        Uint64 t1 = SDL_GetPerformanceCounter();
        load_time_ms = (double)(t1 - t0) * 1000.0 / t_freq;

        // Parse the JSON using the fast Arena allocator
        Uint64 t2 = SDL_GetPerformanceCounter();
        int parse_flags = allow_comments ? JSON_PARSE_ALLOW_COMMENTS : JSON_PARSE_STRICT;
        root_json = json_parse(&main_arena, &scratch_arena, data, size, parse_flags, &last_err);
        Uint64 t3 = SDL_GetPerformanceCounter();
        parse_time_ms = (double)(t3 - t2) * 1000.0 / t_freq;
        
        Uint64 t4 = SDL_GetPerformanceCounter();
        BuildLineOffsets();
        Uint64 t5 = SDL_GetPerformanceCounter();
        index_time_ms = (double)(t5 - t4) * 1000.0 / t_freq;
        
        parse_memory_bytes = GetArenaMemoryUsage(&main_arena) + GetArenaMemoryUsage(&scratch_arena);

        printf("--- Load Stats ---\n");
        printf("File Size: %s\n", FormatMemory(size).c_str());
        printf("Load Time: %s\n", FormatTime(load_time_ms).c_str());
        printf("Parse Time: %s\n", FormatTime(parse_time_ms).c_str());
        printf("Index Time: %s\n", FormatTime(index_time_ms).c_str());
        printf("Parse Memory: %s\n", FormatMemory(parse_memory_bytes).c_str());
        printf("History Memory: %s\n", FormatMemory(GetHistoryMemoryUsage()).c_str());
        printf("------------------\n");
    }

    void RebuildTextFromTree(bool use_tabs, int indent_step) {
        if (!tree_dirty || !root_json) return;
        
        Uint64 t_freq = SDL_GetPerformanceFrequency();
        Uint64 t0 = SDL_GetPerformanceCounter();

        arena_free(&scratch_arena);
        arena_init(&scratch_arena);
        
        char* new_text = json_to_string(&scratch_arena, root_json, is_pretty, use_tabs, indent_step);
        if (new_text) {
            size_t new_len = strlen(new_text);
            if (new_len >= data_capacity) {
                data_capacity = new_len + 1024 * 1024;
                data = (char*)realloc(data, data_capacity);
            }
            strcpy(data, new_text);
            size = new_len;
            
            Uint64 t1 = SDL_GetPerformanceCounter();
            format_time_ms = (double)(t1 - t0) * 1000.0 / t_freq;

            Uint64 t2 = SDL_GetPerformanceCounter();
            BuildLineOffsets();
            Uint64 t3 = SDL_GetPerformanceCounter();
            index_time_ms = (double)(t3 - t2) * 1000.0 / t_freq;
        }
        tree_dirty = false;
        parse_memory_bytes = GetArenaMemoryUsage(&main_arena) + GetArenaMemoryUsage(&scratch_arena);

        printf("--- Format Stats ---\n");
        printf("Format Time: %s\n", FormatTime(format_time_ms).c_str());
        printf("Index Time: %s\n", FormatTime(index_time_ms).c_str());
        printf("Parse Memory: %s\n", FormatMemory(parse_memory_bytes).c_str());
        printf("History Memory: %s\n", FormatMemory(GetHistoryMemoryUsage()).c_str());
        printf("--------------------\n");
    }

    void SaveToFile(const char* filepath, bool use_tabs, int indent_step) {
        RebuildTextFromTree(use_tabs, indent_step); // Ensure the text buffer matches the current tree
        if (!data) return;
        FILE* f = fopen(filepath, "wb");
        if (!f) return;
        fwrite(data, 1, size, f);
        fclose(f);
    }

    void PushUndo(UndoActionType action, const std::vector<size_t>& path, const JsonNode& node = JsonNode{}) {
        undo_stack.push_back({ action, path, node, {} });
        redo_stack.clear();
        if (undo_stack.size() > 50000) {
            undo_stack.erase(undo_stack.begin());
        }
    }

    bool ExecuteHistoryAction(const UndoRecord& rec, std::vector<UndoRecord>& opposite_stack, bool is_undo) {
        if (!root_json && rec.action != UndoActionType::TextReplace) return false;

        if (rec.action == UndoActionType::TextReplace) {
            if (is_undo) {
                long diff = 0;
                for (const auto& d : rec.text_deltas) diff += (long)d.new_str.length() - (long)d.old_str.length();
                
                for (auto it = rec.text_deltas.rbegin(); it != rec.text_deltas.rend(); ++it) {
                    diff -= (long)it->new_str.length() - (long)it->old_str.length();
                    size_t current_pos = it->pos + diff;
                    size_t old_len = it->new_str.length();
                    size_t new_len = it->old_str.length();
                    
                    if (size - old_len + new_len >= data_capacity) {
                        data_capacity = size + new_len + 1024 * 1024;
                        data = (char*)realloc(data, data_capacity);
                    }
                    memmove(data + current_pos + new_len, data + current_pos + old_len, size - current_pos - old_len);
                    memcpy(data + current_pos, it->old_str.c_str(), new_len);
                    size = size - old_len + new_len;
                    data[size] = '\0';
                }
            } else {
                long diff = 0;
                for (const auto& d : rec.text_deltas) {
                    size_t current_pos = d.pos + diff;
                    size_t old_len = d.old_str.length();
                    size_t new_len = d.new_str.length();
                    
                    if (size - old_len + new_len >= data_capacity) {
                        data_capacity = size + new_len + 1024 * 1024;
                        data = (char*)realloc(data, data_capacity);
                    }
                    memmove(data + current_pos + new_len, data + current_pos + old_len, size - current_pos - old_len);
                    memcpy(data + current_pos, d.new_str.c_str(), new_len);
                    size = size - old_len + new_len;
                    data[size] = '\0';
                    diff += (long)new_len - (long)old_len;
                }
            }
            opposite_stack.push_back(rec);
            return true;
        }
        
        if (rec.path.empty()) {
            if (rec.action == UndoActionType::SetNode) {
                JsonNode current_root_node = JsonNode{};
                current_root_node.value = *root_json;
                opposite_stack.push_back({ UndoActionType::SetNode, rec.path, current_root_node, {} });
                *root_json = rec.saved_node.value;
                tree_dirty = true;
                graph_dirty = true;
                graph_pagination.clear();
            }
            return false;
        }

        JsonValue* parent = GetParentValueFromPath(root_json, rec.path);
        if (!parent) return false;
        size_t target_idx = rec.path.back();

        if (rec.action == UndoActionType::SetNode) {
            if (target_idx < parent->as.list.count) {
                opposite_stack.push_back({ UndoActionType::SetNode, rec.path, parent->as.list.items[target_idx], {} });
                parent->as.list.items[target_idx] = rec.saved_node;
                tree_dirty = true;
                graph_dirty = true;
                graph_pagination.clear();
            }
        }
        else if (rec.action == UndoActionType::InsertNode) {
            opposite_stack.push_back({ UndoActionType::RemoveNode, rec.path, JsonNode{}, {} });
            JsonInsertAtIndex(&main_arena, parent, target_idx, rec.saved_node);
            tree_dirty = true;
            graph_dirty = true;
            graph_pagination.clear();
        }
        else if (rec.action == UndoActionType::RemoveNode) {
            if (target_idx < parent->as.list.count) {
                opposite_stack.push_back({ UndoActionType::InsertNode, rec.path, parent->as.list.items[target_idx], {} });
                JsonRemoveAtIndex(parent, target_idx);
                tree_dirty = true;
                graph_dirty = true;
                graph_pagination.clear();
            }
        }
        return false;
    }

    void RebuildTreeFromText(bool allow_comments) {
        Uint64 t_freq = SDL_GetPerformanceFrequency();
        Uint64 t0 = SDL_GetPerformanceCounter();

        arena_free(&main_arena);
        arena_free(&scratch_arena);
        arena_init(&main_arena);
        arena_init(&scratch_arena);
        int parse_flags = allow_comments ? JSON_PARSE_ALLOW_COMMENTS : JSON_PARSE_STRICT;
        root_json = json_parse(&main_arena, &scratch_arena, data, size, parse_flags, &last_err);
        BuildLineOffsets();
        ClearGraph();
        tree_dirty = false;
        graph_dirty = true;
        
        Uint64 t1 = SDL_GetPerformanceCounter();
        parse_time_ms = (double)(t1 - t0) * 1000.0 / t_freq;
        parse_memory_bytes = GetArenaMemoryUsage(&main_arena) + GetArenaMemoryUsage(&scratch_arena);
    }

    void ReplaceCurrent(const char* search_str, const char* replace_str, const std::vector<size_t>& results, int idx) {
        if (idx < 0 || idx >= (int)results.size()) return;
        size_t match_pos = results[idx];
        size_t search_len = strlen(search_str);
        size_t replace_len = strlen(replace_str);
        
        UndoRecord rec{};
        rec.action = UndoActionType::TextReplace;
        rec.text_deltas.push_back({match_pos, search_str, replace_str});
        undo_stack.push_back(rec);
        redo_stack.clear();

        if (size - search_len + replace_len >= data_capacity) {
            data_capacity = size + replace_len + 1024 * 1024;
            data = (char*)realloc(data, data_capacity);
        }
        memmove(data + match_pos + replace_len, data + match_pos + search_len, size - match_pos - search_len);
        memcpy(data + match_pos, replace_str, replace_len);
        size = size - search_len + replace_len;
        data[size] = '\0';
    }

    void ReplaceAll(const char* search_str, const char* replace_str, const std::vector<size_t>& results) {
        if (results.empty()) return;
        size_t search_len = strlen(search_str);
        size_t replace_len = strlen(replace_str);
        
        UndoRecord rec{};
        rec.action = UndoActionType::TextReplace;
        for (size_t match_pos : results) {
            rec.text_deltas.push_back({match_pos, search_str, replace_str});
        }
        undo_stack.push_back(rec);
        redo_stack.clear();

        if (replace_len <= search_len) {
            size_t write_pos = results[0];
            size_t read_pos = results[0];
            size_t diff = search_len - replace_len;
            size_t total_diff = 0;
            for (size_t i = 0; i < results.size(); i++) {
                size_t match_pos = results[i];
                size_t chunk_size = match_pos - read_pos;
                if (chunk_size > 0 && write_pos != read_pos) memmove(data + write_pos, data + read_pos, chunk_size);
                write_pos += chunk_size;
                read_pos = match_pos + search_len;
                memcpy(data + write_pos, replace_str, replace_len);
                write_pos += replace_len;
                total_diff += diff;
            }
            if (read_pos < size) memmove(data + write_pos, data + read_pos, size - read_pos);
            size -= total_diff;
            data[size] = '\0';
        } else {
            size_t diff = replace_len - search_len;
            size_t new_size = size + results.size() * diff;
            if (new_size >= data_capacity) { data_capacity = new_size + 1024 * 1024; data = (char*)realloc(data, data_capacity); }
            size_t read_pos = size, write_pos = new_size;
            for (int i = (int)results.size() - 1; i >= 0; i--) {
                size_t match_pos = results[i];
                size_t chunk_size = read_pos - (match_pos + search_len);
                if (chunk_size > 0) { write_pos -= chunk_size; read_pos -= chunk_size; memmove(data + write_pos, data + read_pos, chunk_size); }
                write_pos -= replace_len; read_pos -= search_len; memcpy(data + write_pos, replace_str, replace_len);
            }
            size = new_size;
            data[size] = '\0';
        }
    }

    bool Undo() {
        if (undo_stack.empty()) return false;
        UndoRecord rec = undo_stack.back();
        undo_stack.pop_back();
        return ExecuteHistoryAction(rec, redo_stack, true);
    }

    bool Redo() {
        if (redo_stack.empty()) return false;
        UndoRecord rec = redo_stack.back();
        redo_stack.pop_back();
        return ExecuteHistoryAction(rec, undo_stack, false);
    }
};

static GraphNode* BuildGraphNode(LargeTextFile* doc, JsonValue* val, const std::string& key, int depth, int& node_count) {
    if (!val || node_count > 100000) return nullptr;
    node_count++;

    GraphNode* node = new GraphNode();
    node->offset = val->offset;
    node->source_val = val;

    std::string preview = "";
    if (val->type == JSON_STRING) {
        std::string s = val->as.string ? val->as.string : "";
        if (s.length() > 20) s = s.substr(0, 17) + "...";
        preview = "\"" + s + "\"";
    }
    else if (val->type == JSON_NUMBER) {
        char buf[64];
        if (val->as.number == (int64_t)val->as.number) snprintf(buf, sizeof(buf), "%lld", (long long)val->as.number);
        else snprintf(buf, sizeof(buf), "%.17g", val->as.number);
        preview = buf;
    }
    else if (val->type == JSON_BOOL) preview = val->as.boolean ? "true" : "false";
    else if (val->type == JSON_NULL) preview = "null";
    else if (val->type == JSON_OBJECT) preview = "{...}";
    else if (val->type == JSON_ARRAY) preview = "[...]";

    node->label = key.empty() ? preview : (key + ": " + preview);

    if (val->type == JSON_OBJECT || val->type == JSON_ARRAY) {
        size_t start_idx = doc->graph_pagination[val];
        if (start_idx >= val->as.list.count && val->as.list.count > 0)
            start_idx = (val->as.list.count - 1) / doc->pagination_size * doc->pagination_size;

        size_t remaining = val->as.list.count > start_idx ? val->as.list.count - start_idx : 0;
        size_t count = remaining > (size_t)doc->pagination_size ? (size_t)doc->pagination_size : remaining;

        if (start_idx > 0) {
            GraphNode* prev_node = new GraphNode();
            prev_node->label = "[< Previous Page]";
            prev_node->type = GraphNodeType::PrevPage;
            prev_node->source_val = val;
            prev_node->parent = node;
            node->children.push_back(prev_node);
        }

        size_t actual_count = 0;
        for (size_t i = start_idx; i < start_idx + count; i++) {
            if (node_count > 100000) break; // Global cap reached
            std::string child_key = (val->type == JSON_OBJECT) ? (val->as.list.items[i].key ? val->as.list.items[i].key : "") : ("[" + std::to_string(i) + "]");
            GraphNode* child = BuildGraphNode(doc, &val->as.list.items[i].value, child_key, depth + 1, node_count);
            if (child) { child->parent = node; node->children.push_back(child); actual_count++; }
        }
        
        if (start_idx + actual_count < val->as.list.count) {
            size_t step = actual_count > 0 ? actual_count : count;
            GraphNode* more = new GraphNode();
            more->label = "[Next " + std::to_string(step) + " >] (" + std::to_string(val->as.list.count - (start_idx + actual_count)) + " remaining)";
            more->type = GraphNodeType::NextPage;
            more->source_val = val;
            more->page_step = step;
            more->parent = node;
            node->children.push_back(more);
        }
    }
    return node;
}

static void LayoutGraphNode(GraphNode* node, int depth, float& current_y, float& max_x, float current_x = 40.0f) {
    node->x = current_x;
    node->drawn_width = ImGui::CalcTextSize(node->label.c_str()).x + 16.0f;
    if (node->x + node->drawn_width > max_x) max_x = node->x + node->drawn_width;

    if (node->children.empty()) {
        node->y = current_y;
        current_y += 34.0f;
    } else {
        float start_y = current_y;
        float next_x = current_x + node->drawn_width + 80.0f; // Ensure exactly 80px space for the connecting bezier lines
        for (auto child : node->children) {
            LayoutGraphNode(child, depth + 1, current_y, max_x, next_x);
        }
        node->y = (start_y + (current_y - 34.0f)) / 2.0f;
    }
}

static void DrawGraphEdges(ImDrawList* dl, GraphNode* node, ImVec2 offset, const ImRect& clip_rect) {
    ImU32 edge_col = IM_COL32(100, 100, 100, 255);
    for (auto child : node->children) {
        ImVec2 p1 = ImVec2(offset.x + node->x + node->drawn_width, offset.y + node->y);
        ImVec2 p2 = ImVec2(offset.x + child->x, offset.y + child->y);

        float min_x = std::min(p1.x, p2.x);
        float max_x = std::max(p1.x, p2.x);
        float min_y = std::min(p1.y, p2.y);
        float max_y = std::max(p1.y, p2.y);

        if (clip_rect.Overlaps(ImRect(min_x, min_y - 40.0f, max_x, max_y + 40.0f))) {
            dl->AddBezierCubic(p1, ImVec2(p1.x + 40.0f, p1.y), ImVec2(p2.x - 40.0f, p2.y), p2, edge_col, 1.5f);
        }
        DrawGraphEdges(dl, child, offset, clip_rect);
    }
}

static void DrawGraphNodes(ImDrawList* dl, GraphNode* node, ImVec2 offset, ImVec2 mouse_pos, GraphNode** out_hovered, const ImRect& clip_rect) {
    ImVec2 p_min = ImVec2(offset.x + node->x, offset.y + node->y - 12.0f);
    ImVec2 p_max = ImVec2(p_min.x + node->drawn_width, p_min.y + 24.0f);

    if (clip_rect.Overlaps(ImRect(p_min, p_max))) {
        bool hovered = mouse_pos.x >= p_min.x && mouse_pos.x <= p_max.x && mouse_pos.y >= p_min.y && mouse_pos.y <= p_max.y;
        if (hovered) *out_hovered = node;

        ImU32 bg_col = hovered ? IM_COL32(70, 70, 80, 255) : IM_COL32(40, 40, 50, 255);
        if (node->type != GraphNodeType::Normal) {
            bg_col = hovered ? IM_COL32(50, 80, 120, 255) : IM_COL32(30, 50, 80, 255);
        }
        ImU32 border_col = IM_COL32(100, 100, 120, 255);
        ImU32 text_col = IM_COL32(230, 230, 230, 255);

        dl->AddRectFilled(p_min, p_max, bg_col, 4.0f);
        dl->AddRect(p_min, p_max, border_col, 4.0f);
        
        float text_y = p_min.y + (24.0f - ImGui::GetFontSize()) * 0.5f;
        dl->AddText(ImVec2(p_min.x + 8.0f, text_y), text_col, node->label.c_str());
    }

    for (auto child : node->children) {
        DrawGraphNodes(dl, child, offset, mouse_pos, out_hovered, clip_rect);
    }
}

// Recursive UI for editing JSON trees
int DrawEditableJsonNode(LargeTextFile* doc, JsonNode* node, int node_index, std::vector<size_t>& current_path, const std::vector<JsonValue*>& focus_path, JsonValue* highlight_val, double highlight_time) {
    // Action Flags: 1 = Modified, 2 = Remove Requested
    int action = 0;
    JsonValue* val = &node->value;
    if (!val) return false;

    // Use index-based ID so tree node open/close state is preserved across Undo/Redo deep clones
    // and doesn't lose keyboard focus or collapse when renaming keys.
    if (node_index >= 0) {
        current_path.push_back((size_t)node_index);
        ImGui::PushID(node_index);
    } else {
        ImGui::PushID("Root");
    }

    bool in_focus_path = false;
    bool is_focus_target = false;
    
    // The root node (node_index == -1) is passed as a local dummy copy, so we must compare against the true root pointer
    JsonValue* actual_val = (node_index == -1) ? doc->root_json : val;
    
    if (!focus_path.empty() && actual_val != nullptr) {
        in_focus_path = (std::find(focus_path.begin(), focus_path.end(), actual_val) != focus_path.end());
        is_focus_target = (actual_val == focus_path.front());
    }

    auto ApplyFocusTarget = [&]() {
        if (is_focus_target) {
            ImGui::SetScrollHereY(0.5f);
        }
        if (actual_val != nullptr && actual_val == highlight_val) {
            float fade = ImMax(0.0f, 1.0f - (float)(ImGui::GetTime() - highlight_time) / 1.5f);
            if (fade > 0.0f) {
                ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(255, 255, 0, (int)(100 * fade)));
            }
        }
    };

    // Inline Editor for Keys (Only show if it's an object member, omit for array elements)
    if (node->key != nullptr) {
        char key_buf[256];
        strncpy(key_buf, node->key, sizeof(key_buf) - 1);
        key_buf[sizeof(key_buf) - 1] = '\0';
        float calc_w = ImGui::CalcTextSize(key_buf).x + ImGui::GetStyle().FramePadding.x * 2.0f + 8.0f;
        ImGui::SetNextItemWidth(calc_w < 30.0f ? 30.0f : calc_w);
        if (ImGui::InputText("##key", key_buf, sizeof(key_buf), ImGuiInputTextFlags_NoHorizontalScroll)) {
            node->key = (char*)arena_alloc(&doc->main_arena, strlen(key_buf) + 1);
            strcpy(node->key, key_buf);
            action |= 1;
        }
        if (ImGui::IsItemActivated()) doc->PushUndo(UndoActionType::SetNode, current_path, *node);
        ImGui::SameLine(); ImGui::Text(":"); ImGui::SameLine();
    } else if (node_index >= 0) {
        ImGui::Text("[%d]:", node_index);
        ImGui::SameLine();
    } else {
        ImGui::Text("Root:");
        ImGui::SameLine();
    }

    bool is_container = (val->type == JSON_OBJECT || val->type == JSON_ARRAY);
    bool node_open = false;

    if (in_focus_path && is_container) {
        ImGui::SetNextItemOpen(true);
    }

    switch (val->type) {
        case JSON_OBJECT:
        case JSON_ARRAY: {
            bool is_obj = (val->type == JSON_OBJECT);
            node_open = ImGui::TreeNodeEx("Node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s (%zu items)", is_obj ? "{...}" : "[...]", val->as.list.count);
            ApplyFocusTarget();
            break;
        }
        case JSON_STRING: {
            static char str_buf[4096]; // Static buffer limits individual string edits to 4KB in tree view
            strncpy(str_buf, val->as.string ? val->as.string : "", sizeof(str_buf) - 1);
            str_buf[sizeof(str_buf) - 1] = '\0';
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText("##val", str_buf, sizeof(str_buf))) {
                val->as.string = (char*)arena_alloc(&doc->main_arena, strlen(str_buf) + 1);
                strcpy(val->as.string, str_buf);
                action |= 1;
            }
            if (ImGui::IsItemActivated()) doc->PushUndo(UndoActionType::SetNode, current_path, *node);
            ApplyFocusTarget();
            break;
        }
        case JSON_NUMBER: {
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputDouble("##val", &val->as.number)) {
                action |= 1;
            }
            if (ImGui::IsItemActivated()) doc->PushUndo(UndoActionType::SetNode, current_path, *node);
            ApplyFocusTarget();
            break;
        }
        case JSON_BOOL: {
            bool b = val->as.boolean;
            if (ImGui::Checkbox("##val", &b)) {
                doc->PushUndo(UndoActionType::SetNode, current_path, *node);
                val->as.boolean = b;
                action |= 1;
            }
            ApplyFocusTarget();
            break;
        }
        case JSON_NULL: {
            ImGui::TextDisabled("null");
            ApplyFocusTarget();
            break;
        }
    }
    
    // Right-Click Context Menu for structural edits
    if (ImGui::BeginPopupContextItem("node_context")) {
        ImGui::TextDisabled("Change Type");
        ImGui::Separator();
        if (ImGui::MenuItem("Object",  nullptr, val->type == JSON_OBJECT)) { 
            if (val->type != JSON_OBJECT) { doc->PushUndo(UndoActionType::SetNode, current_path, *node); *val = *json_create_object(&doc->main_arena); action |= 1; }
        }
        if (ImGui::MenuItem("Array",   nullptr, val->type == JSON_ARRAY)) { 
            if (val->type != JSON_ARRAY) { doc->PushUndo(UndoActionType::SetNode, current_path, *node); *val = *json_create_array(&doc->main_arena); action |= 1; }
        }
        if (ImGui::MenuItem("String",  nullptr, val->type == JSON_STRING)) { 
            if (val->type != JSON_STRING) { 
                char buf[128] = "";
                if (val->type == JSON_NUMBER) {
                    if (val->as.number == (int64_t)val->as.number)
                        snprintf(buf, sizeof(buf), "%lld", (long long)val->as.number);
                    else
                        snprintf(buf, sizeof(buf), "%.17g", val->as.number);
                }
                else if (val->type == JSON_BOOL) {
                    snprintf(buf, sizeof(buf), "%s", val->as.boolean ? "true" : "false");
                }
                doc->PushUndo(UndoActionType::SetNode, current_path, *node);
                *val = *json_create_string(&doc->main_arena, buf); 
                action |= 1; 
            }
        }
        if (ImGui::MenuItem("Number",  nullptr, val->type == JSON_NUMBER)) { 
            if (val->type != JSON_NUMBER) { 
                double num = 0.0;
                if (val->type == JSON_STRING && val->as.string) num = atof(val->as.string);
                else if (val->type == JSON_BOOL) num = val->as.boolean ? 1.0 : 0.0;
                doc->PushUndo(UndoActionType::SetNode, current_path, *node);
                *val = *json_create_number(&doc->main_arena, num); 
                action |= 1; 
            }
        }
        if (ImGui::MenuItem("Boolean", nullptr, val->type == JSON_BOOL)) { 
            if (val->type != JSON_BOOL) { 
                bool b = false;
                if (val->type == JSON_NUMBER) b = (val->as.number != 0.0);
                else if (val->type == JSON_STRING && val->as.string) b = (strcmp(val->as.string, "true") == 0 || strcmp(val->as.string, "True") == 0 || atof(val->as.string) != 0.0);
                doc->PushUndo(UndoActionType::SetNode, current_path, *node);
                *val = *json_create_bool(&doc->main_arena, b); 
                action |= 1; 
            }
        }
        if (ImGui::MenuItem("Null",    nullptr, val->type == JSON_NULL)) { 
            if (val->type != JSON_NULL) { doc->PushUndo(UndoActionType::SetNode, current_path, *node); *val = *json_create_null(&doc->main_arena); action |= 1; }
        }
        
        if (node_index >= 0) {
            ImGui::Separator();
            if (ImGui::MenuItem("Remove Node")) { doc->PushUndo(UndoActionType::InsertNode, current_path, *node); action |= 2; }
        }
        ImGui::EndPopup();
    }

    // Draw nested items if the node is expanded
    if (is_container && node_open) {
        bool is_obj = (val->type == JSON_OBJECT);
        int item_to_remove = -1;
        
        // Paginate massive arrays/objects
        const size_t chunk_size = doc->pagination_size;
        for (size_t chunk_start = 0; chunk_start < val->as.list.count; chunk_start += chunk_size) {
            size_t chunk_end = chunk_start + chunk_size;
            if (chunk_end > val->as.list.count) chunk_end = val->as.list.count;
            
            bool show_chunk = true;
            if (val->as.list.count > chunk_size) {
                bool chunk_has_focus = false;
                if (in_focus_path && focus_path.size() > 1 && actual_val != nullptr) {
                    auto it = std::find(focus_path.begin(), focus_path.end(), actual_val);
                    if (it != focus_path.end() && it != focus_path.begin()) {
                        JsonValue* next_in_path = *(it - 1);
                        for (size_t i = chunk_start; i < chunk_end; i++) {
                            if (&val->as.list.items[i].value == next_in_path) {
                                chunk_has_focus = true; break;
                            }
                        }
                    }
                }
                if (chunk_has_focus) {
                    ImGui::SetNextItemOpen(true);
                }

                char chunk_label[64];
                snprintf(chunk_label, sizeof(chunk_label), is_obj ? "{...} [%zu - %zu]" : "[...] [%zu - %zu]", chunk_start, chunk_end - 1);
                show_chunk = ImGui::TreeNodeEx((void*)(uintptr_t)chunk_start, ImGuiTreeNodeFlags_SpanAvailWidth, "%s", chunk_label);
            }
            
            if (show_chunk) {
                for (size_t i = chunk_start; i < chunk_end; i++) {
                    int child_action = DrawEditableJsonNode(doc, &val->as.list.items[i], (int)i, current_path, focus_path, highlight_val, highlight_time);
                    if (child_action & 1) action |= 1;
                    if (child_action & 2) item_to_remove = (int)i; // Mark child for removal
                }
                if (val->as.list.count > chunk_size) ImGui::TreePop();
            }
        }
        
        if (item_to_remove >= 0) {
            JsonRemoveAtIndex(val, item_to_remove);
            action |= 1;
        }

        if (ImGui::Button(is_obj ? "+ Add Key" : "+ Add Item")) {
            size_t insert_idx = val->as.list.count;
            current_path.push_back(insert_idx);
            doc->PushUndo(UndoActionType::RemoveNode, current_path, JsonNode{});
            current_path.pop_back();

            if (is_obj) {
                char new_key[32];
                snprintf(new_key, sizeof(new_key), "new_key_%zu", val->as.list.count);
                json_add(&doc->main_arena, val, new_key, json_create_null(&doc->main_arena));
            } else {
                json_append(&doc->main_arena, val, json_create_null(&doc->main_arena));
            }
            action |= 1;
        }

        ImGui::TreePop();
    }

    if (node_index >= 0) {
        current_path.pop_back();
    }
    ImGui::PopID();
    return action;
}

int main(int /*argc*/, char** /*argv*/) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        return -1;
    }

    const char* glsl_version = "#version 130";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Window* window = SDL_CreateWindow("JsonLens - Dear ImGui", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, window_flags);
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1); // Enable vsync

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    
    // Tweak styles to make menus and popups easier to distinguish without adding crazy colors
    ImGuiStyle& style = ImGui::GetStyle();
    style.Colors[ImGuiCol_PopupBg]   = ImVec4(0.15f, 0.15f, 0.15f, 0.98f); // Lighter gray background for dropdowns
    style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.08f, 0.08f, 0.08f, 1.00f); // Slightly lighter menu bar
    style.Colors[ImGuiCol_Border]    = ImVec4(0.30f, 0.30f, 0.30f, 1.00f); // Crisp subtle border
    style.PopupRounding              = 4.0f; // Soften popup corners
    style.FrameRounding              = 3.0f; // Soften text inputs and buttons

    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);

    LargeTextFile* doc = new LargeTextFile();
    char filepath_buffer[512] = "";

    AppSettings settings;
    settings.Load();
    io.FontGlobalScale = settings.zoom;

    std::atomic<bool> doc_loading{false};
    std::atomic<bool> doc_saving{false};
    std::atomic<bool> doc_formatting{false};
    std::atomic<LargeTextFile*> doc_ready{nullptr};
    std::thread doc_thread;

    bool show_search_bar = false;
    char search_buf[256] = "";
    char replace_buf[256] = "";
    std::vector<size_t> search_results;
    int search_active_idx = -1;
    std::atomic<bool> search_dirty{false};
    int scroll_to_line = -1;
    int scroll_to_line_frames = 0;
    int highlight_line = -1;
    double highlight_time = 0.0;
    bool force_text_tab = false;
    bool force_tree_tab = false;
    std::vector<JsonValue*> tree_focus_path;
    int tree_focus_frames = 0;
    JsonValue* tree_highlight_val = nullptr;
    double tree_highlight_time = 0.0;

    auto JumpToLine = [&](int line) {
        if (line < 0) return;
        scroll_to_line = line;
        scroll_to_line_frames = 3;
        highlight_line = line;
        highlight_time = ImGui::GetTime();
        force_text_tab = true;
    };

    auto LoadFile = [&](std::string path) {
        if (!path.empty() && !doc_loading && !doc_saving && !doc_formatting) {
            snprintf(filepath_buffer, sizeof(filepath_buffer), "%s", path.c_str());
            char title[1024];
            snprintf(title, sizeof(title), "JsonLens - %s", filepath_buffer);
            SDL_SetWindowTitle(window, title);
            settings.AddRecentFile(path);
            
            size_t pos = path.find_last_of("/\\");
            if (pos != std::string::npos) {
                settings.last_folder = path.substr(0, pos);
                settings.Save();
            }

            doc_loading = true;
            if (doc_thread.joinable()) doc_thread.join();
            
            bool allow_comments = settings.allow_comments;
            int pagination_size = settings.pagination_size;
            doc_thread = std::thread([path, allow_comments, pagination_size, &doc_ready, &doc_loading]() {
                LargeTextFile* new_doc = new LargeTextFile();
                new_doc->pagination_size = pagination_size;
                new_doc->Load(path.c_str(), allow_comments);
                doc_ready = new_doc;
                doc_loading = false;
            });
        }
    };

    auto SaveFile = [&](std::string path) {
        if (!path.empty() && !doc_loading && !doc_saving && !doc_formatting && doc->data) {
            snprintf(filepath_buffer, sizeof(filepath_buffer), "%s", path.c_str());
            char title[1024];
            snprintf(title, sizeof(title), "JsonLens - %s", filepath_buffer);
            SDL_SetWindowTitle(window, title);
            settings.AddRecentFile(path);

            doc_saving = true;
            if (doc_thread.joinable()) doc_thread.join();
            
            bool use_tabs = settings.use_tabs;
            int indent_size = settings.indent_size;
            doc_thread = std::thread([doc, path, use_tabs, indent_size, &doc_saving]() {
                doc->SaveToFile(path.c_str(), use_tabs, indent_size);
                doc_saving = false;
            });
        }
    };

    auto FormatFile = [&](bool pretty) {
        if (!doc_loading && !doc_saving && !doc_formatting && doc->data && doc->root_json) {
            doc_formatting = true;
            if (doc_thread.joinable()) doc_thread.join();
            
            bool use_tabs = settings.use_tabs;
            int indent_size = settings.indent_size;
            doc_thread = std::thread([doc, pretty, use_tabs, indent_size, &doc_formatting, &search_dirty]() {
                std::string old_text(doc->data, doc->size);
                doc->is_pretty = pretty;
                doc->tree_dirty = true;
                doc->RebuildTextFromTree(use_tabs, indent_size);
                
                if (doc->data) {
                    std::string new_text(doc->data, doc->size);
                    if (old_text != new_text) {
                        UndoRecord rec{};
                        rec.action = UndoActionType::TextReplace;
                        rec.text_deltas.push_back({0, std::move(old_text), std::move(new_text)});
                        doc->undo_stack.push_back(std::move(rec));
                        doc->redo_stack.clear();
                        while (doc->GetHistoryMemoryUsage() > 1024ULL * 1024ULL * 500ULL && doc->undo_stack.size() > 1) {
                            doc->undo_stack.erase(doc->undo_stack.begin());
                        }
                    }
                }
                doc_formatting = false;
                search_dirty = true;
            });
        }
    };

    // Application Main Loop
    bool done = false;
    while (!done) {
        bool zoom_changed = false;
        bool focus_search = false;
        bool focus_replace = false;
        
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) done = true;
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window)) done = true;
        }

        if (LargeTextFile* ready_doc = doc_ready.exchange(nullptr)) {
            delete doc;
            doc = ready_doc;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        // Setup a fullscreen dockspace/window
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->Pos);
        ImGui::SetNextWindowSize(ImGui::GetMainViewport()->Size);
        ImGui::Begin("Main", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_MenuBar);

        // Ensure perfect integer line heights to prevent floating point accumulation drift when scrolling massive virtualized and unvirtualized lists
        float font_size = ImGui::GetFontSize();
        float exact_frame_padding_y = std::round(style.FramePadding.y);
        float exact_line_height = std::max(1.0f, std::round(font_size + exact_frame_padding_y * 2.0f + style.ItemSpacing.y));
        float exact_item_spacing_y = exact_line_height - font_size - exact_frame_padding_y * 2.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, exact_frame_padding_y));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, exact_item_spacing_y));

        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Open...", "Ctrl+O")) {
                    LoadFile(ShowOpenFileDialog(settings.last_folder));
                }
                if (ImGui::BeginMenu("Recent Files")) {
                    if (settings.recent_files.empty()) {
                        ImGui::MenuItem("No recent files", nullptr, false, false);
                    } else {
                        for (const auto& file : settings.recent_files) {
                            if (ImGui::MenuItem(file.c_str())) {
                                LoadFile(file);
                            }
                        }
                        ImGui::Separator();
                        if (ImGui::MenuItem("Clear Recent")) {
                            settings.recent_files.clear();
                            settings.Save();
                        }
                    }
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                bool has_doc = (doc->data != nullptr) && !doc_loading && !doc_saving && !doc_formatting;
                if (ImGui::MenuItem("Save", "Ctrl+S", false, has_doc && filepath_buffer[0] != '\0')) {
                    SaveFile(filepath_buffer);
                }
                if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S", false, has_doc)) {
                    SaveFile(ShowSaveFileDialog());
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Close", "Ctrl+W", false, has_doc)) {
                    delete doc;
                    doc = new LargeTextFile();
                    filepath_buffer[0] = '\0';
                    SDL_SetWindowTitle(window, "JsonLens - Dear ImGui");
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Quit", "Alt+F4")) done = true;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Edit")) {
                bool has_doc = (doc->data != nullptr) && !doc_loading && !doc_saving && !doc_formatting;
                if (ImGui::MenuItem("Undo", "Ctrl+Z", false, has_doc && !doc->undo_stack.empty())) {
                    if (doc->Undo()) {
                        doc->ClearAstHistory();
                        doc_formatting = true;
                        if (doc_thread.joinable()) doc_thread.join();
                        doc_thread = std::thread([doc, &doc_formatting, &search_dirty]() { doc->RebuildTreeFromText(); doc_formatting = false; search_dirty = true; });
                    }
                }
                if (ImGui::MenuItem("Redo", "Ctrl+Y", false, has_doc && !doc->redo_stack.empty())) {
                    if (doc->Redo()) {
                        doc->ClearAstHistory();
                        doc_formatting = true;
                        if (doc_thread.joinable()) doc_thread.join();
                        doc_thread = std::thread([doc, &doc_formatting, &search_dirty]() { doc->RebuildTreeFromText(); doc_formatting = false; search_dirty = true; });
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Copy All", "Ctrl+Shift+C", false, has_doc)) {
                    if (doc->tree_dirty) doc->RebuildTextFromTree(settings.use_tabs, settings.indent_size);
                    ImGui::SetClipboardText(doc->data);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Find", "Ctrl+F", false, has_doc)) {
                    if (!show_search_bar) search_dirty = true;
                    show_search_bar = true;
                    focus_search = true;
                }
                if (ImGui::MenuItem("Replace", "Ctrl+H", false, has_doc)) {
                    if (!show_search_bar) search_dirty = true;
                    show_search_bar = true;
                    focus_replace = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Format JSON", "Alt+Shift+F", false, has_doc)) FormatFile(true);
                if (ImGui::MenuItem("Minify JSON", "Ctrl+Alt+M", false, has_doc)) FormatFile(false);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                if (ImGui::MenuItem("Zoom In", "Ctrl++")) { ImGui::GetIO().FontGlobalScale += 0.1f; zoom_changed = true; }
                if (ImGui::MenuItem("Zoom Out", "Ctrl+-")) { ImGui::GetIO().FontGlobalScale -= 0.1f; zoom_changed = true; }
                if (ImGui::MenuItem("Reset Zoom", "Ctrl+0")) { ImGui::GetIO().FontGlobalScale = 1.0f; zoom_changed = true; }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Settings")) {
                ImGui::TextDisabled("Graph View Double-Click");
                if (ImGui::RadioButton("Go to Text View", settings.graph_goto_target == 0)) {
                    settings.graph_goto_target = 0; settings.Save();
                }
                if (ImGui::RadioButton("Go to Tree View", settings.graph_goto_target == 1)) {
                    settings.graph_goto_target = 1; settings.Save();
                }
                ImGui::Separator();
                
                ImGui::TextDisabled("Parser");
                if (ImGui::Checkbox("Allow Comments", &settings.allow_comments)) settings.Save();
                
                ImGui::Separator();
                ImGui::TextDisabled("Formatting");
                if (ImGui::Checkbox("Use Tabs", &settings.use_tabs)) settings.Save();
                if (!settings.use_tabs) {
                    if (ImGui::SliderInt("Indent Size", &settings.indent_size, 1, 8)) settings.Save();
                }
                
                ImGui::Separator();
                ImGui::TextDisabled("Performance");
                if (ImGui::SliderInt("Pagination Size", &settings.pagination_size, 100, 10000)) {
                    settings.Save();
                    if (doc) {
                        doc->pagination_size = settings.pagination_size;
                        doc->graph_dirty = true;
                    }
                }
                ImGui::EndMenu();
            }
            
            // Push the Stats menu to the far right
            float stats_width = ImGui::CalcTextSize("Stats").x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
            float avail_width = ImGui::GetContentRegionAvail().x;
            if (avail_width > stats_width) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail_width - stats_width);
            }

            if (ImGui::BeginMenu("Stats")) {
                if (doc->data) {
                    ImGui::TextDisabled("File Size: %s", FormatMemory(doc->size).c_str());
                    ImGui::TextDisabled("Load Time: %s", FormatTime(doc->load_time_ms).c_str());
                    ImGui::TextDisabled("Parse Time: %s", FormatTime(doc->parse_time_ms).c_str());
                    ImGui::TextDisabled("Index Time: %s", FormatTime(doc->index_time_ms).c_str());
                    if (doc->format_time_ms > 0) ImGui::TextDisabled("Format Time: %s", FormatTime(doc->format_time_ms).c_str());
                    ImGui::Separator();
                    size_t view_mem = doc->data_capacity + doc->line_offsets.capacity() * sizeof(size_t);
                    ImGui::TextDisabled("View Memory: %s", FormatMemory(view_mem).c_str());
                    ImGui::TextDisabled("Parse Memory: %s", FormatMemory(doc->parse_memory_bytes).c_str());
                    ImGui::TextDisabled("History Memory: %s (%zu Undo, %zu Redo)", FormatMemory(doc->GetHistoryMemoryUsage()).c_str(), doc->undo_stack.size(), doc->redo_stack.size());
                } else {
                    ImGui::TextDisabled("No file loaded.");
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        // Global keyboard shortcuts
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O)) {
            LoadFile(ShowOpenFileDialog(settings.last_folder));
        }
        
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S) && !doc_loading && !doc_saving && !doc_formatting && doc->data) {
            if (ImGui::GetIO().KeyShift) {
                SaveFile(ShowSaveFileDialog());
            } else if (filepath_buffer[0] != '\0') {
                SaveFile(filepath_buffer);
            } else {
                SaveFile(ShowSaveFileDialog());
            }
        }
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_W) && !doc_loading && !doc_saving && !doc_formatting && doc->data) {
            delete doc;
            doc = new LargeTextFile();
            filepath_buffer[0] = '\0';
            SDL_SetWindowTitle(window, "JsonLens - Dear ImGui");
        }

        if (ImGui::GetIO().KeyCtrl && ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_C)) {
            if (!doc_loading && !doc_saving && !doc_formatting && doc->data) {
                if (doc->tree_dirty) doc->RebuildTextFromTree(settings.use_tabs, settings.indent_size);
                ImGui::SetClipboardText(doc->data);
            }
        }
        if (ImGui::GetIO().KeyAlt && ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_F)) {
            FormatFile(true);
        }
        if (ImGui::GetIO().KeyCtrl && ImGui::GetIO().KeyAlt && ImGui::IsKeyPressed(ImGuiKey_M)) {
            FormatFile(false);
        }
        
        bool can_global_search = !doc_loading && !doc_saving && !doc_formatting && doc->data;
        if (can_global_search && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F)) {
            if (!show_search_bar) search_dirty = true;
            show_search_bar = true;
            focus_search = true;
        }
        if (can_global_search && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_H)) {
            if (!show_search_bar) search_dirty = true;
            show_search_bar = true;
            focus_replace = true;
        }
        
        bool can_global_undo = !doc_loading && !doc_saving && !doc_formatting && doc->data && !ImGui::IsAnyItemActive();
        if (can_global_undo && ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)) {
            if (doc->Undo()) {
                doc->ClearAstHistory();
                doc_formatting = true;
                bool allow_comments = settings.allow_comments;
                if (doc_thread.joinable()) doc_thread.join();
                doc_thread = std::thread([doc, allow_comments, &doc_formatting, &search_dirty]() { doc->RebuildTreeFromText(allow_comments); doc_formatting = false; search_dirty = true; });
            }
        }
        if (can_global_undo && ImGui::GetIO().KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Y) || (ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)))) {
            if (doc->Redo()) {
                doc->ClearAstHistory();
                doc_formatting = true;
                bool allow_comments = settings.allow_comments;
                if (doc_thread.joinable()) doc_thread.join();
                doc_thread = std::thread([doc, allow_comments, &doc_formatting, &search_dirty]() { doc->RebuildTreeFromText(allow_comments); doc_formatting = false; search_dirty = true; });
            }
        }

        if (ImGui::GetIO().KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)))
            { ImGui::GetIO().FontGlobalScale += 0.1f; zoom_changed = true; }
        if (ImGui::GetIO().KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)))
            { ImGui::GetIO().FontGlobalScale -= 0.1f; zoom_changed = true; }
        if (ImGui::GetIO().KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_0) || ImGui::IsKeyPressed(ImGuiKey_Keypad0)))
            { ImGui::GetIO().FontGlobalScale = 1.0f; zoom_changed = true; }

        if (ImGui::GetIO().FontGlobalScale < 0.3f) ImGui::GetIO().FontGlobalScale = 0.3f;
        if (ImGui::GetIO().FontGlobalScale > 5.0f) ImGui::GetIO().FontGlobalScale = 5.0f;

        if (zoom_changed && settings.zoom != ImGui::GetIO().FontGlobalScale) {
            settings.zoom = ImGui::GetIO().FontGlobalScale;
            settings.Save();
        }

        if (filepath_buffer[0] != '\0') {
            ImGui::TextDisabled("Loaded: %s", filepath_buffer);
            ImGui::Separator();
        }

        if (!doc_loading && !doc_saving && !doc_formatting && doc->root_json == nullptr && doc->data != nullptr) {
            ImGui::TextColored(ImVec4(1, 0, 0, 1), "JSON Parse Error at Line %d, Col %d: %s", doc->last_err.line, doc->last_err.col, doc->last_err.msg);
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (ImGui::IsMouseClicked(0)) {
                    JumpToLine(doc->last_err.line - 1);
                }
            }
        }

        if (show_search_bar && doc->data) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.12f, 0.12f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
            ImGui::BeginChild("SearchAndReplace", ImVec2(0, ImGui::GetFrameHeightWithSpacing() * 2 + 8.0f), ImGuiChildFlags_Border);
            
            // Find Row
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Find:   ");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(300);
            if (focus_search) ImGui::SetKeyboardFocusHere();
            if (ImGui::InputText("##search", search_buf, sizeof(search_buf))) search_dirty = true;
            
            // Allow hitting Enter/Shift+Enter to navigate results while typing
            if (ImGui::IsItemFocused()) {
                if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
                    if (ImGui::GetIO().KeyShift) {
                        if (!search_results.empty()) {
                            search_active_idx = (search_active_idx > 0) ? search_active_idx - 1 : (int)search_results.size() - 1;
                            JumpToLine(doc->GetLineFromOffset(search_results[search_active_idx]));
                        }
                    } else {
                        if (!search_results.empty()) {
                            search_active_idx = (search_active_idx < (int)search_results.size() - 1) ? search_active_idx + 1 : 0;
                            JumpToLine(doc->GetLineFromOffset(search_results[search_active_idx]));
                        }
                    }
                }
            }

            ImGui::SameLine();
            if (search_results.empty()) {
                ImGui::TextDisabled("No results");
            } else {
                ImGui::Text("%d of %d", search_active_idx + 1, (int)search_results.size());
            }
            
            ImGui::SameLine();
            if (ImGui::Button("< Prev") && !search_results.empty()) {
                search_active_idx = (search_active_idx > 0) ? search_active_idx - 1 : (int)search_results.size() - 1;
                JumpToLine(doc->GetLineFromOffset(search_results[search_active_idx]));
            }
            ImGui::SameLine();
            if (ImGui::Button("Next >") && !search_results.empty()) {
                search_active_idx = (search_active_idx < (int)search_results.size() - 1) ? search_active_idx + 1 : 0;
                JumpToLine(doc->GetLineFromOffset(search_results[search_active_idx]));
            }
            ImGui::SameLine();
            if (ImGui::Button("X Close")) {
                show_search_bar = false;
            }

            // Replace Row
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Replace:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(300);
            if (focus_replace) ImGui::SetKeyboardFocusHere();
            ImGui::InputText("##replace", replace_buf, sizeof(replace_buf));
            
            ImGui::SameLine();
            if (ImGui::Button("Replace") && !search_results.empty() && doc->data) {
                doc->ReplaceCurrent(search_buf, replace_buf, search_results, search_active_idx);
                doc->ClearAstHistory();
                doc_formatting = true;
                bool allow_comments = settings.allow_comments;
                if (doc_thread.joinable()) doc_thread.join();
                doc_thread = std::thread([doc, allow_comments, &doc_formatting, &search_dirty]() { doc->RebuildTreeFromText(allow_comments); doc_formatting = false; search_dirty = true; });
            }
            ImGui::SameLine();
            if (ImGui::Button("Replace All") && !search_results.empty() && doc->data) {
                doc->ReplaceAll(search_buf, replace_buf, search_results);
                doc->ClearAstHistory();
                doc_formatting = true;
                bool allow_comments = settings.allow_comments;
                if (doc_thread.joinable()) doc_thread.join();
                doc_thread = std::thread([doc, allow_comments, &doc_formatting, &search_dirty]() { doc->RebuildTreeFromText(allow_comments); doc_formatting = false; search_dirty = true; });
            }

            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }

        if (search_dirty && !doc_formatting && doc->data) {
            search_results.clear();
            if (search_buf[0] != '\0') {
                size_t search_len = strlen(search_buf);
                const char* ptr = doc->data;
                while ((ptr = strstr(ptr, search_buf)) != nullptr) {
                    search_results.push_back(ptr - doc->data);
                    ptr += search_len;
                }
            }
            if (search_results.empty()) search_active_idx = -1;
            else if (search_active_idx >= (int)search_results.size()) search_active_idx = 0;
            else if (search_active_idx < 0) search_active_idx = 0;
            
            if (search_active_idx >= 0 && doc->line_offsets.size() > 0) {
                JumpToLine(doc->GetLineFromOffset(search_results[search_active_idx]));
            }
            search_dirty = false;
        }

        if (ImGui::BeginTabBar("Views")) {
            ImGuiTabItemFlags tree_tab_flags = force_tree_tab ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem("Tree View", nullptr, tree_tab_flags)) {
                force_tree_tab = false;
                ImGui::BeginChild("TreeChild", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
                if (doc_loading || doc_saving || doc_formatting) {
                    const char* msg = doc_loading ? "Loading and parsing JSON... Please wait." : 
                                      doc_saving ? "Saving JSON... Please wait." : 
                                      "Processing JSON... Please wait.";
                    ImGui::TextDisabled("%s", msg);
                } else if (doc->root_json) {
                    JsonNode dummy_root = {};
                    dummy_root.key = nullptr;
                    dummy_root.value = *doc->root_json;
                    
                    std::vector<size_t> root_path;
                    if (DrawEditableJsonNode(doc, &dummy_root, -1, root_path, tree_focus_path, tree_highlight_val, tree_highlight_time) & 1) {
                        *doc->root_json = dummy_root.value;
                        doc->tree_dirty = true;
                        doc->graph_dirty = true;
                    }
                } else {
                    ImGui::Text("No valid JSON loaded.");
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
                
                if (tree_focus_frames > 0) {
                    tree_focus_frames--;
                } else {
                    tree_focus_path.clear();
                }
            }
            
            if (ImGui::BeginTabItem("Graph View")) {
                ImGui::BeginChild("GraphViewChild", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_AlwaysVerticalScrollbar);
                if (doc_loading || doc_saving || doc_formatting) {
                    const char* msg = doc_loading ? "Loading and parsing JSON... Please wait." : 
                                      doc_saving ? "Saving JSON... Please wait." : 
                                      "Processing JSON... Please wait.";
                    ImGui::TextDisabled("%s", msg);
                } else if (doc->root_json) {
                    if (doc->graph_dirty) {
                        doc->ClearGraph();
                        int node_count = 0;
                        doc->graph_root = BuildGraphNode(doc, doc->root_json, "Root", 0, node_count);
                        float current_y = 40.0f;
                        float max_x = 0.0f;
                        if (doc->graph_root) {
                            LayoutGraphNode(doc->graph_root, 0, current_y, max_x);
                            doc->graph_total_width = max_x + 100.0f;
                            doc->graph_total_height = current_y + 40.0f;
                        }
                        doc->graph_dirty = false;
                    }

                    ImVec2 offset = ImGui::GetCursorScreenPos();
                    ImGui::Dummy(ImVec2(doc->graph_total_width, doc->graph_total_height));
                    
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    ImVec2 mouse_pos = ImGui::GetMousePos();
                    GraphNode* hovered_node = nullptr;
                    
                    if (doc->graph_root) {
                        ImVec2 win_pos = ImGui::GetWindowPos();
                        ImVec2 win_size = ImGui::GetWindowSize();
                        ImRect clip_rect(win_pos, ImVec2(win_pos.x + win_size.x, win_pos.y + win_size.y));
                        DrawGraphEdges(dl, doc->graph_root, offset, clip_rect);
                        DrawGraphNodes(dl, doc->graph_root, offset, mouse_pos, &hovered_node, clip_rect);
                    }
                    
                    if (hovered_node) {
                        if (hovered_node->type == GraphNodeType::Normal) {
                            if (ImGui::IsMouseDoubleClicked(0)) {
                                if (settings.graph_goto_target == 0) {
                                    JumpToLine(doc->GetLineFromOffset(hovered_node->offset));
                                } else {
                                    tree_focus_path.clear();
                                    GraphNode* curr = hovered_node;
                                    while (curr) {
                                        tree_focus_path.push_back(curr->source_val);
                                        curr = curr->parent;
                                    }
                                    tree_highlight_val = hovered_node->source_val;
                                    tree_highlight_time = ImGui::GetTime();
                                    force_tree_tab = true;
                                    tree_focus_frames = 3;
                                }
                            }
                        } else if (ImGui::IsMouseClicked(0)) {
                            if (hovered_node->type == GraphNodeType::PrevPage) {
                                if (doc->graph_pagination[hovered_node->source_val] >= (size_t)doc->pagination_size)
                                    doc->graph_pagination[hovered_node->source_val] -= doc->pagination_size;
                                else doc->graph_pagination[hovered_node->source_val] = 0;
                            } else if (hovered_node->type == GraphNodeType::NextPage) {
                                doc->graph_pagination[hovered_node->source_val] += hovered_node->page_step;
                            }
                            doc->graph_dirty = true;
                        }
                    }
                } else {
                    ImGui::Text("No valid JSON loaded.");
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            ImGuiTabItemFlags text_tab_flags = force_text_tab ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem("Text View", nullptr, text_tab_flags)) {
                force_text_tab = false;
                ImGui::BeginChild("TextChild", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
                // If the DOM was edited, trigger the text regeneration in a background thread
                if (doc->tree_dirty && !doc_loading && !doc_saving && !doc_formatting) {
                    doc->ClearTextHistory();
                    doc_formatting = true;
                    bool use_tabs = settings.use_tabs;
                    int indent_size = settings.indent_size;
                    if (doc_thread.joinable()) doc_thread.join();
                    doc_thread = std::thread([doc, use_tabs, indent_size, &doc_formatting, &search_dirty]() {
                        doc->RebuildTextFromTree(use_tabs, indent_size);
                        doc_formatting = false;
                        search_dirty = true;
                    });
                }

                if (doc_loading || doc_saving || doc_formatting) {
                    const char* msg = doc_loading ? "Loading text... Please wait." : 
                                      doc_saving ? "Saving text... Please wait." : 
                                      "Updating view... Please wait.";
                    ImGui::TextDisabled("%s", msg);
                } else {
                    if (doc->data) {
                        ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]); // Ensure monospace if loaded
                        
                        float exact_item_height = font_size + exact_item_spacing_y;
                        float char_width = ImGui::CalcTextSize("A").x;
                        ImVec2 text_start_pos = ImGui::GetCursorScreenPos();
                        
                        auto GetOffsetFromMouse = [&]() -> size_t {
                            if (doc->line_offsets.empty()) return 0;
                            
                            double scroll_y = (double)ImGui::GetScrollY();
                            double window_y = (double)ImGui::GetWindowPos().y;
                            double padding_y = (double)ImGui::GetStyle().WindowPadding.y;
                            
                            double local_mouse_y = (double)ImGui::GetMousePos().y - window_y - padding_y + scroll_y;
                            int raw_line_idx = (int)(local_mouse_y / (double)exact_item_height);
                            
                            if (raw_line_idx < 0) raw_line_idx = 0;
                            size_t line_idx = (size_t)raw_line_idx;
                            if (line_idx >= doc->line_offsets.size()) line_idx = doc->line_offsets.size() - 1;
                            size_t line_start = doc->line_offsets[line_idx];
                            size_t line_end = (line_idx + 1 < doc->line_offsets.size()) ? doc->line_offsets[line_idx + 1] : doc->size;
                            if (line_end > line_start && doc->data[line_end - 1] == '\n') line_end--;
                            if (line_end > line_start && doc->data[line_end - 1] == '\r') line_end--;
                            
                            float local_mouse_x = ImGui::GetMousePos().x - text_start_pos.x;
                            if (local_mouse_x <= 0.0f) return line_start;
                            
                            const char* s = doc->data + line_start;
                            const char* end = doc->data + line_end;
                            float current_x = 0.0f;
                            
                            ImFont* font = ImGui::GetFont();
                            const float scale = font_size / font->FontSize;
                            
                            while (s < end) {
                                unsigned int c = (unsigned int)(unsigned char)*s;
                                int bytes = 1;
                                if (c >= 0x80) bytes = ImTextCharFromUtf8(&c, s, end);
                                
                                if (c != '\r') {
                                    float c_width = font->GetCharAdvance((ImWchar)c) * scale;
                                    if (current_x + c_width * 0.5f > local_mouse_x) break;
                                    current_x += c_width;
                                }
                                s += bytes;
                            }
                            return (size_t)(s - doc->data);
                        };

                        if (ImGui::IsWindowHovered()) {
                            if (ImGui::IsMouseClicked(0)) {
                                doc->select_start = doc->select_end = GetOffsetFromMouse();
                            }
                        }
                        if (ImGui::IsMouseDown(0) && doc->select_start != (size_t)-1 && (ImGui::IsWindowHovered() || ImGui::IsWindowFocused())) {
                            doc->select_end = GetOffsetFromMouse();
                        }

                        if (ImGui::IsWindowFocused() && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C)) {
                            if (doc->select_start != (size_t)-1 && doc->select_start != doc->select_end) {
                                size_t s_start = std::min(doc->select_start, doc->select_end);
                                size_t s_end = std::max(doc->select_start, doc->select_end);
                                std::string sel(doc->data + s_start, s_end - s_start);
                                ImGui::SetClipboardText(sel.c_str());
                            }
                        }

                        if (scroll_to_line >= 0 && scroll_to_line_frames > 0) {
                            double target_y = (double)scroll_to_line * (double)exact_item_height;
                            ImGui::SetScrollY((float)(target_y - (double)ImGui::GetWindowHeight() * 0.5 + (double)exact_item_height * 0.5));
                            scroll_to_line_frames--;
                        } else {
                            scroll_to_line = -1;
                        }

                        // Always use read-only virtualized view for massive performance
                        ImGuiListClipper clipper;
                        clipper.Begin((int)doc->line_offsets.size(), exact_item_height);
                        while (clipper.Step()) {
                            for (size_t i = (size_t)clipper.DisplayStart; i < (size_t)clipper.DisplayEnd; i++) {
                                size_t start = doc->line_offsets[i];
                                size_t end = (i + 1 < doc->line_offsets.size()) ? doc->line_offsets[i + 1] : doc->size;
                                if (end > start && doc->data[end - 1] == '\n') end--; // Trim trailing newline for rendering
                                
                                ImVec2 pos = ImGui::GetCursorScreenPos();

                                if (highlight_line == (int)i) {
                                    float fade = ImMax(0.0f, 1.0f - (float)(ImGui::GetTime() - highlight_time) / 1.5f);
                                    if (fade > 0.0f) {
                                        ImGui::GetWindowDrawList()->AddRectFilled(
                                            ImVec2(pos.x, pos.y),
                                            ImVec2(pos.x + ImGui::GetContentRegionAvail().x, pos.y + ImGui::GetTextLineHeight()),
                                            IM_COL32(255, 255, 0, (int)(100 * fade))
                                        );
                                    } else {
                                        highlight_line = -1;
                                    }
                                }

                                // Selection Highlight
                                size_t s_start = std::min(doc->select_start, doc->select_end);
                                size_t s_end = std::max(doc->select_start, doc->select_end);
                                if (s_start != s_end && s_start < end && s_end > start) {
                                    size_t match_start = std::max(s_start, start);
                                    size_t match_end = std::min(s_end, end);
                                    float pre_width = ImGui::CalcTextSize(doc->data + start, doc->data + match_start).x;
                                    float match_width = ImGui::CalcTextSize(doc->data + match_start, doc->data + match_end).x;
                                    ImGui::GetWindowDrawList()->AddRectFilled(
                                        ImVec2(pos.x + pre_width, pos.y), 
                                        ImVec2(pos.x + pre_width + match_width, pos.y + ImGui::GetTextLineHeight()), 
                                        IM_COL32(0, 120, 215, 100)
                                    );
                                }

                                // Error Highlight
                                if (!doc->root_json && doc->data != nullptr && doc->last_err.offset > 0 && doc->last_err.offset <= doc->size) {
                                    size_t err_off = doc->last_err.offset;
                                    if (err_off >= start && err_off <= end) {
                                        float err_x = ImGui::CalcTextSize(doc->data + start, doc->data + err_off).x;
                                        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(pos.x, pos.y), ImVec2(pos.x + ImGui::GetContentRegionAvail().x, pos.y + ImGui::GetTextLineHeight()), IM_COL32(255, 0, 0, 50));
                                        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(pos.x + err_x, pos.y), ImVec2(pos.x + err_x + char_width, pos.y + ImGui::GetTextLineHeight()), IM_COL32(255, 0, 0, 200));
                                    }
                                }

                                if (show_search_bar && !search_results.empty() && search_buf[0] != '\0') {
                                    auto it = std::lower_bound(search_results.begin(), search_results.end(), start);
                                    while (it != search_results.end() && *it < end) {
                                        size_t match_offset = *it;
                                        size_t match_len = strlen(search_buf);
                                        size_t match_end_offset = std::min(match_offset + match_len, end);
                                        
                                        float pre_width = ImGui::CalcTextSize(doc->data + start, doc->data + match_offset).x;
                                        float match_width = ImGui::CalcTextSize(doc->data + match_offset, doc->data + match_end_offset).x;
                                        
                                        ImU32 bg_col = (search_active_idx >= 0 && search_results[search_active_idx] == match_offset) ? IM_COL32(255, 150, 0, 200) : IM_COL32(255, 255, 0, 100);
                                        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(pos.x + pre_width, pos.y), ImVec2(pos.x + pre_width + match_width, pos.y + ImGui::GetTextLineHeight()), bg_col);
                                        ++it;
                                    }
                                }

                                ImGui::TextUnformatted(doc->data + start, doc->data + end);
                            }
                        }
                        ImGui::PopFont();
                    }
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::PopStyleVar(2);
        ImGui::End(); // End Main

        // Rendering
        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.1f, 0.1f, 0.1f, 1.00f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    if (doc_thread.joinable()) doc_thread.join();
    delete doc;
    if (LargeTextFile* leftover = doc_ready.exchange(nullptr))
        delete leftover;

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}