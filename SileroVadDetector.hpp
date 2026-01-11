#include <vector>
#include <string>
#include <memory>
#include <cstring>
#include <onnxruntime_cxx_api.h>

class SileroVadDetector {
public:
    struct Config {
        std::string model_path;
        int sample_rate = 16000;
        float threshold = 0.5f;     // 判定阈值
        int intra_op_threads = 1;
    };

    SileroVadDetector(const Config& config) : config_(config),memory_info_(nullptr) {
        Ort::SessionOptions session_options;
        session_options.SetIntraOpNumThreads(config_.intra_op_threads);
        session_options.SetInterOpNumThreads(1);
        session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "SileroVAD");
        session_ = std::make_unique<Ort::Session>(*env_, config_.model_path.c_str(), session_options);
        memory_info_ = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        // 初始化状态 [2, 1, 128]
        state_.assign(1 * 2 * 1 * 128, 0.0f);
        sr_val_ = (int64_t)config_.sample_rate;
    }

    /**
     * @brief 核心识别接口
     * @param pcm_float 归一化后的音频数据 (-1.0 ~ 1.0)
     * @return true 代表检测到语音 (Speech), false 代表静音 (Silence)
     */
    bool is_speech(const std::vector<float>& pcm_float) {
        int64_t input_node_dims[] = {1, (int64_t)pcm_float.size()};
        int64_t sr_node_dims[] = {1};
        int64_t state_node_dims[] = {2, 1, 128};

        const char* input_names[] = {"input", "sr", "state"};
        const char* output_names[] = {"output", "stateN"};

        std::vector<Ort::Value> input_tensors;
        input_tensors.push_back(Ort::Value::CreateTensor<float>(memory_info_, const_cast<float*>(pcm_float.data()), pcm_float.size(), input_node_dims, 2));
        input_tensors.push_back(Ort::Value::CreateTensor<int64_t>(memory_info_, &sr_val_, 1, sr_node_dims, 1));
        input_tensors.push_back(Ort::Value::CreateTensor<float>(memory_info_, state_.data(), state_.size(), state_node_dims, 3));

        auto output_tensors = session_->Run(Ort::RunOptions{nullptr}, input_names, input_tensors.data(), 3, output_names, 2);

        // 更新状态
        float* next_state_ptr = output_tensors[1].GetTensorMutableData<float>();
        std::memcpy(state_.data(), next_state_ptr, state_.size() * sizeof(float));

        // 获取得分
        float score = output_tensors[0].GetTensorMutableData<float>()[0];
        return score >= config_.threshold;
    }

    // 重置状态（用于切换不同音频流时调用）
    void reset() {
        std::fill(state_.begin(), state_.end(), 0.0f);
    }

private:
    Config config_;
    std::unique_ptr<Ort::Env> env_;
    std::unique_ptr<Ort::Session> session_;
    Ort::MemoryInfo memory_info_;
    std::vector<float> state_;
    int64_t sr_val_;
};