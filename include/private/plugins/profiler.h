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

#ifndef PRIVATE_PLUGINS_PROFILER_H_
#define PRIVATE_PLUGINS_PROFILER_H_

#include <lsp-plug.in/plug-fw/plug.h>
#include <lsp-plug.in/dsp-units/ctl/Bypass.h>
#include <lsp-plug.in/dsp-units/sampling/Sample.h>
#include <lsp-plug.in/dsp-units/util/LatencyDetector.h>
#include <lsp-plug.in/dsp-units/util/Oscillator.h>
#include <lsp-plug.in/dsp-units/util/ResponseTaker.h>
#include <lsp-plug.in/dsp-units/util/SyncChirpProcessor.h>


#include <private/meta/profiler.h>

namespace lsp
{
    namespace plugins
    {
        class profiler: public plug::Module
        {
            protected:
                // Class to handle profiling time series generation task
                class PreProcessor: public ipc::ITask
                {
                    private:
                        profiler *pCore;

                    public:
                        explicit PreProcessor(profiler *base);
                        virtual ~PreProcessor();

                    public:
                        virtual status_t run();
                };

                // Task to handle generation of the convolution result
                class Convolver: public ipc::ITask
                {
                    private:
                        profiler *pCore;

                    public:
                        explicit Convolver(profiler *base);
                        virtual ~Convolver();

                    public:
                        virtual status_t run();
                };

                // Class to handle post processing of the convolution result
                class PostProcessor: public ipc::ITask
                {
                    private:
                        profiler           *pCore;
                        ssize_t             nIROffset;
                        dspu::scp_rtcalc_t  enAlgo;

                    public:
                        explicit PostProcessor(profiler *base);
                        virtual ~PostProcessor();

                    public:
                        void set_ir_offset(ssize_t ir_offset);
                        inline ssize_t get_ir_offset() const { return nIROffset; }

                        void set_rt_algo(dspu::scp_rtcalc_t algo);

                        virtual status_t run();
                };

                // Class to handle saving of the convolution result
                class Saver: public ipc::ITask
                {
                    private:
                        profiler       *pCore;
                        ssize_t         nIROffset;
                        char            sFile[PATH_MAX]; // The name of file for saving

                    public:
                        explicit Saver(profiler *base);
                        virtual ~Saver();

                    public:
                        void set_file_name(const char *fname);
                        void set_ir_offset(ssize_t ir_offset);
                        inline ssize_t get_ir_offset() const { return nIROffset; }

                        bool is_file_set() const;

                        virtual status_t run();
                };

                // Object state descriptor
                enum state_t
                {
                    IDLE,                           // Realtime: doing nothing, awaiting for command
                    CALIBRATION,                    // Realtime: callibrating device
                    LATENCYDETECTION,               // Realtime: detecting loopback latency
                    PREPROCESSING,                  // Offline: PreProcessor task
                    WAIT,                           // Realtime: waiting for signal fall-off
                    RECORDING,                      // Realtime: recording response
                    CONVOLVING,                     // Offline: Convolver task
                    POSTPROCESSING,                 // Offline: PostProcessor task
                    SAVING                          // Offline: Saver task
                };

                enum triggers_t
                {
                    T_CHANGE                = 1 << 0, // Change of any following trigger below:

                    T_CALIBRATION           = 1 << 1, // Calibration switch is pressed on
                    T_SKIP_LATENCY_DETECT   = 1 << 2, // Latency detection switch is pressed on
                    T_POSTPROCESS           = 1 << 3, // Post-process switch is pressed on
                    T_POSTPROCESS_STATE     = 1 << 4, // Current post-process switch state
                    T_LAT_TRIGGER           = 1 << 5, // Latency measurement trigger was pressed
                    T_LAT_TRIGGER_STATE     = 1 << 6, // Latency measurement trigger state
                    T_LIN_TRIGGER           = 1 << 7, // Linear measurement trigger is pressed
                    T_LIN_TRIGGER_STATE     = 1 << 8, // Linear measurement trigger state
                    T_FEEDBACK              = 1 << 9  // feedback break switch is pressed on
                };

                typedef struct postproc_t
                {
                    float                   fReverbTime;            // Reverberation time [seconds]
                    size_t                  nReverbTime;            // Reverberation time [samples]
                    float                   fCorrCoeff;             // Energy decay correlation coefficient
                    float                   fIntgLimit;             // IR intgration limit [seconds]
                    bool                    bRTAccuray;             // If true, dynamic range and bacjground noise are optimal for RT accuracy.
                } posproc_t;

                typedef struct channel_t
                {
                    dspu::Bypass            sBypass;
                    dspu::LatencyDetector   sLatencyDetector;       // For latency assessment
                    dspu::ResponseTaker     sResponseTaker;         // To take response of system after Synch Chirp stimulation

                    size_t                  nLatency;               // Store latency value
                    bool                    bLatencyMeasured;       // If true, a latency measurement was performed
                    bool                    bLCycleComplete;        // If true, a latency measurement cycle was finished
                    bool                    bRCycleComplete;        // If true, a chirp response recording cycle was finished.

                    postproc_t              sPostProc;              // Holds IR postproc info.

                    float                  *vBuffer;                // Auxiliary processing buffer

                    float                  *vIn;                    // Input buffer binding
                    float                  *vOut;                   // Output buffer binding

                    plug::IPort            *pIn;
                    plug::IPort            *pOut;

                    plug::IPort            *pLevelMeter;            // dB Input Level Meter
                    plug::IPort            *pLatencyScreen;         // Little screen displaying latency value
                    plug::IPort            *pRTScreen;              // Little screen displaying RT value
                    plug::IPort            *pRTAccuracyLed;         // Led to show whether the RT measurement can be considered accurate
                    plug::IPort            *pILScreen;              // Little screen displaying IL (integration limit) value
                    plug::IPort            *pRScreen;               // Little screen displaying R (RT regression line correlation coefficient) value
                    plug::IPort            *pResultMesh;            // Mesh for result plot
                } channel_t;

                typedef struct response_t
                {
                    dspu::Sample          **vResponses;
                    size_t                 *vOffsets;
                    uint8_t                *pData;
                } response_t;

                typedef struct save_t
                {
                    status_codes            enSaveStatus;
                    float                   fSavePercent;
                } save_t;

            protected:
                size_t                      nChannels;
                channel_t                  *vChannels;

                response_t                  sResponseData;
                save_t                      sSaveData;
                state_t                     nState;                 // Object State

                dspu::Oscillator            sCalOscillator;         // For calibration

                dspu::SyncChirpProcessor    sSyncChirpProcessor;    // To handle Synch Chirp profiling signal and related operations

                ipc::IExecutor             *pExecutor;              // Executor Service
                PreProcessor               *pPreProcessor;          // Pre Processor Task
                Convolver                  *pConvolver;             // Convolver Task
                PostProcessor              *pPostProcessor;         // Post Processor Task
                Saver                      *pSaver;                 // Saver Task

                size_t                      nSampleRate;            // Sample Rate
                float                       fLtAmplitude;           // Amplitude factor for Latency Detection chirp
                ssize_t                     nWaitCounter;           // Count the samples for wait state
                bool                        bDoLatencyOnly;         // If true, only latency is measured

                float                       fScpDurationPrevious;   // Store Sync Chirp Duration Setting between calls to update_settings()
                bool                        bIRMeasured;            // If true, an IR measurement was performed and post processed
                size_t                      nSaveMode;              // Hold save mode enumeration index

                size_t                      nTriggers;              // Set of triggers controlled by triggers_t

                float                      *vTempBuffer;            // Additional auxiliary buffer for processing
                float                      *vDisplayAbscissa;       // Buffer for display. Abscissa data
                float                      *vDisplayOrdinate;       // Buffer for display. Ordinate data
                uint8_t                    *pData;

                plug::IPort                *pBypass;
                plug::IPort                *pStateLEDs;             // State LEDs

                plug::IPort                *pCalFrequency;          // Calibration wave frequency
                plug::IPort                *pCalAmplitude;          // Calibration wave amplitude
                plug::IPort                *pCalSwitch;             // Calibration wave switch
                plug::IPort                *pFeedback;              // Switch to open feedback loop

                plug::IPort                *pLdMaxLatency;          // Latency Detector Max expected latency
                plug::IPort                *pLdPeakThs;             // Latency Detector Peak Threshold
                plug::IPort                *pLdAbsThs;              // Latency Detector Absolute Threshold
                plug::IPort                *pLdEnableSwitch;        // Switch to enable LATENCYDETECTION phase in measurement (if possible)
                plug::IPort                *pLatTrigger;            // Trigger for a latency measurement

                plug::IPort                *pDuration;              // Profiling Sync Chirp Duration
                plug::IPort                *pActualDuration;        // Actual Sync Chirp Duration after optimisation
                plug::IPort                *pLinTrigger;            // Trigger for linear system measurement

                plug::IPort                *pIROffset;              // Offset of the measured convolution result, for plot and export
                plug::IPort                *pRTAlgoSelector;        // Selector for RT calculation algorithm
                plug::IPort                *pPostTrigger;           // Trigger for post processing

                plug::IPort                *pSaveModeSelector;      // Selector for Save Mode
                plug::IPort                *pIRFileName;            // File name for IR file
                plug::IPort                *pIRSaveCmd;             // Command to save IR file
                plug::IPort                *pIRSaveStatus;          // IR file saving status
                plug::IPort                *pIRSavePercent;         // IR file saving percent

            protected:
                static dspu::scp_rtcalc_t   get_rt_algorithm(size_t algorithm);

            protected:
                void                        update_pre_processing_info();
                void                        commit_state_change();
                void                        reset_tasks();
                bool                        update_post_processing_info();
                void                        update_saving_info();
                void                        process_buffer(size_t to_do);
                void                        do_destroy();

            public:
                explicit profiler(const meta::plugin_t *metadata, size_t channels);
                virtual ~profiler() override;

                virtual void        init(plug::IWrapper *wrapper, plug::IPort **ports) override;
                virtual void        destroy() override;

            public:
                virtual void        update_settings() override;
                virtual void        update_sample_rate(long sr) override;

                virtual void        process(size_t samples) override;
                virtual void        dump(dspu::IStateDumper *v) const override;
        };
    } /* namespace plugins */
} /* namespace lsp */

#endif /* PRIVATE_PLUGINS_PROFILER_H_ */
