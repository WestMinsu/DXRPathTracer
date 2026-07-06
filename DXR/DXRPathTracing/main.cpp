#include <Windows.h>

#include "D3D12Renderer.h"
#include "ThirdParty/imgui/imgui.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

namespace
{
    constexpr wchar_t c_windowTitle[] = L"DXRPathTracing";
    constexpr wchar_t c_windowClassName[] = L"DXRPathTracingWindow";

    D3D12Renderer gRenderer;
    bool gRendererReady = false;

    ATOM RegisterMainWindowClass(HINSTANCE hInstance);
    bool CreateMainWindow(HINSTANCE hInstance, int nCmdShow);
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
        HWND hWnd = CreateWindowW(
            c_windowClassName,
            c_windowTitle,
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            0,
            CW_USEDEFAULT,
            0,
            nullptr,
            nullptr,
            hInstance,
            nullptr);

        if (!hWnd)
            return false;

        ShowWindow(hWnd, nCmdShow);
        UpdateWindow(hWnd);

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

