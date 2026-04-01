#ifndef JSONLENS_DOCUMENT_H
#define JSONLENS_DOCUMENT_H

#include <vector>
#include <string>
#include <map>
#include <numeric>
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>

#include "arena_json.h"
#include "utils.h"
#include "views.h"

enum class UndoActionType {
    SetNode,
    InsertNode,
    RemoveNode,
    TextReplace
};

struct TextReplaceDelta {
    size_t pos;
    std::string old_str;
    std::string new_str;
};

struct JsonTreeStats {
    size_t total_nodes = 0;
    size_t max_depth = 0;
    size_t objects = 0;
    size_t arrays = 0;
    size_t strings = 0;
    size_t numbers = 0;
    size_t booleans = 0;
    size_t nulls = 0;
};

inline void CalculateJsonStats(JsonValue* val, JsonTreeStats& stats, size_t current_depth) {
    if (!val) return;
    stats.total_nodes++;
    if (current_depth > stats.max_depth) stats.max_depth = current_depth;
    
    switch (val->type) {
        case JSON_OBJECT:
            stats.objects++;
            for (size_t i = 0; i < val->as.list.count; i++) CalculateJsonStats(&val->as.list.items[i].value, stats, current_depth + 1);
            break;
        case JSON_ARRAY:
            stats.arrays++;
            for (size_t i = 0; i < val->as.list.count; i++) CalculateJsonStats(&val->as.list.items[i].value, stats, current_depth + 1);
            break;
        case JSON_STRING: stats.strings++; break;
        case JSON_NUMBER: stats.numbers++; break;
        case JSON_BOOL:   stats.booleans++; break;
        case JSON_NULL:   stats.nulls++; break;
    }
}

struct UndoRecord {
    UndoActionType action;
    std::vector<size_t> path;
    JsonNode saved_node;
    std::vector<TextReplaceDelta> text_deltas;
};

inline JsonValue* GetParentValueFromPath(JsonValue* root, const std::vector<size_t>& path) {
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

inline void JsonRemoveAtIndex(JsonValue* val, size_t index) {
    if (!val || (val->type != JSON_OBJECT && val->type != JSON_ARRAY)) return;
    if (index >= val->as.list.count) return;
    size_t items_to_move = val->as.list.count - index - 1;
    if (items_to_move > 0) memmove(&val->as.list.items[index], &val->as.list.items[index+1], items_to_move * sizeof(JsonNode));
    val->as.list.count--;
}

inline void JsonInsertAtIndex(Arena* a, JsonValue* parent, size_t index, const JsonNode& node) {
    if (!a || !parent || (parent->type != JSON_OBJECT && parent->type != JSON_ARRAY)) return;
    if (index > parent->as.list.count) index = parent->as.list.count;
    size_t old_count = parent->as.list.count;
    JsonNode* new_items = static_cast<JsonNode*>(arena_alloc(a, (old_count + 1) * sizeof(JsonNode)));
    if (index > 0) memcpy(new_items, parent->as.list.items, index * sizeof(JsonNode));
    if (index < old_count) memcpy(new_items + index + 1, parent->as.list.items + index, (old_count - index) * sizeof(JsonNode));
    new_items[index] = node;
    parent->as.list.items = new_items;
    parent->as.list.count = old_count + 1;
}

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
    size_t graph_memory_bytes = 0;
    
    std::map<JsonValue*, size_t> graph_pagination;

    bool tree_dirty = false;
    bool is_pretty = true;

    double load_time_ms = 0;
    double parse_time_ms = 0;
    double index_time_ms = 0;
    double format_time_ms = 0;
    size_t parse_memory_bytes = 0;
    
    int pagination_size = 2000;

    JsonTreeStats ast_stats;
    size_t max_line_length = 0;

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

    LargeTextFile(const LargeTextFile&) = delete;
    LargeTextFile& operator=(const LargeTextFile&) = delete;

    void BuildLineOffsets() {
        line_offsets.clear();
        if (!data) return;
        line_offsets.reserve(size / 50 + 1024);
        line_offsets.push_back(0);
        size_t start = 0;
        size_t max_len = 0;
        while (start < size) {
            size_t remain = size - start;
            size_t chunk = (remain > 4096) ? 4096 : remain;
            const void* p = memchr(data + start, '\n', chunk);
            if (p) {
                size_t next_start = static_cast<size_t>(static_cast<const char*>(p) - data) + 1;
                size_t len = next_start - start - 1;
                if (len > max_len) max_len = len;
                line_offsets.push_back(next_start);
                start = next_start;
            } else if (remain >= 4096) {
                size_t len = 4096;
                if (len > max_len) max_len = len;
                line_offsets.push_back(start + 4096);
                start += 4096;
            } else {
                size_t len = remain;
                if (len > max_len) max_len = len;
                break;
            }
        }
        line_offsets.shrink_to_fit();
        max_line_length = max_len;
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
        graph_memory_bytes = 0;
    }

    size_t CalculateGraphMemory(const GraphNode* node) const {
        if (!node) return 0;
        size_t mem = sizeof(GraphNode) + node->label.capacity() + node->children.capacity() * sizeof(GraphNode*);
        for (auto child : node->children) {
            mem += CalculateGraphMemory(child);
        }
        return mem;
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
            total += std::accumulate(r.text_deltas.begin(), r.text_deltas.end(), (size_t)0,
                                     [](size_t acc, const TextReplaceDelta& d) { return acc + d.old_str.capacity() + d.new_str.capacity(); });
        }
        for (const auto& r : redo_stack) {
            total += sizeof(UndoRecord) + r.path.capacity() * sizeof(size_t) + r.text_deltas.capacity() * sizeof(TextReplaceDelta);
            total += std::accumulate(r.text_deltas.begin(), r.text_deltas.end(), (size_t)0,
                                     [](size_t acc, const TextReplaceDelta& d) { return acc + d.old_str.capacity() + d.new_str.capacity(); });
        }
        return total;
    }

    void RecomputeStats() {
        ast_stats = JsonTreeStats();
        if (root_json) {
            CalculateJsonStats(root_json, ast_stats, 0);
        }
    }

    void Load(const char* filepath, bool allow_comments) {
        ClearHistory();
        ClearGraph();
        if (data) { free(data); data = nullptr; }
        tree_dirty = false; graph_dirty = true;
        arena_free(&main_arena); arena_free(&scratch_arena);
        arena_init(&main_arena); arena_init(&scratch_arena);
        root_json = nullptr;
        load_time_ms = parse_time_ms = index_time_ms = format_time_ms = 0;
        parse_memory_bytes = 0;
        
        Uint64 t_freq = SDL_GetPerformanceFrequency();
        Uint64 t0 = SDL_GetPerformanceCounter();
        FILE* f = fopen(filepath, "rb");
        if (!f) return;
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        if (fsize < 0) { fclose(f); return; }
        size = (size_t)fsize;
        fseek(f, 0, SEEK_SET);
        if (size > SIZE_MAX - 1024 * 1024) { fclose(f); return; }
        data_capacity = size + 1024 * 1024;
        data = static_cast<char*>(malloc(data_capacity));
        if (data) {
            size_t bytes_read = fread(data, 1, size, f);
            data[bytes_read] = '\0';
            size = bytes_read;
        } else {
            size = 0;
        }
        fclose(f);

        Uint64 t1 = SDL_GetPerformanceCounter();
        load_time_ms = (double)(t1 - t0) * 1000.0 / t_freq;
        Uint64 t2 = SDL_GetPerformanceCounter();
        int parse_flags = allow_comments ? JSON_PARSE_ALLOW_COMMENTS : JSON_PARSE_STRICT;
        root_json = json_parse(&main_arena, &scratch_arena, data, size, parse_flags, &last_err);
        Uint64 t3 = SDL_GetPerformanceCounter();
        parse_time_ms = (double)(t3 - t2) * 1000.0 / t_freq;
        Uint64 t4 = SDL_GetPerformanceCounter();
        BuildLineOffsets();
        Uint64 t5 = SDL_GetPerformanceCounter();
        index_time_ms = (double)(t5 - t4) * 1000.0 / t_freq;
        
        // Free the scratch arena to release its high-water mark capacity back to the OS
        arena_free(&scratch_arena);
        arena_init(&scratch_arena);
        
        parse_memory_bytes = GetArenaMemoryUsage(&main_arena) + GetArenaMemoryUsage(&scratch_arena);
        RecomputeStats();
    }

    void RebuildTextFromTree(bool use_tabs, int indent_step, bool keep_comments) {
        if (!tree_dirty || !root_json) return;
        Uint64 t_freq = SDL_GetPerformanceFrequency();
        Uint64 t0 = SDL_GetPerformanceCounter();
        arena_free(&scratch_arena);
        arena_init(&scratch_arena);
        const char* new_text = json_to_string(&scratch_arena, root_json, is_pretty, use_tabs, indent_step, keep_comments);
        if (new_text) {
            size_t new_len = strlen(new_text);
            if (new_len >= data_capacity) {
                data_capacity = new_len + 1024 * 1024;
                char* new_data = static_cast<char*>(realloc(data, data_capacity));
                if (new_data) data = new_data; else return;
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
        
        // Free the scratch arena since the serialized string has been copied
        arena_free(&scratch_arena);
        arena_init(&scratch_arena);
        
        tree_dirty = false;
        parse_memory_bytes = GetArenaMemoryUsage(&main_arena) + GetArenaMemoryUsage(&scratch_arena);
    }

    void SaveToFile(const char* filepath, bool use_tabs, int indent_step, bool keep_comments) {
        RebuildTextFromTree(use_tabs, indent_step, keep_comments);
        if (!data) return;
        FILE* f = fopen(filepath, "wb");
        if (!f) return;
        fwrite(data, 1, size, f);
        fclose(f);
    }

    void PushUndo(UndoActionType action, const std::vector<size_t>& path, const JsonNode& node = JsonNode{}) {
        undo_stack.push_back({ action, path, node, {} });
        redo_stack.clear();
        if (undo_stack.size() > 50000) undo_stack.erase(undo_stack.begin());
    }

    bool ExecuteHistoryAction(const UndoRecord& rec, std::vector<UndoRecord>& opposite_stack, bool is_undo) {
        if (!root_json && rec.action != UndoActionType::TextReplace) return false;
        if (rec.action == UndoActionType::TextReplace) {
            if (is_undo) {
                long diff = std::accumulate(rec.text_deltas.begin(), rec.text_deltas.end(), 0L,
                                       [](long acc, const TextReplaceDelta& d) { return acc + (long)d.new_str.length() - (long)d.old_str.length(); });
                for (auto it = rec.text_deltas.rbegin(); it != rec.text_deltas.rend(); ++it) {
                    diff -= (long)it->new_str.length() - (long)it->old_str.length();
                    size_t current_pos = it->pos + diff;
                    size_t old_len = it->new_str.length();
                    size_t new_len = it->old_str.length();
                    if (size - old_len + new_len >= data_capacity) { data_capacity = size + new_len + 1024 * 1024; char* new_data = static_cast<char*>(realloc(data, data_capacity)); if (new_data) data = new_data; else return false; }
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
                    if (size - old_len + new_len >= data_capacity) { data_capacity = size + new_len + 1024 * 1024; char* new_data = static_cast<char*>(realloc(data, data_capacity)); if (new_data) data = new_data; else return false; }
                    memmove(data + current_pos + new_len, data + current_pos + old_len, size - current_pos - old_len);
                    memcpy(data + current_pos, d.new_str.c_str(), new_len);
                    size = size - old_len + new_len;
                    data[size] = '\0';
                    diff += (long)new_len - (long)old_len;
                }
            }
        BuildLineOffsets();
            opposite_stack.push_back(rec);
            return true;
        }
        if (rec.path.empty()) {
            if (rec.action == UndoActionType::SetNode) {
                JsonNode current_root_node = JsonNode{}; current_root_node.value = *root_json;
                opposite_stack.push_back({ UndoActionType::SetNode, rec.path, current_root_node, {} });
                *root_json = rec.saved_node.value; tree_dirty = true; graph_dirty = true; graph_pagination.clear();
            }
            return false;
        }
        JsonValue* parent = GetParentValueFromPath(root_json, rec.path);
        if (!parent) return false;
        size_t target_idx = rec.path.back();
        if (rec.action == UndoActionType::SetNode) {
            if (target_idx < parent->as.list.count) {
                opposite_stack.push_back({ UndoActionType::SetNode, rec.path, parent->as.list.items[target_idx], {} });
                parent->as.list.items[target_idx] = rec.saved_node; tree_dirty = true; graph_dirty = true; graph_pagination.clear();
            }
        } else if (rec.action == UndoActionType::InsertNode) {
            opposite_stack.push_back({ UndoActionType::RemoveNode, rec.path, JsonNode{}, {} });
            JsonInsertAtIndex(&main_arena, parent, target_idx, rec.saved_node); tree_dirty = true; graph_dirty = true; graph_pagination.clear();
        } else if (rec.action == UndoActionType::RemoveNode) {
            if (target_idx < parent->as.list.count) {
                opposite_stack.push_back({ UndoActionType::InsertNode, rec.path, parent->as.list.items[target_idx], {} });
                JsonRemoveAtIndex(parent, target_idx); tree_dirty = true; graph_dirty = true; graph_pagination.clear();
            }
        }
        return false;
    }

    void RebuildTreeFromText(bool allow_comments) {
        Uint64 t_freq = SDL_GetPerformanceFrequency(); Uint64 t0 = SDL_GetPerformanceCounter();
        arena_free(&main_arena); arena_free(&scratch_arena);
        arena_init(&main_arena); arena_init(&scratch_arena);
        int parse_flags = allow_comments ? JSON_PARSE_ALLOW_COMMENTS : JSON_PARSE_STRICT;
        root_json = json_parse(&main_arena, &scratch_arena, data, size, parse_flags, &last_err);
        BuildLineOffsets(); ClearGraph(); tree_dirty = false; graph_dirty = true;
        Uint64 t1 = SDL_GetPerformanceCounter();
        parse_time_ms = (double)(t1 - t0) * 1000.0 / t_freq;
        
        // Free temporary parsing memory
        arena_free(&scratch_arena);
        arena_init(&scratch_arena);
        
        parse_memory_bytes = GetArenaMemoryUsage(&main_arena) + GetArenaMemoryUsage(&scratch_arena);
        RecomputeStats();
    }

    void ReplaceCurrent(const char* search_str, const char* replace_str, const std::vector<size_t>& results, int idx) { if (idx < 0 || idx >= (int)results.size()) return; size_t match_pos = results[idx]; size_t search_len = strlen(search_str); size_t replace_len = strlen(replace_str); UndoRecord rec{}; rec.action = UndoActionType::TextReplace; rec.text_deltas.push_back({match_pos, search_str, replace_str}); undo_stack.push_back(rec); redo_stack.clear(); if (size - search_len + replace_len >= data_capacity) { data_capacity = size + replace_len + 1024 * 1024; char* new_data = static_cast<char*>(realloc(data, data_capacity)); if (!new_data) return; data = new_data; } memmove(data + match_pos + replace_len, data + match_pos + search_len, size - match_pos - search_len); memcpy(data + match_pos, replace_str, replace_len); size = size - search_len + replace_len; data[size] = '\0'; BuildLineOffsets(); }
    void ReplaceAll(const char* search_str, const char* replace_str, const std::vector<size_t>& results) { if (results.empty()) return; size_t search_len = strlen(search_str); size_t replace_len = strlen(replace_str); UndoRecord rec{}; rec.action = UndoActionType::TextReplace; for (size_t match_pos : results) { rec.text_deltas.push_back({match_pos, search_str, replace_str}); } undo_stack.push_back(rec); redo_stack.clear(); if (replace_len <= search_len) { size_t write_pos = results[0]; size_t read_pos = results[0]; size_t diff = search_len - replace_len; size_t total_diff = 0; for (size_t i = 0; i < results.size(); i++) { size_t match_pos = results[i]; size_t chunk_size = match_pos - read_pos; if (chunk_size > 0 && write_pos != read_pos) memmove(data + write_pos, data + read_pos, chunk_size); write_pos += chunk_size; read_pos = match_pos + search_len; memcpy(data + write_pos, replace_str, replace_len); write_pos += replace_len; total_diff += diff; } if (read_pos < size) memmove(data + write_pos, data + read_pos, size - read_pos); size -= total_diff; data[size] = '\0'; } else { size_t diff = replace_len - search_len; size_t new_size = size + results.size() * diff; if (new_size >= data_capacity) { data_capacity = new_size + 1024 * 1024; char* new_data = static_cast<char*>(realloc(data, data_capacity)); if (!new_data) return; data = new_data; } size_t read_pos = size, write_pos = new_size; for (int i = (int)results.size() - 1; i >= 0; i--) { size_t match_pos = results[i]; size_t chunk_size = read_pos - (match_pos + search_len); if (chunk_size > 0) { write_pos -= chunk_size; read_pos -= chunk_size; memmove(data + write_pos, data + read_pos, chunk_size); } write_pos -= replace_len; read_pos -= search_len; memcpy(data + write_pos, replace_str, replace_len); } size = new_size; data[size] = '\0'; } BuildLineOffsets(); }
    bool Undo() { if (undo_stack.empty()) return false; UndoRecord rec = undo_stack.back(); undo_stack.pop_back(); return ExecuteHistoryAction(rec, redo_stack, true); }
    bool Redo() { if (redo_stack.empty()) return false; UndoRecord rec = redo_stack.back(); redo_stack.pop_back(); return ExecuteHistoryAction(rec, undo_stack, false); }
    
    void ReplaceText(size_t start_off, size_t end_off, const char* new_text) {
        if (start_off > size) start_off = size;
        if (end_off > size) end_off = size;
        if (start_off > end_off) std::swap(start_off, end_off);
        size_t old_len = end_off - start_off;
        size_t new_len = strlen(new_text);
        UndoRecord rec{};
        rec.action = UndoActionType::TextReplace;
        rec.text_deltas.push_back({start_off, std::string(data + start_off, old_len), std::string(new_text, new_len)});
        undo_stack.push_back(std::move(rec));
        redo_stack.clear();
        if (size - old_len + new_len >= data_capacity) {
            data_capacity = size + new_len + 1024 * 1024;
            char* new_data = static_cast<char*>(realloc(data, data_capacity));
            if (!new_data) return;
            data = new_data;
        }
        memmove(data + start_off + new_len, data + end_off, size - end_off);
        memcpy(data + start_off, new_text, new_len);
        size = size - old_len + new_len;
        data[size] = '\0';
        BuildLineOffsets();
    }

    void ReplaceLine(int line_idx, const char* new_text) { if (line_idx < 0 || line_idx >= (int)line_offsets.size()) return; size_t start = line_offsets[line_idx]; size_t end = (line_idx + 1 < (int)line_offsets.size()) ? line_offsets[line_idx + 1] : size; size_t text_end = end; if (text_end > start && data[text_end - 1] == '\n') text_end--; if (text_end > start && data[text_end - 1] == '\r') text_end--; size_t old_len = text_end - start; size_t new_len = strlen(new_text); UndoRecord rec{}; rec.action = UndoActionType::TextReplace; rec.text_deltas.push_back({start, std::string(data + start, old_len), std::string(new_text, new_len)}); undo_stack.push_back(std::move(rec)); redo_stack.clear(); if (size - old_len + new_len >= data_capacity) { data_capacity = size + new_len + 1024 * 1024; char* new_data = static_cast<char*>(realloc(data, data_capacity)); if (!new_data) return; data = new_data; } memmove(data + start + new_len, data + start + old_len, size - start - old_len); memcpy(data + start, new_text, new_len); size = size - old_len + new_len; data[size] = '\0'; 
        long diff = (long)new_len - (long)old_len;
        for (size_t i = line_idx + 1; i < line_offsets.size(); i++) line_offsets[i] += diff;
        std::vector<size_t> added_lines;
        const char* p = new_text;
        while ((p = strchr(p, '\n')) != nullptr) {
            added_lines.push_back(start + (p - new_text) + 1);
            p++;
        }
        if (!added_lines.empty()) {
            line_offsets.insert(line_offsets.begin() + line_idx + 1, added_lines.begin(), added_lines.end());
        }
    }
};

#endif // JSONLENS_DOCUMENT_H