#pragma once

#include <string>
#include <vector>

#include "SceneData.h"

struct SponzaDirectionalLight
{
    bool enabled = false;
    // Direction in which light rays travel, from the source toward the scene.
    float direction[3] = { 0.0f, -1.0f, 0.0f };
    float radiance[3] = { 0.0f, 0.0f, 0.0f };
    float samplingProbability = 0.5f;
};

bool LoadSponzaLightConfig(
    const std::wstring& filePath,
    std::vector<SceneAreaLight>& lights,
    SponzaDirectionalLight& directionalLight,
    std::wstring& errorMessage);
