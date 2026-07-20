#include <Windows.h>
#include <windowsx.h>
#include <shellapi.h>

#include <string>

#include "D3D12Renderer.h"
#include "ThirdParty/imgui/imgui.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

namespace
{
    constexpr wchar_t c_windowTitle[] = L"DXRPathTracing";
    constexpr wchar_t c_windowClassName[] = L"DXRPathTracingWindow";

    D3D12Renderer gRenderer;
    bool gRendererReady = false;

    struct AppOptions
    {
        UINT width = 960;
        UINT height = 540;
        UINT captureSamples = 0;
        UINT maxBounce = 8;
        UINT sceneType = RayTracingManager::c_scenePbrGgx;
        UINT pbrDebugView = RayTracingManager::c_pbrDebugBeauty;
        float pbrMetallic = 1.0f;
        float pbrRoughness = 0.35f;
        bool overridePbrMaterial = false;
        bool enableIbl = true;
        float iblIntensity = 0.5f;
        UINT validationSeed = 0;
        bool headless = false;
        bool composeModelRoom = false;
        bool sponzaLite = true;
        bool cameraPathSpecified = false;
        bool vsync = true;
        bool vsyncSpecified = false;
        bool benchmark = false;
        UINT benchmarkFrames = 600;
        bool benchmarkFramesSpecified = false;
        bool collectRayStatistics = false;
        bool rayStatisticsSpecified = false;
        std::wstring benchmarkOutput;
        std::wstring cameraPathFilePath;
        std::wstring outputPrefix;
        std::wstring sceneFilePath;
        std::wstring sceneManifestPath;
        std::wstring sponzaLightConfigPath;
    };

    AppOptions gOptions;

    ATOM RegisterMainWindowClass(HINSTANCE hInstance);
    bool CreateMainWindow(HINSTANCE hInstance, int nCmdShow);
    void ParseCommandLine();
    std::wstring ResolveBundledInputPath(const std::wstring& relativePath);
    LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
}

int APIENTRY wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine,
    _In_ int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    ParseCommandLine();
    RegisterMainWindowClass(hInstance);

    if (!CreateMainWindow(hInstance, nCmdShow))
        return FALSE;

    MSG msg = {};
    while (msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else
        {
            gRenderer.Render();
        }
    }

    gRenderer.WaitForGpu();
    return static_cast<int>(msg.wParam);
}


namespace
{
    std::wstring ResolveBundledInputPath(const std::wstring& relativePath)
    {
        if (GetFileAttributesW(relativePath.c_str()) !=
            INVALID_FILE_ATTRIBUTES)
        {
            return relativePath;
        }

        std::wstring modulePath(32768, L'\0');
        const DWORD moduleLength = GetModuleFileNameW(
            nullptr,
            &modulePath[0],
            static_cast<DWORD>(modulePath.size()));
        if (moduleLength == 0 || moduleLength >= modulePath.size())
            return relativePath;
        modulePath.resize(moduleLength);
        const std::wstring::size_type slash =
            modulePath.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
            return relativePath;

        const std::wstring candidate =
            modulePath.substr(0, slash + 1) +
            L"..\\.." +
            std::wstring(1, static_cast<wchar_t>(0x5C)) +
            relativePath;
        /*
            L"..\\..\" + relativePath;
        */
        const DWORD requiredLength =
            GetFullPathNameW(candidate.c_str(), 0, nullptr, nullptr);
        if (requiredLength == 0)
            return relativePath;
        std::wstring absolutePath(requiredLength, L'\0');
        const DWORD writtenLength = GetFullPathNameW(
            candidate.c_str(),
            requiredLength,
            &absolutePath[0],
            nullptr);
        if (writtenLength == 0 || writtenLength >= requiredLength)
            return relativePath;
        absolutePath.resize(writtenLength);
        return GetFileAttributesW(absolutePath.c_str()) !=
            INVALID_FILE_ATTRIBUTES
            ? absolutePath
            : relativePath;
    }

    void ParseCommandLine()
    {
        int argumentCount = 0;
        wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
        if (!arguments)
            return;

        for (int index = 1; index < argumentCount; ++index)
        {
            const std::wstring argument = arguments[index];
            if (argument == L"--width" && index + 1 < argumentCount)
            {
                gOptions.width = static_cast<UINT>(_wtoi(arguments[++index]));
            }
            else if (argument == L"--height" && index + 1 < argumentCount)
            {
                gOptions.height = static_cast<UINT>(_wtoi(arguments[++index]));
            }
            else if (argument == L"--capture-samples" && index + 1 < argumentCount)
            {
                gOptions.captureSamples = static_cast<UINT>(_wtoi(arguments[++index]));
            }
            else if (argument == L"--max-bounce" && index + 1 < argumentCount)
            {
                gOptions.maxBounce = static_cast<UINT>(_wtoi(arguments[++index]));
            }
            else if (argument == L"--scene" && index + 1 < argumentCount)
            {
                gOptions.sponzaLite = false;
                const std::wstring scene = arguments[++index];
                if (scene == L"pbr")
                {
                    gOptions.sceneType = RayTracingManager::c_scenePbrGgx;
                }
                else if (scene == L"pbr-validation")
                {
                    gOptions.sceneType =
                        RayTracingManager::c_scenePbrGpuValidation;
                }
                else
                {
                    gOptions.sceneType = RayTracingManager::c_sceneCornellBox;
                }
            }
            else if (argument == L"--gpu-brdf-validation")
            {
                gOptions.sponzaLite = false;
                gOptions.sceneType =
                    RayTracingManager::c_scenePbrGpuValidation;
            }
            else if (argument == L"--validation-seed" && index + 1 < argumentCount)
            {
                gOptions.validationSeed = static_cast<UINT>(_wcstoui64(
                    arguments[++index],
                    nullptr,
                    0));
            }
            else if (argument == L"--pbr-debug" && index + 1 < argumentCount)
            {
                const std::wstring debugView = arguments[++index];
                if (debugView == L"albedo")
                {
                    gOptions.pbrDebugView = RayTracingManager::c_pbrDebugAlbedo;
                }
                else if (debugView == L"metallic")
                {
                    gOptions.pbrDebugView = RayTracingManager::c_pbrDebugMetallic;
                }
                else if (debugView == L"roughness")
                {
                    gOptions.pbrDebugView = RayTracingManager::c_pbrDebugRoughness;
                }
                else if (debugView == L"depth")
                {
                    gOptions.pbrDebugView = RayTracingManager::c_pbrDebugDepth;
                }
                else if (debugView == L"material-id" || debugView == L"material")
                {
                    gOptions.pbrDebugView = RayTracingManager::c_pbrDebugMaterialId;
                }
                else if (debugView == L"normal")
                {
                    gOptions.pbrDebugView = RayTracingManager::c_pbrDebugNormal;
                }
                else
                {
                    gOptions.pbrDebugView = RayTracingManager::c_pbrDebugBeauty;
                }
            }
            else if (argument == L"--pbr-metallic" && index + 1 < argumentCount)
            {
                gOptions.pbrMetallic = static_cast<float>(_wtof(arguments[++index]));
            }
            else if (argument == L"--pbr-roughness" && index + 1 < argumentCount)
            {
                gOptions.pbrRoughness = static_cast<float>(_wtof(arguments[++index]));
            }
            else if (argument == L"--override-pbr-material")
            {
                gOptions.overridePbrMaterial = true;
            }
            else if (argument == L"--ibl-intensity" && index + 1 < argumentCount)
            {
                gOptions.iblIntensity = static_cast<float>(_wtof(arguments[++index]));
            }
            else if (argument == L"--disable-ibl")
            {
                gOptions.enableIbl = false;
            }
            else if (argument == L"--output-prefix" && index + 1 < argumentCount)
            {
                gOptions.outputPrefix = arguments[++index];
            }
            else if ((argument == L"--model" || argument == L"--gltf") &&
                     index + 1 < argumentCount)
            {
                gOptions.sceneFilePath = arguments[++index];
                gOptions.sponzaLite = false;
            }
            else if (argument == L"--model-room")
            {
                gOptions.composeModelRoom = true;
            }
            else if (argument == L"--sponza-lite")
            {
                gOptions.sponzaLite = true;
            }
            else if (argument == L"--no-sponza-lite")
            {
                gOptions.sponzaLite = false;
            }
            else if (argument == L"--scene-manifest" &&
                     index + 1 < argumentCount)
            {
                gOptions.sceneManifestPath = arguments[++index];
            }
            else if (argument == L"--sponza-lights" &&
                     index + 1 < argumentCount)
            {
                gOptions.sponzaLightConfigPath = arguments[++index];
            }
            else if (argument == L"--headless")
            {
                gOptions.headless = true;
            }
            else if (argument == L"--vsync" && index + 1 < argumentCount)
            {
                gOptions.vsync = _wtoi(arguments[++index]) != 0;
                gOptions.vsyncSpecified = true;
            }
            else if (argument == L"--benchmark")
            {
                gOptions.benchmark = true;
            }
            else if (argument == L"--benchmark-output" &&
                     index + 1 < argumentCount)
            {
                gOptions.benchmarkOutput = arguments[++index];
            }
            else if (argument == L"--benchmark-frames" &&
                     index + 1 < argumentCount)
            {
                gOptions.benchmarkFrames = static_cast<UINT>(
                    _wtoi(arguments[++index]));
                gOptions.benchmarkFramesSpecified = true;
            }
            else if (argument == L"--ray-stats" &&
                     index + 1 < argumentCount)
            {
                gOptions.collectRayStatistics = _wtoi(arguments[++index]) != 0;
                gOptions.rayStatisticsSpecified = true;
            }
            else if (argument == L"--camera-path" &&
                     index + 1 < argumentCount)
            {
                gOptions.cameraPathFilePath = arguments[++index];
                gOptions.cameraPathSpecified = true;
            }
        }

        if (gOptions.width == 0)
            gOptions.width = 1;
        if (gOptions.height == 0)
            gOptions.height = 1;
        if (!gOptions.sceneFilePath.empty())
            gOptions.sceneType = RayTracingManager::c_scenePbrGgx;
        if (gOptions.sponzaLite)
        {
            gOptions.sceneType = RayTracingManager::c_scenePbrGgx;
            if (gOptions.sceneFilePath.empty())
            {
                gOptions.sceneFilePath =
                    ResolveBundledInputPath(
                        L"Assets\\KhronosGlTFSampleAssets\\Models\\Sponza\\glTF\\Sponza.gltf");
            }
            if (gOptions.sceneManifestPath.empty())
            {
                gOptions.sceneManifestPath =
                    L"BenchmarkOutput\\SponzaLite\\scene_manifest.json";
            }
            if (gOptions.sponzaLightConfigPath.empty())
            {
                gOptions.sponzaLightConfigPath =
                    ResolveBundledInputPath(
                        L"Config\\sponza_lights.json");
            }
            if (gOptions.cameraPathFilePath.empty())
            {
                gOptions.cameraPathFilePath =
                    ResolveBundledInputPath(
                        L"Config\\sponza_camera_path.json");
            }
        }
        if (gOptions.benchmark && !gOptions.vsyncSpecified)
            gOptions.vsync = false;
        if (gOptions.benchmark && !gOptions.rayStatisticsSpecified)
            gOptions.collectRayStatistics = true;
        if (gOptions.benchmark &&
            gOptions.cameraPathSpecified &&
            !gOptions.benchmarkFramesSpecified)
        {
            gOptions.benchmarkFrames = 0;
        }

        LocalFree(arguments);
    }

    ATOM RegisterMainWindowClass(HINSTANCE hInstance)
    {
        WNDCLASSEXW windowClass = {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = WndProc;
        windowClass.hInstance = hInstance;
        windowClass.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        windowClass.lpszClassName = c_windowClassName;
        windowClass.hIconSm = LoadIcon(nullptr, IDI_APPLICATION);

        return RegisterClassExW(&windowClass);
    }

    bool CreateMainWindow(HINSTANCE hInstance, int nCmdShow)
    {
        RECT windowRect =
        {
            0,
            0,
            static_cast<LONG>(gOptions.width),
            static_cast<LONG>(gOptions.height)
        };
        AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);

        HWND hWnd = CreateWindowW(
            c_windowClassName,
            c_windowTitle,
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            0,
            windowRect.right - windowRect.left,
            windowRect.bottom - windowRect.top,
            nullptr,
            nullptr,
            hInstance,
            nullptr);

        if (!hWnd)
            return false;

        gRenderer.SetSceneFilePath(gOptions.sceneFilePath);
        gRenderer.SetInitialSceneType(gOptions.sceneType);
        gRenderer.SetComposeModelRoom(gOptions.composeModelRoom);
        gRenderer.SetSponzaLite(gOptions.sponzaLite);
        gRenderer.SetSceneManifestPath(gOptions.sceneManifestPath);
        gRenderer.SetSponzaLightConfigPath(
            gOptions.sponzaLightConfigPath);
        gRenderer.SetCameraPathFilePath(gOptions.cameraPathFilePath);
        gRenderer.SetCameraPathAutoPlay(
            gOptions.cameraPathSpecified);
        gRenderer.SetVSyncEnabled(gOptions.vsync);
        gRenderer.ConfigureBenchmark(
            gOptions.benchmark,
            gOptions.benchmarkOutput,
            gOptions.benchmarkFrames);
        gRenderer.SetCollectRayStatistics(gOptions.collectRayStatistics);

        if (gOptions.captureSamples > 0)
        {
            gRenderer.ConfigureAutomatedCapture(
                gOptions.captureSamples,
                gOptions.outputPrefix,
                gOptions.maxBounce,
                gOptions.sceneType,
                gOptions.pbrDebugView,
                gOptions.pbrMetallic,
                gOptions.pbrRoughness,
                gOptions.overridePbrMaterial,
                gOptions.enableIbl,
                gOptions.iblIntensity,
                gOptions.validationSeed);
        }

        if (!gOptions.headless)
        {
            ShowWindow(hWnd, nCmdShow);
            UpdateWindow(hWnd);
        }

        if (!gRenderer.Initialize(hWnd))
        {
            DestroyWindow(hWnd);
            return false;
        }

        gRendererReady = true;
        return true;
    }

    LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (gRendererReady)
        {
            switch (message)
            {
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
                gRenderer.OnKey(static_cast<UINT>(wParam), true);
                break;
            case WM_KEYUP:
            case WM_SYSKEYUP:
                gRenderer.OnKey(static_cast<UINT>(wParam), false);
                break;
            case WM_RBUTTONDOWN:
                gRenderer.OnRightMouseButton(
                    true,
                    GET_X_LPARAM(lParam),
                    GET_Y_LPARAM(lParam));
                break;
            case WM_RBUTTONUP:
                gRenderer.OnRightMouseButton(
                    false,
                    GET_X_LPARAM(lParam),
                    GET_Y_LPARAM(lParam));
                break;
            case WM_MOUSEMOVE:
                gRenderer.OnMouseMove(
                    GET_X_LPARAM(lParam),
                    GET_Y_LPARAM(lParam));
                break;
            case WM_KILLFOCUS:
                gRenderer.OnFocusLost();
                break;
            }
        }
        if (gRendererReady && ImGui_ImplWin32_WndProcHandler(hWnd, message, wParam, lParam))
            return TRUE;

        switch (message)
        {
        case WM_PAINT:
            {
                PAINTSTRUCT ps = {};
                BeginPaint(hWnd, &ps);
                EndPaint(hWnd, &ps);
            }
            return 0;

        case WM_SIZE:
            if (gRendererReady && wParam != SIZE_MINIMIZED)
            {
                gRenderer.Resize(LOWORD(lParam), HIWORD(lParam));
            }
            return 0;

        case WM_DESTROY:
            gRendererReady = false;
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
    }
}

