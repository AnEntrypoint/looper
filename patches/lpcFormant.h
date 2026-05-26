// lpcFormant.h — zero-latency independent FORMANT shifter via LPC pole
// detection + stable peaking-EQ envelope remap.
//
// Independent formant control: moves the spectral ENVELOPE (formants) without
// touching pitch. Used as a POST stage on the -12 octaver output so the great
// -12 pitch path is completely untouched — this stage only re-colours timbre.
//
// Why EQ-remap and not pole-rebuild or pre-resample:
//   * pre-resample shifts pitch+formants together → cancels against the pitch
//     stage (measured: centroid did not move, only doubling artifacts).
//   * rebuilding the all-pole synthesis filter from frequency-scaled poles is
//     unstable at order 12 on real signals (coefficient swap at a hop boundary
//     places a rebuilt pole ≥1 → the IIR rings to infinity).
//   * An EQ curve is UNCONDITIONALLY stable (no feedback poles near the unit
//     circle) and still moves the spectral envelope = a real formant shift.
//
// Pipeline:
//   1. LPC analysis (autocorrelation order P) on a trailing past-only window,
//      re-run every HOP samples → all-pole envelope.
//   2. Root-find A(z) to get the FORMANT POLE FREQUENCIES (read-only; the
//      instability only ever came from rebuilding a feedback filter, never
//      from reading the roots).
//   3. For each resonant pole, place a peaking biquad that CUTS at the original
//      formant freq f and BOOSTS at the shifted freq f·shift — together they
//      slide the spectral peak. Bank of biquads = the envelope-remap EQ.
//   4. Filter the signal through the biquad bank. Always stable; zero latency.
//
// shift>1 = formants up (bright), <1 = down (dark), ==1 = bit-clean bypass.

#ifndef LPC_FORMANT_H
#define LPC_FORMANT_H
#include <cmath>
#include <cstring>

class LpcFormant {
public:
    static const int P = 12;          // LPC order
    static const int WIN = 512;       // analysis window (~10.7ms @48k), past-only
    static const int HOP = 128;       // re-analyse every 128 samples (~2.7ms)
    static const int RBUF = 1024;     // trailing-sample ring (>= WIN)
    static const int NBANK = 4;       // up to 4 strongest formant peaks remapped

    void reset() {
        memset(m_ring, 0, sizeof(m_ring));
        memset(m_bq, 0, sizeof(m_bq));
        for (int b = 0; b < 2 * NBANK; b++) {   // start at identity, not silence
            m_bq[b].b0 = 1; m_bq[b].tb0 = 1;
        }
        m_wr = 0; m_sinceHop = 0;
        m_shift = 1.0f; m_targetShift = 1.0f;
        m_sr = 48000.0f;
        m_nActive = 0;
        for (int s = 0; s < NBANK; s++) { m_slotF[s] = 0.0f; m_slotActive[s] = false; }
    }
    LpcFormant() { reset(); }
    void setSampleRate(float sr) { m_sr = sr; }
    void setShift(float s) { m_targetShift = s; }

    inline float process(float x) {
        m_shift += (m_targetShift - m_shift) * (1.0f / 480.0f);
        m_ring[m_wr & (RBUF - 1)] = x;
        m_wr++;
        if (++m_sinceHop >= HOP) { m_sinceHop = 0; analyse(); }

        if (m_shift > 0.995f && m_shift < 1.005f) return x;  // bit-clean bypass

        // Always run the full bank; unused slots ramp to identity so a peak
        // appearing/disappearing across hops fades rather than clicks.
        float y = x;
        for (int b = 0; b < 2 * NBANK; b++) y = m_bq[b].process(y);
        if (y > 4.0f) y = 4.0f; else if (y < -4.0f) y = -4.0f;
        return y;
    }

private:
    struct Biquad {
        // current + target coeffs; per-sample ramp avoids the click that a
        // hard coefficient swap at each analysis hop produces.
        float b0, b1, b2, a1, a2;
        float tb0, tb1, tb2, ta1, ta2;
        float z1, z2;
        inline float process(float x) {
            // ramp coeffs toward target so hop swaps are smooth but still
            // track within ~1ms (fast enough that the formant actually reaches
            // its target between hops, slow enough to avoid the swap click).
            const float R = 1.0f / 48.0f;
            b0 += (tb0 - b0) * R; b1 += (tb1 - b1) * R; b2 += (tb2 - b2) * R;
            a1 += (ta1 - a1) * R; a2 += (ta2 - a2) * R;
            float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        // set TARGET peaking EQ at fc (Hz), gain dB, Q (ramped to, not snapped)
        void setPeak(float fc, float gainDb, float Q, float sr) {
            float A = powf(10.0f, gainDb / 40.0f);
            float w0 = 2.0f * 3.14159265f * fc / sr;
            float cw = cosf(w0), sw = sinf(w0);
            float alpha = sw / (2.0f * Q);
            float a0 =  1.0f + alpha / A;
            tb0 = (1.0f + alpha * A) / a0;
            tb1 = (-2.0f * cw) / a0;
            tb2 = (1.0f - alpha * A) / a0;
            ta1 = (-2.0f * cw) / a0;
            ta2 = (1.0f - alpha / A) / a0;
        }
        // snap to identity (used for slots that become inactive)
        void setIdentityTarget() { tb0 = 1; tb1 = 0; tb2 = 0; ta1 = 0; ta2 = 0; }
    };

    float m_ring[RBUF];
    Biquad m_bq[2 * NBANK];     // a cut + a boost per formant
    unsigned m_wr; int m_sinceHop, m_nActive;
    float m_shift, m_targetShift, m_sr;
    float m_slotF[NBANK];       // per-slot smoothed formant freq (Hz), 0=unseeded
    bool  m_slotActive[NBANK];

    void analyse() {
        float w[WIN];
        unsigned start = m_wr - WIN;
        for (int i = 0; i < WIN; i++) {
            float win = 0.5f - 0.5f * cosf(2.0f * 3.14159265f * i / (WIN - 1));
            w[i] = m_ring[(start + i) & (RBUF - 1)] * win;
        }
        float r[P + 1];
        for (int lag = 0; lag <= P; lag++) {
            float s = 0.0f;
            for (int i = lag; i < WIN; i++) s += w[i] * w[i - lag];
            r[lag] = s;
        }
        if (r[0] <= 1e-9f) { m_nActive = 0; return; }
        r[0] *= 1.0f + 1e-4f;

        float a[P + 1] = {0}; a[0] = 1.0f;
        float err = r[0];
        for (int m = 1; m <= P; m++) {
            float k = -r[m];
            for (int j = 1; j < m; j++) k -= a[j] * r[m - j];
            k /= err;
            if (k > 0.999f) k = 0.999f; else if (k < -0.999f) k = -0.999f;
            float an[P + 1];
            for (int j = 0; j <= m; j++) an[j] = a[j];
            for (int j = 1; j < m; j++) an[j] = a[j] + k * a[m - j];
            an[m] = k;
            for (int j = 0; j <= m; j++) a[j] = an[j];
            err *= (1.0f - k * k);
            if (err < 1e-9f) err = 1e-9f;
        }

        // read-only root find → formant pole freqs + radii (resonance sharpness)
        double pr[P], pi[P];
        if (!findRoots(a, pr, pi)) { m_nActive = 0; return; }

        // Confidence gate: on noisy real Pi input the per-hop pole estimate
        // jitters and jerks the EQ = clicks. err is the LPC residual energy; a
        // strongly-resonant (formant-rich) frame has err << r[0]. When the
        // frame is weakly predictable (err/r0 high = noise-like, or signal is
        // too quiet), DON'T re-estimate — hold the previous smoothed formants
        // so the EQ never jerks on an unreliable estimate.
        float predGain = err / (r[0] + 1e-9f);   // ~0 = strong formants, ~1 = noise
        bool confident = (predGain < 0.5f) && (r[0] > 1e-4f);

        if (confident) {
            // collect resonant poles, sort by FREQUENCY (stable identity across
            // hops, unlike radius-order), map to NBANK fixed slots.
            float cand[P]; int nc = 0;
            for (int k = 0; k < P; k++) {
                if (pi[k] < 0) continue;
                double rad = sqrt(pr[k]*pr[k] + pi[k]*pi[k]);
                double ang = atan2(pi[k], pr[k]);
                if (rad < 0.7 || ang <= 0.02 || ang >= 3.10) continue;
                float fc = (float)(ang * m_sr / (2.0 * 3.14159265));
                if (fc < 200.0f || fc > m_sr * 0.45f) continue;
                cand[nc++] = fc;
            }
            // insertion sort by freq
            for (int i = 1; i < nc; i++) { float v=cand[i]; int j=i-1; while(j>=0&&cand[j]>v){cand[j+1]=cand[j];j--;} cand[j+1]=v; }
            // 1-pole smooth each slot's target freq toward the matched candidate
            // (so a one-hop bad estimate barely nudges it = no jerk/click).
            const float FS = 0.25f;   // freq smoothing per hop
            for (int s = 0; s < NBANK; s++) {
                if (s < nc) {
                    if (m_slotF[s] <= 0.0f) m_slotF[s] = cand[s];     // seed
                    else m_slotF[s] += (cand[s] - m_slotF[s]) * FS;
                    m_slotActive[s] = true;
                } else {
                    m_slotActive[s] = false;   // fade this slot to identity
                }
            }
        }
        // (if not confident: keep previous m_slotF / m_slotActive → EQ frozen)

        // build the remap EQ from the SMOOTHED slots: CUT at f, BOOST at f*shift.
        float move = m_shift; float lg = fabsf(logf(move > 0.05f ? move : 0.05f));
        float gDb = lg * 20.0f; if (gDb > 12.0f) gDb = 12.0f;  // cap ±12 dB
        const float Q = 1.6f;   // fixed moderate Q (radius-derived Q jittered)
        int bi = 0;
        for (int s = 0; s < NBANK; s++) {
            if (m_slotActive[s] && m_slotF[s] > 0.0f) {
                float f0 = m_slotF[s];
                float f1 = f0 * move;
                if (f1 < 60.0f) f1 = 60.0f;
                if (f1 > m_sr * 0.45f) f1 = m_sr * 0.45f;
                m_bq[bi].setPeak(f0, -gDb, Q, m_sr); bi++;
                m_bq[bi].setPeak(f1, +gDb, Q, m_sr); bi++;
            } else {
                m_bq[bi].setIdentityTarget(); bi++;
                m_bq[bi].setIdentityTarget(); bi++;
            }
        }
        m_nActive = 2 * NBANK;
    }

    bool findRoots(const float a[P + 1], double pr[P], double pi[P]) {
        double c[P + 1];
        for (int i = 0; i <= P; i++) c[i] = a[i];
        double sr = 0.4, si = 0.9, zr = 1.0, zi = 0.0;
        for (int k = 0; k < P; k++) {
            double nr = zr*sr - zi*si, ni = zr*si + zi*sr; zr = nr; zi = ni;
            pr[k] = zr; pi[k] = zi;
        }
        for (int iter = 0; iter < 60; iter++) {
            double maxd = 0.0;
            for (int k = 0; k < P; k++) {
                double fr = c[0], fi = 0.0;
                for (int i = 1; i <= P; i++) {
                    double tr = fr*pr[k] - fi*pi[k] + c[i];
                    double ti = fr*pi[k] + fi*pr[k];
                    fr = tr; fi = ti;
                }
                double dr = 1.0, di = 0.0;
                for (int j = 0; j < P; j++) {
                    if (j == k) continue;
                    double er = pr[k]-pr[j], ei = pi[k]-pi[j];
                    double nr = dr*er - di*ei, ni = dr*ei + di*er;
                    dr = nr; di = ni;
                }
                double dd = dr*dr + di*di; if (dd < 1e-18) dd = 1e-18;
                double qr = (fr*dr + fi*di) / dd, qi = (fi*dr - fr*di) / dd;
                pr[k] -= qr; pi[k] -= qi;
                double mag = qr*qr + qi*qi; if (mag > maxd) maxd = mag;
            }
            if (maxd < 1e-14) break;
        }
        for (int k = 0; k < P; k++)
            if (!(pr[k] == pr[k]) || !(pi[k] == pi[k])) return false;
        return true;
    }
};
#endif
