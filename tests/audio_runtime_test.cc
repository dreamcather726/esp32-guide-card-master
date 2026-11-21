#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
#include "audio_constants.inc"

template<class... Args> void Log(Args&&...) {}
#define ESP_LOGI(...) Log(__VA_ARGS__)
#define ESP_LOGW(...) Log(__VA_ARGS__)
#define ESP_LOGE(...) Log(__VA_ARGS__)
#define TAG "AudioService"
#define CONFIG_USE_AUDIO_PROCESSOR 1
using BaseType_t = int;
using EventBits_t = unsigned long;
using TaskHandle_t = void*;
constexpr int pdPASS = 1, pdTRUE = 1, pdFALSE = 0, portMAX_DELAY = -1;
constexpr int ESP_AUDIO_ERR_OK = 0, ESP_AUDIO_MONO = 1;
constexpr int ESP_AUDIO_DEC_RECOVERY_NONE = 0;
int pdMS_TO_TICKS(int ms) { return ms / 50; }

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct EventGroup {
    std::mutex mutex;
    std::condition_variable cv;
    EventBits_t bits = AS_EVENT_WORKERS_EXITED;
};
using EventGroupHandle_t = EventGroup*;
EventBits_t xEventGroupGetBits(EventGroupHandle_t event) {
    std::lock_guard<std::mutex> lock(event->mutex);
    return event->bits;
}
void xEventGroupSetBits(EventGroupHandle_t event, EventBits_t bits) {
    std::lock_guard<std::mutex> lock(event->mutex);
    event->bits |= bits;
    event->cv.notify_all();
}
void xEventGroupClearBits(EventGroupHandle_t event, EventBits_t bits) {
    std::lock_guard<std::mutex> lock(event->mutex);
    event->bits &= ~bits;
}
EventBits_t xEventGroupWaitBits(EventGroupHandle_t event, EventBits_t mask, int clear, int all, int timeout) {
    std::unique_lock<std::mutex> lock(event->mutex);
    auto ready = [&] { return all ? (event->bits & mask) == mask : (event->bits & mask) != 0; };
    if (timeout == portMAX_DELAY) event->cv.wait(lock, ready);
    else event->cv.wait_for(lock, std::chrono::milliseconds(timeout), ready);
    const auto bits = event->bits;
    if (clear && ready()) event->bits &= ~mask;
    return bits;
}
std::vector<std::thread> workers;
std::string fail_task;
std::atomic<int> created{0};
BaseType_t xTaskCreate(void (*entry)(void*), const char* name, int, void* arg, int, TaskHandle_t*) {
    if (fail_task == name) return 0;
    ++created;
    workers.emplace_back([=] { entry(arg); });
    return pdPASS;
}
BaseType_t xTaskCreatePinnedToCore(void (*entry)(void*), const char* name, int stack, void* arg,
                                 int priority, TaskHandle_t* handle, int) {
    return xTaskCreate(entry, name, stack, arg, priority, handle);
}
void vTaskDelete(void*) {}
void vTaskDelay(int ticks) { std::this_thread::sleep_for(std::chrono::milliseconds(ticks)); }
void esp_timer_stop(void*) {}
void esp_timer_start_periodic(void*, int) {}
void JoinWorkers() {
    for (auto& worker : workers) worker.join();
    workers.clear();
}

struct AudioStreamPacket {
    int sample_rate = 16000, frame_duration = 60;
    uint32_t timestamp = 0;
    std::vector<uint8_t> payload{1};
};
enum AudioTaskType { kAudioTaskTypeEncodeToSendQueue, kAudioTaskTypeEncodeToTestingQueue,
                     kAudioTaskTypeDecodeToPlaybackQueue };
struct AudioTask {
    AudioTaskType type{};
    std::vector<int16_t> pcm;
    uint32_t timestamp = 0, generation = 0;
};
struct esp_opus_dec_cfg_t { int sample_rate, frame_duration; };
struct esp_ae_rate_cvt_cfg_t { int from, to, channels; };
#define OPUS_DEC_CFG(rate, duration) esp_opus_dec_cfg_t{rate, duration}
#define RATE_CVT_CFG(from, to, channels) esp_ae_rate_cvt_cfg_t{from, to, channels}
struct esp_audio_dec_in_raw_t { uint8_t* buffer; uint32_t len, consumed; int frame_recover; };
struct esp_audio_dec_out_frame_t { uint8_t* buffer; uint32_t len, decoded_size; };
struct esp_audio_dec_info_t {};
struct esp_audio_enc_in_frame_t { uint8_t* buffer; uint32_t len; };
struct esp_audio_enc_out_frame_t { uint8_t* buffer; uint32_t len, encoded_bytes; };
using esp_ae_sample_t = void*;
struct Decoder { std::atomic<int> busy{0}; };
std::atomic<int> decodes{0}, live_decoders{0}, live_resamplers{0}, resets{0};
std::atomic<bool> fail_open{false}, fail_resampler{false};
int esp_opus_dec_open(esp_opus_dec_cfg_t*, size_t, void** handle) {
    if (fail_open) return -1;
    *handle = new Decoder;
    ++live_decoders;
    return 0;
}
void esp_opus_dec_close(void* handle) {
    auto decoder = static_cast<Decoder*>(handle);
    Check(decoder->busy == 0, "decoder closed during decode");
    delete decoder;
    --live_decoders;
}
void esp_opus_dec_reset(void* handle) {
    auto decoder = static_cast<Decoder*>(handle);
    Check(decoder->busy == 0, "decoder reset during decode");
    ++resets;
}
int esp_opus_dec_decode(void* handle, esp_audio_dec_in_raw_t*, esp_audio_dec_out_frame_t* out,
                        esp_audio_dec_info_t*) {
    auto decoder = static_cast<Decoder*>(handle);
    Check(decoder->busy.fetch_add(1) == 0, "overlapping decoder calls");
    std::this_thread::sleep_for(100us);
    out->decoded_size = out->len;
    ++decodes;
    --decoder->busy;
    return 0;
}
std::atomic<bool> hold_encode{false}, encode_active{false};
int esp_opus_enc_process(void*, esp_audio_enc_in_frame_t*, esp_audio_enc_out_frame_t* out) {
    encode_active = true;
    while (hold_encode) std::this_thread::sleep_for(100us);
    out->buffer[0] = 1;
    out->encoded_bytes = 1;
    encode_active = false;
    return ESP_AUDIO_ERR_OK;
}
int esp_ae_rate_cvt_open(esp_ae_rate_cvt_cfg_t*, void** handle) {
    if (fail_resampler) return -1;
    *handle = new int(0);
    ++live_resamplers;
    return 0;
}
void esp_ae_rate_cvt_close(void* handle) { delete static_cast<int*>(handle); --live_resamplers; }
void esp_ae_rate_cvt_get_max_out_sample_num(void*, size_t count, uint32_t* out) { *out = count; }
void esp_ae_rate_cvt_process(void*, void*, size_t, void*, uint32_t*) {}
struct FakeCodec {
    int output_sample_rate() { return 16000; }
    int input_channels() { return 1; }
};
struct FakeProcessor {
    std::function<void()> on_feed;
    int GetFeedSize() const { return 960; }
    void Feed(const std::vector<int16_t>&) { on_feed(); }
};

class AudioService {
public:
    EventGroup event;
    EventGroupHandle_t event_group_ = &event;
    TaskHandle_t audio_input_task_handle_ = nullptr, audio_output_task_handle_ = nullptr,
                 opus_codec_task_handle_ = nullptr;
    void* audio_power_timer_ = nullptr;
    std::mutex lifecycle_mutex_, audio_queue_mutex_, decoder_mutex_, audio_processor_mutex_;
    std::condition_variable audio_queue_cv_;
    std::atomic<bool> service_stopped_{true};
    std::atomic<uint32_t> file_playback_generation_{0};
    bool voice_send_enabled_ = false;
    enum class VoiceCaptureStage { Waiting, AfeNotReady, MicReadError, MicRead, EncodeError, Encoded };
    std::atomic<VoiceCaptureStage> voice_capture_stage_{VoiceCaptureStage::Waiting};
    std::atomic<bool> audio_input_need_warmup_{false};
    uint32_t encode_generation_ = 0, decode_generation_ = 0;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_decode_queue_, audio_send_queue_, audio_testing_queue_;
    std::deque<std::unique_ptr<AudioTask>> audio_encode_queue_, audio_playback_queue_;
    std::deque<uint32_t> timestamp_queue_;
    struct { unsigned decode_count = 0, encode_count = 0; } debug_statistics_;
    struct { std::function<void()> on_send_queue_available; } callbacks_;
    void* opus_decoder_ = nullptr;
    void* opus_encoder_ = nullptr;
    void* output_resampler_ = nullptr;
    int decoder_sample_rate_ = 0, decoder_duration_ms_ = 60, decoder_frame_size_ = 0;
    int encoder_frame_size_ = 960, encoder_outbuf_size_ = 2048;
    FakeCodec codec;
    FakeCodec* codec_ = &codec;
    std::atomic<bool> hold_input{false};
    std::atomic<int> input_active{0}, max_input_active{0};
    unsigned read_attempts = 0;
    FakeProcessor voice_processor, wake_processor;
    FakeProcessor* audio_processor_ = &voice_processor;
    FakeProcessor* wake_word_ = &wake_processor;

    ~AudioService() {
        if (opus_decoder_) esp_opus_dec_close(opus_decoder_);
        if (output_resampler_) esp_ae_rate_cvt_close(output_resampler_);
    }
    void Start();
    void Stop();
    void StopWorkers();
    void OpusCodecTask();
    bool SetDecodeSampleRate(int, int);
    void ResetDecoder();
    void PushTaskToEncodeQueue(AudioTaskType, std::vector<int16_t>&&);
    bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket>, bool wait = false);
    void TestProductionInput();
    bool ReadAudioData(std::vector<int16_t>& data, int, int samples) {
        if (++read_attempts == 1) return false;
        data.assign(samples, 1);
        return true;
    }
    void EnableAudioTesting(bool) {}
    void EnableVoiceProcessing(bool) {}
    void EnableWakeWordDetection(bool) {}
    void AudioInputTask() {
        const int active = ++input_active;
        max_input_active.store(std::max(max_input_active.load(), active));
        while (!service_stopped_ || hold_input) std::this_thread::sleep_for(100us);
        --input_active;
    }
    void AudioOutputTask() {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        while (!service_stopped_) {
            audio_queue_cv_.wait(lock, [&] { return service_stopped_ || !audio_playback_queue_.empty(); });
            audio_playback_queue_.clear();
            audio_queue_cv_.notify_all();
        }
    }
};
#include "audio_runtime.inc"
#include "audio_input_production.inc"

int main() {
    {
        AudioService audio;
        audio.service_stopped_ = false;
        audio.voice_send_enabled_ = true;
        audio.voice_processor.on_feed = [&] { audio.service_stopped_ = true; };
        audio.wake_processor.on_feed = [] { throw std::runtime_error("wake word captured during voice processing"); };
        xEventGroupSetBits(&audio.event, AS_EVENT_AUDIO_PROCESSOR_RUNNING | AS_EVENT_WAKE_WORD_RUNNING);
        audio.TestProductionInput();
        Check(audio.read_attempts == 2, "input worker did not retry the failed microphone read");
        Check(audio.voice_capture_stage_ == AudioService::VoiceCaptureStage::MicRead,
              "microphone recovery did not advance diagnostics");
        std::cout << "PASS: microphone read retries and voice capture takes priority over wake word\n";
    }
    {
        AudioService audio;
        for (int i = 0; i < 50; ++i) {
            const int before = created;
            audio.Start(); audio.Start();
            Check(created == before + 3, "Start must be idempotent");
            std::this_thread::sleep_for(1ms);
            audio.Stop();
            Check(xEventGroupGetBits(&audio.event) & AS_EVENT_INPUT_EXITED, "input worker still running");
            Check(audio.input_active == 0, "Stop returned before input exited");
            JoinWorkers();
        }
        Check(audio.max_input_active <= 1, "duplicate input workers");
        std::cout << "PASS: 50 stop/restart cycles and duplicate Start\n";
    }
    for (const char* name : {"audio_input", "audio_output", "opus_codec"}) {
        AudioService audio;
        fail_task = name;
        audio.Start();
        Check(audio.service_stopped_, "partial task creation left service running");
        Check((xEventGroupGetBits(&audio.event) & AS_EVENT_WORKERS_EXITED) == AS_EVENT_WORKERS_EXITED,
              "partial startup workers not stopped");
        JoinWorkers();
        fail_task.clear();
        audio.Start(); audio.Stop(); JoinWorkers();
    }
    std::cout << "PASS: each task allocation failure rolls back and recovers\n";
    for (const auto type : {kAudioTaskTypeEncodeToTestingQueue, kAudioTaskTypeEncodeToSendQueue}) {
        AudioService audio;
        audio.opus_encoder_ = &audio;
        audio.voice_send_enabled_ = true;
        hold_encode = true;
        audio.Start();
        audio.PushTaskToEncodeQueue(type, std::vector<int16_t>(audio.encoder_frame_size_));
        while (!encode_active) std::this_thread::yield();
        std::thread stopper([&] { audio.Stop(); });
        while (!audio.service_stopped_) std::this_thread::yield();
        hold_encode = false;
        stopper.join();
        JoinWorkers();
        Check(audio.audio_testing_queue_.empty(), "in-flight testing packet survived Stop");
        Check(audio.audio_send_queue_.empty(), "in-flight send packet survived Stop");
        audio.Start(); audio.Stop(); JoinWorkers();
    }
    std::cout << "PASS: Stop discards in-flight testing and send encodes before restart\n";
    {
        AudioService audio;
        audio.service_stopped_ = false;
        for (int i = 0; i < MAX_DECODE_PACKETS_IN_QUEUE; ++i)
            audio.audio_decode_queue_.push_back(std::make_unique<AudioStreamPacket>());
        for (int i = 0; i < MAX_ENCODE_TASKS_IN_QUEUE; ++i)
            audio.audio_encode_queue_.push_back(std::make_unique<AudioTask>());
        std::atomic<int> waiting{0};
        bool accepted = true;
        std::thread decoder_producer([&] {
            ++waiting;
            accepted = audio.PushPacketToDecodeQueue(std::make_unique<AudioStreamPacket>(), true);
        });
        std::thread encoder_producer([&] {
            ++waiting;
            audio.PushTaskToEncodeQueue(kAudioTaskTypeEncodeToTestingQueue, {1, 2});
        });
        while (waiting != 2) std::this_thread::yield();
        std::this_thread::sleep_for(5ms);
        audio.Stop();
        decoder_producer.join(); encoder_producer.join();
        Check(!accepted, "blocked decode producer accepted packet after Stop");
        Check(audio.audio_encode_queue_.empty(), "blocked encode producer wrote after Stop");
        std::cout << "PASS: Stop releases producers blocked by saturated queues\n";
    }
    {
        AudioService audio;
        audio.hold_input = true;
        audio.Start();
        while (!audio.input_active) std::this_thread::yield();
        audio.Stop();
        const int before = created;
        audio.Start();
        Check(created == before, "restart created workers while old worker blocked");
        audio.hold_input = false;
        JoinWorkers();
        audio.Start(); audio.Stop(); JoinWorkers();
        std::cout << "PASS: stalled worker prevents overlapping restart\n";
    }
    {
        AudioService audio;
        Check(!audio.PushPacketToDecodeQueue(std::make_unique<AudioStreamPacket>(), true),
              "stopped decoder accepted packet");
        audio.PushTaskToEncodeQueue(kAudioTaskTypeEncodeToTestingQueue, {1, 2});
        Check(audio.audio_encode_queue_.empty(), "stopped encoder accepted frame");
        audio.Start();
        std::thread producer([&] {
            for (int i = 0; i < 300; ++i) {
                auto packet = std::make_unique<AudioStreamPacket>();
                packet->sample_rate = i % 2 ? 24000 : 16000;
                audio.PushPacketToDecodeQueue(std::move(packet), true);
            }
        });
        std::thread resetter([&] {
            for (int i = 0; i < 500; ++i) { audio.ResetDecoder(); std::this_thread::sleep_for(50us); }
        });
        producer.join(); resetter.join();
        audio.Stop(); JoinWorkers();
        Check(decodes > 0 && resets > 0, "concurrent decoder test performed no work");
        std::cout << "PASS: concurrent packet decode/rate changes/reset, stopped queue cancellation\n";
    }
    {
        AudioService audio;
        Check(audio.SetDecodeSampleRate(24000, 60), "resampler configuration failed");
        Check(live_resamplers == 1, "missing output resampler");
        Check(audio.SetDecodeSampleRate(16000, 60), "native configuration failed");
        Check(live_resamplers == 0, "return to native rate leaked resampler");
        fail_open = true;
        Check(!audio.SetDecodeSampleRate(24000, 60), "decoder open failure accepted");
        fail_open = false; fail_resampler = true;
        Check(!audio.SetDecodeSampleRate(24000, 60), "resampler failure accepted");
        Check(audio.opus_decoder_ == nullptr, "failed resampler left usable decoder");
        fail_resampler = false;
        Check(audio.SetDecodeSampleRate(24000, 60), "decoder did not recover");
        std::cout << "PASS: rate transitions, allocation failure and retry\n";
    }
    Check(live_decoders == 0 && live_resamplers == 0, "codec handle leak");
    std::cout << "PASS: all codec handles released; ASan/UBSan enabled\n";
}
