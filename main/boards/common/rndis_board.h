#ifndef RNDIS_BOARD_H
#define RNDIS_BOARD_H

#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S3
#include "board.h"
#include "iot_eth.h"
#include "iot_usbh_rndis.h"
#include "iot_usbh_cdc.h"
#include "iot_eth_netif_glue.h"
#include "ml307_at.h"
#include <esp_netif.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>

#include <array>
#include <functional>
#include <mutex>

class RndisBoard : public Board {
private:
    EventGroupHandle_t s_event_group = nullptr;
    iot_eth_driver_t *rndis_eth_driver = nullptr;
    esp_netif_t *s_rndis_netif = nullptr;
    TaskHandle_t network_task_handle_ = nullptr;
    usbh_cdc_port_handle_t usb_at_port_ = nullptr;
    std::mutex usb_state_mutex_;
    std::mutex usb_at_command_mutex_;
    std::mutex usb_at_rx_mutex_;
    std::mutex usb_at_line_callback_mutex_;
    std::string usb_at_rx_buffer_;
    std::string usb_at_line_buffer_;
    std::function<void(const std::string&)> usb_at_line_callback_;
    std::string imei_;
    uint8_t usb_device_addr_ = 0;
    uint16_t usb_device_pid_ = 0;
    uint32_t usb_session_ = 0;
    std::array<ml307_at::Interface, 16> usb_interfaces_ = {};
    size_t usb_interface_count_ = 0;
    bool usb_at_dial_succeeded_ = false;
    bool cdc_driver_ready_ = false;

    bool InitializeUsbCdcHost();
    void install_rndis(uint16_t idVendor, uint16_t idProduct, const char *netif_name);
    static void iot_event_handle(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
    static void UsbDeviceEventCallback(usbh_cdc_device_event_t event,
                                       usbh_cdc_device_event_data_t *event_data,
                                       void *user_ctx);
    static void UsbAtReceiveCallback(usbh_cdc_port_handle_t port_handle, void *user_data);
    static void UsbAtClosedCallback(usbh_cdc_port_handle_t port_handle, void *user_data);
    static void NetworkTaskEntry(void *arg);
    void HandleUsbDeviceConnected(const usbh_cdc_device_event_data_t& event_data);
    void HandleUsbDeviceDisconnected(uint8_t device_addr);
    void NetworkTask();
    bool DetectAndDialUsbAt();
    bool EnsureUsbAtControlPort(uint32_t session);
    bool OpenAndProbeUsbAtInterface(uint8_t device_addr, uint8_t interface_number, uint32_t session);
    bool SendUsbAtCommand(const std::string& command, std::string& response,
                          uint32_t timeout_ms, uint32_t expected_session = 0);
    bool QueryAndCacheImei(uint32_t session);
protected:
    NetworkEventCallback network_event_callback_ = nullptr;

    virtual std::string GetBoardJson() override;

    /** Handle network events emitted by the USB RNDIS netif. */
    void OnNetworkEvent(NetworkEvent event, const std::string& data = "");

public:
    explicit RndisBoard(bool initialize_usb = true);
    virtual ~RndisBoard();
    
    virtual std::string GetBoardType() override;
    
    /**
     * Start network connection asynchronously
     * This function returns immediately. Network events are notified through the callback set by SetNetworkEventCallback().
     */
    virtual void StartNetwork() override;
    
    virtual NetworkInterface* GetNetwork() override;
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) override;
    virtual const char* GetNetworkStateIcon() override;
    virtual bool IsCellularNetwork() const override { return true; }
    virtual void SetPowerSaveLevel(PowerSaveLevel level) override;
    virtual AudioCodec* GetAudioCodec() override { return nullptr; }
    virtual std::string GetDeviceStatusJson() override;

    /** Return the 15-digit IMEI read from the ML307 over USB AT. */
    std::string GetImei();

    /** Execute a serialized command on the detected ML307 USB AT interface. */
    bool ExecuteUsbAtCommand(const std::string& command,
                             std::string& response,
                             uint32_t timeout_ms = 5000);

    /** Receive complete unsolicited AT response lines, such as +MGNSSLOC. */
    void SetUsbAtLineCallback(std::function<void(const std::string&)> callback);

    /** Wait until USB RNDIS has received an IP address from the modem. */
    bool WaitForNetworkReady(uint32_t timeout_ms);
    bool IsNetworkReady() const;
    
};
#endif // CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S3

#endif // RNDIS_BOARD_H
