#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace NitLink::AudioConvert {
struct Format {
    unsigned channels = 0, bits = 0, validBits = 0;
    uint32_t mask = 0;
    bool floating = false;
};
inline bool Valid(const Format& f) {
    if (!f.channels || f.channels > 32) return false;
    if (f.mask && std::popcount(f.mask) != f.channels) return false;
    if (f.floating) return f.bits == 32 || f.bits == 64;
    return (f.bits == 8 || f.bits == 16 || f.bits == 24 || f.bits == 32) &&
        f.validBits > 0 && f.validBits <= f.bits && (f.bits != 8 || f.validBits == 8);
}
inline double Read(const uint8_t* p, const Format& f) {
    if (f.floating) {
        double value;
        if (f.bits == 32) { float v; std::memcpy(&v,p,4); value=v; }
        else std::memcpy(&value,p,8);
        return std::isfinite(value) ? value : 0;
    }
    if (f.bits == 8) return (int(*p)-128) / 128.0;
    uint32_t u=0;
    for (unsigned i=0; i<f.bits/8; ++i) u |= uint32_t(p[i]) << (8*i);
    u >>= f.bits-f.validBits;
    const int64_t sign=int64_t(1) << (f.validBits-1);
    const int64_t n=(u & uint64_t(sign)) ? int64_t(u) - (sign*2) : int64_t(u);
    return double(n)/double(sign);
}
inline void Write(uint8_t* p, const Format& f, double value) {
    if (!std::isfinite(value)) value=0;
    value=std::clamp(value,-1.0,1.0);
    if (f.floating) {
        if (f.bits==32) { const float v=float(value); std::memcpy(p,&v,4); }
        else std::memcpy(p,&value,8);
        return;
    }
    if (f.bits==8) { *p=uint8_t(std::clamp(std::lround(value*128+128),0L,255L)); return; }
    const int64_t scale=int64_t(1) << (f.validBits-1);
    const int64_t n=std::clamp(int64_t(std::llround(value*scale)), -scale, scale-1);
    const uint32_t u=uint32_t(n) << (f.bits-f.validBits);
    for (unsigned i=0;i<f.bits/8;++i) p[i]=uint8_t(u >> (8*i));
}
inline uint32_t Mask(const Format& f) {
    return f.mask ? f.mask : f.channels==1 ? 4u : f.channels==2 ? 3u : 0u;
}
inline bool CanMap(const Format& in, const Format& out) {
    if (!Valid(in) || !Valid(out)) return false;
    if (in.channels==out.channels && Mask(in)==Mask(out)) return true;
    if (in.channels==1 && Mask(in)==4 && out.channels==2 && Mask(out)==3) return true;
    if (in.channels==2 && Mask(in)==3 && out.channels==1 && Mask(out)==4) return true;
    return in.channels<=2 && Mask(in)==(in.channels==1 ? 4u : 3u) && (Mask(out)&3)==3;
}
// No invented surround content: stereo is placed only at FL/FR. LFE and
// rear channels remain silent. Unsupported layouts use Windows conversion.
inline void Map(const double* input, const Format& in, double* output, const Format& out) {
    std::fill(output,output+out.channels,0.0);
    if (in.channels==out.channels && Mask(in)==Mask(out)) {
        std::copy(input,input+in.channels,output); return;
    }
    if (out.channels==1) { output[0]=(input[0]+input[1])*0.5; return; }
    unsigned index=0;
    for (unsigned bit=0;bit<32;++bit) if (Mask(out)&(uint32_t(1)<<bit)) {
        if (bit==0) output[index]=input[0];
        if (bit==1) output[index]=input[in.channels==1 ? 0 : 1];
        ++index;
    }
}

// Polyphase Blackman-windowed sinc. Tables are allocated only at endpoint
// setup; the audio pump does no allocation or coefficient construction.
class SincKernel {
public:
    static constexpr unsigned Phases=512;
    bool Initialize(double sourcePerOutput) {
        if (!std::isfinite(sourcePerOutput) || sourcePerOutput<0.125 || sourcePerOutput>8) return false;
        taps_=64*unsigned(std::ceil(std::max(1.0,sourcePerOutput)));
        const double cutoff=0.94/std::max(1.0,sourcePerOutput*1.002);
        try { table_.resize(size_t(Phases+1)*taps_); } catch (...) { return false; }
        constexpr double pi=3.14159265358979323846;
        for (unsigned phase=0;phase<=Phases;++phase) {
            const double fraction=double(phase)/Phases;
            double sum=0;
            for (unsigned tap=0;tap<taps_;++tap) {
                const double x=double(tap)-History()-fraction;
                const double radius=x/(taps_/2.0);
                const double window=std::abs(radius)<1 ? 0.42+0.5*std::cos(pi*radius)+0.08*std::cos(2*pi*radius) : 0;
                const double sinc=std::abs(x)<1e-12 ? cutoff : std::sin(pi*cutoff*x)/(pi*x);
                table_[size_t(phase)*taps_+tap]=float(sinc*window);
                sum+=sinc*window;
            }
            for (unsigned tap=0;tap<taps_;++tap) table_[size_t(phase)*taps_+tap]/=float(sum);
        }
        return true;
    }
    unsigned Taps() const { return taps_; }
    unsigned History() const { return taps_/2-1; }
    const float* Phase(unsigned p) const { return table_.data()+size_t(p)*taps_; }
    bool Ready() const { return !table_.empty(); }
private:
    unsigned taps_=64;
    std::vector<float> table_;
};

inline uint32_t SelectPeriod(uint32_t requested, uint32_t fundamental, uint32_t minimum, uint32_t maximum) {
    if (!fundamental || !minimum || minimum>maximum) return 0;
    const uint64_t low=(uint64_t(minimum)+fundamental-1)/fundamental*fundamental;
    const uint64_t high=uint64_t(maximum)/fundamental*fundamental;
    if (low>high) return 0;
    return uint32_t(std::clamp((uint64_t(requested)+fundamental-1)/fundamental*fundamental,low,high));
}
}
