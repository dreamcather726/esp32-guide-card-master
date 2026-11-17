package xiaozhi.modules.device;

import static org.junit.jupiter.api.Assertions.*;
import java.nio.charset.StandardCharsets;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;
import xiaozhi.modules.device.service.FirmwareDownloadResponse;

class FirmwareDownloadResponseTest {
    private static final byte[] DATA = "0123456789".getBytes(StandardCharsets.US_ASCII);

    @Test void fullAndResume() {
        var full = FirmwareDownloadResponse.create(DATA, "app.bin", null, null);
        assertEquals(200, full.getStatusCode().value());
        assertEquals(10, full.getHeaders().getContentLength());
        assertEquals("bytes", full.getHeaders().getFirst("Accept-Ranges"));
        String etag = full.getHeaders().getETag();
        assertEquals("\"" + full.getHeaders().getFirst("X-Firmware-SHA256") + "\"", etag);
        var part = FirmwareDownloadResponse.create(DATA, "app.bin", "bytes=4-", etag);
        assertEquals(206, part.getStatusCode().value());
        assertEquals("bytes 4-9/10", part.getHeaders().getFirst("Content-Range"));
        assertEquals(6, part.getHeaders().getContentLength());
        assertArrayEquals("456789".getBytes(StandardCharsets.US_ASCII), part.getBody());
    }

    @Test void changedImageRestarts() {
        var result = FirmwareDownloadResponse.create(DATA, "app.bin", "bytes=4-", "\"old\"");
        assertEquals(200, result.getStatusCode().value());
        assertArrayEquals(DATA, result.getBody());
    }

    @Test void suffixAndBoundedRange() {
        var suffix = FirmwareDownloadResponse.create(DATA, "app.bin", "bytes=-2", null);
        assertArrayEquals("89".getBytes(StandardCharsets.US_ASCII), suffix.getBody());
        var bounded = FirmwareDownloadResponse.create(DATA, "app.bin", "bytes=1-3", null);
        assertArrayEquals("123".getBytes(StandardCharsets.US_ASCII), bounded.getBody());
    }

    @ParameterizedTest
    @ValueSource(strings = {"bytes=10-", "bytes=99-", "bytes=4-1", "bytes=0-1,3-4", "nonsense", "bytes=-0"})
    void invalidRanges(String range) {
        var result = FirmwareDownloadResponse.create(DATA, "app.bin", range, null);
        assertEquals(416, result.getStatusCode().value());
        assertEquals("bytes */10", result.getHeaders().getFirst("Content-Range"));
    }
}
