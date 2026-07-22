#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "SceneManifest.h"

#include <Windows.h>
#include <shlobj.h>

#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "shell32.lib")

namespace
{
    bool Fail(std::wstring& errorMessage, const wchar_t* message)
    {
        errorMessage = message;
        return false;
    }

    bool WideToUtf8(const std::wstring& text, std::string& converted)
    {
        converted.clear();
        const int requiredLength = WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            text.c_str(),
            -1,
            nullptr,
            0,
            nullptr,
            nullptr);
        if (requiredLength <= 0)
            return false;

        converted.resize(static_cast<std::size_t>(requiredLength));
        const int convertedLength = WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            text.c_str(),
            -1,
            &converted[0],
            requiredLength,
            nullptr,
            nullptr);
        if (convertedLength <= 0)
        {
            converted.clear();
            return false;
        }
        converted.resize(static_cast<std::size_t>(convertedLength - 1));
        return true;
    }

    std::string EscapeJson(const std::string& value)
    {
        static const char hexDigits[] = "0123456789ABCDEF";
        std::string escaped;
        escaped.reserve(value.size());
        for (const unsigned char character : value)
        {
            switch (character)
            {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (character < 0x20)
                {
                    escaped += "\\u00";
                    escaped += hexDigits[(character >> 4) & 0x0F];
                    escaped += hexDigits[character & 0x0F];
                }
                else
                {
                    escaped.push_back(static_cast<char>(character));
                }
                break;
            }
        }
        return escaped;
    }

    bool EnsureParentDirectory(
        const std::wstring& filePath,
        std::wstring& errorMessage)
    {
        const DWORD requiredLength =
            GetFullPathNameW(filePath.c_str(), 0, nullptr, nullptr);
        if (requiredLength == 0)
            return Fail(errorMessage, L"Failed to resolve the manifest path.");

        std::vector<wchar_t> absolutePath(requiredLength);
        const DWORD writtenLength = GetFullPathNameW(
            filePath.c_str(),
            requiredLength,
            absolutePath.data(),
            nullptr);
        if (writtenLength == 0 || writtenLength >= requiredLength)
            return Fail(errorMessage, L"Failed to resolve the manifest path.");

        const std::wstring resolvedPath(absolutePath.data(), writtenLength);
        const std::size_t separator = resolvedPath.find_last_of(L"\\/");
        if (separator == std::wstring::npos)
            return true;

        const std::wstring directory = resolvedPath.substr(0, separator);
        const DWORD attributes = GetFileAttributesW(directory.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES)
        {
            return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
                ? true
                : Fail(
                    errorMessage,
                    L"The manifest parent path is not a directory.");
        }

        const int result =
            SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr);
        if (result == ERROR_SUCCESS ||
            result == ERROR_ALREADY_EXISTS ||
            result == ERROR_FILE_EXISTS)
        {
            return true;
        }
        return Fail(
            errorMessage,
            L"Failed to create the manifest output directory.");
    }
}

bool WriteSponzaSceneManifest(
    const std::wstring& manifestPath,
    const std::wstring& sourcePath,
    const GltfLoadReport& report,
    const SponzaSceneManifestSettings& settings,
    std::wstring& errorMessage)
{
    errorMessage.clear();
    if (manifestPath.empty())
        return Fail(errorMessage, L"The manifest output path is empty.");
    if (!EnsureParentDirectory(manifestPath, errorMessage))
        return false;

    std::string sourcePathUtf8;
    if (!WideToUtf8(sourcePath, sourcePathUtf8))
        return Fail(errorMessage, L"The Sponza path cannot be converted to UTF-8.");

    FILE* file = nullptr;
    if (_wfopen_s(&file, manifestPath.c_str(), L"wb") != 0 || !file)
        return Fail(errorMessage, L"Failed to create the Sponza scene manifest.");

    const std::string escapedPath = EscapeJson(sourcePathUtf8);
    const int writeResult = std::fprintf(
        file,
        "{\n"
        "  \"scene\": \"Sponza-lite\",\n"
        "  \"source_model\": \"%s\",\n"
        "  \"source_primitive_count\": %u,\n"
        "  \"loaded_primitive_count\": %u,\n"
        "  \"skipped_non_opaque_primitive_count\": %u,\n"
        "  \"source_material_count\": %u,\n"
        "  \"loaded_material_count\": %u,\n"
        "  \"loaded_texture_count\": %u,\n"
        "  \"ignored_occlusion_texture_count\": %u,\n"
        "  \"supported_material_inputs\": [\n"
        "    \"baseColor\",\n"
        "    \"metallicRoughness\",\n"
        "    \"normalMap\",\n"
        "    \"alphaMask\"\n"
        "  ],\n"
        "  \"area_light_count\": %u,\n"
        "  \"dynamic_metal_sphere\": %s,\n"
        "  \"non_opaque_policy\": \"MASK evaluated, BLEND primitive excluded\",\n"
        "  \"occlusion_policy\": \"texture ignored\"\n"
        "}\n",
        escapedPath.c_str(),
        report.sourcePrimitiveCount,
        report.loadedPrimitiveCount,
        report.skippedNonOpaquePrimitiveCount,
        report.sourceMaterialCount,
        report.loadedMaterialCount,
        report.loadedTextureCount,
        report.ignoredOcclusionTextureCount,
        settings.areaLightCount,
        settings.dynamicMetalSphere ? "true" : "false");

    const bool closeSucceeded = std::fclose(file) == 0;
    if (writeResult < 0 || !closeSucceeded)
    {
        DeleteFileW(manifestPath.c_str());
        return Fail(errorMessage, L"Failed to write the Sponza scene manifest.");
    }
    return true;
}
