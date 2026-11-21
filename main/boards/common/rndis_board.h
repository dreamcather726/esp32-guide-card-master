#ifndef RNDIS_BOARD_H
#define RNDIS_BOARD_H

#include "iot_eth.h"
#include "iot_usbh_cdc.h"
#include "ml307_at.h"

#include <esp_event.h>
#include <esp_netif.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include <array>
#include <mutex>
#include <string>

class RndisBoard {
public:
    void StartNetwork();

private:
    EventGroupHandle_t s_event_group = nullptr;
    esp_netif_t* s_rndis_netif = nullptr;
    usbh_cdc_port_handle_t usb_at_port_ = nullptr;
    std::mutex usb_state_mutex_;
    std::mutex usb_at_rx_mutex_;
    std::string usb_at_rx_buffer_;
    uint8_t usb_device_addr_ = 0;
    uint16_t usb_device_pid_ = 0;
    uint32_t usb_session_ = 0;
    std::array<ml307_at::Interface, 16> usb_interfaces_ = {};
    size_t usb_interface_count_ = 0;
    bool usb_at_dial_succeeded_ = false;

    bool InitializeUsbCdcHost();
    bool InstallRndis();
    bool IsNetworkReady() const;
    bool HasUsbModem();
    void OnEnumerationTimeout();
    static void iot_event_handle(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
    static void UsbDeviceEventCallback(usbh_cdc_device_event_t event,
                                       usbh_cdc_device_event_data_t* event_data, void* user_ctx);
    static void UsbAtReceiveCallback(usbh_cdc_port_handle_t port_handle, void* user_data);
    static void UsbAtClosedCallback(usbh_cdc_port_handle_t port_handle, void* user_data);
    static void NetworkTaskEntry(void* arg);
    void HandleUsbDeviceConnected(const usbh_cdc_device_event_data_t& event_data);
    void HandleUsbDeviceDisconnected(uint8_t device_addr);
    void NetworkTask();
    bool DetectAndDialUsbAt();
    bool EnsureUsbAtControlPort(uint32_t session);
    bool OpenAndProbeUsbAtInterface(uint8_t device_addr, uint8_t interface_number, uint32_t session);
    bool SendUsbAtCommand(const std::string& command, std::string& response,
                          uint32_t timeout_ms, uint32_t expected_session);
};

#endif
