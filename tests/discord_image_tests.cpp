#include "discord/discord_rpc.h"
#include "discord/pipe_transport.h"
#include "app/hdmi_source_display.h"
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace NitLink {
struct DiscordRPCTestAccess {
    static void SetOpener(DiscordRPC& rpc, std::function<HANDLE()> opener) { rpc.m_pipeOpener=std::move(opener); }
    static void SetProbe(DiscordRPC& rpc, std::function<DiscordImageCheck(std::string_view,std::string_view,HANDLE)> probe) { rpc.m_imageProbe=std::move(probe); }
};
}
using namespace NitLink;
using namespace winrt::Windows::Data::Json;
namespace {
unsigned checks=0;
void Check(bool ok, const char* message) { ++checks;if (!ok) throw std::runtime_error(message); }
constexpr const char* Image="https://cdn2.steamgriddb.com/grid/72654158f52bf25f9a8101f78aceaa2f.jpg";
constexpr const char* Proxy="mp:external/test-token/https/cdn2.steamgriddb.com/grid/72654158f52bf25f9a8101f78aceaa2f.jpg";
constexpr const char* SquareFallback1="https://cdn2.steamgriddb.com/grid/11111111111111111111111111111111.jpg";
constexpr const char* SquareFallback1Proxy="mp:external/fallback-one/https/cdn2.steamgriddb.com/grid/11111111111111111111111111111111.jpg";
constexpr const char* SquareFallback2="https://cdn2.steamgriddb.com/grid/22222222222222222222222222222222.png";
constexpr const char* SquareFallback2Proxy="mp:external/fallback-two/https/cdn2.steamgriddb.com/grid/22222222222222222222222222222222.png";
const auto Started=std::chrono::system_clock::time_point{std::chrono::seconds(1700000000)};
struct Pipe {
    HANDLE server=INVALID_HANDLE_VALUE, client=INVALID_HANDLE_VALUE;
    Pipe() {
        static unsigned sequence=0;
        const auto name=L"\\\\.\\pipe\\NitLink-image-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(++sequence);
        server=CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,65536,65536,0,nullptr);
        Check(server!=INVALID_HANDLE_VALUE,"create isolated image pipe");
        client=OpenRpcPipe(name.c_str()); Check(client!=INVALID_HANDLE_VALUE,"open isolated image pipe");
        Check(ConnectNamedPipe(server,nullptr)||GetLastError()==ERROR_PIPE_CONNECTED,"connect image pipe");
        Send("{}");
    }
    ~Pipe() { if(client!=INVALID_HANDLE_VALUE)CloseHandle(client);if(server!=INVALID_HANDLE_VALUE)CloseHandle(server); }
    void Send(const std::string& body) {
        const uint32_t header[]={1,static_cast<uint32_t>(body.size())};DWORD written=0;
        Check(WriteFile(server,header,sizeof(header),&written,nullptr)&&written==sizeof(header),"write reply header");
        Check(WriteFile(server,body.data(),static_cast<DWORD>(body.size()),&written,nullptr)&&written==body.size(),"write reply body");
    }
    JsonObject Receive(uint32_t opcode=1) {
        const auto deadline=GetTickCount64()+6000;
        const auto wait=[&](DWORD count) { DWORD available=0;
            while(GetTickCount64()<deadline) { if(PeekNamedPipe(server,nullptr,0,nullptr,&available,nullptr)&&available>=count)return;Sleep(5); }
            throw std::runtime_error("bounded image wire wait");
        };
        wait(8);uint32_t header[2]{};DWORD read=0;
        Check(ReadFile(server,header,8,&read,nullptr)&&read==8&&header[0]==opcode&&header[1]<=65536,"wire header");
        wait(header[1]);std::string body(header[1],'\0');
        Check(ReadFile(server,body.data(),header[1],&read,nullptr)&&read==header[1],"wire body");
        return JsonObject::Parse(winrt::to_hstring(body));
    }
    void Connect(DiscordRPC& rpc) {
        DiscordRPCTestAccess::SetOpener(rpc,[&]{return std::exchange(client,INVALID_HANDLE_VALUE);});
        Check(rpc.Connect("12345"),"isolated image handshake");Receive(0);
    }
    void Accept(const JsonObject& request, const char* image=Proxy, bool nullAssets=false) {
        const auto nonce=winrt::to_string(request.GetNamedString(L"nonce"));
        Send("{\"cmd\":\"SET_ACTIVITY\",\"evt\":null,\"nonce\":\""+nonce+
            "\",\"data\":{\"name\":\"Switch 2\",\"assets\":"+(nullAssets?"null":std::string("{\"large_image\":\"")+image+"\"}")+"}}");
    }
    static JsonObject Activity(const JsonObject& request) { return request.GetNamedObject(L"args").GetNamedObject(L"activity"); }
};
void WaitStatus(DiscordRPC& rpc,const wchar_t* status) {
    const auto deadline=GetTickCount64()+4000;
    while(GetTickCount64()<deadline) { if(std::wstring_view(rpc.ImageStatus())==status)return;Sleep(10); }
    throw std::runtime_error("expected image acknowledgement/status");
}
void CheckImage(const JsonObject& request,const char* expected) {
    const auto activity=Pipe::Activity(request);
    Check(activity.GetNamedObject(L"timestamps").GetNamedNumber(L"start")==1700000000,"image changes retain exact start timestamp");
    if(!expected)Check(!activity.HasKey(L"assets"),"invalid/unavailable image omits assets instead of inventing a key");
    else Check(winrt::to_string(activity.GetNamedObject(L"assets").GetNamedString(L"large_image"))==expected,"exact public URL reaches SET_ACTIVITY");
}
void TestSelectionAndProxyValidation() {
    Games::SavedGame game;game.id=5303117;game.coverUrl=std::wstring(winrt::to_hstring(Image));
    Check(DiscordCoverImage(&game,true)==Image,"library selects original HTTPS URL");
    game.artworkMode=L"crop";game.finalImageKey=std::wstring(64,L'a');
    Check(DiscordCoverImage(&game,true)==Image,"local crop is not passed off as remotely hosted artwork");
    Check(DiscordCoverImage(&game,false).empty()&&DiscordCoverImage(nullptr,true).empty(),"unvalidated image is omitted");
    for(const auto* invalid:{L"5303117",L"sgdb-5303117",L"file:///C:/cover.png",L"C:\\cover.png",L"https://nitlink.invalid/game-images/1.png"}) {
        game.coverUrl=invalid;Check(DiscordCoverImage(&game,true).empty(),"local paths and identifiers are never URLs");
    }
    Check(DiscordImageProxyPath(Proxy,Image)==L"/external/test-token/https/cdn2.steamgriddb.com/grid/72654158f52bf25f9a8101f78aceaa2f.jpg","proxy path binds to exact requested image");
    for(const auto* invalid:{"mp:external/../https/cdn2.steamgriddb.com/grid/72654158f52bf25f9a8101f78aceaa2f.jpg",
        "mp:external/test/https/evil.example/cover.png","https://evil.example/cover.png","mp:external/test-token/http/cdn2.steamgriddb.com/grid/72654158f52bf25f9a8101f78aceaa2f.jpg"})
        Check(DiscordImageProxyPath(invalid,Image).empty(),"untrusted or redirected proxy target rejected");
}
void TestSuccessfulNullEvent() {
    Pipe pipe;DiscordRPC rpc;pipe.Connect(rpc);
    DiscordRPCTestAccess::SetProbe(rpc,[](auto,auto,HANDLE){return DiscordImageCheck{DiscordImageCheckState::Available,200,0,{}};});
    rpc.SetActivity(L"王國之淚",L"1920×1080 · 60 FPS",Started,Image,L"王國之淚",L"Switch 2");
    const auto request=pipe.Receive();CheckImage(request,Image);pipe.Accept(request);
    WaitStatus(rpc,L"externalAvailable");Check(std::wstring_view(rpc.NameStatus())==L"accepted","evt:null success acknowledges dynamic name");
    rpc.SetActivity(L"王國之淚",L"1920×1080 · 60 FPS",Started,Image,L"王國之淚",L"Switch 2");
    Sleep(2200);DWORD available=0;
    Check(PeekNamedPipe(pipe.server,nullptr,0,nullptr,&available,nullptr)&&available==0,"unchanged state does not resend activity");
    rpc.Disconnect();
}
void TestMalformedReplyWorkerRecovery() {
    Pipe pipe;DiscordRPC rpc;pipe.Connect(rpc);
    DiscordRPCTestAccess::SetProbe(rpc,[](auto,auto,HANDLE){return DiscordImageCheck{DiscordImageCheckState::Available,200,0,{}};});
    rpc.SetActivity(L"test game",L"signal",Started,Image,L"test game",L"Switch 2");
    const auto request=pipe.Receive();
    const auto nonce=winrt::to_string(request.GetNamedString(L"nonce"));
    pipe.Send("{");
    pipe.Send("{\"cmd\":\"SET_ACTIVITY\",\"evt\":null,\"data\":{}}");
    pipe.Send("{\"cmd\":\"SET_ACTIVITY\",\"evt\":null,\"nonce\":\"not-our-request\",\"data\":{}}");
    for(const char* event:{"42","{}","false","[]"})
        pipe.Send("{\"cmd\":\"SET_ACTIVITY\",\"evt\":"+std::string(event)+",\"nonce\":\""+nonce+"\",\"data\":{}}");
    pipe.Send("{\"cmd\":\"SET_ACTIVITY\",\"evt\":null,\"nonce\":\""+nonce+"\",\"data\":null}");
    Sleep(1500);
    Check(std::wstring_view(rpc.ActivityStatus())==L"pending"&&rpc.IsConnected(),"malformed/unrelated replies never count as success or disconnect the worker");
    pipe.Accept(request);
    WaitStatus(rpc,L"externalAvailable");
    Check(std::wstring_view(rpc.ActivityStatus())==L"accepted","worker processes a valid reply after malformed frames");
    DWORD available=0;Check(PeekNamedPipe(pipe.server,nullptr,0,nullptr,&available,nullptr)&&available==0,"malformed replies cause no extra sends/reconnects");
    rpc.Disconnect();
}
void TestNameErrorKeepsImage() {
    Pipe pipe;DiscordRPC rpc;pipe.Connect(rpc);
    DiscordRPCTestAccess::SetProbe(rpc,[](auto,auto,HANDLE){return DiscordImageCheck{DiscordImageCheckState::Available,200,0,{}};});
    rpc.SetActivity(L"王國之淚",L"signal",Started,Image,L"王國之淚",L"Switch 2");
    const auto first=pipe.Receive();
    const auto nonce=winrt::to_string(first.GetNamedString(L"nonce"));
    pipe.Send("{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\""+nonce+"\",\"evt\":\"ERROR\",\"data\":{\"code\":4000,\"message\":\"unsupported name\"}}");
    const auto retry=pipe.Receive();CheckImage(retry,Image);
    Check(!Pipe::Activity(retry).HasKey(L"name"),"name comparison retries without name");
    pipe.Accept(retry);WaitStatus(rpc,L"externalAvailable");rpc.Disconnect();
}
void TestProxyFailureAndRetry() {
    Pipe pipe;DiscordRPC rpc;pipe.Connect(rpc);std::atomic<bool> readable=false;
    DiscordRPCTestAccess::SetProbe(rpc,[&](auto,auto,HANDLE){return readable.load()?DiscordImageCheck{DiscordImageCheckState::Available,200,0,{}}:
        DiscordImageCheck{DiscordImageCheckState::HttpError,415,0,L"Discord image proxy HTTP 415"};});
    rpc.SetActivity(L"災厄啟示錄",L"signal",Started,Image,L"災厄啟示錄",L"Switch 2");
    auto request=pipe.Receive();pipe.Accept(request);WaitStatus(rpc,L"proxyFailed");
    Check(rpc.ImageError().find(L"415")!=std::wstring::npos,"failure includes actual proxy HTTP status");
    CheckImage(pipe.Receive(),nullptr);
    rpc.SetActivity(L"災厄啟示錄",L"new HDMI signal",Started,Image,L"災厄啟示錄",L"Switch 2");
    CheckImage(pipe.Receive(),nullptr);
    Check(rpc.ImageError().find(L"415")!=std::wstring::npos,"source refresh preserves image failure reason");
    rpc.SetActivity(L"NitLink viewer",L"signal",Started,"",L"NitLink",L"Switch 2");
    CheckImage(pipe.Receive(),nullptr);
    Check(rpc.ImageError().empty(),"leaving a failed cover clears that cover's old error");
    readable=true;rpc.RetryImage();
    rpc.SetActivity(L"災厄啟示錄",L"new HDMI signal",Started,Image,L"災厄啟示錄",L"Switch 2");
    request=pipe.Receive();CheckImage(request,Image);pipe.Accept(request);WaitStatus(rpc,L"externalAvailable");
    Check(rpc.ImageError().empty(),"explicit successful retry clears prior error");rpc.Disconnect();
}
void TestUnresolvedAndInvalidWireImages() {
    Pipe pipe;DiscordRPC rpc;pipe.Connect(rpc);
    rpc.SetActivity(L"test game",L"signal",Started,Image,L"test game",L"Switch 2");
    auto request=pipe.Receive();pipe.Accept(request,"",true);WaitStatus(rpc,L"externalUnresolved");
    for(const char* invalid:{"sgdb-5303117","5303117","file:///C:/cover.png","https://nitlink.invalid/game-images/1.png"}) {
        // Restore a real cover before each invalid value: equal empty activities
        // are intentionally deduplicated, but each invalid input must clear art.
        rpc.SetActivity(L"test game",L"restored signal",Started,Image,L"test game",L"Switch 2");
        CheckImage(pipe.Receive(),Image);
        rpc.SetActivity(L"test game",L"signal",Started,invalid,L"test game",L"Switch 2");
        CheckImage(pipe.Receive(),nullptr);
    }
    rpc.Disconnect();
}
void TestTaiwanFallbackKeepsTimerAndSelection() {
    constexpr const char* taiwan="https://www.nintendo.com/tw/switch/zelda-yakusai/img/product/package.jpg";
    constexpr const char* taiwanProxy="mp:external/test-token/https/www.nintendo.com/tw/switch/zelda-yakusai/img/product/package.jpg";
    Pipe pipe;DiscordRPC rpc;pipe.Connect(rpc);
    DiscordRPCTestAccess::SetProbe(rpc,[](auto,std::string_view original,HANDLE){
        return original==Image?DiscordImageCheck{DiscordImageCheckState::HttpError,415,0,L"Discord image proxy HTTP 415"}:
            DiscordImageCheck{DiscordImageCheckState::Available,200,0,{}};
    });
    rpc.SetActivity(L"災厄啟示錄",L"signal",Started,Image,L"災厄啟示錄",L"Switch 2",taiwan);
    auto request=pipe.Receive();CheckImage(request,Image);pipe.Accept(request);
    request=pipe.Receive();CheckImage(request,taiwan);pipe.Accept(request,taiwanProxy);
    WaitStatus(rpc,L"externalAvailable");
    auto snapshot=rpc.GetImageSnapshot();Check(snapshot.usingFallback&&snapshot.url==taiwan,"fallback uses the verified Taiwan URL without changing the original selection");
    rpc.SetActivity(L"災厄啟示錄",L"new signal",Started,Image,L"災厄啟示錄",L"Switch 2",taiwan);
    CheckImage(pipe.Receive(),taiwan);
    rpc.SetActivity(L"災厄啟示錄",L"new signal",Started,Image,L"災厄啟示錄",L"PS5","");
    CheckImage(pipe.Receive(),nullptr);
    Check(!rpc.GetImageSnapshot().usingFallback&&std::wstring_view(rpc.ImageStatus())==L"proxyFailed","removing Switch fallback never claims a stale image is available");
    rpc.RetryImage();rpc.SetActivity(L"災厄啟示錄",L"new signal",Started,Image,L"災厄啟示錄",L"Switch 2",taiwan);
    CheckImage(pipe.Receive(),Image);rpc.Disconnect();
}
void TestSquareFallbackQueueSkipsRejectedImages() {
    Pipe pipe;DiscordRPC rpc;pipe.Connect(rpc);
    DiscordRPCTestAccess::SetProbe(rpc,[](auto,std::string_view original,HANDLE){
        return original==Image||original==SquareFallback1
            ?DiscordImageCheck{DiscordImageCheckState::HttpError,415,0,L"Discord image proxy HTTP 415"}
            :DiscordImageCheck{DiscordImageCheckState::Available,200,0,{}};
    });
    const std::vector<std::string> fallbacks{SquareFallback1,SquareFallback2};
    rpc.SetActivity(L"災厄啟示錄",L"signal",Started,Image,L"災厄啟示錄",L"Switch 2","",fallbacks);
    auto request=pipe.Receive();CheckImage(request,Image);pipe.Accept(request);
    request=pipe.Receive();CheckImage(request,SquareFallback1);pipe.Accept(request,SquareFallback1Proxy);
    request=pipe.Receive();CheckImage(request,SquareFallback2);pipe.Accept(request,SquareFallback2Proxy);
    WaitStatus(rpc,L"externalAvailable");
    const auto snapshot=rpc.GetImageSnapshot();
    Check(snapshot.usingFallback&&snapshot.url==SquareFallback2,"rejected candidates are skipped until a Discord-readable square cover is found");
    rpc.Disconnect();
}
void TestUnsupportedLocalDecoderRetainsRemoteImage() {
    Pipe pipe;DiscordRPC rpc;pipe.Connect(rpc);
    DiscordRPCTestAccess::SetProbe(rpc,[](auto,auto,HANDLE){return DiscordImageCheck{
        DiscordImageCheckState::UnsupportedFormat,200,0,L"Proxy AVIF needs an unavailable local decoder"};});
    rpc.SetActivity(L"test game",L"signal",Started,Image,L"test game",L"Switch 2");
    const auto request=pipe.Receive();CheckImage(request,Image);pipe.Accept(request);
    const auto deadline=GetTickCount64()+4000;
    while(GetTickCount64()<deadline&&rpc.ImageError().empty())Sleep(10);
    Check(std::wstring_view(rpc.ImageStatus())==L"externalAccepted"&&!rpc.ImageError().empty(),"unsupported local decoding remains unverified instead of declaring a proxy failure");
    Check(rpc.GetImageSnapshot().url==Image,"a valid remote image is not removed merely because a local codec is unavailable");
    Sleep(2200);DWORD available=0;
    Check(PeekNamedPipe(pipe.server,nullptr,0,nullptr,&available,nullptr)&&!available,"unsupported local decoding causes no resend or fallback loop");
    rpc.Disconnect();
}
int LiveProbe() {
    DiscordRPC rpc;Check(rpc.Connect("1505211372286246944"),"live Discord connected");
    const auto start=std::chrono::system_clock::now();
    for(const char* url:{Image,"https://www.nintendo.com/tw/switch/zelda-yakusai/img/product/package.jpg",
        "https://store.nintendo.com.hk/media/catalog/product/cache/55351146f864384e20b16a2043f3a323/h/e/hero_nhl_v2.jpg",
        "https://image.api.playstation.com/vulcan/ap/rnd/202406/0500/8f15268257b878597757fcc5f2c9545840867bc71fc863b1.png"}) {
        const bool hongKong=std::string_view(url).starts_with("https://store.nintendo.com.hk/");
        rpc.SetActivity(L"NitLink image diagnostic",L"Live RPC compatibility check",start,url,L"NitLink image diagnostic",L"Switch 2",
            hongKong?"https://www.nintendo.com/tw/switch/zelda-yakusai/img/product/package.jpg":"");
        const auto deadline=GetTickCount64()+20000;
        while(GetTickCount64()<deadline) {
            const std::wstring_view status=rpc.ImageStatus();
            if(status==L"externalAvailable"||status==L"proxyFailed"||status==L"rpcError")break;
            Sleep(50);
        }
        const auto snapshot=rpc.GetImageSnapshot();
        std::cout<<"selected="<<url<<"; large_image="<<snapshot.url<<"; fallback="<<snapshot.usingFallback
            <<"; proxyPreview="<<bool(snapshot.image)<<"; status="<<winrt::to_string(rpc.ImageStatus())
            <<"; error="<<winrt::to_string(rpc.ImageError())<<"; clientRendered=unverified\n";
    }
    rpc.ClearActivity();Sleep(2200);rpc.Disconnect();return 0;
}
}
int main(int argc,char** argv) {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        if(argc==2&&std::string_view(argv[1])=="--live")return LiveProbe();
        const auto run=[](const char* name,auto test){std::cout<<"Checking "<<name<<std::endl;test();};
        run("selected URL",TestSelectionAndProxyValidation);
        run("accepted response",TestSuccessfulNullEvent);run("worker recovery",TestMalformedReplyWorkerRecovery);
        run("name rejection",TestNameErrorKeepsImage);run("image retry",TestProxyFailureAndRetry);
        run("invalid images",TestUnresolvedAndInvalidWireImages);run("Taiwan fallback",TestTaiwanFallbackKeepsTimerAndSelection);
        run("square fallback queue",TestSquareFallbackQueueSkipsRejectedImages);
        run("unavailable local codec",TestUnsupportedLocalDecoderRetainsRemoteImage);
        winrt::clear_factory_cache();
        std::cout<<"PASS "<<checks<<" Discord wire/reply/proxy/retry/timer/window title checks\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
