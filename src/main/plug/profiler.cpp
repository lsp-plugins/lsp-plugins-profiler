/*
 * Copyright (C) 2023 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2023 Vladimir Sadovnikov <sadko4u@gmail.com>
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

#include <lsp-plug.in/common/alloc.h>
#include <lsp-plug.in/common/debug.h>
#include <lsp-plug.in/dsp/dsp.h>
#include <lsp-plug.in/dsp-units/units.h>
#include <lsp-plug.in/stdlib/math.h>

#include <private/plugins/profiler.h>

#define TMP_BUF_SIZE                1024
#define BGNOISE_INTERVAL_FACTOR     2.0f    /* Multiply reactivity by this to calculate the time to spend in BGNOISEPROFILING state */
#define POSTPROCESSOR_REACTIVITY    0.085f  /* Window Size for the Post Processor IR envelope follower [s] */
#define POSTPROCESSOR_TOLERANCE     3.0     /* Maximum level above the noise floor below which, if IR peaks are not found, the IR is considered concluded [dB] */
#define SYNC_CHIRP_START_FREQ       1.0     /* Synchronized Chirp Starting frequency [Hz] */
#define SYNC_CHIRP_MAX_FREQ         23000.0 /* Synchronized Chirp Final frequency [Hz] */
#define TRACE_PORT(p)               lsp_trace("  port id=%s", (p)->metadata()->id);

namespace lsp
{
    namespace plugins
    {
        //---------------------------------------------------------------------
        // Plugin factory
        static const meta::plugin_t *plugins[] =
        {
            &meta::profiler_mono,
            &meta::profiler_stereo
        };

        static plug::Module *plugin_factory(const meta::plugin_t *meta)
        {
            return new profiler(meta, (meta == &meta::profiler_stereo) ? 2 : 1);
        }

        static plug::Factory factory(plugin_factory, plugins, 2);

        //---------------------------------------------------------------------
        profiler::PreProcessor::PreProcessor(profiler *base)
        {
            pCore = base;
        }

        profiler::PreProcessor::~PreProcessor()
        {
            pCore = NULL;
        }

        status_t profiler::PreProcessor::run()
        {
            // reconfigure() will call update_settings() if needed.
            status_t  returnValue = pCore->sSyncChirpProcessor.reconfigure();
            if (returnValue != STATUS_OK)
                return returnValue;

            for (size_t ch = 0; ch < pCore->nChannels; ++ch)
            {
                returnValue = pCore->vChannels[ch].sResponseTaker.reconfigure(pCore->sSyncChirpProcessor.get_chirp());
                if (returnValue != STATUS_OK)
                    return returnValue;
            }

            return STATUS_OK;
        }

        //---------------------------------------------------------------------
        profiler::Convolver::Convolver(profiler *base)
        {
            pCore = base;
        }

        profiler::Convolver::~Convolver()
        {
            pCore = NULL;
        }

        status_t profiler::Convolver::run()
        {
            for (size_t ch = 0; ch < pCore->nChannels; ++ch)
            {
                pCore->sResponseData.vResponses[ch] = pCore->vChannels[ch].sResponseTaker.get_capture();
                pCore->sResponseData.vOffsets[ch]   = pCore->vChannels[ch].sResponseTaker.get_capture_start();
            }

            // Use 0 to force default partition size defined in SyncChirpProcessor
            return pCore->sSyncChirpProcessor.do_linear_convolutions(
                    pCore->sResponseData.vResponses,
                    pCore->sResponseData.vOffsets,
                    pCore->nChannels,
                    0
                    );
        }

        //---------------------------------------------------------------------
        profiler::PostProcessor::PostProcessor(profiler *base)
        {
            pCore       = base;
            nIROffset   = 0;
            enAlgo      = dspu::SCP_RT_DEFAULT;
        }

        profiler::PostProcessor::~PostProcessor()
        {
            pCore       = NULL;
            nIROffset   = 0;
            enAlgo      = dspu::SCP_RT_DEFAULT;
        }

        void profiler::PostProcessor::set_ir_offset(ssize_t ir_offset)
        {
            nIROffset = ir_offset;
        }

        void profiler::PostProcessor::set_rt_algo(dspu::scp_rtcalc_t algo)
        {
            enAlgo = algo;
        }

        status_t profiler::PostProcessor::run()
        {
            for (size_t ch = 0; ch < pCore->nChannels; ++ch)
            {
                status_t returnValue = pCore->sSyncChirpProcessor.postprocess_linear_convolution(ch, nIROffset, enAlgo, POSTPROCESSOR_REACTIVITY, POSTPROCESSOR_TOLERANCE);
                if (returnValue != STATUS_OK)
                    return returnValue;

                pCore->vChannels[ch].sPostProc.fReverbTime  = pCore->sSyncChirpProcessor.get_reverberation_time_seconds();
                pCore->vChannels[ch].sPostProc.nReverbTime  = pCore->sSyncChirpProcessor.get_reverberation_time_samples();
                pCore->vChannels[ch].sPostProc.fCorrCoeff   = pCore->sSyncChirpProcessor.get_reverberation_correlation();
                pCore->vChannels[ch].sPostProc.fIntgLimit   = pCore->sSyncChirpProcessor.get_integration_limit_seconds();
                pCore->vChannels[ch].sPostProc.bRTAccuray   = pCore->sSyncChirpProcessor.get_background_noise_optimality();
            }

            return STATUS_OK;
        }

        //---------------------------------------------------------------------
        profiler::Saver::Saver(profiler *base)
        {
            pCore       = base;
            nIROffset   = 0;
            sFile[0]    = '\0';
        }

        profiler::Saver::~Saver()
        {
            pCore       = NULL;
            sFile[0]    = '\0';
        }

        void profiler::Saver::set_ir_offset(ssize_t ir_offset)
        {
            nIROffset = ir_offset;
        }

        void profiler::Saver::set_file_name(const char *fname)
        {
            if (fname != NULL)
            {
                strncpy(sFile, fname, PATH_MAX);
                sFile[PATH_MAX - 1] = '\0';
            }
            else
                sFile[0] = '\0';
        }

        bool profiler::Saver::is_file_set() const
        {
            return sFile[0] != '\0';
        }

        status_t profiler::Saver::run()
        {
            // Doing Checks:
            if (!pCore->bIRMeasured)
            {
                pCore->sSaveData.enSaveStatus = STATUS_NO_DATA;
                pCore->sSaveData.fSavePercent = 0.0f;
                return STATUS_NO_DATA;
            }

            // Calculate saving time.
            float fRT   = 0.0f;
            float fIL   = 0.0f;
            float fAll  = pCore->sSyncChirpProcessor.get_convolution_result_positive_time_length();

            for (size_t ch = 0; ch < pCore->nChannels; ++ch)
            {
                if (pCore->vChannels[ch].sPostProc.fReverbTime > fRT)
                    fRT = pCore->vChannels[ch].sPostProc.fReverbTime;

                if (pCore->vChannels[ch].sPostProc.fIntgLimit > fIL)
                    fIL = pCore->vChannels[ch].sPostProc.fIntgLimit;
            }

            float saveTime      = (fRT > fIL) ? fRT : fIL;
            bool doNlinearSave  = false;

            switch (pCore->nSaveMode)
            {
                case meta::profiler_metadata::SC_SVMODE_RT:
                    saveTime        = fRT;
                    break;
                case meta::profiler_metadata::SC_SVMODE_IT:
                    saveTime        = fIL;
                    break;
                case meta::profiler_metadata::SC_SVMODE_ALL:
                    saveTime        = fAll;
                    break;
                case meta::profiler_metadata::SC_SVMOD_NLINEAR:
                    doNlinearSave   = true;
                    break;
                case meta::profiler_metadata::SC_SVMODE_AUTO:
                default:
                    break;
            }

            saveTime            = ceilf(saveTime * 10.0f) / 10.0f; // Round to the next tenth of second
            size_t saveCount    = dspu::seconds_to_samples(pCore->nSampleRate, saveTime); // This count is relative to the middle of the convolution result

            // Saving Data:
            status_t returnValue;

            // Update saveCount to account for offset
            if (nIROffset > 0)
                saveCount += size_t(nIROffset);
            else
                saveCount += size_t(-nIROffset);

            lsp_trace("Saving %s convolution to path = %s", ((doNlinearSave) ? "nonlinear" : "linear"), sFile);
            if (doNlinearSave)
                returnValue = pCore->sSyncChirpProcessor.save_to_lspc(sFile, nIROffset);
            else
                returnValue = pCore->sSyncChirpProcessor.save_linear_convolution(sFile, nIROffset, saveCount);
            lsp_trace("save status: %d", int(returnValue));

            if (returnValue == STATUS_OK)
            {
                pCore->sSaveData.enSaveStatus = STATUS_OK;
                pCore->sSaveData.fSavePercent = 100.0f;
            }
            else
            {
                pCore->sSaveData.enSaveStatus = STATUS_UNKNOWN_ERR;
                pCore->sSaveData.fSavePercent = 0.0f;
            }

            return returnValue;
        }

        //---------------------------------------------------------------------
        profiler::profiler(const meta::plugin_t *metadata, size_t channels): plug::Module(metadata)
        {
            nChannels                   = channels;
            vChannels                   = NULL;

            sResponseData.vResponses    = NULL;
            sResponseData.vOffsets      = NULL;

            sSaveData.enSaveStatus      = STATUS_OK;
            sSaveData.fSavePercent      = 0.0f;

            nState                      = IDLE;

            pExecutor                   = NULL;
            pPreProcessor               = NULL;
            pConvolver                  = NULL;
            pPostProcessor              = NULL;
            pSaver                      = NULL;

            nSampleRate                 = 0;
            fLtAmplitude                = 1.0f;
            nWaitCounter                = 0;
            bDoLatencyOnly              = false;

            fScpDurationPrevious        = 0.0f;
            bIRMeasured                 = false;
            nSaveMode                   = meta::profiler_metadata::SC_SVMODE_DFL;

            nTriggers                   = 0;

            vTempBuffer                 = NULL;
            vDisplayAbscissa            = NULL;
            vDisplayOrdinate            = NULL;
            pData                       = NULL;

            pBypass                     = NULL;

            pStateLEDs                  = NULL;

            pCalFrequency               = NULL;
            pCalAmplitude               = NULL;
            pCalSwitch                  = NULL;
            pFeedback                   = NULL;

            pLdMaxLatency               = NULL;
            pLdPeakThs                  = NULL;
            pLdAbsThs                   = NULL;
            pLdEnableSwitch             = NULL;
            pLatTrigger                 = NULL;

            pDuration                   = NULL;
            pActualDuration             = NULL;
            pLinTrigger                 = NULL;

            pIROffset                   = NULL;
            pRTAlgoSelector             = NULL;
            pPostTrigger                = NULL;

            pSaveModeSelector           = NULL;
            pIRFileName                 = NULL;
            pIRSaveCmd                  = NULL;
            pIRSaveStatus               = NULL;
            pIRSavePercent              = NULL;
        }

        profiler::~profiler()
        {
            do_destroy();
        }

        void profiler::destroy()
        {
            plug::Module::destroy();
            do_destroy();
        }

        void profiler::do_destroy()
        {
            if (pPreProcessor != NULL)
            {
                delete pPreProcessor;
                pPreProcessor = NULL;
            }

            if (pConvolver != NULL)
            {
                delete pConvolver;
                pConvolver = NULL;
            }

            if (pPostProcessor != NULL)
            {
                delete pPostProcessor;
                pPostProcessor = NULL;
            }

            if (pSaver != NULL)
            {
                delete pSaver;
                pSaver = NULL;
            }

            if (sResponseData.vResponses != NULL)
            {
                for (size_t ch = 0; ch < nChannels; ++ch)
                {
                    dspu::Sample *s = sResponseData.vResponses[ch];
                    if (s != NULL)
                        s->destroy();
                }

                delete[] sResponseData.vResponses;
                sResponseData.vResponses = NULL;
            }

            free_aligned(sResponseData.pData);
            sResponseData.pData     = NULL;
            sResponseData.vOffsets  = NULL;

            free_aligned(pData);
            pData                   = NULL;
            vTempBuffer             = NULL;
            vDisplayAbscissa        = NULL;
            vDisplayOrdinate        = NULL;

            if (vChannels != NULL)
            {
                for (size_t ch = 0; ch < nChannels; ++ch)
                {
                    channel_t *c = &vChannels[ch];
                    c->sLatencyDetector.destroy();
                    c->sResponseTaker.destroy();
                    c->vBuffer = NULL;
                }
                delete [] vChannels;
                vChannels = NULL;
            }

            sSyncChirpProcessor.destroy();
            sCalOscillator.destroy();
        }

        void profiler::update_pre_processing_info()
        {
            pActualDuration->set_value(sSyncChirpProcessor.get_chirp_duration_seconds());

            for (size_t ch = 0; ch < nChannels; ++ch)
            {
                vChannels[ch].sResponseTaker.set_op_tail(sSyncChirpProcessor.get_chirp_duration_seconds());
            }
        }

        bool profiler::update_post_processing_info()
        {
            ssize_t nIROffset = pPostProcessor->get_ir_offset();

            for (size_t ch = 0; ch < nChannels; ++ch)
            {
                channel_t *c = &vChannels[ch];

                c->pRTScreen->set_value(c->sPostProc.fReverbTime);
                c->pRScreen->set_value(c->sPostProc.fCorrCoeff);
                c->pILScreen->set_value(c->sPostProc.fIntgLimit);
                c->pRTAccuracyLed->set_value((c->sPostProc.bRTAccuray) ? 1.0f : 0.0f);
            }

            // Do the plots
            for (size_t ch = 0; ch < nChannels; ++ch)
            {
                channel_t *c = &vChannels[ch];
                size_t irQuery = (nIROffset > 0) ? c->sPostProc.nReverbTime : c->sPostProc.nReverbTime + size_t(-nIROffset);

                sSyncChirpProcessor.get_convolution_result_plottable_samples(ch, vDisplayOrdinate, nIROffset, irQuery , meta::profiler_metadata::RESULT_MESH_SIZE, true);

                plug::mesh_t *mesh = c->pResultMesh->buffer<plug::mesh_t>();
                if (mesh != NULL)
                {
                    if (!mesh->isEmpty())
                        return false;

                    dsp::copy(mesh->pvData[0], vDisplayAbscissa, meta::profiler_metadata::RESULT_MESH_SIZE);
                    dsp::copy(mesh->pvData[1], vDisplayOrdinate, meta::profiler_metadata::RESULT_MESH_SIZE);
                    mesh->data(2, meta::profiler_metadata::RESULT_MESH_SIZE);
                }
            }

            if (pWrapper != NULL)
                pWrapper->query_display_draw();

            return true;
        }

        void profiler::update_saving_info()
        {
            pIRSaveStatus->set_value(sSaveData.enSaveStatus);
            pIRSavePercent->set_value(sSaveData.fSavePercent);
        }

        dspu::scp_rtcalc_t profiler::get_rt_algorithm(size_t algorithm)
        {
            switch (algorithm)
            {
                case meta::profiler_metadata::SC_RTALGO_EDT_0:
                    return dspu::SCP_RT_EDT_0;
                case meta::profiler_metadata::SC_RTALGO_EDT_1:
                    return dspu::SCP_RT_EDT_1;
                case meta::profiler_metadata::SC_RTALGO_T_10:
                    return dspu::SCP_RT_T_10;
                case meta::profiler_metadata::SC_RTALGO_T_20:
                    return dspu::SCP_RT_T_20;
                case meta::profiler_metadata::SC_RTALGO_T_30:
                    return dspu::SCP_RT_T_30;
                default:
                    return dspu::SCP_RT_T_20;
            }
        }

        void profiler::init(plug::IWrapper *wrapper, plug::IPort **ports)
        {
            plug::Module::init(wrapper, ports);

            pExecutor = wrapper->executor();
            lsp_trace("Executor = %p", pExecutor);

            // 1X Global Temporary Buffer + 2X buffer to plot data + 1X temporary DSP buffer per channel
            size_t samples = TMP_BUF_SIZE + 2 * meta::profiler_metadata::RESULT_MESH_SIZE + nChannels * TMP_BUF_SIZE;

            float *ptr = alloc_aligned<float>(pData, samples);
            if (ptr == NULL)
                return;

            lsp_guard_assert(float *save = ptr);
            vTempBuffer             = ptr;
            ptr                     += TMP_BUF_SIZE;
            vDisplayAbscissa        = ptr;
            ptr                    += meta::profiler_metadata::RESULT_MESH_SIZE;
            vDisplayOrdinate        = ptr;
            ptr                    += meta::profiler_metadata::RESULT_MESH_SIZE;

            // vAbscissa never changes, so it can be filled here
            for (size_t n = 0; n < meta::profiler_metadata::RESULT_MESH_SIZE; ++n)
                vDisplayAbscissa[n] = float(2 * n) / meta::profiler_metadata::RESULT_MESH_SIZE;

            // Buffers are allocate when allocating channels

            // Allocate channels
            vChannels = new channel_t[nChannels];
            if (vChannels == NULL)
                return;

            for (size_t ch = 0; ch < nChannels; ++ch)
            {
                channel_t *c = &vChannels[ch];

                c->sLatencyDetector.init();
                c->sLatencyDetector.set_delay_ratio(0.5f);
                c->sLatencyDetector.set_duration(0.050f);
                c->sLatencyDetector.set_op_fading(0.030f);
                c->sLatencyDetector.set_op_pause(0.025f);

                c->sResponseTaker.init();
                c->sResponseTaker.set_op_fading(0.030f);
                c->sResponseTaker.set_op_pause(0.025f);

                c->nLatency         = 0;
                c->bLatencyMeasured = false;
                c->bLCycleComplete  = false;
                c->bRCycleComplete  = false;

                c->sPostProc.fReverbTime    = 0.0f;
                c->sPostProc.nReverbTime    = 0;
                c->sPostProc.fCorrCoeff     = 0.0f;
                c->sPostProc.fIntgLimit     = 0.0f;
                c->sPostProc.bRTAccuray     = false;

                vChannels[ch].vBuffer   = ptr;
                ptr                    += TMP_BUF_SIZE;

                c->vIn  = NULL;
                c->vOut = NULL;

                c->pIn  = NULL;
                c->pOut = NULL;

                c->pLevelMeter      = NULL;
                c->pLatencyScreen   = NULL;
                c->pRTScreen        = NULL;
                c->pRTAccuracyLed   = NULL;
                c->pILScreen        = NULL;
                c->pRScreen         = NULL;
                c->pResultMesh      = NULL;
            }

            lsp_assert(ptr <= &save[samples]);

            // Allocate array of pointers to Samples objects for convolution.
            sResponseData.vResponses = new dspu::Sample* [nChannels];
            for (size_t i=0; i<nChannels; ++i)
                sResponseData.vResponses[i]     = NULL;

            // Allocate vector of chirp responses offsets. Long as the number of channels
            size_t *sRDptr = alloc_aligned<size_t>(sResponseData.pData, nChannels);
            if (sRDptr == NULL)
                return;

            lsp_guard_assert(size_t *sRDsave = sRDptr);
            sResponseData.vOffsets  = sRDptr;
            sRDptr                 += nChannels;
            lsp_assert(sRDptr <= &sRDsave[nChannels]);

            if (!sCalOscillator.init())
                return;
            sCalOscillator.set_function(dspu::FG_SINE);
            sCalOscillator.set_dc_offset(0.0f);
            sCalOscillator.set_dc_reference(dspu::DC_ZERO);
            sCalOscillator.set_phase(0.0f);

            if (!sSyncChirpProcessor.init())
                return;
            sSyncChirpProcessor.set_chirp_synthesis_method(dspu::SCP_SYNTH_BANDLIMITED);
            sSyncChirpProcessor.set_chirp_initial_frequency(SYNC_CHIRP_START_FREQ);
            sSyncChirpProcessor.set_chirp_final_frequency(SYNC_CHIRP_MAX_FREQ);
            sSyncChirpProcessor.set_chirp_amplitude(meta::profiler_metadata::AMPLITUDE_DFL);
            sSyncChirpProcessor.set_fader_fading_method(dspu::SCP_FADE_RAISED_COSINES);
            sSyncChirpProcessor.set_fader_fadein(0.500f);
            sSyncChirpProcessor.set_fader_fadeout(0.020f);
            sSyncChirpProcessor.set_oversampler_mode(dspu::OM_LANCZOS_8X2);

            pPreProcessor           = new PreProcessor(this);
            pConvolver              = new Convolver(this);
            pPostProcessor          = new PostProcessor(this);
            pSaver                  = new Saver(this);

            // Bind ports
            size_t port_id          = 0;

            // Audio
            lsp_trace("Binding audio ports");
            for (size_t ch = 0; ch < nChannels; ++ch)
            {
                TRACE_PORT(ports[port_id]);
                vChannels[ch].pIn = ports[port_id++];
            }
            for (size_t ch = 0; ch < nChannels; ++ch)
            {
                TRACE_PORT(ports[port_id]);
                vChannels[ch].pOut = ports[port_id++];
            }

            // Common
            lsp_trace("Binding common ports");
            pBypass             = ports[port_id++];

            pStateLEDs          = ports[port_id++];

            pCalFrequency       = ports[port_id++];
            pCalAmplitude       = ports[port_id++];
            pCalSwitch          = ports[port_id++];
            pFeedback           = ports[port_id++];

            pLdMaxLatency       = ports[port_id++];
            pLdPeakThs          = ports[port_id++];
            pLdAbsThs           = ports[port_id++];
            pLdEnableSwitch     = ports[port_id++];
            pLatTrigger         = ports[port_id++];

            pDuration           = ports[port_id++];
            pActualDuration     = ports[port_id++];
            pLinTrigger         = ports[port_id++];

            pIROffset           = ports[port_id++];
            pRTAlgoSelector     = ports[port_id++];
            pPostTrigger        = ports[port_id++];

            pSaveModeSelector   = ports[port_id++];
            pIRFileName         = ports[port_id++];
            pIRSaveCmd          = ports[port_id++];
            pIRSaveStatus       = ports[port_id++];
            pIRSavePercent      = ports[port_id++];

            // Data Visualisation
            lsp_trace("Binding data visualisation ports");

            for (size_t ch = 0; ch < nChannels; ++ch)
            {
                TRACE_PORT(ports[port_id]);
                vChannels[ch].pLevelMeter       = ports[port_id++];

                TRACE_PORT(ports[port_id]);
                vChannels[ch].pLatencyScreen    = ports[port_id++];

                TRACE_PORT(ports[port_id]);
                vChannels[ch].pRTScreen         = ports[port_id++];
                TRACE_PORT(ports[port_id]);
                vChannels[ch].pRTAccuracyLed    = ports[port_id++];

                TRACE_PORT(ports[port_id]);
                vChannels[ch].pILScreen         = ports[port_id++];

                TRACE_PORT(ports[port_id]);
                vChannels[ch].pRScreen          = ports[port_id++];

                TRACE_PORT(ports[port_id]);
                vChannels[ch].pResultMesh       = ports[port_id++];
            }
        }

        void profiler::update_sample_rate(long sr)
        {
            nSampleRate = sr;

            for (size_t ch = 0; ch < nChannels; ++ch)
            {
                vChannels[ch].sBypass.init(sr);
                vChannels[ch].sLatencyDetector.set_sample_rate(sr);
                vChannels[ch].sResponseTaker.set_sample_rate(sr);
            }

            sCalOscillator.set_sample_rate(sr);
            sSyncChirpProcessor.set_sample_rate(sr);
        }

        void profiler::process_buffer(size_t to_do)
        {
            switch (nState)
            {
                case IDLE:
                {
                    for (size_t ch = 0; ch < nChannels; ++ch)
                        dsp::fill_zero(vChannels[ch].vBuffer, to_do);
                }
                break;

                case CALIBRATION:
                {
                    // Create one calibrator output sequence all, and copy it over the other channels
                    if (nTriggers & T_CALIBRATION)
                    {
                        sCalOscillator.process_overwrite(vTempBuffer, to_do);
                    }
                    else
                    {
                        dsp::fill_zero(vTempBuffer, to_do);
                        // This state transition is here, instead than in update_settings(), to avoid
                        // forcing IDLE every time the calibrator is found off (which could happen during system test
                        // if a control is moved).
                        nState  = IDLE;
                    }

                    for (size_t ch = 0; ch < nChannels; ++ch)
                        dsp::copy(vChannels[ch].vBuffer, vTempBuffer, to_do);
                }
                break;

                case LATENCYDETECTION:
                {
                    // Transition to next states only if all latency detectors have finished the job
                    bool bAllMeasured = true;
                    bool bAllComplete = true;

                    for (size_t ch = 0; ch < nChannels; ++ch)
                    {
                        channel_t *c = &vChannels[ch];

                        c->sLatencyDetector.process_in(c->vBuffer, c->vIn, to_do);

                        if (!(nTriggers & T_FEEDBACK))
                            dsp::fill_zero(c->vBuffer, to_do);

                        c->sLatencyDetector.process_out(c->vBuffer, c->vBuffer, to_do);
                        dsp::mul_k2(c->vBuffer, fLtAmplitude, to_do);

                        if (c->sLatencyDetector.latency_detected())
                        {
                            c->bLatencyMeasured = true;
                            c->bLCycleComplete  = true;
                            c->nLatency         = c->sLatencyDetector.get_latency_samples();

                            c->pLatencyScreen->set_value(c->sLatencyDetector.get_latency_seconds() * 1000.0f); // * 1000.0f to show ms instead of s
                            c->sResponseTaker.set_latency_samples(c->nLatency);
                            c->sLatencyDetector.reset_capture();
                        }
                        else if (c->sLatencyDetector.cycle_complete())
                        {
                            c->bLatencyMeasured = false;
                            c->bLCycleComplete  = true;
                            c->nLatency         = 0;
                            c->sLatencyDetector.reset_capture();
                        }

                        bAllMeasured = bAllMeasured && vChannels[ch].bLatencyMeasured;
                        bAllComplete = bAllComplete && vChannels[ch].bLCycleComplete;
                    }

                    if (bAllMeasured)
                    {
                        nState              = (bDoLatencyOnly) ? IDLE : PREPROCESSING;
                        bDoLatencyOnly      = false;
                    }
                    else if (bAllComplete)
                    {
                        nState              = IDLE;
                    }

                    nWaitCounter           -= to_do;
                }
                break;

                case PREPROCESSING:
                {
                    // Check task state. If needed (first time we get here after state transition) submit the
                    // task.
                    if (pPreProcessor->idle())
                        pExecutor->submit(pPreProcessor);
                    else if (pPreProcessor->completed()) // Advance machine status only if when (and if) the pre processing task is completed
                    {
                        nState = (pPreProcessor->successful()) ? WAIT : IDLE;
                        if (nState == WAIT)
                            update_pre_processing_info();

                        pPreProcessor->reset();
                    }

                    for (size_t ch = 0; ch < nChannels; ++ch)
                        dsp::fill_zero(vChannels[ch].vBuffer, to_do);

                    nWaitCounter   -= to_do;
                }
                break;

                case WAIT:
                {
                    if (nWaitCounter <= 0)
                    {
                        bIRMeasured = false;
                        nState      = RECORDING;

                        for (size_t ch = 0; ch < nChannels; ++ch)
                        {
                            vChannels[ch].sResponseTaker.start_capture();
                            vChannels[ch].bRCycleComplete = false;
                        }
                    }

                    for (size_t ch = 0; ch < nChannels; ++ch)
                        dsp::fill_zero(vChannels[ch].vBuffer, to_do);

                    nWaitCounter   -= to_do;
                }
                break;

                case RECORDING:
                {
                    bool bAllComplete = true;

                    for (size_t ch = 0; ch < nChannels; ++ch)
                    {
                        channel_t *c = &vChannels[ch];

                        c->sResponseTaker.process_in(c->vBuffer, c->vIn, to_do);

                        if (!(nTriggers & T_FEEDBACK))
                            dsp::fill_zero(c->vBuffer, to_do);

                        c->sResponseTaker.process_out(c->vBuffer, c->vBuffer, to_do);

                        if (c->sResponseTaker.cycle_complete())
                        {
                            c->bRCycleComplete = true;
                            c->sResponseTaker.reset_capture();
                        }

                        bAllComplete = bAllComplete && c->bRCycleComplete;
                    }

                    if (bAllComplete)
                        nState = CONVOLVING;
                }
                break;

                case CONVOLVING:
                {
                    if (pConvolver->idle())
                        pExecutor->submit(pConvolver);
                    else if (pConvolver->completed())
                    {
                        bIRMeasured = true;
                        pConvolver->reset();
                        nState = POSTPROCESSING;
                    }

                    for (size_t ch = 0; ch < nChannels; ++ch)
                        dsp::fill_zero(vChannels[ch].vBuffer, to_do);
                }
                break;

                case POSTPROCESSING:
                {
                    if (pPostProcessor->idle())
                    {
                        ssize_t nIROffset = dspu::millis_to_samples(nSampleRate, pIROffset->value());
                        pPostProcessor->set_ir_offset(nIROffset);
                        pSaver->set_ir_offset(nIROffset); // We set it here also for the saver, so that it matches the postprocessing value.
                        pPostProcessor->set_rt_algo(get_rt_algorithm(pRTAlgoSelector->value()));
                        pExecutor->submit(pPostProcessor);
                    }
                    else if (pPostProcessor->completed())
                    {
                        // We should loop until the output mesh is committed to UI
                        if (update_post_processing_info())
                        {
                            bIRMeasured = true;
                            nState      = IDLE;
                            pPostProcessor->reset();
                        }
                    }

                    for (size_t ch = 0; ch < nChannels; ++ch)
                        dsp::fill_zero(vChannels[ch].vBuffer, to_do);
                }
                break;

                case SAVING:
                {
                    if (pSaver->idle())
                    {
                        sSaveData.enSaveStatus = STATUS_LOADING;
                        sSaveData.fSavePercent = 0.0f;
                        update_saving_info();

                        pExecutor->submit(pSaver);
                    }
                    else if (pSaver->completed())
                    {
                        update_saving_info();
                        nState      = IDLE;
                        pSaver->reset();
                    }

                    for (size_t ch = 0; ch < nChannels; ++ch)
                        dsp::fill_zero(vChannels[ch].vBuffer, to_do);
                }
                break;
            }
        }

        void profiler::process(size_t samples)
        {
            // Bind audio ports
            for (size_t ch = 0; ch < nChannels; ++ch)
            {
                vChannels[ch].vIn   = vChannels[ch].pIn->buffer<float>();
                vChannels[ch].vOut  = vChannels[ch].pOut->buffer<float>();

                if ((vChannels[ch].vIn == NULL) || (vChannels[ch].vOut == NULL))
                    return;
            }

            // Commit new changes to processors
            commit_state_change();

            plug::path_t *path = (pIRFileName != NULL) ? pIRFileName->buffer<plug::path_t>() : NULL;
            if ((path != NULL) && (path->pending()) && (pSaver->idle()))
            {
                // Accept new file name
                path->accept();
                lsp_trace("set file name to %s", path->path());
                pSaver->set_file_name(path->path());

                // Commit
                path->commit();
            }

            // Set state to SAVING if all conditions are met
            if (pIRSaveCmd->value() > 0.5f)
            {
                if ((nState == IDLE) && (bIRMeasured) && (pSaver->is_file_set()))
                    nState = SAVING;
                else if (nState != SAVING)
                {
                    sSaveData.enSaveStatus  = STATUS_BAD_STATE;
                    sSaveData.fSavePercent  = 0.0f;
                    update_saving_info();
                }
            }

            //---------------------------------------------------------------------
            // Perform processing loop
            for (size_t ch = 0; ch < nChannels; ++ch)
                vChannels[ch].pLevelMeter->set_value(dsp::abs_max(vChannels[ch].vIn, samples));

            while (samples > 0)
            {
                size_t to_do = (samples > TMP_BUF_SIZE) ? TMP_BUF_SIZE : samples;

                process_buffer(to_do);

                for (size_t ch = 0; ch < nChannels; ++ch)
                {
                    vChannels[ch].sBypass.process(vChannels[ch].vOut, vChannels[ch].vIn, vChannels[ch].vBuffer, to_do);

                    vChannels[ch].vIn   += to_do;
                    vChannels[ch].vOut  += to_do;
                }

                samples    -= to_do;
            }

            // Write always
    //        lsp_trace("state = %d", nState);
            pStateLEDs->set_value(nState);
        }

        void profiler::update_settings()
        {
            // Commit bypass state
            bool bPassValue = pBypass->value() >= 0.5f;
            for (size_t ch = 0; ch < nChannels; ++ch)
                vChannels[ch].sBypass.set_bypass(bPassValue);

            // Mark that there is pending state change request
            nTriggers               |= T_CHANGE;

            // Linear measurement trigger
            bool old = nTriggers & T_LIN_TRIGGER_STATE;
            if (pLinTrigger->value() >= 0.5f)
                nTriggers                  |= T_LIN_TRIGGER_STATE;
            else
                nTriggers                  &= ~T_LIN_TRIGGER_STATE;
            if (old && (!(nTriggers & T_LIN_TRIGGER_STATE))) // React on button release
                nTriggers                  |= T_LIN_TRIGGER;

            // Latency measurement trigger
            old = nTriggers & T_LAT_TRIGGER_STATE;
            if (pLatTrigger->value() >= 0.5f)
                nTriggers                  |= T_LAT_TRIGGER_STATE;
            else
                nTriggers                  &= ~T_LAT_TRIGGER_STATE;
            if (old && (!(nTriggers & T_LAT_TRIGGER_STATE))) // React on button release
                nTriggers                  |= T_LAT_TRIGGER;

            // Post-process trigger
            old = nTriggers & T_POSTPROCESS_STATE;
            if (pPostTrigger->value() >= 0.5f)
                nTriggers                  |= T_POSTPROCESS_STATE;
            else
                nTriggers                  &= ~T_POSTPROCESS_STATE;
            if (old && (!(nTriggers & T_POSTPROCESS_STATE))) // React on button release
                nTriggers                  |= T_POSTPROCESS;

            // Calibration switch
            if (pCalSwitch->value() >= 0.5f)
                nTriggers                  |= T_CALIBRATION;
            else
                nTriggers                  &= ~T_CALIBRATION;

            // Latency detect switch
            if (pLdEnableSwitch->value() >= 0.5f)
                nTriggers                  &= ~T_SKIP_LATENCY_DETECT; // We skip if the switch is enabled
            else
                nTriggers                  |= T_SKIP_LATENCY_DETECT;

            // Feedback switch
            if (pFeedback->value() >= 0.5f)
                nTriggers                  |= T_FEEDBACK;
            else
                nTriggers                  &= ~T_FEEDBACK;
        }

        void profiler::reset_tasks()
        {
            pPreProcessor->reset();
            pConvolver->reset();
            pPostProcessor->reset();
            pSaver->reset();

            for (size_t ch = 0; ch < nChannels; ++ch)
                vChannels[ch].sResponseTaker.reset_capture();
        }

        void profiler::commit_state_change()
        {
            switch (nState)
            {
                // Valid states to perform immediate reset
                case CALIBRATION:
                case IDLE:
                case LATENCYDETECTION:
                case WAIT:
                case RECORDING:
                    if (!(nTriggers & T_CHANGE))
                        return;
                    break;
                // Do not commit changes for other states
                default:
                    return;
            }

            bool reset_saver            = false;
            fLtAmplitude                = pCalAmplitude->value();
            float scDurationSetting     = pDuration->value();

            // Do not allow changes for latency detector when it's active
            float maxLat    = pLdMaxLatency->value() * 0.001f;
            float pkThs     = pLdPeakThs->value();
            float absThs    = pLdAbsThs->value();

            for (size_t ch = 0; ch < nChannels; ++ch)
            {
                vChannels[ch].sLatencyDetector.set_ip_detection(maxLat);
                vChannels[ch].sLatencyDetector.set_peak_threshold(pkThs);
                vChannels[ch].sLatencyDetector.set_abs_threshold(absThs);
            }

            sCalOscillator.set_amplitude(fLtAmplitude);
            sCalOscillator.set_frequency(pCalFrequency->value());

            // Change duration setting only if the controller actually changed
            sSyncChirpProcessor.set_chirp_duration(scDurationSetting);
            sSyncChirpProcessor.set_chirp_amplitude(fLtAmplitude);
            pActualDuration->set_value(scDurationSetting);

            size_t saveMode = pSaveModeSelector->value();
            if (saveMode != nSaveMode)
            {
                nSaveMode   = saveMode;
                reset_saver = true;
            }

            // Update state according to pressed triggers
            if (nTriggers & T_CALIBRATION)
            {
                reset_tasks();

                for (size_t ch = 0; ch < nChannels; ++ch)
                    vChannels[ch].sLatencyDetector.reset_capture();

                reset_saver = true;
                nState      = CALIBRATION;
            }
            else if (nTriggers & T_LIN_TRIGGER) // Allow measurement cycle to start only if not calibrating, T_CALIBRATION = 0
            {
                // Needs resets in case it was pressed while a previous cycle did not finish yet.
                reset_tasks();

                reset_saver     = true;
                nWaitCounter    = dspu::seconds_to_samples(nSampleRate, pDuration->value());

                bool bLatencyMeasured = true;
                for (size_t ch = 0; ch < nChannels; ++ch)
                    bLatencyMeasured = bLatencyMeasured && vChannels[ch].bLatencyMeasured;

                if ((nTriggers & T_SKIP_LATENCY_DETECT) && bLatencyMeasured)
                    nState = PREPROCESSING;
                else
                {
                    for (size_t ch = 0; ch < nChannels; ++ch)
                    {
                        vChannels[ch].bLatencyMeasured  = false;
                        vChannels[ch].bLCycleComplete   = false;
                        vChannels[ch].sLatencyDetector.start_capture();
                        vChannels[ch].pLatencyScreen->set_value(0.0f);
                    }

                    nState = LATENCYDETECTION;
                }
            }
            else if (nTriggers & T_LAT_TRIGGER) // Allow only if not calibrating and not measuring, T_CALIBRATION = 0, T_LIN_TRIGGER = 0
            {
                // Needs resets in case it was pressed while a previous cycle did not finish yet.
                reset_tasks();

                for (size_t ch = 0; ch < nChannels; ++ch)
                {
                    vChannels[ch].bLatencyMeasured  = false;
                    vChannels[ch].bLCycleComplete   = false;
                    vChannels[ch].sLatencyDetector.start_capture();
                    vChannels[ch].pLatencyScreen->set_value(0.0f);
                }

                reset_saver     = true;
                bDoLatencyOnly  = true;

                nWaitCounter    = dspu::seconds_to_samples(nSampleRate, pDuration->value());
                nState          = LATENCYDETECTION;
            }
            else if (nTriggers & T_POSTPROCESS) // Allow only if not calibrating and not measuring, T_CALIBRATION = 0, T_LIN_TRIGGER = 0, T_LAT_TRIGGER = 0
            {
                // Reset is done here.
                reset_tasks();

                for (size_t ch = 0; ch < nChannels; ++ch)
                    vChannels[ch].sLatencyDetector.reset_capture();

                bIRMeasured = false;
                reset_saver = true;
                nState      = POSTPROCESSING;
            }
            else if (nTriggers & T_CHANGE)
            {
                reset_tasks();

                for (size_t ch = 0; ch < nChannels; ++ch)
                    vChannels[ch].sLatencyDetector.reset_capture();

                nState      = IDLE;
            }

            // Reset all pending trigger events (mark as processed)
            nTriggers &= ~(T_CHANGE | T_LAT_TRIGGER | T_LIN_TRIGGER | T_POSTPROCESS);

            // Reset saver
            if (reset_saver)
                pIRSaveStatus->set_value(STATUS_UNSPECIFIED);

            // Update pending settings for processors
            if (sSyncChirpProcessor.needs_update())
                sSyncChirpProcessor.update_settings();
            if (sCalOscillator.needs_update())
                sCalOscillator.update_settings();

            for (size_t ch = 0; ch < nChannels; ++ch)
            {
                if (vChannels[ch].sLatencyDetector.needs_update())
                    vChannels[ch].sLatencyDetector.update_settings();
                if (vChannels[ch].sResponseTaker.needs_update())
                    vChannels[ch].sResponseTaker.update_settings();
            }
        }

        void profiler::dump(dspu::IStateDumper *v) const
        {
            plug::Module::dump(v);

            v->write("nChannels", nChannels);
            v->begin_array("vChannels", vChannels, nChannels);
            {
                for (size_t i=0; i<nChannels; ++i)
                {
                    const channel_t *c = &vChannels[i];

                    v->begin_object(c, sizeof(channel_t));
                    {
                        v->write_object("sBypass", &c->sBypass);
                        v->write_object("sLatencyDetector", &c->sLatencyDetector);
                        v->write_object("sResponseTaker", &c->sResponseTaker);

                        v->write("nLatency", c->nLatency);
                        v->write("bLatencyMeasured", c->bLatencyMeasured);
                        v->write("bLCycleComplete", c->bLCycleComplete);
                        v->write("bRCycleComplete", c->bRCycleComplete);

                        const postproc_t *pp = &c->sPostProc;
                        v->begin_object("sPostProc", pp, sizeof(postproc_t));
                        {
                            v->write("fReverbTime", pp->fReverbTime);
                            v->write("nReverbTime", pp->nReverbTime);
                            v->write("fCorrCoeff", pp->fCorrCoeff);
                            v->write("fIntgLimit", pp->fIntgLimit);
                            v->write("bRTAccuray", pp->bRTAccuray);
                        }
                        v->end_object();

                        v->write("vBuffer", c->vBuffer);
                        v->write("vIn", c->vIn);
                        v->write("vOut", c->vOut);
                        v->write("pIn", c->pIn);
                        v->write("pOut", c->pOut);
                        v->write("pLevelMeter", c->pLevelMeter);
                        v->write("pLatencyScreen", c->pLatencyScreen);
                        v->write("pRTScreen", c->pRTScreen);
                        v->write("pRTAccuracyLed", c->pRTAccuracyLed);
                        v->write("pILScreen", c->pILScreen);
                        v->write("pRScreen", c->pRScreen);
                        v->write("pResultMesh", c->pResultMesh);
                    }
                    v->end_object();
                }
            }
            v->end_array();
            v->begin_object("sResponseData", &sResponseData, sizeof(response_t));
            {
                const response_t *r = &sResponseData;
                v->begin_array("vResponses", r->vResponses, nChannels);
                {
                    for (size_t i=0; i<nChannels; ++i)
                        v->write_object(r->vResponses[i]);
                }
                v->end_array();
                v->writev(r->vOffsets, nChannels);
                v->write("pData", r->pData);
            }
            v->end_object();
            v->begin_object("sSaveData", &sSaveData, sizeof(save_t));
            {
                const save_t *s = &sSaveData;
                v->write("enSaveStatus", s->enSaveStatus);
                v->write("fSavePercent", s->fSavePercent);
            }
            v->end_object();

            v->write("nState", nState);

            v->write_object("sCalOscillator", &sCalOscillator);
            v->write_object("sSyncChirpProcessor", &sSyncChirpProcessor);

            v->write("pExecutor", pExecutor);
            v->write("pPreProcessor", pPreProcessor);
            v->write("pConvolver", pConvolver);
            v->write("pPostProcessor", pPostProcessor);
            v->write("pSaver", pSaver);

            v->write("nSampleRate", nSampleRate);
            v->write("fLtAmplitude", fLtAmplitude);
            v->write("nWaitCounter", nWaitCounter);
            v->write("bDoLatencyOnly", bDoLatencyOnly);

            v->write("fScpDurationPrevious", fScpDurationPrevious);
            v->write("bIRMeasured", bIRMeasured);
            v->write("nSaveMode", nSaveMode);

            v->write("nTriggers", nTriggers);

            v->write("vTempBuffer", vTempBuffer);
            v->write("vDisplayAbscissa", vDisplayAbscissa);
            v->write("vDisplayOrdinate", vDisplayOrdinate);
            v->write("pData", pData);

            v->write("pBypass", pBypass);
            v->write("pStateLEDs", pStateLEDs);

            v->write("pCalFrequency", pCalFrequency);
            v->write("pCalAmplitude", pCalAmplitude);
            v->write("pCalSwitch", pCalSwitch);
            v->write("pFeedback", pFeedback);

            v->write("pLdMaxLatency", pLdMaxLatency);
            v->write("pLdPeakThs", pLdPeakThs);
            v->write("pLdAbsThs", pLdAbsThs);
            v->write("pLdEnableSwitch", pLdEnableSwitch);
            v->write("pLatTrigger", pLatTrigger);

            v->write("pDuration", pDuration);
            v->write("pActualDuration", pActualDuration);
            v->write("pLinTrigger", pLinTrigger);

            v->write("pIROffset", pIROffset);
            v->write("pRTAlgoSelector", pRTAlgoSelector);
            v->write("pPostTrigger", pPostTrigger);

            v->write("pSaveModeSelector", pSaveModeSelector);
            v->write("pIRFileName", pIRFileName);
            v->write("pIRSaveCmd", pIRSaveCmd);
            v->write("pIRSaveStatus", pIRSaveStatus);
            v->write("pIRSavePercent", pIRSavePercent);
        }

    } /* namespace plugins */
} /* namespace lsp */


