package xiaozhi.modules.binding.service;

import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.util.Arrays;
import java.util.Base64;
import java.util.List;

import javax.crypto.Cipher;
import javax.crypto.spec.GCMParameterSpec;
import javax.crypto.spec.SecretKeySpec;

import org.springframework.stereotype.Component;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.ObjectMapper;
import lombok.RequiredArgsConstructor;
import xiaozhi.common.constant.Constant;
import xiaozhi.modules.binding.dto.UserBindingDTOs.Binding;
import xiaozhi.modules.binding.entity.MqttBindingOutbox;
import xiaozhi.modules.sys.service.SysParamsService;

@Component
@RequiredArgsConstructor
public class MqttBindingCipher {
    private static final byte[] DOMAIN = "xiaozhi:mqtt-binding-outbox:v1\0".getBytes(StandardCharsets.UTF_8);
    private static final SecureRandom RANDOM = new SecureRandom();
    private final SysParamsService params;
    // Persistence must retain milliseconds independently of the application's display date format.
    private final ObjectMapper json = new ObjectMapper();

    public String encrypt(MqttBindingOutbox row, Binding binding) {
        byte[] plaintext = null;
        try {
            byte[] nonce = new byte[12];
            RANDOM.nextBytes(nonce);
            Cipher cipher = cipher(Cipher.ENCRYPT_MODE, nonce, row);
            plaintext = json.writeValueAsBytes(binding);
            byte[] ciphertext = cipher.doFinal(plaintext);
            return "v1." + Base64.getEncoder().encodeToString(
                    ByteBuffer.allocate(nonce.length + ciphertext.length).put(nonce).put(ciphertext).array());
        } catch (GeneralSecurityException | JsonProcessingException e) {
            // Do not attach serializer exceptions: they can contain plaintext field values.
            throw new IllegalStateException("MQTT command encryption failed");
        } finally {
            if (plaintext != null) Arrays.fill(plaintext, (byte) 0);
        }
    }

    public Binding decrypt(MqttBindingOutbox row) {
        byte[] plaintext = null;
        try {
            String encoded = row.getEncryptedBinding();
            if (encoded == null || !encoded.startsWith("v1.")) throw new GeneralSecurityException();
            byte[] bytes = Base64.getDecoder().decode(encoded.substring(3));
            if (bytes.length < 28) throw new GeneralSecurityException();
            Cipher cipher = cipher(Cipher.DECRYPT_MODE, Arrays.copyOf(bytes, 12), row);
            plaintext = cipher.doFinal(bytes, 12, bytes.length - 12);
            return json.readValue(plaintext, Binding.class);
        } catch (GeneralSecurityException | java.io.IOException | IllegalArgumentException e) {
            throw new IllegalStateException("MQTT command decryption failed");
        } finally {
            if (plaintext != null) Arrays.fill(plaintext, (byte) 0);
        }
    }

    private Cipher cipher(int mode, byte[] nonce, MqttBindingOutbox row)
            throws GeneralSecurityException, JsonProcessingException {
        String secret = params.getValue(Constant.SERVER_SECRET, true);
        if (secret == null || secret.isBlank()) throw new IllegalStateException("Server secret is not configured");
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        digest.update(DOMAIN);
        byte[] key = digest.digest(secret.getBytes(StandardCharsets.UTF_8));
        try {
            Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
            cipher.init(mode, new SecretKeySpec(key, "AES"), new GCMParameterSpec(128, nonce));
            cipher.updateAAD(json.writeValueAsBytes(List.of("v1", row.getMacAddress(), row.getCabGuid(),
                    row.getRequestId(), row.getMappingId(), row.getBootId(), row.getUserGuid(),
                    row.getUsageSessionId(), row.getBindingVersion().toString(),
                    Long.toString(row.getExpiresAt().getTime()))));
            return cipher;
        } finally {
            Arrays.fill(key, (byte) 0);
        }
    }
}
