#include <esp_check.h>

#include "rndis_board.h"

#include "iot_eth_netif_glue.h"
#include "iot_usbh_rndis.h"

#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/task.h>
#include <usb/usb_helpers.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>

static const char *TAG = "RndisBoard";
#define EVENT_GOT_IP_BIT (1 << 0)
#define EVENT_USB_DEVICE_BIT (1 << 1)
#define EVENT_USB_GONE_BIT (1 << 2)
#define EVENT_USB_AT_RX_BIT (1 << 3)
#define EVENT_USB_MAINTENANCE_BIT (1 << 4)

static constexpr uint16_t kMl307UsbVendorId = 0x2ECC;
static constexpr uint32_t kExistingRndisGracePeriodMs = 8000;
static constexpr uint32_t kUsbEnumerationTimeoutMs = 12000;
static constexpr uint32_t kModemRetryMarker = 0x4d4c3337;
RTC_NOINIT_ATTR static uint32_t modem_retry_marker;

static bool ParseCregRegistration(const std::string& response, int& status) {
    const size_t marker = response.find("+CREG:");
    if (marker == std::string::npos) {
        return false;
    }

    size_t value_start = marker + 6;
    while (value_start < response.size() &&
           std::isspace(static_cast<unsigned char>(response[value_start]))) {
        ++value_start;
    }
    const size_t comma = response.find(',', value_start);
    if (comma != std::string::npos) {
        value_start = comma + 1;
        while (value_start < response.size() &&
               std::isspace(static_cast<unsigned char>(response[value_start]))) {
            ++value_start;
        }
    }
    if (value_start >= response.size() ||
        !std::isdigit(static_cast<unsigned char>(response[value_start]))) {
        return false;
    }

    char* end = nullptr;
    const long parsed = std::strtol(response.c_str() + value_start, &end, 10);
    if (end == response.c_str() + value_start || parsed < 0 || parsed > 9) {
        return false;
    }
    status = static_cast<int>(parsed);
    return true;
}

static constexpr auto kMl307UsbMatchList = [] {
    std::array<usb_device_match_id_t, 2> matches{};
    matches[0].match_flags = static_cast<usb_dev_match_flags_t>(USB_DEVICE_ID_MATCH_VENDOR);
    matches[0].idVendor = kMl307UsbVendorId;
    return matches;
}();

void RndisBoard::StartNetwork() {
    if (esp_reset_reason() != ESP_RST_SW) {
        modem_retry_marker = 0;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    if (!InitializeUsbCdcHost()) {
        ESP_LOGE(TAG, "USB CDC host is unavailable");
        return;
    }
    ESP_ERROR_CHECK(esp_event_handler_register(IOT_ETH_EVENT, ESP_EVENT_ANY_ID, iot_event_handle, this));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, iot_event_handle, this));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, iot_event_handle, this));
    if (!InstallRndis()) {
        ESP_LOGE(TAG, "Could not initialize the 4G network interface");
        return;
    }
    if (xTaskCreate(NetworkTaskEntry, "usb_rndis_net", 6144, this, 5, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Could not create the 4G network task");
        return;
    }

    // Register both the modem and RNDIS callbacks before processing USB events.
    const esp_err_t result = usbh_cdc_start_event_handling();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "USB CDC event handling start failed: %s", esp_err_to_name(result));
    }
}

bool RndisBoard::InitializeUsbCdcHost() {
    s_event_group = xEventGroupCreate();
    if (!s_event_group) {
        ESP_LOGE(TAG, "Failed to create USB event group");
        return false;
    }
    const usbh_cdc_driver_config_t config = {
        .task_stack_size = 5 * 1024,
        .task_priority = 5,
        .task_coreid = 0,
        .skip_init_usb_host_driver = false,
        .defer_event_handling = true,
        .power_root_port_early = modem_retry_marker == kModemRetryMarker,
    };
    if (const esp_err_t result = usbh_cdc_driver_install(&config); result != ESP_OK) {
        ESP_LOGE(TAG, "USB CDC host installation failed: %s", esp_err_to_name(result));
        return false;
    }
    const esp_err_t result = usbh_cdc_register_dev_event_cb(
        kMl307UsbMatchList.data(), UsbDeviceEventCallback, this);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "ML307 USB event callback registration failed: %s", esp_err_to_name(result));
    }
    return result == ESP_OK;
}

void RndisBoard::UsbDeviceEventCallback(usbh_cdc_device_event_t event,
                                        usbh_cdc_device_event_data_t *event_data,
                                        void *user_ctx) {
    auto* board = static_cast<RndisBoard*>(user_ctx);
    if (!board || !event_data) {
        return;
    }

    if (event == CDC_HOST_DEVICE_EVENT_CONNECTED) {
        board->HandleUsbDeviceConnected(*event_data);
    } else if (event == CDC_HOST_DEVICE_EVENT_DISCONNECTED) {
        board->HandleUsbDeviceDisconnected(event_data->dev_gone.dev_addr);
    }
}

void RndisBoard::HandleUsbDeviceConnected(const usbh_cdc_device_event_data_t& event_data) {
    const auto* device = event_data.new_dev.device_desc;
    const auto* config = event_data.new_dev.active_config_desc;
    if (!device || !config) {
        ESP_LOGW(TAG, "ML307 USB connected without complete descriptors");
        return;
    }

    std::lock_guard<std::mutex> lock(usb_state_mutex_);
    usb_device_addr_ = event_data.new_dev.dev_addr;
    usb_device_pid_ = device->idProduct;
    ++usb_session_;
    usb_interface_count_ = 0;
    usb_at_port_ = nullptr;
    usb_at_dial_succeeded_ = false;

    ESP_LOGI(TAG,
             "[USB_DIAG] ML307 connected: addr=%u VID:PID=%04x:%04x class=%02x/%02x/%02x interfaces=%u total_len=%u",
             usb_device_addr_, device->idVendor, device->idProduct,
             device->bDeviceClass, device->bDeviceSubClass, device->bDeviceProtocol,
             config->bNumInterfaces, config->wTotalLength);

    for (uint8_t interface_number = 0; interface_number < 16; ++interface_number) {
        int offset = 0;
        const usb_intf_desc_t* interface_desc =
            usb_parse_interface_descriptor(config, interface_number, 0, &offset);
        if (!interface_desc) {
            continue;
        }

        ESP_LOGI(TAG,
                 "[USB_DIAG] interface=%u alt=%u class=%02x subclass=%02x protocol=%02x endpoints=%u",
                 interface_desc->bInterfaceNumber, interface_desc->bAlternateSetting,
                 interface_desc->bInterfaceClass, interface_desc->bInterfaceSubClass,
                 interface_desc->bInterfaceProtocol, interface_desc->bNumEndpoints);

        if (usb_interface_count_ < usb_interfaces_.size()) {
            usb_interfaces_[usb_interface_count_] = {
                interface_desc->bInterfaceNumber, interface_desc->bInterfaceClass,
                interface_desc->bInterfaceSubClass, interface_desc->bInterfaceProtocol,
                interface_desc->bNumEndpoints};
            ++usb_interface_count_;
        }
    }

    // A new enumeration is a new transport session. Clear stale status from
    // the previous USB address before publishing the device event.
    xEventGroupClearBits(s_event_group, EVENT_USB_GONE_BIT | EVENT_GOT_IP_BIT);
    xEventGroupSetBits(s_event_group, EVENT_USB_DEVICE_BIT);
}

void RndisBoard::HandleUsbDeviceDisconnected(uint8_t device_addr) {
    bool matched = false;
    {
        std::lock_guard<std::mutex> lock(usb_state_mutex_);
        if (usb_device_addr_ == device_addr) {
            matched = true;
            usb_device_addr_ = 0;
            usb_device_pid_ = 0;
            ++usb_session_;
            usb_interface_count_ = 0;
            usb_at_port_ = nullptr;
            usb_at_dial_succeeded_ = false;
        }
    }
    if (!matched) {
        ESP_LOGW(TAG, "[USB_DIAG] ignoring stale ML307 disconnect: addr=%u", device_addr);
        return;
    }
    ESP_LOGW(TAG, "[USB_DIAG] ML307 USB device disconnected: addr=%u", device_addr);
    xEventGroupClearBits(s_event_group, EVENT_GOT_IP_BIT | EVENT_USB_DEVICE_BIT);
    xEventGroupSetBits(s_event_group, EVENT_USB_GONE_BIT | EVENT_USB_AT_RX_BIT |
                                     EVENT_USB_MAINTENANCE_BIT);
}

void RndisBoard::UsbAtReceiveCallback(usbh_cdc_port_handle_t port_handle, void *user_data) {
    auto* board = static_cast<RndisBoard*>(user_data);
    if (!board) {
        return;
    }

    std::array<uint8_t, 512> buffer = {};
    std::unique_lock<std::mutex> state_lock(board->usb_state_mutex_);
    if (board->usb_at_port_ != port_handle) {
        return;
    }
    while (true) {
        size_t available = 0;
        if (usbh_cdc_get_rx_buffer_size(port_handle, &available) != ESP_OK || available == 0) {
            break;
        }
        size_t read_length = std::min(available, buffer.size());
        if (usbh_cdc_read_bytes(port_handle, buffer.data(), &read_length, 0) != ESP_OK ||
            read_length == 0) {
            break;
        }
        std::lock_guard<std::mutex> lock(board->usb_at_rx_mutex_);
        constexpr size_t kMaxResponseBytes = 4096;
        const size_t remaining = kMaxResponseBytes - board->usb_at_rx_buffer_.size();
        board->usb_at_rx_buffer_.append(
            reinterpret_cast<const char*>(buffer.data()), std::min(read_length, remaining));
    }
    state_lock.unlock();
    xEventGroupSetBits(board->s_event_group, EVENT_USB_AT_RX_BIT);
}

void RndisBoard::UsbAtClosedCallback(usbh_cdc_port_handle_t port_handle, void *user_data) {
    auto* board = static_cast<RndisBoard*>(user_data);
    if (!board) {
        return;
    }
    bool matched = false;
    {
        std::lock_guard<std::mutex> lock(board->usb_state_mutex_);
        if (board->usb_at_port_ == port_handle) {
            board->usb_at_port_ = nullptr;
            matched = true;
        }
    }
    ESP_LOGW(TAG, "[USB_AT] control port closed");
    xEventGroupSetBits(board->s_event_group, EVENT_USB_AT_RX_BIT |
        (matched ? EVENT_USB_MAINTENANCE_BIT : 0));
}

bool RndisBoard::SendUsbAtCommand(const std::string& command,
                                  std::string& response,
                                  uint32_t timeout_ms, uint32_t expected_session) {
    response.clear();
    std::unique_lock<std::mutex> state_lock(usb_state_mutex_);
    const auto port = usb_at_port_;
    const uint32_t session = usb_session_;
    if (!port || expected_session != session) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(usb_at_rx_mutex_);
        usb_at_rx_buffer_.clear();
    }
    xEventGroupClearBits(s_event_group, EVENT_USB_AT_RX_BIT);

    std::string wire_command = command;
    if (wire_command.size() < 2 ||
        wire_command.compare(wire_command.size() - 2, 2, "\r\n") != 0) {
        wire_command += "\r\n";
    }
    esp_err_t write_result = usbh_cdc_write_bytes(
        port, reinterpret_cast<const uint8_t*>(wire_command.data()),
        wire_command.size(), pdMS_TO_TICKS(1000));
    state_lock.unlock();
    if (write_result != ESP_OK) {
        ESP_LOGW(TAG, "[USB_AT] write %s failed: %s", command.c_str(),
                 esp_err_to_name(write_result));
        return false;
    }

    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() - start < timeout) {
        xEventGroupWaitBits(s_event_group, EVENT_USB_AT_RX_BIT, pdTRUE, pdFALSE,
                            pdMS_TO_TICKS(100));
        {
            std::lock_guard<std::mutex> state_lock(usb_state_mutex_);
            if (usb_at_port_ != port || usb_session_ != session) {
                response.clear();
                return false;
            }
            std::lock_guard<std::mutex> lock(usb_at_rx_mutex_);
            response = usb_at_rx_buffer_;
        }
        if (ml307_at::ParseReply(response) != ml307_at::Reply::Pending) {
            break;
        }
    }

    std::string printable = response;
    std::replace(printable.begin(), printable.end(), '\r', ' ');
    std::replace(printable.begin(), printable.end(), '\n', ' ');
    ESP_LOGI(TAG, "[USB_AT] %s -> %s", command.c_str(), printable.c_str());
    std::lock_guard<std::mutex> lock(usb_state_mutex_);
    return usb_at_port_ == port && usb_session_ == session &&
           ml307_at::ParseReply(response) == ml307_at::Reply::Ok;
}

bool RndisBoard::OpenAndProbeUsbAtInterface(uint8_t device_addr, uint8_t interface_number,
                                           uint32_t session) {
    {
        std::lock_guard<std::mutex> lock(usb_state_mutex_);
        if (usb_session_ != session || usb_device_addr_ != device_addr) {
            return false;
        }
    }
    ESP_LOGI(TAG, "[USB_AT] probing interface %u", interface_number);
    usbh_cdc_port_config_t port_config = {
        .dev_addr = device_addr,
        .itf_num = interface_number,
        .in_ringbuf_size = 0,
        .out_ringbuf_size = 0,
        .in_transfer_buffer_size = 256,
        .out_transfer_buffer_size = 512,
        .cbs = {
            .closed = UsbAtClosedCallback,
            .recv_data = UsbAtReceiveCallback,
            .notif_cb = nullptr,
            .user_data = this,
        },
        .flags = USBH_CDC_FLAGS_DISABLE_NOTIFICATION,
    };

    usbh_cdc_port_handle_t candidate_port = nullptr;
    esp_err_t open_result = usbh_cdc_port_open(&port_config, &candidate_port);
    if (open_result != ESP_OK || !candidate_port) {
        constexpr uint32_t dma_caps = MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL;
        ESP_LOGW(TAG, "[USB_AT] interface %u cannot be opened: %s; dma_free=%u dma_largest=%u internal_free=%u",
                 interface_number, esp_err_to_name(open_result),
                 static_cast<unsigned>(heap_caps_get_free_size(dma_caps)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(dma_caps)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(usb_state_mutex_);
        if (usb_session_ != session || usb_device_addr_ != device_addr) {
            // The CDC driver owns automatic cleanup after a disconnect.
            return false;
        }
        usb_at_port_ = candidate_port;
        std::lock_guard<std::mutex> rx_lock(usb_at_rx_mutex_);
        usb_at_rx_buffer_.clear();
    }
    vTaskDelay(pdMS_TO_TICKS(250));

    std::string response;
    bool detected = false;
    for (int attempt = 1; attempt <= 2 && !detected; ++attempt) {
        ESP_LOGI(TAG, "[USB_AT] send AT on interface %u, attempt=%d/2",
                 interface_number, attempt);
        detected = SendUsbAtCommand("AT", response, 2000, session);
    }
    if (detected) {
        ESP_LOGI(TAG, "[USB_AT] ML307 AT control interface detected: %u", interface_number);
        return true;
    }

    bool close_candidate = false;
    {
        std::lock_guard<std::mutex> lock(usb_state_mutex_);
        if (usb_session_ == session && usb_at_port_ == candidate_port) {
            usb_at_port_ = nullptr;
            close_candidate = true;
        }
    }
    if (close_candidate) {
        usbh_cdc_port_close(candidate_port);
    }
    return false;
}

bool RndisBoard::EnsureUsbAtControlPort(uint32_t session) {
    uint8_t device_addr = 0;
    uint16_t device_pid = 0;
    std::array<ml307_at::Interface, 16> interfaces = {};
    size_t interface_count = 0;
    {
        std::lock_guard<std::mutex> lock(usb_state_mutex_);
        if (usb_session_ != session || !usb_device_addr_) {
            return false;
        }
        if (usb_at_port_) {
            return true;
        }
        device_addr = usb_device_addr_;
        device_pid = usb_device_pid_;
        interfaces = usb_interfaces_;
        interface_count = usb_interface_count_;
    }

    ESP_LOGI(TAG, "[USB_AT] detect control port for PID=%04x", device_pid);
    const auto candidates = ml307_at::ControlCandidates(interfaces.data(), interface_count);
    for (uint8_t candidate : candidates) {
        if (OpenAndProbeUsbAtInterface(device_addr, candidate, session)) {
            std::string response;
            SendUsbAtCommand("ATE0", response, 2000, session);
            return true;
        }
    }
    ESP_LOGW(TAG, "[USB_AT] no AT response on eligible control interfaces");
    return false;
}

bool RndisBoard::DetectAndDialUsbAt() {
    if (IsNetworkReady()) {
        return true;
    }
    uint32_t session = 0;
    {
        std::lock_guard<std::mutex> lock(usb_state_mutex_);
        session = usb_session_;
        if (usb_at_dial_succeeded_) {
            return true;
        }
    }
    if (!EnsureUsbAtControlPort(session)) {
        return false;
    }
    if (IsNetworkReady()) {
        return true;
    }

    std::string response;
    SendUsbAtCommand("AT+CPIN?", response, 3000, session);

    int registration_status = -1;
    response.clear();
    if (SendUsbAtCommand("AT+CREG?", response, 2000, session) &&
        ParseCregRegistration(response, registration_status)) {
        ESP_LOGI(TAG, "[USB_AT] AT+CREG? registration=%d",
                 registration_status);
        if (registration_status != 1 && registration_status != 5) {
            ESP_LOGW(TAG,
                     "[USB_AT] cellular registration is not ready yet; continue with MDIALUP retries");
        }
    } else {
        // Some ML307 firmware variants return ERROR for CREG while their RNDIS
        // data path is already usable. Keep this query diagnostic-only.
        ESP_LOGW(TAG,
                 "[USB_AT] AT+CREG? is unsupported or not ready; continue with MDIALUP: %s",
                 response.c_str());
    }

    bool dialed = false;
    for (int attempt = 1; attempt <= 5; ++attempt) {
        {
            std::lock_guard<std::mutex> lock(usb_state_mutex_);
            if (usb_session_ != session || !usb_device_addr_ || !usb_at_port_) {
                return false;
            }
        }
        if (IsNetworkReady()) {
            ESP_LOGI(TAG,
                     "[USB_AT] RNDIS already has an IP address; no further MDIALUP retry is needed");
            dialed = true;
            break;
        }
        ESP_LOGI(TAG, "[USB_AT] send AT+MDIALUP=1,1, attempt=%d/5", attempt);
        if (SendUsbAtCommand("AT+MDIALUP=1,1", response, 6000, session)) {
            dialed = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (dialed) {
        std::lock_guard<std::mutex> lock(usb_state_mutex_);
        if (usb_session_ != session || !usb_device_addr_) {
            return false;
        }
        usb_at_dial_succeeded_ = true;
        ESP_LOGI(TAG, "[USB_AT] Dial accepted; waiting for RNDIS DHCP");
    } else {
        ESP_LOGE(TAG, "[USB_AT] RNDIS dial-up command failed");
    }
    return dialed;
}

void RndisBoard::NetworkTaskEntry(void *arg) {
    static_cast<RndisBoard*>(arg)->NetworkTask();
}

void RndisBoard::NetworkTask() {
    TickType_t wait = pdMS_TO_TICKS(kUsbEnumerationTimeoutMs);
    unsigned failures = 0;
    while (true) {
        const EventBits_t bits = xEventGroupWaitBits(
            s_event_group, EVENT_USB_DEVICE_BIT | EVENT_USB_MAINTENANCE_BIT,
            pdTRUE, pdFALSE, wait);
        uint32_t session = 0;
        bool modem_present = false;
        {
            std::lock_guard<std::mutex> lock(usb_state_mutex_);
            modem_present = usb_device_addr_ != 0;
            session = usb_session_;
            if (modem_present && !(bits & (EVENT_USB_DEVICE_BIT | EVENT_USB_MAINTENANCE_BIT)) &&
                wait == pdMS_TO_TICKS(30000) && !IsNetworkReady()) {
                // A dial command accepted without a subsequent IP event must
                // not suppress all future recovery attempts.
                usb_at_dial_succeeded_ = false;
            }
        }
        if (!modem_present) {
            if (wait == pdMS_TO_TICKS(kUsbEnumerationTimeoutMs)) {
                if (bits & (EVENT_USB_DEVICE_BIT | EVENT_USB_MAINTENANCE_BIT)) continue;
                OnEnumerationTimeout();
            }
            failures = 0;
            wait = portMAX_DELAY;
            continue;
        }
        if (bits & EVENT_USB_DEVICE_BIT) {
            failures = 0;
            if (!IsNetworkReady()) {
                xEventGroupWaitBits(s_event_group, EVENT_GOT_IP_BIT | EVENT_USB_GONE_BIT,
                                    pdFALSE, pdFALSE, pdMS_TO_TICKS(kExistingRndisGracePeriodMs));
            }
        }
        {
            std::lock_guard<std::mutex> lock(usb_state_mutex_);
            if (usb_session_ != session || !usb_device_addr_) {
                wait = portMAX_DELAY;
                continue;
            }
        }

        if (DetectAndDialUsbAt()) {
            failures = 0;
            wait = IsNetworkReady() ? portMAX_DELAY : pdMS_TO_TICKS(30000);
        } else {
            const uint32_t retry_ms = ml307_at::RetryDelayMs(failures);
            failures = std::min<unsigned>(failures + 1, 4);
            wait = pdMS_TO_TICKS(retry_ms);
            ESP_LOGW(TAG, "[USB_AT] AT dial pending; retry in %lu ms, RNDIS=%s",
                     static_cast<unsigned long>(retry_ms), IsNetworkReady() ? "online" : "offline");
        }
    }
}

static constexpr const char* kPrimaryDnsServer = "223.5.5.5";
static constexpr const char* kBackupDnsServer = "119.29.29.29";

static void SetRndisDnsServer(esp_netif_t* netif, esp_netif_dns_type_t type, const char* address, const char* name) {
    esp_netif_dns_info_t dns_info = {};
    dns_info.ip.type = ESP_IPADDR_TYPE_V4;
    dns_info.ip.u_addr.ip4.addr = esp_ip4addr_aton(address);

    esp_err_t ret = esp_netif_set_dns_info(netif, type, &dns_info);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "USB RNDIS %s DNS set to %s", name, address);
    } else {
        ESP_LOGE(TAG, "set USB RNDIS %s DNS to %s failed: %s", name, address, esp_err_to_name(ret));
    }
}

static void ConfigureRndisDns(esp_netif_t* netif) {
    if (!netif) {
        ESP_LOGE(TAG, "cannot configure USB RNDIS DNS: netif is null");
        return;
    }

    esp_netif_dns_info_t dhcp_dns = {};
    if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dhcp_dns) == ESP_OK &&
        dhcp_dns.ip.type == ESP_IPADDR_TYPE_V4 && dhcp_dns.ip.u_addr.ip4.addr != 0) {
        ESP_LOGI(TAG, "USB RNDIS DHCP DNS: " IPSTR, IP2STR(&dhcp_dns.ip.u_addr.ip4));
    } else {
        ESP_LOGW(TAG, "USB RNDIS DHCP did not provide a usable DNS server");
    }

    SetRndisDnsServer(netif, ESP_NETIF_DNS_MAIN, kPrimaryDnsServer, "primary");
    SetRndisDnsServer(netif, ESP_NETIF_DNS_BACKUP, kBackupDnsServer, "backup");
}


void RndisBoard::iot_event_handle(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* board = static_cast<RndisBoard*>(arg);
    if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP) {
        const auto* event = static_cast<const ip_event_got_ip_t*>(event_data);
        if (!event || event->esp_netif != board->s_rndis_netif) {
            return;
        }
        ConfigureRndisDns(board->s_rndis_netif);
        ESP_LOGI(TAG, "4G IP OK: " IPSTR ", gateway: " IPSTR,
                 IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.gw));
        xEventGroupSetBits(board->s_event_group, EVENT_GOT_IP_BIT | EVENT_USB_MAINTENANCE_BIT);
    } else if ((event_base == IOT_ETH_EVENT && event_id == IOT_ETH_EVENT_DISCONNECTED) ||
               (event_base == IP_EVENT && event_id == IP_EVENT_ETH_LOST_IP)) {
        {
            std::lock_guard<std::mutex> lock(board->usb_state_mutex_);
            xEventGroupClearBits(board->s_event_group, EVENT_GOT_IP_BIT);
            board->usb_at_dial_succeeded_ = false;
        }
        xEventGroupSetBits(board->s_event_group, EVENT_USB_MAINTENANCE_BIT);
        ESP_LOGW(TAG, "4G link or IP lost; waiting for recovery");
    } else if (event_base == IOT_ETH_EVENT && event_id == IOT_ETH_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "USB RNDIS link up; waiting for DHCP");
    }
}

bool RndisBoard::InstallRndis() {
    const iot_usbh_rndis_config_t rndis_config = {
        .match_id_list = ESP_USB_DEVICE_MATCH_ID_ANY,
    };
    iot_eth_driver_t* driver = nullptr;
    ESP_RETURN_ON_FALSE(iot_eth_new_usb_rndis(&rndis_config, &driver) == ESP_OK && driver,
                        false, TAG, "Failed to create USB RNDIS driver");

    const iot_eth_config_t eth_config = {
        .driver = driver,
        .stack_input = nullptr,
    };
    iot_eth_handle_t eth = nullptr;
    ESP_RETURN_ON_FALSE(iot_eth_install(&eth_config, &eth) == ESP_OK, false, TAG,
                        "Failed to install USB RNDIS driver");

    esp_netif_inherent_config_t inherent = ESP_NETIF_INHERENT_DEFAULT_ETH();
    inherent.if_key = "USB_RNDIS0";
    inherent.if_desc = "USB RNDIS";
    const esp_netif_config_t netif_config = {
        .base = &inherent,
        .driver = nullptr,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    s_rndis_netif = esp_netif_new(&netif_config);
    ESP_RETURN_ON_FALSE(s_rndis_netif, false, TAG, "Failed to create RNDIS network interface");

    const auto glue = iot_eth_new_netif_glue(eth);
    ESP_RETURN_ON_FALSE(glue, false, TAG, "Failed to create RNDIS netif glue");
    ESP_RETURN_ON_FALSE(esp_netif_attach(s_rndis_netif, glue) == ESP_OK, false, TAG,
                        "Failed to attach RNDIS network interface");
    return iot_eth_start(eth) == ESP_OK;
}

bool RndisBoard::IsNetworkReady() const {
    return s_event_group != nullptr &&
           (xEventGroupGetBits(s_event_group) & EVENT_GOT_IP_BIT) != 0;
}

bool RndisBoard::HasUsbModem() {
    std::lock_guard<std::mutex> lock(usb_state_mutex_);
    return usb_device_addr_ != 0;
}

void RndisBoard::OnEnumerationTimeout() {
    if (HasUsbModem() || IsNetworkReady()) {
        return;
    }
    if (modem_retry_marker != kModemRetryMarker) {
        modem_retry_marker = kModemRetryMarker;
        ESP_LOGW(TAG, "[USB_BOOT] Retrying ML307 with early USB port power");
        esp_restart();
    }
    ESP_LOGW(TAG, "[USB_BOOT] ML307 did not enumerate; waiting for USB attach");
}
