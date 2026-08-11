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

#include <ngs/modules/envelope.h>
#include <util/log.h>

#include <algorithm>

namespace ngs {

bool EnvelopeModule::process(KernelState &kern, const MemState &mem, const SceUID thread_id, ModuleData &data, std::unique_lock<std::recursive_mutex> &scheduler_lock, std::unique_lock<std::mutex> &voice_lock) {
    if (data.is_bypassed)
        return false;

    const SceNgsEnvelopeParams *params = data.get_parameters<SceNgsEnvelopeParams>(mem);
    if (!params || params->desc.id != SCE_NGS_ENVELOPE_PARAMS_STRUCT_ID)
        return false;

    const int32_t total_points = std::min<int32_t>(params->uNumPoints, SCE_NGS_ENVELOPE_MAX_POINTS);
    if (total_points <= 0)
        return false;

    // The envelope sits right behind the module that produced the audio, so it shapes whatever
    // that left in the voice product. Nothing to scale if it did not run this frame.
    float *samples = reinterpret_cast<float *>(data.parent->products[0].data);
    if (!samples)
        return false;

    const int32_t granularity = data.parent->rack->system->granularity;
    const int32_t sample_rate = data.parent->rack->system->sample_rate;
    if (granularity <= 0 || sample_rate <= 0)
        return false;

    SceNgsEnvelopeStates *state = data.get_state<SceNgsEnvelopeStates>();
    const float msecs_per_sample = 1000.0f / static_cast<float>(sample_rate);

    for (int32_t i = 0; i < granularity; i++) {
        if (!state->nReleasing && data.parent->is_keyed_off) {
            // Release always starts from wherever the envelope had got to, so a key off part way
            // up the attack does not jump to full volume before fading.
            state->nReleasing = 1;
            state->fReleaseScale = state->fCurrentHeight;
            state->fPosition = 0.0f;
        }

        if (state->nReleasing) {
            if (params->uReleaseMsecs == 0) {
                state->fCurrentHeight = 0.0f;
            } else {
                const float progress = std::min(state->fPosition / static_cast<float>(params->uReleaseMsecs), 1.0f);
                state->fCurrentHeight = state->fReleaseScale * (1.0f - progress);
            }
        } else if (state->nCurrentPoint >= total_points - 1) {
            // Past the last point the envelope holds the amplitude that point asked for.
            state->fCurrentHeight = params->envelopePoints[total_points - 1].fAmplitude;
        } else {
            // A point carries the amplitude the envelope has when it reaches it, and the time it
            // takes to travel from there to the next one. The very first point is where the
            // envelope starts rather than something it climbs to.
            const SceNgsEnvelopePoint &point = params->envelopePoints[state->nCurrentPoint];
            const float from = point.fAmplitude;
            const float to = params->envelopePoints[state->nCurrentPoint + 1].fAmplitude;

            if (point.uMsecsToNextPoint == 0) {
                state->fCurrentHeight = to;
            } else {
                const float progress = std::min(state->fPosition / static_cast<float>(point.uMsecsToNextPoint), 1.0f);
                // The shape of a curved segment is not documented anywhere we know of, so it is
                // eased with a quadratic: it ends on the amplitude a linear one would, which is
                // what keeps a chain of segments continuous.
                const float t = (point.eCurveType == SCE_NGS_ENVELOPE_CURVED) ? progress * progress : progress;
                state->fCurrentHeight = from + (to - from) * t;
            }

            if (state->fPosition >= static_cast<float>(point.uMsecsToNextPoint)) {
                // A loop only holds while the voice is still held down; a key off lets the
                // envelope fall out of it and into the release.
                if (params->nLoopEnd >= 0 && state->nCurrentPoint == params->nLoopEnd
                    && static_cast<int32_t>(params->uLoopStart) < total_points) {
                    state->nCurrentPoint = static_cast<int32_t>(params->uLoopStart);
                } else {
                    state->nCurrentPoint++;
                }

                state->fPosition = 0.0f;
            }
        }

        samples[i * 2] *= state->fCurrentHeight;
        samples[i * 2 + 1] *= state->fCurrentHeight;

        state->fPosition += msecs_per_sample;
    }

    return false;
}
} // namespace ngs
