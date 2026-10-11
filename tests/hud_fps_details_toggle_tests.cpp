#include "overlay/hud_fps_details_toggle.h"

#include <cstdlib>
#include <iostream>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
}

int main() {
    NitLink::HudFpsDetailsToggle toggle;
    Require(!toggle.IsExpanded(), "auxiliary FPS rows start collapsed");
    Require(toggle.PanelHeight() == 162.0f, "collapsed panel uses compact height");

    Require(NitLink::HudFpsDetailsToggle::kGlyphFontSize == 14.0f,
            "approved toggle glyph size is used");
    Require(!toggle.ToggleAt(83, 28), "click left of glyph is ignored");
    Require(!toggle.ToggleAt(84, 16), "click above glyph is ignored");
    Require(!toggle.ToggleAt(112, 28), "right edge is outside the glyph hit area");
    Require(!toggle.IsExpanded(), "outside clicks do not change state");

    Require(toggle.ToggleAt(98, 29), "glyph click is handled");
    Require(toggle.IsExpanded(), "first button click expands auxiliary rows");
    Require(toggle.PanelHeight() == 202.0f, "expanded panel uses full height");
    Require(toggle.FooterRuleTop() == 170.0f && toggle.FooterTop() == 172.0f &&
            toggle.FooterBottom() == 194.0f,
            "expanded footer retains the existing layout");

    Require(toggle.ToggleAt(111, 40), "glyph bottom-right interior is clickable");
    Require(!toggle.IsExpanded(), "second button click collapses auxiliary rows");
    Require(toggle.FooterRuleTop() == 128.0f && toggle.FooterTop() == 130.0f &&
            toggle.FooterBottom() == 152.0f,
            "collapsed footer moves below the primary graph");

    Require(!toggle.ToggleFromTab(false, false, false) && !toggle.IsExpanded(),
            "Tab leaves a hidden Ctrl+F3 HUD unchanged");
    Require(!toggle.ToggleFromTab(true, true, false) && !toggle.IsExpanded(),
            "Tab ignores the HUD suppressed by PiP");
    Require(!toggle.ToggleFromTab(true, false, true) && !toggle.IsExpanded(),
            "F1 keeps Tab for settings keyboard navigation");
    Require(toggle.ToggleFromTab(true, false, false) && toggle.IsExpanded() &&
            toggle.PanelHeight() == 202.0f,
            "Tab expands the visible HUD exactly like the glyph");
    Require(!toggle.ToggleFromTab(false, false, false) && toggle.IsExpanded(),
            "hiding the HUD prevents Tab from changing its retained state");
    Require(toggle.ToggleAt(98, 29) && !toggle.IsExpanded(),
            "mouse and Tab share the same toggle state");
    Require(toggle.ToggleFromTab(true, false, false) && toggle.IsExpanded() &&
            toggle.ToggleFromTab(true, false, false) && !toggle.IsExpanded(),
            "successive Tab presses alternate expanded and collapsed rows");
    return 0;
}
