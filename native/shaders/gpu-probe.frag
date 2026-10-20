// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
layout(push_constant) uniform Colour { vec4 rgba; } colour;
layout(location=0) out vec4 pixel;
void main() { pixel = colour.rgba; }
