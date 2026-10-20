#version 450
// MihawkOS - the guided setup's sky: one triangle over the whole screen (examples/ps5proton/kit/setup_aero.cpp).
// Copyright (C) 2026 Mihawk
// SPDX-License-Identifier: GPL-3.0-or-later

void main()
{
	vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
