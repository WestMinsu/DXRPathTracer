#define NOMINMAX
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

namespace
{
    struct HdrImage
    {
        UINT width = 0;
        UINT height = 0;
        std::vector<float> rgb;
    };

    void ThrowIfFailed(HRESULT hr, const char* message)
    {
        if (SUCCEEDED(hr))
            return;

        std::ostringstream error;
        error << message << " HRESULT=0x"
              << std::hex << std::uppercase << static_cast<unsigned long>(hr);
        throw std::runtime_error(error.str());
    }

    std::wstring JoinPath(const std::wstring& lhs, const std::wstring& rhs)
    {
        if (lhs.empty())
            return rhs;

        const wchar_t last = lhs[lhs.size() - 1];
        if (last == L'\\' || last == L'/')
            return lhs + rhs;

        return lhs + std::wstring(1, static_cast<wchar_t>(92)) + rhs;
    }

    std::string ReadPfmHeaderLine(FILE* file)
    {
        char line[512] = {};
        while (std::fgets(line, static_cast<int>(sizeof(line)), file))
        {
            std::string text(line);
            const std::size_t comment = text.find('#');
            if (comment != std::string::npos)
                text.erase(comment);

            const std::size_t first = text.find_first_not_of(" \t\r\n");
            if (first == std::string::npos)
                continue;

            const std::size_t last = text.find_last_not_of(" \t\r\n");
            return text.substr(first, last - first + 1);
        }

        throw std::runtime_error("Unexpected end of PFM header.");
    }

    float ByteSwapFloat(float value)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        bits =
            ((bits & 0x000000FFu) << 24) |
            ((bits & 0x0000FF00u) << 8) |
            ((bits & 0x00FF0000u) >> 8) |
            ((bits & 0xFF000000u) >> 24);
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    HdrImage LoadPfm(const wchar_t* path)
    {
        FILE* file = nullptr;
        if (_wfopen_s(&file, path, L"rb") != 0 || !file)
            throw std::runtime_error("Could not open PFM image.");

        try
        {
            if (ReadPfmHeaderLine(file) != "PF")
                throw std::runtime_error("Only RGB PFM files are supported.");

            const std::string sizeLine = ReadPfmHeaderLine(file);
            unsigned int width = 0;
            unsigned int height = 0;
            if (sscanf_s(sizeLine.c_str(), "%u %u", &width, &height) != 2 ||
                width == 0 || height == 0)
            {
                throw std::runtime_error("Invalid PFM dimensions.");
            }

            const float scale = std::stof(ReadPfmHeaderLine(file));
            if (scale == 0.0f)
                throw std::runtime_error("Invalid PFM scale.");

            const bool fileIsBigEndian = scale > 0.0f;
            const float valueScale = std::abs(scale);
            HdrImage image;
            image.width = width;
            image.height = height;
            image.rgb.resize(static_cast<std::size_t>(width) * height * 3);

            std::vector<float> row(static_cast<std::size_t>(width) * 3);
            for (UINT fileY = 0; fileY < height; ++fileY)
            {
                if (std::fread(row.data(), sizeof(float), row.size(), file) != row.size())
                    throw std::runtime_error("PFM pixel data is truncated.");

                const UINT outputY = height - 1u - fileY;
                float* output = image.rgb.data() +
                    static_cast<std::size_t>(outputY) * width * 3;
                for (std::size_t index = 0; index < row.size(); ++index)
                {
                    float value = fileIsBigEndian ? ByteSwapFloat(row[index]) : row[index];
                    output[index] = value * valueScale;
                }
            }

            std::fclose(file);
            return image;
        }
        catch (...)
        {
            std::fclose(file);
            throw;
        }
    }

    float LinearToSrgb(float value)
    {
        value = std::max(0.0f, std::min(value, 1.0f));
        return value <= 0.0031308f
            ? value * 12.92f
            : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
    }

    uint8_t ToByte(float value)
    {
        value = std::max(0.0f, std::min(value, 1.0f));
        return static_cast<uint8_t>(value * 255.0f + 0.5f);
    }

    void StorePixel(
        std::vector<uint8_t>& pixels,
        UINT width,
        UINT x,
        UINT y,
        float red,
        float green,
        float blue)
    {
        const std::size_t index = (static_cast<std::size_t>(y) * width + x) * 4;
        pixels[index + 0] = ToByte(red);
        pixels[index + 1] = ToByte(green);
        pixels[index + 2] = ToByte(blue);
        pixels[index + 3] = 255;
    }

    float ToneMapChannel(float radiance, float exposureScale)
    {
        const float exposed = std::max(0.0f, radiance) * exposureScale;
        return LinearToSrgb(exposed / (1.0f + exposed));
    }

    float Luminance(const float* rgb)
    {
        return rgb[0] * 0.2126f + rgb[1] * 0.7152f + rgb[2] * 0.0722f;
    }

    void ValidateMatchingImages(const HdrImage& reference, const HdrImage& test)
    {
        if (reference.width != test.width || reference.height != test.height)
            throw std::runtime_error("PFM image dimensions do not match.");
    }

    std::vector<uint8_t> MakeSideBySide(
        const HdrImage& reference,
        const HdrImage& test,
        float exposureEv)
    {
        ValidateMatchingImages(reference, test);
        const UINT outputWidth = reference.width * 2u;
        std::vector<uint8_t> output(
            static_cast<std::size_t>(outputWidth) * reference.height * 4);
        const float exposureScale = std::pow(2.0f, exposureEv);

        for (UINT y = 0; y < reference.height; ++y)
        {
            for (UINT x = 0; x < reference.width; ++x)
            {
                const std::size_t inputIndex =
                    (static_cast<std::size_t>(y) * reference.width + x) * 3;
                StorePixel(
                    output,
                    outputWidth,
                    x,
                    y,
                    ToneMapChannel(reference.rgb[inputIndex + 0], exposureScale),
                    ToneMapChannel(reference.rgb[inputIndex + 1], exposureScale),
                    ToneMapChannel(reference.rgb[inputIndex + 2], exposureScale));
                StorePixel(
                    output,
                    outputWidth,
                    x + reference.width,
                    y,
                    ToneMapChannel(test.rgb[inputIndex + 0], exposureScale),
                    ToneMapChannel(test.rgb[inputIndex + 1], exposureScale),
                    ToneMapChannel(test.rgb[inputIndex + 2], exposureScale));
            }
        }

        return output;
    }

    float FindDifferenceDisplayScale(const HdrImage& reference, const HdrImage& test)
    {
        std::vector<float> differences;
        differences.reserve(static_cast<std::size_t>(reference.width) * reference.height);
        for (std::size_t index = 0; index < reference.rgb.size(); index += 3)
        {
            differences.push_back(std::abs(
                Luminance(test.rgb.data() + index) -
                Luminance(reference.rgb.data() + index)));
        }

        const std::size_t percentileIndex =
            differences.empty() ? 0 : (differences.size() - 1) * 99 / 100;
        std::nth_element(
            differences.begin(),
            differences.begin() + percentileIndex,
            differences.end());
        return std::max(differences[percentileIndex], 1e-6f);
    }

    std::vector<uint8_t> MakeSignedDifference(
        const HdrImage& reference,
        const HdrImage& test,
        float displayScale)
    {
        std::vector<uint8_t> output(
            static_cast<std::size_t>(reference.width) * reference.height * 4);
        for (UINT y = 0; y < reference.height; ++y)
        {
            for (UINT x = 0; x < reference.width; ++x)
            {
                const std::size_t index =
                    (static_cast<std::size_t>(y) * reference.width + x) * 3;
                const float difference =
                    Luminance(test.rgb.data() + index) -
                    Luminance(reference.rgb.data() + index);
                const float magnitude = std::sqrt(std::min(
                    std::abs(difference) / displayScale,
                    1.0f));

                StorePixel(
                    output,
                    reference.width,
                    x,
                    y,
                    difference > 0.0f ? magnitude : 0.0f,
                    0.08f * magnitude,
                    difference < 0.0f ? magnitude : 0.0f);
            }
        }
        return output;
    }

    std::vector<uint8_t> MakeRelativeRatio(
        const HdrImage& reference,
        const HdrImage& test)
    {
        const float epsilon = 1e-4f;
        const float evRange = 2.0f;
        std::vector<uint8_t> output(
            static_cast<std::size_t>(reference.width) * reference.height * 4);
        for (UINT y = 0; y < reference.height; ++y)
        {
            for (UINT x = 0; x < reference.width; ++x)
            {
                const std::size_t index =
                    (static_cast<std::size_t>(y) * reference.width + x) * 3;
                const float referenceY = std::max(0.0f, Luminance(reference.rgb.data() + index));
                const float testY = std::max(0.0f, Luminance(test.rgb.data() + index));
                const float logRatio = std::log2((testY + epsilon) / (referenceY + epsilon));
                const float amount = std::min(std::abs(logRatio) / evRange, 1.0f);
                const float neutral = 0.15f * (1.0f - amount);

                StorePixel(
                    output,
                    reference.width,
                    x,
                    y,
                    neutral + (logRatio > 0.0f ? amount : 0.0f),
                    neutral,
                    neutral + (logRatio < 0.0f ? amount : 0.0f));
            }
        }
        return output;
    }

    void SavePng(
        IWICImagingFactory* factory,
        const std::wstring& path,
        UINT width,
        UINT height,
        const std::vector<uint8_t>& rgba)
    {
        Microsoft::WRL::ComPtr<IWICStream> stream;
        ThrowIfFailed(factory->CreateStream(&stream), "WIC stream creation failed.");
        ThrowIfFailed(
            stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE),
            "PNG file creation failed.");

        Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
        ThrowIfFailed(
            factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder),
            "PNG encoder creation failed.");
        ThrowIfFailed(
            encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache),
            "PNG encoder initialization failed.");

        Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
        ThrowIfFailed(encoder->CreateNewFrame(&frame, nullptr), "PNG frame creation failed.");
        ThrowIfFailed(frame->Initialize(nullptr), "PNG frame initialization failed.");
        ThrowIfFailed(frame->SetSize(width, height), "PNG size setup failed.");

        WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
        ThrowIfFailed(frame->SetPixelFormat(&pixelFormat), "PNG pixel format setup failed.");
        if (!IsEqualGUID(pixelFormat, GUID_WICPixelFormat32bppBGRA))
            throw std::runtime_error("WIC did not accept BGRA8 PNG output.");

        const UINT rowPitch = width * 4u;
        std::vector<uint8_t> bgra = rgba;
        for (std::size_t index = 0; index < bgra.size(); index += 4)
            std::swap(bgra[index + 0], bgra[index + 2]);
        ThrowIfFailed(
            frame->WritePixels(
                height,
                rowPitch,
                static_cast<UINT>(bgra.size()),
                bgra.data()),
            "PNG pixel write failed.");
        ThrowIfFailed(frame->Commit(), "PNG frame commit failed.");
        ThrowIfFailed(encoder->Commit(), "PNG encoder commit failed.");
    }

    void PrintUsage()
    {
        std::wcout
            << L"Usage:\n"
            << L"  ImageCompare.exe <reference.pfm> <test.pfm> [output-folder] [exposure-EV]\n\n"
            << L"Outputs:\n"
            << L"  side_by_side.png       reference on the left, test on the right\n"
            << L"  signed_difference.png  red=test brighter, blue=test darker\n"
            << L"  relative_ratio.png     red=test/reference > 1, blue=< 1\n";
    }
}

int wmain(int argc, wchar_t* argv[])
{
    if (argc < 3 || argc > 5)
    {
        PrintUsage();
        return 1;
    }

    const std::wstring outputFolder = argc >= 4 ? argv[3] : L"ComparisonOutputs";
    const float exposureEv = argc >= 5
        ? static_cast<float>(std::wcstod(argv[4], nullptr))
        : 0.0f;

    if (!CreateDirectoryW(outputFolder.c_str(), nullptr))
    {
        const DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS)
        {
            std::cerr << "Could not create comparison output folder.\n";
            return 1;
        }
    }

    HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitialize = SUCCEEDED(coHr);
    if (FAILED(coHr) && coHr != RPC_E_CHANGED_MODE)
    {
        std::cerr << "COM initialization failed.\n";
        return 1;
    }

    try
    {
        const HdrImage reference = LoadPfm(argv[1]);
        const HdrImage test = LoadPfm(argv[2]);
        ValidateMatchingImages(reference, test);

        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        ThrowIfFailed(
            CoCreateInstance(
                CLSID_WICImagingFactory,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&factory)),
            "WIC factory creation failed.");

        const float differenceScale = FindDifferenceDisplayScale(reference, test);
        SavePng(
            factory.Get(),
            JoinPath(outputFolder, L"side_by_side.png"),
            reference.width * 2u,
            reference.height,
            MakeSideBySide(reference, test, exposureEv));
        SavePng(
            factory.Get(),
            JoinPath(outputFolder, L"signed_difference.png"),
            reference.width,
            reference.height,
            MakeSignedDifference(reference, test, differenceScale));
        SavePng(
            factory.Get(),
            JoinPath(outputFolder, L"relative_ratio.png"),
            reference.width,
            reference.height,
            MakeRelativeRatio(reference, test));

        std::wcout << L"Reference : " << argv[1] << L"\n";
        std::wcout << L"Test      : " << argv[2] << L"\n";
        std::wcout << L"Size      : " << reference.width << L" x " << reference.height << L"\n";
        std::wcout << L"Output    : " << outputFolder << L"\n";
        std::cout << std::fixed << std::setprecision(6)
                  << "Display exposure (EV)       : " << exposureEv << "\n"
                  << "Signed-difference map scale : " << differenceScale
                  << " linear radiance (99th percentile, visualization only)\n"
                  << "Relative-ratio map range    : +/- 2 EV\n";
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Image comparison failed: " << exception.what() << "\n";
        if (shouldUninitialize)
            CoUninitialize();
        return 1;
    }

    if (shouldUninitialize)
        CoUninitialize();
    return 0;
}
