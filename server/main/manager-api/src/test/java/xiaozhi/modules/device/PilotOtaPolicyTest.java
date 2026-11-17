package xiaozhi.modules.device;

import static org.junit.jupiter.api.Assertions.*;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;
import xiaozhi.modules.device.service.PilotOtaPolicy;

class PilotOtaPolicyTest {
    private static final String ID = "0123456789abcdef0123456789abcdef";
    private static final String MAC = "7c:4f:ad:24:81:fc";
    private static final String CONFIG = "{\"enabled\":true,\"boardType\":\"jiuchuan-s3\",\"otaId\":\"" + ID +
            "\",\"macAddresses\":[\"" + MAC + "\"]}";

    @Test void selectsOnlyTheNamedDeviceAndBoard() {
        assertEquals(ID, PilotOtaPolicy.firmwareId(CONFIG, "jiuchuan-s3", MAC));
        assertEquals(ID, PilotOtaPolicy.firmwareId(CONFIG, "jiuchuan-s3", MAC.toUpperCase()));
        assertNull(PilotOtaPolicy.firmwareId(CONFIG, "jiuchuan-s3", "7c:4f:ad:24:81:fd"));
        assertNull(PilotOtaPolicy.firmwareId(CONFIG, "other-board", MAC));
    }

    @Test void stagedTypesAreReserved() {
        assertEquals("pilot-jiuchuan-s3", PilotOtaPolicy.pilotType("jiuchuan-s3"));
        assertTrue(PilotOtaPolicy.isPilotType("pilot-jiuchuan-s3"));
        assertFalse(PilotOtaPolicy.isPilotType("jiuchuan-s3"));
        assertNull(PilotOtaPolicy.firmwareId(CONFIG, "pilot-jiuchuan-s3", MAC));
    }

    @ParameterizedTest
    @ValueSource(strings = {"", "null", "{}", "[]", "broken", "{\"enabled\":true}"})
    void absentOrMalformedConfigDoesNotSelect(String config) {
        assertNull(PilotOtaPolicy.firmwareId(config, "jiuchuan-s3", MAC));
    }

    @Test void disabledAndWrongFieldTypesDoNotSelect() {
        assertNull(PilotOtaPolicy.firmwareId(null, "jiuchuan-s3", MAC));
        assertNull(PilotOtaPolicy.firmwareId(CONFIG.replace("true", "false"), "jiuchuan-s3", MAC));
        assertNull(PilotOtaPolicy.firmwareId(CONFIG.replace("true", "\"true\""), "jiuchuan-s3", MAC));
        assertNull(PilotOtaPolicy.firmwareId(CONFIG.replace(ID, "../image"), "jiuchuan-s3", MAC));
        assertNull(PilotOtaPolicy.firmwareId(CONFIG.replace("[\"" + MAC + "\"]", "\"" + MAC + "\""), "jiuchuan-s3", MAC));
        assertNull(PilotOtaPolicy.firmwareId(CONFIG.replace("[\"" + MAC + "\"]", "[]"), "jiuchuan-s3", MAC));
    }

    @Test void invalidTargetsRejectTheEntireConfiguration() {
        assertNull(PilotOtaPolicy.firmwareId(CONFIG.replace("[\"" + MAC + "\"]", "[\"" + MAC + "\",\"*\"]"), "jiuchuan-s3", MAC));
        assertNull(PilotOtaPolicy.firmwareId(CONFIG.replace("[\"" + MAC + "\"]", "[\"" + MAC + "\",7]"), "jiuchuan-s3", MAC));
        assertNull(PilotOtaPolicy.firmwareId(CONFIG, "jiuchuan-s3", "*"));
        assertNull(PilotOtaPolicy.firmwareId(CONFIG, "jiuchuan-s3", null));
        assertNull(PilotOtaPolicy.firmwareId(" ".repeat(8193), "jiuchuan-s3", MAC));
    }
}
