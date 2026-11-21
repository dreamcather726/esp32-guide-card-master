#include "audio_service.h"
#include <esp_log.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "esp_audio_dec_reg.h"
#include "esp_mp3_dec.h"
#include "esp_audio_simple_dec.h"

#define RATE_CVT_CFG(_src_rate, _dest_rate, _channel)        \
    (esp_ae_rate_cvt_cfg_t)                                  \
    {                                                        \
        .src_rate        = (uint32_t)(_src_rate),            \
        .dest_rate       = (uint32_t)(_dest_rate),           \
        .channel         = (uint8_t)(_channel),              \
        .bits_per_sample = ESP_AUDIO_BIT16,                  \
        .complexity      = 2,                                \
        .perf_type       = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,  \
    }

#define OPUS_DEC_CFG(_sample_rate, _frame_duration_ms)                                                    \
    (esp_opus_dec_cfg_t)                                                                                  \
    {                                                                                                     \
        .sample_rate    = (uint32_t)(_sample_rate),                                                       \
        .channel        = ESP_AUDIO_MONO,                                                                 \
        .frame_duration = (esp_opus_dec_frame_duration_t)AS_OPUS_GET_FRAME_DRU_ENUM(_frame_duration_ms),  \
        .self_delimited = false,                                                                          \
    }

#if CONFIG_USE_AUDIO_PROCESSOR
#include "processors/afe_audio_processor.h"
#else
#include "processors/no_audio_processor.h"
#endif

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
#include "wake_words/afe_wake_word.h"
#include "wake_words/custom_wake_word.h"
#else
#include "wake_words/esp_wake_word.h"
#endif

#define TAG "AudioService"

static uint16_t ReadLe16(const uint8_t* value) {
    return static_cast<uint16_t>(value[0]) |
           (static_cast<uint16_t>(value[1]) << 8);
}

static uint32_t ReadLe32(const uint8_t* value) {
    return static_cast<uint32_t>(value[0]) |
           (static_cast<uint32_t>(value[1]) << 8) |
           (static_cast<uint32_t>(value[2]) << 16) |
           (static_cast<uint32_t>(value[3]) << 24);
}

static bool EndsWith(const std::string& value, const char* suffix) {
    const size_t suffix_length = std::strlen(suffix);
    return value.size() >= suffix_length &&
           value.compare(value.size() - suffix_length, suffix_length, suffix) == 0;
}

AudioService::AudioService() {
    event_group_ = xEventGroupCreate();
    assert(event_group_ != nullptr);
    xEventGroupSetBits(event_group_, AS_EVENT_WORKERS_EXITED);
}

AudioService::~AudioService() {
    {
        std::lock_guard<std::mutex> lock(file_playback_mutex_);
        if (mp3_decoder_ != nullptr) {
            esp_audio_simple_dec_close(mp3_decoder_);
            mp3_decoder_ = nullptr;
        }
        if (mp3_decoder_registry_owned_) {
            esp_audio_dec_unregister(ESP_AUDIO_TYPE_MP3);
            mp3_decoder_registry_owned_ = false;
        }
        mp3_decoder_registered_ = false;
    }
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
    if (opus_encoder_ != nullptr) {
        esp_opus_enc_close(opus_encoder_);
    }
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_close(opus_decoder_);
    }
    if (input_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(input_resampler_);
    }
    if (output_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(output_resampler_);
    }
}

void AudioService::Initialize(AudioCodec* codec) {
    codec_ = codec;
    codec_->Start();

    if (esp_audio_dec_get_ops(ESP_AUDIO_TYPE_MP3) != nullptr) {
        mp3_decoder_registered_ = true;
        ESP_LOGI(TAG, "MP3 decoder registry is already available");
    } else {
        const esp_audio_err_t result = esp_mp3_dec_register();
        mp3_decoder_registered_ = result == ESP_AUDIO_ERR_OK;
        mp3_decoder_registry_owned_ = mp3_decoder_registered_;
        if (mp3_decoder_registered_) {
            ESP_LOGI(TAG, "MP3 decoder registered for process lifetime");
        } else {
            ESP_LOGE(TAG, "Failed to register MP3 decoder: %d", result);
        }
    }

    esp_opus_dec_cfg_t opus_dec_cfg = OPUS_DEC_CFG(codec->output_sample_rate(), OPUS_FRAME_DURATION_MS);
    auto ret = esp_opus_dec_open(&opus_dec_cfg, sizeof(esp_opus_dec_cfg_t), &opus_decoder_);
    if (opus_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio decoder, error code: %d", ret);
    } else {
        decoder_sample_rate_ = codec->output_sample_rate();
        decoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
        decoder_frame_size_ = decoder_sample_rate_ / 1000 * OPUS_FRAME_DURATION_MS;
    }
    esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
    ret = esp_opus_enc_open(&opus_enc_cfg, sizeof(esp_opus_enc_config_t), &opus_encoder_);
    if (opus_encoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio encoder, error code: %d", ret);
    } else {
        encoder_sample_rate_ = 16000;
        encoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
        esp_opus_enc_get_frame_size(opus_encoder_, &encoder_frame_size_, &encoder_outbuf_size_);
        encoder_frame_size_ = encoder_frame_size_ / sizeof(int16_t);
    }

    if (codec->input_sample_rate() != 16000) {
        esp_ae_rate_cvt_cfg_t input_resampler_cfg = RATE_CVT_CFG(
            codec->input_sample_rate(), ESP_AUDIO_SAMPLE_RATE_16K, codec->input_channels());
        auto resampler_ret = esp_ae_rate_cvt_open(&input_resampler_cfg, &input_resampler_);
        if (input_resampler_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create input resampler, error code: %d", resampler_ret);
        }
    }

#if CONFIG_USE_AUDIO_PROCESSOR
    audio_processor_ = std::make_unique<AfeAudioProcessor>();
#else
    audio_processor_ = std::make_unique<NoAudioProcessor>();
#endif

    audio_processor_->OnOutput([this](std::vector<int16_t>&& data) {
        PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue, std::move(data));
    });

    audio_processor_->OnVadStateChange([this](bool speaking) {
        voice_detected_ = speaking;
        if (callbacks_.on_vad_change) {
            callbacks_.on_vad_change(speaking);
        }
    });

    esp_timer_create_args_t audio_power_timer_args = {
        .callback = [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->CheckAndUpdateAudioPowerState();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "audio_power_timer",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&audio_power_timer_args, &audio_power_timer_);
}

void AudioService::Start() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!service_stopped_) {
        return;
    }
    if ((xEventGroupGetBits(event_group_) & AS_EVENT_WORKERS_EXITED) != AS_EVENT_WORKERS_EXITED) {
        ESP_LOGE(TAG, "Audio restart deferred: previous workers are still stopping");
        return;
    }
    service_stopped_ = false;
    xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING | AS_EVENT_WAKE_WORD_RUNNING |
                        AS_EVENT_AUDIO_PROCESSOR_RUNNING | AS_EVENT_WORKERS_EXITED);

    esp_timer_start_periodic(audio_power_timer_, 1000000);

#if CONFIG_USE_AUDIO_PROCESSOR
    /* Start the audio input task */
    const BaseType_t input_created = xTaskCreatePinnedToCore([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioInputTask();
        xEventGroupSetBits(audio_service->event_group_, AS_EVENT_INPUT_EXITED);
        vTaskDelete(NULL);
    }, "audio_input", 2048 * 3, this, 8, &audio_input_task_handle_, 0);

    /* Start the audio output task */
    const BaseType_t output_created = xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioOutputTask();
        xEventGroupSetBits(audio_service->event_group_, AS_EVENT_OUTPUT_EXITED);
        vTaskDelete(NULL);
    }, "audio_output", 2048 * 2, this, 4, &audio_output_task_handle_);
#else
    /* Start the audio input task */
    const BaseType_t input_created = xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioInputTask();
        xEventGroupSetBits(audio_service->event_group_, AS_EVENT_INPUT_EXITED);
        vTaskDelete(NULL);
    }, "audio_input", 2048 * 2, this, 8, &audio_input_task_handle_);

    /* Start the audio output task */
    const BaseType_t output_created = xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioOutputTask();
        xEventGroupSetBits(audio_service->event_group_, AS_EVENT_OUTPUT_EXITED);
        vTaskDelete(NULL);
    }, "audio_output", 2048, this, 4, &audio_output_task_handle_);
#endif

    /* Start the opus codec task */
    const BaseType_t codec_created = xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->OpusCodecTask();
        xEventGroupSetBits(audio_service->event_group_, AS_EVENT_CODEC_EXITED);
        vTaskDelete(NULL);
    }, "opus_codec", 2048 * 12, this, 2, &opus_codec_task_handle_);

    if (input_created != pdPASS) xEventGroupSetBits(event_group_, AS_EVENT_INPUT_EXITED);
    if (output_created != pdPASS) xEventGroupSetBits(event_group_, AS_EVENT_OUTPUT_EXITED);
    if (codec_created != pdPASS) xEventGroupSetBits(event_group_, AS_EVENT_CODEC_EXITED);
    if (input_created != pdPASS || output_created != pdPASS || codec_created != pdPASS) {
        ESP_LOGE(TAG, "Audio task creation failed: input=%d output=%d codec=%d",
                 input_created, output_created, codec_created);
        StopWorkers();
    }
}

void AudioService::Stop() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    StopWorkers();
}

void AudioService::StopWorkers() {
    esp_timer_stop(audio_power_timer_);
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        service_stopped_ = true;
        voice_send_enabled_ = false;
        ++encode_generation_;
        ++decode_generation_;
        file_playback_generation_.fetch_add(1);
        audio_encode_queue_.clear();
        audio_decode_queue_.clear();
        audio_send_queue_.clear();
        audio_playback_queue_.clear();
        audio_testing_queue_.clear();
        timestamp_queue_.clear();
        audio_queue_cv_.notify_all();
    }
    EnableVoiceProcessing(false);
    EnableWakeWordDetection(false);
    xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING |
        AS_EVENT_WAKE_WORD_RUNNING |
        AS_EVENT_AUDIO_PROCESSOR_RUNNING);

    const EventBits_t exited = xEventGroupWaitBits(event_group_, AS_EVENT_WORKERS_EXITED,
                                                 pdFALSE, pdTRUE, pdMS_TO_TICKS(5000));
    esp_timer_stop(audio_power_timer_);
    if ((exited & AS_EVENT_WORKERS_EXITED) != AS_EVENT_WORKERS_EXITED) {
        ESP_LOGE(TAG, "Audio stop timed out; restart blocked until workers exit: bits=0x%lx", exited);
        return;
    }
    xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING |
                        AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    ESP_LOGI(TAG, "Audio workers stopped");
}

bool AudioService::ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples) {
    if (!codec_->input_enabled()) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        codec_->EnableInput(true);
    }

    if (codec_->input_sample_rate() != sample_rate) {
        if (input_resampler_ == nullptr) return false;
        data.resize(samples * codec_->input_sample_rate() / sample_rate * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            uint32_t in_sample_num = data.size() / codec_->input_channels();
            uint32_t output_samples = 0;
            esp_ae_rate_cvt_get_max_out_sample_num(input_resampler_, in_sample_num, &output_samples);
            auto resampled = std::vector<int16_t>(output_samples * codec_->input_channels());
            uint32_t actual_output = output_samples;
            if (esp_ae_rate_cvt_process(input_resampler_, (esp_ae_sample_t)data.data(), in_sample_num,
                                        (esp_ae_sample_t)resampled.data(), &actual_output) != ESP_AE_ERR_OK ||
                actual_output == 0) {
                return false;
            }
            resampled.resize(actual_output * codec_->input_channels());
            // AFE always consumes exactly one feed chunk.
            resampled.resize(samples * codec_->input_channels());
            data = std::move(resampled);
        }
    } else {
        data.resize(samples * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
    }

    /* Update the last input time */
    last_input_time_ = std::chrono::steady_clock::now();
    debug_statistics_.input_count++;

#if CONFIG_USE_AUDIO_DEBUGGER
    // 音频调试：发送原始音频数据
    if (!Board::GetInstance().RequiresUserBinding()) {
        if (audio_debugger_ == nullptr) {
            audio_debugger_ = std::make_unique<AudioDebugger>();
        }
        audio_debugger_->Feed(data);
    }
#endif

    return true;
}

void AudioService::AudioInputTask() {
    bool reported_failure = false;
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING |
            AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING,
            pdFALSE, pdFALSE, portMAX_DELAY);

        if (service_stopped_) {
            break;
        }
        if (audio_input_need_warmup_.exchange(false)) {
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        /* Used for audio testing in NetworkConfiguring mode by clicking the BOOT button */
        if (bits & AS_EVENT_AUDIO_TESTING_RUNNING) {
            bool testing_queue_full;
            {
                std::lock_guard<std::mutex> lock(audio_queue_mutex_);
                testing_queue_full = audio_testing_queue_.size() >=
                    AUDIO_TESTING_MAX_DURATION_MS / OPUS_FRAME_DURATION_MS;
            }
            if (testing_queue_full) {
                ESP_LOGW(TAG, "Audio testing queue is full, stopping audio testing");
                EnableAudioTesting(false);
                continue;
            }
            std::vector<int16_t> data;
            int samples = OPUS_FRAME_DURATION_MS * 16000 / 1000;
            if (ReadAudioData(data, 16000, samples)) {
                // If input channels is 2, we need to fetch the left channel data
                if (codec_->input_channels() == 2) {
                    auto mono_data = std::vector<int16_t>(data.size() / 2);
                    for (size_t i = 0, j = 0; i < mono_data.size(); ++i, j += 2) {
                        mono_data[i] = data[j];
                    }
                    data = std::move(mono_data);
                }
                PushTaskToEncodeQueue(kAudioTaskTypeEncodeToTestingQueue, std::move(data));
                continue;
            }
        }

        /* Feed the wake word */
        if ((bits & AS_EVENT_WAKE_WORD_RUNNING) && !(bits & AS_EVENT_AUDIO_PROCESSOR_RUNNING)) {
            std::vector<int16_t> data;
            int samples = wake_word_->GetFeedSize();
            if (samples > 0) {
                if (ReadAudioData(data, 16000, samples)) {
                    wake_word_->Feed(data);
                    reported_failure = false;
                    continue;
                }
            }
        }

        /* Feed the audio processor */
        if (bits & AS_EVENT_AUDIO_PROCESSOR_RUNNING) {
            uint32_t generation;
            {
                std::lock_guard<std::mutex> lock(audio_queue_mutex_);
                generation = encode_generation_;
            }
            std::vector<int16_t> data;
            int samples = audio_processor_->GetFeedSize();
            bool current;
            const bool read = samples > 0 && ReadAudioData(data, 16000, samples);
            {
                std::lock_guard<std::mutex> processor_lock(audio_processor_mutex_);
                {
                    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
                    current = voice_send_enabled_ && generation == encode_generation_;
                    if (current && voice_capture_stage_ != VoiceCaptureStage::Encoded &&
                        voice_capture_stage_ != VoiceCaptureStage::EncodeError) {
                        voice_capture_stage_ = read ? VoiceCaptureStage::MicRead :
                            samples > 0 ? VoiceCaptureStage::MicReadError : VoiceCaptureStage::AfeNotReady;
                    }
                }
                if (read && current) audio_processor_->Feed(std::move(data));
            }
            if (read) {
                reported_failure = false;
                continue;
            }
        }

        if (!reported_failure) {
            ESP_LOGW(TAG, "Audio input unavailable, retrying (bits: %lx)", bits);
            reported_failure = true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    ESP_LOGW(TAG, "Audio input task stopped");
}

void AudioService::AudioOutputTask() {
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]() { return !audio_playback_queue_.empty() || service_stopped_; });
        if (service_stopped_) {
            break;
        }

        auto task = std::move(audio_playback_queue_.front());
        audio_playback_queue_.pop_front();
        playback_active_ = true;
        audio_queue_cv_.notify_all();
        lock.unlock();

        if (!codec_->output_enabled()) {
            esp_timer_stop(audio_power_timer_);
            esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
            codec_->EnableOutput(true);
        }
        codec_->OutputData(task->pcm);

        lock.lock();
        playback_active_ = false;
        audio_queue_cv_.notify_all();
        lock.unlock();

        /* Update the last output time */
        last_output_time_ = std::chrono::steady_clock::now();
        debug_statistics_.playback_count++;

#if CONFIG_USE_SERVER_AEC
        /* Record the timestamp for server AEC */
        if (task->timestamp > 0) {
            lock.lock();
            timestamp_queue_.push_back(task->timestamp);
        }
#endif
    }

    ESP_LOGW(TAG, "Audio output task stopped");
}

void AudioService::OpusCodecTask() {
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]() {
            return service_stopped_ ||
                (!audio_encode_queue_.empty() && audio_send_queue_.size() < MAX_SEND_PACKETS_IN_QUEUE) ||
                (!audio_decode_queue_.empty() && audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE);
        });
        if (service_stopped_) {
            break;
        }

        /* Decode the audio from decode queue */
        if (!audio_decode_queue_.empty() && audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE) {
            const uint32_t generation = decode_generation_;
            auto packet = std::move(audio_decode_queue_.front());
            audio_decode_queue_.pop_front();
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto task = std::make_unique<AudioTask>();
            task->type = kAudioTaskTypeDecodeToPlaybackQueue;
            task->timestamp = packet->timestamp;

            std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
            if (SetDecodeSampleRate(packet->sample_rate, packet->frame_duration)) {
                task->pcm.resize(decoder_frame_size_);
                esp_audio_dec_in_raw_t raw = {
                    .buffer = (uint8_t *)(packet->payload.data()),
                    .len = (uint32_t)(packet->payload.size()),
                    .consumed = 0,
                    .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
                };
                esp_audio_dec_out_frame_t out_frame = {
                    .buffer = (uint8_t *)(task->pcm.data()),
                    .len = (uint32_t)(task->pcm.size() * sizeof(int16_t)),
                    .decoded_size = 0,
                };
                esp_audio_dec_info_t dec_info = {};
                auto ret = esp_opus_dec_decode(opus_decoder_, &raw, &out_frame, &dec_info);
                if (ret == ESP_AUDIO_ERR_OK) {
                    task->pcm.resize(out_frame.decoded_size / sizeof(int16_t));
                    if (decoder_sample_rate_ != codec_->output_sample_rate() && output_resampler_ != nullptr) {
                        uint32_t target_size = 0;
                        esp_ae_rate_cvt_get_max_out_sample_num(output_resampler_, task->pcm.size(), &target_size);
                        std::vector<int16_t> resampled(target_size);
                        uint32_t actual_output = target_size;
                        esp_ae_rate_cvt_process(output_resampler_, (esp_ae_sample_t)task->pcm.data(), task->pcm.size(),
                                                (esp_ae_sample_t)resampled.data(), &actual_output);
                        resampled.resize(actual_output);
                        task->pcm = std::move(resampled);
                    }
                    decoder_lock.unlock();
                    lock.lock();
                    if (!service_stopped_ && generation == decode_generation_) {
                        audio_playback_queue_.push_back(std::move(task));
                    }
                    audio_queue_cv_.notify_all();
                    debug_statistics_.decode_count++;
                } else {
                    ESP_LOGE(TAG, "Failed to decode audio after resize, error code: %d", ret);
                    decoder_lock.unlock();
                    lock.lock();
                }
            } else {
                ESP_LOGE(TAG, "Audio decoder is not configured");
                decoder_lock.unlock();
                lock.lock();
            }
            debug_statistics_.decode_count++;
        }
        /* Encode the audio to send queue */
        if (!audio_encode_queue_.empty() && audio_send_queue_.size() < MAX_SEND_PACKETS_IN_QUEUE) {
            auto task = std::move(audio_encode_queue_.front());
            audio_encode_queue_.pop_front();
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto packet = std::make_unique<AudioStreamPacket>();
            packet->frame_duration = OPUS_FRAME_DURATION_MS;
            packet->sample_rate = 16000;
            packet->timestamp = task->timestamp;
            bool encoded = false;

            if (opus_encoder_ != nullptr && task->pcm.size() == static_cast<size_t>(encoder_frame_size_)) {
                std::vector<uint8_t> buf(encoder_outbuf_size_);
                esp_audio_enc_in_frame_t in = {
                    .buffer = (uint8_t *)(task->pcm.data()),
                    .len = (uint32_t)(encoder_frame_size_ * sizeof(int16_t)),
                };
                esp_audio_enc_out_frame_t out = {
                    .buffer = buf.data(),
                    .len = (uint32_t)encoder_outbuf_size_,
                    .encoded_bytes = 0,
                };
                auto ret = esp_opus_enc_process(opus_encoder_, &in, &out);
                if (ret == ESP_AUDIO_ERR_OK && out.encoded_bytes > 0) {
                    encoded = true;
                    packet->payload.assign(buf.data(), buf.data() + out.encoded_bytes);

                    if (task->type == kAudioTaskTypeEncodeToSendQueue) {
                        {
                            std::lock_guard<std::mutex> lock2(audio_queue_mutex_);
                            if (!service_stopped_ && voice_send_enabled_ && task->generation == encode_generation_) {
                                voice_capture_stage_ = VoiceCaptureStage::Encoded;
                                audio_send_queue_.push_back(std::move(packet));
                            }
                        }
                        if (callbacks_.on_send_queue_available) {
                            callbacks_.on_send_queue_available();
                        }
                    } else if (task->type == kAudioTaskTypeEncodeToTestingQueue) {
                        std::lock_guard<std::mutex> lock2(audio_queue_mutex_);
                        if (!service_stopped_ && task->generation == encode_generation_) {
                            audio_testing_queue_.push_back(std::move(packet));
                        }
                    }
                    debug_statistics_.encode_count++;
                } else {
                    ESP_LOGE(TAG, "Failed to encode audio, error code: %d", ret);
                }
            } else {
                ESP_LOGE(TAG, "Failed to encode audio: encoder not configured or invalid frame size (got %u, expected %u)",
                         task->pcm.size(), encoder_frame_size_);
            }
            lock.lock();
            if (!encoded && task->type == kAudioTaskTypeEncodeToSendQueue &&
                voice_send_enabled_ && task->generation == encode_generation_ &&
                voice_capture_stage_ != VoiceCaptureStage::Encoded) {
                voice_capture_stage_ = VoiceCaptureStage::EncodeError;
            }
        }
    }

    ESP_LOGW(TAG, "Opus codec task stopped");
}

bool AudioService::SetDecodeSampleRate(int sample_rate, int frame_duration) {
    // The caller holds decoder_mutex_ through configuration, decode and resampling.
    if (opus_decoder_ != nullptr && decoder_sample_rate_ == sample_rate && decoder_duration_ms_ == frame_duration) {
        return true;
    }
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_close(opus_decoder_);
        opus_decoder_ = nullptr;
    }
    esp_opus_dec_cfg_t opus_dec_cfg = OPUS_DEC_CFG(sample_rate, frame_duration);
    auto ret = esp_opus_dec_open(&opus_dec_cfg, sizeof(esp_opus_dec_cfg_t), &opus_decoder_);
    if (opus_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio decoder, error code: %d", ret);
        return false;
    }
    decoder_sample_rate_ = sample_rate;
    decoder_duration_ms_ = frame_duration;
    decoder_frame_size_ = decoder_sample_rate_ / 1000 * frame_duration;

    if (output_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(output_resampler_);
        output_resampler_ = nullptr;
    }
    if (decoder_sample_rate_ != codec_->output_sample_rate()) {
        ESP_LOGI(TAG, "Resampling audio from %d to %d", decoder_sample_rate_, codec_->output_sample_rate());
        esp_ae_rate_cvt_cfg_t output_resampler_cfg = RATE_CVT_CFG(
            decoder_sample_rate_, codec_->output_sample_rate(), ESP_AUDIO_MONO);
        auto resampler_ret = esp_ae_rate_cvt_open(&output_resampler_cfg, &output_resampler_);
        if (output_resampler_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create output resampler, error code: %d", resampler_ret);
            esp_opus_dec_close(opus_decoder_);
            opus_decoder_ = nullptr;
            return false;
        }
    }
    return true;
}

void AudioService::PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm) {
    auto task = std::make_unique<AudioTask>();
    task->type = type;
    task->pcm = std::move(pcm);
    /* Push the task to the encode queue */
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    task->generation = encode_generation_;
    if (service_stopped_ || (type == kAudioTaskTypeEncodeToSendQueue && !voice_send_enabled_)) return;

    /* If the task is to send queue, we need to set the timestamp */
    if (type == kAudioTaskTypeEncodeToSendQueue && !timestamp_queue_.empty()) {
        if (timestamp_queue_.size() <= MAX_TIMESTAMPS_IN_QUEUE) {
            task->timestamp = timestamp_queue_.front();
        } else {
            ESP_LOGW(TAG, "Timestamp queue (%u) is full, dropping timestamp", timestamp_queue_.size());
        }
        timestamp_queue_.pop_front();
    }

    audio_queue_cv_.wait(lock, [this]() { return service_stopped_ || audio_encode_queue_.size() < MAX_ENCODE_TASKS_IN_QUEUE; });
    if (service_stopped_) return;
    if (type == kAudioTaskTypeEncodeToSendQueue &&
        (!voice_send_enabled_ || task->generation != encode_generation_)) return;
    audio_encode_queue_.push_back(std::move(task));
    audio_queue_cv_.notify_all();
}

bool AudioService::PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait) {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    if (service_stopped_ || !packet) return false;
    if (audio_decode_queue_.size() >= MAX_DECODE_PACKETS_IN_QUEUE) {
        if (wait) {
            audio_queue_cv_.wait(lock, [this]() { return service_stopped_ || audio_decode_queue_.size() < MAX_DECODE_PACKETS_IN_QUEUE; });
        } else {
            return false;
        }
    }
    if (service_stopped_) return false;
    audio_decode_queue_.push_back(std::move(packet));
    audio_queue_cv_.notify_all();
    return true;
}

std::unique_ptr<AudioStreamPacket> AudioService::PopPacketFromSendQueue() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (audio_send_queue_.empty()) {
        return nullptr;
    }
    auto packet = std::move(audio_send_queue_.front());
    audio_send_queue_.pop_front();
    audio_queue_cv_.notify_all();
    return packet;
}

void AudioService::EncodeWakeWord() {
    if (wake_word_) {
        wake_word_->EncodeWakeWordData();
    }
}

const std::string& AudioService::GetLastWakeWord() const {
    return wake_word_->GetLastDetectedWakeWord();
}

std::unique_ptr<AudioStreamPacket> AudioService::PopWakeWordPacket() {
    auto packet = std::make_unique<AudioStreamPacket>();
    if (wake_word_->GetWakeWordOpus(packet->payload)) {
        return packet;
    }
    return nullptr;
}

void AudioService::EnableWakeWordDetection(bool enable) {
    if (!wake_word_) {
        return;
    }

    ESP_LOGD(TAG, "%s wake word detection", enable ? "Enabling" : "Disabling");
    if (enable) {
        if (!wake_word_initialized_) {
            if (!wake_word_->Initialize(codec_, models_list_)) {
                ESP_LOGE(TAG, "Failed to initialize wake word");
                return;
            }
            wake_word_initialized_ = true;
        }
        // Reset input resampler to clear cached data from previous mode (e.g. AudioProcessor)
        // This prevents buffer overflow when switching between different feed sizes
        {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            if (input_resampler_ != nullptr) {
                esp_ae_rate_cvt_reset(input_resampler_);
            }
        }
        wake_word_->Start();
        xEventGroupSetBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
    } else {
        wake_word_->Stop();
        xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
    }
}

void AudioService::EnableVoiceProcessing(bool enable) {
    ESP_LOGD(TAG, "%s voice processing", enable ? "Enabling" : "Disabling");
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        voice_send_enabled_ = enable;
        if (enable) voice_capture_stage_ = VoiceCaptureStage::Waiting;
        ++encode_generation_;
        audio_encode_queue_.clear();
        audio_send_queue_.clear();
        audio_queue_cv_.notify_all();
    }
    std::lock_guard<std::mutex> processor_lock(audio_processor_mutex_);
    if (enable) {
        if (!audio_processor_initialized_) {
            audio_processor_->Initialize(codec_, OPUS_FRAME_DURATION_MS, models_list_);
            audio_processor_initialized_ = audio_processor_->GetFeedSize() > 0;
        }
        if (!audio_processor_initialized_) {
            std::lock_guard<std::mutex> lock(audio_queue_mutex_);
            voice_send_enabled_ = false;
            voice_capture_stage_ = VoiceCaptureStage::AfeNotReady;
            return;
        }

        /* We should make sure no audio is playing */
        ResetDecoder();
        audio_input_need_warmup_ = true;
        // Reset input resampler to clear cached data from previous mode (e.g. WakeWord)
        // This prevents buffer overflow when switching between different feed sizes
        {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            if (input_resampler_ != nullptr) {
                esp_ae_rate_cvt_reset(input_resampler_);
            }
        }
        audio_processor_->Start();
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    } else {
        audio_processor_->Stop();
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    }
}

const char* AudioService::VoiceCaptureDiagnostic() const {
    if (service_stopped_ || (xEventGroupGetBits(event_group_) & AS_EVENT_INPUT_EXITED)) return "VOICE INPUT STOPPED";
    if (!input_resampler_ && codec_->input_sample_rate() != 16000) return "VOICE RESAMPLE INIT ERR";
    switch (voice_capture_stage_.load()) {
        case VoiceCaptureStage::AfeNotReady: return "VOICE AFE NOT READY";
        case VoiceCaptureStage::MicReadError: return "VOICE MIC OR RATE ERR";
        case VoiceCaptureStage::MicRead: return "VOICE AFE NO FRAME";
        case VoiceCaptureStage::EncodeError: return "VOICE OPUS ERR";
        case VoiceCaptureStage::Encoded: return "VOICE OPUS NO TX";
        case VoiceCaptureStage::Waiting: return "VOICE NO MIC SAMPLES";
    }
    return "VOICE NO AUDIO TX";
}

void AudioService::EnableAudioTesting(bool enable) {
    ESP_LOGI(TAG, "%s audio testing", enable ? "Enabling" : "Disabling");
    if (enable) {
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
    } else {
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
        /* Copy audio_testing_queue_ to audio_decode_queue_ */
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        audio_decode_queue_ = std::move(audio_testing_queue_);
        audio_queue_cv_.notify_all();
    }
}

void AudioService::EnableDeviceAec(bool enable) {
    ESP_LOGI(TAG, "%s device AEC", enable ? "Enabling" : "Disabling");
    std::lock_guard<std::mutex> processor_lock(audio_processor_mutex_);
    if (!audio_processor_initialized_) {
        audio_processor_->Initialize(codec_, OPUS_FRAME_DURATION_MS, models_list_);
        audio_processor_initialized_ = audio_processor_->GetFeedSize() > 0;
    }

    audio_processor_->EnableDeviceAec(enable);
}

void AudioService::SetCallbacks(AudioServiceCallbacks& callbacks) {
    callbacks_ = callbacks;
}

void AudioService::PlaySound(const std::string_view& ogg) {
    if (!codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        codec_->EnableOutput(true);
    }

    const uint8_t* buf = reinterpret_cast<const uint8_t*>(ogg.data());
    size_t size = ogg.size();
    size_t offset = 0;

    auto find_page = [&](size_t start)->size_t {
        for (size_t i = start; i + 4 <= size; ++i) {
            if (buf[i] == 'O' && buf[i+1] == 'g' && buf[i+2] == 'g' && buf[i+3] == 'S') return i;
        }
        return static_cast<size_t>(-1);
    };

    bool seen_head = false;
    bool seen_tags = false;
    int sample_rate = 16000; // 默认值

    while (true) {
        size_t pos = find_page(offset);
        if (pos == static_cast<size_t>(-1)) break;
        offset = pos;
        if (offset + 27 > size) break;

        const uint8_t* page = buf + offset;
        uint8_t page_segments = page[26];
        size_t seg_table_off = offset + 27;
        if (seg_table_off + page_segments > size) break;

        size_t body_size = 0;
        for (size_t i = 0; i < page_segments; ++i) body_size += page[27 + i];

        size_t body_off = seg_table_off + page_segments;
        if (body_off + body_size > size) break;

        // Parse packets using lacing
        size_t cur = body_off;
        size_t seg_idx = 0;
        while (seg_idx < page_segments) {
            size_t pkt_len = 0;
            size_t pkt_start = cur;
            bool continued = false;
            do {
                uint8_t l = page[27 + seg_idx++];
                pkt_len += l;
                cur += l;
                continued = (l == 255);
            } while (continued && seg_idx < page_segments);

            if (pkt_len == 0) continue;
            const uint8_t* pkt_ptr = buf + pkt_start;

            if (!seen_head) {
                // 解析OpusHead包
                if (pkt_len >= 19 && std::memcmp(pkt_ptr, "OpusHead", 8) == 0) {
                    seen_head = true;
                    // OpusHead结构：[0-7] "OpusHead", [8] version, [9] channel_count, [10-11] pre_skip
                    // [12-15] input_sample_rate, [16-17] output_gain, [18] mapping_family
                    if (pkt_len >= 12) {
                        uint8_t version = pkt_ptr[8];
                        uint8_t channel_count = pkt_ptr[9];
                        if (pkt_len >= 16) {
                            // 读取输入采样率 (little-endian)
                            sample_rate = pkt_ptr[12] | (pkt_ptr[13] << 8) |
                                        (pkt_ptr[14] << 16) | (pkt_ptr[15] << 24);
                            ESP_LOGI(TAG, "OpusHead: version=%d, channels=%d, sample_rate=%d",
                                   version, channel_count, sample_rate);
                        }
                    }
                }
                continue;
            }
            if (!seen_tags) {
                // Expect OpusTags in second packet
                if (pkt_len >= 8 && std::memcmp(pkt_ptr, "OpusTags", 8) == 0) {
                    seen_tags = true;
                }
                continue;
            }

            // Audio packet (Opus)
            auto packet = std::make_unique<AudioStreamPacket>();
            packet->sample_rate = sample_rate;
            packet->frame_duration = 60;
            packet->payload.resize(pkt_len);
            std::memcpy(packet->payload.data(), pkt_ptr, pkt_len);
            if (!PushPacketToDecodeQueue(std::move(packet), true)) return;
        }

        offset = body_off + body_size;
    }
}

bool AudioService::IsIdle() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return audio_encode_queue_.empty() && audio_decode_queue_.empty() &&
           audio_playback_queue_.empty() && audio_testing_queue_.empty() &&
           !playback_active_;
}

void AudioService::WaitForPlaybackIdle() {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    audio_queue_cv_.wait(lock, [this]() {
        return service_stopped_ || (audio_playback_queue_.empty() && !playback_active_);
    });
}

bool AudioService::PlayFile(const std::string& path) {
    std::lock_guard<std::mutex> playback_lock(file_playback_mutex_);
    const uint32_t playback_generation = file_playback_generation_.load();
    if (path.size() < 5) {
        ESP_LOGW(TAG, "Invalid audio file path: %s", path.c_str());
        return false;
    }

    std::string lower_path = path;
    std::transform(lower_path.begin(), lower_path.end(), lower_path.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });

    if (EndsWith(lower_path, ".ogg")) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            ESP_LOGW(TAG, "Failed to open OGG file: %s", path.c_str());
            return false;
        }
        std::string data((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
        if (data.empty()) {
            ESP_LOGW(TAG, "OGG file is empty: %s", path.c_str());
            return false;
        }
        ESP_LOGI(TAG, "Playing OGG file: %s, size=%u", path.c_str(),
                 static_cast<unsigned>(data.size()));
        PlaySound(data);
        return true;
    }

    if (EndsWith(lower_path, ".mp3")) {
        return PlayMp3File(path, playback_generation);
    }

    if (!EndsWith(lower_path, ".wav")) {
        ESP_LOGW(TAG, "Unsupported audio file type: %s", path.c_str());
        return false;
    }

    FILE* file = fopen(path.c_str(), "rb");
    if (!file) {
        ESP_LOGW(TAG, "Failed to open WAV file: %s", path.c_str());
        return false;
    }

    uint8_t header[12];
    if (fread(header, 1, sizeof(header), file) != sizeof(header) ||
        std::memcmp(header, "RIFF", 4) != 0 ||
        std::memcmp(header + 8, "WAVE", 4) != 0) {
        ESP_LOGW(TAG, "Invalid WAV header: %s", path.c_str());
        fclose(file);
        return false;
    }

    bool has_format = false;
    bool has_data = false;
    uint16_t audio_format = 0;
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits_per_sample = 0;
    uint32_t data_size = 0;

    while (!has_data) {
        uint8_t chunk_header[8];
        if (fread(chunk_header, 1, sizeof(chunk_header), file) != sizeof(chunk_header)) {
            break;
        }

        const uint32_t chunk_size = ReadLe32(chunk_header + 4);
        if (std::memcmp(chunk_header, "fmt ", 4) == 0) {
            std::vector<uint8_t> format(chunk_size);
            if (chunk_size < 16 || fread(format.data(), 1, chunk_size, file) != chunk_size) {
                ESP_LOGW(TAG, "Invalid WAV format chunk: %s", path.c_str());
                fclose(file);
                return false;
            }
            audio_format = ReadLe16(format.data());
            channels = ReadLe16(format.data() + 2);
            sample_rate = ReadLe32(format.data() + 4);
            bits_per_sample = ReadLe16(format.data() + 14);
            has_format = true;
        } else if (std::memcmp(chunk_header, "data", 4) == 0) {
            data_size = chunk_size;
            has_data = true;
            break;
        } else {
            fseek(file, chunk_size, SEEK_CUR);
        }

        if (chunk_size & 1U) {
            fseek(file, 1, SEEK_CUR);
        }
    }

    if (!has_format || !has_data) {
        ESP_LOGW(TAG, "Missing WAV format/data chunk: %s", path.c_str());
        fclose(file);
        return false;
    }
    if (audio_format != 1 || bits_per_sample != 16 ||
        (channels != 1 && channels != 2) || sample_rate == 0) {
        ESP_LOGW(TAG, "Unsupported WAV: format=%u channels=%u rate=%u bits=%u",
                 audio_format, channels, static_cast<unsigned>(sample_rate), bits_per_sample);
        fclose(file);
        return false;
    }

    const int output_sample_rate = codec_->output_sample_rate();
    esp_ae_rate_cvt_handle_t resampler = nullptr;
    if (static_cast<int>(sample_rate) != output_sample_rate) {
        esp_ae_rate_cvt_cfg_t config = RATE_CVT_CFG(
            sample_rate, output_sample_rate, ESP_AUDIO_MONO);
        const esp_err_t result = esp_ae_rate_cvt_open(&config, &resampler);
        if (resampler == nullptr) {
            ESP_LOGE(TAG, "Failed to create WAV resampler: %d", result);
            fclose(file);
            return false;
        }
    }

    ESP_LOGI(TAG, "Playing WAV: %s, channels=%u rate=%u output_rate=%d bytes=%u",
             path.c_str(), channels, static_cast<unsigned>(sample_rate),
             output_sample_rate, static_cast<unsigned>(data_size));

    constexpr size_t kFramesPerChunk = 960;
    std::vector<uint8_t> raw(kFramesPerChunk * channels * sizeof(int16_t));
    uint32_t remaining = data_size;
    bool success = true;

    while (remaining > 0 && !service_stopped_ &&
           file_playback_generation_.load() == playback_generation) {
        const size_t requested = std::min<size_t>(raw.size(), remaining);
        const size_t bytes_read = fread(raw.data(), 1, requested, file);
        if (bytes_read == 0) {
            success = remaining == 0;
            break;
        }
        remaining -= static_cast<uint32_t>(bytes_read);

        const size_t sample_count = bytes_read / sizeof(int16_t);
        const size_t frame_count = sample_count / channels;
        std::vector<int16_t> mono(frame_count);
        const int16_t* samples = reinterpret_cast<const int16_t*>(raw.data());
        for (size_t index = 0; index < frame_count; ++index) {
            if (channels == 1) {
                mono[index] = samples[index];
            } else {
                const int32_t mixed = static_cast<int32_t>(samples[index * 2]) +
                                      static_cast<int32_t>(samples[index * 2 + 1]);
                mono[index] = static_cast<int16_t>(mixed / 2);
            }
        }

        std::vector<int16_t> pcm;
        if (resampler) {
            uint32_t maximum_output = 0;
            esp_ae_rate_cvt_get_max_out_sample_num(
                resampler, static_cast<uint32_t>(mono.size()), &maximum_output);
            pcm.resize(maximum_output);
            uint32_t actual_output = maximum_output;
            const esp_err_t result = esp_ae_rate_cvt_process(
                resampler, reinterpret_cast<esp_ae_sample_t>(mono.data()),
                static_cast<uint32_t>(mono.size()),
                reinterpret_cast<esp_ae_sample_t>(pcm.data()), &actual_output);
            if (result != ESP_OK) {
                ESP_LOGE(TAG, "WAV resampling failed: %d", result);
                success = false;
                break;
            }
            pcm.resize(actual_output);
        } else {
            pcm = std::move(mono);
        }

        auto task = std::make_unique<AudioTask>();
        task->type = kAudioTaskTypeDecodeToPlaybackQueue;
        task->pcm = std::move(pcm);
        {
            std::unique_lock<std::mutex> lock(audio_queue_mutex_);
            audio_queue_cv_.wait(lock, [this]() {
                return audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE ||
                       service_stopped_;
            });
            if (service_stopped_) {
                success = false;
                break;
            }
            audio_playback_queue_.push_back(std::move(task));
            audio_queue_cv_.notify_all();
        }
    }

    if (resampler) {
        esp_ae_rate_cvt_close(resampler);
    }
    fclose(file);
    return success && remaining == 0 &&
           file_playback_generation_.load() == playback_generation;
}

bool AudioService::PlayMp3File(const std::string& path,
                               uint32_t playback_generation) {
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) {
        ESP_LOGW(TAG, "Failed to open MP3 file: %s", path.c_str());
        return false;
    }

    esp_ae_rate_cvt_handle_t resampler = nullptr;
    bool success = true;

    if (!mp3_decoder_registered_) {
        ESP_LOGE(TAG, "MP3 decoder registry is unavailable");
        success = false;
    }

    if (success) {
        if (mp3_decoder_ == nullptr) {
            esp_audio_simple_dec_cfg_t config = {};
            config.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
            config.use_frame_dec = false;
            const esp_audio_err_t open_result =
                esp_audio_simple_dec_open(&config, &mp3_decoder_);
            if (open_result != ESP_AUDIO_ERR_OK || !mp3_decoder_) {
                ESP_LOGE(TAG, "Failed to open persistent MP3 decoder: %d",
                         open_result);
                mp3_decoder_ = nullptr;
                success = false;
            } else {
                ESP_LOGI(TAG, "Persistent MP3 decoder opened");
            }
        } else {
            const esp_audio_err_t reset_result =
                esp_audio_simple_dec_reset(mp3_decoder_);
            if (reset_result != ESP_AUDIO_ERR_OK) {
                ESP_LOGE(TAG, "Failed to reset persistent MP3 decoder: %d",
                         reset_result);
                success = false;
            } else {
                ESP_LOGI(TAG, "Persistent MP3 decoder reset for next file");
            }
        }
    }

    constexpr size_t kInputChunkBytes = 1024;
    constexpr size_t kInitialOutputBytes = 8192;
    constexpr size_t kMaximumOutputBytes = 64 * 1024;
    std::vector<uint8_t> input(kInputChunkBytes);
    std::vector<uint8_t> decoded(kInitialOutputBytes);
    uint32_t resampler_source_rate = 0;
    bool logged_audio_info = false;

    ESP_LOGI(TAG, "Playing MP3 file: %s", path.c_str());
    while (success && !service_stopped_ &&
           file_playback_generation_.load() == playback_generation) {
        const size_t bytes_read = fread(input.data(), 1, input.size(), file);
        if (bytes_read == 0) {
            if (ferror(file)) {
                ESP_LOGE(TAG, "Failed while reading MP3 file: %s", path.c_str());
                success = false;
            }
            break;
        }

        esp_audio_simple_dec_raw_t raw = {};
        raw.buffer = input.data();
        raw.len = static_cast<uint32_t>(bytes_read);
        raw.eos = feof(file) != 0;

        while (success && raw.len > 0 && !service_stopped_ &&
               file_playback_generation_.load() == playback_generation) {
            esp_audio_simple_dec_out_t output = {};
            output.buffer = decoded.data();
            output.len = static_cast<uint32_t>(decoded.size());

            esp_audio_err_t result =
                esp_audio_simple_dec_process(mp3_decoder_, &raw, &output);
            if (result == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                if (output.needed_size == 0 ||
                    output.needed_size > kMaximumOutputBytes) {
                    ESP_LOGE(TAG, "Invalid MP3 decoder output size: %u",
                             static_cast<unsigned>(output.needed_size));
                    success = false;
                    break;
                }
                decoded.resize(output.needed_size);
                continue;
            }
            if (result != ESP_AUDIO_ERR_OK || raw.consumed > raw.len) {
                ESP_LOGE(TAG, "MP3 decoding failed: result=%d consumed=%u remaining=%u",
                         result, static_cast<unsigned>(raw.consumed),
                         static_cast<unsigned>(raw.len));
                success = false;
                break;
            }

            if (output.decoded_size > 0) {
                esp_audio_simple_dec_info_t info = {};
                result = esp_audio_simple_dec_get_info(mp3_decoder_, &info);
                if (result != ESP_AUDIO_ERR_OK || info.sample_rate == 0 ||
                    info.bits_per_sample != 16 ||
                    (info.channel != 1 && info.channel != 2) ||
                    output.decoded_size % sizeof(int16_t) != 0) {
                    ESP_LOGE(TAG,
                             "Unsupported decoded MP3: result=%d rate=%u channels=%u bits=%u bytes=%u",
                             result, static_cast<unsigned>(info.sample_rate),
                             static_cast<unsigned>(info.channel),
                             static_cast<unsigned>(info.bits_per_sample),
                             static_cast<unsigned>(output.decoded_size));
                    success = false;
                    break;
                }
                if (!logged_audio_info) {
                    ESP_LOGI(TAG, "MP3 audio info: rate=%u channels=%u bits=%u",
                             static_cast<unsigned>(info.sample_rate),
                             static_cast<unsigned>(info.channel),
                             static_cast<unsigned>(info.bits_per_sample));
                    logged_audio_info = true;
                }

                const size_t sample_count =
                    output.decoded_size / sizeof(int16_t);
                const size_t frame_count = sample_count / info.channel;
                const int16_t* samples =
                    reinterpret_cast<const int16_t*>(output.buffer);
                std::vector<int16_t> mono(frame_count);
                for (size_t index = 0; index < frame_count; ++index) {
                    if (info.channel == 1) {
                        mono[index] = samples[index];
                    } else {
                        const int32_t mixed =
                            static_cast<int32_t>(samples[index * 2]) +
                            static_cast<int32_t>(samples[index * 2 + 1]);
                        mono[index] = static_cast<int16_t>(mixed / 2);
                    }
                }

                const int output_sample_rate = codec_->output_sample_rate();
                if (info.sample_rate != resampler_source_rate) {
                    if (resampler) {
                        esp_ae_rate_cvt_close(resampler);
                        resampler = nullptr;
                    }
                    resampler_source_rate = info.sample_rate;
                    if (static_cast<int>(info.sample_rate) != output_sample_rate) {
                        esp_ae_rate_cvt_cfg_t resampler_config = RATE_CVT_CFG(
                            info.sample_rate, output_sample_rate, ESP_AUDIO_MONO);
                        const esp_err_t resampler_result =
                            esp_ae_rate_cvt_open(&resampler_config, &resampler);
                        if (resampler_result != ESP_OK || !resampler) {
                            ESP_LOGE(TAG, "Failed to create MP3 resampler: %d",
                                     resampler_result);
                            success = false;
                            break;
                        }
                    }
                }

                std::vector<int16_t> pcm;
                if (resampler) {
                    uint32_t maximum_output = 0;
                    esp_ae_rate_cvt_get_max_out_sample_num(
                        resampler, static_cast<uint32_t>(mono.size()),
                        &maximum_output);
                    pcm.resize(maximum_output);
                    uint32_t actual_output = maximum_output;
                    const esp_err_t resample_result = esp_ae_rate_cvt_process(
                        resampler,
                        reinterpret_cast<esp_ae_sample_t>(mono.data()),
                        static_cast<uint32_t>(mono.size()),
                        reinterpret_cast<esp_ae_sample_t>(pcm.data()),
                        &actual_output);
                    if (resample_result != ESP_OK) {
                        ESP_LOGE(TAG, "MP3 resampling failed: %d",
                                 resample_result);
                        success = false;
                        break;
                    }
                    pcm.resize(actual_output);
                } else {
                    pcm = std::move(mono);
                }

                auto task = std::make_unique<AudioTask>();
                task->type = kAudioTaskTypeDecodeToPlaybackQueue;
                task->pcm = std::move(pcm);
                std::unique_lock<std::mutex> lock(audio_queue_mutex_);
                audio_queue_cv_.wait(lock, [this, playback_generation]() {
                    return audio_playback_queue_.size() <
                               MAX_PLAYBACK_TASKS_IN_QUEUE ||
                           service_stopped_ ||
                           file_playback_generation_.load() !=
                               playback_generation;
                });
                if (service_stopped_ ||
                    file_playback_generation_.load() != playback_generation) {
                    success = false;
                    break;
                }
                audio_playback_queue_.push_back(std::move(task));
                audio_queue_cv_.notify_all();
            }

            if (raw.consumed == 0) {
                ESP_LOGE(TAG, "MP3 decoder made no input progress");
                success = false;
                break;
            }
            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
        }
    }

    if (resampler) {
        esp_ae_rate_cvt_close(resampler);
    }
    fclose(file);

    return success && logged_audio_info &&
           file_playback_generation_.load() == playback_generation;
}

void AudioService::CancelFilePlayback() {
    file_playback_generation_.fetch_add(1);
    ResetDecoder();
}

void AudioService::ResetDecoder() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    ++decode_generation_;
    std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_reset(opus_decoder_);
    }
    decoder_lock.unlock();
    timestamp_queue_.clear();
    audio_decode_queue_.clear();
    audio_playback_queue_.clear();
    audio_testing_queue_.clear();
    audio_queue_cv_.notify_all();
}

void AudioService::CheckAndUpdateAudioPowerState() {
    auto now = std::chrono::steady_clock::now();
    auto input_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_input_time_).count();
    auto output_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_output_time_).count();
    if (input_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->input_enabled()) {
        codec_->EnableInput(false);
    }
    if (output_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->output_enabled()) {
        codec_->EnableOutput(false);
    }
    if (!codec_->input_enabled() && !codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
    }
}

void AudioService::SetModelsList(srmodel_list_t* models_list) {
    models_list_ = models_list;

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
    if (esp_srmodel_filter(models_list_, ESP_MN_PREFIX, NULL) != nullptr) {
        wake_word_ = std::make_unique<CustomWakeWord>();
    } else if (esp_srmodel_filter(models_list_, ESP_WN_PREFIX, NULL) != nullptr) {
        wake_word_ = std::make_unique<AfeWakeWord>();
    } else {
        wake_word_ = nullptr;
    }
#else
    if (esp_srmodel_filter(models_list_, ESP_WN_PREFIX, NULL) != nullptr) {
        wake_word_ = std::make_unique<EspWakeWord>();
    } else {
        wake_word_ = nullptr;
    }
#endif

    if (wake_word_) {
        wake_word_->OnWakeWordDetected([this](const std::string& wake_word) {
            if (callbacks_.on_wake_word_detected) {
                callbacks_.on_wake_word_detected(wake_word);
            }
        });
    }
}

bool AudioService::IsAfeWakeWord() {
#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
    return wake_word_ != nullptr && dynamic_cast<AfeWakeWord*>(wake_word_.get()) != nullptr;
#else
    return false;
#endif
}
