// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "OceanFft.h"
#include <algorithm>
#include <cmath>
#include <random>

namespace {
	const double PI_ = 3.14159265358979323846;
	const double SIGMA_RHO = 7.28e-5; // surface tension / density of water [m^3/s^2] (capillary waves)

	// in-place radix-2 FFT of n points with stride s, sign +1 (inverse, unnormalised: a sum)
	void Fft1 (std::complex<float> *a, int n, int s)
	{
		for (int i = 1, j = 0; i < n; i++) {
			int bit = n >> 1;
			for (; j & bit; bit >>= 1) j ^= bit;
			j ^= bit;
			if (i < j) std::swap (a[i * s], a[j * s]);
		}
		for (int len = 2; len <= n; len <<= 1) {
			double ang = 2.0 * PI_ / len;
			std::complex<float> wl ((float)cos (ang), (float)sin (ang));
			for (int i = 0; i < n; i += len) {
				std::complex<float> w (1.0f, 0.0f);
				for (int k = 0; k < len / 2; k++) {
					std::complex<float> u = a[(i + k) * s], v = a[(i + k + len / 2) * s] * w;
					a[(i + k) * s] = u + v;
					a[(i + k + len / 2) * s] = u - v;
					w *= wl;
				}
			}
		}
	}
}

void OceanFft::Ifft2 (std::complex<float> *a)
{
	for (int y = 0; y < N; y++) Fft1 (a + (size_t)y * N, N, 1);
	for (int x = 0; x < N; x++) Fft1 (a + x, N, N);
}

bool OceanFft::Setup (double U, double azimuth, double g, double l0, double targetVar, uint32_t seed)
{
	valid = false;
	if (U < 0.3 || targetVar <= 0.0 || g <= 0.0) return false;
	L[0] = std::clamp (2.0 * l0, 16.0, 800.0);
	L[1] = L[0] / RIPPLE_RATIO;
	const double kc = 2.0 * PI_ / (L[0] / 32.0);   // the split: the long patch's 32-texel wavelength
	const double Lw = U * U / g;                    // the largest wave the wind makes
	const double wx = cos (azimuth), wy = sin (azimuth);
	std::mt19937 rng (seed);
	std::normal_distribution<double> gauss (0.0, 1.0);
	double sum = 0.0;
	for (int l = 0; l < LAYERS; l++) {
		h0[l].assign ((size_t)N * N, 0.0f);
		omega[l].assign ((size_t)N * N, 0.0);
		kx[l].assign ((size_t)N * N, 0.0f), ky[l].assign ((size_t)N * N, 0.0f);
		const double dk = 2.0 * PI_ / L[l];
		for (int y = 0; y < N; y++)
			for (int x = 0; x < N; x++) {
				double r1 = gauss (rng), r2 = gauss (rng); // (drawn for every mode: the same sea for any band split)
				int mx = x < N / 2 ? x : x - N, my = y < N / 2 ? y : y - N;
				if (mx == -N / 2 || my == -N / 2 || (mx == 0 && my == 0)) continue;
				double kx = mx * dk, ky = my * dk, k = sqrt (kx * kx + ky * ky);
				if (l == 0 ? k >= kc : k < kc) continue;
				double c = (kx * wx + ky * wy) / k;
				// Phillips: exp (-1 / (k Lw)^2) / k^4 |k^ . w^|^2, the waves against the wind
				// weak, the shortest (capillary) damped
				double P = exp (-1.0 / (k * k * Lw * Lw)) / (k * k * k * k) * c * c * (c < 0.0 ? 0.07 : 1.0)
					* exp (-k * k * 1e-4);
				h0[l][(size_t)y * N + x] = std::complex<float> ((float)(r1 * sqrt (0.5 * P)), (float)(r2 * sqrt (0.5 * P)));
				omega[l][(size_t)y * N + x] = sqrt (g * k + SIGMA_RHO * k * k * k);
				this->kx[l][(size_t)y * N + x] = (float)kx, this->ky[l][(size_t)y * N + x] = (float)ky;
			}
		// mean-square slope: 2 sum k^2 |h0|^2 (the sum transform, no 1/N^2)
		double v = 0.0;
		for (int y = 0; y < N; y++)
			for (int x = 0; x < N; x++) {
				int mx = x < N / 2 ? x : x - N, my = y < N / 2 ? y : y - N;
				double kx = mx * dk, ky = my * dk;
				v += 2.0 * (kx * kx + ky * ky) * std::norm (h0[l][(size_t)y * N + x]);
			}
		var[l] = v;
		sum += v;
	}
	if (!(sum > 0.0)) return false;
	double s = sqrt (targetVar / sum);
	for (int l = 0; l < LAYERS; l++) {
		for (auto &h : h0[l]) h *= (float)s;
		var[l] *= s * s;
	}
	work.assign ((size_t)N * N, 0.0f);
	valid = true;
	return true;
}

void OceanFft::Evaluate (double t, float *out)
{
	if (!valid) return;
	for (int l = 0; l < LAYERS; l++) {
		// the slopes' spectrum: i k h(k, t) per axis, as sx + i sz (both fields real)
		const std::complex<float> *h0l = h0[l].data ();
		for (int y = 0; y < N; y++)
			for (int x = 0; x < N; x++) {
				size_t i = (size_t)y * N + x;
				if (h0l[i] == std::complex<float> (0.0f) && omega[l][i] == 0.0) { work[i] = 0.0f; continue; }
				// h(k, t) = h0(k) e^(i w t) + conj (h0(-k)) e^(-i w t)
				int xn = (N - x) & (N - 1), yn = (N - y) & (N - 1);
				size_t in = (size_t)yn * N + xn;
				float ph = (float)fmod (omega[l][i] * t, 2.0 * PI_);
				std::complex<float> e (cosf (ph), sinf (ph));
				std::complex<float> h = h0l[i] * e + std::conj (h0l[in]) * std::conj (e);
				// i kx h + i (i ky h) = (-kx Im h - ky Re h) + i (kx Re h - ky Im h)
				float kxv = kx[l][i], kyv = ky[l][i];
				work[i] = std::complex<float> (-kxv * h.imag () - kyv * h.real (), kxv * h.real () - kyv * h.imag ());
			}
		Ifft2 (work.data ());
		float *o = out + (size_t)l * N * N * 2;
		double v = 0.0;
		for (size_t i = 0; i < (size_t)N * N; i++) {
			o[2 * i] = (float)work[i].real ();
			o[2 * i + 1] = (float)work[i].imag ();
			v += (double)std::norm (work[i]);
		}
		if (l == 0) mipVar[0] = v / ((double)N * N);
	}
	// the long layer's levels: 2 x 2 box filtered (periodic: no border)
	const float *src = out;
	float *dst = out + (size_t)LAYERS * N * N * 2;
	for (int m = 1; m <= MIPS; m++) {
		int n = N >> m, ns = N >> (m - 1);
		double v = 0.0;
		for (int y = 0; y < n; y++)
			for (int x = 0; x < n; x++) {
				for (int c = 0; c < 2; c++) {
					float a = 0.25f * (src[2 * ((2 * y) * ns + 2 * x) + c] + src[2 * ((2 * y) * ns + 2 * x + 1) + c]
						+ src[2 * ((2 * y + 1) * ns + 2 * x) + c] + src[2 * ((2 * y + 1) * ns + 2 * x + 1) + c]);
					dst[2 * (y * n + x) + c] = a;
					v += (double)a * a;
				}
			}
		mipVar[m] = v / ((double)n * n);
		src = dst;
		dst += (size_t)n * n * 2;
	}
}
