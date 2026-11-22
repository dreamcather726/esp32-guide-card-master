#include "rndis_board.h"
#include "display.h"
#include "application.h"
#include "system_info.h"
#include "settings.h"
#include "assets/lang_config.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_network.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <utility>
#include <font_awesome.h>
#include <usb/usb_helpers.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <vector>

#if CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S3

static const char *TAG = "RndisBoard";
#define EVENT_GOT_IP_BIT (1 << 0)
#define EVENT_USB_DEVICE_BIT (1 << 1)
#define EVENT_USB_GONE_BIT (1 << 2)
#define EVENT_USB_AT_RX_BIT (1 << 3)
#define EVENT_USB_MAINTENANCE_BIT (1 << 4)

static constexpr uint16_t kMl307UsbVendorId = 0x2ECC;
static constexpr uint32_t kExistingRndisGracePeriodMs = 8000;
static constexpr uint32_t kUsbEnumerationTimeoutMs = 12000;

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

static const usb_device_match_id_t kMl307UsbMatchList[] = {
    {
        .match_flags = static_cast<usb_dev_match_flags_t>(USB_DEVICE_ID_MATCH_VENDOR),
        .idVendor = kMl307UsbVendorId,
    },
    {.match_flags = static_cast<usb_dev_match_flags_t>(0)},
};

RndisBoard::RndisBoard(bool initialize_usb, bool monitor_signal, bool power_root_port_early)
    : monitor_signal_(monitor_signal), power_root_port_early_(power_root_port_early) {
    if (initialize_usb) {
        // Install the USB host early so the native USB interrupt is reserved.
        // Event consumption is deferred until both the board AT callback and
        // the RNDIS callback have been registered in StartNetwork().
        InitializeUsbCdcHost();
    } else {
        ESP_LOGI(TAG, "USB RNDIS initialization skipped for WiFi mode");
    }
}

RndisBoard::~RndisBoard() {
}

std::string RndisBoard::GetBoardType() {
    return "rndis";
}

void RndisBoard::SetNetworkDiagnostic(DiagnosticChannel channel, const char* message, bool fault) {
    if (auto* display = GetDisplay()) {
        if (fault) display->SetDiagnostic(channel, message);
        else display->SetDiagnosticStatus(channel, message);
    }
}

void RndisBoard::StartNetwork() {
     ESP_ERROR_CHECK(esp_netif_init());
     esp_err_t event_loop_result = esp_event_loop_create_default();
     if (event_loop_result != ESP_OK && event_loop_result != ESP_ERR_INVALID_STATE) {
         ESP_ERROR_CHECK(event_loop_result);
     }

     if (!InitializeUsbCdcHost()) {
         SetNetworkDiagnostic(DiagnosticChannel::Usb, "USB HOST ERROR");
         ESP_LOGE(TAG, "USB CDC host is unavailable; keep the application running without network");
         OnNetworkEvent(NetworkEvent::Disconnected);
         return;
     }
     esp_event_handler_register(IOT_ETH_EVENT, ESP_EVENT_ANY_ID, iot_event_handle, this);
     esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, iot_event_handle, this);
     esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, iot_event_handle, this);

     if (!install_rndis(USB_DEVICE_VENDOR_ANY, USB_DEVICE_PRODUCT_ANY, "USB RNDIS0")) {
         SetNetworkDiagnostic(DiagnosticChannel::Network, "4G RNDIS INIT ERROR");
         OnNetworkEvent(NetworkEvent::Disconnected);
         return;
     }
     BaseType_t task_created = xTaskCreate(
         NetworkTaskEntry, "usb_rndis_net", 6144, this, 5, &network_task_handle_);
     if (task_created != pdPASS) {
         SetNetworkDiagnostic(DiagnosticChannel::Usb, "USB TASK NO MEM");
         ESP_LOGE(TAG, "Failed to create USB RNDIS network task");
         OnNetworkEvent(NetworkEvent::Disconnected);
         return;
     }

     ESP_LOGI(TAG,
              "USB startup scheduled: preserve existing RNDIS link, read IMEI over USB AT, dial only when offline");
     OnNetworkEvent(NetworkEvent::Connecting, "USB RNDIS");

     // Both the board AT callback and the RNDIS callback are now registered.
     // Only start consuming USB events after this point so a modem that stayed
     // powered during an ESP32 reset cannot have its enumeration event lost.
     esp_err_t start_result = usbh_cdc_start_event_handling();
     if (start_result != ESP_OK) {
         SetNetworkDiagnostic(DiagnosticChannel::Usb, "USB PORT START ERROR");
         ESP_LOGE(TAG, "USB CDC event handling start failed: %s",
                  esp_err_to_name(start_result));
         OnNetworkEvent(NetworkEvent::Disconnected);
     }
}

bool RndisBoard::InitializeUsbCdcHost() {
     if (cdc_driver_ready_) {
         return true;
     }
     if (!s_event_group) {
         s_event_group = xEventGroupCreate();
         if (!s_event_group) {
             ESP_LOGE(TAG, "Failed to create USB event group");
             return false;
         }
     }

     usbh_cdc_driver_config_t config = {
         .task_stack_size = 1024 * 5,
         .task_priority = 5,
         .task_coreid = 0,
         .skip_init_usb_host_driver = false,
         .defer_event_handling = true,
         .power_root_port_early = power_root_port_early_,
     };
     esp_err_t cdc_result = usbh_cdc_driver_install(&config);
     if (cdc_result != ESP_OK && cdc_result != ESP_ERR_INVALID_STATE) {
         ESP_LOGE(TAG, "USB CDC host installation failed: %s", esp_err_to_name(cdc_result));
         return false;
     }

     esp_err_t callback_result = usbh_cdc_register_dev_event_cb(
         kMl307UsbMatchList, UsbDeviceEventCallback, this);
     if (callback_result != ESP_OK) {
         ESP_LOGE(TAG, "ML307 USB event callback registration failed: %s",
                  esp_err_to_name(callback_result));
         return false;
     }
     cdc_driver_ready_ = true;
     ESP_LOGI(TAG, "USB CDC host reserved before peripheral initialization");
     return true;
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
    imei_.clear();
    cellular_csq_ = -1;
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
            imei_.clear();
            cellular_csq_ = -1;
            usb_at_dial_succeeded_ = false;
        }
    }
    if (!matched) {
        ESP_LOGW(TAG, "[USB_DIAG] ignoring stale ML307 disconnect: addr=%u", device_addr);
        return;
    }
    ESP_LOGW(TAG, "[USB_DIAG] ML307 USB device disconnected: addr=%u", device_addr);
    SetNetworkDiagnostic(DiagnosticChannel::Usb, "USB DISCONNECTED");
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
    std::vector<std::string> completed_lines;
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
        {
            std::lock_guard<std::mutex> lock(board->usb_at_rx_mutex_);
            if (board->usb_at_rx_buffer_.size() < 4096) {
                const size_t remaining = 4096 - board->usb_at_rx_buffer_.size();
                board->usb_at_rx_buffer_.append(
                    reinterpret_cast<const char*>(buffer.data()),
                    std::min(read_length, remaining));
            }

            for (size_t index = 0; index < read_length; ++index) {
                const char value = static_cast<char>(buffer[index]);
                if (value == '\r' || value == '\n') {
                    if (!board->usb_at_line_buffer_.empty()) {
                        completed_lines.push_back(std::move(board->usb_at_line_buffer_));
                        board->usb_at_line_buffer_.clear();
                    }
                } else if (board->usb_at_line_buffer_.size() < 1024) {
                    board->usb_at_line_buffer_.push_back(value);
                } else {
                    ESP_LOGW(TAG, "[USB_AT] dropping oversized response line");
                    board->usb_at_line_buffer_.clear();
                }
            }
        }
    }
    state_lock.unlock();
    xEventGroupSetBits(board->s_event_group, EVENT_USB_AT_RX_BIT);

    std::function<void(const std::string&)> line_callback;
    {
        std::lock_guard<std::mutex> lock(board->usb_at_line_callback_mutex_);
        line_callback = board->usb_at_line_callback_;
    }
    if (line_callback) {
        for (const auto& line : completed_lines) {
            line_callback(line);
        }
    }
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
            board->cellular_csq_ = -1;
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
    std::lock_guard<std::mutex> command_lock(usb_at_command_mutex_);
    response.clear();
    usbh_cdc_port_handle_t port = nullptr;
    std::unique_lock<std::mutex> state_lock(usb_state_mutex_);
    port = usb_at_port_;
    const uint32_t session = usb_session_;
    if (!port || (expected_session != 0 && expected_session != session)) {
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

bool RndisBoard::ExecuteUsbAtCommand(const std::string& command,
                                     std::string& response,
                                     uint32_t timeout_ms) {
    return SendUsbAtCommand(command, response, timeout_ms);
}

void RndisBoard::SetUsbAtLineCallback(
    std::function<void(const std::string&)> callback) {
    std::lock_guard<std::mutex> lock(usb_at_line_callback_mutex_);
    usb_at_line_callback_ = std::move(callback);
}

std::string RndisBoard::GetImei() {
    {
        std::lock_guard<std::mutex> lock(usb_state_mutex_);
        if (!imei_.empty()) {
            return imei_;
        }
    }
    if (IsCellularNetwork()) {
        return {};  // Never identify a newly connected modem using an old modem's NVS entry.
    }
    Settings settings("device", false);
    const auto cached = settings.GetString("imei");
    return ml307_at::IsImei(cached) ? cached : std::string{};
}

bool RndisBoard::QueryAndCacheImei(uint32_t session) {
    static constexpr std::array<const char*, 3> kImeiCommands = {
        "AT+CGSN=1", "AT+GSN", "AT+CGSN"
    };

    for (const char* command : kImeiCommands) {
        std::string response;
        if (!SendUsbAtCommand(command, response, 3000, session)) {
            continue;
        }
        std::string imei = ml307_at::ExtractImei(response);
        if (imei.empty()) {
            ESP_LOGW(TAG, "[USB_AT] %s did not return a standalone 15-digit IMEI",
                     command);
            continue;
        }

        std::lock_guard<std::mutex> lock(usb_state_mutex_);
        if (usb_session_ != session || !usb_device_addr_ || !usb_at_port_) {
            return false;
        }
        imei_ = imei;
        Settings settings("device", true);
        if (settings.GetString("imei") != imei) {
            settings.SetString("imei", imei);
        }
        ESP_LOGI(TAG, "[USB_AT] ML307 IMEI parsed: %s", imei.c_str());
        return true;
    }

    ESP_LOGE(TAG, "[USB_AT] unable to read a valid 15-digit ML307 IMEI");
    return false;
}

bool RndisBoard::OpenAndProbeUsbAtInterface(uint8_t device_addr, uint8_t interface_number,
                                           uint32_t session, bool& out_of_memory) {
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
        out_of_memory |= open_result == ESP_ERR_NO_MEM;
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
        usb_at_line_buffer_.clear();
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

    std::lock_guard<std::mutex> command_lock(usb_at_command_mutex_);
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
    bool out_of_memory = false;
    for (uint8_t candidate : candidates) {
        if (OpenAndProbeUsbAtInterface(device_addr, candidate, session, out_of_memory)) {
            std::string response;
            SendUsbAtCommand("ATE0", response, 2000, session);
            OnUsbAtReady();
            return true;
        }
    }
    if (HasUsbModem()) {
        SetNetworkDiagnostic(DiagnosticChannel::Usb, out_of_memory ? "AT NO MEM" : "AT NO RESPONSE");
    }
    ESP_LOGW(TAG, "[USB_AT] no AT response on eligible control interfaces; background retry scheduled");
    return false;
}

bool RndisBoard::DetectAndDialUsbAt() {
    uint32_t session = 0;
    bool identity_ready = false;
    bool dial_already_accepted = false;
    {
        std::lock_guard<std::mutex> lock(usb_state_mutex_);
        session = usb_session_;
        identity_ready = !imei_.empty();
        dial_already_accepted = usb_at_dial_succeeded_;
    }
    if (!EnsureUsbAtControlPort(session)) {
        return false;
    }

    if (monitor_signal_) {
        std::string response;
        const int csq = SendUsbAtCommand("AT+CSQ", response, 3000, session)
            ? ml307_at::ParseCsq(response) : -1;
        std::lock_guard<std::mutex> lock(usb_state_mutex_);
        if (usb_session_ == session && usb_at_port_) cellular_csq_ = csq;
    }

    // Identity/URCs are needed even when DHCP succeeded before the AT task ran.
    if (!identity_ready) {
        identity_ready = QueryAndCacheImei(session);
    }
    if (HasUsbModem()) {
        SetNetworkDiagnostic(DiagnosticChannel::Usb, identity_ready ? "USB IMEI OK" : "IMEI READ ERROR",
                             !identity_ready);
    }
    if (IsNetworkReady()) {
        ESP_LOGD(TAG, "[USB_AT] RNDIS online; preserve data session, IMEI=%s; no dial-up",
                 identity_ready ? "ready" : "pending");
        return identity_ready;
    }
    if (dial_already_accepted) {
        const bool ip_ready = IsNetworkReady();
        SetNetworkDiagnostic(DiagnosticChannel::Network, ip_ready ? "4G IP OK" : "4G NO IP", !ip_ready);
        return identity_ready;
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
        const bool ip_ready = IsNetworkReady();
        SetNetworkDiagnostic(DiagnosticChannel::Network, ip_ready ? "4G IP OK" : "4G WAIT IP", !ip_ready);
        ESP_LOGI(TAG, "[USB_AT] RNDIS dial-up command accepted; waiting for USB RNDIS enumeration");
    } else {
        const bool ip_ready = IsNetworkReady();
        SetNetworkDiagnostic(DiagnosticChannel::Network, ip_ready ? "4G IP OK" : "4G DIAL ERROR", !ip_ready);
        ESP_LOGE(TAG, "[USB_AT] RNDIS dial-up command failed");
    }
    return dialed && identity_ready;
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
                if (!IsNetworkReady()) {
                    ESP_LOGW(TAG, "[USB_BOOT] ML307 has not enumerated after %lu ms; waiting for USB attach",
                             static_cast<unsigned long>(kUsbEnumerationTimeoutMs));
                    OnNetworkEvent(NetworkEvent::ModemDetecting, "ML307 USB");
                }
            }
            failures = 0;
            wait = portMAX_DELAY;
            continue;
        }
        if (bits & EVENT_USB_DEVICE_BIT) {
            SetNetworkDiagnostic(DiagnosticChannel::Usb, "USB ML307 OK", false);
            failures = 0;
            if (!IsNetworkReady()) {
                OnNetworkEvent(NetworkEvent::Connecting, "ML307 USB");
                // Only the network worker waits; display/audio/UI keep running.
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
            wait = !IsNetworkReady() ? pdMS_TO_TICKS(30000) :
                !monitor_signal_ ? portMAX_DELAY :
                pdMS_TO_TICKS(GetCellularSignal() >= 0 ? 10000 : 5000);
        } else {
            const uint32_t retry_ms = ml307_at::RetryDelayMs(failures);
            failures = std::min<unsigned>(failures + 1, 4);
            wait = pdMS_TO_TICKS(retry_ms);
            ESP_LOGW(TAG, "[USB_AT] control/identity pending; retry in %lu ms, RNDIS=%s",
                     static_cast<unsigned long>(retry_ms), IsNetworkReady() ? "online" : "offline");
        }
    }
}

static constexpr const char* kPrimaryDnsServer = "223.5.5.5";
static constexpr const char* kBackupDnsServer = "119.29.29.29";

static void SetRndisDnsServer(esp_netif_t* netif, esp_netif_dns_type_t type, const char* address, const char* name) {
    esp_netif_dns_info_t dns_info = {};
    dns_info.ip.type = IPADDR_TYPE_V4;
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
        dhcp_dns.ip.type == IPADDR_TYPE_V4 && dhcp_dns.ip.u_addr.ip4.addr != 0) {
        ESP_LOGI(TAG, "USB RNDIS DHCP DNS: " IPSTR, IP2STR(&dhcp_dns.ip.u_addr.ip4));
    } else {
        ESP_LOGW(TAG, "USB RNDIS DHCP did not provide a usable DNS server");
        SetRndisDnsServer(netif, ESP_NETIF_DNS_MAIN, kPrimaryDnsServer, "primary fallback");
    }

    esp_netif_dns_info_t backup_dns = {};
    if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_BACKUP, &backup_dns) != ESP_OK ||
        backup_dns.ip.type != IPADDR_TYPE_V4 || backup_dns.ip.u_addr.ip4.addr == 0) {
        SetRndisDnsServer(netif, ESP_NETIF_DNS_BACKUP, kBackupDnsServer, "secondary fallback");
    }
}


void RndisBoard::iot_event_handle(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == IOT_ETH_EVENT) {
        switch (event_id) {
        case IOT_ETH_EVENT_START:
            ESP_LOGI(TAG, "IOT_ETH_EVENT_START");
            break;
        case IOT_ETH_EVENT_STOP:
            ESP_LOGI(TAG, "IOT_ETH_EVENT_STOP");
            break;
        case IOT_ETH_EVENT_CONNECTED:
            ESP_LOGI(TAG, "IOT_ETH_EVENT_CONNECTED; waiting for DHCP address");
            static_cast<RndisBoard*>(arg)->OnNetworkEvent(NetworkEvent::Connecting, "USB RNDIS");
            break;
        case IOT_ETH_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "IOT_ETH_EVENT_DISCONNECTED");
            static_cast<RndisBoard*>(arg)->HandleLinkLoss();
            break;
        default:
            ESP_LOGI(TAG, "IOT_ETH_EVENT_UNKNOWN");
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_LOST_IP) {
        static_cast<RndisBoard*>(arg)->HandleLinkLoss();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP) {
        auto* board = static_cast<RndisBoard*>(arg);
        const auto* event = static_cast<const ip_event_got_ip_t*>(event_data);
        if (!event || event->esp_netif != board->s_rndis_netif) return;
        // Configure DNS before waking tasks waiting on EVENT_GOT_IP_BIT. The
        // previous order allowed the first MQTT HTTP request to race DNS setup.
        ConfigureRndisDns(board->s_rndis_netif);
        xEventGroupSetBits(board->s_event_group, EVENT_GOT_IP_BIT | EVENT_USB_MAINTENANCE_BIT);
        board->SetNetworkDiagnostic(DiagnosticChannel::Network, "4G IP OK", false);
        ESP_LOGI(TAG, "4G IP OK: " IPSTR ", gateway: " IPSTR,
                 IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.gw));
        board->OnNetworkEvent(NetworkEvent::Connected, "USB RNDIS");
    }
}


void RndisBoard::HandleLinkLoss() {
    {
        std::lock_guard<std::mutex> lock(usb_state_mutex_);
        xEventGroupClearBits(s_event_group, EVENT_GOT_IP_BIT);
        usb_at_dial_succeeded_ = false;
    }
    xEventGroupSetBits(s_event_group, EVENT_USB_MAINTENANCE_BIT);
    SetNetworkDiagnostic(DiagnosticChannel::Network, "4G LINK DOWN");
    OnNetworkEvent(NetworkEvent::Disconnected);
}

void RndisBoard::OnNetworkEvent(NetworkEvent event, const std::string& data) {
    switch (event) {
        case NetworkEvent::Connected:
            ESP_LOGI(TAG, "USB RNDIS network connected: %s", data.c_str());
            break;
        case NetworkEvent::Scanning:
            ESP_LOGI(TAG, "USB RNDIS scanning event");
            break;
        case NetworkEvent::Connecting:
            ESP_LOGI(TAG, "USB RNDIS connecting: %s", data.c_str());
            break;
        case NetworkEvent::Disconnected:
            ESP_LOGW(TAG, "USB RNDIS disconnected");
            break;
        default:
            break;
    }

    // Notify external callback if set
    if (network_event_callback_) {
        network_event_callback_(event, data);
    }
}

void RndisBoard::SetNetworkEventCallback(NetworkEventCallback callback) {
    network_event_callback_ = std::move(callback);
}

bool RndisBoard::install_rndis(uint16_t idVendor, uint16_t idProduct, const char *netif_name)
{
    esp_err_t ret = ESP_OK;
    iot_eth_handle_t eth_handle = nullptr;
    iot_eth_netif_glue_handle_t glue = nullptr;

    usb_device_match_id_t *dev_match_id = (usb_device_match_id_t*)calloc(2, sizeof(usb_device_match_id_t));
    if (dev_match_id == nullptr) return false;
    dev_match_id[0].match_flags = static_cast<usb_dev_match_flags_t>(
        USB_DEVICE_ID_MATCH_VENDOR | USB_DEVICE_ID_MATCH_PRODUCT);
    dev_match_id[0].idVendor = idVendor;
    dev_match_id[0].idProduct = idProduct;
    memset(&dev_match_id[1], 0, sizeof(usb_device_match_id_t)); // end of list
    iot_usbh_rndis_config_t rndis_cfg = {
        .match_id_list = dev_match_id,
    };

    ret = iot_eth_new_usb_rndis(&rndis_cfg, &rndis_eth_driver);
    if (ret != ESP_OK || rndis_eth_driver == NULL) {
        ESP_LOGE(TAG, "Failed to create USB RNDIS driver");
        free(dev_match_id);
        return false;
    }

    iot_eth_config_t eth_cfg = {
        .driver = rndis_eth_driver,
        .stack_input = NULL,
    };
    ret = iot_eth_install(&eth_cfg, &eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install USB RNDIS driver");
        return false;
    }

    esp_netif_inherent_config_t _inherent_eth_config = ESP_NETIF_INHERENT_DEFAULT_ETH();
    _inherent_eth_config.if_key = netif_name;
    _inherent_eth_config.if_desc = netif_name;
    esp_netif_config_t netif_cfg = {
        .base = &_inherent_eth_config,
        .driver = NULL,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    s_rndis_netif = esp_netif_new(&netif_cfg);
    if (s_rndis_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create network interface");
        return false;
    }

    glue = iot_eth_new_netif_glue(eth_handle);
    if (glue == NULL) {
        ESP_LOGE(TAG, "Failed to create netif glue");
        return false;
    }
    return esp_netif_attach(s_rndis_netif, glue) == ESP_OK && iot_eth_start(eth_handle) == ESP_OK;
}


NetworkInterface* RndisBoard::GetNetwork() {
    static EspNetwork network;
    return &network;
}

const char* RndisBoard::GetNetworkStateIcon() {
    return FONT_AWESOME_SIGNAL_STRONG;
}

std::string RndisBoard::GetBoardJson() {

    std::string json = R"({"type":")" + std::string(BOARD_TYPE) + R"(",)";
    json += R"("name":")" + std::string(BOARD_NAME) + R"(",)";

    json += R"("mac":")" + SystemInfo::GetMacAddress() + R"(")";
    const std::string imei = GetImei();
    if (!imei.empty()) {
        json += R"(,"imei":")" + imei + R"(")";
    }
    json += "}";
    return json;
}

bool RndisBoard::WaitForNetworkReady(uint32_t timeout_ms) {
    if (!s_event_group) {
        return false;
    }
    const EventBits_t bits = xEventGroupWaitBits(
        s_event_group, EVENT_GOT_IP_BIT | EVENT_USB_GONE_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return (bits & EVENT_GOT_IP_BIT) != 0;
}

bool RndisBoard::IsNetworkReady() const {
    return s_event_group != nullptr &&
           (xEventGroupGetBits(s_event_group) & EVENT_GOT_IP_BIT) != 0;
}

bool RndisBoard::GetRndisIpInfo(esp_netif_ip_info_t& info) const {
    return IsNetworkReady() && s_rndis_netif != nullptr &&
           esp_netif_get_ip_info(s_rndis_netif, &info) == ESP_OK &&
           info.ip.addr != 0 && info.gw.addr != 0;
}

bool RndisBoard::HasUsbModem() {
    std::lock_guard<std::mutex> lock(usb_state_mutex_);
    return usb_device_addr_ != 0;
}

void RndisBoard::SetPowerSaveLevel(PowerSaveLevel level) {

}

std::string RndisBoard::GetDeviceStatusJson() {
    auto& board = Board::GetInstance();
    auto root = cJSON_CreateObject();

    // Audio speaker
    auto audio_speaker = cJSON_CreateObject();
    if (auto codec = board.GetAudioCodec()) {
        cJSON_AddNumberToObject(audio_speaker, "volume", codec->output_volume());
    }
    cJSON_AddItemToObject(root, "audio_speaker", audio_speaker);

    // Screen
    auto screen = cJSON_CreateObject();
    if (auto backlight = board.GetBacklight()) {
        cJSON_AddNumberToObject(screen, "brightness", backlight->brightness());
    }
    if (auto display = board.GetDisplay(); display && display->height() > 64) {
        if (auto theme = display->GetTheme()) {
            cJSON_AddStringToObject(screen, "theme", theme->name().c_str());
        }
    }
    cJSON_AddItemToObject(root, "screen", screen);

    // Battery
    int level = 0;
    bool charging = false, discharging = false;
    if (board.GetBatteryLevel(level, charging, discharging)) {
        auto battery = cJSON_CreateObject();
        cJSON_AddNumberToObject(battery, "level", level);
        cJSON_AddBoolToObject(battery, "charging", charging);
        cJSON_AddItemToObject(root, "battery", battery);
    }

    // Network
    auto network = cJSON_CreateObject();
    cJSON_AddStringToObject(network, "type", "rndis");
    cJSON_AddItemToObject(root, "network", network);

    // Chip temperature
    float temp = 0.0f;
    if (board.GetTemperature(temp)) {
        auto chip = cJSON_CreateObject();
        cJSON_AddNumberToObject(chip, "temperature", temp);
        cJSON_AddItemToObject(root, "chip", chip);
    }

    auto str = cJSON_PrintUnformatted(root);
    std::string result(str);
    cJSON_free(str);
    cJSON_Delete(root);
    return result;
}
#endif // CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S3
