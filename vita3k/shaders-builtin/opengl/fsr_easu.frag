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

// the FSR library (ffx_a.h + ffx_fsr1.h), the sampler and the gather callbacks it
// expects are pasted in front of this file by ScreenRenderer::init_fsr

in vec2 uv_frag;

// the region of the source texture to upscale, in source pixels
uniform vec2 src_offset;
uniform vec2 src_size;
// the size of the whole source texture, the region is only a part of it
uniform vec2 texture_size;
// the size of this render target
uniform vec2 output_size;

out vec4 color_frag;

void main() {
	AU4 con0, con1, con2, con3;
	FsrEasuConOffset(con0, con1, con2, con3,
		src_size.x, src_size.y,
		texture_size.x, texture_size.y,
		output_size.x, output_size.y,
		src_offset.x, src_offset.y);

	// uv_frag is 0 at the bottom of the target like any GL framebuffer while FSR
	// addresses the image top-down, so the row is mirrored
	AU2 pos = AU2(uv_frag.x * output_size.x, (1.0 - uv_frag.y) * output_size.y);

	AF3 color;
	FsrEasuF(color, pos, con0, con1, con2, con3);
	color_frag = vec4(color, 1.0);
}
