#!/bin/bash
set -e

mkdir -p third_party
echo "Cloning Dear ImGui..."
git clone --depth 1 -b v1.90.4 https://github.com/ocornut/imgui.git third_party/imgui
echo "ImGui downloaded successfully to third_party/imgui."