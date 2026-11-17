package xiaozhi.modules.device;

import static org.junit.jupiter.api.Assertions.*;
import org.junit.jupiter.api.Test;
import org.springframework.test.util.ReflectionTestUtils;
import xiaozhi.modules.device.service.impl.DeviceServiceImpl;

class FirmwareVersionCompatibilityTest {
    private int compare(String a, String b) {
        return ReflectionTestUtils.invokeMethod(DeviceServiceImpl.class, "compareVersions", a, b);
    }

    @Test void diagnosticSuffixDoesNotBreakConfigurationOrDowngrade() {
        assertEquals(-1, compare("2.1.10", "2.1.11-diag"));
        assertEquals(0, compare("2.1.11", "2.1.11-diag"));
        assertEquals(1, compare("2.1.12", "2.1.11-diag"));
        assertEquals(0, compare("2.1.11-diag.2+build.123", "2.1.11+release"));
    }

    @Test void numericOrderingAndMissingComponentsArePreserved() {
        assertEquals(1, compare("2.1.10", "2.1.9"));
        assertEquals(0, compare("2.1", "2.1.0"));
        assertEquals(1, compare("2.1.0.1", "2.1.0"));
        assertEquals(1, compare("999999999.0.0", "2.1.11"));
    }

    @Test void invalidAndOverflowValuesSkipUpdateWithoutThrowing() {
        for (String invalid : new String[]{null, "", " ", "latest", "v2.1.11", "2..1", "2.1.",
                "2.1.-1", "2.1.11-", "2.1.11+", "2.1.11??", "999999999999.1.0"}) {
            assertEquals(0, compare("2.1.12", invalid));
            assertEquals(0, compare(invalid, "2.1.11-diag"));
        }
    }
}
