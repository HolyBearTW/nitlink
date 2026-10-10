#include "audio/clock_recovery.h"
#include "audio/drift_controller.h"
#include <iostream>
#include <stdexcept>
using namespace NitLink;
void Check(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
 try {
    // Independent nominal rates and unrelated device-position units. Timestamp
    // noise and variable delivery delays must not turn into estimated drift.
    for (double captureRate : {44100.0,48000.0,96000.0}) {
      for (double skew : {-1200.0,-300.0,0.0,500.0,1200.0}) {
        AudioClockRate capture,render;
        for (int i=0;i<=4000;++i) {
          const double t=i*0.01;
          const auto qpc=uint64_t((100+t)*1e7);
          const auto cqpc=qpc+int64_t(80*std::sin(i*0.7));
          capture.Sample(uint64_t(t*captureRate*(1+skew/1e6)),cqpc,captureRate,t+0.001*(i%7));
          render.Sample(uint64_t(t*10000000),qpc,10000000,t+0.001*(i%7));
        }
        Check(capture.Valid(40.04) && render.Valid(40.04),"both clock regressions converge");
        Check(std::abs((capture.Rate()/render.Rate()-1)*1e6-skew)<20,"relative clock rate accuracy");
        Check(!capture.Valid(41),"stalled clock estimate expires");
        capture.Sample(0,2000000000,captureRate,41);
        Check(!capture.Valid(41),"position reset discards old clock fit");
      }
    }
    AudioClockRate invalid;
    for (int i=0;i<300;++i) invalid.Sample(i*480,uint64_t(1000000000+i*100000),0,i*.01);
    Check(!invalid.Valid(3),"invalid clock frequency cannot enable correction");
    AudioClockRate jump;
    for (int i=0;i<300;++i) jump.Sample(i*480,uint64_t(1000000000+i*100000),48000,i*.01);
    Check(jump.Valid(2.99),"valid baseline before timestamp fault");
    jump.Sample(300*480,1000000000,48000,3);
    Check(!jump.Valid(3),"backwards QPC invalidates estimate");

    // Closed-loop occupancy: no reserve adaptation. Re-prime periodically,
    // retaining the clock feed-forward and protective negative feedback.
    for (double skew : {-1000.0,1000.0}) {
      DriftController controller;
      double error=0,previous=0,ppm=0;
      for (int i=0;i<60000;++i) {
        const double dt=.003;
        ppm=controller.Update(error,dt,i>700 ? skew : 0);
        error+=dt*(skew-ppm)/1e6;
        Check(std::abs(ppm)<=2000,"total correction remains bounded");
        Check(std::abs(ppm-previous)<=400*dt+1e-7,"feed-forward obeys slew limit");
        previous=ppm;
        if (i>10000 && i%1000==0) { controller.Reprime(); previous=controller.Update(0,0); }
      }
      Check(std::abs(ppm-skew)<20 && std::abs(error)<.0002,"retained clock estimate settles to fixed FIFO target");
    }
    AudioRecoveryGuard guard;
    Check(!guard.Observe(0,0,.001,.003),"isolated underrun does not reload");
    Check(!guard.Observe(.5,0,.001,.003),"second small underrun does not reload");
    Check(guard.Observe(1,0,.001,.003),"third underrun reloads");
    guard.ClearWindow(); // exactly what an endpoint rebuild does
    for (int i=0;i<99;++i) Check(!guard.Observe(1.1+i*.1,.2,.03,.003),"cooldown survives reload");
    Check(guard.Observe(11.01,0,.020,.003),"large missing interval can reload after cooldown");
    AudioRecoveryGuard severe;
    Check(!severe.Observe(0,.03,0,.003),"one queue spike does not reload");
    Check(!severe.Observe(.1,0,0,.003),"normal block cancels persistence");
    Check(!severe.Observe(.2,.03,0,.003),"new severe interval starts");
    for (int i=1;i<=25;++i) Check(!severe.Observe(.2+i*.009,.03,0,.003),"persistent error waits for threshold");
    Check(severe.Observe(.451,.03,0,.003),"sustained severe queue error reloads");
    AudioRecoveryGuard ordinary;
    for (int i=0;i<10000;++i) Check(!ordinary.Observe(i*.003,.006*std::sin(i),0,.003),"ordinary packet jitter never reloads");
    std::cout << "Clock regression, fixed-target feedback, timestamp faults and reload cooldown passed\n";
 } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
