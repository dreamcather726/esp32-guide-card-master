package xiaozhi.modules.integration;

import static org.junit.jupiter.api.Assertions.*;
import static org.mockito.Mockito.*;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.*;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.*;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.*;
import org.apache.shiro.util.ThreadContext;
import org.junit.jupiter.api.*;
import org.mockito.MockedStatic;
import org.springframework.test.util.ReflectionTestUtils;
import org.springframework.test.web.servlet.MockMvc;
import org.springframework.test.web.servlet.setup.MockMvcBuilders;
import jakarta.servlet.Filter;
import xiaozhi.common.utils.MessageUtils;
import xiaozhi.modules.binding.dao.ProfileApiKeyDao;
import xiaozhi.modules.binding.entity.ProfileApiKeyEntity;
import xiaozhi.modules.binding.service.ProfileApiKeyService;
import xiaozhi.modules.integration.controller.DeviceIntegrationController;
import xiaozhi.modules.integration.dto.DeviceIntegrationDTOs.*;
import xiaozhi.modules.integration.service.*;
import xiaozhi.modules.security.config.*;
import xiaozhi.modules.security.entity.SysUserTokenEntity;
import xiaozhi.modules.security.oauth2.Oauth2Realm;
import xiaozhi.modules.security.service.ShiroService;
import xiaozhi.modules.sys.entity.SysUserEntity;
import xiaozhi.modules.sys.service.SysParamsService;

class DeviceIntegrationApiTest {
    static final String ID="1".repeat(32), KEY="vxp_"+ID+"_"+"a".repeat(64);
    static final String ROOT="/xiaozhi/integration";
    MockMvc mvc;
    ProfileApiKeyEntity key;
    DeviceRoleService roles;
    DeviceConversationService history;
    DeviceRuntimeService runtime;
    MockedStatic<MessageUtils> messages;

    @BeforeEach void setup() throws Exception {
        var keys=mock(ProfileApiKeyDao.class); key=new ProfileApiKeyEntity();
        key.setOwnerId(10L); key.setScopes("roles:read,roles:write,device:role,history:read,history:delete");
        key.setSecretHash(HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256")
                .digest(KEY.getBytes(StandardCharsets.US_ASCII))));
        when(keys.findActive(ID)).thenReturn(key);
        var shiro=mock(ShiroService.class); var user=new SysUserEntity();
        user.setId(20L); user.setStatus(1); user.setSuperAdmin(0);
        var token=new SysUserTokenEntity(); token.setUserId(20L); token.setExpireDate(new Date(System.currentTimeMillis()+60000));
        when(shiro.getByToken("login-fixture")).thenReturn(token); when(shiro.getUser(20L)).thenReturn(user);
        var realm=new Oauth2Realm(); ReflectionTestUtils.setField(realm,"shiroService",shiro);
        var config=new ShiroConfig(); var manager=config.securityManager(realm,config.sessionManager());
        var factory=config.shirFilter(manager,mock(SysParamsService.class),new ProfileApiKeyService(keys));
        roles=mock(DeviceRoleService.class); history=mock(DeviceConversationService.class); runtime=mock(DeviceRuntimeService.class);
        mvc=MockMvcBuilders.standaloneSetup(new DeviceIntegrationController(roles,history,runtime))
                .setMessageConverters(new WebMvcConfig().jackson2HttpMessageConverter())
                .addFilters((Filter)factory.getObject()).build();
        messages=mockStatic(MessageUtils.class); messages.when(()->MessageUtils.getMessage(anyInt())).thenReturn("Authentication required");
    }

    @AfterEach void cleanup() { messages.close(); ThreadContext.remove(); }

    @Test void roleReadContainsAllPublicFieldsAndUsesKeyOwnerOnly() throws Exception {
        when(roles.roles(10L,"agent")).thenReturn(List.of(new Role("agent","role","Guide","Definition",
                "voice","Voice name","code","https://example.com/a.png","https://example.com/v.mp4",
                "https://example.com/a.mp3",0,true)));
        var result=mvc.perform(get(ROOT+"/agents/agent/roles").contextPath("/xiaozhi").header("X-API-Key",KEY)
                        .param("ownerId","999"))
                .andExpect(jsonPath("$.code").value(0)).andExpect(jsonPath("$.data[0].roleId").value("role"))
                .andExpect(jsonPath("$.data[0].voiceName").value("Voice name"))
                .andExpect(jsonPath("$.data[0].audioUrl").value("https://example.com/a.mp3"))
                .andExpect(header().string("Cache-Control","no-store")).andReturn();
        assertNull(result.getRequest().getSession(false)); verify(roles).roles(10L,"agent");
    }

    @Test void profileOnlyAndReadOnlyKeysDoNotGrantDestructiveOperations() throws Exception {
        key.setScopes("profile");
        mvc.perform(get(ROOT+"/agents/agent/roles").contextPath("/xiaozhi").header("X-API-Key",KEY))
                .andExpect(status().isUnauthorized());
        key.setScopes("history:read");
        mvc.perform(delete(ROOT+"/devices/device/chat-history").contextPath("/xiaozhi")
                        .param("confirm","true").header("X-API-Key",KEY)).andExpect(status().isUnauthorized());
        verifyNoInteractions(history,runtime,roles);
    }

    @Test void clearRequiresExplicitConfirmationAndOnlyTouchesTheRequestedDevice() throws Exception {
        mvc.perform(delete(ROOT+"/devices/device/chat-history").contextPath("/xiaozhi").header("X-API-Key",KEY))
                .andExpect(jsonPath("$.code").value(400));
        verifyNoInteractions(history,runtime);
        var cleared=new Cleared("device","aa:bb:cc:dd:ee:ff",2,new Date(),"pending");
        when(history.clear(10L,"device")).thenReturn(cleared); when(runtime.apply(any())).thenReturn("saved_device_offline");
        mvc.perform(delete(ROOT+"/devices/device/chat-history").contextPath("/xiaozhi").header("X-API-Key",KEY)
                        .param("confirm","true")).andExpect(jsonPath("$.code").value(0))
                .andExpect(jsonPath("$.data.deletedCount").value("2"))
                .andExpect(jsonPath("$.data.runtimeStatus").value("saved_device_offline"));
        verify(runtime).apply(Map.of("action","clear_history","deviceId","aa:bb:cc:dd:ee:ff"));
    }

    @Test void bindingPersistsBeforeRuntimeDispatchAndValidatesBody() throws Exception {
        mvc.perform(put(ROOT+"/devices/device/role-binding").contextPath("/xiaozhi").header("X-API-Key",KEY)
                        .contentType("application/json").content("{}")) .andExpect(status().isBadRequest());
        var binding=new RoleBinding("device","aa:bb:cc:dd:ee:ff","agent","role",1L,new Date());
        when(roles.bind(10L,"device",new SelectRole("agent","role"))).thenReturn(binding);
        when(runtime.apply(any())).thenReturn("applied");
        mvc.perform(put(ROOT+"/devices/device/role-binding").contextPath("/xiaozhi").header("X-API-Key",KEY)
                        .contentType("application/json").content("{\"agentId\":\"agent\",\"roleId\":\"role\"}"))
                .andExpect(jsonPath("$.code").value(0)).andExpect(jsonPath("$.data.runtimeStatus").value("applied"));
        var order=inOrder(roles,runtime); order.verify(roles).bind(10L,"device",new SelectRole("agent","role"));
        order.verify(runtime).apply(any());
    }

    @Test void rejectsMixedCredentialsExtraPathsAndMethods() throws Exception {
        mvc.perform(get(ROOT+"/agents/agent/roles").contextPath("/xiaozhi").header("X-API-Key",KEY)
                        .header("Authorization","Bearer login-fixture")).andExpect(status().isBadRequest());
        for(String suffix:List.of("/extra",";suffix","/")) {
            mvc.perform(get(ROOT+"/agents/agent/roles"+suffix).contextPath("/xiaozhi").header("X-API-Key",KEY))
                    .andExpect(result->assertTrue(result.getResponse().getStatus()>=400));
        }
        mvc.perform(post(ROOT+"/devices/device/chat-history").contextPath("/xiaozhi").header("X-API-Key",KEY))
                .andExpect(status().isForbidden());
        verifyNoInteractions(history,runtime,roles);
    }

    @Test void bearerStillUsesLoginOwnerAndAnonymousRequestsAreRejected() throws Exception {
        mvc.perform(get(ROOT+"/agents/agent/roles").contextPath("/xiaozhi"))
                .andExpect(jsonPath("$.code").value(401));
        mvc.perform(get(ROOT+"/agents/agent/roles").contextPath("/xiaozhi")
                        .header("Authorization","Bearer login-fixture")).andExpect(jsonPath("$.code").value(0));
        verify(roles).roles(20L,"agent");
    }
}
