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

constexpr const char* APP_TITLE = "JsonLens v0.1.2-alpha";

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
    SDL_Window* window = SDL_CreateWindow(APP_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, settings.window_width, settings.window_height, window_flags);
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
    std::atomic<bool> doc_formatting_done{false};
    std::atomic<bool> doc_graph_building{false};
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

    int inline_edit_line = -1;
    std::vector<char> inline_edit_buf;
    bool inline_edit_needs_refresh = false;
    int inline_edit_cursor_pos = -1;
    bool inline_edit_dirty = false;
    bool text_dirty = false;

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
        settings.show_text_view = true;
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
                    settings.show_tree_view = true;
                    force_tree_tab = true;
                } else if (active_view == ActiveView::Graph) {
                    settings.show_graph_view = true;
                    force_graph_tab = true;
                    doc->graph_dirty = true;
                }
            } else JumpToLine(doc->GetLineFromOffset(offset));
        }
    };

    auto SwitchToLineEdit = [&](int line_idx, int cursor_pos = -1) {
        if (doc->line_offsets.empty()) return;
        if (line_idx < 0) line_idx = 0;
        if (line_idx >= (int)doc->line_offsets.size()) line_idx = (int)doc->line_offsets.size() - 1;
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
        
        if (cursor_pos != -1) inline_edit_cursor_pos = cursor_pos;
        if (inline_edit_cursor_pos > (int)len) inline_edit_cursor_pos = (int)len;
        
        inline_edit_dirty = false;
        doc->select_start = doc->select_end = (size_t)-1;
    };

    auto ApplyInlineEdit = [&]() -> bool {
        if (inline_edit_line >= 0 && inline_edit_line < (int)doc->line_offsets.size() && inline_edit_dirty) {
            size_t start = doc->line_offsets[inline_edit_line];
            size_t end = (inline_edit_line + 1 < (int)doc->line_offsets.size()) ? doc->line_offsets[inline_edit_line + 1] : doc->size;
            size_t text_end = end;
            if (text_end > start && doc->data[text_end - 1] == '\n') text_end--;
            if (text_end > start && doc->data[text_end - 1] == '\r') text_end--;
            std::string original_text(doc->data + start, text_end - start);
            if (strcmp(inline_edit_buf.data(), original_text.c_str()) != 0) {
                doc->ReplaceLine(inline_edit_line, inline_edit_buf.data());
                doc->ClearAstHistory();
                text_dirty = true;
                inline_edit_dirty = false;
                return true;
            }
        }
        return false;
    };

    auto PerformUndo = [&]() {
        ApplyInlineEdit();
        if (doc->Undo()) {
            doc->ClearAstHistory();
            text_dirty = true;
            inline_edit_line = -1;
            if (!doc->redo_stack.empty() && doc->redo_stack.back().action == UndoActionType::TextReplace && !doc->redo_stack.back().text_deltas.empty()) {
                doc->select_start = doc->redo_stack.back().text_deltas.front().pos;
                doc->select_end = doc->select_start + doc->redo_stack.back().text_deltas.front().old_str.length();
            } else {
                doc->select_start = doc->select_end = (size_t)-1;
            }
        }
    };

    auto PerformRedo = [&]() {
        ApplyInlineEdit();
        if (doc->Redo()) {
            doc->ClearAstHistory();
            text_dirty = true;
            inline_edit_line = -1;
            if (!doc->undo_stack.empty() && doc->undo_stack.back().action == UndoActionType::TextReplace && !doc->undo_stack.back().text_deltas.empty()) {
                doc->select_start = doc->undo_stack.back().text_deltas.front().pos;
                doc->select_end = doc->select_start + doc->undo_stack.back().text_deltas.front().new_str.length();
            } else {
                doc->select_start = doc->select_end = (size_t)-1;
            }
        }
    };

    auto LoadFile = [&](std::string path) {
        if (!path.empty() && !doc_loading && !doc_saving && !doc_formatting) {
            snprintf(filepath_buffer, sizeof(filepath_buffer), "%s", path.c_str());
            char title[1024];
            snprintf(title, sizeof(title), "%s - %s", APP_TITLE, filepath_buffer);
            SDL_SetWindowTitle(window, title);
            settings.AddRecentFile(path);
            
            size_t pos = path.find_last_of("/\\");
            if (pos != std::string::npos) {
                settings.last_folder = path.substr(0, pos);
                settings.Save();
            }

            settings.show_text_view = true;
            settings.show_tree_view = false;
            settings.show_graph_view = false;
            force_text_tab = true;

            doc_loading = true;
            if (doc_thread.joinable()) doc_thread.join();
            
            bool allow_comments = settings.allow_comments;
            doc_thread = std::thread([path, allow_comments, &doc_ready, &doc_loading]() {
                LargeTextFile* new_doc = new LargeTextFile();
                new_doc->Load(path.c_str(), allow_comments, true);
                doc_ready = new_doc;
                doc_loading = false;
            });
        }
    };

    auto SaveFile = [&](std::string path) {
        if (!path.empty() && !doc_loading && !doc_saving && !doc_formatting && doc->data) {
            snprintf(filepath_buffer, sizeof(filepath_buffer), "%s", path.c_str());
            char title[1024];
            snprintf(title, sizeof(title), "%s - %s", APP_TITLE, filepath_buffer);
            SDL_SetWindowTitle(window, title);
            settings.AddRecentFile(path);

            ApplyInlineEdit();

            doc_saving = true;
            if (doc_thread.joinable()) doc_thread.join();
            
            bool use_tabs = settings.use_tabs;
            int indent_size = settings.indent_size;
            bool keep_comments = settings.allow_comments;
            bool need_ast_rebuild = text_dirty;
            text_dirty = false;
            doc_thread = std::thread([doc, path, use_tabs, indent_size, keep_comments, need_ast_rebuild, &doc_saving, &search_dirty]() {
                if (need_ast_rebuild) {
                    doc->RebuildTreeFromText(keep_comments);
                    search_dirty = true;
                }
                doc->SaveToFile(path.c_str(), use_tabs, indent_size, keep_comments);
                doc_saving = false;
            });
        }
    };

    auto FormatFile = [&](bool pretty) {
        if (!doc_loading && !doc_saving && !doc_formatting && doc->data) {
            if (doc->root_json == nullptr && doc->last_err.msg[0] != '\0') return;
            ApplyInlineEdit();
            doc_formatting = true;
            if (doc_thread.joinable()) doc_thread.join();
            
            bool use_tabs = settings.use_tabs;
            int indent_size = settings.indent_size;
            bool keep_comments = settings.allow_comments;
            bool need_ast_rebuild = text_dirty;
            text_dirty = false;
            doc_thread = std::thread([doc, pretty, use_tabs, indent_size, keep_comments, need_ast_rebuild, &doc_formatting, &doc_formatting_done, &search_dirty]() {
                if (doc->root_json == nullptr || need_ast_rebuild) {
                    doc->RebuildTreeFromText(keep_comments);
                }
                if (doc->root_json) {
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
                }
                doc_formatting = false;
                doc_formatting_done = true;
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
                doc->graph_dirty = true;
            }
        }
    };

    // Lambda to re-parse the current text data in memory
    auto RecheckParseError = [&]() {
        if (!doc_loading && !doc_saving && !doc_formatting && doc->data) {
            ApplyInlineEdit(); // Ensure any typed fixes are applied to doc->data
            text_dirty = false; // Prevent redundant rebuilds since we are doing it right now
            doc_formatting = true; // Use doc_formatting to indicate background work
            if (doc_thread.joinable()) doc_thread.join();
            bool allow_comments = settings.allow_comments;
            doc_thread = std::thread([doc, allow_comments, &doc_formatting, &doc_formatting_done, &search_dirty]() {
                doc->last_err = JsonError{}; // Clear previous error before re-parsing
                doc->RebuildTreeFromText(allow_comments);
                doc_formatting = false;
                doc_formatting_done = true;
                search_dirty = true; // Re-evaluate search results after potential text changes
            });
        }
    };

    bool done = false;
    while (!done) {
        bool zoom_changed = false;
        
        if (doc_formatting_done.exchange(false)) {
            if (inline_edit_line >= 0) inline_edit_needs_refresh = true;
        }

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
            bool has_doc = (doc->data != nullptr) && !doc_loading && !doc_saving && !doc_formatting && !doc_graph_building;
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
                    SDL_SetWindowTitle(window, APP_TITLE);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Quit", "Alt+F4")) done = true;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Edit")) {
            bool has_doc = (doc->data != nullptr) && !doc_loading && !doc_saving && !doc_formatting && !doc_graph_building;
                if (ImGui::MenuItem("Undo", "Ctrl+Z", false, has_doc && !doc->undo_stack.empty())) {
                    PerformUndo();
                }
                if (ImGui::MenuItem("Redo", "Ctrl+Y", false, has_doc && !doc->redo_stack.empty())) {
                    PerformRedo();
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
                ImGui::MenuItem("Text View", nullptr, &settings.show_text_view);
                ImGui::MenuItem("Tree View", nullptr, &settings.show_tree_view);
                ImGui::MenuItem("Graph View", nullptr, &settings.show_graph_view);
                ImGui::Separator();
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
                ImGui::EndMenu();
            }
            
            float stats_width = ImGui::CalcTextSize("Stats").x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
            float avail_width = ImGui::GetContentRegionAvail().x;
            if (avail_width > stats_width) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail_width - stats_width);
            }

            if (ImGui::BeginMenu("Stats")) {
                if (doc_loading || doc_saving || doc_formatting || doc_graph_building) {
                    ImGui::TextDisabled("Processing... Please wait.");
                    ImGui::Separator();
                    ImGui::TextDisabled("Application FPS: %.1f", ImGui::GetIO().Framerate);
                } else if (doc->data) {
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
                    
                    size_t text_buffer_mem = doc->data_capacity + doc->line_offsets.capacity() * sizeof(size_t);
                    size_t history_mem = doc->GetHistoryMemoryUsage();
                    size_t text_mem = text_buffer_mem + history_mem;
                    size_t tree_mem = doc->parse_memory_bytes;
                    size_t graph_mem = doc->graph_memory_bytes;
                    size_t total_mem = text_mem + tree_mem + graph_mem + sizeof(LargeTextFile);

                    ImGui::TextDisabled("Text View: %s", FormatMemory(text_mem).c_str());
                    ImGui::Indent();
                    ImGui::TextDisabled("- Buffer & Offsets: %s", FormatMemory(text_buffer_mem).c_str());
                    ImGui::TextDisabled("- History: %s (%zu Undo, %zu Redo)", FormatMemory(history_mem).c_str(), doc->undo_stack.size(), doc->redo_stack.size());
                    ImGui::Unindent();

                    ImGui::TextDisabled("Tree View (AST): %s", FormatMemory(tree_mem).c_str());
                    ImGui::Indent();
                    ImGui::TextDisabled("- Parse Arena: %s (%zu Regions)", FormatMemory(doc->parse_memory_bytes).c_str(), GetArenaRegionCount(&doc->main_arena) + GetArenaRegionCount(&doc->scratch_arena));
                    ImGui::Unindent();

                    ImGui::TextDisabled("Graph View: %s", FormatMemory(graph_mem).c_str());
                    ImGui::Separator();
                    ImGui::TextDisabled("Total App Memory: %s", FormatMemory(total_mem).c_str());
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
        
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S) && !doc_loading && !doc_saving && !doc_formatting && !doc_graph_building && doc->data) {
            if (ImGui::GetIO().KeyShift) {
                SaveFile(ShowSaveFileDialog());
            } else if (filepath_buffer[0] != '\0') {
                SaveFile(filepath_buffer);
            } else {
                SaveFile(ShowSaveFileDialog());
            }
        }
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_W) && !doc_loading && !doc_saving && !doc_formatting && !doc_graph_building && doc->data) {
            delete doc;
            doc = new LargeTextFile();
            filepath_buffer[0] = '\0';
            SDL_SetWindowTitle(window, APP_TITLE);
        }

    if (ImGui::GetIO().KeyCtrl && ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_C)) {
        if (!doc_loading && !doc_saving && !doc_formatting && !doc_graph_building && doc->data) {
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
        
        bool can_global_search = !doc_loading && !doc_saving && !doc_formatting && !doc_graph_building && doc->data;
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
        
        bool can_global_undo = !doc_loading && !doc_saving && !doc_formatting && !doc_graph_building && doc->data && !ImGui::IsAnyItemActive();
        if (can_global_undo && ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)) {
            PerformUndo();
        }
        if (can_global_undo && ImGui::GetIO().KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Y) || (ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)))) {
            PerformRedo();
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

        if (!doc_loading && !doc_saving && !doc_formatting && doc->root_json == nullptr && doc->last_err.msg[0] != '\0') {
            if (doc->data != nullptr) {
                ImGui::TextColored(ImVec4(1, 0, 0, 1), "JSON Parse Error at Line %d, Col %d: %s", doc->last_err.line, doc->last_err.col, doc->last_err.msg);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    if (ImGui::IsMouseClicked(0)) {
                        JumpToLine(doc->last_err.line - 1);
                        
                        // Select the character causing the error and place the cursor exactly there
                        doc->select_start = doc->last_err.offset;
                        doc->select_end = doc->last_err.offset < doc->size ? doc->last_err.offset + 1 : doc->last_err.offset;
                        SwitchToLineEdit(doc->last_err.line - 1, doc->last_err.col - 1);
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Re-check")) {
                    RecheckParseError();
                }
            } else {
                ImGui::TextColored(ImVec4(1, 0, 0, 1), "File Error: %s", doc->last_err.msg);
            }
        }

        if (show_search_bar && doc->data && !doc_loading && !doc_saving && !doc_formatting && !doc_graph_building) {
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
                ApplyInlineEdit();
                doc->ReplaceCurrent(search_buf, replace_buf, search_results, search_active_idx);
                doc->ClearAstHistory();
                text_dirty = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Replace All") && !search_results.empty() && doc->data) {
                ApplyInlineEdit();
                doc->ReplaceAll(search_buf, replace_buf, search_results);
                doc->ClearAstHistory();
                text_dirty = true;
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
        } else {
            bool needs_ast = (settings.show_tree_view || settings.show_graph_view);
            if (!needs_ast && doc->root_json != nullptr && !doc->tree_dirty && !doc_loading && !doc_saving && !doc_formatting) {
                doc->FreeAST();
            }

            if (needs_ast && doc->data && doc->root_json == nullptr && doc->last_err.msg[0] == '\0' && !doc_loading && !doc_saving && !doc_formatting) {
                doc_formatting = true;
                bool allow_comments = settings.allow_comments;
                if (doc_thread.joinable()) doc_thread.join();
                doc_thread = std::thread([doc, allow_comments, &doc_formatting, &doc_formatting_done, &search_dirty]() { 
                    doc->RebuildTreeFromText(allow_comments); 
                    doc_formatting = false; 
                    doc_formatting_done = true;
                    search_dirty = true; 
                });
            }

            if (ImGui::BeginTabBar("Views")) {
                ImGuiTabItemFlags text_tab_flags = force_text_tab ? ImGuiTabItemFlags_SetSelected : 0;
                if (settings.show_text_view && ImGui::BeginTabItem("Text View", &settings.show_text_view, text_tab_flags)) {
                    active_view = ActiveView::Text;
                    force_text_tab = false;
                    
                    auto DeleteSelection = [&]() -> bool {
                        if (doc->select_start != (size_t)-1 && doc->select_start != doc->select_end) {
                            ApplyInlineEdit();
                            size_t s_start = std::min(doc->select_start, doc->select_end);
                            size_t s_end = std::max(doc->select_start, doc->select_end);
                            doc->ReplaceText(s_start, s_end, "");
                            doc->ClearAstHistory();
                            text_dirty = true;
                            
                            int new_line = doc->GetLineFromOffset(s_start);
                            int new_col = s_start - doc->line_offsets[new_line];
                            
                            SwitchToLineEdit(new_line, new_col);
                            return true;
                        }
                        return false;
                    };

                    ImGui::BeginChild("TextChild", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNavInputs);
                    // If the DOM was edited, trigger the text regeneration in a background thread
                    if (doc->tree_dirty && !doc_loading && !doc_saving && !doc_formatting) {
                        doc->ClearTextHistory();
                        doc_formatting = true;
                        bool use_tabs = settings.use_tabs;
                        int indent_size = settings.indent_size;
                        bool keep_comments = settings.allow_comments;
                        if (doc_thread.joinable()) doc_thread.join();
                        doc_thread = std::thread([doc, use_tabs, indent_size, keep_comments, &doc_formatting, &doc_formatting_done, &search_dirty]() {
                            doc->RebuildTextFromTree(use_tabs, indent_size, keep_comments);
                            doc_formatting = false;
                            doc_formatting_done = true;
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
                            
                            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, 0.0f));
                            float exact_item_height = font_size;
                            float char_width = ImGui::CalcTextSize("A").x;
                            float start_padding_x = ImGui::GetCursorPosX();
                            float start_padding_y = ImGui::GetCursorPosY();
                            ImVec2 text_start_pos = ImGui::GetCursorScreenPos();

                            auto GetOffsetFromPos = [&](ImVec2 pos) -> size_t {
                                if (doc->line_offsets.empty()) return 0;
                                
                                double scroll_y = (double)ImGui::GetScrollY();
                                double window_base_y = (double)ImGui::GetWindowPos().y + (double)start_padding_y;
                                double local_y = (double)pos.y - window_base_y + scroll_y;
                                
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

                            auto GetOffsetFromMouse = [&]() -> size_t {
                                return GetOffsetFromPos(ImGui::GetMousePos());
                            };

                            auto ScrollToKeepCursorVisible = [&](int line, int cursor_pos) {
                                if (line < 0) return;
                                        double line_y_d = (double)start_padding_y + (double)line * (double)exact_item_height;
                                float scroll_y = ImGui::GetScrollY();
                                float window_h = ImGui::GetWindowHeight();
                                
                                float target_scroll_y = scroll_y;
                                if (line_y_d < (double)scroll_y) {
                                    target_scroll_y = (float)line_y_d;
                                } else if (line_y_d + (double)exact_item_height > (double)scroll_y + (double)window_h - (double)ImGui::GetStyle().ScrollbarSize) {
                                    target_scroll_y = (float)(line_y_d + (double)exact_item_height - (double)window_h + (double)ImGui::GetStyle().ScrollbarSize);
                                }
                                if (target_scroll_y != scroll_y) {
                                    ImGui::SetScrollY(target_scroll_y);
                                }
                                
                                if (cursor_pos >= 0) {
                                    float cursor_x = 0.0f;
                                    if (cursor_pos > 0 && cursor_pos <= (int)strlen(inline_edit_buf.data())) {
                                        cursor_x = ImGui::GetFont()->CalcTextSizeA(font_size, FLT_MAX, -1.0f, inline_edit_buf.data(), inline_edit_buf.data() + cursor_pos).x;
                                    }
                                    float scroll_x = ImGui::GetScrollX();
                                    float window_w = ImGui::GetWindowWidth();
                                    
                                    float target_scroll_x = scroll_x;
                                    if (cursor_x < scroll_x) {
                                        target_scroll_x = cursor_x;
                                    } else if (cursor_x > scroll_x + window_w - ImGui::GetStyle().ScrollbarSize - char_width) {
                                        target_scroll_x = cursor_x - window_w + ImGui::GetStyle().ScrollbarSize + char_width;
                                    }
                                    if (target_scroll_x != scroll_x) {
                                        ImGui::SetScrollX(target_scroll_x);
                                    }
                                }
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
                                    ImGui::SetWindowFocus("TextChild");
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

                            bool has_selection = (doc->select_start != (size_t)-1 && doc->select_start != doc->select_end);
                            if (ImGui::IsWindowFocused() && (inline_edit_line != -1 || has_selection)) {
                                bool selection_handled = false;
                                if (has_selection) {
                                    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_Home) || ImGui::IsKeyPressed(ImGuiKey_PageUp)) {
                                        size_t s_start = std::min(doc->select_start, doc->select_end);
                                        doc->select_start = doc->select_end = (size_t)-1;
                                        if (inline_edit_line != -1) ApplyInlineEdit();
                                        int new_line = doc->GetLineFromOffset(s_start);
                                        int new_col = s_start - doc->line_offsets[new_line];
                                        SwitchToLineEdit(new_line, new_col);
                                        ScrollToKeepCursorVisible(new_line, new_col);
                                        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow)) selection_handled = true;
                                    } else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_End) || ImGui::IsKeyPressed(ImGuiKey_PageDown)) {
                                        size_t s_end = std::max(doc->select_start, doc->select_end);
                                        doc->select_start = doc->select_end = (size_t)-1;
                                        if (inline_edit_line != -1) ApplyInlineEdit();
                                        int new_line = doc->GetLineFromOffset(s_end);
                                        int new_col = s_end - doc->line_offsets[new_line];
                                        SwitchToLineEdit(new_line, new_col);
                                        ScrollToKeepCursorVisible(new_line, new_col);
                                        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow)) selection_handled = true;
                                    }
                                }

                                if (selection_handled) {
                                    // Handled arrow selection jump
                                }
                                else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && inline_edit_line > 0) {
                                    int target = inline_edit_line - 1;
                                    ApplyInlineEdit();
                                    SwitchToLineEdit(target, inline_edit_cursor_pos);
                                    ScrollToKeepCursorVisible(target, inline_edit_cursor_pos);
                                }
                                else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && inline_edit_line != -1 && inline_edit_line < (int)doc->line_offsets.size() - 1) {
                                    int target = inline_edit_line + 1;
                                    ApplyInlineEdit();
                                    SwitchToLineEdit(target, inline_edit_cursor_pos);
                                    ScrollToKeepCursorVisible(target, inline_edit_cursor_pos);
                                }
                                else if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && inline_edit_line != -1) {
                                    if (inline_edit_cursor_pos > 0) {
                                        inline_edit_cursor_pos--;
                                        ScrollToKeepCursorVisible(inline_edit_line, inline_edit_cursor_pos);
                                    } else if (inline_edit_line > 0) {
                                        int target = inline_edit_line - 1;
                                        ApplyInlineEdit();
                                        SwitchToLineEdit(target, INT_MAX);
                                        ScrollToKeepCursorVisible(target, inline_edit_cursor_pos);
                                    }
                                }
                                else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && inline_edit_line != -1) {
                                    if (inline_edit_cursor_pos < (int)strlen(inline_edit_buf.data())) {
                                        inline_edit_cursor_pos++;
                                        ScrollToKeepCursorVisible(inline_edit_line, inline_edit_cursor_pos);
                                    } else if (inline_edit_line < (int)doc->line_offsets.size() - 1) {
                                        int target = inline_edit_line + 1;
                                        ApplyInlineEdit();
                                        SwitchToLineEdit(target, 0);
                                        ScrollToKeepCursorVisible(target, inline_edit_cursor_pos);
                                    }
                                }
                                else if (ImGui::IsKeyPressed(ImGuiKey_Home) && inline_edit_line != -1) {
                                    if (ImGui::GetIO().KeyCtrl) {
                                        ApplyInlineEdit();
                                        SwitchToLineEdit(0, 0);
                                        ScrollToKeepCursorVisible(0, 0);
                                    } else {
                                        inline_edit_cursor_pos = 0;
                                        ScrollToKeepCursorVisible(inline_edit_line, inline_edit_cursor_pos);
                                    }
                                }
                                else if (ImGui::IsKeyPressed(ImGuiKey_End) && inline_edit_line != -1) {
                                    if (ImGui::GetIO().KeyCtrl) {
                                        ApplyInlineEdit();
                                        int target = std::max(0, (int)doc->line_offsets.size() - 1);
                                        SwitchToLineEdit(target, INT_MAX);
                                        ScrollToKeepCursorVisible(target, inline_edit_cursor_pos);
                                    } else {
                                        inline_edit_cursor_pos = (int)strlen(inline_edit_buf.data());
                                        ScrollToKeepCursorVisible(inline_edit_line, inline_edit_cursor_pos);
                                    }
                                }
                                else if (ImGui::IsKeyPressed(ImGuiKey_PageUp) && inline_edit_line != -1) {
                                    int lines_per_page = std::max(1, (int)(ImGui::GetWindowHeight() / exact_item_height) - 1);
                                    int target = std::max(0, inline_edit_line - lines_per_page);
                                    ApplyInlineEdit();
                                    SwitchToLineEdit(target, inline_edit_cursor_pos);
                                    ScrollToKeepCursorVisible(target, inline_edit_cursor_pos);
                                }
                                else if (ImGui::IsKeyPressed(ImGuiKey_PageDown) && inline_edit_line != -1) {
                                    int lines_per_page = std::max(1, (int)(ImGui::GetWindowHeight() / exact_item_height) - 1);
                                    int target = std::min((int)doc->line_offsets.size() - 1, inline_edit_line + lines_per_page);
                                    ApplyInlineEdit();
                                    SwitchToLineEdit(target, inline_edit_cursor_pos);
                                    ScrollToKeepCursorVisible(target, inline_edit_cursor_pos);
                                }
                                else if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
                                    if (!DeleteSelection()) {
                                        if (inline_edit_line != -1 && inline_edit_cursor_pos > 0) {
                                            size_t len = strlen(inline_edit_buf.data());
                                            memmove(inline_edit_buf.data() + inline_edit_cursor_pos - 1, inline_edit_buf.data() + inline_edit_cursor_pos, len - inline_edit_cursor_pos + 1);
                                            inline_edit_cursor_pos--;
                                            inline_edit_dirty = true;
                                        } else if (inline_edit_line > 0) {
                                            ApplyInlineEdit();
                                            int prev_line = inline_edit_line - 1;
                                            size_t start = doc->line_offsets[prev_line];
                                            size_t end = doc->line_offsets[inline_edit_line];
                                            size_t text_end = end;
                                            if (text_end > start && doc->data[text_end - 1] == '\n') text_end--;
                                            if (text_end > start && doc->data[text_end - 1] == '\r') text_end--;
                                            
                                            int new_col = text_end - start;
                                            doc->ReplaceText(text_end, end, "");
                                            doc->ClearAstHistory();
                                            text_dirty = true;
                                            SwitchToLineEdit(prev_line, new_col);
                                        }
                                    }
                                }
                                else if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
                                    if (!DeleteSelection()) {
                                        if (inline_edit_line != -1) {
                                            size_t len = strlen(inline_edit_buf.data());
                                            if (inline_edit_cursor_pos < (int)len) {
                                                memmove(inline_edit_buf.data() + inline_edit_cursor_pos, inline_edit_buf.data() + inline_edit_cursor_pos + 1, len - inline_edit_cursor_pos);
                                                inline_edit_dirty = true;
                                            } else if (inline_edit_line < (int)doc->line_offsets.size() - 1) {
                                                ApplyInlineEdit();
                                                size_t start = doc->line_offsets[inline_edit_line];
                                                size_t end = doc->line_offsets[inline_edit_line + 1];
                                                size_t text_end = end;
                                                if (text_end > start && doc->data[text_end - 1] == '\n') text_end--;
                                                if (text_end > start && doc->data[text_end - 1] == '\r') text_end--;
                                                
                                                doc->ReplaceText(text_end, end, "");
                                                doc->ClearAstHistory();
                                                text_dirty = true;
                                                
                                                SwitchToLineEdit(inline_edit_line, inline_edit_cursor_pos);
                                            }
                                        }
                                    }
                                }
                                else if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
                                    DeleteSelection();
                                    if (inline_edit_line != -1) {
                                        ApplyInlineEdit();
                                        size_t offset = doc->line_offsets[inline_edit_line] + inline_edit_cursor_pos;
                                        doc->ReplaceText(offset, offset, "\n");
                                        doc->ClearAstHistory();
                                        text_dirty = true;
                                        
                                        SwitchToLineEdit(inline_edit_line + 1, 0);
                                    }
                                }
                                else if (ImGui::IsKeyPressed(ImGuiKey_Tab)) {
                                    std::string tab_str = settings.use_tabs ? "\t" : std::string(settings.indent_size, ' ');
                                    if (doc->select_start != (size_t)-1 && doc->select_start != doc->select_end) {
                                        ApplyInlineEdit();
                                        size_t s_start = std::min(doc->select_start, doc->select_end);
                                        size_t s_end = std::max(doc->select_start, doc->select_end);
                                        int line_start = doc->GetLineFromOffset(s_start);
                                        int line_end = doc->GetLineFromOffset(s_end);
                                        
                                        // Do not indent the next line if the selection cleanly ends exactly at its start
                                        if (line_end > line_start && s_end == doc->line_offsets[line_end]) {
                                            line_end--;
                                        }
                                        
                                        size_t block_start = doc->line_offsets[line_start];
                                        size_t block_end = (line_end + 1 < (int)doc->line_offsets.size()) ? doc->line_offsets[line_end + 1] : doc->size;
                                        
                                        std::string old_block(doc->data + block_start, block_end - block_start);
                                        std::string new_block;
                                        new_block.reserve(old_block.size() + (line_end - line_start + 1) * tab_str.length());
                                        
                                        const char* ptr = old_block.c_str();
                                        const char* end_ptr = ptr + old_block.length();
                                        
                                        for (int i = line_start; i <= line_end; i++) {
                                            new_block += tab_str;
                                            const char* next_nl = strchr(ptr, '\n');
                                            if (next_nl && next_nl < end_ptr) {
                                                new_block.append(ptr, next_nl - ptr + 1);
                                                ptr = next_nl + 1;
                                            } else {
                                                new_block.append(ptr, end_ptr - ptr);
                                                ptr = end_ptr;
                                            }
                                        }
                                        if (ptr < end_ptr) {
                                            new_block.append(ptr, end_ptr - ptr);
                                        }
                                        
                                        size_t saved_start = doc->select_start;
                                        size_t saved_end = doc->select_end;
                                        
                                        for (int i = line_start; i <= line_end; i++) {
                                            size_t original_pos = doc->line_offsets[i];
                                            if (doc->select_start >= original_pos) saved_start += tab_str.length();
                                            if (doc->select_end >= original_pos) saved_end += tab_str.length();
                                        }
                                        
                                        doc->ReplaceText(block_start, block_end, new_block.c_str());
                                        doc->ClearAstHistory();
                                        text_dirty = true;
                                        
                                        int active_line = doc->GetLineFromOffset(saved_end);
                                        int active_col = saved_end - doc->line_offsets[active_line];
                                        SwitchToLineEdit(active_line, active_col);
                                        
                                        // Restore the exact selection bounds so the text remains highlighted
                                        doc->select_start = saved_start;
                                        doc->select_end = saved_end;
                                    } else if (inline_edit_line != -1) {
                                        size_t cb_len = tab_str.length();
                                        size_t len = strlen(inline_edit_buf.data());
                                        if (len + cb_len + 2 > inline_edit_buf.size()) inline_edit_buf.resize(len + cb_len + 1024);
                                        memmove(inline_edit_buf.data() + inline_edit_cursor_pos + cb_len, inline_edit_buf.data() + inline_edit_cursor_pos, len - inline_edit_cursor_pos + 1);
                                        memcpy(inline_edit_buf.data() + inline_edit_cursor_pos, tab_str.c_str(), cb_len);
                                        inline_edit_cursor_pos += cb_len;
                                        inline_edit_dirty = true;
                                    }
                                }
                                
                                if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V)) {
                                    const char* cb = ImGui::GetClipboardText();
                                    if (cb) {
                                        DeleteSelection();
                                        if (inline_edit_line != -1) {
                                            if (strchr(cb, '\n') != nullptr || strchr(cb, '\r') != nullptr) {
                                                ApplyInlineEdit();
                                                size_t offset = doc->line_offsets[inline_edit_line] + inline_edit_cursor_pos;
                                                doc->ReplaceText(offset, offset, cb);
                                                doc->ClearAstHistory();
                                                text_dirty = true;
                                                
                                                int new_line = inline_edit_line;
                                                int new_col = inline_edit_cursor_pos;
                                                for (const char* p = cb; *p; p++) {
                                                    if (*p == '\n') { new_line++; new_col = 0; }
                                                    else if (*p != '\r') new_col++;
                                                }
                                                SwitchToLineEdit(new_line, new_col);
                                            } else {
                                                size_t cb_len = strlen(cb);
                                                size_t len = strlen(inline_edit_buf.data());
                                                if (len + cb_len + 2 > inline_edit_buf.size()) inline_edit_buf.resize(len + cb_len + 1024);
                                                memmove(inline_edit_buf.data() + inline_edit_cursor_pos + cb_len, inline_edit_buf.data() + inline_edit_cursor_pos, len - inline_edit_cursor_pos + 1);
                                                memcpy(inline_edit_buf.data() + inline_edit_cursor_pos, cb, cb_len);
                                                inline_edit_cursor_pos += cb_len;
                                                inline_edit_dirty = true;
                                            }
                                        }
                                    }
                                } else {
                                    for (int n = 0; n < io.InputQueueCharacters.Size; n++) {
                                        unsigned int c = (unsigned int)io.InputQueueCharacters[n];
                                        if (c >= 32 && c != 127) {
                                            DeleteSelection();
                                            if (inline_edit_line != -1) {
                                                size_t len = strlen(inline_edit_buf.data());
                                                if (len + 2 > inline_edit_buf.size()) inline_edit_buf.resize(len + 1024);
                                                memmove(inline_edit_buf.data() + inline_edit_cursor_pos + 1, inline_edit_buf.data() + inline_edit_cursor_pos, len - inline_edit_cursor_pos + 1);
                                                inline_edit_buf[inline_edit_cursor_pos] = (char)c;
                                                inline_edit_cursor_pos++;
                                                inline_edit_dirty = true;
                                            }
                                        }
                                    }
                                }
                            }

                            // Expand scrollable area to support giant files without ImGuiListClipper float precision bugs
                            double exact_total_height = (double)doc->line_offsets.size() * (double)exact_item_height;
                                    ImGui::SetCursorPosY(start_padding_y + (float)exact_total_height);
                            ImGui::Dummy(ImVec2(0.0f, 0.0f));

                            double current_scroll_y = (double)ImGui::GetScrollY();
                            double window_h = (double)ImGui::GetWindowHeight();

                            int display_start = (int)(current_scroll_y / (double)exact_item_height);
                            int display_end = display_start + (int)(window_h / (double)exact_item_height) + 2;

                            if (display_start < 0) display_start = 0;
                            if (display_end > (int)doc->line_offsets.size()) display_end = (int)doc->line_offsets.size();

                                    float precise_screen_x = ImGui::GetWindowPos().x + start_padding_x - ImGui::GetScrollX();

                            for (int clipper_i = display_start; clipper_i < display_end; clipper_i++) {
                                size_t i = (size_t)clipper_i;

                                // Calculate precise layout screen placement using double subtraction to avoid float truncation
                                double exact_local_y = (double)clipper_i * (double)exact_item_height;
                                        float precise_screen_y = ImGui::GetWindowPos().y + start_padding_y + (float)(exact_local_y - current_scroll_y);
                                ImGui::SetCursorScreenPos(ImVec2(precise_screen_x, precise_screen_y));

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

                                    const char* p;
                                    const char* end_ptr;
                                    if (inline_edit_line == (int)i) {
                                        p = inline_edit_buf.data();
                                        end_ptr = p + strlen(p);
                                    } else {
                                        p = doc->data + start;
                                        end_ptr = doc->data + end;
                                    }
                                    
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
                                    
                                    if (inline_edit_line == (int)i && ImGui::IsWindowFocused()) {
                                        static double last_cursor_time = 0.0;
                                        static int last_cursor_pos_track = -1;
                                        if (last_cursor_pos_track != inline_edit_cursor_pos) {
                                            last_cursor_pos_track = inline_edit_cursor_pos;
                                            last_cursor_time = ImGui::GetTime();
                                        } else if (io.InputQueueCharacters.Size > 0) {
                                            last_cursor_time = ImGui::GetTime();
                                        }
                                        if (fmod(ImGui::GetTime() - last_cursor_time, 1.0) < 0.5) {
                                            float cursor_x = 0.0f;
                                            if (inline_edit_cursor_pos > 0) {
                                                cursor_x = ImGui::GetFont()->CalcTextSizeA(font_size, FLT_MAX, -1.0f, inline_edit_buf.data(), inline_edit_buf.data() + inline_edit_cursor_pos).x;
                                            }
                                            ImVec2 p1 = ImVec2(pos.x + cursor_x, pos.y);
                                            ImVec2 p2 = ImVec2(p1.x, p1.y + font_size);
                                            ImGui::GetWindowDrawList()->AddLine(p1, p2, IM_COL32(255, 255, 255, 255), 1.0f);
                                        }
                                    }
                                }
                            ImGui::PopFont();
                            ImGui::PopStyleVar();
                        }
                    }
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }

                ImGuiTabItemFlags tree_tab_flags = force_tree_tab ? ImGuiTabItemFlags_SetSelected : 0;
                if (settings.show_tree_view && ImGui::BeginTabItem("Tree View", &settings.show_tree_view, tree_tab_flags)) {
                active_view = ActiveView::Tree;
                force_tree_tab = false;

                if (inline_edit_dirty) ApplyInlineEdit();
                if (text_dirty && !doc_formatting && !doc_loading && !doc_saving) {
                    text_dirty = false;
                    doc_formatting = true;
                    bool allow_comments = settings.allow_comments;
                    if (doc_thread.joinable()) doc_thread.join();
                    doc_thread = std::thread([doc, allow_comments, &doc_formatting, &doc_formatting_done, &search_dirty]() { doc->RebuildTreeFromText(allow_comments); doc_formatting = false; doc_formatting_done = true; search_dirty = true; });
                }

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
            if (settings.show_graph_view && ImGui::BeginTabItem("Graph View", &settings.show_graph_view, graph_tab_flags)) {
                active_view = ActiveView::Graph;
                force_graph_tab = false;

                if (inline_edit_dirty) ApplyInlineEdit();
                if (text_dirty && !doc_formatting && !doc_loading && !doc_saving) {
                    text_dirty = false;
                    doc_formatting = true;
                    bool allow_comments = settings.allow_comments;
                    if (doc_thread.joinable()) doc_thread.join();
                    doc_thread = std::thread([doc, allow_comments, &doc_formatting, &doc_formatting_done, &search_dirty]() { doc->RebuildTreeFromText(allow_comments); doc_formatting = false; doc_formatting_done = true; search_dirty = true; });
                }

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
            if (doc_loading || doc_saving || doc_formatting || doc_graph_building) {
                    const char* msg = doc_loading ? "Loading and parsing JSON... Please wait." : 
                                      doc_saving ? "Saving JSON... Please wait." : 
                                  doc_graph_building ? "Building Graph View... Please wait." :
                                      "Processing JSON... Please wait.";
                    ImGui::TextDisabled("%s", msg);
                } else if (doc->root_json) {
                    if (doc->graph_dirty) {
                        doc->graph_dirty = false;
                    doc_graph_building = true;
                    if (doc_thread.joinable()) doc_thread.join();
                    doc_thread = std::thread([doc, &doc_graph_building]() {
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
                        doc->graph_memory_bytes = doc->CalculateGraphMemory(doc->graph_root);
                        doc_graph_building = false;
                    });
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
                                    settings.show_text_view = true;
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
                                    settings.show_tree_view = true;
                                    force_tree_tab = true;
                                    tree_focus_frames = 3;
                                }
                            }
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
            ImGui::EndTabBar();
        }
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