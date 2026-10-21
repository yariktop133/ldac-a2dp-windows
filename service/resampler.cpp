// Streaming windowed-sinc resampler (Blackman window, 32 taps), any ratio.
// Good enough to bring a 44.1/48 kHz mix up to the negotiated LDAC rate.
#include "svc.h"

#include <cmath>

static const double kPi = 3.14159265358979323846;

Resampler::Resampler(double inRate, double outRate, int channels)
    : step_(inRate / outRate), cutoff_((outRate < inRate ? outRate / inRate : 1.0) * 0.95), pos_(kHalfTaps),
      ch_(channels), passthrough_(inRate == outRate), hist_((size_t)kHalfTaps * channels, 0.0f),
      kernel_(2 * kHalfTaps)
{
}

void Resampler::Process(const float *in, size_t frames, std::vector<float> &out)
{
    if (passthrough_) {
        out.insert(out.end(), in, in + frames * ch_);
        return;
    }
    hist_.insert(hist_.end(), in, in + frames * ch_);
    size_t have = hist_.size() / ch_;

    // An output sample at input position `pos_` needs kHalfTaps frames on each side.
    while (pos_ + kHalfTaps < (double)have) {
        long center = (long)std::floor(pos_);
        double frac = pos_ - center;
        double norm = 0;
        for (int k = 0; k < 2 * kHalfTaps; k++) {
            double t = (k - kHalfTaps + 1) - frac;  // distance from the output position
            double x = cutoff_ * t;
            double sinc = std::fabs(x) < 1e-9 ? 1.0 : std::sin(kPi * x) / (kPi * x);
            double w = 0.42 + 0.5 * std::cos(kPi * t / kHalfTaps) + 0.08 * std::cos(2 * kPi * t / kHalfTaps);
            kernel_[k] = std::fabs(t) >= kHalfTaps ? 0.0 : cutoff_ * sinc * w;
            norm += kernel_[k];
        }
        for (int c = 0; c < ch_; c++) {
            double acc = 0;
            const float *x = &hist_[(size_t)(center - kHalfTaps + 1) * ch_ + c];
            for (int k = 0; k < 2 * kHalfTaps; k++)
                acc += kernel_[k] * x[(size_t)k * ch_];
            out.push_back((float)(acc / norm));  // unity gain at DC
        }
        pos_ += step_;
    }

    // Drop history no future output can reach.
    long keepFrom = (long)std::floor(pos_) - kHalfTaps;
    if (keepFrom > 0) {
        hist_.erase(hist_.begin(), hist_.begin() + (size_t)keepFrom * ch_);
        pos_ -= keepFrom;
    }
}
