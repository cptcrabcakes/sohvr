#include "Library.h"
#include "Zip.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <fstream>

namespace fs = std::filesystem;

namespace Launcher {

namespace {

constexpr size_t kHeaderSize = 0x40;
constexpr size_t kCrcOffset = 0x10;
constexpr size_t kNameOffset = 0x20;
constexpr size_t kNameSize = 20;

struct KnownRom {
    uint32_t crc;
    RomKind kind;
    const wchar_t* version;
};

// The ones each game's Extract.cpp lists in verMap.
const KnownRom kKnownRoms[] = {
    { 0xEC7011B7, RomKind::Oot, L"N64 NTSC 1.0" },
    { 0xD43DA81F, RomKind::Oot, L"N64 NTSC 1.1" },
    { 0x693BA2AE, RomKind::Oot, L"N64 NTSC 1.2" },
    { 0xB044B569, RomKind::Oot, L"N64 PAL 1.0" },
    { 0xB2055FBD, RomKind::Oot, L"N64 PAL 1.1" },
    { 0xF3DD35BA, RomKind::Oot, L"GameCube US" },
    { 0xF611F4BA, RomKind::Oot, L"GameCube JP" },
    { 0xF7F52DB8, RomKind::Oot, L"GameCube JP Collector's Edition" },
    { 0x09465AC3, RomKind::Oot, L"GameCube PAL" },
    { 0x871E1C92, RomKind::Oot, L"PAL Debug" },
    { 0x87121EFE, RomKind::Oot, L"PAL Debug" },
    { 0xF034001A, RomKind::OotMq, L"Master Quest US" },
    { 0xF43B45BA, RomKind::OotMq, L"Master Quest JP" },
    { 0x1D4136F3, RomKind::OotMq, L"Master Quest PAL" },
    { 0x917D18F6, RomKind::OotMq, L"Master Quest PAL Debug" },
    { 0x5354631C, RomKind::Mm, L"N64 US 1.0" },
    { 0xB443EB08, RomKind::Mm, L"GameCube US" },
};

bool HasRomExtension(const fs::path& path) {
    std::wstring ext = path.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    return ext == L".z64" || ext == L".n64" || ext == L".v64";
}

bool HasRomExtension(const std::string& name) {
    return HasRomExtension(fs::path(name));
}

// Puts a header into the byte order of a .z64, from any of the three a ROM can be in. False if it's none of them.
bool Normalize(uint8_t* header, size_t size) {
    if (header[0] == 0x80 && header[1] == 0x37) {
        return true;
    }
    if (header[0] == 0x37 && header[1] == 0x80) { // .v64: each pair swapped
        for (size_t i = 0; i + 1 < size; i += 2) {
            std::swap(header[i], header[i + 1]);
        }
        return true;
    }
    if (header[0] == 0x40 && header[1] == 0x12) { // .n64: each four reversed
        for (size_t i = 0; i + 3 < size; i += 4) {
            std::swap(header[i], header[i + 3]);
            std::swap(header[i + 1], header[i + 2]);
        }
        return true;
    }
    return false;
}

// Fills in a ROM from its header. False if it isn't a Zelda ROM at all, so stray files don't fill the list.
bool Identify(uint8_t* header, Rom& rom) {
    if (!Normalize(header, kHeaderSize)) {
        return false;
    }
    rom.crc = (static_cast<uint32_t>(header[kCrcOffset]) << 24) | (static_cast<uint32_t>(header[kCrcOffset + 1]) << 16) |
              (static_cast<uint32_t>(header[kCrcOffset + 2]) << 8) | header[kCrcOffset + 3];
    for (const KnownRom& known : kKnownRoms) {
        if (known.crc == rom.crc) {
            rom.kind = known.kind;
            rom.version = known.version;
            return true;
        }
    }
    std::string name(reinterpret_cast<const char*>(header + kNameOffset), kNameSize);
    if (name.find("ZELDA") == std::string::npos && name.find("MAJORA") == std::string::npos) {
        return false;
    }
    rom.kind = RomKind::Unknown;
    rom.version = L"Not a version these games can read";
    return true;
}

void ScanFolder(const fs::path& folder, std::vector<Rom>& roms) {
    std::error_code error;
    for (const fs::directory_entry& entry : fs::directory_iterator(folder, error)) {
        if (!entry.is_regular_file(error)) {
            continue;
        }
        const fs::path& path = entry.path();
        uint8_t header[kHeaderSize] = {};
        Rom rom;
        rom.path = path;
        rom.fileName = path.filename().wstring();
        if (HasRomExtension(path)) {
            std::ifstream file(path, std::ios::binary);
            file.read(reinterpret_cast<char*>(header), kHeaderSize);
            if (file.gcount() != kHeaderSize || !Identify(header, rom)) {
                continue;
            }
            roms.push_back(rom);
        } else if (path.extension() == L".zip" || path.extension() == L".ZIP") {
            std::vector<ZipEntry> entries;
            if (!ListZip(path, entries)) {
                continue;
            }
            for (const ZipEntry& zipEntry : entries) {
                if (!HasRomExtension(zipEntry.name)) {
                    continue;
                }
                std::vector<uint8_t> start;
                if (ReadZipEntry(path, zipEntry, kHeaderSize, start) && start.size() >= kHeaderSize &&
                    Identify(start.data(), rom)) {
                    rom.zipped = true;
                    rom.zipEntry = zipEntry.name;
                    roms.push_back(rom);
                }
                break;
            }
        }
    }
}

void AddFolder(std::vector<fs::path>& folders, const fs::path& folder) {
    std::error_code error;
    fs::path full = fs::weakly_canonical(folder, error);
    if (error || !fs::is_directory(full, error)) {
        return;
    }
    if (std::find(folders.begin(), folders.end(), full) == folders.end()) {
        folders.push_back(full);
    }
}

// Where a game might be, by hand-placed install or by build tree.
fs::path FindExe(const wchar_t* name, std::initializer_list<const wchar_t*> folders) {
    fs::path base = ProgramFolder();
    std::error_code error;
    for (const wchar_t* folder : folders) {
        fs::path candidate = base / folder / name;
        if (fs::is_regular_file(candidate, error)) {
            return fs::weakly_canonical(candidate, error);
        }
    }
    return {};
}

} // namespace

fs::path ProgramFolder() {
    wchar_t path[MAX_PATH * 4] = {};
    GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    return fs::path(path).parent_path();
}

void FindGames(std::vector<Game>& games) {
    std::error_code error;
    games.clear();
    Game oot;
    oot.id = GameId::Oot;
    oot.title = L"Ocarina of Time";
    oot.key = "oot";
    oot.exe = FindExe(L"soh.exe", { L".", L"oot", L"../../x64/Release" });
    if (!oot.exe.empty()) {
        fs::path folder = oot.exe.parent_path();
        oot.hasArchive = fs::is_regular_file(folder / L"oot.o2r", error);
        oot.hasMqArchive = fs::is_regular_file(folder / L"oot-mq.o2r", error);
    }
    games.push_back(oot);

    Game mm;
    mm.id = GameId::Mm;
    mm.title = L"Majora's Mask";
    mm.key = "mm";
    mm.exe = FindExe(L"2ship.exe", { L".", L"mm", L"2ship/x64/Release", L"../../2ship/x64/Release" });
    if (!mm.exe.empty()) {
        mm.hasArchive = fs::is_regular_file(mm.exe.parent_path() / L"mm.o2r", error);
    }
    games.push_back(mm);
}

void FindRoms(const std::vector<fs::path>& extraFolders, std::vector<Rom>& roms) {
    fs::path base = ProgramFolder();
    std::vector<fs::path> folders;
    AddFolder(folders, base);
    AddFolder(folders, base / L"roms");
    // A build tree keeps the program a few folders down from the ROMs.
    fs::path up = base;
    for (int i = 0; i < 3; i++) {
        up = up.parent_path();
        AddFolder(folders, up);
        AddFolder(folders, up / L"roms");
    }
    for (const fs::path& folder : extraFolders) {
        AddFolder(folders, folder);
    }
    roms.clear();
    for (const fs::path& folder : folders) {
        ScanFolder(folder, roms);
    }
    // The same dump twice over shows once, unzipped if it's there both ways.
    std::stable_sort(roms.begin(), roms.end(), [](const Rom& a, const Rom& b) {
        if (a.crc != b.crc) {
            return a.crc < b.crc;
        }
        return !a.zipped && b.zipped;
    });
    roms.erase(std::unique(roms.begin(), roms.end(), [](const Rom& a, const Rom& b) { return a.crc == b.crc; }),
               roms.end());
    // Playable ones first, each game together.
    std::stable_sort(roms.begin(), roms.end(), [](const Rom& a, const Rom& b) {
        return static_cast<int>(a.kind) < static_cast<int>(b.kind);
    });
}

GameId GameForRom(RomKind kind) {
    switch (kind) {
        case RomKind::Oot:
        case RomKind::OotMq:
            return GameId::Oot;
        case RomKind::Mm:
            return GameId::Mm;
        default:
            return GameId::Count;
    }
}

fs::path PrepareRom(const Rom& rom) {
    if (!rom.zipped) {
        return rom.path;
    }
    std::vector<ZipEntry> entries;
    if (!ListZip(rom.path, entries)) {
        return {};
    }
    for (const ZipEntry& entry : entries) {
        if (entry.name != rom.zipEntry) {
            continue;
        }
        std::error_code error;
        fs::path folder = ProgramFolder() / L"unzipped-roms";
        fs::create_directories(folder, error);
        fs::path out = folder / fs::path(entry.name).filename();
        if (fs::is_regular_file(out, error) && fs::file_size(out, error) == entry.size) {
            return out;
        }
        std::vector<uint8_t> data;
        if (!ReadZipEntry(rom.path, entry, entry.size, data) || data.size() != entry.size) {
            return {};
        }
        std::ofstream file(out, std::ios::binary);
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        return file ? out : fs::path();
    }
    return {};
}

bool StartGame(const Game& game, const fs::path& rom) {
    if (game.exe.empty()) {
        return false;
    }
    std::wstring command = L"\"" + game.exe.wstring() + L"\"";
    if (!rom.empty()) {
        command += L" \"" + rom.wstring() + L"\"";
    }
    std::wstring folder = game.exe.parent_path().wstring();
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(game.exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, folder.c_str(), &startup,
                        &process)) {
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

} // namespace Launcher
