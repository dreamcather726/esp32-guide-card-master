package xiaozhi.modules.device;

import static org.junit.jupiter.api.Assertions.*;
import static org.mockito.ArgumentMatchers.*;
import static org.mockito.Mockito.*;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;
import org.springframework.test.util.ReflectionTestUtils;
import xiaozhi.modules.device.dao.OtaDao;
import xiaozhi.modules.device.entity.OtaEntity;
import xiaozhi.modules.device.service.FirmwareImageIdentity;
import xiaozhi.modules.device.service.impl.OtaServiceImpl;

class FirmwareImageIdentityTest {
    @TempDir Path directory;

    private byte[] image() {
        byte[] data = new byte[1024];
        data[0] = (byte) 0xe9;
        ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN).putShort(12, (short) 9).putInt(32, 0xabcd5432);
        System.arraycopy("2.1.13-diag".getBytes(StandardCharsets.US_ASCII), 0, data, 48, 11);
        System.arraycopy("xiaozhi".getBytes(StandardCharsets.US_ASCII), 0, data, 80, 7);
        return data;
    }

    private OtaEntity record(byte[] data) throws Exception {
        Path path = directory.resolve("firmware.bin");
        Files.write(path, data);
        OtaEntity entity = new OtaEntity();
        entity.setId("fixture");
        entity.setType("jiuchuan-s3");
        entity.setVersion("2.1.13-diag");
        entity.setFirmwarePath(path.toString());
        entity.setSize(1L);
        return entity;
    }

    @Test void matchesEmbeddedVersionAndDerivesActualSize() throws Exception {
        var entity = record(image());
        FirmwareImageIdentity.validate(entity);
        assertEquals(1024L, entity.getSize());
        entity.setType("pilot-jiuchuan-s3");
        assertDoesNotThrow(() -> FirmwareImageIdentity.validate(entity));
    }

    @Test void relabelingOldImageAsNewReleaseIsRejected() throws Exception {
        var entity = record(image());
        for (String version : List.of("2.1.15", "2.1.13", "2.1.12-diag")) {
            entity.setVersion(version);
            var error = assertThrows(IllegalArgumentException.class, () -> FirmwareImageIdentity.validate(entity));
            assertTrue(error.getMessage().contains("2.1.13-diag"));
        }
    }

    @Test void rejectsWrongChipBootloaderFullImageAndOtherProject() throws Exception {
        for (int offset : new int[]{0, 12, 32, 80}) {
            byte[] data = image();
            data[offset] = 0;
            var entity = record(data);
            assertThrows(IllegalArgumentException.class, () -> FirmwareImageIdentity.validate(entity));
        }
    }

    @Test void rejectsTruncatedMissingAndOversizedImages() throws Exception {
        var truncated = record(new byte[100]);
        assertThrows(IllegalArgumentException.class, () -> FirmwareImageIdentity.validate(truncated));
        byte[] oversized = new byte[0x3f0001];
        System.arraycopy(image(), 0, oversized, 0, 1024);
        var entity = record(oversized);
        var oversizedEntity = entity;
        assertThrows(IllegalArgumentException.class, () -> FirmwareImageIdentity.validate(oversizedEntity));
        Files.delete(Path.of(entity.getFirmwarePath()));
        assertThrows(IllegalArgumentException.class, () -> FirmwareImageIdentity.validate(oversizedEntity));
        entity.setFirmwarePath(null);
        assertThrows(IllegalArgumentException.class, () -> FirmwareImageIdentity.validate(oversizedEntity));
    }

    @Test void preservesOtherBoardAndAssetPublication() {
        var entity = new OtaEntity();
        entity.setType("assets");
        assertDoesNotThrow(() -> FirmwareImageIdentity.validate(entity));
    }

    @Test void partialUpdateCannotBypassEmbeddedVersionCheck() throws Exception {
        var dao = mock(OtaDao.class);
        var service = new OtaServiceImpl();
        ReflectionTestUtils.setField(service, "baseDao", dao);
        when(dao.selectById("fixture")).thenReturn(record(image()));
        var edit = new OtaEntity();
        edit.setId("fixture"); edit.setVersion("2.1.15");
        assertThrows(IllegalArgumentException.class, () -> service.update(edit));
        verify(dao, never()).updateById(any(OtaEntity.class));
    }

    @Test void invalidCreateDoesNotReachDatabase() throws Exception {
        var dao = mock(OtaDao.class);
        var service = new OtaServiceImpl();
        ReflectionTestUtils.setField(service, "baseDao", dao);
        var entity = record(image()); entity.setVersion("2.1.15");
        assertThrows(IllegalArgumentException.class, () -> service.save(entity));
        verifyNoInteractions(dao);
    }

    @Test void validPartialMetadataEditRetainsIdentity() throws Exception {
        var dao = mock(OtaDao.class);
        var service = new OtaServiceImpl();
        ReflectionTestUtils.setField(service, "baseDao", dao);
        when(dao.selectById("fixture")).thenReturn(record(image()));
        when(dao.selectCount(any())).thenReturn(0L);
        var edit = new OtaEntity(); edit.setId("fixture"); edit.setRemark("new description");
        service.update(edit);
        assertEquals("2.1.13-diag", edit.getVersion());
        assertEquals(1024L, edit.getSize());
        verify(dao).updateById(edit);
    }
}
