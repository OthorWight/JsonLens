#ifndef JSONLENS_VIEWS_H
#define JSONLENS_VIEWS_H

#include <vector>
#include <string>
#include "imgui.h"
#include "imgui_internal.h"
#include "arena_json.h"

struct LargeTextFile; // Forward declaration

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

GraphNode* BuildGraphNode(LargeTextFile* doc, JsonValue* val, const std::string& key, int depth, int& node_count);
void LayoutGraphNode(GraphNode* node, int depth, float& current_y, float& max_x, float current_x = 40.0f);
void DrawGraphEdges(ImDrawList* dl, GraphNode* node, ImVec2 offset, const ImRect& clip_rect);
void DrawGraphNodes(ImDrawList* dl, GraphNode* node, ImVec2 offset, ImVec2 mouse_pos, GraphNode** out_hovered, const ImRect& clip_rect, JsonValue* highlight_val, double highlight_time, int focus_frames);
int DrawEditableJsonNode(LargeTextFile* doc, JsonNode* node, int node_index, std::vector<size_t>& current_path, const std::vector<JsonValue*>& focus_path, JsonValue* highlight_val, double highlight_time, std::string current_string_path = "root");

#endif // JSONLENS_VIEWS_H