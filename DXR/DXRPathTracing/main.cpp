#include <Windows.h>
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
        UINT sceneType = RayTracingManager::c_sceneCornellBox;
        UINT pbrDebugView = RayTracingManager::c_pbrDebugBeauty;
        float pbrMetallic = 1.0f;
        float pbrRoughness = 0.35f;
        bool enableIbl = true;
        float iblIntensity = 0.5f;
        UINT validationSeed = 0;
        bool headless = false;
        std::wstring outputPrefix;
    };

    AppOptions gOptions;

    ATOM RegisterMainWindowClass(HINSTANCE hInstance);
    bool CreateMainWindow(HINSTANCE hInstance, int nCmdShow);
    void ParseCommandLine();
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
            else if (argument == L"--headless")
            {
                gOptions.headless = true;
            }
        }

        if (gOptions.width == 0)
            gOptions.width = 1;
        if (gOptions.height == 0)
            gOptions.height = 1;

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

