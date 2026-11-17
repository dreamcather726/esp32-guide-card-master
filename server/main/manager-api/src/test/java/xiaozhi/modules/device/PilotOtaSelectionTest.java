package xiaozhi.modules.device;

import static org.junit.jupiter.api.Assertions.*;
import static org.mockito.ArgumentMatchers.*;
import static org.mockito.Mockito.*;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.springframework.test.util.ReflectionTestUtils;
import org.springframework.aop.framework.AopContext;
import xiaozhi.common.constant.Constant;
import xiaozhi.common.redis.RedisUtils;
import xiaozhi.modules.device.dao.DeviceDao;
import xiaozhi.modules.device.dto.DeviceReportRespDTO;
import xiaozhi.modules.device.dto.DeviceReportReqDTO;
import xiaozhi.modules.device.entity.DeviceEntity;
import xiaozhi.modules.device.entity.OtaEntity;
import xiaozhi.modules.device.service.OtaService;
import xiaozhi.modules.device.service.PilotOtaPolicy;
import xiaozhi.modules.device.service.impl.DeviceServiceImpl;
import xiaozhi.modules.sys.service.SysParamsService;
import xiaozhi.modules.sys.service.SysUserUtilService;

class PilotOtaSelectionTest {
    private static final String ID = "0123456789abcdef0123456789abcdef";
    private static final String MAC = "7c:4f:ad:24:81:fc";
    private OtaService ota;
    private RedisUtils redis;
    private SysParamsService params;
    private DeviceServiceImpl service;
    private OtaEntity staged;

    @BeforeEach void setup() {
        ota = mock(OtaService.class);
        redis = mock(RedisUtils.class);
        params = mock(SysParamsService.class);
        service = new DeviceServiceImpl(mock(DeviceDao.class), mock(SysUserUtilService.class), params, redis, ota);
        when(params.getValue(PilotOtaPolicy.PARAM_CODE, false)).thenReturn(
                "{\"enabled\":true,\"boardType\":\"jiuchuan-s3\",\"otaId\":\"" + ID + "\",\"macAddresses\":[\"" + MAC + "\"]}");
        when(params.getValue(Constant.SERVER_OTA, true)).thenReturn("https://robot.suji.work/xiaozhi/ota/");
        staged = new OtaEntity();
        staged.setId(ID); staged.setType("pilot-jiuchuan-s3"); staged.setVersion("2.1.9");
        staged.setFirmwarePath("/uploadfile/vanex-s3-2.1.9.bin");
        when(ota.selectById(ID)).thenReturn(staged);
        OtaEntity normal = new OtaEntity();
        normal.setId("normal"); normal.setType("jiuchuan-s3"); normal.setVersion("2.1.3");
        normal.setFirmwarePath("/uploadfile/vanex-s3-2.1.3.bin");
        when(ota.getLatestOta("jiuchuan-s3")).thenReturn(normal);
    }

    private DeviceReportRespDTO.Firmware select(String type, String version, String mac) {
        return ReflectionTestUtils.invokeMethod(service, "buildFirmwareInfo", type, version, mac);
    }

    @Test void oldTargetReceives219WithoutChangingGlobalSelection() {
        var result = select("jiuchuan-s3", "2.1.0", MAC);
        assertEquals("2.1.9", result.getVersion());
        assertTrue(result.getUrl().startsWith("https://robot.suji.work/xiaozhi/otaMag/download/"));
        verify(redis).set(anyString(), eq(ID));
        verify(ota, never()).getLatestOta(anyString());
    }

    @Test void otherDeviceStillReceivesNormalRelease() {
        assertEquals("2.1.3", select("jiuchuan-s3", "2.1.0", "7c:4f:ad:24:81:fd").getVersion());
        verify(ota, never()).selectById(anyString());
        verify(redis).set(anyString(), eq("normal"));
    }

    @Test void equalOrNewerVersionDoesNotDownloadOrDowngrade() {
        assertEquals(Constant.INVALID_FIRMWARE_URL, select("jiuchuan-s3", "2.1.9", MAC).getUrl());
        assertEquals(Constant.INVALID_FIRMWARE_URL, select("jiuchuan-s3", "2.1.10", MAC).getUrl());
        assertEquals(Constant.INVALID_FIRMWARE_URL, select("jiuchuan-s3", "2.1.11-diag", MAC).getUrl());
        assertEquals(Constant.INVALID_FIRMWARE_URL, select("jiuchuan-s3", "invalid", MAC).getUrl());
        verify(redis, never()).set(anyString(), any());
    }

    @Test void reservedTypeCannotBypassDeviceAllowlist() {
        assertNull(select("pilot-jiuchuan-s3", "2.1.0", "7c:4f:ad:24:81:fd"));
        verifyNoInteractions(ota, redis);
    }

    @Test void missingPilotKeepsNormalRelease() {
        when(ota.selectById(ID)).thenReturn(null);
        assertEquals("2.1.3", select("jiuchuan-s3", "2.1.0", MAC).getVersion());
    }

    @Test void incompatibleTypeOrInvalidVersionCannotBeServed() {
        staged.setType("pilot-other-board");
        assertEquals("2.1.3", select("jiuchuan-s3", "2.1.0", MAC).getVersion());
        staged.setType("pilot-jiuchuan-s3"); staged.setVersion("unexpected");
        assertEquals("2.1.3", select("jiuchuan-s3", "2.1.0", MAC).getVersion());
    }

    @Test void pilotDisabledRestoresUnchangedPublicRelease() {
        when(params.getValue(PilotOtaPolicy.PARAM_CODE, false)).thenReturn("");
        assertEquals("2.1.3", select("jiuchuan-s3", "2.1.0", MAC).getVersion());
        verify(ota, never()).selectById(anyString());
    }

    @Test void diagnosticAndUnknownVersionStillReceiveVoiceConfigAndTime() {
        service = spy(service);
        var device = new DeviceEntity();
        device.setId(MAC); device.setAgentId("fixture-agent"); device.setAutoUpdate(1);
        doReturn(device).when(service).getDeviceByMacAddress(MAC);
        when(params.getValue(Constant.SERVER_WEBSOCKET, true)).thenReturn("wss://example.test/xiaozhi/v1/");
        when(params.getValue(Constant.SERVER_AUTH_ENABLED, true)).thenReturn("false");
        var proxy = mock(DeviceServiceImpl.class);
        try (var context = mockStatic(AopContext.class)) {
            context.when(AopContext::currentProxy).thenReturn(proxy);
            for (String version : new String[]{"2.1.11-diag", "bad-version", "999999999999.1.1"}) {
                var request = new DeviceReportReqDTO();
                var application = new DeviceReportReqDTO.Application();
                application.setVersion(version); request.setApplication(application);
                var board = new DeviceReportReqDTO.BoardInfo();
                board.setType("jiuchuan-s3"); request.setBoard(board);
                var response = service.checkDeviceActive(MAC, "fixture-client", request);
                assertEquals("wss://example.test/xiaozhi/v1/", response.getWebsocket().getUrl());
                assertNotNull(response.getServer_time());
                assertEquals(Constant.INVALID_FIRMWARE_URL, response.getFirmware().getUrl());
            }
        }
    }
}
