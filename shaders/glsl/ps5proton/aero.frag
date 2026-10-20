#version 450
// MihawkOS - the guided setup's sky (examples/ps5proton/setup_tour.hpp): the desktop's Aero Meadow in blue, white and
// green, drawn lighter and alive. The sun's glow and rays, two drifting ribbons of light, the horizon haze, the far and
// near hills, and bubbles rising in three depths, the far ones softer and slower, iridescent at the rim.
// Laid out on the kit's 1920x1080 virtual canvas. Its GPU time is measured each run (the "setup sky" klog line).
// Copyright (C) 2026 Mihawk
// SPDX-License-Identifier: GPL-3.0-or-later

layout(push_constant) uniform Push {
	vec2 size;   // the framebuffer, in pixels
	float time;  // seconds
	float calm;  // 0..1: how far the bubbles have faded for a busy step (the install's progress)
} pc;

layout(location = 0) out vec4 color;

const int BUBBLES = 24;
// Each bubble's radius, where it starts its climb (0..1), its column and how fast it sways: fixed numbers, so a pixel
// spends nothing on them (the first build hashed them per pixel: 1.75 ms a frame at 3840x2160 on the console)
const float RADIUS[BUBBLES] = float[](
	23.470, 28.303, 84.004, 20.816, 43.127, 74.495, 25.747, 46.193, 84.892, 25.283, 42.493, 77.981, 14.880,
	46.587, 56.826, 23.625, 29.813, 54.692, 25.233, 30.380, 81.127, 13.496, 34.657, 86.035);
const float PHASE[BUBBLES] = float[](
	0.918, 0.822, 0.949, 0.750, 0.650, 0.206, 0.936, 0.121, 0.830, 0.173, 0.067, 0.945, 0.199, 0.728,
	0.107, 0.393, 0.851, 0.228, 0.533, 0.584, 0.490, 0.054, 0.057, 0.291);
const float COLUMN[BUBBLES] = float[](
	1224.592, 1528.146, 1087.258, 1943.171, 1812.424, 1617.511, 1875.477, 1469.339, 1265.044, 359.653,
	1849.015, 186.145, 1633.917, 293.563, 77.316, 1868.245, 346.148, 1425.475, 157.957, 742.714, 1675.782,
	404.485, 1026.843, 1130.444);
const float SWAY[BUBBLES] = float[](
	0.362, 0.446, 0.434, 0.414, 0.440, 0.400, 0.380, 0.291, 0.437, 0.274, 0.416, 0.285, 0.263, 0.439,
	0.290, 0.396, 0.271, 0.329, 0.420, 0.296, 0.357, 0.367, 0.348, 0.261);

float bell(float v)
{
	return exp(-v * v);
}

void main()
{
	vec2 p = gl_FragCoord.xy * (1080.0 / pc.size.y); // virtual pixels, y down
	float y = p.y / 1080.0;

	// Aero Meadow, the Mihawk OS desktop (tools/make-mihawk-wallpaper.py), drawn lighter for a page with text on it: its
	// sky, sun, rays, ribbons and haze, and its far and near hills with their shapes and greens. Dark ink reads on the
	// sky (7:1 at the top) and on the hint row's grass (7:1).
	float x = p.x / 1920.0;
	vec3 sky = mix(vec3(0.353, 0.663, 0.918), vec3(0.58, 0.82, 0.985), smoothstep(0.0, 0.55, y));
	sky = mix(sky, vec3(0.90, 0.975, 1.0), smoothstep(0.50, 0.80, y));
	vec2 sun = vec2((x - 0.80) * 1920.0 / 1080.0, y - 0.08);
	float distance = length(sun);
	sky = mix(sky, vec3(0.97, 1.0, 1.0), 0.55 * bell(distance / 0.42));
	sky = mix(sky, vec3(1.0), 0.45 * bell(distance / 0.10));
	float angle = atan(sun.y, sun.x) + 0.02 * sin(pc.time * 0.11);
	float rays = bell((angle - 2.05) / 0.035) + 0.7 * bell((angle - 2.30) / 0.05) +
		0.8 * bell((angle - 2.60) / 0.03) + 0.5 * bell((angle - 2.85) / 0.06) +
		0.6 * bell((angle - 1.70) / 0.04) + 0.45 * bell((angle - 3.10) / 0.045);
	sky = mix(sky, vec3(1.0), clamp(0.18 * rays * exp(-distance * distance) * smoothstep(0.03, 0.20, distance), 0.0, 1.0));
	// The two ribbons of light across the sky, drifting slowly
	for (int i = 0; i < 2; i++) {
		float base = i == 0 ? 0.30 : 0.36, amplitude = i == 0 ? 0.07 : 0.05, frequency = i == 0 ? 0.8 : 0.95;
		float thickness = i == 0 ? 0.004 : 0.0025, strength = i == 0 ? 0.45 : 0.32;
		float curve = base + amplitude * sin(6.2831853 * frequency * x + (i == 0 ? 0.4 : 1.3) + pc.time * 0.06) + 0.12 * (x - 0.5);
		float gap = abs(y - curve), fade = smoothstep(0.0, 0.25, x) * smoothstep(1.0, 0.70, x);
		sky = mix(sky, vec3(1.0), strength * fade * bell(gap / thickness));
		sky = mix(sky, vec3(0.85, 0.97, 1.0), 0.18 * strength * fade * bell(gap / (thickness * 8.0)));
	}
	sky = mix(sky, vec3(1.0), 0.6 * bell((y - 0.765) / 0.055));   // the haze where sky meets land
	// The far hill, pale and hazy, and the near one of glossy grass lit from the upper right
	float far = 0.785 + 0.020 * sin(0.55 * 6.2831853 * x + 0.3) + 0.010 * sin(1.7 * 6.2831853 * x + 2.1) +
		0.004 * sin(4.3 * 6.2831853 * x + 0.8);
	vec3 farGrass = mix(vec3(0.70, 0.90, 0.62), vec3(0.55, 0.82, 0.46), smoothstep(0.0, 0.12, y - far));
	sky = mix(sky, farGrass, 0.95 * clamp((y - far) * 1080.0 / 1.5 + 0.5, 0.0, 1.0));
	float near = 0.865 + 0.045 * sin(0.42 * 6.2831853 * x + 4.0) + 0.018 * sin(1.1 * 6.2831853 * x + 1.0) +
		0.006 * sin(3.1 * 6.2831853 * x + 2.6);
	float depth = y - near;
	vec3 grass = mix(vec3(0.64, 0.92, 0.27), vec3(0.36, 0.70, 0.17), smoothstep(0.0, 0.16, depth));
	grass = mix(grass, vec3(0.80, 1.0, 0.42), 0.35 * smoothstep(0.3, 1.0, x) * bell(depth / 0.05));
	grass = mix(grass, vec3(1.0, 1.0, 0.92), 0.45 * bell(depth / 0.008));   // the shine along the crest
	sky = mix(sky, grass, clamp(depth * 1080.0 / 1.5 + 0.5, 0.0, 1.0));

	vec3 result = sky;
	for (int i = 0; i < BUBBLES; i++) {
		float n = float(i);
		int depth = i % 3; // 0 far, 2 near
		float radius = RADIUS[i];
		float speed = depth == 0 ? 14.0 : depth == 1 ? 22.0 : 32.0;
		float travel = 1080.0 + radius * 2.0;
		float cy = 1080.0 + radius - fract(pc.time * speed / travel + PHASE[i]) * travel;
		float reach = radius * 1.2;
		if (abs(p.y - cy) > reach)   // most bubbles are not in this pixel's row: nothing more to do
			continue;
		vec2 centre = vec2(COLUMN[i] + sin(pc.time * SWAY[i] + n) * (10.0 + 8.0 * float(depth)), cy);
		vec2 q = (p - centre) / radius;
		float d = length(q);
		if (d > 1.2)
			continue;
		float soft = depth == 0 ? 0.10 : depth == 1 ? 0.05 : 0.025; // depth of field
		float body = 1.0 - smoothstep(1.0 - soft, 1.0 + soft, d);
		// Glass is clear in the middle and bright at the rim, where the light grazes it.
		float fresnel = pow(clamp(d, 0.0, 1.0), 5.0) * body;
		float angle = atan(q.y, q.x) / 6.2831853 + pc.time * 0.03;
		vec3 film = 0.5 + 0.5 * cos(6.2831853 * (angle + vec3(0.0, 0.33, 0.67)));
		// The rim's thin film, pink to cyan as on the desktop's bubbles, with the grass below in it
		vec3 rim = mix(vec3(1.0, 0.72, 0.95), vec3(0.62, 0.96, 1.0), 0.5 + 0.5 * sin(atan(q.y, q.x) * 1.3 + n + pc.time * 0.2));
		rim = mix(rim, vec3(0.80, 1.0, 0.70), 0.35 * smoothstep(0.2, 0.9, q.y));
		rim = mix(rim, film, 0.15);
		// The window it reflects, top left, and a fainter one across from it.
		vec2 s = (q - vec2(-0.36, -0.42)) * vec2(1.0, 1.7);
		float spot = (1.0 - smoothstep(0.0, 0.34 + soft, length(s))) * body;
		float bounce = (1.0 - smoothstep(0.0, 0.22 + soft, length((q - vec2(0.42, 0.5)) * vec2(1.6, 1.0)))) * body * 0.35;
		float weight = (depth == 0 ? 0.45 : depth == 1 ? 0.7 : 0.9) * (1.0 - pc.calm * 0.6);
		result = mix(result, rim, fresnel * 0.55 * weight);
		result = mix(result, vec3(0.86, 0.95, 1.0), body * 0.05 * weight);
		result += vec3(1.0) * (spot * 0.75 + bounce) * weight;
	}
	color = vec4(min(result, vec3(1.0)), 1.0);
}
