// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The wind's sea near the camera as a field of many waves (pass 2, W3):
// a directional wave spectrum (Phillips, Tessendorf 2001) for the sea state
// of Atmosphere::SeaState, its slopes evaluated each frame by an inverse FFT
// on two periodic patches, a long-wave patch (about twice the longest wave)
// and a ripple patch 9.37 times smaller (the spectrum split between them at
// the long patch's 32-texel wavelength, so no wave is drawn twice). Hundreds
// of components with random directions and phases: short, irregular crests
// from any angle (the 14 plane waves read as a crosshatch from above).
//
// Out: per layer, N x N slopes (dh/du, dh/dv) along the anchor's t1 and t2
// axes, at the patch's texel (i + 0.5) L / N; periodic. The total mean-square
// slope of both layers is scaled to the sea state's drawn share.
// =======================================================================

#ifndef __OCEANFFT_H
#define __OCEANFFT_H

#include <complex>
#include <cstdint>
#include <vector>

class OceanFft {
public:
	static constexpr int N = 128;            ///< texels per patch side (power of two)
	static constexpr int LAYERS = 2;
	static constexpr double RIPPLE_RATIO = 9.37; ///< long patch / ripple patch

	/// a sea: wind U [m/s] towards azimuth [rad] from t1, gravity g, longest wave l0 [m];
	/// the drawn mean-square slope (both axes) targetVar. False: no sea to draw (calm)
	bool Setup (double U, double azimuth, double g, double l0, double targetVar, uint32_t seed);
	static constexpr int MIPS = 5;           ///< the long layer's coarser levels (N/2 .. N/32), box filtered
	/// floats in the output: the layers (N x N x 2 each), then the long layer's levels 1..MIPS
	static constexpr size_t Floats () { size_t n = (size_t)LAYERS * N * N; for (int l = 1; l <= MIPS; l++) n += (size_t)(N >> l) * (N >> l); return 2 * n; }
	/// the slopes at time t [s] into out (Floats ()); mipVar: the mean-square slope of the long layer's
	/// levels 0..MIPS, as drawn (the filtered share goes into the glitter's roughness)
	void Evaluate (double t, float *out);
	double mipVar[MIPS + 1] = {};

	bool Valid () const { return valid; }
	double L[LAYERS] = {};     ///< patch sizes [m]
	double var[LAYERS] = {};   ///< mean-square slope (both axes) of each layer

private:
	bool valid = false;
	std::vector<std::complex<float>> h0[LAYERS];   ///< amplitudes per mode (0 outside the layer's band)
	std::vector<float> kx[LAYERS], ky[LAYERS];
	std::vector<double> omega[LAYERS];
	std::vector<std::complex<float>> work;
	void Ifft2 (std::complex<float> *a);
};

#endif // !__OCEANFFT_H
