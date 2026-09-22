#include "app/config.h"

#include <filesystem>
#include <fstream>
#include <iostream>

int main()
{
    const auto path = std::filesystem::temp_directory_path() /
        "nitlink-custom-no-signal-config-test.txt";
    std::error_code ec;
    std::filesystem::remove(path, ec);

    // A pre-E1 config with none of the new keys keeps compiled defaults.
    {
        std::ofstream legacy(path, std::ios::trunc);
        legacy << "window_width = 1280\n";
    }
    NitLink::Config legacyLoaded;
    if (!legacyLoaded.Load(path.string())) return 1;
    if (legacyLoaded.noSignalMode != "default" ||
        !legacyLoaded.noSignalImage.empty() ||
        legacyLoaded.noSignalFit != "contain" ||
        !legacyLoaded.noSignalDimImage) return 2;

    NitLink::Config defaults;
    if (!defaults.Save(path.string())) return 3;
    NitLink::Config defaultLoaded;
    if (!defaultLoaded.Load(path.string())) return 4;
    if (defaultLoaded.noSignalMode != "default" ||
        !defaultLoaded.noSignalImage.empty() ||
        defaultLoaded.noSignalFit != "contain" ||
        !defaultLoaded.noSignalDimImage) return 5;

    NitLink::Config saved;
    saved.noSignalMode = "image";
    saved.noSignalImage =
        "C:\\Users\\測試者\\Pictures\\無訊號圖片.png";
    saved.noSignalFit = "cover";
    saved.noSignalDimImage = false;
    if (!saved.Save(path.string())) return 6;

    NitLink::Config loaded;
    if (!loaded.Load(path.string())) return 7;
    if (loaded.noSignalMode != saved.noSignalMode ||
        loaded.noSignalImage != saved.noSignalImage ||
        loaded.noSignalFit != saved.noSignalFit ||
        loaded.noSignalDimImage != saved.noSignalDimImage) return 8;

    saved.noSignalFit = "stretch";
    saved.noSignalDimImage = true;
    if (!saved.Save(path.string())) return 9;
    NitLink::Config stretchLoaded;
    if (!stretchLoaded.Load(path.string()) ||
        stretchLoaded.noSignalFit != "stretch" ||
        !stretchLoaded.noSignalDimImage) return 10;

    {
        std::ofstream invalid(path, std::ios::trunc);
        invalid << "no_signal_mode = unsupported\n"
                << "no_signal_fit = unsupported\n"
                << "no_signal_image = C:\\Users\\測試者\\無訊號.bmp\n";
    }
    NitLink::Config invalidLoaded;
    if (!invalidLoaded.Load(path.string())) return 11;
    if (invalidLoaded.noSignalMode != "default" ||
        invalidLoaded.noSignalFit != "contain") return 12;

    if (!std::filesystem::remove(path, ec) && std::filesystem::exists(path)) {
        return 13;
    }

    std::cout << "custom No Signal config tests passed\n";
    return 0;
}
