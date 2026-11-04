// One program to start either game. It finds what's installed and what ROMs are around, sets a game up from a ROM
// through that game's own extractor, and gets out of the way. Each game stays its own program: the two decomps can't
// share an address space.
//
//   HarkinianVR.exe                 the window
//   HarkinianVR.exe --play mm       starts a game with no window, as the games do to swap to each other in the headset
//   HarkinianVR.exe --after <pid>   first waits for that process to end, so two games don't fight over the headset
//   HarkinianVR.exe --scan <file>   writes what it found to a file
//   HarkinianVR.exe --shot <file>   saves a picture of the window to a PNG
//   HarkinianVR.exe --unzip <name>  unzips the ROM with that file name, as setting up from it would

#include "Library.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <objbase.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <gdiplus.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace Launcher;

namespace {

constexpr int kWindowWidth = 900;
constexpr int kWindowHeight = 640;
constexpr int kMargin = 28;
constexpr int kCardTop = 96;
constexpr int kCardHeight = 214;
constexpr int kCardGap = 20;
constexpr int kListTop = 362;
constexpr int kRowHeight = 42;
constexpr int kAfterTimeoutMs = 15000;

const Gdiplus::Color kBackground(255, 19, 21, 25);
const Gdiplus::Color kCard(255, 30, 33, 39);
const Gdiplus::Color kCardHover(255, 37, 41, 48);
const Gdiplus::Color kText(255, 236, 238, 242);
const Gdiplus::Color kMuted(255, 150, 156, 168);
const Gdiplus::Color kLine(255, 48, 52, 60);
const Gdiplus::Color kWarn(255, 226, 170, 92);
const Gdiplus::Color kOotAccent(255, 74, 168, 96);
const Gdiplus::Color kMmAccent(255, 148, 102, 214);
const Gdiplus::Color kDisabled(255, 58, 62, 70);

struct Button {
    Gdiplus::RectF rect;
    std::wstring label;
    Gdiplus::Color color;
    bool enabled = true;
    bool quiet = false;
    std::function<void()> action;
};

// What's kept between runs, next to the program.
std::wstring SettingsPath() {
    return (ProgramFolder() / L"HarkinianVR.ini").wstring();
}

std::wstring ReadSetting(const wchar_t* section, const wchar_t* key) {
    wchar_t value[1024] = {};
    GetPrivateProfileStringW(section, key, L"", value, 1024, SettingsPath().c_str());
    return value;
}

void WriteSetting(const wchar_t* section, const wchar_t* key, const std::wstring& value) {
    WritePrivateProfileStringW(section, key, value.c_str(), SettingsPath().c_str());
}

std::wstring Widen(const char* text) {
    std::wstring out;
    for (; *text != '\0'; text++) {
        out.push_back(static_cast<wchar_t>(*text));
    }
    return out;
}

struct App {
    HWND window = nullptr;
    float scale = 1.0f;
    std::vector<Game> games;
    std::vector<Rom> roms;
    std::vector<fs::path> extraFolders;
    std::vector<Button> buttons;
    int hovered = -1;
    int scroll = 0;
    int maxScroll = 0;
    std::wstring notice;
};

App sApp;

void LoadExtraFolders() {
    sApp.extraFolders.clear();
    for (int i = 0;; i++) {
        std::wstring key = L"Folder" + std::to_wstring(i);
        std::wstring value = ReadSetting(L"Library", key.c_str());
        if (value.empty()) {
            break;
        }
        sApp.extraFolders.push_back(value);
    }
}

void Refresh() {
    FindGames(sApp.games);
    for (Game& game : sApp.games) {
        game.version = ReadSetting(L"SetUpWith", Widen(game.key).c_str());
    }
    FindRoms(sApp.extraFolders, sApp.roms);
}

const Game* FindGame(GameId id) {
    for (const Game& game : sApp.games) {
        if (game.id == id) {
            return &game;
        }
    }
    return nullptr;
}

const Rom* RomFor(GameId id) {
    for (const Rom& rom : sApp.roms) {
        if (GameForRom(rom.kind) == id) {
            return &rom;
        }
    }
    return nullptr;
}

void Play(const Game& game) {
    if (StartGame(game, {})) {
        WriteSetting(L"Last", L"Game", Widen(game.key));
        PostMessageW(sApp.window, WM_CLOSE, 0, 0);
    } else {
        sApp.notice = L"Couldn't start " + std::wstring(game.title) + L".";
        InvalidateRect(sApp.window, nullptr, FALSE);
    }
}

void SetUp(const Game& game, const Rom& rom) {
    if (rom.zipped) {
        sApp.notice = L"Unzipping " + rom.fileName + L"...";
        InvalidateRect(sApp.window, nullptr, FALSE);
        UpdateWindow(sApp.window);
    }
    fs::path path = PrepareRom(rom);
    if (path.empty() || !StartGame(game, path)) {
        sApp.notice = L"Couldn't set up from " + rom.fileName + L".";
        InvalidateRect(sApp.window, nullptr, FALSE);
        return;
    }
    WriteSetting(L"SetUpWith", Widen(game.key).c_str(), rom.version);
    WriteSetting(L"Last", L"Game", Widen(game.key));
    PostMessageW(sApp.window, WM_CLOSE, 0, 0);
}

void AddRomFolder() {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"Choose a folder with ROMs in it");
    if (SUCCEEDED(dialog->Show(sApp.window))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                std::wstring key = L"Folder" + std::to_wstring(sApp.extraFolders.size());
                WriteSetting(L"Library", key.c_str(), path);
                sApp.extraFolders.push_back(path);
                CoTaskMemFree(path);
                Refresh();
                InvalidateRect(sApp.window, nullptr, FALSE);
            }
            item->Release();
        }
    }
    dialog->Release();
}

void OpenModsFolder(const Game& game) {
    fs::path folder = game.exe.parent_path() / L"mods";
    std::error_code error;
    fs::create_directories(folder, error);
    ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---- Drawing ------------------------------------------------------------------------------------------------------

void FillRounded(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius, const Gdiplus::Color& color) {
    Gdiplus::GraphicsPath path;
    float d = radius * 2.0f;
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    path.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    path.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    path.CloseFigure();
    Gdiplus::SolidBrush brush(color);
    g.FillPath(&brush, &path);
}

void Text(Gdiplus::Graphics& g, const std::wstring& text, float size, const Gdiplus::RectF& r, const Gdiplus::Color& color,
          bool bold = false, Gdiplus::StringAlignment across = Gdiplus::StringAlignmentNear,
          Gdiplus::StringAlignment down = Gdiplus::StringAlignmentNear) {
    Gdiplus::FontFamily family(L"Segoe UI");
    Gdiplus::Font font(&family, size * sApp.scale, bold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular,
                       Gdiplus::UnitPixel);
    Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
    format.SetAlignment(across);
    format.SetLineAlignment(down);
    format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    Gdiplus::SolidBrush brush(color);
    g.DrawString(text.c_str(), -1, &font, r, &format, &brush);
}

Gdiplus::RectF Scaled(float x, float y, float w, float h) {
    float s = sApp.scale;
    return Gdiplus::RectF(x * s, y * s, w * s, h * s);
}

void AddButton(const Gdiplus::RectF& rect, const std::wstring& label, const Gdiplus::Color& color, bool enabled, bool quiet,
               std::function<void()> action) {
    Button button;
    button.rect = rect;
    button.label = label;
    button.color = color;
    button.enabled = enabled;
    button.quiet = quiet;
    button.action = std::move(action);
    sApp.buttons.push_back(std::move(button));
}

void DrawCard(Gdiplus::Graphics& g, const Game& game, float x, float width, const Gdiplus::Color& accent) {
    float s = sApp.scale;
    Gdiplus::RectF card = Scaled(x, kCardTop, width, kCardHeight);
    FillRounded(g, card, 10.0f * s, kCard);
    Gdiplus::SolidBrush bar(accent);
    g.FillRectangle(&bar, Scaled(x + 22, kCardTop + 24, 34, 4));
    Text(g, game.title, 25.0f, Scaled(x + 22, kCardTop + 36, width - 44, 40), kText, true);

    const Rom* rom = RomFor(game.id);
    std::wstring status;
    std::wstring detail;
    std::wstring action;
    bool enabled = true;
    bool ready = false;
    std::function<void()> run;
    const Game* target = &game;
    if (game.exe.empty()) {
        status = L"Not built or installed";
        detail = game.id == GameId::Oot ? L"soh.exe wasn't found" : L"2ship.exe wasn't found";
        action = L"Play";
        enabled = false;
    } else if (game.hasArchive || game.hasMqArchive) {
        ready = true;
        status = L"Ready to play";
        detail = !game.version.empty()                  ? L"Set up with " + game.version
                 : game.hasArchive && game.hasMqArchive ? L"Original and Master Quest"
                 : game.hasMqArchive                    ? L"Master Quest"
                                                        : L"Already set up";
        action = L"Play";
        run = [target]() { Play(*target); };
    } else if (rom != nullptr) {
        status = L"Needs setting up, once";
        detail = L"Will use " + rom->version;
        action = L"Set up";
        const Rom* chosen = rom;
        run = [target, chosen]() { SetUp(*target, *chosen); };
    } else {
        status = L"No ROM found";
        detail = L"Add a folder with your own ROM in it";
        action = L"Add folder";
        run = []() { AddRomFolder(); };
    }
    Text(g, status, 15.0f, Scaled(x + 22, kCardTop + 80, width - 44, 24), ready ? kText : kWarn);
    Text(g, detail, 13.0f, Scaled(x + 22, kCardTop + 104, width - 44, 22), kMuted);
    AddButton(Scaled(x + 22, kCardTop + 146, 136, 44), action, accent, enabled, false, run);
    if (!game.exe.empty()) {
        AddButton(Scaled(x + 170, kCardTop + 146, 120, 44), L"Mods folder", kMuted, true, true,
                  [target]() { OpenModsFolder(*target); });
    }
}

void DrawRomRow(Gdiplus::Graphics& g, const Rom& rom, float y, float width) {
    float x = static_cast<float>(kMargin);
    const Game* game = FindGame(GameForRom(rom.kind));
    bool readable = rom.kind != RomKind::Unknown;
    bool canUse = readable && game != nullptr && !game->exe.empty();
    bool inUse = game != nullptr && !game->version.empty() && game->version == rom.version;
    std::wstring gameName = rom.kind == RomKind::OotMq ? L"Ocarina of Time" : game != nullptr ? game->title : L"";
    Text(g, rom.fileName, 13.5f, Scaled(x + 14, y + 3, width - 300, 21), readable ? kText : kMuted);
    std::wstring line = readable ? gameName + L"  ·  " + rom.version : rom.version;
    if (rom.zipped) {
        line += L"  ·  zipped";
    }
    Text(g, line, 12.0f, Scaled(x + 14, y + 21, width - 300, 18), readable ? kMuted : kWarn);
    if (inUse) {
        Text(g, L"In use", 13.0f, Scaled(x + width - 124, y, 110, kRowHeight - 2), kMuted, false,
             Gdiplus::StringAlignmentFar, Gdiplus::StringAlignmentCenter);
    } else if (canUse) {
        const Game* target = game;
        const Rom* chosen = &rom;
        AddButton(Scaled(x + width - 124, y + 6, 110, 30), L"Set up with this", kMuted, true, true,
                  [target, chosen]() { SetUp(*target, *chosen); });
    }
}

void Paint(Gdiplus::Graphics& g, int width, int height) {
    float s = sApp.scale;
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
    g.Clear(kBackground);
    sApp.buttons.clear();

    float logicalWidth = width / s;
    float logicalHeight = height / s;
    float inner = logicalWidth - kMargin * 2;
    Text(g, L"Harkinian VR", 30.0f, Scaled(kMargin, 24, inner, 42), kText, true);
    Text(g, L"Choose a game. Your headset's runtime should already be running.", 14.0f,
         Scaled(kMargin + 1, 64, inner, 22), kMuted);

    float cardWidth = (inner - kCardGap) / 2.0f;
    const Game* oot = FindGame(GameId::Oot);
    const Game* mm = FindGame(GameId::Mm);
    DrawCard(g, *oot, kMargin, cardWidth, kOotAccent);
    DrawCard(g, *mm, kMargin + cardWidth + kCardGap, cardWidth, kMmAccent);

    Text(g, L"ROMs", 16.0f, Scaled(kMargin, kListTop - 34, 200, 26), kText, true);
    AddButton(Scaled(kMargin + inner - 118, kListTop - 36, 118, 28), L"Add folder", kMuted, true, true,
              []() { AddRomFolder(); });

    float listBottom = logicalHeight - 40.0f;
    float listHeight = listBottom - kListTop;
    int rows = static_cast<int>(sApp.roms.size());
    sApp.maxScroll = std::max(0, rows * kRowHeight - static_cast<int>(listHeight));
    sApp.scroll = std::clamp(sApp.scroll, 0, sApp.maxScroll);
    Gdiplus::RectF clip = Scaled(0, kListTop, logicalWidth, listHeight);
    g.SetClip(clip);
    if (rows == 0) {
        Text(g, L"None found yet. Put your ROMs next to this program, or add the folder they're in.", 13.0f,
             Scaled(kMargin + 14, kListTop + 10, inner, 22), kMuted);
    }
    size_t buttonsBefore = sApp.buttons.size();
    for (int i = 0; i < rows; i++) {
        float y = static_cast<float>(kListTop + i * kRowHeight - sApp.scroll);
        if (y + kRowHeight < kListTop || y > listBottom) {
            continue;
        }
        Gdiplus::SolidBrush line(kLine);
        g.FillRectangle(&line, Scaled(kMargin, y + kRowHeight - 1, inner, 1));
        DrawRomRow(g, sApp.roms[i], y, inner);
    }
    g.ResetClip();

    // A row scrolled mostly out of the list can't be clicked.
    for (size_t i = sApp.buttons.size(); i-- > buttonsBefore;) {
        const Gdiplus::RectF& r = sApp.buttons[i].rect;
        if (r.Y < clip.Y || r.GetBottom() > clip.GetBottom()) {
            sApp.buttons.erase(sApp.buttons.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }

    for (size_t i = 0; i < sApp.buttons.size(); i++) {
        const Button& b = sApp.buttons[i];
        bool hot = static_cast<int>(i) == sApp.hovered && b.enabled;
        Gdiplus::Color fill = !b.enabled   ? kDisabled
                              : b.quiet    ? (hot ? kCardHover : Gdiplus::Color(0, 0, 0, 0))
                              : hot        ? Gdiplus::Color(255, std::min(b.color.GetR() + 22, 255),
                                                            std::min(b.color.GetG() + 22, 255),
                                                            std::min(b.color.GetB() + 22, 255))
                                           : b.color;
        FillRounded(g, b.rect, 7.0f * s, fill);
        if (b.quiet) {
            Gdiplus::Pen outline(kLine, 1.0f * s);
            Gdiplus::GraphicsPath path;
            g.DrawRectangle(&outline, b.rect.X, b.rect.Y, b.rect.Width, b.rect.Height);
        }
        Gdiplus::Color textColor = !b.enabled ? kMuted : b.quiet ? kText : Gdiplus::Color(255, 16, 18, 22);
        Gdiplus::FontFamily family(L"Segoe UI");
        Gdiplus::Font font(&family, (b.quiet ? 12.5f : 16.0f) * s, b.quiet ? Gdiplus::FontStyleRegular : Gdiplus::FontStyleBold,
                           Gdiplus::UnitPixel);
        Gdiplus::StringFormat format;
        format.SetAlignment(Gdiplus::StringAlignmentCenter);
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        Gdiplus::SolidBrush brush(textColor);
        g.DrawString(b.label.c_str(), -1, &font, b.rect, &format, &brush);
    }

    std::wstring footer = !sApp.notice.empty()
                              ? sApp.notice
                              : L"ROM hacks that change the game's code can't run here. Asset packs (.o2r) go in a game's mods folder.";
    Text(g, footer, 12.0f, Scaled(kMargin, logicalHeight - 30, inner, 20), sApp.notice.empty() ? kMuted : kWarn);
}

int ButtonAt(int x, int y) {
    for (size_t i = sApp.buttons.size(); i-- > 0;) {
        if (sApp.buttons[i].rect.Contains(static_cast<float>(x), static_cast<float>(y))) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint;
            HDC dc = BeginPaint(window, &paint);
            RECT client;
            GetClientRect(window, &client);
            Gdiplus::Bitmap buffer(client.right, client.bottom, PixelFormat32bppPARGB);
            Gdiplus::Graphics g(&buffer);
            Paint(g, client.right, client.bottom);
            Gdiplus::Graphics screen(dc);
            screen.DrawImage(&buffer, 0, 0);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_MOUSEMOVE: {
            int hovered = ButtonAt(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            if (hovered != sApp.hovered) {
                sApp.hovered = hovered;
                InvalidateRect(window, nullptr, FALSE);
            }
            SetCursor(LoadCursor(nullptr, hovered >= 0 && sApp.buttons[hovered].enabled ? IDC_HAND : IDC_ARROW));
            return 0;
        }
        case WM_LBUTTONUP: {
            int index = ButtonAt(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            if (index >= 0 && sApp.buttons[index].enabled && sApp.buttons[index].action) {
                // Refreshing rebuilds the list the button lives in, so it runs from a copy.
                std::function<void()> action = sApp.buttons[index].action;
                action();
            }
            return 0;
        }
        case WM_MOUSEWHEEL:
            sApp.scroll -= GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA * kRowHeight;
            sApp.scroll = std::clamp(sApp.scroll, 0, sApp.maxScroll);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wParam) != WA_INACTIVE) {
                // The games can be set up while this is open, and ROMs dropped in.
                Refresh();
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                PostMessageW(window, WM_CLOSE, 0, 0);
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// Saves a picture of the window as it would draw, for checking the layout without a screen.
bool SaveShot(const std::wstring& path) {
    sApp.scale = 1.0f;
    Gdiplus::Bitmap buffer(kWindowWidth, kWindowHeight, PixelFormat32bppPARGB);
    Gdiplus::Graphics g(&buffer);
    Paint(g, kWindowWidth, kWindowHeight);
    CLSID png;
    CLSIDFromString(L"{557CF406-1A04-11D3-9A73-0000F81EF32E}", &png);
    return buffer.Save(path.c_str(), &png, nullptr) == Gdiplus::Ok;
}

bool WriteScan(const std::wstring& path) {
    std::wofstream out{ fs::path(path) };
    for (const Game& game : sApp.games) {
        out << game.title << L": exe=" << game.exe.wstring() << L" archive=" << game.hasArchive << L" mq=" << game.hasMqArchive
            << L" version=" << game.version << L"\n";
    }
    for (const Rom& rom : sApp.roms) {
        wchar_t crc[16];
        swprintf_s(crc, L"%08X", rom.crc);
        out << rom.fileName << L" | " << crc << L" | kind " << static_cast<int>(rom.kind) << L" | " << rom.version
            << (rom.zipped ? L" | zipped" : L"") << L"\n";
    }
    return out.good();
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring play;
    std::wstring scanTo;
    std::wstring shotTo;
    std::wstring unzipName;
    DWORD after = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::wstring option = argv[i];
        if (option == L"--play") {
            play = argv[i + 1];
        } else if (option == L"--after") {
            after = static_cast<DWORD>(_wtoi64(argv[i + 1]));
        } else if (option == L"--scan") {
            scanTo = argv[i + 1];
        } else if (option == L"--shot") {
            shotTo = argv[i + 1];
        } else if (option == L"--unzip") {
            unzipName = argv[i + 1];
        }
    }
    // The game handing over needs to be gone first: only one can hold the headset.
    if (after != 0) {
        if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, after)) {
            WaitForSingleObject(process, kAfterTimeoutMs);
            CloseHandle(process);
        }
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartupInput gdiplusInput;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr);

    LoadExtraFolders();
    Refresh();

    int result = 0;
    if (!scanTo.empty()) {
        result = WriteScan(scanTo) ? 0 : 1;
    } else if (!shotTo.empty()) {
        result = SaveShot(shotTo) ? 0 : 1;
    } else if (!unzipName.empty()) {
        result = 1;
        for (const Rom& rom : sApp.roms) {
            if (rom.fileName == unzipName && !PrepareRom(rom).empty()) {
                result = 0;
            }
        }
    } else if (!play.empty()) {
        result = 1;
        for (const Game& game : sApp.games) {
            if (Widen(game.key) == play && (game.hasArchive || game.hasMqArchive)) {
                result = StartGame(game, {}) ? 0 : 1;
            }
        }
    } else {
        WNDCLASSEXW windowClass = {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        windowClass.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        windowClass.lpszClassName = L"HarkinianVR";
        RegisterClassExW(&windowClass);

        UINT dpi = GetDpiForSystem();
        sApp.scale = dpi / 96.0f;
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        RECT rect = { 0, 0, static_cast<LONG>(kWindowWidth * sApp.scale), static_cast<LONG>(kWindowHeight * sApp.scale) };
        AdjustWindowRectExForDpi(&rect, style, FALSE, 0, dpi);
        sApp.window = CreateWindowExW(0, windowClass.lpszClassName, L"Harkinian VR", style, CW_USEDEFAULT, CW_USEDEFAULT,
                                      rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
        ShowWindow(sApp.window, show);
        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    Gdiplus::GdiplusShutdown(gdiplusToken);
    CoUninitialize();
    LocalFree(argv);
    return result;
}
