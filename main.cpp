#include <iostream>
#include <vector>
#include <string>
#include <mutex>
#include <unordered_map>
#include <memory>
#include <cstring>
#include <random>
#include <thread>
#include <algorithm>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <getopt.h>

#include <opus/opus.h>
#include "audio_processing.h"
#include "webrtc_vad.h"
#include "SileroVadDetector.hpp"

using namespace webrtc;

// --- 配置参数与全局变量 ---
static constexpr int kSampleRate = 16000;
static constexpr int kFrameSize = 160; 
static constexpr int SESSION_TIMEOUT_SEC = 60; 
static constexpr int STT_PORT = 9000;

enum class VadMode { kSilero = 0, kWebRTC = 1 };

// 全局模式设定（由 main 初始化后不再更改）
VadMode g_vad_mode = VadMode::kSilero; 
std::string g_model_path = "./silero_vad.onnx";
int g_sockfd;

void LOG_INFO(const std::string& msg) {
    time_t now = time(nullptr);
    char timestamp[20];
    strftime(timestamp, sizeof(timestamp), "%H:%M:%S", localtime(&now));
    std::cout << "[" << timestamp << "] " << msg << std::endl;
}

std::string generate_uuid() {
    static const char* chars = "0123456789abcdef";
    std::string uuid;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    for (int i = 0; i < 32; ++i) uuid += chars[dis(gen)];
    return uuid;
}

// --- Session 类定义 ---
class AudioSession {
public:
    std::string session_id;
    struct sockaddr_in addr; 
    OpusDecoder* decoder;
    rtc::scoped_refptr<AudioProcessing> apm;
    
    // VAD 资源分支
    VadMode mode;
    VadInst* webrtc_vad_inst = nullptr;
    std::unique_ptr<SileroVadDetector> silero_vad_detector;
    std::vector<float> pcm_buffer; // 仅用于 Silero 攒包

    bool is_speaking = false;    
    bool stt_started = false;   
    int silence_frames = 0;     
    time_t last_active_time;

    AudioSession(VadMode m) : mode(m) {
        session_id = generate_uuid();
        int err;
        decoder = opus_decoder_create(kSampleRate, 1, &err);
        
        apm = AudioProcessingBuilder().Create();
        AudioProcessing::Config apm_config;
        apm_config.echo_canceller.enabled = true;
        apm_config.noise_suppression.enabled = true;
        apm->ApplyConfig(apm_config);

        if (mode == VadMode::kWebRTC) {
            webrtc_vad_inst = WebRtcVad_Create();
            WebRtcVad_Init(webrtc_vad_inst);
            WebRtcVad_set_mode(webrtc_vad_inst, 3);
        } else {
            SileroVadDetector::Config vad_cfg;
            vad_cfg.model_path = g_model_path;
            vad_cfg.threshold = 0.5f;
            silero_vad_detector = std::make_unique<SileroVadDetector>(vad_cfg);
            pcm_buffer.reserve(512);
        }
        last_active_time = time(nullptr);
    }

    ~AudioSession() {
        if (decoder) opus_decoder_destroy(decoder);
        if (webrtc_vad_inst) WebRtcVad_Free(webrtc_vad_inst);
        std::cout << "[Session] 释放 ID: " << session_id << std::endl;
    }
};

std::mutex g_session_mu;
std::unordered_map<std::string, std::shared_ptr<AudioSession>> g_sessions; 
std::unordered_map<std::string, std::shared_ptr<AudioSession>> g_id_map;   

void send_to_stt(const std::string& sid, const void* data, size_t len) {
    static struct sockaddr_in stt_addr{};
    static bool init = false;
    if (!init) {
        stt_addr.sin_family = AF_INET;
        stt_addr.sin_port = htons(STT_PORT);
        inet_pton(AF_INET, "127.0.0.1", &stt_addr.sin_addr);
        init = true;
    }
    std::vector<uint8_t> buf(32 + len);
    std::memcpy(buf.data(), sid.c_str(), 32);
    std::memcpy(buf.data() + 32, data, len);
    sendto(g_sockfd, (const char*)buf.data(), buf.size(), 0, (struct sockaddr*)&stt_addr, sizeof(stt_addr));
}

// --- 核心逻辑：VAD 状态机 ---
void handle_vad_logic(std::shared_ptr<AudioSession> session, bool is_voice, int16_t* out_pcm, int silence_limit) {
    if (is_voice) {
        if (!session->stt_started) {
            send_to_stt(session->session_id, "start", 5);
            session->stt_started = true;
            LOG_INFO("[VAD] 会话开始: " + session->session_id);
        }
        session->is_speaking = true;
        session->silence_frames = 0;
    } else if (session->is_speaking) {
        if (++session->silence_frames >= silence_limit) {
            send_to_stt(session->session_id, "end", 3);
            session->stt_started = false;
            session->is_speaking = false;
            LOG_INFO("[VAD] 会话断句: " + session->session_id);
        }
    }
    // 开启 STT 后，每 10ms 实时转发
    if (session->stt_started) {
        send_to_stt(session->session_id, out_pcm, kFrameSize * sizeof(int16_t));
    }
}

void receiver_processor_thread() {
    uint8_t buffer[8192];
    struct sockaddr_in cli_addr{};
    socklen_t cli_len = sizeof(cli_addr);
    StreamConfig s_conf(kSampleRate, 1);

    while (true) {
        ssize_t nbytes = recvfrom(g_sockfd, (char*)buffer, sizeof(buffer), 0, (sockaddr*)&cli_addr, &cli_len);
        if (nbytes < 4) continue;

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cli_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
        std::string addr_key = std::string(ip_str) + ":" + std::to_string(ntohs(cli_addr.sin_port));
        
        std::shared_ptr<AudioSession> session;
        {
            std::lock_guard<std::mutex> lock(g_session_mu);
            auto it = g_sessions.find(addr_key);
            if (it == g_sessions.end()) {
                // 统一应用启动时设定的 g_vad_mode
                session = std::make_shared<AudioSession>(g_vad_mode);
                session->addr = cli_addr;
                g_sessions[addr_key] = session;
                g_id_map[session->session_id] = session;
                std::cout << "[New Session] " << addr_key << " ID: " << session->session_id << std::endl;
            } else {
                session = it->second;
            }
            session->last_active_time = time(nullptr);
        }

        uint16_t near_len = (buffer[0] << 8) | buffer[1];
        int16_t near_pcm[kFrameSize], ref_pcm[kFrameSize], out_pcm[kFrameSize];
        if (opus_decode(session->decoder, buffer + 2, near_len, near_pcm, kFrameSize, 0) < 0) continue;

        // 处理回声参考帧逻辑省略...
        std::memset(ref_pcm, 0, sizeof(ref_pcm)); 

        session->apm->ProcessReverseStream(ref_pcm, s_conf, s_conf, nullptr);
        session->apm->ProcessStream(near_pcm, s_conf, s_conf, out_pcm);

        if (session->mode == VadMode::kWebRTC) {
            // WebRTC 模式: 每 10ms 判定一次
            bool is_voice = WebRtcVad_Process(session->webrtc_vad_inst, kSampleRate, out_pcm, kFrameSize) == 1;
            handle_vad_logic(session, is_voice, out_pcm, 50); 
        } else {
            // Silero 模式: 攒包 512 点判定一次
            for (int i = 0; i < kFrameSize; ++i) 
                session->pcm_buffer.push_back(static_cast<float>(out_pcm[i]) / 32768.0f);

            if (session->pcm_buffer.size() >= 512) {
                bool is_voice = session->silero_vad_detector->is_speech(session->pcm_buffer);
                session->pcm_buffer.clear();
                handle_vad_logic(session, is_voice, out_pcm, 30); 
            } else if (session->stt_started) {
                // 没攒够包时，如果已开始识别，也要转发 10ms 帧
                send_to_stt(session->session_id, out_pcm, kFrameSize * sizeof(int16_t));
            }
        }
    }
}

void ai_response_thread() {

    int ai_sock = socket(AF_INET, SOCK_DGRAM, 0);

    struct sockaddr_in ai_addr{};

    ai_addr.sin_family = AF_INET;

    ai_addr.sin_port = htons(8001);

    ai_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(ai_sock, (struct sockaddr*)&ai_addr, sizeof(ai_addr)) < 0) return;



    char buf[4096];

    while (true) {

        ssize_t n = recvfrom(ai_sock, buf, sizeof(buf), 0, nullptr, nullptr);

        if (n < 32) continue;

        std::string sid(buf, 32);

        std::string text(buf + 32, n - 32);



        std::lock_guard<std::mutex> lock(g_session_mu);

        if (g_id_map.count(sid)) {

            auto session = g_id_map[sid];

            std::cout<<text.c_str()<<std::endl;
            sendto(g_sockfd, text.c_str(), text.size(), 0, (struct sockaddr*)&session->addr, sizeof(session->addr));

        }

    }

}

void session_cleaner_thread() {

    while (true) {

        std::this_thread::sleep_for(std::chrono::seconds(20));

        time_t now = time(nullptr);

        std::lock_guard<std::mutex> lock(g_session_mu);

        for (auto it = g_sessions.begin(); it != g_sessions.end(); ) {

            if (now - it->second->last_active_time > SESSION_TIMEOUT_SEC) {

                g_id_map.erase(it->second->session_id);

                it = g_sessions.erase(it);

            } else ++it;

        }

    }

}

int main(int argc, char* argv[]) {
    int opt;
    // 使用 -v 指定模式 (0: Silero, 1: WebRTC), -m 指定模型路径
    while ((opt = getopt(argc, argv, "v:m:h")) != -1) {
        switch (opt) {
            case 'v':
                g_vad_mode = (std::stoi(optarg) == 1) ? VadMode::kWebRTC : VadMode::kSilero;
                break;
            case 'm':
                g_model_path = optarg;
                break;
            case 'h':
                std::cout << "Usage: " << argv[0] << " -v [0|1] -m [model_path]" << std::endl;
                return 0;
        }
    }

    g_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(8000);
    if (bind(g_sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Bind failed");
        return -1;
    }

    std::thread(receiver_processor_thread).detach();
    std::thread(session_cleaner_thread).detach();
   // ai_response_thread 请自行根据您的 socket 需求补全
    std::thread(ai_response_thread).detach();

    std::cout << ">>> 网关启动, VAD 模式: " << (g_vad_mode == VadMode::kWebRTC ? "WebRTC" : "Silero") << " <<<" << std::endl;
    if (g_vad_mode == VadMode::kSilero) std::cout << ">>> 模型路径: " << g_model_path << std::endl;

    while (true) std::this_thread::sleep_for(std::chrono::minutes(1));
    return 0;
}
