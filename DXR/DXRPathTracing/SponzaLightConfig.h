#pragma once

#include <string>
#include <vector>

#include "SceneData.h"

bool LoadSponzaLightConfig(
    const std::wstring& filePath,
    std::vector<SceneAreaLight>& lights,
    std::wstring& errorMessage);
