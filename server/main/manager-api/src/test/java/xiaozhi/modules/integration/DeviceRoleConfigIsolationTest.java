package xiaozhi.modules.integration;

import static org.junit.jupiter.api.Assertions.*;
import static org.mockito.Mockito.*;

import java.util.HashMap;
import java.util.List;
import java.util.Map;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.extension.ExtendWith;
import org.mockito.InjectMocks;
import org.mockito.Mock;
import org.mockito.junit.jupiter.MockitoExtension;
import org.springframework.test.util.ReflectionTestUtils;

import xiaozhi.modules.agent.dto.AgentRoleVoiceBindingDTO;
import xiaozhi.modules.agent.entity.AgentEntity;
import xiaozhi.modules.agent.service.AgentRoleVoiceBindingService;
import xiaozhi.modules.config.service.impl.ConfigServiceImpl;
import xiaozhi.modules.model.entity.ModelConfigEntity;
import xiaozhi.modules.model.service.ModelConfigService;
import xiaozhi.modules.timbre.service.TimbreService;
import xiaozhi.modules.timbre.vo.TimbreDetailsVO;

@ExtendWith(MockitoExtension.class)
class DeviceRoleConfigIsolationTest {
    @Mock AgentRoleVoiceBindingService roles;
    @Mock ModelConfigService models;
    @Mock TimbreService timbres;
    @InjectMocks ConfigServiceImpl config;

    @Test void promptOnlyRoleUsesAgentDefaultVoice() {
        var agent = agent();
        var role = role(); role.setSystemPrompt("Role prompt");
        when(roles.listByAgentId("agent")).thenReturn(List.of(role));
        var voice = new TimbreDetailsVO(); voice.setTtsVoice("default-code");
        when(timbres.get("default-voice")).thenReturn(voice);
        List<Map<String, Object>> result = ReflectionTestUtils.invokeMethod(config, "buildRoleVoiceBindings", agent);
        assertEquals("default-voice", result.getFirst().get("ttsVoiceId"));
        assertEquals("default-code", result.getFirst().get("private_voice"));
        assertEquals("Role prompt", result.getFirst().get("prompt"));
    }

    @Test void voiceOnlyRoleUsesAgentDefaultPrompt() {
        var role = role(); role.setTtsVoiceId("custom");
        when(roles.listByAgentId("agent")).thenReturn(List.of(role));
        when(timbres.get("custom")).thenReturn(new TimbreDetailsVO());
        List<Map<String, Object>> result = ReflectionTestUtils.invokeMethod(config, "buildRoleVoiceBindings", agent());
        assertEquals("Agent prompt", result.getFirst().get("prompt"));
    }

    @Test void deviceVoiceOverridesNeverMutateCacheOrAnotherResponse() {
        var model = new ModelConfigEntity(); model.setId("tts");
        var shared = new cn.hutool.json.JSONObject(Map.of("type", "test", "private_voice", "original"));
        model.setConfigJson(shared);
        when(models.getModelByIdFromCache("tts")).thenReturn(model);
        var first = build("voice-one");
        var second = build("voice-two");
        assertEquals("original", shared.get("private_voice"));
        assertEquals("voice-one", ((Map<?, ?>)((Map<?, ?>)first.get("TTS")).get("tts")).get("private_voice"));
        assertEquals("voice-two", ((Map<?, ?>)((Map<?, ?>)second.get("TTS")).get("tts")).get("private_voice"));
    }

    private Map<String, Object> build(String voice) {
        Map<String, Object> result = new HashMap<>();
        ReflectionTestUtils.invokeMethod(config, "buildModuleConfig", null, null, null, voice,
                null, null, null, null, null, null, null, null, null, null, null,
                "tts", null, null, null, result, false);
        return result;
    }

    private AgentEntity agent() {
        var agent = new AgentEntity(); agent.setId("agent"); agent.setSystemPrompt("Agent prompt");
        agent.setTtsVoiceId("default-voice"); return agent;
    }

    private AgentRoleVoiceBindingDTO role() {
        var role = new AgentRoleVoiceBindingDTO(); role.setId("role"); role.setEnabled(true); return role;
    }
}
