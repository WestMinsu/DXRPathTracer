#pragma once

#include <array>
#include <string>
#include <vector>

struct CameraPose
{
    std::array<float, 3> position = { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> target = { 0.0f, 0.0f, 1.0f };
};

class CameraPath
{
public:
    struct Keyframe
    {
        double timeSeconds = 0.0;
        CameraPose pose;
    };

    bool Load(const std::wstring& filePath, std::wstring* errorMessage);
    bool SetKeyframes(
        const std::vector<Keyframe>& keyframes,
        double framesPerSecond,
        bool loop,
        std::wstring* errorMessage);
    bool Save(
        const std::wstring& filePath,
        const char* description,
        std::wstring* errorMessage) const;
    bool Sample(double timeSeconds, CameraPose& pose) const;

    bool IsEmpty() const { return m_keyframes.empty(); }
    double GetDurationSeconds() const;
    double GetFramesPerSecond() const { return m_framesPerSecond; }
    bool IsLooping() const { return m_loop; }
    const std::vector<Keyframe>& GetKeyframes() const { return m_keyframes; }

private:
    std::vector<Keyframe> m_keyframes;
    double m_framesPerSecond = 60.0;
    bool m_loop = false;
};
