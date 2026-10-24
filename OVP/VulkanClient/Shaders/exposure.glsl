// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Auto-exposure: luminance histogram of the scene and adapted exposure

const int HIST_BINS = 256;
const float LOG_LUM_MIN = -12.0; // log2 luminance of bin 1 (bin 0: darker)
const float LOG_LUM_RANGE = 20.0;

struct ExposureState {
	float logExposure; // adapted exposure (log2)
	float avgLogLum;   // metered scene luminance (log2), for diagnostics
	float pad0, pad1;
};
