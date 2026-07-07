#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

namespace
{
    const int c_maxParentSearchDepth = 8;

    struct Image
    {
        UINT width = 0;
        UINT height = 0;
        std::vector<uint8_t> rgbaPixels;
    };

    struct ImageError
    {
        double mse = 0.0;
        double psnr = std::numeric_limits<double>::infinity();
        double channelMse[3] = {};
    };

    struct CaptureInfo
    {
        std::wstring path;
        std::wstring fileName;
        int sampleCount = 0;
        uint64_t fileSize = 0;
        FILETIME lastWriteTime = {};
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

    bool IsDirectory(const std::wstring& path)
    {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    std::wstring JoinPath(const std::wstring& lhs, const std::wstring& rhs)
    {
        if (lhs.empty())
            return rhs;

        const wchar_t last = lhs[lhs.size() - 1];
        if (last == L'\\' || last == L'/')
            return lhs + rhs;

        return lhs + L"\\" + rhs;
    }

    std::wstring GetParentPath(const std::wstring& path)
    {
        const std::size_t slash = path.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
            return std::wstring();
        return path.substr(0, slash);
    }

    std::string WideToNarrowLossy(const std::wstring& text)
    {
        std::string result;
        result.reserve(text.size());
        for (wchar_t ch : text)
        {
            result.push_back(ch >= 0 && ch < 128 ? static_cast<char>(ch) : '?');
        }
        return result;
    }

    std::wstring GetCurrentDirectoryPath()
    {
        DWORD length = GetCurrentDirectoryW(0, nullptr);
        if (length == 0)
            throw std::runtime_error("Current directory query failed.");

        std::wstring path(length, L'\0');
        length = GetCurrentDirectoryW(static_cast<DWORD>(path.size()), &path[0]);
        if (length == 0)
            throw std::runtime_error("Current directory query failed.");

        path.resize(length);
        return path;
    }

    std::wstring GetExecutableDirectoryPath()
    {
        std::vector<wchar_t> buffer(MAX_PATH);
        DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        while (length == buffer.size())
        {
            buffer.resize(buffer.size() * 2);
            length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        }

        if (length == 0)
            throw std::runtime_error("Executable path query failed.");

        std::wstring executablePath(buffer.data(), length);
        return GetParentPath(executablePath);
    }

    std::wstring FindCapturesDirectoryFrom(std::wstring directory)
    {
        for (int depth = 0; depth < c_maxParentSearchDepth && !directory.empty(); ++depth)
        {
            const std::wstring capturesPath = JoinPath(directory, L"Captures");
            if (IsDirectory(capturesPath))
                return capturesPath;

            directory = GetParentPath(directory);
        }

        return std::wstring();
    }

    std::wstring FindCapturesDirectory()
    {
        std::wstring capturesPath = FindCapturesDirectoryFrom(GetCurrentDirectoryPath());
        if (!capturesPath.empty())
            return capturesPath;

        capturesPath = FindCapturesDirectoryFrom(GetExecutableDirectoryPath());
        if (!capturesPath.empty())
            return capturesPath;

        return L"Captures";
    }

    int ParseSampleCount(const std::wstring& fileName)
    {
        const std::wstring suffix = L"spp.png";
        if (fileName.size() <= suffix.size())
            return 0;

        const std::size_t suffixPos = fileName.rfind(suffix);
        if (suffixPos == std::wstring::npos)
            return 0;

        std::size_t begin = suffixPos;
        while (begin > 0 && std::iswdigit(fileName[begin - 1]) != 0)
        {
            --begin;
        }

        if (begin == suffixPos)
            return 0;

        return std::stoi(fileName.substr(begin, suffixPos - begin));
    }

    std::vector<CaptureInfo> LoadCaptureList(const std::wstring& capturesDirectory)
    {
        std::vector<CaptureInfo> captures;

        WIN32_FIND_DATAW findData = {};
        const std::wstring searchPath = JoinPath(capturesDirectory, L"*.png");
        HANDLE findHandle = FindFirstFileW(searchPath.c_str(), &findData);
        if (findHandle == INVALID_HANDLE_VALUE)
            return captures;

        do
        {
            if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                continue;

            const uint64_t fileSize =
                (static_cast<uint64_t>(findData.nFileSizeHigh) << 32) |
                static_cast<uint64_t>(findData.nFileSizeLow);
            if (fileSize == 0)
                continue;

            CaptureInfo capture;
            capture.fileName = findData.cFileName;
            capture.path = JoinPath(capturesDirectory, capture.fileName);
            capture.sampleCount = ParseSampleCount(capture.fileName);
            capture.fileSize = fileSize;
            capture.lastWriteTime = findData.ftLastWriteTime;
            captures.push_back(capture);
        } while (FindNextFileW(findHandle, &findData));

        FindClose(findHandle);

        std::sort(captures.begin(), captures.end(), [](const CaptureInfo& lhs, const CaptureInfo& rhs)
        {
            if (lhs.sampleCount != rhs.sampleCount)
                return lhs.sampleCount < rhs.sampleCount;
            return CompareFileTime(&lhs.lastWriteTime, &rhs.lastWriteTime) < 0;
        });

        return captures;
    }

    CaptureInfo SelectReferenceCapture(const std::vector<CaptureInfo>& captures)
    {
        return *std::max_element(captures.begin(), captures.end(), [](const CaptureInfo& lhs, const CaptureInfo& rhs)
        {
            if (lhs.sampleCount != rhs.sampleCount)
                return lhs.sampleCount < rhs.sampleCount;
            return CompareFileTime(&lhs.lastWriteTime, &rhs.lastWriteTime) < 0;
        });
    }

    Image LoadImage(IWICImagingFactory* factory, const wchar_t* filePath)
    {
        Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
        ThrowIfFailed(
            factory->CreateDecoderFromFilename(
                filePath,
                nullptr,
                GENERIC_READ,
                WICDecodeMetadataCacheOnLoad,
                &decoder),
            "Image decoder creation failed.");

        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
        ThrowIfFailed(decoder->GetFrame(0, &frame), "Image frame loading failed.");

        Image image;
        ThrowIfFailed(frame->GetSize(&image.width, &image.height), "Image size query failed.");

        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
        ThrowIfFailed(factory->CreateFormatConverter(&converter), "Format converter creation failed.");
        ThrowIfFailed(
            converter->Initialize(
                frame.Get(),
                GUID_WICPixelFormat32bppRGBA,
                WICBitmapDitherTypeNone,
                nullptr,
                0.0,
                WICBitmapPaletteTypeCustom),
            "Image format conversion failed.");

        const UINT rowPitch = image.width * 4;
        const UINT imageSize = rowPitch * image.height;
        image.rgbaPixels.resize(imageSize);
        ThrowIfFailed(
            converter->CopyPixels(nullptr, rowPitch, imageSize, image.rgbaPixels.data()),
            "Image pixel copy failed.");

        return image;
    }

    ImageError CompareImages(const Image& referenceImage, const Image& testImage)
    {
        if (referenceImage.width != testImage.width || referenceImage.height != testImage.height)
            throw std::runtime_error("Image sizes do not match.");

        const uint64_t pixelCount = static_cast<uint64_t>(referenceImage.width) * referenceImage.height;
        const uint64_t channelCount = pixelCount * 3;

        double channelErrorSum[3] = {};
        double totalErrorSum = 0.0;

        for (uint64_t pixelIndex = 0; pixelIndex < pixelCount; ++pixelIndex)
        {
            const uint64_t baseIndex = pixelIndex * 4;
            for (int channel = 0; channel < 3; ++channel)
            {
                const double referenceValue = referenceImage.rgbaPixels[baseIndex + channel] / 255.0;
                const double testValue = testImage.rgbaPixels[baseIndex + channel] / 255.0;
                const double difference = referenceValue - testValue;
                const double squaredError = difference * difference;
                channelErrorSum[channel] += squaredError;
                totalErrorSum += squaredError;
            }
        }

        ImageError error;
        error.mse = totalErrorSum / static_cast<double>(channelCount);
        for (int channel = 0; channel < 3; ++channel)
        {
            error.channelMse[channel] = channelErrorSum[channel] / static_cast<double>(pixelCount);
        }

        if (error.mse > 0.0)
        {
            error.psnr = 10.0 * std::log10(1.0 / error.mse);
        }

        return error;
    }

    void PrintSingleComparison(
        IWICImagingFactory* factory,
        const wchar_t* referencePath,
        const wchar_t* testPath)
    {
        const Image referenceImage = LoadImage(factory, referencePath);
        const Image testImage = LoadImage(factory, testPath);
        const ImageError error = CompareImages(referenceImage, testImage);

        std::wcout << L"Reference : " << referencePath << L"\n";
        std::wcout << L"Test      : " << testPath << L"\n";
        std::wcout << L"Size      : " << referenceImage.width << L" x " << referenceImage.height << L"\n\n";

        std::cout << std::fixed << std::setprecision(10);
        std::cout << "MSE       : " << error.mse << "\n";
        std::cout << "MSE R/G/B : "
                  << error.channelMse[0] << " / "
                  << error.channelMse[1] << " / "
                  << error.channelMse[2] << "\n";

        if (std::isinf(error.psnr))
            std::cout << "PSNR      : inf dB\n";
        else
            std::cout << "PSNR      : " << std::setprecision(4) << error.psnr << " dB\n";
    }

    void PrintBatchComparison(IWICImagingFactory* factory, const std::wstring& capturesDirectory)
    {
        const std::vector<CaptureInfo> captures = LoadCaptureList(capturesDirectory);
        if (captures.empty())
        {
            std::ostringstream message;
            message << "Need at least one non-empty PNG file in " << WideToNarrowLossy(capturesDirectory);
            throw std::runtime_error(message.str());
        }

        const CaptureInfo reference = SelectReferenceCapture(captures);
        const Image referenceImage = LoadImage(factory, reference.path.c_str());

        std::wcout << L"Captures  : " << capturesDirectory << L"\n";
        std::wcout << L"Reference : " << reference.fileName
                   << L" (" << reference.sampleCount << L" spp)\n";
        std::wcout << L"Size      : " << referenceImage.width << L" x " << referenceImage.height << L"\n\n";
        std::wcout << std::left
                   << std::setw(44) << L"Test"
                   << std::right
                   << std::setw(8) << L"SPP"
                   << std::setw(18) << L"MSE"
                   << std::setw(14) << L"PSNR\n";
        std::wcout << L"------------------------------------------------------------------------------------\n";

        for (const CaptureInfo& capture : captures)
        {
            const Image testImage = LoadImage(factory, capture.path.c_str());
            const ImageError error = CompareImages(referenceImage, testImage);

            std::wcout << std::left << std::setw(44) << capture.fileName
                       << std::right << std::setw(8) << capture.sampleCount
                       << std::setw(18) << std::fixed << std::setprecision(10) << error.mse;

            if (std::isinf(error.psnr))
                std::wcout << std::setw(14) << L"inf";
            else
                std::wcout << std::setw(14) << std::fixed << std::setprecision(4) << error.psnr;

            std::wcout << L"\n";
        }
    }

    void PrintUsage()
    {
        std::wcout << L"Usage:\n"
                   << L"  ImageCompare.exe\n"
                   << L"  ImageCompare.exe <captures-folder>\n"
                   << L"  ImageCompare.exe <reference.png> <test.png>\n\n"
                   << L"No-argument mode finds the nearest Captures folder, selects the highest spp PNG\n"
                   << L"as reference, and compares every non-empty PNG in that folder.\n\n"
                   << L"Output:\n"
                   << L"  MSE  : mean squared error in normalized RGB space [0, 1]\n"
                   << L"  PSNR : 10 * log10(1 / MSE), higher is closer to reference\n";
    }
}

int wmain(int argc, wchar_t* argv[])
{
    if (argc > 3)
    {
        PrintUsage();
        return 1;
    }

    HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitialize = SUCCEEDED(coHr);
    if (FAILED(coHr) && coHr != RPC_E_CHANGED_MODE)
    {
        std::cerr << "COM initialization failed. HRESULT=0x"
                  << std::hex << std::uppercase << static_cast<unsigned long>(coHr) << "\n";
        return 1;
    }

    try
    {
        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        ThrowIfFailed(
            CoCreateInstance(
                CLSID_WICImagingFactory,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&factory)),
            "WIC factory creation failed.");

        if (argc == 1)
        {
            PrintBatchComparison(factory.Get(), FindCapturesDirectory());
        }
        else if (argc == 2)
        {
            if (!IsDirectory(argv[1]))
            {
                PrintUsage();
                throw std::runtime_error("One-argument mode expects a captures folder.");
            }
            PrintBatchComparison(factory.Get(), argv[1]);
        }
        else
        {
            PrintSingleComparison(factory.Get(), argv[1], argv[2]);
        }
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