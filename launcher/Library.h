#pragma once

// What the launcher knows without loading a game: which games are installed, what they've been set up with, and which
// ROMs are around. A ROM is told by the version CRC in its header, the same one each game's extractor checks; the game
// does the full check of the file itself when it's set up.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Launcher {

enum class GameId { Oot, Mm, Count };

enum class RomKind {
    Oot,       // reads as oot.o2r
    OotMq,     // reads as oot-mq.o2r
    Mm,        // reads as mm.o2r
    Unknown,   // a Zelda ROM neither game can read: a hack, a demo, a version they don't list
};

struct Rom {
    std::filesystem::path path;
    std::wstring fileName;
    std::wstring version;
    RomKind kind = RomKind::Unknown;
    uint32_t crc = 0;
    bool zipped = false;
    std::string zipEntry; // the ROM's name inside a zip
};

struct Game {
    GameId id = GameId::Oot;
    const wchar_t* title = L"";
    const char* key = "";        // the name it goes by on the command line and in the settings file
    std::filesystem::path exe;   // empty if not found
    bool hasArchive = false;     // set up: its assets have been extracted
    bool hasMqArchive = false;
    std::wstring version;        // what the launcher last set it up with, if it did
};

// The folder this program is in.
std::filesystem::path ProgramFolder();

void FindGames(std::vector<Game>& games);
// ROMs next to the program, in a roms folder, a few folders up (for a build tree) and in extraFolders.
void FindRoms(const std::vector<std::filesystem::path>& extraFolders, std::vector<Rom>& roms);

// Which game reads a ROM of this kind, or GameId::Count if neither does.
GameId GameForRom(RomKind kind);

// The path to give the game: the ROM itself, or a copy unzipped next to the program. Empty on failure.
std::filesystem::path PrepareRom(const Rom& rom);

// Starts a game, optionally extracting a ROM first. Its own window takes over from there.
bool StartGame(const Game& game, const std::filesystem::path& rom);

} // namespace Launcher
