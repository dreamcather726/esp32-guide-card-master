package xiaozhi.modules.binding;

import static org.junit.jupiter.api.Assertions.*;
import static org.mockito.Mockito.*;

import java.util.Date;

import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

import com.fasterxml.jackson.databind.ObjectMapper;
import xiaozhi.common.constant.Constant;
import xiaozhi.modules.binding.dto.UserBindingDTOs.*;
import xiaozhi.modules.binding.entity.MqttBindingOutbox;
import xiaozhi.modules.binding.service.MqttBindingCipher;
import xiaozhi.modules.sys.service.SysParamsService;

class MqttBindingCipherTest {
    private final SysParamsService params = mock(SysParamsService.class);
    private final MqttBindingCipher cipher = new MqttBindingCipher(params);
    private final Binding binding = new Binding("user-1", "boot-1", 7, "session-1", "secret-token",
            new Profile("name", null, null, null), 3, "agent-1", new Date(1700000000123L),
            new Command("bindUser", "cmd-1", "user-1", "boot-1", 7, "session-1", "secret-token"));
    private MqttBindingOutbox row;

    @BeforeEach
    void setup() {
        when(params.getValue(Constant.SERVER_SECRET, true)).thenReturn("stable-test-secret");
        row = new MqttBindingOutbox();
        row.setMacAddress("aa:bb:cc:dd:ee:ff");
        row.setCabGuid("cab-1");
        row.setRequestId("request-1");
        row.setMappingId("mapping-1");
        row.setBootId("boot-1");
        row.setUserGuid("user-1");
        row.setUsageSessionId("session-1");
        row.setBindingVersion(7L);
        row.setExpiresAt(new Date(1700000000456L));
    }

    @Test
    void independentInstancesRecoverIdenticalCommandWithRandomNonceAndNoSerializationLeak() throws Exception {
        row.setEncryptedBinding(cipher.encrypt(row, binding));
        assertNotEquals(row.getEncryptedBinding(), cipher.encrypt(row, binding));
        assertEquals(binding, new MqttBindingCipher(params).decrypt(row));
        assertFalse(row.toString().contains(row.getEncryptedBinding()));
        assertFalse(new ObjectMapper().writeValueAsString(row).contains("encryptedBinding"));
        assertFalse(row.getEncryptedBinding().contains(binding.bindingToken()));
        assertFalse(binding.toString().contains(binding.bindingToken()));
        assertFalse(binding.command().toString().contains(binding.bindingToken()));
    }

    @Test
    void alteredCiphertextAndAssociatedIdentityFailClosedWithoutSecretsInExceptions() {
        String encrypted = cipher.encrypt(row, binding);
        row.setEncryptedBinding(encrypted);
        row.setRequestId("different-request");
        IllegalStateException failure = assertThrows(IllegalStateException.class, () -> cipher.decrypt(row));
        assertNull(failure.getCause());
        assertFalse(failure.toString().contains("secret-token"));
        row.setRequestId("request-1");
        row.setExpiresAt(new Date(1700000000457L));
        assertThrows(IllegalStateException.class, () -> cipher.decrypt(row));
        row.setExpiresAt(new Date(1700000000456L));
        row.setEncryptedBinding(encrypted.substring(0, 10) + "!" + encrypted.substring(11));
        assertThrows(IllegalStateException.class, () -> cipher.decrypt(row));
    }

    @Test
    void missingOrRotatedServerSecretDoesNotMintReplacementCommand() {
        row.setEncryptedBinding(cipher.encrypt(row, binding));
        when(params.getValue(Constant.SERVER_SECRET, true)).thenReturn("different-server-secret");
        assertThrows(IllegalStateException.class, () -> cipher.decrypt(row));
        when(params.getValue(Constant.SERVER_SECRET, true)).thenReturn(" ");
        assertThrows(IllegalStateException.class, () -> cipher.encrypt(row, binding));
        when(params.getValue(Constant.SERVER_SECRET, true)).thenReturn(null);
        assertThrows(IllegalStateException.class, () -> cipher.decrypt(row));
    }
}
