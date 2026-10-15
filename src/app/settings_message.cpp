#include "settings_message.h"
#include "game_database.h"

#include <charconv>
#include <cmath>
#include <map>
#include <string>

namespace NitLink {
namespace {

struct Value {
    enum class Kind { Object, String, Number, Boolean } kind = Kind::Object;
    std::map<std::wstring, Value> fields;
    std::wstring text;
    double number = 0;
    bool boolean = false;
};

// The wire schema needs objects, strings, numbers and booleans only. Rejecting
// other JSON types and limiting depth keeps malformed input bounded.
class Parser {
public:
    explicit Parser(std::wstring_view input) : input(input) {}
    bool Parse(Value& value) {
        return Read(value, 0) && (Whitespace(), pos == input.size());
    }

private:
    std::wstring_view input;
    size_t pos = 0;
    void Whitespace() {
        while (pos < input.size() && (input[pos] == L' ' || input[pos] == L'\t' ||
               input[pos] == L'\r' || input[pos] == L'\n')) ++pos;
    }
    bool Take(wchar_t c) {
        Whitespace();
        if (pos == input.size() || input[pos] != c) return false;
        ++pos;
        return true;
    }
    bool Hex(wchar_t& out) {
        unsigned n = 0;
        for (int i = 0; i < 4; ++i) {
            if (pos == input.size()) return false;
            const wchar_t c = input[pos++];
            unsigned d;
            if (c >= L'0' && c <= L'9') d = c - L'0';
            else if (c >= L'a' && c <= L'f') d = c - L'a' + 10;
            else if (c >= L'A' && c <= L'F') d = c - L'A' + 10;
            else return false;
            n = n * 16 + d;
        }
        out = static_cast<wchar_t>(n);
        return true;
    }
    bool String(std::wstring& out) {
        if (!Take(L'"')) return false;
        while (pos < input.size()) {
            wchar_t c = input[pos++];
            if (c == L'"') {
                for (size_t i = 0; i < out.size(); ++i) {
                    const auto u = static_cast<unsigned>(out[i]);
                    if (u >= 0xD800 && u <= 0xDBFF) {
                        if (++i == out.size() || out[i] < 0xDC00 || out[i] > 0xDFFF)
                            return false;
                    } else if (u >= 0xDC00 && u <= 0xDFFF) return false;
                }
                return true;
            }
            if (c < 0x20) return false;
            if (c == L'\\') {
                if (pos == input.size()) return false;
                c = input[pos++];
                switch (c) {
                case L'"': case L'\\': case L'/': break;
                case L'b': c = L'\b'; break;
                case L'f': c = L'\f'; break;
                case L'n': c = L'\n'; break;
                case L'r': c = L'\r'; break;
                case L't': c = L'\t'; break;
                case L'u': if (!Hex(c)) return false; break;
                default: return false;
                }
            }
            out += c;
        }
        return false;
    }
    bool Digit() const { return pos < input.size() && input[pos] >= L'0' && input[pos] <= L'9'; }
    bool Number(Value& v) {
        const size_t start = pos;
        if (pos < input.size() && input[pos] == L'-') ++pos;
        if (!Digit()) return false;
        if (input[pos] == L'0') ++pos;
        else while (Digit()) ++pos;
        if (pos < input.size() && input[pos] == L'.') {
            ++pos;
            if (!Digit()) return false;
            while (Digit()) ++pos;
        }
        if (pos < input.size() && (input[pos] == L'e' || input[pos] == L'E')) {
            ++pos;
            if (pos < input.size() && (input[pos] == L'+' || input[pos] == L'-')) ++pos;
            if (!Digit()) return false;
            while (Digit()) ++pos;
        }
        std::string token;
        for (size_t i = start; i < pos; ++i) token += static_cast<char>(input[i]);
        const auto result = std::from_chars(token.data(), token.data() + token.size(), v.number);
        v.kind = Value::Kind::Number;
        return result.ec == std::errc{} && result.ptr == token.data() + token.size() &&
               std::isfinite(v.number);
    }
    bool Read(Value& v, unsigned depth) {
        Whitespace();
        if (pos == input.size() || depth > 2) return false;
        if (input[pos] == L'{') {
            if (depth == 2) return false;
            ++pos;
            if (Take(L'}')) return true;
            do {
                std::wstring key;
                Value child;
                if (v.fields.size() >= 8 || !String(key) || !Take(L':') ||
                    !Read(child, depth + 1) || !v.fields.emplace(key, std::move(child)).second)
                    return false;
                if (Take(L'}')) return true;
            } while (Take(L','));
            return false;
        }
        if (input[pos] == L'"') {
            v.kind = Value::Kind::String;
            return String(v.text);
        }
        for (const auto literal : {std::wstring_view(L"true"), std::wstring_view(L"false")}) {
            if (input.substr(pos, literal.size()) == literal) {
                pos += literal.size();
                v.kind = Value::Kind::Boolean;
                v.boolean = literal == L"true";
                return true;
            }
        }
        return Number(v);
    }
};

bool OneOf(std::wstring_view value, std::initializer_list<std::wstring_view> allowed) {
    for (auto candidate : allowed) if (value == candidate) return true;
    return false;
}
bool Text(const Value& v, size_t limit) {
    if (v.kind != Value::Kind::String || v.text.size() > limit) return false;
    for (auto c : v.text) if (c < 0x20 || c == 0x7F) return false;
    return true;
}
bool Uint(const Value& object, const wchar_t* key, uint32_t max, uint32_t& out) {
    const auto it = object.fields.find(key);
    if (it == object.fields.end()) return false;
    const auto& v = it->second;
    if (v.kind != Value::Kind::Number || v.number < 0 || v.number > max ||
        std::floor(v.number) != v.number) return false;
    out = static_cast<uint32_t>(v.number);
    return true;
}
} // namespace

std::optional<SettingsMessage> ParseSettingsMessage(std::wstring_view json) {
    if (json.empty() || json.size() > 8192) return std::nullopt;
    Value root;
    if (!Parser(json).Parse(root) || root.kind != Value::Kind::Object ||
        root.fields.empty() || root.fields.size() > 2) return std::nullopt;
    const auto actionIt = root.fields.find(L"action");
    if (actionIt == root.fields.end() || !Text(actionIt->second, 64)) return std::nullopt;
    const auto valueIt = root.fields.find(L"value");
    const Value* value = valueIt == root.fields.end() ? nullptr : &valueIt->second;
    if (root.fields.size() != (value ? 2u : 1u)) return std::nullopt;
    SettingsMessage result;
    result.action = actionIt->second.text;
    const auto& action = result.action;
    if (action == L"javascriptException") {
        if (!value || !Text(*value, 512)) return std::nullopt;
        result.text = value->text;
    } else if (OneOf(action, {L"ready", L"cycleNoSignalMode", L"chooseNoSignalImage",
        L"clearNoSignalImage", L"cycleNoSignalFit", L"cycleNoSignalDimImage",
        L"cyclePresentPacing", L"cycleAspect", L"cyclePanelSide", L"cycleScaler",
        L"clearGame", L"saveGameSettings", L"getDeviceList", L"refreshDevices",
        L"openScreenshotFolder", L"checkHolyBearUpdate", L"markHolyBearUpdateSeen"})) {
        if (value) return std::nullopt;
    } else if (action == L"restoreAutoCover" || action == L"cancelGameSearch") {
        if (value) return std::nullopt;
    } else if(action==L"renderGameArtwork"){
        if(!value||value->kind!=Value::Kind::Object||value->fields.size()!=6||
           !Uint(*value,L"gameId",0xffffffffu,result.gameId)||!result.gameId||!Uint(*value,L"coverId",0xffffffffu,result.coverId)||!result.coverId)return std::nullopt;
        const auto mode=value->fields.find(L"mode");
        if(mode==value->fields.end()||!Text(mode->second,16)||!OneOf(mode->second.text,{L"original",L"fit",L"crop"}))return std::nullopt;
        result.artworkMode=mode->second.text;
        const auto number=[&](const wchar_t* key,double lo,double hi,double& out){const auto item=value->fields.find(key);if(item==value->fields.end()||item->second.kind!=Value::Kind::Number||item->second.number<lo||item->second.number>hi)return false;out=item->second.number;return true;};
        if(!number(L"x",0,1,result.cropX)||!number(L"y",0,1,result.cropY)||!number(L"zoom",1,3,result.artworkZoom))return std::nullopt;
    } else if (action == L"setGameCustomName") {
        if (!value || value->kind != Value::Kind::Object || value->fields.size()!=2) return std::nullopt;
        const auto key=value->fields.find(L"key"), name=value->fields.find(L"name");
        if (key==value->fields.end() || name==value->fields.end() || !Text(key->second,64) ||
            key->second.text.empty() || !Text(name->second,256)) return std::nullopt;
        std::string id;
        for(auto c:key->second.text){if(c>127)return std::nullopt;id.push_back(static_cast<char>(c));}
        if(!FindGameById(id)&&!Games::DynamicId(id))return std::nullopt;
        result.key=key->second.text;result.text=name->second.text;
    } else if (action == L"searchGames" || action == L"getGameCovers" || action == L"selectGameCover") {
        if (!value || value->kind != Value::Kind::Object) return std::nullopt;
        if (action == L"searchGames") {
            const auto query = value->fields.find(L"query");
            if (value->fields.size() != 2 || query == value->fields.end() || !Text(query->second, 256) ||
                query->second.text.empty() || !Uint(*value, L"request", 0xffffffffu, result.request) || !result.request)
                return std::nullopt;
            result.text = query->second.text;
        } else if (action == L"getGameCovers") {
            const auto shape = value->fields.find(L"shape");
            if ((value->fields.size() != 4 && value->fields.size()!=6) || shape == value->fields.end() || !Text(shape->second, 16) ||
                !OneOf(shape->second.text, {L"square", L"portrait", L"all"}) || !Uint(*value, L"page", 1000, result.page) ||
                !Uint(*value, L"gameId", 0xffffffffu, result.gameId) || !result.gameId ||
                !Uint(*value, L"request", 0xffffffffu, result.request) || !result.request) return std::nullopt;
            result.shape = shape->second.text;
            if(value->fields.size()==6){
                const auto provider=value->fields.find(L"provider"),query=value->fields.find(L"officialQuery");
                if(provider==value->fields.end()||!Text(provider->second,16)||!OneOf(provider->second.text,{L"all",L"nintendo",L"community"})||
                   query==value->fields.end()||!Text(query->second,256))return std::nullopt;
                result.coverProvider=provider->second.text;result.catalogueQuery=query->second.text;
            }
        } else if (value->fields.size() != 2 || !Uint(*value, L"gameId", 0xffffffffu, result.gameId) ||
            !result.gameId || !Uint(*value, L"coverId", 0xffffffffu, result.coverId) || !result.coverId)
            return std::nullopt;
    } else if (OneOf(action, {L"toggleHDR", L"toggleColorExpansion", L"toggleNIS",
        L"toggleMute", L"toggleVSync", L"toggleLowLatency", L"togglePreventSleep"})) {
        // Existing controls send the next visual state; native toggles remain authoritative.
        if (value && value->kind != Value::Kind::Boolean) return std::nullopt;
    } else if (action == L"refreshAudioDiagnostics") {
        uint32_t request = 0;
        if (!Uint(root, L"value", 0xffffffffu, request) || request == 0) return std::nullopt;
        result.number = request;
    } else if (action == L"setAudioFifoMs" || action == L"setAudioRenderMs") {
        if (!value || value->kind != Value::Kind::Number || value->number < 3 ||
            value->number > 100 || std::floor(value->number) != value->number) return std::nullopt;
        result.number = value->number;
    } else if (action == L"setAudioDrift" || action == L"setAudioExclusive" || action == L"setHolyBearAutoUpdateCheck") {
        if (!value || value->kind != Value::Kind::Boolean) return std::nullopt;
        result.number = value->boolean ? 1 : 0;
    } else if (action == L"setVolume" || action == L"setPiPOpacity") {
        if (!value || value->kind != Value::Kind::Number || value->number > 1 ||
            value->number < (action == L"setPiPOpacity" ? 0.1 : 0.0)) return std::nullopt;
        result.number = value->number;
    } else if (action == L"setCaptureFormatOverride") {
        if (!value || value->kind != Value::Kind::Object) return std::nullopt;
        auto& f = result.format;
        if (!Uint(*value, L"width", 16384, f.width) ||
            !Uint(*value, L"height", 16384, f.height) ||
            !Uint(*value, L"fps", 1000, f.fps)) return std::nullopt;
        const auto fmt = value->fields.find(L"format");
        if (fmt == value->fields.end() || !Text(fmt->second, 4) ||
            !OneOf(fmt->second.text, {L"", L"NV12", L"P010", L"BGRA"})) return std::nullopt;
        f.format = fmt->second.text;
        const bool rational = value->fields.count(L"fpsNumerator") != 0;
        if (value->fields.size() != (rational ? 6u : 4u)) return std::nullopt;
        if (rational) {
            if (!Uint(*value, L"fpsNumerator", 1000000000, f.fpsNumerator) ||
                !Uint(*value, L"fpsDenominator", 1000000000, f.fpsDenominator) ||
                f.fpsDenominator == 0 ||
                static_cast<uint64_t>(f.fpsNumerator) > 1000ull * f.fpsDenominator)
                return std::nullopt;
            if (f.fpsNumerator) f.fps = f.fpsNumerator / f.fpsDenominator;
        }
    } else {
        if (!value || !Text(*value, action == L"setManualHdmiSourceCustom" ? 4096 :
                                   action == L"setPreferredDevice" ? 1024 : 64))
            return std::nullopt;
        result.text = value->text;
        if (action == L"setLanguage") {
            if (!OneOf(result.text, {L"system", L"en-US", L"zh-TW", L"zh-CN"})) return std::nullopt;
        } else if (action == L"setManualHdmiSource") {
            if (!IsValidManualHdmiSource(std::wstring_view(result.text))) return std::nullopt;
        } else if (action == L"setManualHdmiSourceCustom") {
            if (!NormalizeManualHdmiSourceCustom(std::wstring_view(result.text))) return std::nullopt;
            result.text = TrimManualHdmiSourceCustom(result.text);
        } else if (action == L"setNoSignalMode") {
            if (!OneOf(result.text, {L"default", L"image"})) return std::nullopt;
        } else if (action == L"setNoSignalFit") {
            if (!OneOf(result.text, {L"contain", L"cover", L"stretch"})) return std::nullopt;
        } else if (action == L"setPreferredDevice") {
            if (result.text.empty()) return std::nullopt;
        } else if (action == L"setGame" || action == L"addGameFavorite" || action == L"removeGameFavorite") {
            std::string id;
            for (auto c : result.text) {
                if (c > 127) return std::nullopt;
                id += static_cast<char>(c);
            }
            if (!id.empty() && !FindGameById(id) && !Games::DynamicId(id))
                return std::nullopt;
        } else return std::nullopt;
    }
    return result;
}
} // namespace NitLink
