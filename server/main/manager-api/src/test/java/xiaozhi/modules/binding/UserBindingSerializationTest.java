package xiaozhi.modules.binding;

import static org.junit.jupiter.api.Assertions.*;
import static org.mockito.Mockito.*;

import java.util.Date;
import java.util.Map;

import org.junit.jupiter.api.Test;
import org.springframework.http.MediaType;
import org.springframework.http.converter.json.MappingJackson2HttpMessageConverter;
import org.springframework.mock.http.MockHttpOutputMessage;

import com.fasterxml.jackson.databind.JsonNode;
import xiaozhi.common.utils.Result;
import xiaozhi.modules.agent.dto.AgentChatHistoryDTO;
import xiaozhi.modules.agent.dto.AgentChatHistoryReportDTO;
import xiaozhi.modules.agent.entity.AgentChatHistoryEntity;
import xiaozhi.modules.binding.dto.UserBindingDTOs.*;
import xiaozhi.modules.binding.entity.UsageSessionEntity;
import xiaozhi.modules.binding.service.UserBindingService;
import xiaozhi.modules.config.controller.ConfigController;
import xiaozhi.modules.config.service.ConfigService;
import xiaozhi.modules.security.config.WebMvcConfig;

class UserBindingSerializationTest {
    // Exercise the real MVC converter, including its primitive/boxed Long ToStringSerializer module.
    private final MappingJackson2HttpMessageConverter converter = new WebMvcConfig().jackson2HttpMessageConverter();
    private final Profile profile = new Profile("Guide", "en", null, null);

    private JsonNode wire(Object body) throws Exception {
        MockHttpOutputMessage output = new MockHttpOutputMessage();
        converter.write(body, MediaType.APPLICATION_JSON, output);
        return converter.getObjectMapper().readTree(output.getBodyAsBytes());
    }

    private void integral(JsonNode object, String field, long expected) {
        assertNotNull(object.get(field), field);
        assertTrue(object.get(field).isIntegralNumber(), field + " must be a JSON integer: " + object.get(field));
        assertEquals(expected, object.get(field).longValue(), field);
    }

    @Test
    void productionConverterKeepsLegacyLongIdsAsStrings() throws Exception {
        JsonNode ordinary = wire(Map.of("id", 9007199254740993L));
        assertTrue(ordinary.get("id").isTextual());
        assertEquals("9007199254740993", ordinary.get("id").textValue());
        record LegacyNumbers(long primitiveId, Long boxedId) {}
        JsonNode legacy = wire(new LegacyNumbers(17L, 18L));
        assertTrue(legacy.get("primitiveId").isTextual());
        assertTrue(legacy.get("boxedId").isTextual());
    }

    @Test
    void createAndBothFirmwareCommandsEmitIntegralVersions() throws Exception {
        Command command = new Command("bindUser", "cmd-1", "user-1", "boot-1", 7L, "usage-1", "token");
        Binding binding = new Binding("user-1", "boot-1", 7L, "usage-1", "token", profile, 3L,
                "agent-1", new Date(1700000000123L), command);
        JsonNode response = wire(new Result<Binding>().ok(binding)).get("data");
        integral(response, "bindingVersion", 7L);
        integral(response, "profileVersion", 3L);
        integral(response.get("command"), "bindingVersion", 7L);
        JsonNode revoke = wire(new Result<Command>().ok(
                new Command("unbindUser", "cmd-2", "", "boot-1", 8L, null, null))).get("data");
        integral(revoke, "bindingVersion", 8L);
        assertFalse(revoke.has("bindingToken"));
    }

    @Test
    void resolveControllerResponseUsesIntegralVersionsWithProductionConverter() throws Exception {
        UserBindingService service = mock(UserBindingService.class);
        Resolve request = new Resolve("aa:bb:cc:dd:ee:ff", "user-1", "boot-1", 7L, "t".repeat(43));
        when(service.resolve(request)).thenReturn(new Binding("user-1", "boot-1", 7L, "usage-1", null,
                profile, 3L, "agent-1", new Date(1700000000123L), null));
        ConfigController controller = new ConfigController(mock(ConfigService.class), service);
        JsonNode response = wire(controller.resolveUserBinding(request)).get("data");
        integral(response, "bindingVersion", 7L);
        integral(response, "profileVersion", 3L);
        assertFalse(response.has("bindingToken"));
        integral(wire(request), "bindingVersion", 7L);
    }

    @Test
    void profileAndUsageSessionVersionsAreNumbersButOwnerIdStaysString() throws Exception {
        integral(wire(new ProfileView("user-1", 3L, profile)), "profileVersion", 3L);
        UsageSessionEntity session = new UsageSessionEntity();
        session.setBindingVersion(7L);
        session.setProfileVersion(3L);
        session.setOwnerId(10L);
        session.setTokenHash("hash");
        JsonNode response = wire(session);
        integral(response, "bindingVersion", 7L);
        integral(response, "profileVersion", 3L);
        assertTrue(response.get("ownerId").isTextual());
        assertFalse(response.has("tokenHash"));
    }

    @Test
    void chatResponseAndReportProtocolNumbersStayIntegral() throws Exception {
        long timestamp = 1700000000123L;
        AgentChatHistoryEntity entity = AgentChatHistoryEntity.builder().id(11L).ownerId(10L)
                .bindingVersion(7L).profileVersion(3L).createdAt(new Date(timestamp)).build();
        JsonNode response = wire(entity);
        integral(response, "bindingVersion", 7L);
        integral(response, "profileVersion", 3L);
        integral(response, "reportTime", timestamp);
        assertTrue(response.get("id").isTextual());
        assertTrue(response.get("ownerId").isTextual());
        AgentChatHistoryDTO dto = new AgentChatHistoryDTO();
        dto.setBindingVersion(7L);
        dto.setProfileVersion(3L);
        dto.setCreatedAt(new Date(timestamp));
        JsonNode history = wire(dto);
        integral(history, "bindingVersion", 7L);
        integral(history, "profileVersion", 3L);
        integral(history, "reportTime", timestamp);
        AgentChatHistoryReportDTO report = new AgentChatHistoryReportDTO();
        report.setBindingVersion(7L);
        report.setReportTime(timestamp);
        JsonNode input = wire(report);
        integral(input, "bindingVersion", 7L);
        integral(input, "reportTime", timestamp);
    }

    @Test
    void optionalLegacyMetadataRemainsNullInsteadOfZero() throws Exception {
        JsonNode history = wire(new AgentChatHistoryDTO());
        assertTrue(history.get("bindingVersion").isNull());
        assertTrue(history.get("profileVersion").isNull());
        assertTrue(history.get("reportTime").isNull());
    }
}
