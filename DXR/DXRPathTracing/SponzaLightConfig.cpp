#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "SponzaLightConfig.h"

#include <cmath>
#include <cstdio>
#include <regex>
#include <string>

namespace
{
    constexpr char c_numberPattern[] =
        "[-+]?(?:[0-9]*\\.?[0-9]+)(?:[eE][-+]?[0-9]+)?";

    bool ReadUtf8File(const std::wstring& filePath, std::string& text)
    {
        FILE* file = nullptr;
        if (_wfopen_s(&file, filePath.c_str(), L"rb") != 0 || !file)
            return false;
        if (std::fseek(file, 0, SEEK_END) != 0)
        {
            std::fclose(file);
            return false;
        }
        const long size = std::ftell(file);
        if (size < 0 || std::fseek(file, 0, SEEK_SET) != 0)
        {
            std::fclose(file);
            return false;
        }
        text.resize(static_cast<std::size_t>(size));
        const std::size_t readSize = text.empty()
            ? 0
            : std::fread(&text[0], 1, text.size(), file);
        std::fclose(file);
        return readSize == text.size();
    }

    bool FindMatchingDelimiter(
        const std::string& text,
        std::size_t begin,
        char openCharacter,
        char closeCharacter,
        std::size_t& end)
    {
        int depth = 0;
        bool inString = false;
        bool escaped = false;
        for (std::size_t position = begin; position < text.size(); ++position)
        {
            const char character = text[position];
            if (inString)
            {
                if (escaped)
                    escaped = false;
                else if (character == '\\')
                    escaped = true;
                else if (character == '"')
                    inString = false;
                continue;
            }
            if (character == '"')
                inString = true;
            else if (character == openCharacter)
                ++depth;
            else if (character == closeCharacter && --depth == 0)
            {
                end = position;
                return true;
            }
        }
        return false;
    }

    bool ExtractFloat(
        const std::string& object,
        const char* propertyName,
        float& value)
    {
        const std::regex expression(
            std::string("\"") + propertyName +
            "\"\\s*:\\s*(" + c_numberPattern + ")");
        std::smatch match;
        if (!std::regex_search(object, match, expression))
            return false;
        try
        {
            const double parsed = std::stod(match[1].str());
            if (!std::isfinite(parsed))
                return false;
            value = static_cast<float>(parsed);
        }
        catch (...)
        {
            return false;
        }
        return true;
    }

    bool ExtractFloat3(
        const std::string& object,
        const char* propertyName,
        float value[3])
    {
        const std::string number = std::string("(") + c_numberPattern + ")";
        const std::regex expression(
            std::string("\"") + propertyName +
            "\"\\s*:\\s*\\[\\s*" + number +
            "\\s*,\\s*" + number +
            "\\s*,\\s*" + number + "\\s*\\]");
        std::smatch match;
        if (!std::regex_search(object, match, expression))
            return false;
        try
        {
            for (std::size_t component = 0; component < 3; ++component)
            {
                const double parsed = std::stod(match[component + 1].str());
                if (!std::isfinite(parsed))
                    return false;
                value[component] = static_cast<float>(parsed);
            }
        }
        catch (...)
        {
            return false;
        }
        return true;
    }

    float LengthSquared(const float value[3])
    {
        return value[0] * value[0] +
            value[1] * value[1] +
            value[2] * value[2];
    }

    bool ValidateLight(const SceneAreaLight& light)
    {
        if (!(light.width > 0.0f) || !(light.height > 0.0f) ||
            LengthSquared(light.right) <= 1.0e-8f ||
            LengthSquared(light.up) <= 1.0e-8f)
        {
            return false;
        }
        const float dot = light.right[0] * light.up[0] +
            light.right[1] * light.up[1] +
            light.right[2] * light.up[2];
        const float crossSquared =
            LengthSquared(light.right) * LengthSquared(light.up) - dot * dot;
        if (crossSquared <= 1.0e-8f)
            return false;
        return light.radiance[0] >= 0.0f &&
            light.radiance[1] >= 0.0f &&
            light.radiance[2] >= 0.0f;
    }
}

bool LoadSponzaLightConfig(
    const std::wstring& filePath,
    std::vector<SceneAreaLight>& lights,
    std::wstring& errorMessage)
{
    lights.clear();
    errorMessage.clear();
    std::string text;
    if (!ReadUtf8File(filePath, text))
    {
        errorMessage = L"Light config file could not be read.";
        return false;
    }

    const std::size_t property = text.find("\"lights\"");
    const std::size_t arrayBegin = property == std::string::npos
        ? std::string::npos
        : text.find('[', property);
    std::size_t arrayEnd = 0;
    if (arrayBegin == std::string::npos ||
        !FindMatchingDelimiter(text, arrayBegin, '[', ']', arrayEnd))
    {
        errorMessage = L"sponza_lights.json needs a lights array.";
        return false;
    }

    std::size_t searchPosition = arrayBegin + 1;
    while (searchPosition < arrayEnd)
    {
        const std::size_t objectBegin = text.find('{', searchPosition);
        if (objectBegin == std::string::npos || objectBegin >= arrayEnd)
            break;
        std::size_t objectEnd = 0;
        if (!FindMatchingDelimiter(text, objectBegin, '{', '}', objectEnd) ||
            objectEnd > arrayEnd)
        {
            errorMessage = L"The light array contains an invalid object.";
            return false;
        }

        const std::string object =
            text.substr(objectBegin, objectEnd - objectBegin + 1);
        SceneAreaLight light = {};
        if (!ExtractFloat3(object, "position", light.position) ||
            !ExtractFloat3(object, "right", light.right) ||
            !ExtractFloat3(object, "up", light.up) ||
            !ExtractFloat3(object, "radiance", light.radiance) ||
            !ExtractFloat(object, "width", light.width) ||
            !ExtractFloat(object, "height", light.height) ||
            !ValidateLight(light))
        {
            errorMessage =
                L"Each light needs valid position/right/up/radiance, width, and height.";
            return false;
        }
        lights.push_back(light);
        searchPosition = objectEnd + 1;
    }

    if (lights.empty())
    {
        errorMessage = L"The light config does not contain any lights.";
        return false;
    }
    return true;
}
