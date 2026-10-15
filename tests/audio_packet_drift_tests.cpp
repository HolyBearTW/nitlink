#include "audio/drift_controller.h"
#include <cmath>
#include <algorithm>
#include <iostream>
#include <stdexcept>

// Deterministic event simulation: whole capture packets, independent device
// clocks, delayed packet delivery and exact FIR availability/phase accounting.
// This models packet scheduling, not a trace of any particular capture card.
// Initial priming/clock learning is reported separately from steady playback.
struct Result {int early=0,late=0,trims=0;double ppm=0,min=1e9,max=0;};
Result Run(double packetMs,double skew,double rate,double outputRate,bool clocks,double offset,double jitter=0,bool enabled=true) {
 NitLink::DriftController c;Result r;
 const int packet=std::lround(rate*packetMs/1000),block=std::lround(outputRate*.003);
 const double interval=packet/(rate*(1+skew/1e6)),tick=block/outputRate,ratio=rate/outputRate;
 const int taps=64*int(std::ceil(std::max(1.,ratio))),history=taps/2-1,lookahead=taps-history;
 double fill=0,phase=history,last=0,next=interval+offset;
 bool primed=false;int n=0;
 for(int i=1;i*tick<=180;++i) {
  double now=i*tick;
  while(next<=now+1e-10){fill+=packet;++n;next=(n+1)*interval+offset+jitter*.5*(1+std::sin(n*2.31));}
  const double reserve=rate*.003,target=reserve+block*ratio,need=std::ceil(block*ratio*1.002)+lookahead+1;
  if(fill>history+target+rate*.020){const auto keep=history+reserve+need;if(fill>keep){fill=keep;++r.trims;c.Reprime();phase=history;last=now;}}
  if(!primed){if(fill<history+reserve+need)continue;primed=true;c.Reprime();phase=history;last=now;}
  const double ppm=enabled?c.Update((fill-history-target)/rate,now-last,clocks&&now>2.1?skew:0,(fill-phase-lookahead-block*ratio)/rate):0;last=now;r.ppm=ppm;
  const double actual=ratio*(1+ppm/1e6);
  int written=0;for(;written<block;++written){if(std::floor(phase)-history+taps>fill)break;phase+=actual;}
  const auto consumed=std::floor(phase)-history;fill-=consumed;phase-=consumed;
  if(written<block){if(now<10)++r.early;else ++r.late;primed=false;c.Reprime();phase=history;last=now;}
  if(now>10){r.min=std::min(r.min,(fill-history)/rate);r.max=std::max(r.max,(fill-history)/rate);}
 }
 return r;
}
int main() {
 try {
  int cases=0;
  for(double packet : {1.,3.,5.,10.})
   for(double skew : {-1000.,0.,1000.})
    for(bool clock : {false,true})
     for(double offset : {0.,.0013})
      for(double jitter : {0.,.0003}) {
       auto r=Run(packet,skew,48000,48000,clock,offset,jitter); ++cases;
       if(r.late || r.trims) {
        std::cerr << "packet=" << packet << " skew=" << skew << " clock=" << clock
                  << " offset=" << offset << " jitter=" << jitter
                  << " startup=" << r.early << " steady=" << r.late << " trims=" << r.trims << '\n';
        throw std::runtime_error("packetized fixed-target playback must settle without recurring underruns or trims");
       }
       if(r.max>.023 || std::abs(r.ppm)>2000) throw std::runtime_error("bounded queue and correction");
      }
  for(double source : {44100.,96000.})
   for(double output : {44100.,48000.})
    for(double skew : {-1000.,0.,1000.}) {
     auto r=Run(10,skew,source,output,true,.0013,.0003); ++cases;
     if(r.late || r.trims) throw std::runtime_error("packetized nominal rate conversion");
    }
  const auto off=Run(10,0,48000,48000,false,0,0,false);
  const auto on=Run(10,0,48000,48000,true,0);
  if(on.late!=off.late || on.early>off.early) throw std::runtime_error("enabling drift must not add underruns to a stable zero-skew stream");
  std::cout << "Packetized drift: " << cases << " cases, 180 seconds each, fixed 3 ms FIFO/output; "
            << "no underruns after 10 s settling and no trims. Baseline startup underruns: " << off.early << " / " << on.early << " (off/on).\n";
 } catch(const std::exception& e) {std::cerr << e.what() << '\n';return 1;}
}
