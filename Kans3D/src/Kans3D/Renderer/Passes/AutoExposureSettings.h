#pragma once
#include <algorithm>
#include <cmath>
namespace Kans
{
    struct AutoExposureSettings
    {
        float        HistogramMinEV = -12, HistogramMaxEV = 20, LowPercentile = .5f, HighPercentile = .9f;
        float        CentreWeight = 0, MinEV = -10, MaxEV = 20, SpeedUp = 3, SpeedDown = 1;
        static float Adapt(float current, float target, float delta, float up, float down)
        {
            const float speed = target > current ? up : down;
            return current + (target - current) * (-std::expm1(-std::max(0.f, speed) * std::max(0.f, delta)));
        }
        AutoExposureSettings Validated() const
        {
            auto r = *this;
            auto f = [](float v, float lo, float hi, float fallback) {
                return std::clamp(std::isfinite(v) ? v : fallback, lo, hi);
            };
            r.HistogramMinEV = f(HistogramMinEV, -32, 31, -12);
            r.HistogramMaxEV = f(HistogramMaxEV, r.HistogramMinEV + .01f, 64, 20);
            r.LowPercentile  = f(LowPercentile, 0, .99f, .5f);
            r.HighPercentile = f(HighPercentile, r.LowPercentile + .001f, 1, .9f);
            r.CentreWeight   = f(CentreWeight, 0, 1, 0);
            r.MinEV          = f(MinEV, -24, 32, -10);
            r.MaxEV          = f(MaxEV, r.MinEV, 32, 20);
            r.SpeedUp        = f(SpeedUp, 0, 100, 3);
            r.SpeedDown      = f(SpeedDown, 0, 100, 1);
            return r;
        }
        bool operator==(const AutoExposureSettings& o) const
        {
            return HistogramMinEV == o.HistogramMinEV && HistogramMaxEV == o.HistogramMaxEV &&
                   LowPercentile == o.LowPercentile && HighPercentile == o.HighPercentile &&
                   CentreWeight == o.CentreWeight && MinEV == o.MinEV && MaxEV == o.MaxEV && SpeedUp == o.SpeedUp &&
                   SpeedDown == o.SpeedDown;
        }
    };
} // namespace Kans
