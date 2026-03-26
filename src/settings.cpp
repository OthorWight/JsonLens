#include "settings.h"
#include "arena_json.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        default_view = (int)json_get_number(root, "default_view", 0.0);
        window_width = (int)json_get_number(root, "window_width", 1280.0);
        window_height = (int)json_get_number(root, "window_height", 720.0);
        window_maximized = json_get_bool(root, "window_maximized", false);
    }
    arena_free(&arena);
    arena_free(&scratch);
    free(data);
}

void AppSettings::Save() {
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
    
    if (!last_folder.empty()) json_add_string(&arena, root, "last_folder", last_folder.c_str());
    json_add_number(&arena, root, "graph_goto_target", (double)graph_goto_target);
    json_add_bool(&arena, root, "allow_comments", allow_comments);
    json_add_bool(&arena, root, "use_tabs", use_tabs);
    json_add_number(&arena, root, "indent_size", (double)indent_size);
    json_add_number(&arena, root, "pagination_size", (double)pagination_size);
    json_add_number(&arena, root, "default_view", (double)default_view);
    json_add_number(&arena, root, "window_width", (double)window_width);
    json_add_number(&arena, root, "window_height", (double)window_height);
    json_add_bool(&arena, root, "window_maximized", window_maximized);

    char* str = json_to_string(&arena, root, true, false, 4, false);
    if (str) {
        FILE* f = fopen(path.c_str(), "wb");
        if (f) { fwrite(str, 1, strlen(str), f); fclose(f); }
    }
    arena_free(&arena);
}

void AppSettings::AddRecentFile(std::string path) {
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