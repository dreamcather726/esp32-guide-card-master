from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ConfigBatteryIntegrationTests(unittest.TestCase):
    def test_error_response_is_rejected_before_any_configuration_write(self):
        source = (ROOT/'main/ota.cc').read_text(encoding='utf-8')
        check = source[source.index('esp_err_t Ota::CheckVersion()'):source.index('void Ota::MarkCurrentVersionValid()')]
        self.assertLess(check.index('ota_response::IsConfiguration(root)'), check.index('Settings settings("mqtt", true)'))
        self.assertLess(check.index('has_new_version_ = false'), check.index('http->Open'))
        self.assertIn('return last_error == ESP_OK ? ESP_FAIL : last_error', check)
        self.assertNotIn('std::stoi', source)

    def test_adc_pin_is_not_used_as_charge_detection(self):
        board = (ROOT/'main/boards/jiuchuan-s3/jiuchuan_dev_board.cc').read_text(encoding='utf-8')
        self.assertIn('new PowerManager(GPIO_NUM_NC)', board)
        self.assertNotIn('new PowerManager(PWR_ADC_GPIO)', board)
        power = (ROOT/'main/boards/jiuchuan-s3/power_manager.h').read_text(encoding='utf-8')
        self.assertIn('allow_bidirectional_capacity = charging_pin_ == GPIO_NUM_NC', power)
        self.assertIn('adc_battery_estimation_get_sample', power)
        self.assertIn('voltage=%.3fV adc=%dmV curve=%.1f%%', power)
        self.assertLess(power.index('adc_battery_estimation_create(&config)'), power.index('esp_timer_start_periodic'))

    def test_exhausted_foreground_check_schedules_retry(self):
        source = (ROOT/'main/application.cc').read_text(encoding='utf-8')
        retry = source[source.index('if (retry_count >= MAX_RETRY)'):source.index('char error_message[128]')]
        self.assertIn('deferred_ota_check_needed_ = true', retry)
        self.assertIn('provisioning pending', source)
        self.assertIn('app->has_server_time_ = true', source)


if __name__ == '__main__':
    unittest.main()
