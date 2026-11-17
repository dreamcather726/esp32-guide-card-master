package xiaozhi.modules.binding.service;

import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.HexFormat;
import java.util.regex.Pattern;

import org.springframework.stereotype.Service;

import lombok.RequiredArgsConstructor;
import xiaozhi.modules.binding.dao.ProfileApiKeyDao;

@Service
@RequiredArgsConstructor
public class ProfileApiKeyService {
    private static final Pattern KEY = Pattern.compile("vxp_[0-9a-f]{32}_[0-9a-f]{64}");
    private final ProfileApiKeyDao dao;

    public Long authenticate(String key) {
        return authenticate(key, "profile");
    }

    public Long authenticate(String key, String scope) {
        if (key == null || !KEY.matcher(key).matches()) {
            return null;
        }
        // Query each time so revocation and account suspension take effect immediately.
        var record = dao.findActive(key.substring(4, 36));
        if (record == null || record.getSecretHash() == null) {
            return null;
        }
        String scopes = record.getScopes() == null ? "profile" : record.getScopes();
        if (scope == null || java.util.Arrays.stream(scopes.split(",")).noneMatch(scope::equals)) return null;
        try {
            byte[] actual = MessageDigest.getInstance("SHA-256").digest(key.getBytes(StandardCharsets.US_ASCII));
            byte[] expected = HexFormat.of().parseHex(record.getSecretHash());
            return MessageDigest.isEqual(actual, expected) ? record.getOwnerId() : null;
        } catch (IllegalArgumentException e) {
            return null;
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException("SHA-256 unavailable", e);
        }
    }
}
