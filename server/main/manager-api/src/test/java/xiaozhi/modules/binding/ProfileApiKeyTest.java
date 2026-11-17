package xiaozhi.modules.binding;

import static org.junit.jupiter.api.Assertions.*;
import static org.mockito.Mockito.*;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.*;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.*;

import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Date;
import java.util.HexFormat;
import java.util.List;

import org.apache.shiro.util.ThreadContext;
import org.apache.shiro.mgt.SecurityManager;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;
import org.mockito.MockedStatic;
import org.springframework.test.util.ReflectionTestUtils;
import org.springframework.test.web.servlet.MockMvc;
import org.springframework.test.web.servlet.setup.MockMvcBuilders;

import jakarta.servlet.Filter;
import xiaozhi.common.utils.MessageUtils;
import xiaozhi.modules.binding.controller.UserBindingController;
import xiaozhi.modules.binding.controller.UserProfileController;
import xiaozhi.modules.binding.dao.ProfileApiKeyDao;
import xiaozhi.modules.binding.dto.UserBindingDTOs.Profile;
import xiaozhi.modules.binding.dto.UserBindingDTOs.ProfileView;
import xiaozhi.modules.binding.entity.ProfileApiKeyEntity;
import xiaozhi.modules.binding.service.ProfileApiKeyService;
import xiaozhi.modules.binding.service.UserBindingService;
import xiaozhi.modules.security.config.ShiroConfig;
import xiaozhi.modules.security.config.WebMvcConfig;
import xiaozhi.modules.security.entity.SysUserTokenEntity;
import xiaozhi.modules.security.oauth2.Oauth2Realm;
import xiaozhi.modules.security.service.ShiroService;
import xiaozhi.modules.sys.entity.SysUserEntity;
import xiaozhi.modules.sys.service.SysParamsService;

class ProfileApiKeyTest {
    private static final String KEY_ID = "1".repeat(32);
    private static final String KEY = "vxp_" + KEY_ID + "_" + "a".repeat(64);
    private static final String URL = "/xiaozhi/user/profile/customer-user";
    private ProfileApiKeyDao keys;
    private ProfileApiKeyService auth;
    private UserBindingService profiles;
    private ShiroService shiro;
    private SecurityManager manager;
    private MockedStatic<MessageUtils> messages;
    private MockMvc mvc;

    @BeforeEach
    void setup() throws Exception {
        keys = mock(ProfileApiKeyDao.class);
        auth = new ProfileApiKeyService(keys);
        var record = new ProfileApiKeyEntity();
        record.setOwnerId(10L);
        record.setSecretHash(HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256")
                .digest(KEY.getBytes(StandardCharsets.US_ASCII))));
        when(keys.findActive(KEY_ID)).thenReturn(record);
        profiles = mock(UserBindingService.class);
        when(profiles.getProfile(10L, "customer-user")).thenReturn(new ProfileView("customer-user", 1L,
                new Profile("Alice", "zh-CN", "history", "brief")));
        shiro = mock(ShiroService.class);
        var user = new SysUserEntity();
        user.setId(20L);
        user.setStatus(1);
        user.setSuperAdmin(0);
        var token = new SysUserTokenEntity();
        token.setUserId(20L);
        token.setExpireDate(new Date(System.currentTimeMillis() + 60000));
        when(shiro.getByToken("fixture-login-token")).thenReturn(token);
        when(shiro.getUser(20L)).thenReturn(user);
        var realm = new Oauth2Realm();
        ReflectionTestUtils.setField(realm, "shiroService", shiro);
        var config = new ShiroConfig();
        manager = config.securityManager(realm, config.sessionManager());
        var factory = config.shirFilter(manager, mock(SysParamsService.class), auth);
        mvc = MockMvcBuilders.standaloneSetup(new UserProfileController(profiles), new UserBindingController(profiles))
                .setMessageConverters(new WebMvcConfig().jackson2HttpMessageConverter())
                .addFilters((Filter) factory.getObject()).build();
        messages = mockStatic(MessageUtils.class);
        messages.when(() -> MessageUtils.getMessage(anyInt())).thenReturn("Authentication required");
    }

    @AfterEach
    void cleanup() {
        if (messages != null) messages.close();
        ThreadContext.remove();
    }

    @Test
    void keyReadWriteUsesOnlyStoredOwnerAndDoesNotAuthenticateASession() throws Exception {
        var response = mvc.perform(get(URL).contextPath("/xiaozhi").header("X-API-Key", KEY)
                        .param("ownerId", "999"))
                .andExpect(status().isOk()).andExpect(jsonPath("$.data.profile.nickname").value("Alice"))
                .andExpect(header().string("Cache-Control", "no-store")).andReturn();
        assertNull(response.getRequest().getSession(false));
        mvc.perform(put(URL).contextPath("/xiaozhi").header("X-API-Key", KEY)
                        .contentType("application/json").content("""
                        {"ownerId":999,"nickname":"Updated","language":"zh-CN","interests":"art","explanationStyle":"brief"}
                        """))
                .andExpect(status().isOk()).andExpect(jsonPath("$.code").value(0));
        verify(profiles).upsertProfile(10L, "customer-user", new Profile("Updated", "zh-CN", "art", "brief"));
        mvc.perform(get(URL).contextPath("/xiaozhi")).andExpect(jsonPath("$.code").value(401));
        verifyNoInteractions(shiro);
    }

    @Test
    void bearerAuthenticationStillUsesLoginOwner() throws Exception {
        mvc.perform(get(URL).contextPath("/xiaozhi").header("Authorization", "Bearer fixture-login-token"))
                .andExpect(status().isOk()).andExpect(jsonPath("$.code").value(0));
        verify(profiles).getProfile(20L, "customer-user");
        verifyNoInteractions(keys);
    }

    @Test
    void prefixSearchWorksForKeyAndBearerWithoutChangingExactReads() throws Exception {
        var rows = List.of(new ProfileView("customer-user", 1L,
                new Profile("Alice", "zh-CN", "history", "brief", "self", 3)),
                new ProfileView("customer-user-1", 2L, new Profile("Bob", null, null, null)));
        when(profiles.searchProfiles(anyLong(), eq("customer-user"))).thenReturn(rows);
        for (String header : List.of("X-API-Key", "Authorization")) {
            mvc.perform(get(URL).contextPath("/xiaozhi").param("match", "prefix")
                            .param("ownerId", "999").param("limit", "1000")
                            .header(header, header.equals("X-API-Key") ? KEY : "Bearer fixture-login-token"))
                    .andExpect(status().isOk()).andExpect(jsonPath("$.code").value(0))
                    .andExpect(jsonPath("$.data").isArray()).andExpect(jsonPath("$.data.length()").value(2))
                    .andExpect(jsonPath("$.data[0].userGUID").value("customer-user"))
                    .andExpect(jsonPath("$.data[0].profile.usageCount").value(3))
                    .andExpect(jsonPath("$.data[1].userGUID").value("customer-user-1"))
                    .andExpect(header().string("Cache-Control", "no-store"));
        }
        verify(profiles).searchProfiles(10L, "customer-user");
        verify(profiles).searchProfiles(20L, "customer-user");
        mvc.perform(get(URL).contextPath("/xiaozhi").param("match", "exact").header("X-API-Key", KEY))
                .andExpect(jsonPath("$.data.profile.nickname").value("Alice"));
        verify(profiles).getProfile(10L, "customer-user");
        verifyNoMoreInteractions(profiles);
    }

    @Test
    void prefixMissIsAnEmptyArray() throws Exception {
        when(profiles.searchProfiles(10L, "customer-user")).thenReturn(List.of());
        mvc.perform(get(URL).contextPath("/xiaozhi").param("match", "prefix").header("X-API-Key", KEY))
                .andExpect(jsonPath("$.code").value(0)).andExpect(jsonPath("$.data").isArray())
                .andExpect(jsonPath("$.data.length()").value(0));
    }

    @Test
    void prefixSearchStillRequiresAuthentication() throws Exception {
        mvc.perform(get(URL).contextPath("/xiaozhi").param("match", "prefix"))
                .andExpect(jsonPath("$.code").value(401));
        mvc.perform(get(URL).contextPath("/xiaozhi").param("match", "prefix").header("X-API-Key", "wrong"))
                .andExpect(status().isUnauthorized());
        verifyNoInteractions(profiles);
    }

    @Test
    void invalidModesAndPrefixWritesNeverCallService() throws Exception {
        for (String mode : List.of("contains", "PREFIX", "prefix,exact")) {
            mvc.perform(get(URL).contextPath("/xiaozhi").param("match", mode).header("X-API-Key", KEY))
                    .andExpect(jsonPath("$.code").value(400));
        }
        mvc.perform(put(URL).contextPath("/xiaozhi").param("match", "prefix").header("X-API-Key", KEY)
                        .contentType("application/json").content("{\"nickname\":\"Do not save\"}"))
                .andExpect(jsonPath("$.code").value(400));
        verifyNoInteractions(profiles);
    }

    @Test
    void rejectWrongInactiveAndMalformedKeysWithoutFallback() throws Exception {
        for (String key : List.of("", "wrong", KEY + "," + KEY, KEY.substring(0, KEY.length()-1) + "b")) {
            mvc.perform(get(URL).contextPath("/xiaozhi").header("X-API-Key", key))
                    .andExpect(status().isUnauthorized()).andExpect(jsonPath("$.code").value(401));
        }
        when(keys.findActive(KEY_ID)).thenReturn(null);
        mvc.perform(get(URL).contextPath("/xiaozhi").header("X-API-Key", KEY))
                .andExpect(status().isUnauthorized());
        mvc.perform(get(URL).contextPath("/xiaozhi").header("X-API-Key", "invalid")
                        .header("Authorization", "Bearer fixture-login-token"))
                .andExpect(status().isBadRequest());
        mvc.perform(get(URL).contextPath("/xiaozhi").header("X-API-Key", KEY, KEY))
                .andExpect(status().isBadRequest());
        verifyNoInteractions(profiles, shiro);
    }

    @Test
    void databaseFailureFailsClosedAndDoesNotExposeCredentials() throws Exception {
        when(keys.findActive(KEY_ID)).thenThrow(new IllegalStateException("fixture DB failure"));
        String body = mvc.perform(get(URL).contextPath("/xiaozhi").header("X-API-Key", KEY))
                .andExpect(status().isServiceUnavailable()).andReturn().getResponse().getContentAsString();
        assertFalse(body.contains(KEY));
        assertFalse(body.contains("fixture DB failure"));
        verifyNoInteractions(profiles);
    }

    @ParameterizedTest
    @ValueSource(strings = {"/sessions", "/sessions/id/messages", "/", ";suffix", "%2Fsessions"})
    void keyCannotAccessNestedOrNonCanonicalPaths(String suffix) throws Exception {
        mvc.perform(get(java.net.URI.create(URL + suffix)).contextPath("/xiaozhi").header("X-API-Key", KEY))
                .andExpect(result -> assertTrue(result.getResponse().getStatus() >= 400));
        verifyNoInteractions(profiles, keys);
    }

    @ParameterizedTest
    @ValueSource(strings = {"POST", "DELETE", "PATCH", "HEAD"})
    void keyCannotUseOtherMethods(String method) throws Exception {
        mvc.perform(request(org.springframework.http.HttpMethod.valueOf(method), URL).contextPath("/xiaozhi")
                        .header("X-API-Key", KEY).contentType("application/json").content("{}"))
                .andExpect(status().isForbidden());
        verifyNoInteractions(profiles, keys);
    }

    @ParameterizedTest
    @ValueSource(strings = {"/device/aa/user-binding", "/device/aa/mqtt-binding", "/sys/user/page",
            "/config/user-binding/resolve", "/agent/chat-history/report"})
    void keyCannotGrantDeviceAdminOrInternalAccess(String path) throws Exception {
        mvc.perform(get("/xiaozhi" + path).contextPath("/xiaozhi").header("X-API-Key", KEY))
                .andExpect(jsonPath("$.code").value(401));
        verifyNoInteractions(profiles, keys);
    }

    @Test
    void queryStringIsNotACredentialAndProfileValidationRemains() throws Exception {
        mvc.perform(get(URL).contextPath("/xiaozhi").param("X-API-Key", KEY))
                .andExpect(jsonPath("$.code").value(401));
        mvc.perform(put(URL).contextPath("/xiaozhi").header("X-API-Key", KEY)
                        .contentType("application/json").content("{\"nickname\":\"" + "n".repeat(65) + "\"}"))
                .andExpect(status().isBadRequest());
        verifyNoInteractions(profiles);
    }

    @Test
    void keyAndBearerCanSaveSixFieldsWithNumericCount() throws Exception {
        var profile = new Profile("Alice", "zh-CN", "history", "brief", "self", 3);
        when(profiles.upsertProfile(anyLong(), eq("customer-user"), eq(profile)))
                .thenReturn(new ProfileView("customer-user", 2, profile));
        for (String header : List.of("X-API-Key", "Authorization")) {
            mvc.perform(put(URL).contextPath("/xiaozhi")
                            .header(header, header.equals("X-API-Key") ? KEY : "Bearer fixture-login-token")
                            .contentType("application/json").content("""
                            {"nickname":"Alice","language":"zh-CN","interests":"history",
                             "explanationStyle":"brief","relationship":"self","usageCount":3}
                            """))
                    .andExpect(status().isOk()).andExpect(jsonPath("$.code").value(0))
                    .andExpect(jsonPath("$.data.profile.relationship").value("self"))
                    .andExpect(jsonPath("$.data.profile.usageCount").isNumber())
                    .andExpect(jsonPath("$.data.profile.usageCount").value(3));
        }
        verify(profiles).upsertProfile(10L, "customer-user", profile);
        verify(profiles).upsertProfile(20L, "customer-user", profile);
    }

    @ParameterizedTest
    @ValueSource(strings = {"-1", "1.5", "true", "\"3\"", "2147483648"})
    void badCountsAreRejectedBeforeCallingService(String count) throws Exception {
        mvc.perform(put(URL).contextPath("/xiaozhi").header("X-API-Key", KEY)
                        .contentType("application/json").content("{\"usageCount\":" + count + "}"))
                .andExpect(status().isBadRequest());
        verifyNoInteractions(profiles);
    }

    @Test
    void keySupportsExistingIdentifierLengthLimit() throws Exception {
        String guid = "g".repeat(128);
        mvc.perform(get("/xiaozhi/user/profile/" + guid).contextPath("/xiaozhi").header("X-API-Key", KEY))
                .andExpect(status().isOk()).andExpect(jsonPath("$.code").value(0));
        verify(profiles).getProfile(10L, guid);
        mvc.perform(get("/xiaozhi/user/profile/" + guid + "g").contextPath("/xiaozhi").header("X-API-Key", KEY))
                .andExpect(status().isForbidden());
        verifyNoMoreInteractions(profiles);
    }

    @Test
    void invalidHashFailsClosedAndEachRequestChecksDatabaseAgain() {
        assertEquals(10L, auth.authenticate(KEY));
        var invalid = new ProfileApiKeyEntity();
        invalid.setSecretHash("not-hex");
        when(keys.findActive(KEY_ID)).thenReturn(invalid);
        assertNull(auth.authenticate(KEY));
        when(keys.findActive(KEY_ID)).thenReturn(null);
        assertNull(auth.authenticate(KEY));
        verify(keys, times(3)).findActive(KEY_ID);
        assertNull(auth.authenticate(null));
    }
}
