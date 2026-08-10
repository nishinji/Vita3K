// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

// the FSR library (ffx_a.h + ffx_fsr1.h), the sampler and the load callbacks it
// expects are pasted in front of this file by ScreenRenderer::init_fsr

in vec2 uv_frag;

// the size of the upscaled texture, which covers exactly this pass' viewport
uniform vec2 output_size;
uniform float sharpening;

out vec4 color_frag;

void main() {
	AU4 con;
	FsrRcasCon(con, sharpening);

	// the upscaled texture has the same orientation as this viewport, and the sharpening
	// kernel is symmetric, so no mirroring is needed here
	AU2 pos = AU2(uv_frag * output_size);

	AF3 color;
	FsrRcasF(color.r, color.g, color.b, pos, con);
	color_frag = vec4(color, 1.0);
}
