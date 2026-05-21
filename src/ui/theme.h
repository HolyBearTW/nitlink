#pragma once

#include <d2d1.h>

namespace NitLink {

// All visual styling lives in ONE place. Both the overlay (stats panel,
// sparkline) and the settings panel pull from here. Want a new look?
// Edit this file. Want multiple themes (light/dark/CRT/cyberpunk)? Add
// a Theme parameter to GetTheme() and return different palettes.
//
// Colors are D2D1_COLOR_F (R, G, B, A) where each channel is 0.0-1.0.
//
// === CURRENT THEME ===
// Dark navy backgrounds, indigo accents, soft text. Generic "developer
// tool" look. Replace the values in GetTheme() to re-skin everything.

struct Theme {
    // Backgrounds
    D2D1_COLOR_F overlayBg;        // FPS counter panel background (semi-transparent)
    D2D1_COLOR_F panelBg;          // Settings panel full background
    D2D1_COLOR_F backdrop;         // Dim layer behind settings panel
    D2D1_COLOR_F trackBg;          // Slider tracks, dropdown closed state, button rest

    // Foregrounds
    D2D1_COLOR_F text;             // Primary text
    D2D1_COLOR_F textDim;          // Secondary text, disabled state
    D2D1_COLOR_F accent;           // Brand color, highlights, active states
    D2D1_COLOR_F hover;            // Hover overlay color

    // Semantic states
    D2D1_COLOR_F good;             // 60fps locked, low latency
    D2D1_COLOR_F warn;             // Frame drops, signal loss, high latency

    // Font
    const wchar_t* fontFamily;     // "Segoe UI", "Consolas", whatever
    const wchar_t* overlayFont;    // Monospace works best for stats. "Consolas", "Cascadia Code"
    float fontSizeTitle;           // Section headers, panel title
    float fontSizeLabel;           // Widget labels, body
    float fontSizeValue;           // Slider values, dropdown selection
    float fontSizeSmall;           // Hints, footnotes
    float overlayFontSize;         // FPS counter main text size
    float overlayFontSizeHeader;   // "NITLINK" / game name in stats panel

    // Sizing
    float panelWidth;              // Settings panel width in pixels
    float panelPadding;            // Inner padding from panel edges
    float rowHeight;               // Standard widget row height
    float sectionHeight;           // Section header row height
    float sectionGap;              // Extra space above section headers
    float cornerRadius;            // Rounded corners on buttons, dropdowns
    float labelWidth;              // Width of the "label" column inside widgets
};

inline const Theme& GetTheme() {
    static const Theme t = {
        // Backgrounds
        /*overlayBg*/ D2D1::ColorF(0.04f, 0.05f, 0.10f, 0.85f),
        /*panelBg*/   D2D1::ColorF(0.06f, 0.07f, 0.12f, 0.92f),
        /*backdrop*/  D2D1::ColorF(0.00f, 0.00f, 0.00f, 0.45f),
        /*trackBg*/   D2D1::ColorF(0.16f, 0.18f, 0.24f, 1.00f),

        // Foregrounds
        /*text*/      D2D1::ColorF(0.92f, 0.93f, 0.96f, 1.00f),
        /*textDim*/   D2D1::ColorF(0.55f, 0.58f, 0.65f, 1.00f),
        /*accent*/    D2D1::ColorF(0.51f, 0.53f, 0.97f, 1.00f), // indigo-400
        /*hover*/     D2D1::ColorF(0.22f, 0.24f, 0.32f, 1.00f),

        // Semantic
        /*good*/      D2D1::ColorF(0.13f, 0.77f, 0.37f, 1.00f), // green-500
        /*warn*/      D2D1::ColorF(0.96f, 0.62f, 0.04f, 1.00f), // amber-500

        // Font
        /*fontFamily*/            L"Segoe UI",
        /*overlayFont*/           L"Consolas",
        /*fontSizeTitle*/         17.0f,
        /*fontSizeLabel*/         15.0f,
        /*fontSizeValue*/         15.0f,
        /*fontSizeSmall*/         12.0f,
        /*overlayFontSize*/       14.0f,
        /*overlayFontSizeHeader*/ 14.0f,

        // Sizing: these are in DIPs (device-independent pixels). At 100%
        // scaling 1 DIP = 1 pixel; at 175% scaling 1 DIP = ~1.75 pixels.
        // Sized generously so widgets stay easy to hit on high-DPI displays
        // (your typical 4K @ 150%+ setup) without dropping to native pixels.
        /*panelWidth*/    480.0f,
        /*panelPadding*/   24.0f,
        /*rowHeight*/      48.0f,
        /*sectionHeight*/  52.0f,
        /*sectionGap*/     22.0f,
        /*cornerRadius*/    4.0f,
        /*labelWidth*/    140.0f,
    };
    return t;
}

} // namespace NitLink
