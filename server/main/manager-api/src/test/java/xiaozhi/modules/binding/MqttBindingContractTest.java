package xiaozhi.modules.binding;

import static org.junit.jupiter.api.Assertions.*;
import static org.mockito.Mockito.*;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.*;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.*;

import org.apache.shiro.authz.annotation.RequiresPermissions;
import org.junit.jupiter.api.Test;
import org.springframework.http.MediaType;
import org.springframework.test.web.servlet.setup.MockMvcBuilders;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PutMapping;

import com.fasterxml.jackson.databind.ObjectMapper;
import xiaozhi.modules.binding.controller.MqttBindingController;
import xiaozhi.modules.binding.controller.MqttBindingInternalController;
import xiaozhi.modules.binding.dto.MqttBindingDTOs.*;
import xiaozhi.modules.binding.service.MqttBindingService;
import xiaozhi.modules.security.config.ShiroConfig;
import xiaozhi.modules.security.config.WebMvcConfig;
import xiaozhi.modules.sys.service.SysParamsService;

class MqttBindingContractTest {
    @Test
    void exactRoutesInheritExistingOwnerAndServerAuth() throws Exception {
        assertArrayEquals(new String[]{"sys:role:normal"}, MqttBindingController.class
                .getAnnotation(RequiresPermissions.class).value());
        assertArrayEquals(new String[]{"/device/{macAddress}/mqtt-binding"}, MqttBindingController.class
                .getMethod("pair", String.class, Pair.class).getAnnotation(PutMapping.class).value());
        assertArrayEquals(new String[]{"/device/{macAddress}/mqtt-binding"}, MqttBindingController.class
                .getMethod("current", String.class).getAnnotation(GetMapping.class).value());
        var chains = new ShiroConfig().shirFilter(mock(org.apache.shiro.mgt.SecurityManager.class),
                mock(SysParamsService.class), mock(xiaozhi.modules.binding.service.ProfileApiKeyService.class))
                .getFilterChainDefinitionMap();
        assertEquals("server", chains.get("/config/**"));
        assertEquals("oauth2", chains.get("/**"));
        assertNull(chains.get("/device/**"));
    }

    @Test
    void mvcOnlineConsumesRawObjectAndLegacyIgnoresSuppliedOwnerAndProfile() throws Exception {
        MqttBindingService service = mock(MqttBindingService.class);
        var mvc = MockMvcBuilders.standaloneSetup(new MqttBindingInternalController(service))
                .setMessageConverters(new WebMvcConfig().jackson2HttpMessageConverter()).build();
        String online = "{\"cmd\":\"KJ\",\"cabGuid\":\"cab\",\"bootID\":\"boot\"}";
        when(service.online(new ObjectMapper().readTree(online))).thenReturn(
                new View("aa:bb:cc:dd:ee:ff", "cab", null, "2026-10-04T12:00:00Z", true));
        mvc.perform(post("/config/user-binding/online").contentType(MediaType.APPLICATION_JSON).content(online))
                .andExpect(status().isOk()).andExpect(jsonPath("$.code").value(0))
                .andExpect(jsonPath("$.data.receivedAt").value("2026-10-04T12:00:00Z"))
                .andExpect(jsonPath("$.data.report").value(org.hamcrest.Matchers.nullValue()));
        mvc.perform(post("/config/user-binding/legacy").contentType(MediaType.APPLICATION_JSON).content("""
                {"cabGuid":"cab","bootID":"boot","userGUID":"user","requestID":"request",
                 "ownerId":999,"profile":{"nickname":"ignored"}}
                """))
                .andExpect(status().isOk()).andExpect(jsonPath("$.code").value(0));
        verify(service).legacy(new Legacy("cab", "boot", "user", "request"));
    }
}
