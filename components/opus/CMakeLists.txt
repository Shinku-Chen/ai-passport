# libopus decoder component for the Voice Keychain.
# Built as a fixed-point decoder (ESP32-C3 has no FPU).
#
# Source list mirrors libopus' SILK_SOURCES + CELT_SOURCES (top-level only, no
# float/SSE/NEON/ARM subdirs). The full SILK set is used even though only the
# decoder is linked: opus_decoder -> celt_decoder / silk decode pipeline pulls in
# resampler tables (resampler_rom.c), bwexpander (bwexpander_32.c), and the shared
# NLSF/PLC sources. -ffunction-sections + --gc-sections drop unused encoder code.
idf_component_register(
    SRCS
        "libopus/src/opus.c"
        "libopus/src/opus_decoder.c"
        "libopus/src/opus_multistream_decoder.c"
        "libopus/src/repacketizer.c"
        # --- CELT (decoder path) ---
        "libopus/celt/bands.c"
        "libopus/celt/celt.c"
        "libopus/celt/celt_decoder.c"
        "libopus/celt/celt_lpc.c"
        "libopus/celt/cwrs.c"
        "libopus/celt/entcode.c"
        "libopus/celt/entdec.c"
        "libopus/celt/entenc.c"
        "libopus/celt/kiss_fft.c"
        "libopus/celt/laplace.c"
        "libopus/celt/mathops.c"
        "libopus/celt/mdct.c"
        "libopus/celt/modes.c"
        "libopus/celt/pitch.c"
        "libopus/celt/quant_bands.c"
        "libopus/celt/rate.c"
        "libopus/celt/vq.c"
        # --- SILK (SILK_SOURCES, encoder+decoder shared) ---
        "libopus/silk/A2NLSF.c"
        "libopus/silk/CNG.c"
        "libopus/silk/HP_variable_cutoff.c"
        "libopus/silk/LPC_analysis_filter.c"
        "libopus/silk/LPC_fit.c"
        "libopus/silk/LPC_inv_pred_gain.c"
        "libopus/silk/LP_variable_cutoff.c"
        "libopus/silk/NLSF2A.c"
        "libopus/silk/NLSF_VQ.c"
        "libopus/silk/NLSF_VQ_weights_laroia.c"
        "libopus/silk/NLSF_decode.c"
        "libopus/silk/NLSF_del_dec_quant.c"
        "libopus/silk/NLSF_encode.c"
        "libopus/silk/NLSF_stabilize.c"
        "libopus/silk/NLSF_unpack.c"
        "libopus/silk/NSQ.c"
        "libopus/silk/NSQ_del_dec.c"
        "libopus/silk/PLC.c"
        "libopus/silk/VAD.c"
        "libopus/silk/VQ_WMat_EC.c"
        "libopus/silk/ana_filt_bank_1.c"
        "libopus/silk/biquad_alt.c"
        "libopus/silk/bwexpander.c"
        "libopus/silk/bwexpander_32.c"
        "libopus/silk/check_control_input.c"
        "libopus/silk/code_signs.c"
        "libopus/silk/control_SNR.c"
        "libopus/silk/control_audio_bandwidth.c"
        "libopus/silk/control_codec.c"
        "libopus/silk/debug.c"
        "libopus/silk/dec_API.c"
        "libopus/silk/decode_core.c"
        "libopus/silk/decode_frame.c"
        "libopus/silk/decode_indices.c"
        "libopus/silk/decode_parameters.c"
        "libopus/silk/decode_pitch.c"
        "libopus/silk/decode_pulses.c"
        "libopus/silk/decoder_set_fs.c"
        "libopus/silk/enc_API.c"
        "libopus/silk/encode_indices.c"
        "libopus/silk/encode_pulses.c"
        "libopus/silk/gain_quant.c"
        "libopus/silk/init_decoder.c"
        "libopus/silk/init_encoder.c"
        "libopus/silk/inner_prod_aligned.c"
        "libopus/silk/interpolate.c"
        "libopus/silk/lin2log.c"
        "libopus/silk/log2lin.c"
        "libopus/silk/pitch_est_tables.c"
        "libopus/silk/process_NLSFs.c"
        "libopus/silk/quant_LTP_gains.c"
        "libopus/silk/resampler.c"
        "libopus/silk/resampler_down2.c"
        "libopus/silk/resampler_down2_3.c"
        "libopus/silk/resampler_private_AR2.c"
        "libopus/silk/resampler_private_IIR_FIR.c"
        "libopus/silk/resampler_private_down_FIR.c"
        "libopus/silk/resampler_private_up2_HQ.c"
        "libopus/silk/resampler_rom.c"
        "libopus/silk/shell_coder.c"
        "libopus/silk/sigm_Q15.c"
        "libopus/silk/sort.c"
        "libopus/silk/stereo_LR_to_MS.c"
        "libopus/silk/stereo_MS_to_LR.c"
        "libopus/silk/stereo_decode_pred.c"
        "libopus/silk/stereo_encode_pred.c"
        "libopus/silk/stereo_find_predictor.c"
        "libopus/silk/stereo_quant_pred.c"
        "libopus/silk/sum_sqr_shift.c"
        "libopus/silk/table_LSF_cos.c"
        "libopus/silk/tables_LTP.c"
        "libopus/silk/tables_NLSF_CB_NB_MB.c"
        "libopus/silk/tables_NLSF_CB_WB.c"
        "libopus/silk/tables_gain.c"
        "libopus/silk/tables_other.c"
        "libopus/silk/tables_pitch_lag.c"
        "libopus/silk/tables_pulses_per_block.c"
        # --- SILK fixed-point HQ encoder helpers (needed by enc_API/NSQ/control_*) ---
        # Only silk/fixed/*.c is compiled; silk/float/*.c is excluded because the
        # C3 has no FPU, and the FIXED_POINT compile-time switch already routes the
        # encoder to the fixed implementations (float symbols are never referenced).
        "libopus/silk/fixed/LTP_analysis_filter_FIX.c"
        "libopus/silk/fixed/LTP_scale_ctrl_FIX.c"
        "libopus/silk/fixed/apply_sine_window_FIX.c"
        "libopus/silk/fixed/autocorr_FIX.c"
        "libopus/silk/fixed/burg_modified_FIX.c"
        "libopus/silk/fixed/corrMatrix_FIX.c"
        "libopus/silk/fixed/encode_frame_FIX.c"
        "libopus/silk/fixed/find_LPC_FIX.c"
        "libopus/silk/fixed/find_LTP_FIX.c"
        "libopus/silk/fixed/find_pitch_lags_FIX.c"
        "libopus/silk/fixed/find_pred_coefs_FIX.c"
        "libopus/silk/fixed/k2a_FIX.c"
        "libopus/silk/fixed/k2a_Q16_FIX.c"
        "libopus/silk/fixed/noise_shape_analysis_FIX.c"
        "libopus/silk/fixed/pitch_analysis_core_FIX.c"
        "libopus/silk/fixed/process_gains_FIX.c"
        "libopus/silk/fixed/regularize_correlations_FIX.c"
        "libopus/silk/fixed/residual_energy16_FIX.c"
        "libopus/silk/fixed/residual_energy_FIX.c"
        "libopus/silk/fixed/schur64_FIX.c"
        "libopus/silk/fixed/schur_FIX.c"
        "libopus/silk/fixed/vector_ops_FIX.c"
        "libopus/silk/fixed/warped_autocorrelation_FIX.c"
    INCLUDE_DIRS "libopus/include"
    REQUIRES freertos
)

# Fixed-point build for the no-FPU ESP32-C3. These flags match the feasibility
# cross-compile test (57 files, 0 failures).
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    OPUS_BUILD
    FIXED_POINT
    VAR_ARRAYS
    OPUS_EXPORT=
)
# 说明: 用 VAR_ARRAYS(任务栈 VLA) 而非 NONTHREADSAFE_PSEUDOSTACK(堆上 alloc 120KB
#  伪栈, ESP32-C3 空闲堆仅 ~68KB 会 malloc 失败导致 SILK 解码崩溃)。VLA 的栈需求
#  由 player_task 的 16KB 栈承载。
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-parameter
    -Wno-error
    -Wno-error=sizeof-array-div
    -Wno-error=stringop-overread
    -Wno-error=array-bounds
    -ffunction-sections
    -fdata-sections
)
target_include_directories(${COMPONENT_LIB} PRIVATE
    "libopus/include"
    "libopus/src"
    "libopus/celt"
    "libopus/silk"
    "libopus/silk/fixed"
    "libopus/silk/float"
)
