#include "views.h"
#include "document.h"
#include <cstdio>    // For snprintf
#include <cstdlib>   // For atof
#include <cstring>   // For strncpy, strcmp
#include <algorithm> // For std::find

GraphNode* BuildGraphNode(LargeTextFile* doc, JsonValue* val, const std::string& key, int depth, int& node_count) {
    if (!val || node_count > 100000) return nullptr;
    node_count++;

    GraphNode* node = new GraphNode();
    node->offset = val->offset;
    node->source_val = val;

    std::string preview = "";
    if (val->type == JSON_STRING) {
        std::string s = val->as.string ? val->as.string : "";
        if (s.length() > 20) {
            s.resize(17);
            s += "...";
        }
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
            if (node_count > 100000) {
                if (depth == 1) {
                    delete node;
                    return nullptr;
                }
                break; // Global cap reached
            }

            std::string child_key = (val->type == JSON_OBJECT) ? (val->as.list.items[i].key ? val->as.list.items[i].key : "") : ("[" + std::to_string(i) + "]");
            GraphNode* child = BuildGraphNode(doc, &val->as.list.items[i].value, child_key, depth + 1, node_count);
            
            if (!child && node_count > 100000) {
                if (depth == 1) {
                    delete node;
                    return nullptr;
                }
                break;
            }
            
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

void LayoutGraphNode(GraphNode* node, int depth, float& current_y, float& max_x, float current_x) {
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

void DrawGraphEdges(ImDrawList* dl, GraphNode* node, ImVec2 offset, const ImRect& clip_rect) {
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

void DrawGraphNodes(ImDrawList* dl, GraphNode* node, ImVec2 offset, ImVec2 mouse_pos, GraphNode** out_hovered, const ImRect& clip_rect, JsonValue* highlight_val, double highlight_time, int focus_frames) {
    ImVec2 p_min = ImVec2(offset.x + node->x, offset.y + node->y - 12.0f);
    ImVec2 p_max = ImVec2(p_min.x + node->drawn_width, p_min.y + 24.0f);

    if (node->source_val == highlight_val && node->type == GraphNodeType::Normal) {
        float fade = ImMax(0.0f, 1.0f - (float)(ImGui::GetTime() - highlight_time) / 1.5f);
        if (fade > 0.0f) {
            dl->AddRectFilled(ImVec2(p_min.x - 4.0f, p_min.y - 4.0f), ImVec2(p_max.x + 4.0f, p_max.y + 4.0f), IM_COL32(255, 255, 0, (int)(100 * fade)), 4.0f);
        }
        if (focus_frames > 0) {
            ImGui::SetScrollFromPosY(p_min.y - ImGui::GetWindowPos().y, 0.5f);
            ImGui::SetScrollFromPosX(p_min.x - ImGui::GetWindowPos().x, 0.5f);
        }
    }

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
        DrawGraphNodes(dl, child, offset, mouse_pos, out_hovered, clip_rect, highlight_val, highlight_time, focus_frames);
    }
}

// Globals for managing in-place editing in the tree view to avoid complex state passing
// through recursion and issues with shared static buffers.
static void* s_editing_item_ptr = nullptr; // Can point to a JsonValue (for string value)
static char s_edit_buf[4096] = "";
static bool s_edit_wants_focus = false;

int DrawEditableJsonNode(LargeTextFile* doc, JsonNode* node, int node_index, std::vector<size_t>& current_path, const std::vector<JsonValue*>& focus_path, JsonValue* highlight_val, double highlight_time, const std::string& current_string_path) {
    if (!node) return 0;
    int action = 0;
    JsonValue* val = &node->value;

    if (node_index >= 0) {
        current_path.push_back((size_t)node_index);
        ImGui::PushID(node_index);
    } else {
        ImGui::PushID("Root");
    }

    bool in_focus_path = false;
    bool is_focus_target = false;
    
    JsonValue* actual_val = (node_index == -1) ? doc->root_json : val;
    
    if (!focus_path.empty() && actual_val != nullptr) {
        in_focus_path = (std::find(focus_path.begin(), focus_path.end(), actual_val) != focus_path.end());
        is_focus_target = (actual_val == focus_path.back());
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

    std::string my_path = current_string_path;
    if (node_index >= 0) {
        if (node->key) { // Part of an object
            bool is_simple_key = false;
            if (node->key && node->key[0] != '\0') {
                is_simple_key = (isalpha(node->key[0]) || node->key[0] == '_');
                for (int i = 1; is_simple_key && node->key[i] != '\0'; i++) {
                    if (!isalnum(node->key[i]) && node->key[i] != '_') {
                        is_simple_key = false;
                    }
                }
            }
            if (is_simple_key) {
                my_path += ".";
                my_path += node->key;
            } else {
                my_path += "[\"";
                my_path += node->key;
                my_path += "\"]";
            }
        } else { // Part of an array
            my_path += "[";
            my_path += std::to_string(node_index);
            my_path += "]";
        }
    }

    if (node->key != nullptr) {
        char key_buf[256];
        strncpy(key_buf, node->key, sizeof(key_buf) - 1);
        key_buf[sizeof(key_buf) - 1] = '\0';
        float calc_w = ImGui::CalcTextSize(key_buf).x + ImGui::GetStyle().FramePadding.x * 2.0f + 8.0f;
        ImGui::SetNextItemWidth(calc_w < 30.0f ? 30.0f : calc_w);
        if (ImGui::InputText("##key", key_buf, sizeof(key_buf), ImGuiInputTextFlags_NoHorizontalScroll)) {
            size_t key_len = strlen(key_buf);
            node->key = static_cast<char*>(arena_alloc(&doc->main_arena, key_len + 1));
            memcpy(node->key, key_buf, key_len + 1);
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
            if (s_editing_item_ptr == val) {
                if (s_edit_wants_focus) { ImGui::SetKeyboardFocusHere(); s_edit_wants_focus = false; }
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputText("##val", s_edit_buf, sizeof(s_edit_buf), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
                    if (strcmp(s_edit_buf, val->as.string ? val->as.string : "") != 0) {
                        doc->PushUndo(UndoActionType::SetNode, current_path, *node);
                        size_t edit_len = strlen(s_edit_buf);
                        val->as.string = static_cast<char*>(arena_alloc(&doc->main_arena, edit_len + 1));
                        memcpy(val->as.string, s_edit_buf, edit_len + 1);
                        action |= 1;
                    }
                    s_editing_item_ptr = nullptr;
                } else if (ImGui::IsItemDeactivated()) {
                    if (strcmp(s_edit_buf, val->as.string ? val->as.string : "") != 0) {
                        doc->PushUndo(UndoActionType::SetNode, current_path, *node);
                        size_t edit_len = strlen(s_edit_buf);
                        val->as.string = static_cast<char*>(arena_alloc(&doc->main_arena, edit_len + 1));
                        memcpy(val->as.string, s_edit_buf, edit_len + 1);
                        action |= 1;
                    }
                    s_editing_item_ptr = nullptr;
                }
            } else {
                ImGui::Text("\"%s\"", val->as.string ? val->as.string : "");
                if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
                if (ImGui::IsItemClicked() && s_editing_item_ptr == nullptr) {
                    s_editing_item_ptr = val;
                    strncpy(s_edit_buf, val->as.string ? val->as.string : "", sizeof(s_edit_buf) - 1);
                    s_edit_buf[sizeof(s_edit_buf) - 1] = '\0';
                    s_edit_wants_focus = true;
                }
            }
            ApplyFocusTarget();
            break;
        }
        case JSON_NUMBER: {
            ImGui::SetNextItemWidth(-FLT_MIN);
            const char* format = (val->as.number == (int64_t)val->as.number) ? "%.0f" : "%.17g";
            if (ImGui::IsItemActivated()) doc->PushUndo(UndoActionType::SetNode, current_path, *node); // Before edit
            if (ImGui::InputDouble("##val", &val->as.number, 0.0, 0.0, format)) action |= 1; // After edit
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
    
    // Change Type Popup & Removal Logic
    // ... [Content exactly matches the main.cpp equivalent you had written]
    if (ImGui::BeginPopupContextItem("node_context")) {
        ImGui::TextDisabled("Change Type");
        ImGui::Separator();
        if (ImGui::MenuItem("Object",  nullptr, val->type == JSON_OBJECT)) { if (val->type != JSON_OBJECT) { doc->PushUndo(UndoActionType::SetNode, current_path, *node); *val = *json_create_object(&doc->main_arena); action |= 1; } }
        if (ImGui::MenuItem("Array",   nullptr, val->type == JSON_ARRAY)) { if (val->type != JSON_ARRAY) { doc->PushUndo(UndoActionType::SetNode, current_path, *node); *val = *json_create_array(&doc->main_arena); action |= 1; } }
        if (ImGui::MenuItem("String",  nullptr, val->type == JSON_STRING)) { if (val->type != JSON_STRING) { char buf[128] = ""; if (val->type == JSON_NUMBER) { if (val->as.number == (int64_t)val->as.number) snprintf(buf, sizeof(buf), "%lld", (long long)val->as.number); else snprintf(buf, sizeof(buf), "%.17g", val->as.number); } else if (val->type == JSON_BOOL) { snprintf(buf, sizeof(buf), "%s", val->as.boolean ? "true" : "false"); } doc->PushUndo(UndoActionType::SetNode, current_path, *node); *val = *json_create_string(&doc->main_arena, buf); action |= 1; } }
        if (ImGui::MenuItem("Number",  nullptr, val->type == JSON_NUMBER)) { if (val->type != JSON_NUMBER) { double num = 0.0; if (val->type == JSON_STRING && val->as.string) num = atof(val->as.string); else if (val->type == JSON_BOOL) num = val->as.boolean ? 1.0 : 0.0; doc->PushUndo(UndoActionType::SetNode, current_path, *node); *val = *json_create_number(&doc->main_arena, num); action |= 1; } }
        if (ImGui::MenuItem("Boolean", nullptr, val->type == JSON_BOOL)) { if (val->type != JSON_BOOL) { bool b = false; if (val->type == JSON_NUMBER) b = (val->as.number != 0.0); else if (val->type == JSON_STRING && val->as.string) b = (strcmp(val->as.string, "true") == 0 || strcmp(val->as.string, "True") == 0 || atof(val->as.string) != 0.0); doc->PushUndo(UndoActionType::SetNode, current_path, *node); *val = *json_create_bool(&doc->main_arena, b); action |= 1; } }
        if (ImGui::MenuItem("Null",    nullptr, val->type == JSON_NULL)) { if (val->type != JSON_NULL) { doc->PushUndo(UndoActionType::SetNode, current_path, *node); *val = *json_create_null(&doc->main_arena); action |= 1; } }
        
        ImGui::Separator();
        if (ImGui::MenuItem("Copy Path")) {
            ImGui::SetClipboardText(my_path.c_str());
        }
        if (ImGui::MenuItem("Copy Value")) {
            Arena temp; arena_init(&temp);
            const char* str = json_to_string(&temp, val, true, false, 4, false);
            if (str) ImGui::SetClipboardText(str);
            arena_free(&temp);
        }

        if (node_index >= 0) {
            ImGui::Separator();
            if (ImGui::MenuItem("Remove Node")) { doc->PushUndo(UndoActionType::InsertNode, current_path, *node); action |= 2; }
        }
        ImGui::EndPopup();
    }

    if (is_container && node_open) {
        bool is_obj = (val->type == JSON_OBJECT);
        int item_to_remove = -1;
        
        const size_t chunk_size = doc->pagination_size;
        for (size_t chunk_start = 0; chunk_start < val->as.list.count; chunk_start += chunk_size) {
            size_t chunk_end = chunk_start + chunk_size;
            if (chunk_end > val->as.list.count) chunk_end = val->as.list.count;
            
            bool show_chunk = true;
            if (val->as.list.count > chunk_size) {
                bool chunk_has_focus = false;
                if (in_focus_path && focus_path.size() > 1 && actual_val != nullptr) {
                    auto it = std::find(focus_path.begin(), focus_path.end(), actual_val);
                    if (it != focus_path.end() && (it + 1) != focus_path.end()) {
                        const JsonValue* next_in_path = *(it + 1);
                        for (size_t i = chunk_start; i < chunk_end; i++) {
                            if (&val->as.list.items[i].value == next_in_path) {
                                chunk_has_focus = true; break;
                            }
                        }
                    }
                }
                if (chunk_has_focus) ImGui::SetNextItemOpen(true);

                char chunk_label[64];
                snprintf(chunk_label, sizeof(chunk_label), is_obj ? "{...} [%zu - %zu]" : "[...] [%zu - %zu]", chunk_start, chunk_end - 1);
                show_chunk = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<uintptr_t>(chunk_start)), ImGuiTreeNodeFlags_SpanAvailWidth, "%s", chunk_label);
            }
            
            if (show_chunk) {
                for (size_t i = chunk_start; i < chunk_end; i++) {
                    int child_action = DrawEditableJsonNode(doc, &val->as.list.items[i], (int)i, current_path, focus_path, highlight_val, highlight_time, my_path);
                    if (child_action & 1) action |= 1;
                    if (child_action & 2) item_to_remove = (int)i; 
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

    if (node_index >= 0) current_path.pop_back();
    ImGui::PopID();
    return action;
}