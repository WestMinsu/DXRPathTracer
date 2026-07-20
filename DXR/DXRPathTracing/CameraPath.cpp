#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "CameraPath.h"

#include <algorithm>
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
        const bool success = readSize == text.size();
        std::fclose(file);
        return success;
    }

    bool ExtractNumber(
        const std::string& object,
        const char* propertyName,
        double& value)
    {
        const std::regex expression(
            std::string("\"") + propertyName +
            "\"\\s*:\\s*(" + c_numberPattern + ")");
        std::smatch match;
        if (!std::regex_search(object, match, expression))
            return false;
        try
        {
            value = std::stod(match[1].str());
        }
        catch (...)
        {
            return false;
        }
        return std::isfinite(value);
    }

    bool ExtractFloat3(
        const std::string& object,
        const char* propertyName,
        std::array<float, 3>& value)
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
            for (std::size_t component = 0; component < value.size(); ++component)
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
            else if (character == closeCharacter)
            {
                --depth;
                if (depth == 0)
                {
                    end = position;
                    return true;
                }
            }
        }
        return false;
    }

    bool ParseKeyframes(
        const std::string& text,
        std::vector<CameraPath::Keyframe>& keyframes)
    {
        const std::size_t property = text.find("\"keyframes\"");
        if (property == std::string::npos)
            return false;
        const std::size_t arrayBegin = text.find('[', property);
        if (arrayBegin == std::string::npos)
            return false;
        std::size_t arrayEnd = 0;
        if (!FindMatchingDelimiter(text, arrayBegin, '[', ']', arrayEnd))
            return false;

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
                return false;
            }

            const std::string object =
                text.substr(objectBegin, objectEnd - objectBegin + 1);
            CameraPath::Keyframe keyframe;
            if (!ExtractNumber(object, "time", keyframe.timeSeconds) ||
                !ExtractFloat3(object, "position", keyframe.pose.position) ||
                !ExtractFloat3(object, "target", keyframe.pose.target))
            {
                return false;
            }
            keyframes.push_back(keyframe);
            searchPosition = objectEnd + 1;
        }
        return !keyframes.empty();
    }

    bool ExtractBoolean(
        const std::string& text,
        const char* propertyName,
        bool& value)
    {
        const std::regex expression(
            std::string("\"") + propertyName +
            "\"\\s*:\\s*(true|false)");
        std::smatch match;
        if (!std::regex_search(text, match, expression))
            return false;
        value = match[1].str() == "true";
        return true;
    }
}

bool CameraPath::Load(
    const std::wstring& filePath,
    std::wstring* errorMessage)
{
    std::string text;
    if (!ReadUtf8File(filePath, text))
    {
        if (errorMessage)
            *errorMessage = L"Camera path file could not be read.";
        return false;
    }

    std::vector<Keyframe> keyframes;
    if (!ParseKeyframes(text, keyframes))
    {
        if (errorMessage)
            *errorMessage =
                L"camera_path.json needs time, position[3], and target[3].";
        return false;
    }

    double framesPerSecond = 60.0;
    double parsedFramesPerSecond = 0.0;
    if (ExtractNumber(text, "frames_per_second", parsedFramesPerSecond))
        framesPerSecond = parsedFramesPerSecond;
    if (!std::isfinite(framesPerSecond) || framesPerSecond <= 0.0)
    {
        if (errorMessage)
            *errorMessage = L"frames_per_second must be positive.";
        return false;
    }

    bool loop = false;
    ExtractBoolean(text, "loop", loop);
    for (std::size_t index = 0; index < keyframes.size(); ++index)
    {
        if (keyframes[index].timeSeconds < 0.0 ||
            (index > 0 &&
             keyframes[index].timeSeconds <= keyframes[index - 1].timeSeconds))
        {
            if (errorMessage)
                *errorMessage = L"Keyframe times must be strictly increasing.";
            return false;
        }
    }

    m_keyframes = keyframes;
    m_framesPerSecond = framesPerSecond;
    m_loop = loop;
    if (errorMessage)
        errorMessage->clear();
    return true;
}

bool CameraPath::Sample(double timeSeconds, CameraPose& pose) const
{
    if (m_keyframes.empty())
        return false;

    const double duration = GetDurationSeconds();
    double sampleTime = timeSeconds;
    if (m_loop && duration > 0.0)
        sampleTime = std::fmod((std::max)(sampleTime, 0.0), duration);
    else
        sampleTime = (std::max)(0.0, (std::min)(sampleTime, duration));

    if (sampleTime <= m_keyframes.front().timeSeconds)
    {
        pose = m_keyframes.front().pose;
        return true;
    }

    const auto upper = std::upper_bound(
        m_keyframes.begin(),
        m_keyframes.end(),
        sampleTime,
        [](double time, const Keyframe& keyframe)
        {
            return time < keyframe.timeSeconds;
        });
    if (upper == m_keyframes.end())
    {
        pose = m_keyframes.back().pose;
        return true;
    }

    const Keyframe& end = *upper;
    const Keyframe& begin = *(upper - 1);
    const double interval = end.timeSeconds - begin.timeSeconds;
    const float t = interval > 0.0
        ? static_cast<float>((sampleTime - begin.timeSeconds) / interval)
        : 0.0f;
    for (std::size_t component = 0; component < 3; ++component)
    {
        pose.position[component] =
            begin.pose.position[component] +
            (end.pose.position[component] - begin.pose.position[component]) * t;
        pose.target[component] =
            begin.pose.target[component] +
            (end.pose.target[component] - begin.pose.target[component]) * t;
    }
    return true;
}

double CameraPath::GetDurationSeconds() const
{
    return m_keyframes.empty() ? 0.0 : m_keyframes.back().timeSeconds;
}
