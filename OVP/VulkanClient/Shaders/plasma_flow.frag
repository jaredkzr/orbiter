// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Reentry plasma (Plasma.cpp): the flow map - blended by MIN: the windward
// surface's s (r) and minus the leeward's (g)

#version 450

layout(location = 0) in float vS;
layout(location = 0) out vec2 outS;

void main ()
{
	outS = vec2 (vS, -vS);
}
