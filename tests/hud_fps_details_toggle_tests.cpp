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

    Require(!toggle.ToggleAt(99, 30), "click left of button is ignored");
    Require(!toggle.ToggleAt(100, 18), "click above button is ignored");
    Require(!toggle.ToggleAt(128, 30), "right edge is outside the button");
    Require(!toggle.IsExpanded(), "outside clicks do not change state");

    Require(toggle.ToggleAt(114, 31), "button click is handled");
    Require(toggle.IsExpanded(), "first button click expands auxiliary rows");
    Require(toggle.PanelHeight() == 202.0f, "expanded panel uses full height");
    Require(toggle.FooterRuleTop() == 170.0f && toggle.FooterTop() == 172.0f &&
            toggle.FooterBottom() == 194.0f,
            "expanded footer retains the existing layout");

    Require(toggle.ToggleAt(127, 42), "button bottom-right interior is clickable");
    Require(!toggle.IsExpanded(), "second button click collapses auxiliary rows");
    Require(toggle.FooterRuleTop() == 128.0f && toggle.FooterTop() == 130.0f &&
            toggle.FooterBottom() == 152.0f,
            "collapsed footer moves below the primary graph");
    return 0;
}
