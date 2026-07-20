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
    bool Sample(double timeSeconds, CameraPose& pose) const;

    bool IsEmpty() const { return m_keyframes.empty(); }
    double GetDurationSeconds() const;
    double GetFramesPerSecond() const { return m_framesPerSecond; }

private:
    std::vector<Keyframe> m_keyframes;
    double m_framesPerSecond = 60.0;
    bool m_loop = false;
};
