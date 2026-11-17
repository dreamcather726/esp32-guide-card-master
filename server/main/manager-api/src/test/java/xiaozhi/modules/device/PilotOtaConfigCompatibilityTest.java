package xiaozhi.modules.device;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.mockito.ArgumentMatchers.anyMap;
import static org.mockito.Mockito.when;

import java.util.HashMap;
import java.util.List;
import java.util.Map;

import org.junit.jupiter.api.extension.ExtendWith;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;
import org.mockito.InjectMocks;
import org.mockito.Mock;
import org.mockito.junit.jupiter.MockitoExtension;
import org.springframework.test.util.ReflectionTestUtils;

import xiaozhi.modules.config.service.impl.ConfigServiceImpl;
import xiaozhi.modules.device.service.PilotOtaPolicy;
import xiaozhi.modules.sys.dto.SysParamsDTO;
import xiaozhi.modules.sys.service.SysParamsService;

@ExtendWith(MockitoExtension.class)
class PilotOtaConfigCompatibilityTest {
    @Mock SysParamsService params;
    @InjectMocks ConfigServiceImpl service;

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void pilotDoesNotCollideWithExistingOtaAddress(boolean reversed) {
        SysParamsDTO ota = parameter("server.ota", "https://robot.suji.work/xiaozhi/ota/");
        SysParamsDTO pilot = parameter(PilotOtaPolicy.PARAM_CODE, "{\"enabled\":false}");
        when(params.list(anyMap())).thenReturn(reversed ? List.of(pilot, ota) : List.of(ota, pilot));
        Map<String, Object> config = new HashMap<>();

        ReflectionTestUtils.invokeMethod(service, "buildConfig", config);

        Map<?, ?> server = (Map<?, ?>) config.get("server");
        assertEquals(ota.getParamValue(), server.get("ota"));
        assertEquals(pilot.getParamValue(), server.get("ota_pilot"));
    }

    private SysParamsDTO parameter(String code, String value) {
        SysParamsDTO param = new SysParamsDTO();
        param.setParamCode(code);
        param.setParamValue(value);
        param.setValueType("string");
        return param;
    }
}
