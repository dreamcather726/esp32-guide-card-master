package xiaozhi.modules.device.service;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;

import xiaozhi.modules.device.entity.OtaEntity;

/** Checks the embedded application identity before publishing a Vanex OTA record. */
public final class FirmwareImageIdentity {
    private FirmwareImageIdentity() {}

    public static void validate(OtaEntity entity) {
        if (!"jiuchuan-s3".equals(entity.getType()) && !"pilot-jiuchuan-s3".equals(entity.getType())) {
            return;
        }
        if (entity.getFirmwarePath() == null || entity.getFirmwarePath().isBlank()) {
            throw new IllegalArgumentException("请先上传 OTA 应用 BIN 文件");
        }
        Path path = Path.of(entity.getFirmwarePath());
        if (!path.isAbsolute()) path = Path.of(System.getProperty("user.dir")).resolve(path);
        // Match the download endpoint's legacy firmware-directory fallback.
        if (!Files.isRegularFile(path)) {
            path = Path.of(System.getProperty("user.dir"), "firmware", path.getFileName().toString());
        }
        try {
            long size = Files.size(path);
            byte[] header;
            try (var input = Files.newInputStream(path)) {
                header = input.readNBytes(288);
            }
            if (size <= 288 || size > 0x3f0000 || header.length != 288 ||
                    (header[0] & 0xff) != 0xe9 ||
                    ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN).getShort(12) != 9 ||
                    ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN).getInt(32) != 0xabcd5432 ||
                    !"xiaozhi".equals(text(header, 80))) {
                throw new IllegalArgumentException("请选择 Vanex S3 的 OTA 应用 BIN，非整机烧录包或其他板型固件");
            }
            String embeddedVersion = text(header, 48);
            if (embeddedVersion.isEmpty() || !embeddedVersion.equals(entity.getVersion())) {
                throw new IllegalArgumentException("版本号与 BIN 内置版本不一致：BIN 为 " + embeddedVersion +
                        "。请填写该版本；发布新版需要重新编译并上传新的 BIN 文件");
            }
            entity.setSize(size);
        } catch (IOException e) {
            throw new IllegalArgumentException("OTA 文件不存在或读取失败，请重新上传 BIN 文件", e);
        }
    }

    private static String text(byte[] header, int start) {
        int end = start;
        while (end < start + 32 && header[end] != 0) end++;
        return new String(header, start, end - start, StandardCharsets.US_ASCII);
    }
}
