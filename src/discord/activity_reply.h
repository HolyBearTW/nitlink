#pragma once

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>

namespace NitLink {

enum class ActivityReplyKind { Accepted, Error, Unrelated, Malformed };
struct ActivityReply {
    ActivityReplyKind kind=ActivityReplyKind::Malformed;
    std::string nonce, name, largeImage, message, diagnostic;
    int errorCode=0;
};

// No field accessor assumes that a present key has the expected JSON type.
// In particular GetNamedString(key, default) does NOT accept JSON null.
inline ActivityReply ParseSetActivityReply(std::string_view payload,
    std::string_view expectedNonce, bool clearing=false) noexcept {
    ActivityReply result;
    try {
        using namespace winrt::Windows::Data::Json;
        JsonObject root;
        if (payload.size()>65536 || !JsonObject::TryParse(winrt::to_hstring(payload),root)) {
            result.diagnostic="Invalid JSON in RPC frame";return result;
        }
        const auto stringField=[&](const JsonObject& object,const wchar_t* key,
            std::string& target,bool required,const char* label) {
            if (!object.HasKey(key)) {
                if (required)result.diagnostic=std::string("Missing ")+label;
                return !required;
            }
            const auto value=object.GetNamedValue(key);
            if (value.ValueType()==JsonValueType::Null && !required) return true;
            if (value.ValueType()!=JsonValueType::String) {
                result.diagnostic=std::string("Invalid JSON type for ")+label;return false;
            }
            target=winrt::to_string(value.GetString());
            if (required && target.empty()) {
                result.diagnostic=std::string("Empty ")+label;return false;
            }
            return true;
        };
        std::string command;
        if (!stringField(root,L"cmd",command,true,"cmd")) return result;
        if (command!="SET_ACTIVITY") { result.kind=ActivityReplyKind::Unrelated;return result; }
        if (!stringField(root,L"nonce",result.nonce,true,"nonce")) return result;
        if (expectedNonce.empty() || result.nonce!=expectedNonce) {
            result.kind=ActivityReplyKind::Unrelated;
            result.diagnostic="Unknown or stale SET_ACTIVITY nonce";return result;
        }
        std::string event;
        if (!stringField(root,L"evt",event,false,"evt")) return result;
        if (!event.empty() && event!="ERROR") {
            result.diagnostic="Unexpected SET_ACTIVITY event";return result;
        }
        if (!root.HasKey(L"data")) { result.diagnostic="Missing SET_ACTIVITY data";return result; }
        const auto value=root.GetNamedValue(L"data");
        if (value.ValueType()==JsonValueType::Null && clearing && event.empty()) {
            result.kind=ActivityReplyKind::Accepted;return result;
        }
        if (value.ValueType()!=JsonValueType::Object) {
            result.diagnostic="SET_ACTIVITY data is not an object";return result;
        }
        const auto data=value.GetObject();
        if (event=="ERROR") {
            if (!stringField(data,L"message",result.message,false,"data.message")) return result;
            if (data.HasKey(L"code")) {
                const auto code=data.GetNamedValue(L"code");
                if (code.ValueType()!=JsonValueType::Number || !std::isfinite(code.GetNumber()) ||
                    std::floor(code.GetNumber())!=code.GetNumber() ||
                    code.GetNumber()<std::numeric_limits<int>::min() ||
                    code.GetNumber()>std::numeric_limits<int>::max()) {
                    result.diagnostic="Invalid JSON type/range for data.code";return result;
                }
                result.errorCode=static_cast<int>(code.GetNumber());
            }
            result.kind=ActivityReplyKind::Error;return result;
        }
        if (!stringField(data,L"name",result.name,false,"data.name")) return result;
        if (data.HasKey(L"assets")) {
            const auto assets=data.GetNamedValue(L"assets");
            if (assets.ValueType()!=JsonValueType::Null) {
                if (assets.ValueType()!=JsonValueType::Object) {
                    result.diagnostic="Invalid JSON type for data.assets";return result;
                }
                if (!stringField(assets.GetObject(),L"large_image",result.largeImage,false,"data.assets.large_image")) return result;
            }
        }
        result.kind=ActivityReplyKind::Accepted;
    } catch (const winrt::hresult_error& error) {
        result.kind=ActivityReplyKind::Malformed;
        result.diagnostic="WinRT JSON parsing failed (HRESULT "+std::to_string(error.code().value)+")";
    } catch (...) {
        result.kind=ActivityReplyKind::Malformed;
        result.diagnostic="RPC JSON parsing failed";
    }
    return result;
}
} // namespace NitLink
