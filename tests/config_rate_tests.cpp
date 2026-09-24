#include "app/config.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using NitLink::Config;

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

int main()
{
    const auto path = std::filesystem::temp_directory_path() /
        L"NitLinkConfigRateMigrationTests.ini";
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    try {
        {
            std::ofstream file(path);
            Require(file.is_open(), "legacy config opens for write");
            file << "capture_override.count = 1\n";
            file << "capture_override.0.device = AVerMedia GC553Pro\n";
            file << "capture_override.0.width = 1920\n";
            file << "capture_override.0.height = 1080\n";
            file << "capture_override.0.fps = 59\n";
            file << "capture_override.0.format = P010\n";
        }

        Config loaded;
        Require(loaded.Load(path.string()), "legacy config loads");
        const auto first = loaded.GetOverride(L"AVerMedia GC553Pro");
        Require(first.fps == 59, "legacy display FPS remains 59");
        Require(first.fpsNumerator == 0,
                "legacy FPS stays unresolved until native modes are known");
        Require(first.fpsDenominator == 1,
                "legacy unresolved FPS uses denominator 1");

        Require(loaded.Save(path.string()), "legacy config saves");

        Config reloaded;
        Require(reloaded.Load(path.string()), "saved legacy config reloads");
        const auto second = reloaded.GetOverride(L"AVerMedia GC553Pro");
        Require(second.fps == 59, "reloaded legacy display FPS remains 59");
        Require(second.fpsNumerator == 0,
                "save/reload does not invent an exact 59/1 preference");
        Require(second.fpsDenominator == 1,
                "save/reload keeps unresolved denominator stable");

        std::cout << "config rate migration tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        std::filesystem::remove(path, ignored);
        return 1;
    }

    std::filesystem::remove(path, ignored);
    return 0;
}
