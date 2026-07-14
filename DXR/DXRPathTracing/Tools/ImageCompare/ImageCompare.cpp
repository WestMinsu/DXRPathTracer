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
#include <limits>
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

    struct RegionDiagnostic
    {
        std::string name;
        UINT x = 0;
        UINT y = 0;
        UINT width = 0;
        UINT height = 0;
        float referenceLuminance = 0.0f;
        float testLuminance = 0.0f;
        float ratio = 0.0f;
    };

    struct BlockDiagnostic
    {
        UINT column = 0;
        UINT row = 0;
        UINT x = 0;
        UINT y = 0;
        UINT width = 0;
        UINT height = 0;
        float referenceLuminance = 0.0f;
        float testLuminance = 0.0f;
        float ratio = 0.0f;
    };

    struct SpatialDiagnostics
    {
        float referenceLuminance = 0.0f;
        float testLuminance = 0.0f;
        float ratio = 0.0f;
        std::vector<RegionDiagnostic> regions;
        std::vector<BlockDiagnostic> blocks;
        UINT validBlockCount = 0;
        float blockRatioP10 = 0.0f;
        float blockRatioMedian = 0.0f;
        float blockRatioP90 = 0.0f;
    };

    constexpr UINT c_regionColumns = 3;
    constexpr UINT c_regionRows = 3;
    constexpr UINT c_blockSize = 30;
    constexpr float c_minimumReferenceLuminance = 1e-4f;

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

    void ValidateMatchingImages(const HdrImage& reference, const HdrImage& test);

    float MeanLuminance(
        const HdrImage& image,
        UINT x,
        UINT y,
        UINT width,
        UINT height)
    {
        double sum = 0.0;
        for (UINT row = y; row < y + height; ++row)
        {
            for (UINT column = x; column < x + width; ++column)
            {
                const std::size_t index =
                    (static_cast<std::size_t>(row) * image.width + column) * 3;
                sum += Luminance(image.rgb.data() + index);
            }
        }

        return static_cast<float>(sum / static_cast<double>(width) / height);
    }

    float LuminanceRatio(float referenceLuminance, float testLuminance)
    {
        const float reference = std::max(referenceLuminance, c_minimumReferenceLuminance);
        const float test = std::max(testLuminance, c_minimumReferenceLuminance);
        return test / reference;
    }

    float Percentile(const std::vector<float>& values, float percentile)
    {
        if (values.empty())
            return 0.0f;

        std::vector<float> sorted = values;
        std::sort(sorted.begin(), sorted.end());

        const float position = (static_cast<float>(sorted.size()) - 1.0f) * percentile;
        const std::size_t lower = static_cast<std::size_t>(position);
        const std::size_t upper = std::min(lower + 1, sorted.size() - 1);
        const float fraction = position - static_cast<float>(lower);
        return sorted[lower] * (1.0f - fraction) + sorted[upper] * fraction;
    }

    SpatialDiagnostics ComputeSpatialDiagnostics(
        const HdrImage& reference,
        const HdrImage& test)
    {
        ValidateMatchingImages(reference, test);

        SpatialDiagnostics diagnostics;
        diagnostics.referenceLuminance =
            MeanLuminance(reference, 0, 0, reference.width, reference.height);
        diagnostics.testLuminance =
            MeanLuminance(test, 0, 0, test.width, test.height);
        diagnostics.ratio =
            LuminanceRatio(diagnostics.referenceLuminance, diagnostics.testLuminance);
        constexpr const char* rowNames[c_regionRows] = { "top", "middle", "bottom" };
        constexpr const char* columnNames[c_regionColumns] = { "left", "center", "right" };

        for (UINT row = 0; row < c_regionRows; ++row)
        {
            const UINT y = row * reference.height / c_regionRows;
            const UINT nextY = (row + 1) * reference.height / c_regionRows;
            for (UINT column = 0; column < c_regionColumns; ++column)
            {
                const UINT x = column * reference.width / c_regionColumns;
                const UINT nextX = (column + 1) * reference.width / c_regionColumns;

                RegionDiagnostic region;
                region.name = std::string(rowNames[row]) + "-" + columnNames[column];
                region.x = x;
                region.y = y;
                region.width = nextX - x;
                region.height = nextY - y;
                region.referenceLuminance =
                    MeanLuminance(reference, x, y, region.width, region.height);
                region.testLuminance =
                    MeanLuminance(test, x, y, region.width, region.height);
                region.ratio = LuminanceRatio(region.referenceLuminance, region.testLuminance);
                diagnostics.regions.push_back(region);
            }
        }

        std::vector<float> validBlockRatios;
        for (UINT y = 0, row = 0; y < reference.height; y += c_blockSize, ++row)
        {
            const UINT blockHeight = std::min(c_blockSize, reference.height - y);
            for (UINT x = 0, column = 0; x < reference.width; x += c_blockSize, ++column)
            {
                const UINT blockWidth = std::min(c_blockSize, reference.width - x);
                BlockDiagnostic block;
                block.column = column;
                block.row = row;
                block.x = x;
                block.y = y;
                block.width = blockWidth;
                block.height = blockHeight;
                block.referenceLuminance =
                    MeanLuminance(reference, x, y, blockWidth, blockHeight);
                block.testLuminance =
                    MeanLuminance(test, x, y, blockWidth, blockHeight);
                block.ratio = LuminanceRatio(block.referenceLuminance, block.testLuminance);
                if (block.referenceLuminance > c_minimumReferenceLuminance)
                    validBlockRatios.push_back(block.ratio);
                diagnostics.blocks.push_back(block);
            }
        }

        diagnostics.validBlockCount = static_cast<UINT>(validBlockRatios.size());
        diagnostics.blockRatioP10 = Percentile(validBlockRatios, 0.10f);
        diagnostics.blockRatioMedian = Percentile(validBlockRatios, 0.50f);
        diagnostics.blockRatioP90 = Percentile(validBlockRatios, 0.90f);
        return diagnostics;
    }

    void ValidateMatchingImages(const HdrImage& reference, const HdrImage& test)
    {
        if (reference.width != test.width || reference.height != test.height)
            throw std::runtime_error("PFM image dimensions do not match.");
    }

    std::vector<uint8_t> MakeSideBySide(
        const HdrImage& reference,
        const HdrImage& test,
        float displayExposureStops)
    {
        ValidateMatchingImages(reference, test);
        const UINT outputWidth = reference.width * 2u;
        std::vector<uint8_t> output(
            static_cast<std::size_t>(outputWidth) * reference.height * 4);
        const float exposureScale = std::pow(2.0f, displayExposureStops);

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
        const float log2RatioRange = 2.0f;
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
                const float amount = std::min(std::abs(logRatio) / log2RatioRange, 1.0f);
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

    void WriteSpatialDiagnostics(
        const std::wstring& outputFolder,
        const SpatialDiagnostics& diagnostics)
    {
        const std::wstring markdownPath = JoinPath(outputFolder, L"spatial_diagnostics.md");
        FILE* markdown = nullptr;
        if (_wfopen_s(&markdown, markdownPath.c_str(), L"wb") != 0 || !markdown)
            throw std::runtime_error("Could not create spatial diagnostics Markdown file.");

        try
        {
            std::fprintf(markdown,
                "# Spatial HDR diagnostics\n\n"
                "All values use linear RGB luminance. They are spatial transport diagnostics, "
                "not MSE, PSNR, or a single image-quality score.\n\n"
                "## Whole image\n\n"
                "| Test mean | Reference mean | Test / Reference |\n"
                "| ---: | ---: | ---: |\n"
                "| %.6f | %.6f | %.6f |\n\n"
                "## 3 x 3 regional luminance\n\n"
                "| Region | Test mean | Reference mean | Test / Reference |\n"
                "| --- | ---: | ---: | ---: |\n",
                diagnostics.testLuminance,
                diagnostics.referenceLuminance,
                diagnostics.ratio);
            for (const RegionDiagnostic& region : diagnostics.regions)
            {
                std::fprintf(
                    markdown,
                    "| %s | %.6f | %.6f | %.6f |\n",
                    region.name.c_str(),
                    region.testLuminance,
                    region.referenceLuminance,
                    region.ratio);
            }

            std::fprintf(markdown,
                "\n## 30 x 30 pixel block Test / Reference distribution\n\n"
                "Only blocks with Reference mean above %.6f are included "
                "(%u of %zu blocks).\n\n"
                "| p10 | median | p90 |\n"
                "| ---: | ---: | ---: |\n"
                "| %.6f | %.6f | %.6f |\n\n"
                "Detailed block Test mean, Reference mean, and Test / Reference values "
                "are stored in `block_ratio.csv`.\n",
                c_minimumReferenceLuminance,
                diagnostics.validBlockCount,
                diagnostics.blocks.size(),
                diagnostics.blockRatioP10,
                diagnostics.blockRatioMedian,
                diagnostics.blockRatioP90);
            std::fclose(markdown);
        }
        catch (...)
        {
            std::fclose(markdown);
            throw;
        }

        const std::wstring csvPath = JoinPath(outputFolder, L"block_ratio.csv");
        FILE* csv = nullptr;
        if (_wfopen_s(&csv, csvPath.c_str(), L"wb") != 0 || !csv)
            throw std::runtime_error("Could not create block ratio CSV file.");

        try
        {
            std::fprintf(
                csv,
                "column,row,x,y,width,height,test_mean_luminance,"
                "reference_mean_luminance,test_to_reference_ratio\n");
            for (const BlockDiagnostic& block : diagnostics.blocks)
            {
                std::fprintf(
                    csv,
                    "%u,%u,%u,%u,%u,%u,%.8f,%.8f,%.8f\n",
                    block.column,
                    block.row,
                    block.x,
                    block.y,
                    block.width,
                    block.height,
                    block.testLuminance,
                    block.referenceLuminance,
                    block.ratio);
            }
            std::fclose(csv);
        }
        catch (...)
        {
            std::fclose(csv);
            throw;
        }
    }

    void WriteUtf8(FILE* file, const std::wstring& text)
    {
        if (text.empty())
            return;

        const int byteCount = WideCharToMultiByte(
            CP_UTF8,
            0,
            text.data(),
            static_cast<int>(text.size()),
            nullptr,
            0,
            nullptr,
            nullptr);
        if (byteCount <= 0)
            throw std::runtime_error("Could not convert Korean presentation summary to UTF-8.");

        std::vector<char> utf8(static_cast<std::size_t>(byteCount));
        if (WideCharToMultiByte(
                CP_UTF8,
                0,
                text.data(),
                static_cast<int>(text.size()),
                utf8.data(),
                byteCount,
                nullptr,
                nullptr) != byteCount ||
            std::fwrite(utf8.data(), 1, utf8.size(), file) != utf8.size())
        {
            throw std::runtime_error("Could not write Korean presentation summary.");
        }
    }

    void WritePresentationSummaryKorean(
        const std::wstring& outputFolder,
        const HdrImage& reference,
        const SpatialDiagnostics& diagnostics)
    {
        const std::wstring path = JoinPath(outputFolder, L"presentation_summary_ko.md");
        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file)
            throw std::runtime_error("Could not create Korean presentation summary.");

        try
        {
            constexpr uint8_t utf8Bom[] = { 0xEF, 0xBB, 0xBF };
            if (std::fwrite(utf8Bom, 1, sizeof(utf8Bom), file) != sizeof(utf8Bom))
                throw std::runtime_error("Could not write Korean presentation summary UTF-8 BOM.");

            std::wostringstream summary;
            summary << std::fixed << std::setprecision(6);
            summary << L"# \uBC1C\uD45C\uC6A9 HDR \uBE44\uAD50 \uC694\uC57D\n\n";
            summary << L"## \uD575\uC2EC \uC218\uCE58\n\n";
            summary << L"- \uD574\uC0C1\uB3C4: "
                    << reference.width << L" x " << reference.height << L"\n";
            summary << L"- Test mean: " << diagnostics.testLuminance << L"\n";
            summary << L"- Reference mean: " << diagnostics.referenceLuminance << L"\n";
            summary << L"- Test / Reference: " << diagnostics.ratio << L"\n\n";
            summary << L"## \uD574\uC11D\n\n";
            summary << L"- Test / Reference\uAC00 1.0\uC5D0 \uAC00\uAE4C\uC6B8\uC218\uB85D "
                    << L"\uC804\uCCB4 \uD3C9\uADE0 \uBC1D\uAE30\uAC00 \uC77C\uCE58\uD569\uB2C8\uB2E4.\n\n";
            summary << L"## \uBC1C\uD45C \uBB38\uC7A5\n\n";
            summary << L"> \uC120\uD615 HDR PFM\uC5D0\uC11C \uC804\uCCB4 \uC774\uBBF8\uC9C0\uC758 \uD3C9\uADE0 \uD718\uB3C4\uB97C \uBE44\uAD50\uD588\uC2B5\uB2C8\uB2E4. "
                    << L"Test mean\uC740 " << diagnostics.testLuminance
                    << L", Reference mean\uC740 " << diagnostics.referenceLuminance
                    << L", Test / Reference\uB294 " << diagnostics.ratio << L"\uC785\uB2C8\uB2E4.\n";
            WriteUtf8(file, summary.str());
            std::fclose(file);
        }
        catch (...)
        {
            std::fclose(file);
            throw;
        }
    }

    void PrintUsage()
    {
        std::wcout
            << L"Usage:\n"
            << L"  ImageCompare.exe <reference.pfm> <test.pfm> [output-folder] [display-exposure-stops]\n\n"
            << L"Outputs:\n"
            << L"  side_by_side.png       reference on the left, test on the right\n"
            << L"  signed_difference.png  red=test brighter, blue=test darker\n"
            << L"  relative_ratio.png     red=test/reference > 1, blue=< 1\n"
            << L"  spatial_diagnostics.md whole-image and regional mean/ratio tables\n"
            << L"  block_ratio.csv        per-30x30-pixel-block mean/ratio diagnostics\n"
            << L"  presentation_summary_ko.md concise Korean presentation summary\n";
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
    const float displayExposureStops = argc >= 5
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
        const SpatialDiagnostics diagnostics = ComputeSpatialDiagnostics(reference, test);

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
            MakeSideBySide(reference, test, displayExposureStops));
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
        WriteSpatialDiagnostics(outputFolder, diagnostics);
        WritePresentationSummaryKorean(outputFolder, reference, diagnostics);

        std::wcout << L"Reference : " << argv[1] << L"\n";
        std::wcout << L"Test      : " << argv[2] << L"\n";
        std::wcout << L"Size      : " << reference.width << L" x " << reference.height << L"\n";
        std::wcout << L"Output    : " << outputFolder << L"\n";
        std::cout << std::fixed << std::setprecision(6)
                  << "Display exposure (stops)    : " << displayExposureStops << "\n"
                  << "Signed-difference map scale : " << differenceScale
                  << " linear radiance (99th percentile, visualization only)\n"
                  << "Relative-ratio map range    : 0.25x to 4.00x\n"
                  << "Test mean                   : " << diagnostics.testLuminance << "\n"
                  << "Reference mean              : " << diagnostics.referenceLuminance << "\n"
                  << "Test / Reference            : " << diagnostics.ratio << "\n"
                  << "Numeric outputs             : spatial_diagnostics.md, block_ratio.csv, "
                     "presentation_summary_ko.md\n";
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
