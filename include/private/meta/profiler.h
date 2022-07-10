/*
 * Copyright (C) 2021 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2021 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-plugins-profiler
 * Created on: 3 авг. 2021 г.
 *
 * lsp-plugins-profiler is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * lsp-plugins-profiler is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with lsp-plugins-profiler. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef PRIVATE_META_PROFILER_H_
#define PRIVATE_META_PROFILER_H_

#include <lsp-plug.in/plug-fw/meta/types.h>
#include <lsp-plug.in/plug-fw/const.h>


namespace lsp
{
    namespace meta
    {
        struct profiler_metadata
        {
            static constexpr float FREQUENCY_MIN        = 20.0f;
            static constexpr float FREQUENCY_MAX        = 20000.0f;
            static constexpr float FREQUENCY_DFL        = 1000.0f;
            static constexpr float FREQUENCY_STEP       = 0.01f;

            static constexpr float AMPLITUDE_DFL        = 1.0f;

            static constexpr float LATENCY_MIN          = 0.0f;         /* Min detectable latency [ms] */
            static constexpr float LATENCY_MAX          = 2000.0f;      /* Max detectable latency [ms] */
            static constexpr float LATENCY_DFL          = 1000.0f;
            static constexpr float LATENCY_STEP         = 1.0f;

            static constexpr float PEAK_THRESHOLD_MIN   = GAIN_AMP_M_84_DB;
            static constexpr float PEAK_THRESHOLD_MAX   = GAIN_AMP_0_DB;
            static constexpr float PEAK_THRESHOLD_DFL   = GAIN_AMP_M_24_DB;
            static constexpr float PEAK_THRESHOLD_STEP  = 0.01f;

            static constexpr float ABS_THRESHOLD_MIN    = GAIN_AMP_M_84_DB;
            static constexpr float ABS_THRESHOLD_MAX    = GAIN_AMP_0_DB;
            static constexpr float ABS_THRESHOLD_DFL    = GAIN_AMP_M_24_DB;
            static constexpr float ABS_THRESHOLD_STEP   = 0.01f;

            static constexpr float DURATION_MIN         = 1.0f;
            static constexpr float DURATION_MAX         = 50.0f;
            static constexpr float DURATION_DFL         = 10.0f;
            static constexpr float DURATION_STEP        = 0.5f;

            static constexpr float MTR_T_MIN            = 0.0f;
            static constexpr float MTR_T_MAX            = 60.0f;
            static constexpr float MTR_T_DFL            = 0.0f;
            static constexpr float MTR_T_STEP           = 1.0f;

            static constexpr float IR_OFFSET_MIN        = -1000.0f;
            static constexpr float IR_OFFSET_MAX        = 1000.0f;
            static constexpr float IR_OFFSET_DFL        = 0.0f;
            static constexpr float IR_OFFSET_STEP       = 0.01f;

            static constexpr float MTR_LATENCY_MIN      = 0.0f;         /* Min detectable latency [ms] */
            static constexpr float MTR_LATENCY_MAX      = 2000.0f;      /* Max detectable latency [ms] */
            static constexpr float MTR_LATENCY_DFL      = 0.0f;
            static constexpr float MTR_LATENCY_STEP     = 1.0f;

            enum rt_algorithm_selector_t
            {
                SC_RTALGO_EDT_0,
                SC_RTALGO_EDT_1,
                SC_RTALGO_T_10,
                SC_RTALGO_T_20,
                SC_RTALGO_T_30,

                SC_RTALGO_DFL = SC_RTALGO_T_20
            };

            static constexpr float MTR_RT_MIN           = 0.0f;
            static constexpr float MTR_RT_MAX           = 60.0f;
            static constexpr float MTR_RT_DFL           = 0.0f;
            static constexpr float MTR_RT_STEP          = 1.0f;

            static constexpr float MTR_IL_MIN           = 0.0f;
            static constexpr float MTR_IL_MAX           = 60.0f;
            static constexpr float MTR_IL_DFL           = 0.0f;
            static constexpr float MTR_IL_STEP          = 1.0f;

            static constexpr float MTR_R_MIN            = -1.0f;
            static constexpr float MTR_R_MAX            = 1.0f;
            static constexpr float MTR_R_DFL            = 0.0f;
            static constexpr float MTR_R_STEP           = 0.001f;

            enum saving_mode_selector_t
            {
                SC_SVMODE_AUTO,
                SC_SVMODE_RT,
                SC_SVMODE_IT,
                SC_SVMODE_ALL,
                SC_SVMOD_NLINEAR,

                SC_SVMODE_DFL = SC_SVMODE_AUTO
            };

            static constexpr size_t RESULT_MESH_SIZE    = 512;
        };

        extern const meta::plugin_t profiler_mono;
        extern const meta::plugin_t profiler_stereo;
    } // namespace meta
} // namespace lsp


#endif /* PRIVATE_META_PROFILER_H_ */
