#include "discord/activity_reply.h"
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace NitLink;
int main() {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        struct Case { const char* name; const char* json; ActivityReplyKind expected; bool clearing=false; };
        using K=ActivityReplyKind;
        const Case cases[]={
            {"evt:null",R"({"cmd":"SET_ACTIVITY","evt":null,"nonce":"1001","data":{"name":"NitLink"}})",K::Accepted},
            {"evt absent",R"({"cmd":"SET_ACTIVITY","nonce":"1001","data":{"name":"NitLink"}})",K::Accepted},
            {"evt ERROR",R"({"cmd":"SET_ACTIVITY","evt":"ERROR","nonce":"1001","data":{"code":4000,"message":"invalid activity"}})",K::Error},
            {"evt empty",R"({"cmd":"SET_ACTIVITY","evt":"","nonce":"1001","data":{"name":"NitLink"}})",K::Accepted},
            {"evt number",R"({"cmd":"SET_ACTIVITY","evt":42,"nonce":"1001","data":{}})",K::Malformed},
            {"evt object",R"({"cmd":"SET_ACTIVITY","evt":{},"nonce":"1001","data":{}})",K::Malformed},
            {"evt bool",R"({"cmd":"SET_ACTIVITY","evt":false,"nonce":"1001","data":{}})",K::Malformed},
            {"evt array",R"({"cmd":"SET_ACTIVITY","evt":[],"nonce":"1001","data":{}})",K::Malformed},
            {"data null during update",R"({"cmd":"SET_ACTIVITY","evt":null,"nonce":"1001","data":null})",K::Malformed},
            {"data null after clear",R"({"cmd":"SET_ACTIVITY","evt":null,"nonce":"1001","data":null})",K::Accepted,true},
            {"data absent",R"({"cmd":"SET_ACTIVITY","evt":null,"nonce":"1001"})",K::Malformed},
            {"data wrong type",R"({"cmd":"SET_ACTIVITY","evt":null,"nonce":"1001","data":[]})",K::Malformed},
            {"nonce absent",R"({"cmd":"SET_ACTIVITY","evt":null,"data":{}})",K::Malformed},
            {"nonce mismatch",R"({"cmd":"SET_ACTIVITY","evt":null,"nonce":"other","data":{}})",K::Unrelated},
            {"nonce null",R"({"cmd":"SET_ACTIVITY","evt":null,"nonce":null,"data":{}})",K::Malformed},
            {"nonce number",R"({"cmd":"SET_ACTIVITY","evt":null,"nonce":1001,"data":{}})",K::Malformed},
            {"cmd null",R"({"cmd":null,"evt":null,"nonce":"1001","data":{}})",K::Malformed},
            {"cmd object",R"({"cmd":{},"evt":null,"nonce":"1001","data":{}})",K::Malformed},
            {"other command",R"({"cmd":"DISPATCH","evt":"READY","nonce":"1001","data":{}})",K::Unrelated},
            {"invalid JSON",R"({"cmd":"SET_ACTIVITY",)",K::Malformed},
            {"null root",R"(null)",K::Malformed},
            {"normal success",R"({"cmd":"SET_ACTIVITY","evt":null,"nonce":"1001","data":{"name":"Switch 2","assets":{"large_image":"mp:external/test/https/example.png"}}})",K::Accepted},
            {"assets null",R"({"cmd":"SET_ACTIVITY","nonce":"1001","data":{"name":null,"assets":null}})",K::Accepted},
            {"image null",R"({"cmd":"SET_ACTIVITY","nonce":"1001","data":{"assets":{"large_image":null}}})",K::Accepted},
            {"assets invalid",R"({"cmd":"SET_ACTIVITY","nonce":"1001","data":{"assets":42}})",K::Malformed},
            {"image invalid",R"({"cmd":"SET_ACTIVITY","nonce":"1001","data":{"assets":{"large_image":{}}}})",K::Malformed},
            {"name invalid",R"({"cmd":"SET_ACTIVITY","nonce":"1001","data":{"name":42}})",K::Malformed},
            {"error message invalid",R"({"cmd":"SET_ACTIVITY","evt":"ERROR","nonce":"1001","data":{"code":4000,"message":{}}})",K::Malformed},
            {"error code invalid",R"({"cmd":"SET_ACTIVITY","evt":"ERROR","nonce":"1001","data":{"code":"4000"}})",K::Malformed},
            {"error code out of range",R"({"cmd":"SET_ACTIVITY","evt":"ERROR","nonce":"1001","data":{"code":1e100}})",K::Malformed}
        };
        for (const auto& item:cases) {
            const auto reply=ParseSetActivityReply(item.json,"1001",item.clearing);
            if (reply.kind!=item.expected)throw std::runtime_error(std::string("Wrong classification: ")+item.name);
            if (reply.kind==K::Malformed && reply.diagnostic.empty())throw std::runtime_error("missing malformed-response diagnostic");
            // A bad frame must never poison subsequent parsing on the worker's apartment.
            if (ParseSetActivityReply(cases[0].json,"1001").kind!=K::Accepted)throw std::runtime_error("parsing did not recover after malformed frame");
            std::cout<<"PASS "<<item.name<<'\n';
        }
        const auto error=ParseSetActivityReply(cases[2].json,"1001");
        if(error.errorCode!=4000 || error.message!="invalid activity")throw std::runtime_error("RPC error code/message were not retained");
        for(unsigned i=0;i<1000;++i)if(ParseSetActivityReply(cases[0].json,"1001").kind!=K::Accepted)throw std::runtime_error("repeated null event failed");
        if(ParseSetActivityReply(cases[0].json,"").kind!=K::Unrelated)throw std::runtime_error("unsolicited reply accepted without a sent request");
        winrt::clear_factory_cache();
        std::cout<<"PASS 30 JSON cases, recovery after every case, and 1000 evt:null repetitions\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
}
