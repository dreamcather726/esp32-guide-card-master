package xiaozhi.modules.device.service;

import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Arrays;
import java.util.HexFormat;
import java.util.List;

import org.springframework.http.HttpHeaders;
import org.springframework.http.HttpRange;
import org.springframework.http.HttpStatus;
import org.springframework.http.MediaType;
import org.springframework.http.ResponseEntity;

public final class FirmwareDownloadResponse {
    private FirmwareDownloadResponse() {}

    public static ResponseEntity<byte[]> create(byte[] content, String filename, String range, String ifRange) {
        final String digest;
        try {
            digest = HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(content));
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException("SHA-256 is required", e);
        }
        String etag = "\"" + digest + "\"";
        HttpHeaders headers = new HttpHeaders();
        headers.setContentType(MediaType.APPLICATION_OCTET_STREAM);
        headers.set(HttpHeaders.CONTENT_DISPOSITION, "attachment; filename=\"" + filename + "\"");
        headers.set(HttpHeaders.ACCEPT_RANGES, "bytes");
        headers.setETag(etag);
        headers.set("X-Firmware-SHA256", digest);
        headers.setCacheControl("private, no-transform");
        if (range != null && (ifRange == null || etag.equals(ifRange))) {
            try {
                List<HttpRange> ranges = HttpRange.parseRanges(range);
                if (ranges.size() != 1 || content.length == 0) throw new IllegalArgumentException("Single range required");
                HttpRange requested = ranges.getFirst();
                long start = requested.getRangeStart(content.length);
                long end = requested.getRangeEnd(content.length);
                if (start < 0 || start >= content.length || end < start) throw new IllegalArgumentException("Unsatisfied range");
                headers.set(HttpHeaders.CONTENT_RANGE, "bytes " + start + "-" + end + "/" + content.length);
                byte[] body = Arrays.copyOfRange(content, Math.toIntExact(start), Math.toIntExact(end + 1));
                headers.setContentLength(body.length);
                return new ResponseEntity<>(body, headers, HttpStatus.PARTIAL_CONTENT);
            } catch (IllegalArgumentException e) {
                headers.set(HttpHeaders.CONTENT_RANGE, "bytes */" + content.length);
                headers.setContentLength(0);
                return new ResponseEntity<>(new byte[0], headers, HttpStatus.REQUESTED_RANGE_NOT_SATISFIABLE);
            }
        }
        headers.setContentLength(content.length);
        return new ResponseEntity<>(content, headers, HttpStatus.OK);
    }
}
