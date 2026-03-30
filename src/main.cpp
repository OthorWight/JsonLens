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
#include <functional>

#include "arena_json.h"
#include "settings.h"
#include "utils.h"
#include "views.h"
#include "document.h"

struct PathToken {
    enum Type { Key, Index, Wildcard } type;
    std::string key;
    int index;
};

std::vector<PathToken> ParseJsonPath(const std::string& path) {
    std::vector<PathToken> tokens;
    size_t i = 0;
    while (i < path.length()) {
        if (isspace(path[i])) { i++; continue; }
        if (path[i] == '$') { i++; continue; }
        if (i + 4 <= path.length() && path.substr(i, 4) == "root") { i += 4; continue; }
        
        if (path[i] == '.') {
            i++;
            if (i < path.length() && path[i] == '*') {
                tokens.push_back({PathToken::Wildcard, "", 0});
                i++;
            } else {
                std::string key;
                while (i < path.length() && path[i] != '.' && path[i] != '[' && !isspace(path[i])) {
                    key += path[i];
                    i++;
                }
                if (!key.empty()) tokens.push_back({PathToken::Key, key, 0});
            }
        } else if (path[i] == '[') {
            i++;
            while (i < path.length() && isspace(path[i])) i++;
            if (i < path.length() && path[i] == '*') {
                tokens.push_back({PathToken::Wildcard, "", 0});
                i++;
                while (i < path.length() && path[i] != ']') i++;
                if (i < path.length() && path[i] == ']') i++;
            } else if (i < path.length() && (path[i] == '"' || path[i] == '\'')) {
                char quote = path[i];
                i++;
                std::string key;
                while (i < path.length() && path[i] != quote) {
                    key += path[i];
                    i++;
                }
                if (i < path.length() && path[i] == quote) i++;
                while (i < path.length() && path[i] != ']') i++;
                if (i < path.length() && path[i] == ']') i++;
                tokens.push_back({PathToken::Key, key, 0});
            } else {
                std::string idx_str;
                while (i < path.length() && isdigit(path[i])) {
                    idx_str += path[i];
                    i++;
                }
                while (i < path.length() && path[i] != ']') i++;
                if (i < path.length() && path[i] == ']') i++;
                if (!idx_str.empty()) tokens.push_back({PathToken::Index, "", std::stoi(idx_str)});
            }
        } else {
            std::string key;
            while (i < path.length() && path[i] != '.' && path[i] != '[' && !isspace(path[i])) {
                key += path[i];
                i++;
            }
            if (!key.empty()) tokens.push_back({PathToken::Key, key, 0});
        }
    }
    return tokens;
}

void EvaluateJsonPathTokens(JsonValue* current, const std::vector<PathToken>& tokens, size_t token_idx, std::vector<std::vector<JsonValue*>>& paths, std::vector<JsonValue*>& current_path) {
    if (!current) return;
    current_path.push_back(current);

    if (token_idx >= tokens.size()) {
        paths.push_back(current_path);
        current_path.pop_back();
        return;
    }

    const PathToken& token = tokens[token_idx];
    if (token.type == PathToken::Key) {
        if (current->type == JSON_OBJECT) {
            for (size_t i = 0; i < current->as.list.count; i++) {
                if (current->as.list.items[i].key && token.key == current->as.list.items[i].key) {
                    EvaluateJsonPathTokens(&current->as.list.items[i].value, tokens, token_idx + 1, paths, current_path);
                    break;
                }
            }
        }
    } else if (token.type == PathToken::Index) {
        if (current->type == JSON_ARRAY) {
            if (token.index >= 0 && token.index < (int)current->as.list.count) {
                EvaluateJsonPathTokens(&current->as.list.items[token.index].value, tokens, token_idx + 1, paths, current_path);
            }
        }
    } else if (token.type == PathToken::Wildcard) {
        if (current->type == JSON_OBJECT || current->type == JSON_ARRAY) {
            for (size_t i = 0; i < current->as.list.count; i++) {
                EvaluateJsonPathTokens(&current->as.list.items[i].value, tokens, token_idx + 1, paths, current_path);
            }
        }
    }
    
    current_path.pop_back();
}

int main(int /*argc*/, char** /*argv*/) {
    // Prefer Wayland natively on Linux, fallback to X11
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "wayland,x11");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        return -1;
    }
    
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    AppSettings settings;
    settings.Load();

    const char* glsl_version = "#version 130";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

    Uint32 window_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    SDL_Window* window = SDL_CreateWindow("JsonLens - Dear ImGui", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, settings.window_width, settings.window_height, window_flags);
    if (settings.window_maximized) {
        SDL_MaximizeWindow(window);
    }
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    
    ImGuiStyle& style = ImGui::GetStyle();
    style.Colors[ImGuiCol_PopupBg]   = ImVec4(0.15f, 0.15f, 0.15f, 0.98f);
    style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
    style.Colors[ImGuiCol_Border]    = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    style.Colors[ImGuiCol_NavHighlight]= ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    style.PopupRounding              = 4.0f;
    style.FrameRounding              = 3.0f;

    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);

    LargeTextFile* doc = new LargeTextFile();
    char filepath_buffer[512] = "";

    io.FontGlobalScale = settings.zoom;

    std::atomic<bool> doc_loading{false};
    std::atomic<bool> doc_saving{false};
    std::atomic<bool> doc_formatting{false};
    std::atomic<LargeTextFile*> doc_ready{nullptr};
    std::thread doc_thread;

    enum class ActiveView { Text, Tree, Graph };
    ActiveView active_view = (ActiveView)settings.default_view;
    bool force_graph_tab = (settings.default_view == 2);

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
    bool force_text_tab = (settings.default_view == 0);
    bool force_tree_tab = (settings.default_view == 1);
    std::vector<JsonValue*> tree_focus_path;
    int tree_focus_frames = 0;
    JsonValue* tree_highlight_val = nullptr;
    double tree_highlight_time = 0.0;

    int error_fix_line = -1;
    std::vector<char> error_fix_buf;

    int inline_edit_line = -1;
    std::vector<char> inline_edit_buf;
    bool inline_edit_focus = false;
    bool inline_edit_needs_refresh = false;
    int inline_edit_cursor_pos = -1;

    char jsonpath_buf[256] = "";
    char jsonpath_last_buf[256] = "";
    std::vector<std::vector<JsonValue*>> jsonpath_results;
    int jsonpath_active_idx = -1;

    auto JumpToLine = [&](int line) {
        if (line < 0) return;
        scroll_to_line = line;
        scroll_to_line_frames = 3;
        highlight_line = line;
        highlight_time = ImGui::GetTime();
        force_text_tab = true;
    };

    auto JumpToMatch = [&](size_t offset) {
        if (active_view == ActiveView::Text) {
            JumpToLine(doc->GetLineFromOffset(offset));
        } else {
            std::vector<JsonValue*> current_path;
            std::vector<JsonValue*> best_path;
            std::function<void(JsonValue*)> find_closest = [&](JsonValue* val) {
                if (!val) return;
                current_path.push_back(val);
                best_path = current_path;

                if (val->type == JSON_OBJECT || val->type == JSON_ARRAY) {
                    JsonValue* best_child = nullptr;
                    for (size_t i = 0; i < val->as.list.count; ++i) {
                        JsonValue* child = &val->as.list.items[i].value;
                        if (i + 1 < val->as.list.count) {
                            size_t next_offset = val->as.list.items[i+1].value.offset;
                            size_t boundary = next_offset;
                            while (boundary > child->offset && doc->data[boundary] != ',') {
                                boundary--;
                            }
                            if (boundary <= child->offset) {
                                boundary = child->offset + (next_offset - child->offset) / 2;
                            }
                            if (offset < boundary) {
                                best_child = child;
                                break;
                            }
                        } else {
                            best_child = child;
                            break;
                        }
                    }
                    if (best_child) find_closest(best_child);
                }
                current_path.pop_back();
            };
            
            if (doc->root_json) find_closest(doc->root_json);

            if (!best_path.empty()) {
                tree_focus_path = best_path;
                tree_highlight_val = best_path.back();
                tree_highlight_time = ImGui::GetTime();
                tree_focus_frames = 3;
                
                if (active_view == ActiveView::Tree) {
                    force_tree_tab = true;
                } else if (active_view == ActiveView::Graph) {
                    force_graph_tab = true;
                    for (size_t i = 0; i < best_path.size() - 1; ++i)
                        if (best_path[i]->type == JSON_OBJECT || best_path[i]->type == JSON_ARRAY)
                            for (size_t j = 0; j < best_path[i]->as.list.count; ++j)
                                if (&best_path[i]->as.list.items[j].value == best_path[i+1])
                                    doc->graph_pagination[best_path[i]] = (j / doc->pagination_size) * doc->pagination_size;
                    doc->graph_dirty = true;
                }
            } else JumpToLine(doc->GetLineFromOffset(offset));
        }
    };

    auto LoadFile = [&](const std::string& path) {
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

    auto SaveFile = [&](const std::string& path) {
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
            bool keep_comments = settings.allow_comments;
            doc_thread = std::thread([doc, path, use_tabs, indent_size, keep_comments, &doc_saving]() {
                doc->SaveToFile(path.c_str(), use_tabs, indent_size, keep_comments);
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
            bool keep_comments = settings.allow_comments;
            doc_thread = std::thread([doc, pretty, use_tabs, indent_size, keep_comments, &doc_formatting, &search_dirty]() {
                std::string old_text(doc->data, doc->size);
                doc->is_pretty = pretty;
                doc->tree_dirty = true;
                doc->RebuildTextFromTree(use_tabs, indent_size, keep_comments);
                
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

    auto ApplyJsonPathResult = [&]() {
        if (jsonpath_active_idx >= 0 && jsonpath_active_idx < (int)jsonpath_results.size()) {
            tree_focus_path = jsonpath_results[jsonpath_active_idx];
            tree_highlight_val = tree_focus_path.back();
            tree_highlight_time = ImGui::GetTime();
            tree_focus_frames = 3;
            
            if (active_view == ActiveView::Graph) {
                for (size_t i = 0; i < tree_focus_path.size() - 1; ++i) {
                    if (tree_focus_path[i]->type == JSON_OBJECT || tree_focus_path[i]->type == JSON_ARRAY) {
                        for (size_t j = 0; j < tree_focus_path[i]->as.list.count; ++j) {
                            if (&tree_focus_path[i]->as.list.items[j].value == tree_focus_path[i+1]) {
                                doc->graph_pagination[tree_focus_path[i]] = (j / doc->pagination_size) * doc->pagination_size;
                                break;
                            }
                        }
                    }
                }
                doc->graph_dirty = true;
            }
        }
    };

    bool done = false;
    bool was_formatting = false;
    while (!done) {
        bool zoom_changed = false;
        bool current_formatting = doc_formatting.load();
        if (was_formatting && !current_formatting) {
            if (inline_edit_line >= 0) inline_edit_needs_refresh = true;
        }
        was_formatting = current_formatting;

        bool focus_search = false;
        bool focus_replace = false;
        
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) done = true;
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window)) done = true;
            
            if (event.type == SDL_DROPFILE && event.drop.file != nullptr) {
                std::string file_path = event.drop.file;
                
                // Fix for some Linux Desktop Environments improperly prepending "file://"
                if (file_path.compare(0, 7, "file://") == 0) file_path = file_path.substr(7);
                
                // Strip any trailing newlines or carriage returns
                while (!file_path.empty() && (file_path.back() == '\n' || file_path.back() == '\r')) file_path.pop_back();
                
                LoadFile(file_path);
                SDL_free(event.drop.file);
            }
        }

        if (LargeTextFile* ready_doc = doc_ready.exchange(nullptr)) {
            delete doc;
            doc = ready_doc;
            error_fix_line = -1;
            if (doc->line_offsets.size() > 0 && doc->data) {
                inline_edit_line = 0;
                size_t start = doc->line_offsets[0];
                size_t end = (1 < doc->line_offsets.size()) ? doc->line_offsets[1] : doc->size;
                size_t text_end = end;
                if (text_end > start && doc->data[text_end - 1] == '\n') text_end--;
                if (text_end > start && doc->data[text_end - 1] == '\r') text_end--;
                size_t len = text_end - start;
                inline_edit_buf.resize(len + 8192);
                memcpy(inline_edit_buf.data(), doc->data + start, len);
                inline_edit_buf[len] = '\0';
                inline_edit_focus = true;
                inline_edit_cursor_pos = 0;
            } else {
                inline_edit_line = -1;
            }
            jsonpath_last_buf[0] = '\0'; // Refresh JSONPath when file loads
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->Pos);
        ImGui::SetNextWindowSize(ImGui::GetMainViewport()->Size);
        ImGui::Begin("Main", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_MenuBar);

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
                        bool allow_comments = settings.allow_comments;
                        if (doc_thread.joinable()) doc_thread.join();
                        doc_thread = std::thread([doc, allow_comments, &doc_formatting, &search_dirty]() { doc->RebuildTreeFromText(allow_comments); doc_formatting = false; search_dirty = true; });
                    }
                }
                if (ImGui::MenuItem("Redo", "Ctrl+Y", false, has_doc && !doc->redo_stack.empty())) {
                    if (doc->Redo()) {
                        doc->ClearAstHistory();
                        doc_formatting = true;
                        bool allow_comments = settings.allow_comments;
                        if (doc_thread.joinable()) doc_thread.join();
                        doc_thread = std::thread([doc, allow_comments, &doc_formatting, &search_dirty]() { doc->RebuildTreeFromText(allow_comments); doc_formatting = false; search_dirty = true; });
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Copy All", "Ctrl+Shift+C", false, has_doc)) {
                    if (doc->tree_dirty) doc->RebuildTextFromTree(settings.use_tabs, settings.indent_size, settings.allow_comments);
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
            
            float stats_width = ImGui::CalcTextSize("Stats").x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
            float avail_width = ImGui::GetContentRegionAvail().x;
            if (avail_width > stats_width) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail_width - stats_width);
            }

            if (ImGui::BeginMenu("Stats")) {
                if (doc->data) {
                    ImGui::Text("Document");
                    ImGui::Separator();
                    ImGui::TextDisabled("File Size: %s", FormatMemory(doc->size).c_str());
                    ImGui::TextDisabled("Line Count: %zu", doc->line_offsets.empty() ? 0 : doc->line_offsets.size() - 1);
                    ImGui::TextDisabled("Max Line Length: %zu chars", doc->max_line_length);
                    ImGui::Spacing();
                    ImGui::Text("Performance");
                    ImGui::Separator();
                    ImGui::TextDisabled("Load Time: %s", FormatTime(doc->load_time_ms).c_str());
                    ImGui::TextDisabled("Parse Time: %s", FormatTime(doc->parse_time_ms).c_str());
                    ImGui::TextDisabled("Index Time: %s", FormatTime(doc->index_time_ms).c_str());
                    if (doc->format_time_ms > 0) ImGui::TextDisabled("Format Time: %s", FormatTime(doc->format_time_ms).c_str());
                    ImGui::TextDisabled("Application FPS: %.1f", ImGui::GetIO().Framerate);
                    ImGui::Spacing();
                    ImGui::Text("Memory");
                    ImGui::Separator();
                    size_t view_mem = doc->data_capacity + doc->line_offsets.capacity() * sizeof(size_t);
                    ImGui::TextDisabled("View Buffer: %s", FormatMemory(view_mem).c_str());
                    ImGui::TextDisabled("Parse Arena: %s (%zu Regions)", FormatMemory(doc->parse_memory_bytes).c_str(), GetArenaRegionCount(&doc->main_arena) + GetArenaRegionCount(&doc->scratch_arena));
                    ImGui::TextDisabled("History Memory: %s (%zu Undo, %zu Redo)", FormatMemory(doc->GetHistoryMemoryUsage()).c_str(), doc->undo_stack.size(), doc->redo_stack.size());
                    ImGui::Spacing();

                    if (doc->root_json) {
                        ImGui::Text("JSON Tree (AST)");
                        ImGui::Separator();
                        ImGui::TextDisabled("Total Nodes: %zu", doc->ast_stats.total_nodes);
                        ImGui::TextDisabled("Max Depth: %zu", doc->ast_stats.max_depth);
                        
                        if (ImGui::BeginTable("ASTBreakdown", 2)) {
                            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Objects: %zu", doc->ast_stats.objects); ImGui::TableNextColumn(); ImGui::TextDisabled("Arrays: %zu", doc->ast_stats.arrays);
                            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Strings: %zu", doc->ast_stats.strings); ImGui::TableNextColumn(); ImGui::TextDisabled("Numbers: %zu", doc->ast_stats.numbers);
                            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Booleans: %zu", doc->ast_stats.booleans); ImGui::TableNextColumn(); ImGui::TextDisabled("Nulls: %zu", doc->ast_stats.nulls);
                            ImGui::EndTable();
                        }
                    }
                } else {
                    ImGui::TextDisabled("No file loaded.");
                    ImGui::Separator();
                    ImGui::TextDisabled("Application FPS: %.1f", ImGui::GetIO().Framerate);
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

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
                if (doc->tree_dirty) doc->RebuildTextFromTree(settings.use_tabs, settings.indent_size, settings.allow_comments);
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
            
            int err_line_idx = doc->last_err.line - 1;
            if (err_line_idx >= 0 && err_line_idx < (int)doc->line_offsets.size()) {
                if (error_fix_line != err_line_idx) {
                    size_t start = doc->line_offsets[err_line_idx];
                    size_t end = (err_line_idx + 1 < (int)doc->line_offsets.size()) ? doc->line_offsets[err_line_idx + 1] : doc->size;
                    size_t text_end = end;
                    if (text_end > start && doc->data[text_end - 1] == '\n') text_end--;
                    if (text_end > start && doc->data[text_end - 1] == '\r') text_end--;
                    size_t len = text_end - start;
                    
                    error_fix_buf.resize(len + 8192); // Provide a generous 8KB buffer for typing edits
                    memcpy(error_fix_buf.data(), doc->data + start, len);
                    error_fix_buf[len] = '\0';
                    error_fix_line = err_line_idx;
                }
                
                bool apply_fix = false;
                ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Apply Fix").x - style.ItemSpacing.x * 2.0f);
                if (ImGui::InputText("##ErrorFix", error_fix_buf.data(), error_fix_buf.size(), ImGuiInputTextFlags_EnterReturnsTrue)) apply_fix = true;
                ImGui::PopItemWidth();
                ImGui::SameLine();
                if (ImGui::Button("Apply Fix")) apply_fix = true;

                if (apply_fix) {
                    doc->ReplaceLine(err_line_idx, error_fix_buf.data());
                    doc->ClearAstHistory();
                    doc_formatting = true;
                    bool allow_comments = settings.allow_comments;
                    if (doc_thread.joinable()) doc_thread.join();
                    doc_thread = std::thread([doc, allow_comments, &doc_formatting, &search_dirty]() { 
                        doc->RebuildTreeFromText(allow_comments); 
                        doc_formatting = false; 
                        search_dirty = true; 
                    });
                    error_fix_line = -1;
                }
            }
        } else {
            error_fix_line = -1;
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
            
            if (ImGui::IsItemFocused()) {
                if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
                    if (ImGui::GetIO().KeyShift) {
                        if (!search_results.empty()) {
                            search_active_idx = (search_active_idx > 0) ? search_active_idx - 1 : (int)search_results.size() - 1;
                            JumpToMatch(search_results[search_active_idx]);
                        }
                    } else {
                        if (!search_results.empty()) {
                            search_active_idx = (search_active_idx < (int)search_results.size() - 1) ? search_active_idx + 1 : 0;
                            JumpToMatch(search_results[search_active_idx]);
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
                JumpToMatch(search_results[search_active_idx]);
            }
            ImGui::SameLine();
            if (ImGui::Button("Next >") && !search_results.empty()) {
                search_active_idx = (search_active_idx < (int)search_results.size() - 1) ? search_active_idx + 1 : 0;
                JumpToMatch(search_results[search_active_idx]);
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
                JumpToMatch(search_results[search_active_idx]);
            }
            search_dirty = false;
            jsonpath_last_buf[0] = '\0'; // Refresh JSONPath when AST structure updates
        }

        if (!doc->data && !doc_loading && !doc_saving && !doc_formatting) {
            ImVec2 avail = ImGui::GetContentRegionAvail();
            ImVec2 center = ImVec2(ImGui::GetCursorScreenPos().x + avail.x * 0.5f, ImGui::GetCursorScreenPos().y + avail.y * 0.5f);
            ImVec2 box_size(450, 220);
            ImVec2 box_min = ImVec2(center.x - box_size.x * 0.5f, center.y - box_size.y * 0.5f);
            ImVec2 box_max = ImVec2(center.x + box_size.x * 0.5f, center.y + box_size.y * 0.5f);

            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(box_min, box_max, IM_COL32(35, 38, 43, 255), 12.0f);
            dl->AddRect(box_min, box_max, IM_COL32(100, 150, 200, 150), 12.0f, 0, 2.0f);

            const char* title = "No File Loaded";
            const char* sub = "Drag & Drop a .json file here";
            const char* sub2 = "or click to browse";
            
            float title_size_f = ImGui::GetFontSize() * 1.5f;
            ImVec2 title_dim = ImGui::GetFont()->CalcTextSizeA(title_size_f, FLT_MAX, -1.0f, title);
            ImVec2 sub_dim = ImGui::CalcTextSize(sub);
            ImVec2 sub2_dim = ImGui::CalcTextSize(sub2);

            float total_y = title_dim.y + sub_dim.y + sub2_dim.y + 15.0f;
            float start_y = center.y - total_y * 0.5f;

            dl->AddText(ImGui::GetFont(), title_size_f, ImVec2(center.x - title_dim.x * 0.5f, start_y), IM_COL32(230, 230, 230, 255), title);
            start_y += title_dim.y + 10.0f;
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(center.x - sub_dim.x * 0.5f, start_y), IM_COL32(150, 150, 150, 255), sub);
            start_y += sub_dim.y + 5.0f;
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(center.x - sub2_dim.x * 0.5f, start_y), IM_COL32(120, 120, 120, 255), sub2);

            ImGui::SetCursorScreenPos(box_min);
            if (ImGui::InvisibleButton("##DropZone", box_size)) {
                LoadFile(ShowOpenFileDialog(settings.last_folder));
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                dl->AddRectFilled(box_min, box_max, IM_COL32(255, 255, 255, 15), 12.0f);
            }
        } else if (ImGui::BeginTabBar("Views")) {
            ImGuiTabItemFlags tree_tab_flags = force_tree_tab ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem("Tree View", nullptr, tree_tab_flags)) {
                active_view = ActiveView::Tree;
                force_tree_tab = false;

                ImGui::AlignTextToFramePadding();
                ImGui::Text("JSONPath:");
                ImGui::SameLine();
                float input_w = ImGui::GetContentRegionAvail().x - 180.0f;
                if (input_w < 100.0f) input_w = 100.0f;
                ImGui::SetNextItemWidth(input_w);
                bool enter_pressed = ImGui::InputText("##jsonpath_tree", jsonpath_buf, sizeof(jsonpath_buf), ImGuiInputTextFlags_EnterReturnsTrue);
                if (strcmp(jsonpath_last_buf, jsonpath_buf) != 0) {
                    strncpy(jsonpath_last_buf, jsonpath_buf, sizeof(jsonpath_last_buf) - 1);
                    jsonpath_last_buf[sizeof(jsonpath_last_buf) - 1] = '\0';
                    jsonpath_results.clear();
                    jsonpath_active_idx = -1;
                    if (doc->root_json && jsonpath_buf[0] != '\0') {
                        auto tokens = ParseJsonPath(jsonpath_buf);
                        std::vector<JsonValue*> init_path;
                        EvaluateJsonPathTokens(doc->root_json, tokens, 0, jsonpath_results, init_path);
                        if (!jsonpath_results.empty()) {
                            jsonpath_active_idx = 0;
                            ApplyJsonPathResult();
                        }
                    }
                } else if (enter_pressed) {
                    if (!jsonpath_results.empty()) {
                        jsonpath_active_idx = (jsonpath_active_idx < (int)jsonpath_results.size() - 1) ? jsonpath_active_idx + 1 : 0;
                        ApplyJsonPathResult();
                    }
                }
                if (jsonpath_results.empty()) {
                    if (jsonpath_buf[0] != '\0') {
                        ImGui::SameLine();
                        ImGui::TextDisabled("0 results");
                    }
                } else {
                    ImGui::SameLine();

                    const char* prev_label = "<##jp_t";
                    const char* next_label = ">##jp_t";
                    float prev_button_w = ImGui::CalcTextSize(prev_label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
                    float next_button_w = ImGui::CalcTextSize(next_label).x + ImGui::GetStyle().FramePadding.x * 2.0f;

                    char max_text[64];
                    snprintf(max_text, sizeof(max_text), "%d / %d", (int)jsonpath_results.size(), (int)jsonpath_results.size());
                    float max_text_w = ImGui::CalcTextSize(max_text).x;

                    float group_width = max_text_w + prev_button_w + next_button_w + ImGui::GetStyle().ItemSpacing.x * 2;
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - group_width);

                    char current_text[64];
                    snprintf(current_text, sizeof(current_text), "%d / %d", jsonpath_active_idx + 1, (int)jsonpath_results.size());
                    float current_text_w = ImGui::CalcTextSize(current_text).x;
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (max_text_w - current_text_w));
                    ImGui::TextUnformatted(current_text);

                    ImGui::SameLine();
                    if (ImGui::Button(prev_label)) {
                        jsonpath_active_idx = (jsonpath_active_idx > 0) ? jsonpath_active_idx - 1 : (int)jsonpath_results.size() - 1;
                        ApplyJsonPathResult();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(next_label)) {
                        jsonpath_active_idx = (jsonpath_active_idx < (int)jsonpath_results.size() - 1) ? jsonpath_active_idx + 1 : 0;
                        ApplyJsonPathResult();
                    }
                }

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
            
            ImGuiTabItemFlags graph_tab_flags = force_graph_tab ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem("Graph View", nullptr, graph_tab_flags)) {
                active_view = ActiveView::Graph;
                force_graph_tab = false;

                ImGui::AlignTextToFramePadding();
                ImGui::Text("JSONPath:");
                ImGui::SameLine();
                float input_w = ImGui::GetContentRegionAvail().x - 180.0f;
                if (input_w < 100.0f) input_w = 100.0f;
                ImGui::SetNextItemWidth(input_w);
                bool enter_pressed = ImGui::InputText("##jsonpath_graph", jsonpath_buf, sizeof(jsonpath_buf), ImGuiInputTextFlags_EnterReturnsTrue);
                if (strcmp(jsonpath_last_buf, jsonpath_buf) != 0) {
                    strncpy(jsonpath_last_buf, jsonpath_buf, sizeof(jsonpath_last_buf) - 1);
                    jsonpath_last_buf[sizeof(jsonpath_last_buf) - 1] = '\0';
                    jsonpath_results.clear();
                    jsonpath_active_idx = -1;
                    if (doc->root_json && jsonpath_buf[0] != '\0') {
                        auto tokens = ParseJsonPath(jsonpath_buf);
                        std::vector<JsonValue*> init_path;
                        EvaluateJsonPathTokens(doc->root_json, tokens, 0, jsonpath_results, init_path);
                        if (!jsonpath_results.empty()) {
                            jsonpath_active_idx = 0;
                            ApplyJsonPathResult();
                        }
                    }
                } else if (enter_pressed) {
                    if (!jsonpath_results.empty()) {
                        jsonpath_active_idx = (jsonpath_active_idx < (int)jsonpath_results.size() - 1) ? jsonpath_active_idx + 1 : 0;
                        ApplyJsonPathResult();
                    }
                }
                if (jsonpath_results.empty()) {
                    if (jsonpath_buf[0] != '\0') {
                        ImGui::SameLine();
                        ImGui::TextDisabled("0 results");
                    }
                } else {
                    ImGui::SameLine();

                    const char* prev_label = "<##jp_g";
                    const char* next_label = ">##jp_g";
                    float prev_button_w = ImGui::CalcTextSize(prev_label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
                    float next_button_w = ImGui::CalcTextSize(next_label).x + ImGui::GetStyle().FramePadding.x * 2.0f;

                    char max_text[64];
                    snprintf(max_text, sizeof(max_text), "%d / %d", (int)jsonpath_results.size(), (int)jsonpath_results.size());
                    float max_text_w = ImGui::CalcTextSize(max_text).x;

                    float group_width = max_text_w + prev_button_w + next_button_w + ImGui::GetStyle().ItemSpacing.x * 2;
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - group_width);

                    char current_text[64];
                    snprintf(current_text, sizeof(current_text), "%d / %d", jsonpath_active_idx + 1, (int)jsonpath_results.size());
                    float current_text_w = ImGui::CalcTextSize(current_text).x;
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (max_text_w - current_text_w));
                    ImGui::TextUnformatted(current_text);

                    ImGui::SameLine();
                    if (ImGui::Button(prev_label)) {
                        jsonpath_active_idx = (jsonpath_active_idx > 0) ? jsonpath_active_idx - 1 : (int)jsonpath_results.size() - 1;
                        ApplyJsonPathResult();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(next_label)) {
                        jsonpath_active_idx = (jsonpath_active_idx < (int)jsonpath_results.size() - 1) ? jsonpath_active_idx + 1 : 0;
                        ApplyJsonPathResult();
                    }
                }

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
                        if (doc->graph_root) {
                            float current_y = 40.0f;
                            float max_x = 0.0f;
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
                        DrawGraphNodes(dl, doc->graph_root, offset, mouse_pos, &hovered_node, clip_rect, tree_highlight_val, tree_highlight_time, tree_focus_frames);
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
                                    std::reverse(tree_focus_path.begin(), tree_focus_path.end());
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
                if (tree_focus_frames > 0) {
                    tree_focus_frames--;
                } else {
                    tree_focus_path.clear();
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            ImGuiTabItemFlags text_tab_flags = force_text_tab ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem("Text View", nullptr, text_tab_flags)) {
                active_view = ActiveView::Text;
                force_text_tab = false;
                
                auto SwitchToLineEdit = [&](int line_idx, int cursor_pos = -1) {
                    if (line_idx < 0 || line_idx >= (int)doc->line_offsets.size()) return;
                    inline_edit_line = line_idx;
                    size_t start = doc->line_offsets[inline_edit_line];
                    size_t end = (inline_edit_line + 1 < (int)doc->line_offsets.size()) ? doc->line_offsets[inline_edit_line + 1] : doc->size;
                    size_t text_end = end;
                    if (text_end > start && doc->data[text_end - 1] == '\n') text_end--;
                    if (text_end > start && doc->data[text_end - 1] == '\r') text_end--;
                    size_t len = text_end - start;
                    
                    inline_edit_buf.resize(len + 8192);
                    memcpy(inline_edit_buf.data(), doc->data + start, len);
                    inline_edit_buf[len] = '\0';
                    inline_edit_focus = true;
                    
                    if (cursor_pos != -1) inline_edit_cursor_pos = cursor_pos;
                    if (inline_edit_cursor_pos > (int)len) inline_edit_cursor_pos = (int)len;
                    
                    doc->select_start = doc->select_end = (size_t)-1;
                };

                auto ApplyInlineEdit = [&]() -> bool {
                    if (inline_edit_line >= 0 && inline_edit_line < (int)doc->line_offsets.size()) {
                        size_t start = doc->line_offsets[inline_edit_line];
                        size_t end = (inline_edit_line + 1 < (int)doc->line_offsets.size()) ? doc->line_offsets[inline_edit_line + 1] : doc->size;
                        size_t text_end = end;
                        if (text_end > start && doc->data[text_end - 1] == '\n') text_end--;
                        if (text_end > start && doc->data[text_end - 1] == '\r') text_end--;
                        std::string original_text(doc->data + start, text_end - start);
                        if (strcmp(inline_edit_buf.data(), original_text.c_str()) != 0) {
                            doc->ReplaceLine(inline_edit_line, inline_edit_buf.data());
                            doc->ClearAstHistory();
                            doc_formatting = true;
                            bool allow_comments = settings.allow_comments;
                            if (doc_thread.joinable()) doc_thread.join();
                            doc_thread = std::thread([doc, allow_comments, &doc_formatting, &search_dirty]() { 
                                doc->RebuildTreeFromText(allow_comments); 
                                doc_formatting = false; 
                                search_dirty = true; 
                            });
                            return true;
                        }
                    }
                    return false;
                };

                ImGui::BeginChild("TextChild", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
                // If the DOM was edited, trigger the text regeneration in a background thread
                if (doc->tree_dirty && !doc_loading && !doc_saving && !doc_formatting) {
                    doc->ClearTextHistory();
                    doc_formatting = true;
                    bool use_tabs = settings.use_tabs;
                    int indent_size = settings.indent_size;
                    bool keep_comments = settings.allow_comments;
                    if (doc_thread.joinable()) doc_thread.join();
                    doc_thread = std::thread([doc, use_tabs, indent_size, keep_comments, &doc_formatting, &search_dirty]() {
                        doc->RebuildTextFromTree(use_tabs, indent_size, keep_comments);
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
                    if (inline_edit_needs_refresh) {
                        SwitchToLineEdit(inline_edit_line);
                        inline_edit_needs_refresh = false;
                    }

                    if (doc->data) {
                        ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]); // Ensure monospace if loaded
                        
                        float exact_item_height = font_size + exact_item_spacing_y;
                        float char_width = ImGui::CalcTextSize("A").x;
                        ImVec2 text_start_pos = ImGui::GetCursorScreenPos();
                        
                        auto GetOffsetFromPos = [&](ImVec2 pos) -> size_t {
                            if (doc->line_offsets.empty()) return 0;
                            
                            double local_y = (double)pos.y - (double)text_start_pos.y;
                            int raw_line_idx = (int)(local_y / (double)exact_item_height);
                            
                            if (raw_line_idx < 0) raw_line_idx = 0;
                            size_t line_idx = (size_t)raw_line_idx;
                            if (line_idx >= doc->line_offsets.size()) line_idx = doc->line_offsets.size() - 1;
                            size_t line_start = doc->line_offsets[line_idx];
                            size_t line_end = (line_idx + 1 < doc->line_offsets.size()) ? doc->line_offsets[line_idx + 1] : doc->size;
                            if (line_end > line_start && doc->data[line_end - 1] == '\n') line_end--;
                            if (line_end > line_start && doc->data[line_end - 1] == '\r') line_end--;
                            
                            float local_x = pos.x - text_start_pos.x;
                            if (local_x <= 0.0f) return line_start;
                            
                            const char* s = doc->data + line_start;
                            const char* end = doc->data + line_end;
                            float current_x = 0.0f;
                            
                            const ImFont* font = ImGui::GetFont();
                            const float scale = font_size / font->FontSize;
                            
                            while (s < end) {
                                unsigned int c = (unsigned int)(unsigned char)*s;
                                int bytes = 1;
                                if (c >= 0x80) bytes = ImTextCharFromUtf8(&c, s, end);
                                
                                if (c != '\r') {
                                    float c_width = font->GetCharAdvance((ImWchar)c) * scale;
                                    if (current_x + c_width * 0.5f > local_x) break;
                                    current_x += c_width;
                                }
                                s += bytes;
                            }
                            return (size_t)(s - doc->data);
                        };

                        auto GetOffsetFromLineAndX = [&](int line_idx, float screen_x) -> size_t {
                            if (line_idx < 0 || line_idx >= (int)doc->line_offsets.size()) return 0;
                            ImVec2 fake_pos(screen_x, text_start_pos.y + (float)line_idx * exact_item_height + exact_item_height * 0.5f);
                            return GetOffsetFromPos(fake_pos);
                        };

                        auto GetOffsetFromMouse = [&]() -> size_t {
                            return GetOffsetFromPos(ImGui::GetMousePos());
                        };

                        bool is_mouse_over_scrollbars = false;
                        ImVec2 mouse_pos = ImGui::GetMousePos();
                        ImVec2 win_pos = ImGui::GetWindowPos();
                        ImVec2 win_size = ImGui::GetWindowSize();
                        float scrollbar_size = ImGui::GetStyle().ScrollbarSize;
                        if (ImGui::GetScrollMaxY() > 0.0f && mouse_pos.x >= win_pos.x + win_size.x - scrollbar_size) {
                            is_mouse_over_scrollbars = true;
                        }
                        if (ImGui::GetScrollMaxX() > 0.0f && mouse_pos.y >= win_pos.y + win_size.y - scrollbar_size) {
                            is_mouse_over_scrollbars = true;
                        }

                        static bool is_selecting_text = false;
                        
                        if (ImGui::IsMouseReleased(0) && is_selecting_text) {
                            if (doc->select_start == doc->select_end) {
                                size_t offset = doc->select_start;
                                int clicked_line = doc->GetLineFromOffset(offset);
                                int cursor_pos = (int)(offset - doc->line_offsets[clicked_line]);
                                if (inline_edit_line != clicked_line) {
                                    if (!inline_edit_needs_refresh) {
                                        SwitchToLineEdit(clicked_line, cursor_pos);
                                    }
                                } else {
                                    inline_edit_focus = true;
                                    inline_edit_cursor_pos = cursor_pos;
                                }
                            }
                            is_selecting_text = false;
                        }

                        if (!ImGui::IsMouseDown(0)) {
                            is_selecting_text = false;
                        }

                        if (ImGui::IsWindowHovered() && !is_mouse_over_scrollbars && !ImGui::IsAnyItemHovered()) {
                            if (ImGui::IsMouseClicked(0)) {
                                size_t offset = GetOffsetFromMouse();
                                int clicked_line = doc->GetLineFromOffset(offset);
                                doc->select_start = doc->select_end = offset;
                                is_selecting_text = true;
                                int cursor_pos = (int)(offset - doc->line_offsets[clicked_line]);

                                if (inline_edit_line != -1 && inline_edit_line != clicked_line) {
                                    if (ApplyInlineEdit()) {
                                        inline_edit_line = clicked_line;
                                        inline_edit_needs_refresh = true;
                                        inline_edit_cursor_pos = cursor_pos;
                                    } else {
                                        inline_edit_line = -1;
                                    }
                                }
                            }
                        } else if (ImGui::IsMouseClicked(0)) {
                            is_selecting_text = false;
                        }
                        
                        if (ImGui::IsMouseDown(0) && is_selecting_text) {
                            doc->select_end = GetOffsetFromMouse();
                            if (doc->select_start != doc->select_end && inline_edit_line != -1) {
                                ApplyInlineEdit();
                                inline_edit_line = -1;
                                inline_edit_needs_refresh = false;
                            }
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

                                if (inline_edit_line == (int)i) {
                                    ImGui::PushID((int)i);
                                    ImGui::PushItemWidth(-1.0f);
                                    if (inline_edit_focus) {
                                        ImGui::SetKeyboardFocusHere();
                                        inline_edit_focus = false;
                                    }
                                    
                                    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
                                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                                    
                                    struct EditState {
                                        int* p_cursor;
                                        int current_cursor;
                                    };
                                    EditState edit_state;
                                    edit_state.p_cursor = &inline_edit_cursor_pos;
                                    edit_state.current_cursor = 0;

                                    ImVec4 orig_text_col = ImGui::GetStyleColorVec4(ImGuiCol_Text);
                                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0)); // Transparent text for input

                                    ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways;
                                    bool enter_pressed = ImGui::InputText("##inline_edit", inline_edit_buf.data(), inline_edit_buf.size(), flags,
                                        [](ImGuiInputTextCallbackData* data) -> int {
                                            EditState* s = static_cast<EditState*>(data->UserData);
                                            if (*s->p_cursor >= 0) {
                                                data->CursorPos = *s->p_cursor;
                                                data->SelectionStart = *s->p_cursor;
                                                data->SelectionEnd = *s->p_cursor;
                                                *s->p_cursor = -1;
                                            }
                                            s->current_cursor = data->CursorPos;
                                            return 0;
                                        }, &edit_state);
                                    bool deactivated = ImGui::IsItemDeactivated();
                                    
                                    ImVec2 item_min = ImGui::GetItemRectMin();
                                    ImVec2 item_max = ImGui::GetItemRectMax();

                                    bool is_active = ImGui::IsItemActive();
                                    bool drag_outside = false;
                                    if (is_active && ImGui::IsMouseDragging(0)) {
                                        ImVec2 mouse_pos = ImGui::GetMousePos();
                                        if (mouse_pos.y < item_min.y || mouse_pos.y > item_max.y) {
                                            drag_outside = true;
                                        }
                                    }
                                    
                                    bool move_up = ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_UpArrow);
                                    bool move_down = ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_DownArrow);
                                    
                                    ImGui::PopStyleColor(); // Pop Transparent Text

                                    // Render custom highlighted text
                                    ImGuiID id = ImGui::GetID("##inline_edit");
                                    ImGuiInputTextState* state = ImGui::GetInputTextState(id);
                                    float scroll_x = state ? state->ScrollX : 0.0f;
                                    
                                    ImDrawList* draw_list = ImGui::GetWindowDrawList();
                                    draw_list->PushClipRect(item_min, ImVec2(item_max.x, item_max.y + ImGui::GetFontSize() * 0.3f), true);
                                    
                                    ImVec2 text_pos = ImVec2(item_min.x - scroll_x, item_min.y);
                                    
                                    const char* p = inline_edit_buf.data();
                                    const char* end_ptr = p + strlen(p);
                                    ImVec2 current_pos = text_pos;

                                    ImVec4 col_str(0.80f, 0.53f, 0.35f, 1.0f);
                                    ImVec4 col_key(0.61f, 0.86f, 0.99f, 1.0f);
                                    ImVec4 col_num(0.71f, 0.81f, 0.66f, 1.0f);
                                    ImVec4 col_bool(0.34f, 0.61f, 0.84f, 1.0f);
                                    ImVec4 col_punc(0.60f, 0.60f, 0.60f, 1.0f);
                                    ImVec4 col_comm(0.40f, 0.70f, 0.40f, 1.0f);

                                    const ImFont* font = ImGui::GetFont();
                                    float local_font_size = ImGui::GetFontSize();

                                    while (p < end_ptr) {
                                        const char* token_start = p;
                                        ImVec4 col = orig_text_col;
                                        
                                        if (isspace(*p)) {
                                            while (p < end_ptr && isspace(*p)) p++;
                                        } else if (*p == '"') {
                                            p++;
                                            while (p < end_ptr) {
                                                if (*p == '\\' && p + 1 < end_ptr) p += 2;
                                                else if (*p == '"') { p++; break; }
                                                else p++;
                                            }
                                            bool is_obj_key = false;
                                            const char* peek = p;
                                            while (peek < end_ptr && isspace(*peek)) peek++;
                                            if (peek < end_ptr && *peek == ':') is_obj_key = true;
                                            col = is_obj_key ? col_key : col_str;
                                        } else if (isdigit(*p) || *p == '-') {
                                            col = col_num;
                                            while (p < end_ptr && (isalnum(*p) || *p == '.' || *p == '+' || *p == '-')) p++;
                                        } else if (isalpha(*p)) {
                                            col = col_bool;
                                            while (p < end_ptr && isalpha(*p)) p++;
                                        } else if (*p == '/' && p + 1 < end_ptr) {
                                            if (*(p+1) == '/') {
                                                col = col_comm;
                                                p = end_ptr;
                                            } else if (*(p+1) == '*') {
                                                col = col_comm;
                                                const char* end_comment = p + 2;
                                                while (end_comment < end_ptr) {
                                                    if (*end_comment == '*' && (end_comment + 1 < end_ptr) && *(end_comment + 1) == '/') {
                                                        p = end_comment + 2;
                                                        break;
                                                    }
                                                    end_comment++;
                                                }
                                                if (end_comment >= end_ptr) { p = end_ptr; }
                                            } else { col = col_punc; p++; }
                                        } else {
                                            col = col_punc;
                                            p++;
                                        }
                                        
                                        if (p > token_start) {
                                            draw_list->AddText(font, local_font_size, current_pos, ImGui::ColorConvertFloat4ToU32(col), token_start, p);
                                            current_pos.x += font->CalcTextSizeA(local_font_size, FLT_MAX, -1.0f, token_start, p, NULL).x;
                                        }
                                    }

                                    if (ImGui::IsItemFocused()) {
                                        static double last_cursor_time = 0.0;
                                        static int last_cursor_pos_track = -1;
                                        if (last_cursor_pos_track != edit_state.current_cursor) {
                                            last_cursor_pos_track = edit_state.current_cursor;
                                            last_cursor_time = ImGui::GetTime();
                                        }
                                        if (fmod(ImGui::GetTime() - last_cursor_time, 1.0) < 0.5) {
                                            float cursor_x = font->CalcTextSizeA(local_font_size, FLT_MAX, -1.0f, inline_edit_buf.data(), inline_edit_buf.data() + edit_state.current_cursor, NULL).x;
                                            ImVec2 p1 = ImVec2(text_pos.x + cursor_x, text_pos.y);
                                            ImVec2 p2 = ImVec2(p1.x, p1.y + local_font_size);
                                            draw_list->AddLine(p1, p2, ImGui::ColorConvertFloat4ToU32(orig_text_col), 1.0f);
                                        }
                                    }

                                    draw_list->PopClipRect();

                                    ImGui::PopStyleVar();
                                    ImGui::PopStyleColor();
                                    ImGui::PopItemWidth();
                                    ImGui::PopID();
                                    
                                    if (drag_outside) {
                                        int clicked_line = inline_edit_line;
                                        ApplyInlineEdit();
                                        inline_edit_line = -1;
                                        inline_edit_needs_refresh = false;
                                        is_selecting_text = true;
                                        doc->select_start = GetOffsetFromLineAndX(clicked_line, ImGui::GetIO().MouseClickedPos[0].x);
                                        doc->select_end = GetOffsetFromMouse();
                                    } else if (enter_pressed || move_up || move_down) {
                                        int target_line = inline_edit_line;
                                        if (move_up && inline_edit_line > 0) target_line--;
                                        else if ((move_down || enter_pressed) && inline_edit_line < (int)doc->line_offsets.size() - 1) target_line++;
                                        
                                        if (move_up || move_down) {
                                            inline_edit_cursor_pos = edit_state.current_cursor;
                                        } else {
                                            inline_edit_cursor_pos = 0;
                                        }

                                        if (ApplyInlineEdit()) {
                                            inline_edit_line = target_line;
                                            inline_edit_needs_refresh = true;
                                        } else {
                                            SwitchToLineEdit(target_line);
                                        }
                                    } else if (deactivated) {
                                        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                                            inline_edit_line = -1;
                                        } else {
                                            ApplyInlineEdit();
                                        }
                                    }
                                } else {
                                    const char* p = doc->data + start;
                                    const char* end_ptr = doc->data + end;
                                    bool first = true;
                                    int token_count = 0;
                                    
                                    ImVec4 col_str(0.80f, 0.53f, 0.35f, 1.0f);   // Orange
                                    ImVec4 col_key(0.61f, 0.86f, 0.99f, 1.0f);   // Light Blue
                                    ImVec4 col_num(0.71f, 0.81f, 0.66f, 1.0f);   // Pale Green
                                    ImVec4 col_bool(0.34f, 0.61f, 0.84f, 1.0f);  // Blue
                                    ImVec4 col_punc(0.60f, 0.60f, 0.60f, 1.0f);  // Gray
                                    ImVec4 col_comm(0.40f, 0.70f, 0.40f, 1.0f);  // Dark Green
                                    
                                    while (p < end_ptr) {
                                        const char* token_start = p;
                                        ImVec4 col = ImGui::GetStyleColorVec4(ImGuiCol_Text);
                                        
                                        if (token_count++ > 2000) {
                                            // Safeguard against freezing ImGui on massive single-line minified files
                                            if (!first) ImGui::SameLine(0.0f, 0.0f);
                                            ImGui::TextUnformatted(token_start, end_ptr);
                                            break;
                                        }
                                        
                                        if (isspace(*p)) {
                                            while (p < end_ptr && isspace(*p)) p++;
                                        } else if (*p == '"') {
                                            p++;
                                            while (p < end_ptr) {
                                                if (*p == '\\' && p + 1 < end_ptr) p += 2;
                                                else if (*p == '"') { p++; break; }
                                                else p++;
                                            }
                                            bool is_obj_key = false;
                                            const char* peek = p;
                                            while (peek < end_ptr && isspace(*peek)) peek++;
                                            if (peek < end_ptr && *peek == ':') is_obj_key = true;
                                            col = is_obj_key ? col_key : col_str;
                                        } else if (isdigit(*p) || *p == '-') {
                                            col = col_num;
                                            while (p < end_ptr && (isalnum(*p) || *p == '.' || *p == '+' || *p == '-')) p++;
                                        } else if (isalpha(*p)) {
                                            col = col_bool;
                                            while (p < end_ptr && isalpha(*p)) p++;
                                        } else if (*p == '/' && p + 1 < end_ptr) {
                                            if (*(p+1) == '/') {
                                                col = col_comm;
                                                p = end_ptr; // Rest of line is a comment
                                            } else if (*(p+1) == '*') {
                                                col = col_comm;
                                                const char* end_comment = p + 2;
                                                while (end_comment < end_ptr) {
                                                    if (*end_comment == '*' && (end_comment + 1 < end_ptr) && *(end_comment + 1) == '/') {
                                                        p = end_comment + 2;
                                                        break;
                                                    }
                                                    end_comment++;
                                                }
                                                if (end_comment >= end_ptr) { p = end_ptr; }
                                            } else { col = col_punc; p++; }
                                        } else {
                                            col = col_punc;
                                            p++;
                                        }
                                        
                                        if (!first) ImGui::SameLine(0.0f, 0.0f);
                                        ImGui::PushStyleColor(ImGuiCol_Text, col);
                                        ImGui::TextUnformatted(token_start, p);
                                        ImGui::PopStyleColor();
                                        first = false;
                                    }
                                    
                                    if (first) ImGui::TextUnformatted("");
                                }
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

        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.1f, 0.1f, 0.1f, 1.00f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    settings.default_view = (int)active_view;
    Uint32 current_flags = SDL_GetWindowFlags(window);
    settings.window_maximized = (current_flags & SDL_WINDOW_MAXIMIZED) != 0;
    if (!settings.window_maximized) {
        SDL_GetWindowSize(window, &settings.window_width, &settings.window_height);
    }
    settings.Save();

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