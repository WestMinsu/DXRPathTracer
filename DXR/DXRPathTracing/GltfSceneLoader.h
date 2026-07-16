#pragma once

#include <string>

#include "SceneData.h"

// Loads a static glTF 2.0 scene, converts its right-handed coordinates to the
// renderer's left-handed coordinates, and flattens node transforms into SceneData.
// The first stage supports triangle meshes with POSITION, NORMAL, indices,
// and constant metallic-roughness material factors.
bool LoadGltfSceneData(
    const std::wstring& filePath,
    SceneData& scene,
    std::wstring& errorMessage);
