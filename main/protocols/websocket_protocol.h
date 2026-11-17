#ifndef _WEBSOCKET_PROTOCOL_H_
#define _WEBSOCKET_PROTOCOL_H_


#include "protocol.h"
#include "user_binding.h"
#include <atomic>
#include <mutex>

#include <web_socket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#define WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)
#define WEBSOCKET_PROTOCOL_USER_CONTEXT_EVENT (1 << 1)
#define WEBSOCKET_PROTOCOL_CONTEXT_ERROR_EVENT (1 << 2)

class WebsocketProtocol : public Protocol {
public:
    WebsocketProtocol();
    ~WebsocketProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel() override;
    bool IsAudioChannelOpened() const override;

private:
    EventGroupHandle_t event_group_handle_;
    std::unique_ptr<WebSocket> websocket_;
    int version_ = 1;
    bool binding_required_ = false;
    UserBindingSnapshot channel_binding_;
    std::atomic<bool> channel_ready_{false};
    std::atomic<bool> context_failed_{false};
    std::atomic<uint32_t> channel_generation_{0};
    std::mutex context_mutex_;

    void ParseServerHello(const cJSON* root);
    void ParseUserContext(const cJSON* root);
    void FailUserContext();
    bool SendText(const std::string& text) override;
    std::string GetHelloMessage();
};

#endif
