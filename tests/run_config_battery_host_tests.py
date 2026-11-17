"""Run production OTA validation and battery sampling with deterministic ADC I/O."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'components/espressif__adc_battery_estimation/adc_battery_estimation.c'


def function(text, name):
    start = text.index(name)
    start = text.rfind('\n', 0, start) + 1
    brace = text.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


with tempfile.TemporaryDirectory(prefix='vanex-config-battery-') as folder:
    folder = Path(folder)
    flags = ['-g', '-O1', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie']
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1')
    cjson = Path(os.environ['CJSON_SOURCE_DIR'])
    subprocess.run(['gcc', *flags, '-c', str(cjson/'cJSON.c'), '-o', str(folder/'cjson.o')], check=True)
    subprocess.run(['g++', '-std=c++17', *flags, '-I'+str(ROOT/'main'), '-I'+str(cjson),
        str(ROOT/'tests/ota_response_test.cc'), str(folder/'cjson.o'), '-o', str(folder/'ota')], check=True)
    subprocess.run([str(folder/'ota')], check=True, env=env)
    source = SOURCE.read_text(encoding='utf-8')
    ctx = source[source.index('typedef struct {'):source.index('} adc_battery_estimation_ctx_t;') + len('} adc_battery_estimation_ctx_t;')]
    stubs = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <assert.h>
#define CONFIG_ADC_FILTER_WINDOW_SIZE 10
#define CONFIG_BATTERY_STATE_SOFTWARE_ESTIMATION 0
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG -2
#define ESP_RETURN_ON_FALSE(test, error, ...) do { if (!(test)) return error; } while (0)
#define ESP_LOGE(...)
#define ESP_LOGD(...)
typedef int esp_err_t;
typedef void* adc_oneshot_unit_handle_t;
typedef void* adc_cali_handle_t;
typedef int adc_channel_t;
typedef int adc_unit_t;
typedef void* adc_battery_estimation_handle_t;
typedef bool (*adc_battery_charging_detect_cb_t)(void*);
typedef struct {float voltage; int capacity;} battery_point_t;
typedef struct {float capacity, voltage_capacity, battery_voltage; int adc_voltage_mv;} adc_battery_sample_t;
static int voltage_mv = 1400;
static bool fail_read = false;
static int adc_oneshot_read(void* h, int ch, int* raw) { *raw = voltage_mv; return fail_read ? ESP_FAIL : ESP_OK; }
static int adc_cali_raw_to_voltage(void* h, int raw, int* mv) { *mv = raw; return ESP_OK; }
esp_err_t adc_battery_estimation_get_sample(adc_battery_estimation_handle_t handle, adc_battery_sample_t *sample);
'''
    bodies = '\n'.join(function(source, name) for name in (
        'static float calculate_battery_capacity(', 'esp_err_t adc_battery_estimation_get_capacity(',
        'esp_err_t adc_battery_estimation_get_sample('))
    assertions = r'''
static bool charging(void* arg) { return *(bool*)arg; }
int main(void) {
    static const battery_point_t points[] = {{4.2,100},{4.06,80},{3.82,60},{3.58,40},{3.34,20},{3.1,0},{3.0,-10}};
    adc_battery_estimation_ctx_t ctx = {0};
    ctx.battery_points = points; ctx.battery_points_count = 7;
    ctx.voltage_divider_ratio = 1.0f/3.0f; ctx.filter_alpha = 0.2f;
    ctx.is_first_read = true; ctx.allow_bidirectional_capacity = true;
    adc_battery_sample_t sample = {0};
    assert(adc_battery_estimation_get_sample(NULL, &sample) == ESP_ERR_INVALID_ARG);
    assert(adc_battery_estimation_get_sample(&ctx, NULL) == ESP_ERR_INVALID_ARG);
    assert(adc_battery_estimation_get_capacity(&ctx, NULL) == ESP_ERR_INVALID_ARG);
    assert(adc_battery_estimation_get_sample(&ctx, &sample) == ESP_OK);
    assert(sample.adc_voltage_mv == 1400 && fabsf(sample.battery_voltage - 4.2f) < 0.001f);
    assert(fabsf(sample.capacity - 100) < 0.001f);
    voltage_mv = 1353; ctx.is_first_read = true;
    assert(adc_battery_estimation_get_sample(&ctx, &sample) == ESP_OK);
    assert(fabsf(sample.battery_voltage - 4.059f) < 0.001f);
    assert(sample.voltage_capacity > 79 && sample.voltage_capacity < 81);
    voltage_mv = 1400;
    for (int i=0; i<100; ++i) assert(adc_battery_estimation_get_sample(&ctx, &sample) == ESP_OK);
    assert(lroundf(sample.capacity) == 100);
    voltage_mv = 1273;
    for (int i=0; i<100; ++i) assert(adc_battery_estimation_get_sample(&ctx, &sample) == ESP_OK);
    assert(sample.capacity > 59 && sample.capacity < 61);
    float previous = ctx.last_capacity; fail_read = true;
    float capacity = 42;
    assert(adc_battery_estimation_get_capacity(&ctx, &capacity) == ESP_FAIL);
    assert(capacity == 42 && ctx.last_capacity == previous);
    fail_read = false;
    bool charge = false; ctx.charging_detect_cb = charging; ctx.charging_detect_user_data = &charge;
    ctx.allow_bidirectional_capacity = false; voltage_mv = 1400;
    assert(adc_battery_estimation_get_sample(&ctx, &sample) == ESP_OK && sample.capacity == previous);
    charge = true;
    assert(adc_battery_estimation_get_sample(&ctx, &sample) == ESP_OK && sample.capacity > previous);
    assert(calculate_battery_capacity(2.8, points, 7) == -10);
    assert(calculate_battery_capacity(4.3, points, 7) == 100);
    puts("Battery sampling: voltage/SOC consistency, charge/discharge, failure preservation and legacy mode passed");
}
'''
    fixture = folder / 'battery.c'
    fixture.write_text(stubs + ctx + bodies + assertions)
    subprocess.run(['gcc', '-std=c11', *flags, str(fixture), '-lm', '-o', str(folder/'battery')], check=True)
    subprocess.run([str(folder/'battery')], check=True, env=env)
    wake = (ROOT/'main/audio/wake_words/afe_wake_word.cc').read_text(encoding='utf-8')
    header = (ROOT/'main/audio/wake_words/afe_wake_word.h').read_text(encoding='utf-8')
    chunks = header[header.index('    struct PcmDeleter'):header.index('    srmodel_list_t')]
    wake_fixture = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#define CONFIG_SPIRAM 1
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static int allocations = 0;
static bool fail_allocation = false;
void* heap_caps_malloc(size_t size, uint32_t caps) {
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (fail_allocation) return nullptr;
    ++allocations; return std::malloc(size);
}
void heap_caps_free(void* data) { if (data) --allocations; std::free(data); }
class AfeWakeWord {
public:
''' + chunks + r'''
    std::deque<PcmChunk> wake_word_pcm_;
    std::mutex wake_word_pcm_mutex_;
    void StoreWakeWordData(const int16_t* data, size_t samples);
};
''' + function(wake, 'void AfeWakeWord::StoreWakeWordData(') + r'''
int main() {
    {
        AfeWakeWord wake;
        int16_t pcm[512] = {};
        wake.StoreWakeWordData(nullptr, 512);
        wake.StoreWakeWordData(pcm, 0);
        assert(allocations == 0);
        for (int i=0; i<500; ++i) {
            pcm[0] = i; wake.StoreWakeWordData(pcm, 512);
            assert(allocations <= 66);
        }
        assert(allocations == 66 && wake.wake_word_pcm_.size() == 66);
        assert(wake.wake_word_pcm_.front().data.get()[0] == 434);
        assert(wake.wake_word_pcm_.back().data.get()[0] == 499);
        fail_allocation = true;
        wake.StoreWakeWordData(pcm, 512);
        assert(allocations == 66);
        std::deque<AfeWakeWord::PcmChunk> snapshot;
        snapshot.swap(wake.wake_word_pcm_);
        assert(wake.wake_word_pcm_.empty() && snapshot.size() == 66);
    }
    assert(allocations == 0);
    std::cout << "Wake history: PSRAM-only allocation, 500-frame bound, snapshot ownership and OOM passed\n";
}
'''
    fixture = folder/'wake.cc'
    fixture.write_text(wake_fixture)
    subprocess.run(['g++', '-std=c++17', *flags, str(fixture), '-pthread', '-o', str(folder/'wake')], check=True)
    subprocess.run([str(folder/'wake')], check=True, env=env)
