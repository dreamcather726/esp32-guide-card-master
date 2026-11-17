"""Integration guards supplement tests of the production worker with simulated I/O."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


def body(text, begin, end):
    return text[text.index(begin):text.index(end)]


class RndisIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.board = (ROOT / "main/boards/common/rndis_board.cc").read_text(encoding="utf-8")
        self.driver = (ROOT / "components/iot_usbh_cdc/iot_usbh_cdc.c").read_text(encoding="utf-8")

    def test_metadata_precedes_network_ready_shortcut(self):
        maintain = body(self.board, "bool RndisBoard::DetectAndDialUsbAt()", "void RndisBoard::NetworkTaskEntry(")
        self.assertLess(maintain.index("QueryAndCacheImei(session)"), maintain.index("if (IsNetworkReady())"))
        self.assertLess(maintain.index("QueryAndCacheImei(session)"), maintain.index("if (dial_already_accepted)"))

    def test_no_blocking_identity_read_in_start_or_ip_callback(self):
        start = body(self.board, "void RndisBoard::StartNetwork()", "bool RndisBoard::InitializeUsbCdcHost()")
        event = body(self.board, "void RndisBoard::iot_event_handle(", "void RndisBoard::OnNetworkEvent(")
        self.assertNotIn("QueryAndCacheImei", start + event)
        self.assertIn("EVENT_GOT_IP_BIT | EVENT_USB_MAINTENANCE_BIT", event)

    def test_enumeration_and_disconnect_invalidate_identity(self):
        connected = body(self.board, "void RndisBoard::HandleUsbDeviceConnected(", "void RndisBoard::HandleUsbDeviceDisconnected(")
        gone = body(self.board, "void RndisBoard::HandleUsbDeviceDisconnected(", "void RndisBoard::UsbAtReceiveCallback(")
        for code in (connected, gone):
            self.assertIn("++usb_session_", code)
            self.assertIn("imei_.clear()", code)
            self.assertIn("usb_at_port_ = nullptr", code)

    def test_receive_and_send_are_session_scoped(self):
        rx = body(self.board, "void RndisBoard::UsbAtReceiveCallback(", "void RndisBoard::UsbAtClosedCallback(")
        tx = body(self.board, "bool RndisBoard::SendUsbAtCommand(", "bool RndisBoard::ExecuteUsbAtCommand(")
        self.assertLess(rx.index("usb_at_port_ != port_handle"), rx.index("usbh_cdc_read_bytes"))
        self.assertLess(tx.index("std::unique_lock<std::mutex> state_lock"), tx.index("usbh_cdc_write_bytes"))
        self.assertLess(tx.index("usbh_cdc_write_bytes"), tx.index("state_lock.unlock()"))
        self.assertIn("usb_session_ != session", tx)
        self.assertIn("ml307_at::ParseReply(response)", tx)

    def test_probe_uses_descriptor_filter_and_reuses_port(self):
        ensure = body(self.board, "bool RndisBoard::EnsureUsbAtControlPort(", "bool RndisBoard::DetectAndDialUsbAt()")
        self.assertIn("ml307_at::ControlCandidates", ensure)
        self.assertLess(ensure.index("if (usb_at_port_)"), ensure.index("OpenAndProbeUsbAtInterface"))

    def test_close_is_validated_under_lifecycle_lock(self):
        close = body(self.driver, "esp_err_t usbh_cdc_port_close(", "esp_err_t usbh_cdc_send_custom_request(")
        self.assertLess(close.index("xSemaphoreTake"), close.index("cdc_port_close_locked(cdc_port_handle)"))
        self.assertLess(close.index("is_port_opened(cdc_port)"), close.index("cdc_port->to_close"))
        gone = body(self.driver, "case USB_HOST_CLIENT_EVENT_DEV_GONE:", "esp_err_t usbh_cdc_driver_install(")
        self.assertLess(gone.index("xSemaphoreTake"), gone.index("SLIST_FOREACH_SAFE"))
        self.assertIn("cdc_port_close_locked", gone)


if __name__ == "__main__":
    unittest.main()
