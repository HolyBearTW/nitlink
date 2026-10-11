#pragma once

namespace NitLink {

// Layout and interaction state for the optional Capture / Present FPS rows.
// The hit area matches the small, borderless title glyph. Coordinates are
// client pixels and match the HUD's 96-DPI D2D layout.
class HudFpsDetailsToggle {
public:
    static constexpr int kButtonLeft = 84;
    static constexpr int kButtonTop = 17;
    static constexpr int kButtonRight = 112;
    static constexpr int kButtonBottom = 41;
    static constexpr float kGlyphFontSize = 14.0f;

    static constexpr float kCollapsedPanelHeight = 162.0f;
    static constexpr float kExpandedPanelHeight = 202.0f;

    bool ToggleAt(int clientX, int clientY) noexcept {
        if (!HitTest(clientX, clientY)) return false;
        Toggle();
        return true;
    }

    bool ToggleFromTab(bool hudVisible, bool pipMode, bool settingsVisible) noexcept {
        if (!hudVisible || pipMode || settingsVisible) return false;
        Toggle();
        return true;
    }

    static constexpr bool HitTest(int clientX, int clientY) noexcept {
        return clientX >= kButtonLeft && clientX < kButtonRight &&
               clientY >= kButtonTop && clientY < kButtonBottom;
    }

    bool IsExpanded() const noexcept { return m_expanded; }
    float PanelHeight() const noexcept {
        return m_expanded ? kExpandedPanelHeight : kCollapsedPanelHeight;
    }
    float FooterRuleTop() const noexcept { return m_expanded ? 170.0f : 128.0f; }
    float FooterTop() const noexcept { return m_expanded ? 172.0f : 130.0f; }
    float FooterBottom() const noexcept { return m_expanded ? 194.0f : 152.0f; }

private:
    void Toggle() noexcept { m_expanded = !m_expanded; }
    bool m_expanded = false;
};

} // namespace NitLink
