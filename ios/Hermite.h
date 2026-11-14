#pragma once

// Pure C++ (no Objective-C, no JUCE): 4-point, third-order Hermite (Catmull-Rom) interpolation,
// shared by the backing track's resampler (BackingTrack.mm), the sampler's loader (SamplerBank.mm)
// and the sampler's voices (SamplerEngine.h).

// The value between x0 and x1 at fraction t (0..1), from the samples around it. t = 0 gives x0
// exactly.
inline float hermite4(float xm1, float x0, float x1, float x2, float t) {
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}
