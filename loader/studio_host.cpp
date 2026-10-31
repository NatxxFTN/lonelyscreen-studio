/**
 * @file studio_host.cpp
 * Hôte du LonelyScreen Studio, sur le thread d'interface de LonelyScreen.
 *
 *  - Repère la fenêtre principale de LonelyScreen et s'y accroche.
 *  - Onglet : petite languette collée au bord de la fenêtre ; un clic ouvre
 *    ou ferme le panneau. Raccourci : Ctrl+Maj+P.
 *  - Panneau : fenêtre possédée par LonelyScreen, collée à son côté, qui la
 *    suit (déplacement, redimensionnement, réduction). Elle héberge WebView2,
 *    qui affiche l'interface web du dossier install\studio.
 *  - Pont : messages JSON {id, op, args} venant de la page, réponses
 *    {id, ok, result|error}. Les opérations longues tournent sur un thread de
 *    travail et reviennent par PostMessage.
 */
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <wrl.h>

#include <algorithm>
#include <memory>
#include <string>

#include "WebView2.h"
#include "loader_internal.h"
#include "store.h"
#include "updater.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using json = nlohmann::json;

namespace {

constexpr wchar_t kMessageWindowClass[] = L"LsStudioMessageWindow";
constexpr wchar_t kTabWindowClass[] = L"LsStudioTab";
constexpr wchar_t kPanelWindowClass[] = L"LsStudioPanel";
constexpr wchar_t kPanelTitle[] = L"LonelyScreen Studio";
constexpr wchar_t kStudioHost[] = L"studio.ls";
constexpr wchar_t kUserHost[] = L"user.ls";
constexpr wchar_t kStudioUrl[] = L"https://studio.ls/index.html";
constexpr char kStudioOrigin[] = "https://studio.ls/";
constexpr wchar_t kWindowStateFile[] = L"studio-window.ini";

constexpr UINT kTrackTimerId = 1;
constexpr UINT kTrackIntervalMs = 400;
constexpr UINT kAsyncResultMessage = WM_APP + 1;
constexpr int kBaseDpi = 96;

// Géométrie en pixels à 96 DPI.
constexpr int kTabWidth = 22;
constexpr int kTabHeight = 92;
constexpr int kTabRadius = 9;
constexpr int kDefaultPanelWidth = 780;
constexpr int kMinPanelWidth = 560;
constexpr int kMaxPanelWidth = 1400;
constexpr int kMinPanelHeight = 560;
constexpr int kResizeGrip = 6;
constexpr int kMinMainWidth = 300;
constexpr int kMinMainHeight = 200;

// Couleurs de l'onglet (ARGB).
constexpr Gdiplus::ARGB kTabFill = 0xF2181818;
constexpr Gdiplus::ARGB kTabFillHover = 0xFF242424;
constexpr Gdiplus::ARGB kTabStroke = 0x33FFFFFF;
constexpr Gdiplus::ARGB kTabAccent = 0xFF8B6CFF;
constexpr Gdiplus::ARGB kTabGlyph = 0xFFEDEDED;
constexpr float kTabStrokeWidth = 1.0f;
constexpr float kTabAccentWidth = 3.0f;
constexpr float kGlyphStrokeWidth = 1.8f;
constexpr int kGlyphHalfHeight = 5;
constexpr int kGlyphDepth = 3;
constexpr int kAccentInset = 26;

constexpr BYTE kPanelBackgroundRed = 18;
constexpr BYTE kPanelBackgroundGreen = 18;
constexpr BYTE kPanelBackgroundBlue = 18;
constexpr BYTE kOpaque = 255;

enum class Side { Right, Left };

HWND g_messageWindow = nullptr;
HWND g_mainWindow = nullptr;
HWND g_tabWindow = nullptr;
HWND g_panelWindow = nullptr;
HWINEVENTHOOK g_locationHook = nullptr;
HHOOK g_keyboardHook = nullptr;
ULONG_PTR g_gdiplusToken = 0;
bool g_panelOpen = false;
bool g_tabHover = false;
bool g_webViewRequested = false;
int g_panelWidth = kDefaultPanelWidth; // à 96 DPI
Side g_side = Side::Right;

ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_webView;

HINSTANCE moduleInstance() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&moduleInstance), &module);
    return module;
}

int dpiOf(HWND window) {
    const UINT dpi = window != nullptr ? GetDpiForWindow(window) : 0;
    return dpi != 0 ? static_cast<int>(dpi) : kBaseDpi;
}

int scaled(int value, int dpi) {
    return MulDiv(value, dpi, kBaseDpi);
}

bool isOurWindow(HWND window) {
    return window == g_messageWindow || window == g_tabWindow || window == g_panelWindow;
}

std::wstring windowStatePath() {
    return loader::configDirectory() + L"\\" + kWindowStateFile;
}

void loadWindowState() {
    g_panelWidth = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"panel", L"width", kDefaultPanelWidth,
                                                                     windowStatePath().c_str())),
                              kMinPanelWidth, kMaxPanelWidth);
}

void saveWindowState() {
    WritePrivateProfileStringW(L"panel", L"width", std::to_wstring(g_panelWidth).c_str(), windowStatePath().c_str());
}

// ---------------------------------------------------------------------------
// Fenêtre principale de LonelyScreen
// ---------------------------------------------------------------------------

struct MainSearch {
    DWORD processId;
    HWND best;
    long long bestArea;
};

BOOL CALLBACK collectMainCandidate(HWND window, LPARAM parameter) {
    auto *search = reinterpret_cast<MainSearch *>(parameter);
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != search->processId || isOurWindow(window) || !IsWindowVisible(window) ||
        GetWindow(window, GW_OWNER) != nullptr) {
        return TRUE;
    }
    const LONG_PTR exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) != 0) {
        return TRUE;
    }
    RECT rect = {};
    GetWindowRect(window, &rect);
    const long long width = rect.right - rect.left;
    const long long height = rect.bottom - rect.top;
    if (!IsIconic(window) && (width < kMinMainWidth || height < kMinMainHeight)) {
        return TRUE;
    }
    const long long area = IsIconic(window) ? 1 : width * height;
    if (area > search->bestArea) {
        search->best = window;
        search->bestArea = area;
    }
    return TRUE;
}

HWND findMainWindow() {
    MainSearch search = {GetCurrentProcessId(), nullptr, 0};
    EnumWindows(collectMainCandidate, reinterpret_cast<LPARAM>(&search));
    return search.best;
}

RECT visualBounds(HWND window) {
    RECT rect = {};
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect)))) {
        GetWindowRect(window, &rect);
    }
    return rect;
}

// ---------------------------------------------------------------------------
// Placement de l'onglet et du panneau
// ---------------------------------------------------------------------------

void layoutAttachedWindows() {
    if (g_mainWindow == nullptr || !IsWindow(g_mainWindow)) {
        return;
    }
    const bool mainVisible = IsWindowVisible(g_mainWindow) && !IsIconic(g_mainWindow);
    if (!mainVisible) {
        ShowWindow(g_tabWindow, SW_HIDE);
        if (g_panelWindow != nullptr) {
            ShowWindow(g_panelWindow, SW_HIDE);
        }
        return;
    }

    const int dpi = dpiOf(g_mainWindow);
    const RECT main = visualBounds(g_mainWindow);
    MONITORINFO monitor = {sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(g_mainWindow, MONITOR_DEFAULTTONEAREST), &monitor);
    const RECT work = monitor.rcWork;

    const int tabWidth = scaled(kTabWidth, dpi);
    const int tabHeight = scaled(kTabHeight, dpi);
    const int panelWidth = scaled(g_panelWidth, dpi);
    const int mainHeight = main.bottom - main.top;

    if (main.right + tabWidth + panelWidth <= work.right) {
        g_side = Side::Right;
    } else if (main.left - tabWidth - panelWidth >= work.left) {
        g_side = Side::Left;
    } else {
        g_side = Side::Right;
    }

    const int tabX = g_side == Side::Right ? main.right : main.left - tabWidth;
    const int tabY = main.top + (mainHeight - tabHeight) / 2;
    SetWindowPos(g_tabWindow, nullptr, tabX, tabY, tabWidth, tabHeight,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);

    if (g_panelWindow != nullptr && g_panelOpen) {
        const int panelX = g_side == Side::Right ? main.right + tabWidth : main.left - tabWidth - panelWidth;
        const int panelHeight = std::min(std::max(mainHeight, scaled(kMinPanelHeight, dpi)),
                                         static_cast<int>(work.bottom - main.top));
        SetWindowPos(g_panelWindow, nullptr, panelX, main.top, panelWidth, panelHeight,
                     SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
    }
}

void CALLBACK onLocationChange(HWINEVENTHOOK, DWORD, HWND window, LONG objectId, LONG, DWORD, DWORD) {
    if (window == g_mainWindow && objectId == OBJID_WINDOW) {
        layoutAttachedWindows();
    }
}

// ---------------------------------------------------------------------------
// Onglet (fenêtre à transparence par pixel, dessinée avec GDI+)
// ---------------------------------------------------------------------------

void addRoundedRect(Gdiplus::GraphicsPath *path, float x, float y, float width, float height, float radius,
                    bool roundLeft, bool roundRight) {
    const float diameter = radius * 2.0f;
    if (roundLeft) {
        path->AddArc(x, y, diameter, diameter, 180.0f, 90.0f);
    } else {
        path->AddLine(x, y, x, y);
    }
    if (roundRight) {
        path->AddArc(x + width - diameter, y, diameter, diameter, 270.0f, 90.0f);
        path->AddArc(x + width - diameter, y + height - diameter, diameter, diameter, 0.0f, 90.0f);
    } else {
        path->AddLine(x + width, y, x + width, y + height);
    }
    if (roundLeft) {
        path->AddArc(x, y + height - diameter, diameter, diameter, 90.0f, 90.0f);
    } else {
        path->AddLine(x, y + height, x, y + height);
    }
    path->CloseFigure();
}

void paintTab() {
    if (g_tabWindow == nullptr) {
        return;
    }
    RECT rect = {};
    GetWindowRect(g_tabWindow, &rect);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) {
        return;
    }
    const int dpi = dpiOf(g_tabWindow);

    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ previous = SelectObject(memory, bitmap);
    {
        Gdiplus::Graphics graphics(memory);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.Clear(Gdiplus::Color(0, 0, 0, 0));

        // La languette est arrondie du côté opposé à la fenêtre LonelyScreen.
        const bool roundRight = g_side == Side::Right;
        const float radius = static_cast<float>(scaled(kTabRadius, dpi));
        Gdiplus::GraphicsPath shape;
        addRoundedRect(&shape, 0.5f, 0.5f, static_cast<float>(width) - 1.0f, static_cast<float>(height) - 1.0f,
                       radius, !roundRight, roundRight);
        Gdiplus::SolidBrush fill(Gdiplus::Color(g_tabHover ? kTabFillHover : kTabFill));
        graphics.FillPath(&fill, &shape);
        Gdiplus::Pen stroke(Gdiplus::Color(kTabStroke), kTabStrokeWidth);
        graphics.DrawPath(&stroke, &shape);

        // Trait d'accent vertical, côté fenêtre.
        const float inset = static_cast<float>(scaled(kAccentInset, dpi));
        const float accentX = roundRight ? 1.5f : static_cast<float>(width) - 1.5f;
        Gdiplus::Pen accent(Gdiplus::Color(kTabAccent), static_cast<float>(scaled(1, dpi)) * kTabAccentWidth / 2.0f);
        accent.SetStartCap(Gdiplus::LineCapRound);
        accent.SetEndCap(Gdiplus::LineCapRound);
        graphics.DrawLine(&accent, accentX, inset, accentX, static_cast<float>(height) - inset);

        // Chevron : vers l'extérieur quand le panneau est fermé, vers la fenêtre quand il est ouvert.
        const bool pointOutward = !g_panelOpen;
        const bool pointRight = roundRight ? pointOutward : !pointOutward;
        const float centerX = static_cast<float>(width) / 2.0f + (roundRight ? 1.0f : -1.0f);
        const float centerY = static_cast<float>(height) / 2.0f;
        const float half = static_cast<float>(scaled(kGlyphHalfHeight, dpi));
        const float depth = static_cast<float>(scaled(kGlyphDepth, dpi)) * (pointRight ? 1.0f : -1.0f);
        Gdiplus::Pen glyph(Gdiplus::Color(kTabGlyph), static_cast<float>(scaled(1, dpi)) * kGlyphStrokeWidth);
        glyph.SetStartCap(Gdiplus::LineCapRound);
        glyph.SetEndCap(Gdiplus::LineCapRound);
        glyph.SetLineJoin(Gdiplus::LineJoinRound);
        const Gdiplus::PointF points[3] = {{centerX - depth / 2.0f, centerY - half},
                                           {centerX + depth / 2.0f, centerY},
                                           {centerX - depth / 2.0f, centerY + half}};
        graphics.DrawLines(&glyph, points, 3);
    }
    POINT source = {0, 0};
    SIZE size = {width, height};
    POINT position = {rect.left, rect.top};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, kOpaque, AC_SRC_ALPHA};
    UpdateLayeredWindow(g_tabWindow, screen, &position, &size, memory, &source, 0, &blend, ULW_ALPHA);
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
}

LRESULT CALLBACK tabProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_MOUSEMOVE:
        if (!g_tabHover) {
            g_tabHover = true;
            TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, window, 0};
            TrackMouseEvent(&track);
            paintTab();
        }
        return 0;
    case WM_MOUSELEAVE:
        g_tabHover = false;
        paintTab();
        return 0;
    case WM_LBUTTONUP:
        studio::toggle();
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_WINDOWPOSCHANGED:
        paintTab();
        break;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Pont JavaScript <-> natif
// ---------------------------------------------------------------------------

void postToPage(const json &message) {
    if (g_webView != nullptr) {
        g_webView->PostWebMessageAsJson(store::toWide(message.dump()).c_str());
    }
}

void reply(const json &id, const json &result) {
    postToPage({{"id", id}, {"ok", true}, {"result", result}});
}

void replyError(const json &id, const std::string &error) {
    postToPage({{"id", id}, {"ok", false}, {"error", error}});
}

/** Achemine le résultat d'un thread de travail vers le thread d'interface. */
store::Completion completionFor(const json &id) {
    return [id](const json &result, const std::string &error) {
        auto *message = new std::string(
            (error.empty() ? json{{"id", id}, {"ok", true}, {"result", result}} : json{{"id", id}, {"ok", false}, {"error", error}})
                .dump());
        if (g_panelWindow == nullptr || !PostMessageW(g_panelWindow, kAsyncResultMessage, 0,
                                                      reinterpret_cast<LPARAM>(message))) {
            delete message;
        }
    };
}

void openFolder(const std::string &which) {
    std::wstring path = loader::configDirectory();
    if (which == "plugins") {
        path = store::userPluginsDirectory();
    }
    CreateDirectoryW(path.c_str(), nullptr);
    ShellExecuteW(g_panelWindow, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void handlePageMessage(const std::string &text) {
    const json message = json::parse(text, nullptr, false);
    if (!message.is_object() || !message.contains("id")) {
        return;
    }
    const json id = message["id"];
    const std::string operation = message.value("op", "");
    const json args = message.value("args", json::object());
    try {
        if (operation == "state") {
            reply(id, store::buildState());
        } else if (operation == "readIni") {
            reply(id, store::readIni(args));
        } else if (operation == "writeIni") {
            reply(id, store::writeIni(args));
        } else if (operation == "saveConfig") {
            reply(id, store::saveConfig(args));
        } else if (operation == "uninstall") {
            reply(id, store::uninstall(args));
        } else if (operation == "install") {
            store::installAsync(args.value("item", json::object()), completionFor(id));
        } else if (operation == "fetch") {
            store::fetchTextAsync(args.value("url", ""), completionFor(id));
        } else if (operation == "updateStatus") {
            reply(id, updater::status());
        } else if (operation == "updateCheck") {
            updater::checkAsync(completionFor(id));
        } else if (operation == "updateApply") {
            updater::applyAsync(completionFor(id));
        } else if (operation == "resetSettings") {
            loader::restoreDefaultConfig();
            loader::markConfigDirty();
            reply(id, {{"reset", true}});
        } else if (operation == "openFolder") {
            openFolder(args.value("which", ""));
            reply(id, {{"opened", true}});
        } else if (operation == "openExternal") {
            const std::string url = args.value("url", "");
            if (url.rfind("https://", 0) != 0) {
                throw std::runtime_error("Seuls les liens HTTPS peuvent être ouverts.");
            }
            ShellExecuteW(g_panelWindow, L"open", store::toWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            reply(id, {{"opened", true}});
        } else if (operation == "close") {
            studio::toggle();
            reply(id, {{"closed", true}});
        } else {
            replyError(id, "Opération inconnue : " + operation);
        }
    } catch (const std::exception &error) {
        replyError(id, error.what());
    }
}

// ---------------------------------------------------------------------------
// WebView2
// ---------------------------------------------------------------------------

void resizeWebView() {
    if (g_controller != nullptr && g_panelWindow != nullptr) {
        RECT bounds = {};
        GetClientRect(g_panelWindow, &bounds);
        g_controller->put_Bounds(bounds);
    }
}

bool isStudioSource(ICoreWebView2WebMessageReceivedEventArgs *args) {
    LPWSTR source = nullptr;
    if (FAILED(args->get_Source(&source)) || source == nullptr) {
        return false;
    }
    const std::string origin = store::toUtf8(source);
    CoTaskMemFree(source);
    return origin.rfind(kStudioOrigin, 0) == 0;
}

void configureWebView() {
    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(g_webView->get_Settings(&settings))) {
        wchar_t devtools[2] = {};
        const bool allowDevTools = GetEnvironmentVariableW(L"LS_STUDIO_DEVTOOLS", devtools, 2) > 0;
        settings->put_AreDevToolsEnabled(allowDevTools);
        settings->put_AreDefaultContextMenusEnabled(allowDevTools);
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_IsZoomControlEnabled(FALSE);
    }

    ComPtr<ICoreWebView2_3> webView3;
    if (SUCCEEDED(g_webView.As(&webView3))) {
        webView3->SetVirtualHostNameToFolderMapping(kStudioHost, store::studioDirectory().c_str(),
                                                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
        webView3->SetVirtualHostNameToFolderMapping(kUserHost, loader::configDirectory().c_str(),
                                                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
    }

    EventRegistrationToken token = {};
    g_webView->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [](ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *args) -> HRESULT {
                if (!isStudioSource(args)) {
                    return S_OK;
                }
                LPWSTR text = nullptr;
                if (SUCCEEDED(args->get_WebMessageAsJson(&text)) && text != nullptr) {
                    handlePageMessage(store::toUtf8(text));
                    CoTaskMemFree(text);
                }
                return S_OK;
            })
            .Get(),
        &token);

    // Navigation limitée au Studio ; les liens externes s'ouvrent dans le navigateur.
    g_webView->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
                LPWSTR uri = nullptr;
                if (SUCCEEDED(args->get_Uri(&uri)) && uri != nullptr) {
                    const std::string target = store::toUtf8(uri);
                    CoTaskMemFree(uri);
                    if (target.rfind(kStudioOrigin, 0) != 0) {
                        args->put_Cancel(TRUE);
                    }
                }
                return S_OK;
            })
            .Get(),
        &token);
    g_webView->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *args) -> HRESULT {
                LPWSTR uri = nullptr;
                if (SUCCEEDED(args->get_Uri(&uri)) && uri != nullptr) {
                    if (wcsncmp(uri, L"https://", 8) == 0) {
                        ShellExecuteW(g_panelWindow, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL);
                    }
                    CoTaskMemFree(uri);
                }
                args->put_Handled(TRUE);
                return S_OK;
            })
            .Get(),
        &token);

    g_webView->Navigate(kStudioUrl);
}

void createWebView() {
    if (g_webViewRequested) {
        return;
    }
    g_webViewRequested = true;
    const std::wstring dataDirectory = loader::configDirectory() + L"\\webview";
    const HRESULT result = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, dataDirectory.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT environmentResult, ICoreWebView2Environment *environment) -> HRESULT {
                if (FAILED(environmentResult) || environment == nullptr) {
                    loader::log("WebView2 indisponible (0x%08lX)", static_cast<unsigned long>(environmentResult));
                    g_webViewRequested = false;
                    return S_OK;
                }
                environment->CreateCoreWebView2Controller(
                    g_panelWindow,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [](HRESULT controllerResult, ICoreWebView2Controller *controller) -> HRESULT {
                            if (FAILED(controllerResult) || controller == nullptr) {
                                loader::log("création de la vue Studio impossible (0x%08lX)",
                                            static_cast<unsigned long>(controllerResult));
                                g_webViewRequested = false;
                                return S_OK;
                            }
                            g_controller = controller;
                            g_controller->get_CoreWebView2(&g_webView);
                            ComPtr<ICoreWebView2Controller2> controller2;
                            if (SUCCEEDED(g_controller.As(&controller2))) {
                                controller2->put_DefaultBackgroundColor(
                                    {kOpaque, kPanelBackgroundRed, kPanelBackgroundGreen, kPanelBackgroundBlue});
                            }
                            resizeWebView();
                            configureWebView();
                            g_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                            loader::log("Studio prêt");
                            return S_OK;
                        })
                        .Get());
                return S_OK;
            })
            .Get());
    if (FAILED(result)) {
        loader::log("WebView2 indisponible (0x%08lX) : installe le runtime Microsoft Edge WebView2",
                    static_cast<unsigned long>(result));
        g_webViewRequested = false;
    }
}

// ---------------------------------------------------------------------------
// Panneau
// ---------------------------------------------------------------------------

LRESULT CALLBACK panelProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_SIZE:
        resizeWebView();
        return 0;
    case WM_NCHITTEST: {
        // Bord extérieur redimensionnable (côté opposé à LonelyScreen).
        POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        RECT rect = {};
        GetWindowRect(window, &rect);
        const int grip = scaled(kResizeGrip, dpiOf(window));
        if (g_side == Side::Right && point.x >= rect.right - grip) {
            return HTRIGHT;
        }
        if (g_side == Side::Left && point.x < rect.left + grip) {
            return HTLEFT;
        }
        return HTCLIENT;
    }
    case WM_SIZING: {
        auto *rect = reinterpret_cast<RECT *>(lParam);
        const int dpi = dpiOf(window);
        const int width = std::clamp(static_cast<int>(rect->right - rect->left), scaled(kMinPanelWidth, dpi),
                                     scaled(kMaxPanelWidth, dpi));
        if (wParam == WMSZ_RIGHT) {
            rect->right = rect->left + width;
        } else {
            rect->left = rect->right - width;
        }
        g_panelWidth = MulDiv(width, kBaseDpi, dpi);
        return TRUE;
    }
    case WM_EXITSIZEMOVE:
        saveWindowState();
        layoutAttachedWindows();
        return 0;
    case kAsyncResultMessage: {
        std::unique_ptr<std::string> payload(reinterpret_cast<std::string *>(lParam));
        if (g_webView != nullptr) {
            g_webView->PostWebMessageAsJson(store::toWide(*payload).c_str());
        }
        return 0;
    }
    case WM_CLOSE:
        if (g_panelOpen) {
            studio::toggle();
        }
        return 0;
    case WM_DPICHANGED:
        layoutAttachedWindows();
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

void createPanel() {
    if (g_panelWindow != nullptr) {
        return;
    }
    g_panelWindow = CreateWindowExW(0, kPanelWindowClass, kPanelTitle,
                                    WS_POPUP | WS_CLIPCHILDREN | WS_THICKFRAME, 0, 0, 0, 0, g_mainWindow, nullptr,
                                    moduleInstance(), nullptr);
    if (g_panelWindow == nullptr) {
        loader::log("création du panneau Studio impossible (%lu)", GetLastError());
        return;
    }
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(g_panelWindow, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(g_panelWindow, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    // Supprime le cadre visible de WS_THICKFRAME tout en gardant la poignée de redimensionnement.
    const MARGINS margins = {0, 0, 0, 1};
    DwmExtendFrameIntoClientArea(g_panelWindow, &margins);
    SetWindowPos(g_panelWindow, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
}

LRESULT CALLBACK panelFrameProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCALCSIZE && wParam == TRUE) {
        return 0; // zone client = fenêtre entière, pas de barre de titre
    }
    return panelProc(window, message, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Suivi, raccourci clavier, fenêtre de messages
// ---------------------------------------------------------------------------

void attachTo(HWND main) {
    g_mainWindow = main;
    SetWindowLongPtrW(g_tabWindow, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(main));
    if (g_panelWindow != nullptr) {
        SetWindowLongPtrW(g_panelWindow, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(main));
    }
    loader::log("Studio accroché à la fenêtre LonelyScreen");
    layoutAttachedWindows();
    paintTab();
}

void trackMainWindow() {
    const HWND main = findMainWindow();
    if (main != nullptr && main != g_mainWindow) {
        attachTo(main);
    } else if (main == nullptr && g_mainWindow != nullptr) {
        ShowWindow(g_tabWindow, SW_HIDE);
        if (g_panelWindow != nullptr) {
            ShowWindow(g_panelWindow, SW_HIDE);
        }
        g_mainWindow = nullptr;
    } else {
        layoutAttachedWindows();
    }
}

LRESULT CALLBACK keyboardProc(int code, WPARAM key, LPARAM flags) {
    constexpr DWORD kKeyUpFlag = 1u << 31;
    constexpr DWORD kRepeatFlag = 1u << 30;
    const DWORD keyFlags = static_cast<DWORD>(flags);
    if (code == HC_ACTION && key == 'P' && (keyFlags & kKeyUpFlag) == 0 && (keyFlags & kRepeatFlag) == 0 &&
        (GetKeyState(VK_CONTROL) & 0x8000) != 0 && (GetKeyState(VK_SHIFT) & 0x8000) != 0) {
        studio::toggle();
        return 1;
    }
    return CallNextHookEx(g_keyboardHook, code, key, flags);
}

LRESULT CALLBACK messageProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_TIMER && wParam == kTrackTimerId) {
        trackMainWindow();
        loader::checkConfigChanges();
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void registerClasses() {
    WNDCLASSEXW windowClass = {sizeof(windowClass)};
    windowClass.hInstance = moduleInstance();
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);

    windowClass.lpfnWndProc = messageProc;
    windowClass.lpszClassName = kMessageWindowClass;
    RegisterClassExW(&windowClass);

    windowClass.lpfnWndProc = tabProc;
    windowClass.lpszClassName = kTabWindowClass;
    RegisterClassExW(&windowClass);

    windowClass.lpfnWndProc = panelFrameProc;
    windowClass.lpszClassName = kPanelWindowClass;
    windowClass.style = CS_DROPSHADOW;
    windowClass.hbrBackground = CreateSolidBrush(RGB(kPanelBackgroundRed, kPanelBackgroundGreen, kPanelBackgroundBlue));
    RegisterClassExW(&windowClass);
}

} // namespace

namespace studio {

void bootstrapOnUiThread() {
    static bool started = false;
    if (started) {
        return;
    }
    started = true;

    Gdiplus::GdiplusStartupInput gdiplusInput;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusInput, nullptr);
    loadWindowState();
    registerClasses();

    g_messageWindow = CreateWindowExW(0, kMessageWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                      moduleInstance(), nullptr);
    g_tabWindow = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kTabWindowClass, L"Studio",
                                  WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, moduleInstance(), nullptr);
    SetTimer(g_messageWindow, kTrackTimerId, kTrackIntervalMs, nullptr);
    g_locationHook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
                                     onLocationChange, GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
    g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD, keyboardProc, nullptr, GetCurrentThreadId());
    updater::cleanupOldVersionsAsync();
    loader::log("Studio %s démarré (Ctrl+Maj+P ou onglet au bord de la fenêtre)", loader::coreVersion());
}

void toggle() {
    if (g_mainWindow == nullptr) {
        trackMainWindow();
        if (g_mainWindow == nullptr) {
            return;
        }
    }
    g_panelOpen = !g_panelOpen;
    if (g_panelOpen) {
        createPanel();
        createWebView();
        layoutAttachedWindows();
        SetForegroundWindow(g_panelWindow);
        if (g_controller != nullptr) {
            g_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        }
        postToPage({{"event", "opened"}});
    } else if (g_panelWindow != nullptr) {
        ShowWindow(g_panelWindow, SW_HIDE);
    }
    paintTab();
}

} // namespace studio
