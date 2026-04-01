#ifndef JSONLENS_SETTINGS_H
#define JSONLENS_SETTINGS_H

#include <string>
#include <vector>

struct AppSettings {
    float zoom = 1.0f;
    std::vector<std::string> recent_files;
    std::string last_folder;
    bool show_text_view = true;
    bool show_tree_view = false;
    bool show_graph_view = false;
    int graph_goto_target = 0; // 0 = Text View, 1 = Tree View
    bool allow_comments = true;
    bool use_tabs = false;
    int indent_size = 2;
    int pagination_size = 2000;
    int default_view = 0; // 0 = Text, 1 = Tree, 2 = Graph
    int window_width = 1280;
    int window_height = 720;
    bool window_maximized = false;

    static std::string GetSettingsPath();
    void Load();
    void Save() const;
    void AddRecentFile(std::string path);
};

#endif // JSONLENS_SETTINGS_H