#include "audio/drift_controller.h"
#include <iostream>
#include <stdexcept>
#include <limits>
using namespace NitLink;
void Check(bool ok, const char* msg) { if (!ok) throw std::runtime_error(msg); }
int main() {
    try {
        for (double rate : {8000.0, 44100.0, 48000.0, 96000.0, 192000.0}) {
            for (double skew : {-1000.0, -300.0, 0.0, 300.0, 1000.0}) {
                for (double tick : {0.003, 0.010}) {
                    DriftController c;
                    double error = 0, ppm = 0, previous = 0;
                    double time = 0;
                    for (int i = 0; time < 180; ++i) {
                        // Alternating wake intervals and bounded packet phase jitter.
                        const double dt = tick * (i % 2 ? 0.7 : 1.3);
                        time += dt;
                        const double packetJitter = 0.002 * std::sin(time * 2 * 3.141592653589793 * 37);
                        ppm = c.Update(error + packetJitter, dt);
                        error += dt * (skew - ppm) / 1000000;
                        Check(std::abs(ppm) <= 2000, "correction clamp");
                        Check(std::abs(ppm-previous) <= 400*dt + 1e-7, "slew bound");
                        if (std::abs(error) >= 0.009) { std::cerr << "skew=" << skew << " time=" << time << " error=" << error << " ppm=" << ppm << "\n"; throw std::runtime_error("bounded queue error"); }
                        previous = ppm;
                    }
                    Check(std::abs(error) < 0.0023, "queue settles within the measured packet-jitter band");
                    Check(std::abs(ppm-skew) < 50, "converged clock correction");
                }
            }
        }
        DriftController c;
        for (int i=0; i<10000; ++i) c.Update(1, 0.01);
        Check(c.Update(1, 0.01) == 2000, "positive saturation");
        c.Reset();
        Check(c.Update(0, 0.01) == 0, "reset clears integrator");
        Check(c.Update(std::numeric_limits<double>::quiet_NaN(), 1) == 0, "NaN ignored");
        Check(c.Update(0, -1) == 0, "invalid dt ignored");
        for (double t : {0.0, .1, .5, .9, 1.0}) {
            Check(std::abs(DriftCubic(2,2,2,2,t)-2) < 1e-12, "DC preserved");
            Check(std::abs(DriftCubic(0,1,2,3,t)-(1+t)) < 1e-12, "linear ramp preserved");
        }
        std::cout << "Audio drift simulation: 50 rate/skew/cadence cases, 180 seconds each, passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
