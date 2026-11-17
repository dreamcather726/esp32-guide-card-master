"""No-build integration guards; runtime/host C++ tests are separate."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


def source(path):
    return (ROOT / path).read_text(encoding="utf-8")


def function(text, start, end):
    return text.split(start, 1)[1].split(end, 1)[0]


class BindingIntegrationSourceTests(unittest.TestCase):
    def setUp(self):
        self.app = source("main/application.cc")
        self.ws = source("main/protocols/websocket_protocol.cc")
        self.mqtt = source("main/boards/jiuchuan-s3/location_mqtt_client.cc")

    def test_boot_id_uses_existing_random_uuid_not_nvs(self):
        board = source("main/boards/common/board.cc")
        self.assertIn("user_binding_(GenerateUuid())", board)
        self.assertIn("esp_fill_random(uuid, sizeof(uuid))", board)
        for text in (board, self.mqtt, source("main/boards/jiuchuan-s3/jiuchuan_dev_board.cc")):
            self.assertNotIn('GetString("user_guid")', text)
            self.assertNotIn('SetString("user_guid"', text)

    def test_change_closes_before_replacing_identity(self):
        change = function(self.app, "void Application::ApplyUserBinding(", "void Application::SetAecMode(")
        self.assertLess(change.index("CloseAudioChannel()"), change.index("board.ApplyUserBindingCommand"))
        self.assertLess(change.index("EnableVoiceProcessing(false)"), change.index("board.ApplyUserBindingCommand"))
        self.assertNotIn("SendUserContext", self.app + source("main/protocols/protocol.cc"))

    def test_hello_uses_snapshot_and_explicit_opt_in(self):
        hello = function(self.ws, "std::string WebsocketProtocol::GetHelloMessage()", "void WebsocketProtocol::ParseServerHello")
        for key in ("userGUID", "bootID", "bindingVersion", "bindingToken", "usageSessionID", "user_binding"):
            self.assertIn('"' + key + '"', hello)
        self.assertIn('SetHeader("User-Binding", "1")', self.ws)
        self.assertNotIn("GetUserGuid()", self.ws)

    def test_both_handshake_events_and_fresh_event_bits(self):
        opening = function(self.ws, "bool WebsocketProtocol::OpenAudioChannel()", "std::string WebsocketProtocol::GetHelloMessage()")
        self.assertIn("xEventGroupClearBits", opening)
        self.assertIn("WEBSOCKET_PROTOCOL_USER_CONTEXT_EVENT", opening)
        self.assertIn("(received & required) != required", opening)
        self.assertIn("context_failed_", opening)
        self.assertIn("pdMS_TO_TICKS(binding_required_ ? 30000 : 10000)", opening)

    def test_audio_and_text_have_verified_gate(self):
        for method, end in (("SendAudio", "bool WebsocketProtocol::SendText"),
                            ("SendText", "bool WebsocketProtocol::IsAudioChannelOpened")):
            body = function(self.ws, "bool WebsocketProtocol::" + method, end)
            self.assertIn("if (!IsAudioChannelOpened())", body)
        self.assertIn("binding.SameIdentity(channel_binding_)", self.ws)
        self.assertIn("ScheduleAudioChannelTask", self.app)

    def test_unbound_does_not_enter_ai_or_restore_mqtt_fallback(self):
        for method, end in (("HandleToggleChatEvent", "HandleStartListeningEvent"),
                            ("HandleStartListeningEvent", "HandleStopListeningEvent"),
                            ("HandleWakeWordDetectedEvent", "HandleStateChangedEvent")):
            body = function(self.app, "void Application::" + method, "void Application::" + end)
            self.assertIn("CanStartPersonalChat()", body)
        init = function(self.app, "void Application::InitializeProtocol()", "protocol_->OnConnected")
        self.assertLess(init.index("board.RequiresUserBinding()"), init.index("ota_->HasMqttConfig()"))
        self.assertIn('PlaySdAudioFile("SafetyReminder.mp3")', self.app)
        self.assertIn("ScheduleDeferredOtaCheck();", self.app)

    def test_report_has_state_but_no_token(self):
        report = function(self.mqtt, "bool LocationMqttClient::PublishOnlineReport()", "void LocationMqttClient::PersistRuntimeCheckpoint")
        for key in ("bootID", "bindingState", "bindingVersion", "usageSessionID", "macAddress"):
            self.assertIn('"' + key + '"', report)
        self.assertIn('AddString(root, "macAddress", SystemInfo::GetMacAddress())', report)
        self.assertIn('#include "system_info.h"', self.mqtt)
        self.assertIn('AddString(root, "userGUID", binding.user_guid)', report)
        self.assertIn('AddString(root, "bindingState", BindingStateName(binding.state))', report)
        self.assertNotIn("bindingToken", report)
        self.assertNotIn("binding.token", report)

    def test_group_assignment_precedes_binding_and_skips_retained_commands(self):
        handler = function(self.mqtt, "void LocationMqttClient::HandleMqttPayload(", "void LocationMqttClient::QuerySignalStrength()")
        group = function(handler, "std::string new_group_id;", "const std::string binding_command")
        self.assertIn("const bool has_group_id = !retained && TryFirstJsonScalar(", group)
        for operation in ('settings.SetString("group_id", group_id_)',
                          "UpdateGroupSubscription(old_group_topic, new_group_topic)",
                          "HandleContentGroupId(new_group_id)", "report_requested_ = true"):
            self.assertIn(operation, group)
        self.assertIn("if (has_group_id && new_group_id != group_id_)", group)
        self.assertIn("if (has_group_id && !new_group_id.empty())", group)
        for mutation in ("ApplyUserBinding", "PublishExecution", "BindingState::",
                         "user_playback_baseline_seconds_", "user_conversation_baseline_"):
            self.assertNotIn(mutation, group)

    def test_binding_auto_responses_are_ignored_before_group_or_binding(self):
        guard = function(self.mqtt,
                         'if (topic == command_topic_ &&\n        (cJSON_HasObjectItem(message, "deviceExecute")',
                         "std::string new_group_id;")
        self.assertIn('cJSON_HasObjectItem(message, "downSDfile")', guard)
        self.assertIn('cJSON_HasObjectItem(message, "bindingAuto")', guard)
        self.assertIn('BindingJsonString(message, "cmd") == "bindingAuto"', guard)
        self.assertIn("return;", guard)
        self.assertNotIn("Publish", guard)
        self.assertNotIn("ApplyUserBinding", guard)

    def test_retained_binding_rejected_before_legacy_or_explicit_validation(self):
        binding = function(self.mqtt, "if (binding_requested) {", 'const std::string device = JsonScalar(message, "device")')
        retained = function(binding, "if (retained) {", "if (IsLegacyUserBindingRequest(message))")
        self.assertIn('PublishExecution(BindingJsonString(message, "cmdID"), false,', retained)
        self.assertIn("retained user binding requests are not accepted", retained)
        self.assertIn("return;", retained)
        self.assertNotIn("ApplyUserBinding", retained)
        self.assertNotIn("ParseUserBindingCommand", retained)
        self.assertLess(binding.index("if (retained)"), binding.index("ParseUserBindingCommand(message)"))

    def test_legacy_request_is_pending_not_a_local_binding(self):
        legacy = function(self.mqtt, "if (IsLegacyUserBindingRequest(message)) {", "const auto command = ParseUserBindingCommand(message)")
        self.assertIn('AddString(response.get(), "deviceExecute", "pending")', legacy)
        self.assertIn('AddString(response.get(), "cmdID", BindingJsonString(message, "cmdID"))', legacy)
        self.assertIn("awaiting server-issued bindUser", legacy)
        self.assertIn("identity unchanged", legacy)
        self.assertIn("PublishCommandPayload(response_text)", legacy)
        self.assertIn("return;", legacy)
        for mutation in ("ApplyUserBinding", "PublishExecution", "SetString", "BindingState::",
                         "user_playback_baseline_seconds_", "user_conversation_baseline_"):
            self.assertNotIn(mutation, legacy)
        self.assertNotIn('"OK"', legacy)
        self.assertNotIn('"ready"', legacy)

    def test_retained_flag_is_carried_from_first_fragment_through_dispatch(self):
        header = source("main/boards/jiuchuan-s3/location_mqtt_client.h")
        self.assertIn("bool incoming_retained_ = false;", header)
        self.assertIn("bool retained);", header)
        events = function(self.mqtt, "void LocationMqttClient::HandleMqttEvent(", "void LocationMqttClient::UpdateGroupSubscription(")
        first = function(events, "if (event->current_data_offset == 0) {", "incoming_payload_.append")
        self.assertIn("incoming_retained_ = event->retain;", first)
        self.assertEqual(events.count("event->retain"), 1)
        self.assertIn("HandleMqttPayload(incoming_topic_, incoming_payload_, incoming_retained_)", events)
        completed = function(events, "HandleMqttPayload(incoming_topic_, incoming_payload_, incoming_retained_);", "default:")
        self.assertIn("incoming_retained_ = false;", completed)
        disconnected = function(events, "case MQTT_EVENT_DISCONNECTED:", "case MQTT_EVENT_ERROR:")
        self.assertIn("incoming_retained_ = false;", disconnected)
        stop = function(self.mqtt, "void LocationMqttClient::StopMqtt()", "void LocationMqttClient::MqttEventEntry(")
        self.assertIn("incoming_retained_ = false;", stop)

    def test_no_raw_rx_or_websocket_failure_payload_logging(self):
        self.assertIn("CommandPayloadForLog(root.get())", self.mqtt)
        self.assertIn("RedactSensitiveJsonFields(copy)", self.mqtt)
        redaction = source("main/boards/common/user_binding_json.h")
        self.assertIn('"bindingToken"', redaction)
        self.assertIn("cJSON_ReplaceItemViaPointer", redaction)
        self.assertNotRegex(self.mqtt.split("const bool is_position_topic", 1)[0], r"(?<![\w])payload\.c_str\(\)")
        self.assertNotIn("text.c_str()", self.ws)
        self.assertNotIn("message.c_str()", self.ws)
        self.assertNotIn("token.c_str());\n        ESP_LOG", self.ws)

    def test_wire_commands_and_ack_contract(self):
        self.assertIn('binding_command == "bindUser"', self.mqtt)
        self.assertIn('binding_command == "unbindUser"', self.mqtt)
        self.assertIn('command_topic_ = "cabinet/cmd/" + cab_guid_', self.mqtt)
        self.assertIn('AddString(root, "deviceExecute", success ? "OK" : "error")', self.mqtt)

    def test_audio_queues_invalidate_inflight_work(self):
        audio = source("main/audio/audio_service.cc")
        self.assertIn("task->generation == encode_generation_", audio)
        self.assertIn("generation == decode_generation_", audio)
        self.assertIn("audio_send_queue_.clear()", audio)
        self.assertEqual(self.app.count("if (!Board::GetInstance().RequiresUserBinding()) audio_service_.EncodeWakeWord();"), 2)


if __name__ == "__main__":
    unittest.main()
