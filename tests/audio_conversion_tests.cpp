#include "audio/audio_convert.h"
#include <iostream>
#include <stdexcept>
#include <limits>
using namespace NitLink::AudioConvert;
void Check(bool v,const char* message) { if (!v) throw std::runtime_error(message); }
int main() {
    try {
        for (unsigned bits : {8u,16u,24u,32u}) {
            const Format f{2,bits,bits,3,false};
            for (double v : {-1.0,-0.5,0.0,0.5,0.99}) {
                uint8_t data[8]{}; Write(data,f,v);
                Check(std::abs(Read(data,f)-v)<=1.0/std::pow(2.0,bits-1),"integer PCM round trip");
            }
        }
        Format padded{2,32,24,3,false}; uint8_t data[8]{};
        Write(data,padded,-0.5); Check(data[0]==0 && Read(data,padded)==-0.5,"24-in-32 left alignment");
        Write(data,padded,2); Check(Read(data,padded)<1 && Read(data,padded)>.999,"integer saturation");
        Format floating{2,64,64,3,true};
        double nan=std::numeric_limits<double>::quiet_NaN(); std::memcpy(data,&nan,8);
        Check(Read(data,floating)==0,"nonfinite input silenced");
        Format stereo{2,32,32,3,true}, surround{6,32,32,0x3f,true}, mono{1,16,16,4,false};
        Check(CanMap(stereo,surround) && CanMap(stereo,mono) && CanMap(mono,surround),"supported channel maps");
        Check(!CanMap(surround,stereo),"unsupported downmix uses fallback");
        double input[2]{.25,-.5}, output[6]{};
        Map(input,stereo,output,surround);
        Check(output[0]==.25 && output[1]==-.5,"front channel placement");
        for (int i=2;i<6;++i) Check(output[i]==0,"no invented LFE or surround signal");
        Map(input,stereo,output,mono); Check(output[0]==-.125,"stereo to mono");
        Check(SelectPeriod(48,4,48,448)==48 && SelectPeriod(49,4,48,448)==52,"period alignment");
        Check(SelectPeriod(1,4,48,448)==48 && SelectPeriod(999,4,48,448)==448,"period clamp");
        Check(SelectPeriod(UINT32_MAX,UINT32_MAX,1,UINT32_MAX)==UINT32_MAX,"period overflow safe");
        Check(SelectPeriod(48,0,48,448)==0 && SelectPeriod(1,16,17,20)==0,"invalid driver geometry");
        constexpr double pi=3.14159265358979323846;
        for (double ratio : {44100.0/48000,1.0,48000.0/44100,2.0,4.0}) {
            SincKernel kernel; Check(kernel.Initialize(ratio),"sinc table prepared");
            for (unsigned phase : {0u,17u,256u,511u,512u}) {
                double sum=0, passRe=0,passIm=0,stopRe=0,stopIm=0;
                const double passFrequency=.01;
                const double stopFrequency=ratio>1.5 ? .75/ratio : .499;
                for (unsigned i=0;i<kernel.Taps();++i) {
                    const double w=kernel.Phase(phase)[i]; sum+=w;
                    passRe+=w*std::cos(2*pi*passFrequency*i); passIm+=w*std::sin(2*pi*passFrequency*i);
                    stopRe+=w*std::cos(2*pi*stopFrequency*i); stopIm+=w*std::sin(2*pi*stopFrequency*i);
                }
                Check(std::abs(sum-1)<1e-5,"DC normalization");
                Check(std::abs(std::hypot(passRe,passIm)-1)<.005,"passband preservation");
                if (ratio>1.5) Check(std::hypot(stopRe,stopIm)<.001,"downsample alias rejection >60dB at test frequency");
            }
        }
        SincKernel invalid;
        Check(!invalid.Initialize(0) && !invalid.Initialize(9) && !invalid.Initialize(nan),"unsupported rate bounded");
        std::cout << "Native PCM/float conversion, channel maps, period negotiation and sinc response passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
