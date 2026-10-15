#include "artwork_renderer.h"
#include "game_service.h"
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <shlwapi.h>
#include <algorithm>
#include <array>
#include <cmath>

namespace NitLink::Games {
using Microsoft::WRL::ComPtr;
namespace {
constexpr int Size=512;
using Pixels=std::vector<unsigned char>;
// Three bounded separable box passes approximate a Gaussian blur. BGRA alpha
// compositing preserves transparent logos and avoids stretching source art.
void Blur(Pixels& pixels){
    Pixels output(pixels.size());constexpr int radius=12,window=radius*2+1;
    for(int pass=0;pass<3;++pass)for(int axis=0;axis<2;++axis){
        for(int line=0;line<Size;++line){
            std::array<int,4> sum{};
            const auto index=[&](int p){return (axis?(std::clamp(p,0,Size-1)*Size+line):(line*Size+std::clamp(p,0,Size-1)))*4;};
            for(int p=-radius;p<=radius;++p)for(int c=0;c<4;++c)sum[c]+=pixels[index(p)+c];
            for(int p=0;p<Size;++p){
                for(int c=0;c<4;++c){output[index(p)+c]=static_cast<unsigned char>(sum[c]/window);sum[c]+=pixels[index(p+radius+1)+c]-pixels[index(p-radius)+c];}
            }
        }pixels.swap(output);
    }
}
}
std::shared_ptr<Image> RenderSquareArtwork(const Image& image,std::wstring_view mode,double x,double y,double zoom){
    if(mode==L"original")return std::make_shared<Image>(image);
    if((mode!=L"fit"&&mode!=L"crop")||!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(zoom)||x<0||x>1||y<0||y>1||zoom<1||zoom>3)return {};
    ComPtr<IWICImagingFactory> factory;
    if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))))return {};
    ComPtr<IStream> input;input.Attach(SHCreateMemStream(image.png.data(),static_cast<UINT>(image.png.size())));
    ComPtr<IWICBitmapDecoder> decoder;ComPtr<IWICBitmapFrameDecode> frame;ComPtr<IWICFormatConverter> converter;
    UINT width=0,height=0;
    if(!input||FAILED(factory->CreateDecoderFromStream(input.Get(),nullptr,WICDecodeMetadataCacheOnDemand,&decoder))||FAILED(decoder->GetFrame(0,&frame))||
       FAILED(frame->GetSize(&width,&height))||!width||!height||width>512||height>512||FAILED(factory->CreateFormatConverter(&converter))||
       FAILED(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)))return {};
    Pixels source(static_cast<size_t>(width)*height*4),pixels(Size*Size*4);
    if(FAILED(converter->CopyPixels(nullptr,width*4,static_cast<UINT>(source.size()),source.data())))return {};
    const auto draw=[&](double scale,double posX,double posY,bool foreground){
        const double dw=width*scale,dh=height*scale,left=foreground?(Size-dw)/2:-(dw-Size)*posX,top=foreground?(Size-dh)/2:-(dh-Size)*posY;
        for(int row=0;row<Size;++row)for(int col=0;col<Size;++col){
            if(foreground&&(col<left||col>=left+dw||row<top||row>=top+dh))continue;
            const double sx=std::clamp((col+.5-left)/scale-.5,0.,double(width-1)),sy=std::clamp((row+.5-top)/scale-.5,0.,double(height-1));
            const auto ix=static_cast<UINT>(sx),iy=static_cast<UINT>(sy),jx=std::min(ix+1,width-1),jy=std::min(iy+1,height-1);
            const auto sample=[&](int channel){const double a=source[(iy*width+ix)*4+channel]*(1-(sx-ix))+source[(iy*width+jx)*4+channel]*(sx-ix);
                const double b=source[(jy*width+ix)*4+channel]*(1-(sx-ix))+source[(jy*width+jx)*4+channel]*(sx-ix);return a*(1-(sy-iy))+b*(sy-iy);};
            const auto p=(row*Size+col)*4;const double alpha=sample(3)/255.;
            for(int c=0;c<3;++c)pixels[p+c]=static_cast<unsigned char>(std::clamp(sample(c)*alpha+(foreground?pixels[p+c]:22)*(1-alpha),0.,255.));pixels[p+3]=255;
        }
    };
    const double cover=std::max(double(Size)/width,double(Size)/height);
    draw(cover*(mode==L"crop"?zoom:1),mode==L"crop"?x:.5,mode==L"crop"?y:.5,false);
    if(mode==L"fit"){Blur(pixels);for(size_t i=0;i<pixels.size();i+=4)for(int c=0;c<3;++c)pixels[i+c]=static_cast<unsigned char>(pixels[i+c]*.65);draw(std::min(double(Size)/width,double(Size)/height),.5,.5,true);}
    ComPtr<IWICBitmap> bitmap;ComPtr<IStream> output;ComPtr<IWICBitmapEncoder> encoder;ComPtr<IWICBitmapFrameEncode> encoded;
    if(FAILED(factory->CreateBitmapFromMemory(Size,Size,GUID_WICPixelFormat32bppBGRA,Size*4,static_cast<UINT>(pixels.size()),pixels.data(),&bitmap))||
       FAILED(CreateStreamOnHGlobal(nullptr,TRUE,&output))||FAILED(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder))||
       FAILED(encoder->Initialize(output.Get(),WICBitmapEncoderNoCache))||FAILED(encoder->CreateNewFrame(&encoded,nullptr))||FAILED(encoded->Initialize(nullptr))||
       FAILED(encoded->WriteSource(bitmap.Get(),nullptr))||FAILED(encoded->Commit())||FAILED(encoder->Commit()))return {};
    STATSTG stat{};if(FAILED(output->Stat(&stat,STATFLAG_NONAME))||!stat.cbSize.QuadPart||stat.cbSize.QuadPart>2*1024*1024)return {};
    auto result=std::make_shared<Image>();result->png.resize(static_cast<size_t>(stat.cbSize.QuadPart));LARGE_INTEGER zero{};ULONG read=0;
    if(FAILED(output->Seek(zero,STREAM_SEEK_SET,nullptr))||FAILED(output->Read(result->png.data(),static_cast<ULONG>(result->png.size()),&read))||read!=result->png.size())return {};
    return result;
}
}
