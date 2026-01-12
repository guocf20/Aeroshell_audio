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
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>


#include "audio_processing.h"

#include "webrtc_vad.h"
#include "SileroVadDetector.hpp"
#include "ten_vad.h"

using namespace webrtc;

/* ================= 日志宏 ================= */

#define LOGI(...) spdlog::info(__VA_ARGS__)
#define LOGW(...) spdlog::warn(__VA_ARGS__)
#define LOGE(...) spdlog::error(__VA_ARGS__)

/* ================= 配置 ================= */

static constexpr int kSampleRate = 16000;
static constexpr int kFrameSize  = 160;

static constexpr int SESSION_UDP_TIMEOUT_SEC    = 30;
static constexpr int SESSION_SPEECH_TIMEOUT_SEC = 120;
static constexpr int STT_PORT = 9000;

std::unique_ptr<SileroVadDetector> g_silero_vad;

enum class VadMode {
    kSilero  = 0,
    kWebRTC = 1,
    kTenVad = 2
};


VadMode g_vad_mode = VadMode::kSilero;
std::string g_model_path = "./silero_vad.onnx";
int g_sockfd;

/* ================= 工具 ================= */

std::string generate_uuid() {
    static const char* chars = "0123456789abcdef";
    std::string uuid;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    for (int i = 0; i < 32; ++i) uuid += chars[dis(gen)];
    return uuid;
}

/* ================= Session ================= */

class AudioSession {
public:
    std::string session_id;
    sockaddr_in addr{};

    OpusDecoder* decoder = nullptr;
    rtc::scoped_refptr<AudioProcessing> apm;

  
    VadMode mode;
    //webrtc vad
    VadInst* webrtc_vad_inst = nullptr;

    // Silero VAD 会话状态（RNN hidden state）
    std::vector<float> silero_state;

    //ten vad
   ten_vad_handle_t ten_vad = nullptr;
    
    std::vector<float> pcm_buffer;

    bool is_speaking   = false;
    bool stt_started   = false;
    int  silence_frames = 0;

    time_t last_active_time = 0;
    time_t last_speech_time = 0;

    explicit AudioSession(VadMode m) : mode(m) {
        session_id = generate_uuid();

        int err = 0;
        decoder = opus_decoder_create(kSampleRate, 1, &err);

        apm = AudioProcessingBuilder().Create();
        AudioProcessing::Config cfg;
        cfg.echo_canceller.enabled = true;
        cfg.noise_suppression.enabled = true;
        apm->ApplyConfig(cfg);

        if (mode == VadMode::kWebRTC) {
            webrtc_vad_inst = WebRtcVad_Create();
            WebRtcVad_Init(webrtc_vad_inst);
            WebRtcVad_set_mode(webrtc_vad_inst, 3);
        }else if (mode == VadMode::kSilero) {
    silero_state.assign(2 * 1 * 128, 0.0f); // 必须
    pcm_buffer.reserve(512);
}
        else if (mode == VadMode::kTenVad) {

size_t hop_size = kFrameSize;   // 160 samples = 10ms @ 16kHz
float threshold = 0.5f;

if (ten_vad_create(&ten_vad, hop_size, threshold) != 0) {
    LOGE("[TenVAD] create failed");
    ten_vad = nullptr;
} else {
    LOGI("[TenVAD] initialized (hop={}, th={})", hop_size, threshold);
}

}

        last_active_time = time(nullptr);
        last_speech_time = last_active_time;
    }

    ~AudioSession() {
        if (decoder) opus_decoder_destroy(decoder);
        if (webrtc_vad_inst) WebRtcVad_Free(webrtc_vad_inst);
   if (ten_vad) {
    ten_vad_destroy(&ten_vad);
    ten_vad = nullptr;
}
        LOGI("[Session] destroyed {}", session_id);
        
    }
};

/* ================= 全局会话表 ================= */

std::mutex g_session_mu;
std::unordered_map<std::string, std::shared_ptr<AudioSession>> g_sessions;
std::unordered_map<std::string, std::shared_ptr<AudioSession>> g_id_map;

/* ================= UDP → STT ================= */

void send_to_stt(const std::string& sid, const void* data, size_t len) {
    static sockaddr_in stt_addr{};
    static bool init = false;

    if (!init) {
        stt_addr.sin_family = AF_INET;
        stt_addr.sin_port   = htons(STT_PORT);
        inet_pton(AF_INET, "127.0.0.1", &stt_addr.sin_addr);
        init = true;
    }

    std::vector<uint8_t> buf(32 + len);
    memcpy(buf.data(), sid.c_str(), 32);
    memcpy(buf.data() + 32, data, len);

    sendto(g_sockfd, buf.data(), buf.size(), 0,
           (sockaddr*)&stt_addr, sizeof(stt_addr));
}

/* ================= VAD 状态机 ================= */

void handle_vad_logic(
    const std::shared_ptr<AudioSession>& s,
    bool is_voice,
    int16_t* pcm,
    int silence_limit
) {
    if (is_voice) {
        s->last_speech_time = time(nullptr);

        if (!s->stt_started) {
            send_to_stt(s->session_id, "start", 5);
            s->stt_started = true;
            LOGI("[VAD] start {}", s->session_id);
        }

        s->is_speaking = true;
        s->silence_frames = 0;
    }
    else if (s->is_speaking) {
        if (++s->silence_frames >= silence_limit) {
            send_to_stt(s->session_id, "end", 3);
            s->stt_started = false;
            s->is_speaking = false;
            LOGI("[VAD] end {}", s->session_id);
        }
    }

    if (s->stt_started) {
        send_to_stt(
            s->session_id,
            pcm,
            kFrameSize * sizeof(int16_t)
        );
    }
}

/* ================= 接收线程 ================= */

void receiver_processor_thread() {
    uint8_t buffer[8192];
    sockaddr_in cli_addr{};
    socklen_t cli_len = sizeof(cli_addr);

    StreamConfig sconf(kSampleRate, 1);

    while (true) {
        ssize_t n = recvfrom(
            g_sockfd,
            buffer,
            sizeof(buffer),
            0,
            (sockaddr*)&cli_addr,
            &cli_len
        );
        if (n < 4) {
            continue;
        }

        /* ---------- 会话 key ---------- */
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cli_addr.sin_addr, ip, sizeof(ip));
        std::string key =
            std::string(ip) + ":" + std::to_string(ntohs(cli_addr.sin_port));

        std::shared_ptr<AudioSession> sess;

        /* ---------- 会话获取 / 创建 ---------- */
        {
            std::lock_guard<std::mutex> lk(g_session_mu);

            auto it = g_sessions.find(key);
            if (it == g_sessions.end()) {
                sess = std::make_shared<AudioSession>(g_vad_mode);
                sess->addr = cli_addr;

                g_sessions[key] = sess;
                g_id_map[sess->session_id] = sess;

                LOGI("New session {} {}", key, sess->session_id);
            } else {
                sess = it->second;
            }

            sess->last_active_time = time(nullptr);
        }

        /* ---------- Opus 解码 ---------- */
        uint16_t len = (buffer[0] << 8) | buffer[1];

        int16_t near[kFrameSize];
        int16_t ref[kFrameSize];
        int16_t out[kFrameSize];

        if (opus_decode(
                sess->decoder,
                buffer + 2,
                len,
                near,
                kFrameSize,
                0) < 0)
        {
            continue;
        }

        /* ---------- AEC + NS ---------- */
        memset(ref, 0, sizeof(ref));
        sess->apm->ProcessReverseStream(ref, sconf, sconf, nullptr);
        sess->apm->ProcessStream(near, sconf, sconf, out);

        /* ---------- VAD 分发 ---------- */
        if (sess->mode == VadMode::kWebRTC) {

            bool is_voice =
                (WebRtcVad_Process(
                     sess->webrtc_vad_inst,
                     kSampleRate,
                     out,
                     kFrameSize) == 1);

            handle_vad_logic(sess, is_voice, out, 50);
        }
        else if (sess->mode == VadMode::kSilero) {

    // 1️⃣ int16 → float，累计到 512 samples
    for (int i = 0; i < kFrameSize; ++i) {
        sess->pcm_buffer.push_back(out[i] / 32768.0f);
    }

    bool is_voice = false;

    // 2️⃣ Silero 固定 512 window
    if (sess->pcm_buffer.size() >= 512) {

        is_voice = g_silero_vad->is_speech(
            sess->pcm_buffer,
            sess->silero_state   // 每 session 独立 RNN state
        );

        sess->pcm_buffer.clear();

        // 3️⃣ 进入统一 VAD 状态机
        handle_vad_logic(sess, is_voice, out, 30);
    }
    else if (sess->stt_started) {
        // 4️⃣ 未满窗但已在说话，音频仍然要推给 STT
        send_to_stt(
            sess->session_id,
            out,
            sizeof(out)
        );
    }
}

        else if (sess->mode == VadMode::kTenVad) {

            bool is_voice = false;

            if (sess->ten_vad) {
                float prob = 0.0f;
                int flag = 0;

                if (ten_vad_process(
                        sess->ten_vad,
                        out,
                        kFrameSize,
                        &prob,
                        &flag) == 0)
                {
                    is_voice = (flag == 1);

                    // 如需调试概率，可打开
                    // LOGI("[TenVAD] prob={}", prob);
                }
            }

            handle_vad_logic(sess, is_voice, out, 30);
        }
    }
}


/* ================= 清理线程 ================= */

void session_cleaner_thread() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(20));
        time_t now = time(nullptr);

        std::lock_guard<std::mutex> lk(g_session_mu);
        for (auto it = g_sessions.begin(); it != g_sessions.end();) {
            auto& s = it->second;

            bool udp_to =
                (now - s->last_active_time) > SESSION_UDP_TIMEOUT_SEC;
            bool sp_to =
                (now - s->last_speech_time) > SESSION_SPEECH_TIMEOUT_SEC;

            if (udp_to || sp_to) {
                LOGW("Session {} timeout udp={} speech={}",
                     s->session_id, udp_to, sp_to);
                g_id_map.erase(s->session_id);
                it = g_sessions.erase(it);
            } else {
                ++it;
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

            sendto(g_sockfd, text.c_str(), text.size(), 0, (struct sockaddr*)&session->addr, sizeof(session->addr));

        }

    }

}

/* ================= 日志初始化 ================= */

void log_init()
{
    ::mkdir("logs", 0755);

    // 终端 sink
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        "logs/aeroshell_audio.log",
        10 * 1024 * 1024, 
        5                
    );

    std::vector<spdlog::sink_ptr> sinks { file_sink };

    // 创建 logger
    auto logger = std::make_shared<spdlog::logger>(
        "aeroshell",
        sinks.begin(),
        sinks.end()
    );

    spdlog::set_default_logger(logger);

    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");

    spdlog::flush_on(spdlog::level::info);
}

/* ================= main ================= */

int main(int argc, char* argv[]) {
    log_init();

    int opt;
    while ((opt = getopt(argc, argv, "v:m:h")) != -1) {
       if (opt == 'v') {
    int v = std::stoi(optarg);
    if (v == 1) g_vad_mode = VadMode::kWebRTC;
    else if (v == 2) g_vad_mode = VadMode::kTenVad;
    else g_vad_mode = VadMode::kSilero;
}
        else if (opt == 'm')
            g_model_path = optarg;
        else {
            LOGI("Usage: {} -v [0|1] -m [model_path]", argv[0]);
            return 0;
        }
    }

    g_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8000);

    if (g_vad_mode == VadMode::kSilero) {
    SileroVadDetector::Config cfg;
    cfg.model_path = g_model_path;
    cfg.sample_rate = kSampleRate;
    cfg.threshold = 0.5f;

    g_silero_vad = std::make_unique<SileroVadDetector>(cfg);

    LOGI("[Silero] global model loaded: {}", g_model_path);
}

    if (bind(g_sockfd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        LOGE("Bind failed: {}", strerror(errno));
        return -1;
    }

    std::thread(receiver_processor_thread).detach();
    std::thread(session_cleaner_thread).detach();
        // ai_response_thread 请自行根据您的 socket 需求补全
     std::thread(ai_response_thread).detach();

    LOGI("Gateway started, VAD={}",
     g_vad_mode == VadMode::kWebRTC ? "WebRTC" :
     g_vad_mode == VadMode::kTenVad ? "TenVAD" :
                                      "Silero");

    while (true)
        std::this_thread::sleep_for(std::chrono::minutes(1));
}
