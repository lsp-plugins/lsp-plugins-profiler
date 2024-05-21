/*
 * Copyright (C) 2024 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2024 Vladimir Sadovnikov <sadko4u@gmail.com>
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

#include <lsp-plug.in/plug-fw/meta/ports.h>
#include <lsp-plug.in/shared/meta/developers.h>
#include <lsp-plug.in/common/status.h>
#include <private/meta/profiler.h>

#define LSP_PLUGINS_PROFILER_VERSION_MAJOR       1
#define LSP_PLUGINS_PROFILER_VERSION_MINOR       0
#define LSP_PLUGINS_PROFILER_VERSION_MICRO       18

#define LSP_PLUGINS_PROFILER_VERSION  \
    LSP_MODULE_VERSION( \
        LSP_PLUGINS_PROFILER_VERSION_MAJOR, \
        LSP_PLUGINS_PROFILER_VERSION_MINOR, \
        LSP_PLUGINS_PROFILER_VERSION_MICRO  \
    )

namespace lsp
{
    namespace meta
    {
        static const int plugin_classes[]           = { C_UTILITY, -1};
        static const int clap_features_mono[]       = { CF_AUDIO_EFFECT, CF_UTILITY, CF_MONO, -1 };
        static const int clap_features_stereo[]     = { CF_AUDIO_EFFECT, CF_UTILITY, CF_STEREO, -1 };

        static const port_item_t profiler_states[] =
        {
            { "Idle",                   "profiler.st.idle" },
            { "Calibration",            "profiler.st.cal" },
            { "Latency Detection",      "profiler.st.lat" },
            { "Preprocessing",          "profiler.st.pre" },
            { "Waiting",                "profiler.st.wait" },
            { "Recording",              "profiler.st.rec" },
            { "Convolving",             "profiler.st.conv" },
            { "Postprocessing",         "profiler.st.post" },
            { "Saving",                 "profiler.st.save" },
            { NULL,                     NULL }
        };

        static const port_item_t sc_rtalgo[] =
        {
            { "EDT0",                   "profiler.algo.edt0" },
            { "EDT1",                   "profiler.algo.edt1" },
            { "RT10",                   "profiler.algo.rt10" },
            { "RT20",                   "profiler.algo.rt20" },
            { "RT30",                   "profiler.algo.rt30" },
            { NULL,                     NULL }
        };

        static const port_item_t sc_savemode[] =
        {
            { "LTI Auto (*.wav)",       "profiler.fmt.lti_auto" },
            { "LTI RT (*.wav)",         "profiler.fmt.lti_rt" },
            { "LTI Coarse (*.wav)",     "profiler.fmt.lti_coarse" },
            { "LTI All (*.wav)",        "profiler.fmt.lti_all" },
            { "All Info (*.lspc)",      "profiler.fmt.all" },
            { NULL,                     NULL }
        };

        #define CALIBRATOR \
            LOG_CONTROL("calf", "Frequency", U_HZ, profiler_metadata::FREQUENCY), \
            AMP_GAIN10("cala", "Amplitude", profiler_metadata::AMPLITUDE_DFL), \
            SWITCH("cals", "Calibration", 0.0f), \
            SWITCH("fbck", "Feedback", 0.0f)

        #define LATENCY_DETECTOR \
            CONTROL("ltdm", "Max expected latency", U_MSEC, profiler_metadata::LATENCY), \
            CONTROL("ltdp", "Peak threshold", U_GAIN_AMP, profiler_metadata::PEAK_THRESHOLD), \
            CONTROL("ltda", "Absolute threshold", U_GAIN_AMP, profiler_metadata::ABS_THRESHOLD), \
            SWITCH("ltena", "Enable Latency Detection", 1.0f), \
            TRIGGER("latt", "Trig a Latency measurement")

        #define TEST_SIGNAL \
            CONTROL("tsgl", "Duration", U_SEC, profiler_metadata::DURATION), \
            METER("tind", "Actual Signal Duration", U_SEC, profiler_metadata::MTR_T), \
            TRIGGER("lint", "Trig a Linear measurement")

        #define POSTPROCESSOR \
            CONTROL("offc", "IR Time Offset", U_MSEC, profiler_metadata::IR_OFFSET), \
            COMBO("scra", "RT Algorithm", profiler_metadata::SC_RTALGO_DFL, sc_rtalgo), \
            TRIGGER("post", "Trig Post Processing")

        #define SAVER \
            COMBO("scsv", "Save Mode", profiler_metadata::SC_SVMODE_DFL, sc_savemode), \
            PATH("irfn", "Save file name"), \
            TRIGGER("irfc", "Save file command"), \
            STATUS("irfs", "File saving status"), \
            METER_PERCENT("irfp", "File saving progress")

        #define PROFILER_COMMON \
            BYPASS, \
            { "stld", "State LED", U_ENUM, R_METER, F_INT, 0, 0, 0, 0, profiler_states }, \
            CALIBRATOR, \
            LATENCY_DETECTOR, \
            TEST_SIGNAL, \
            POSTPROCESSOR, \
            SAVER

        #define PROFILER_VISUALOUTS(id, label) \
            METER_GAIN20("ilv" id, "Input Level" label), \
            METER("lti" id, "Latency Value" label, U_MSEC, profiler_metadata::MTR_LATENCY), \
            METER("rti" id, "Reverberation Time" label, U_SEC, profiler_metadata::MTR_RT), \
            BLINK("rta" id, "Reverberation Time Accuracy" label), \
            METER("ili" id, "Integration Time" label, U_SEC, profiler_metadata::MTR_IL), \
            METER("rci" id, "Regression Line Correlation" label, U_NONE, profiler_metadata::MTR_R), \
            MESH("rme" id, "Result" label, 2, profiler_metadata::RESULT_MESH_SIZE)

        #define PROFILER_VISUALOUTS_MONO    PROFILER_VISUALOUTS("", "")
        #define PROFILER_VISUALOUTS_STEREO  PROFILER_VISUALOUTS("_l", " Left"), PROFILER_VISUALOUTS("_r", " Right")

        static const port_t profiler_mono_ports[] =
        {
            PORTS_MONO_PLUGIN,
            PROFILER_COMMON,
            PROFILER_VISUALOUTS_MONO,
            PORTS_END
        };

        static const port_t profiler_stereo_ports[] =
        {
            PORTS_STEREO_PLUGIN,
            PROFILER_COMMON,
            PROFILER_VISUALOUTS_STEREO,
            PORTS_END
        };

        const meta::bundle_t profiler_bundle =
        {
            "profiler",
            "Profiler",
            B_UTILITIES,
            "kXCBRnJL_qo",
            "A simple plugin for audio systems profiling. The profiling is performed by\nan algorithm based on the Synchronized Swept Sine method by Antonin Novak.\nThe profiler plugin allows one to profile audio systems. These properties of an\naudio system can be currently profiled: Latency, Linear Impulse Response,\nNonlinear Characteristics."
        };

        const meta::plugin_t profiler_mono =
        {
            "Profiler Mono",
            "Profiler Mono",
            "Profiler Mono",
            "P1M", // Profiler x1 Mono
            &developers::s_tronci,
            "profiler_mono",
            LSP_LV2_URI("profiler_mono"),
            LSP_LV2UI_URI("profiler_mono"),
            "hwrc",
            LSP_VST3_UID("p1m     hwrc"),
            LSP_VST3UI_UID("p1m     hwrc"),
            0,
            NULL,
            LSP_CLAP_URI("profiler_mono"),
            LSP_PLUGINS_PROFILER_VERSION,
            plugin_classes,
            clap_features_mono,
            E_DUMP_STATE,
            profiler_mono_ports,
            "util/profiler/mono.xml",
            NULL,
            mono_plugin_port_groups,
            &profiler_bundle
        };

        const meta::plugin_t profiler_stereo =
        {
            "Profiler Stereo",
            "Profiler Stereo",
            "Profiler Stereo",
            "P1S", // Profiler x1 Stereo
            &developers::s_tronci,
            "profiler_stereo",
            LSP_LV2_URI("profiler_stereo"),
            LSP_LV2UI_URI("profiler_stereo"),
            "hubw",
            LSP_VST3_UID("p1s     hubw"),
            LSP_VST3UI_UID("p1s     hubw"),
            0,
            NULL,
            LSP_CLAP_URI("profiler_stereo"),
            LSP_PLUGINS_PROFILER_VERSION,
            plugin_classes,
            clap_features_stereo,
            E_DUMP_STATE,
            profiler_stereo_ports,
            "util/profiler/stereo.xml",
            NULL,
            stereo_plugin_port_groups,
            &profiler_bundle
        };
    } /* namespace meta */
} /* namespace lsp */
