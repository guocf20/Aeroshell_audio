#pragma once

#include <vector>
#include <string>
#include <memory>
#include <cstring>

#include <onnxruntime_cxx_api.h>

/**
 * Silero VAD 推理引擎（全局单实例）
 *
 * 设计原则：
 * 1. Ort::Env / Ort::Session / Ort::MemoryInfo 只创建一次（重资源）
 * 2. 不保存任何会话状态（RNN state 由调用方维护）
 * 3. is_speech 可被多个 session 调用
 */
class SileroVadDetector {
public:
    struct Config {
        std::string model_path;
        int   sample_rate      = 16000;
        float threshold        = 0.5f;
        int   intra_op_threads = 1;
    };

    explicit SileroVadDetector(const Config& config)
        : config_(config),
          sr_val_(static_cast<int64_t>(config.sample_rate))
    {
        // ---------- Session options ----------
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(config_.intra_op_threads);
        opts.SetInterOpNumThreads(1);
        opts.SetGraphOptimizationLevel(
            GraphOptimizationLevel::ORT_ENABLE_ALL);

        // ---------- ORT Env ----------
        env_ = std::make_unique<Ort::Env>(
            ORT_LOGGING_LEVEL_WARNING,
            "SileroVAD");

        // ---------- ORT Session ----------
        session_ = std::make_unique<Ort::Session>(
            *env_,
            config_.model_path.c_str(),
            opts);

        // ---------- CPU MemoryInfo ----------
        // ⚠️ Ort::MemoryInfo 没有默认构造函数，必须这样创建
        memory_info_ = std::make_unique<Ort::MemoryInfo>(
            Ort::MemoryInfo::CreateCpu(
                OrtArenaAllocator,
                OrtMemTypeDefault));
    }

    /**
     * @brief 执行一次 VAD 推理
     *
     * @param pcm_float  归一化音频数据（-1 ~ 1），通常 512 samples
     * @param state      会话独立的 RNN hidden state，尺寸必须是 [2,1,128]
     *
     * @return true  : speech
     *         false : silence
     */
    bool is_speech(const std::vector<float>& pcm_float,
                   std::vector<float>& state)
    {
        // ----------- Tensor dims -----------
        int64_t input_dims[] = {1, static_cast<int64_t>(pcm_float.size())};
        int64_t sr_dims[]    = {1};
        int64_t state_dims[] = {2, 1, 128};

        // ----------- I/O names -----------
        const char* input_names[]  = {"input", "sr", "state"};
        const char* output_names[] = {"output", "stateN"};

        // ----------- Build input tensors -----------
        std::vector<Ort::Value> inputs;
        inputs.reserve(3);

        // audio input
        inputs.emplace_back(
            Ort::Value::CreateTensor<float>(
                *memory_info_,
                const_cast<float*>(pcm_float.data()),
                pcm_float.size(),
                input_dims,
                2));

        // sample rate
        inputs.emplace_back(
            Ort::Value::CreateTensor<int64_t>(
                *memory_info_,
                &sr_val_,
                1,
                sr_dims,
                1));

        // RNN state
        inputs.emplace_back(
            Ort::Value::CreateTensor<float>(
                *memory_info_,
                state.data(),
                state.size(),
                state_dims,
                3));

        // ----------- Run inference -----------
        auto outputs = session_->Run(
            Ort::RunOptions{nullptr},
            input_names,
            inputs.data(),
            inputs.size(),
            output_names,
            2);

        // ----------- Update RNN state -----------
        float* next_state =
            outputs[1].GetTensorMutableData<float>();

        std::memcpy(
            state.data(),
            next_state,
            state.size() * sizeof(float));

        // ----------- Read score -----------
        float score =
            outputs[0].GetTensorMutableData<float>()[0];

        return score >= config_.threshold;
    }

private:
    Config config_;

    // ORT heavy objects（全局只一份）
    std::unique_ptr<Ort::Env>        env_;
    std::unique_ptr<Ort::Session>    session_;
    std::unique_ptr<Ort::MemoryInfo> memory_info_;

    int64_t sr_val_;
};
