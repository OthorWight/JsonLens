#include "settings.h"
#include "arena_json.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>

std::string AppSettings::GetSettingsPath() {
    char* pref_path = SDL_GetPrefPath("JsonLens", "JsonLens");
    std::string path = pref_path ? std::string(pref_path) + "settings.json" : "settings.json";
    if (pref_path) SDL_free(pref_path);
    return path;
}

void AppSettings::Load() {
    std::string path = GetSettingsPath();
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize <= 0) { fclose(f); return; }
    size_t size = (size_t)fsize;
    fseek(f, 0, SEEK_SET);
    char* data = static_cast<char*>(malloc(size + 1));
    if (!data) {
        fclose(f);
        return;
    }
    size_t bytes_read = fread(data, 1, size, f);
    data[bytes_read] = '\0';
    fclose(f);

    Arena arena, scratch;
    arena_init(&arena);
    arena_init(&scratch);
    JsonError err;
    JsonValue* root = json_parse(&arena, &scratch, data, bytes_read, JSON_PARSE_ALLOW_COMMENTS, &err);
    if (root && root->type == JSON_OBJECT) {
        zoom = (float)json_object_get_number(root, "zoom", 1.0);
        JsonValue* recents = json_object_get(root, "recent_files");
        
        JsonNode* entry;
        json_array_foreach(entry, recents) {
            if (entry->value.type == JSON_STRING && entry->value.as.string) {
                recent_files.push_back(entry->value.as.string);
            }
        }
        const char* folder = json_object_get_string(root, "last_folder", nullptr);
        if (folder) last_folder = folder;
        show_text_view = json_object_get_bool(root, "show_text_view", true);
        show_tree_view = json_object_get_bool(root, "show_tree_view", false);
        show_graph_view = json_object_get_bool(root, "show_graph_view", false);
        graph_goto_target = (int)json_object_get_number(root, "graph_goto_target", 0.0);
        allow_comments = json_object_get_bool(root, "allow_comments", true);
        use_tabs = json_object_get_bool(root, "use_tabs", false);
        indent_size = (int)json_object_get_number(root, "indent_size", 2.0);
        default_view = (int)json_object_get_number(root, "default_view", 0.0);
        window_width = (int)json_object_get_number(root, "window_width", 1280.0);
        window_height = (int)json_object_get_number(root, "window_height", 720.0);
        window_maximized = json_object_get_bool(root, "window_maximized", false);
    }
    arena_free(&arena);
    arena_free(&scratch);
    free(data);
}

void AppSettings::Save() const {
    std::string path = GetSettingsPath();
    Arena arena;
    arena_init(&arena);
    JsonValue* root = json_create_object(&arena);
    json_object_add_number(&arena, root, "zoom", zoom);
    
    JsonValue* recents = json_create_array(&arena);
    for (const auto& file : recent_files) {
        json_array_append_string(&arena, recents, file.c_str());
    }
    json_object_add(&arena, root, "recent_files", recents);
    
    if (!last_folder.empty()) json_object_add_string(&arena, root, "last_folder", last_folder.c_str());
    json_object_add_bool(&arena, root, "show_text_view", show_text_view);
    json_object_add_bool(&arena, root, "show_tree_view", show_tree_view);
    json_object_add_bool(&arena, root, "show_graph_view", show_graph_view);
    json_object_add_number(&arena, root, "graph_goto_target", (double)graph_goto_target);
    json_object_add_bool(&arena, root, "allow_comments", allow_comments);
    json_object_add_bool(&arena, root, "use_tabs", use_tabs);
    json_object_add_number(&arena, root, "indent_size", (double)indent_size);
    json_object_add_number(&arena, root, "default_view", (double)default_view);
    json_object_add_number(&arena, root, "window_width", (double)window_width);
    json_object_add_number(&arena, root, "window_height", (double)window_height);
    json_object_add_bool(&arena, root, "window_maximized", window_maximized);

    const char* str = json_serialize(&arena, root, true, false, 4, false);
    if (str) {
        FILE* f = fopen(path.c_str(), "wb");
        if (f) { fwrite(str, 1, strlen(str), f); fclose(f); }
    }
    arena_free(&arena);
}

void AppSettings::AddRecentFile(std::string path) {
    auto it = std::find(recent_files.begin(), recent_files.end(), path);
    if (it != recent_files.end()) {
        recent_files.erase(it);
    }
    recent_files.insert(recent_files.begin(), std::move(path));
    if (recent_files.size() > 10) recent_files.resize(10);
    Save();
}