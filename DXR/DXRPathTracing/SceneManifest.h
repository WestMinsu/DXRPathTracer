#pragma once

#include <cstdint>
#include <string>

#include "GltfSceneLoader.h"

struct SponzaSceneManifestSettings
{
    std::uint32_t areaLightCount = 0;
    bool dynamicMetalSphere = false;
};

bool WriteSponzaSceneManifest(
    const std::wstring& manifestPath,
    const std::wstring& sourcePath,
    const GltfLoadReport& report,
    const SponzaSceneManifestSettings& settings,
    std::wstring& errorMessage);
