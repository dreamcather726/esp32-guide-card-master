package xiaozhi.modules.binding.service;

import java.time.Clock;
import java.util.Date;
import java.util.Objects;
import java.util.Set;
import java.util.UUID;

import org.springframework.scheduling.annotation.Scheduled;
import org.springframework.stereotype.Service;
import org.springframework.transaction.annotation.Isolation;
import org.springframework.transaction.annotation.Transactional;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;
import lombok.RequiredArgsConstructor;
import xiaozhi.common.exception.RenException;
import xiaozhi.common.validator.ValidatorUtils;
import xiaozhi.modules.binding.dao.MqttBindingDao;
import xiaozhi.modules.binding.dao.UserBindingDao;
import xiaozhi.modules.binding.dto.MqttBindingDTOs.*;
import xiaozhi.modules.binding.dto.UserBindingDTOs.*;
import xiaozhi.modules.binding.entity.MqttBindingMapping;
import xiaozhi.modules.binding.entity.MqttBindingOutbox;
import xiaozhi.modules.device.entity.DeviceEntity;

@Service
@RequiredArgsConstructor
public class MqttBindingService {
    private static final long FRESH_MILLIS = 180_000;
    private static final Set<String> TEXT_FIELDS = Set.of("imei", "userGUID", "groupID", "bindingState",
            "usageSessionID", "fwVersion");
    private static final Set<String> METRICS = Set.of("Battery Level", "Volume", "Screen Brightness",
            "Signal Strength", "TF Card Capacity", "TF Card Remaining Space", "Total Runtime",
            "Single Session Runtime", "Playback Duration", "Conversation Count", "Role");
    private final MqttBindingDao dao;
    private final UserBindingDao devices;
    private final UserBindingService bindings;
    private final MqttBindingCipher cipher;
    private final ObjectMapper json;
    private Clock clock = Clock.systemUTC();

    @Transactional(rollbackFor = Exception.class, isolation = Isolation.READ_COMMITTED)
    public View pair(Long owner, String mac, Pair request) {
        ValidatorUtils.validateEntity(request);
        identifier(request.cabGuid());
        mac = UserBindingService.normalizeMac(mac);
        DeviceEntity device = ownedDevice(owner, mac);
        MqttBindingMapping previous = dao.lockMapping(mac);
        if (matches(previous, device) && Objects.equals(previous.getCabGuid(), request.cabGuid())) {
            return view(mac, previous);
        }
        MqttBindingMapping occupied = dao.byCab(request.cabGuid());
        if (occupied != null && !Objects.equals(mac, occupied.getMacAddress())) {
            throw new RenException("Cabinet mapping is already registered");
        }
        MqttBindingMapping mapping = new MqttBindingMapping();
        mapping.setMacAddress(mac);
        mapping.setCabGuid(request.cabGuid());
        mapping.setMappingId(UUID.randomUUID().toString());
        mapping.setDeviceId(device.getId());
        mapping.setOwnerId(owner);
        if (previous == null) dao.insertMapping(mapping);
        else dao.updateMapping(mapping);
        dao.invalidateCommands(mac);
        return view(mac, mapping);
    }

    @Transactional(rollbackFor = Exception.class, isolation = Isolation.READ_COMMITTED)
    public View current(Long owner, String mac) {
        mac = UserBindingService.normalizeMac(mac);
        DeviceEntity device = ownedDevice(owner, mac);
        MqttBindingMapping mapping = dao.lockMapping(mac);
        return view(mac, matches(mapping, device) ? mapping : null);
    }

    @Transactional(rollbackFor = Exception.class, isolation = Isolation.READ_COMMITTED)
    public View online(JsonNode report) {
        ObjectNode clean = sanitize(report);
        MqttBindingMapping mapping = registered(clean.get("cabGuid").textValue());
        if (clean.has("macAddress") && !mapping.getMacAddress().equals(clean.get("macAddress").textValue())) {
            throw new RenException("Online report does not match registered device");
        }
        String boot = clean.get("bootID").textValue();
        if (!Objects.equals(boot, mapping.getBootId())) dao.invalidateCommands(mapping.getMacAddress());
        mapping.setBootId(boot);
        mapping.setReportJson(clean.toString());
        mapping.setReceivedAt(Date.from(clock.instant()));
        dao.observe(mapping);
        return view(mapping.getMacAddress(), mapping);
    }

    // The nonlocking cabinet hint must not pin an older session/outbox snapshot while awaiting the device lock.
    @Transactional(rollbackFor = Exception.class, isolation = Isolation.READ_COMMITTED)
    public Binding legacy(Legacy request) {
        ValidatorUtils.validateEntity(request);
        identifier(request.cabGuid());
        identifier(request.bootId());
        identifier(request.userGuid());
        identifier(request.requestId());
        MqttBindingMapping mapping = registered(request.cabGuid());
        if (!fresh(mapping) || !Objects.equals(mapping.getBootId(), request.bootId())) {
            throw new RenException("A fresh online report for the same boot is required");
        }
        String mac = mapping.getMacAddress();
        // Device and mapping are already locked; current/create/resolve now take the binding-state lock.
        Binding active = bindings.current(mapping.getOwnerId(), mac);
        MqttBindingOutbox previous = dao.request(mac, request.cabGuid(), request.requestId());
        if (previous != null) {
            if (!Objects.equals(previous.getBootId(), request.bootId())
                    || !Objects.equals(previous.getUserGuid(), request.userGuid())) {
                throw new RenException("requestID was already used with a different payload");
            }
            return replay(mapping, previous, active);
        }

        Binding result = null;
        Date expiresAt = null;
        if (active != null && Objects.equals(active.bootId(), request.bootId())
                && Objects.equals(active.userGuid(), request.userGuid())) {
            MqttBindingOutbox cached = dao.reusable(mac, request.cabGuid(), mapping.getMappingId(),
                    active.usageSessionId(), Date.from(clock.instant()));
            if (cached != null) {
                result = replay(mapping, cached, active);
                expiresAt = cached.getExpiresAt();
            } else if (observedReady(mapping, active)) {
                throw new RenException("Device already reports the active binding ready");
            }
        }
        if (result == null) {
            result = bindings.create(mapping.getOwnerId(), mac, new Create(request.bootId(), request.userGuid(), null));
            expiresAt = new Date(Math.min(clock.millis() + FRESH_MILLIS, result.expiresAt().getTime()));
        }
        MqttBindingOutbox outbox = new MqttBindingOutbox();
        outbox.setMacAddress(mac);
        outbox.setCabGuid(request.cabGuid());
        outbox.setRequestId(request.requestId());
        outbox.setMappingId(mapping.getMappingId());
        outbox.setBootId(request.bootId());
        outbox.setUserGuid(request.userGuid());
        outbox.setUsageSessionId(result.usageSessionId());
        outbox.setBindingVersion(result.bindingVersion());
        outbox.setExpiresAt(expiresAt);
        outbox.setEncryptedBinding(cipher.encrypt(outbox, result));
        dao.insertOutbox(outbox);
        return result;
    }

    private boolean observedReady(MqttBindingMapping mapping, Binding active) {
        try {
            JsonNode report = json.readTree(mapping.getReportJson());
            return "ready".equals(report.path("bindingState").textValue())
                    && Objects.equals(active.bootId(), report.path("bootID").textValue())
                    && Objects.equals(active.userGuid(), report.path("userGUID").textValue())
                    && Objects.equals(active.usageSessionId(), report.path("usageSessionID").textValue())
                    && report.path("bindingVersion").isIntegralNumber()
                    && active.bindingVersion() == report.path("bindingVersion").longValue();
        } catch (JsonProcessingException e) {
            throw new IllegalStateException("Stored MQTT report is invalid");
        }
    }

    // Keep token-free tombstones indefinitely: deleting request IDs would permit delayed replay to rotate a binding.
    @Scheduled(fixedDelay = 60_000)
    public void purgeExpiredCommands() {
        dao.purgeExpiredCommands(Date.from(clock.instant()));
    }

    private Binding replay(MqttBindingMapping mapping, MqttBindingOutbox outbox, Binding active) {
        if (!Objects.equals(mapping.getMappingId(), outbox.getMappingId())
                || active == null || !Objects.equals(active.usageSessionId(), outbox.getUsageSessionId())
                || active.bindingVersion() != outbox.getBindingVersion()
                || !Objects.equals(active.bootId(), outbox.getBootId())
                || !Objects.equals(active.userGuid(), outbox.getUserGuid())
                || outbox.getExpiresAt().getTime() <= clock.millis() || outbox.getEncryptedBinding() == null) {
            throw new RenException("MQTT request is expired, revoked or superseded");
        }
        Binding result = cipher.decrypt(outbox);
        bindings.resolve(new Resolve(mapping.getMacAddress(), result.userGuid(), result.bootId(),
                result.bindingVersion(), result.bindingToken()));
        return result;
    }

    private DeviceEntity ownedDevice(Long owner, String mac) {
        if (owner == null) throw new RenException("Login required");
        DeviceEntity device = devices.lockDevice(mac);
        if (device == null || !Objects.equals(owner, device.getUserId())) throw new RenException("Device not available");
        return device;
    }

    private MqttBindingMapping registered(String cab) {
        MqttBindingMapping hint = dao.byCab(cab);
        if (hint == null) throw new RenException("Explicit device mapping is required");
        DeviceEntity device = devices.lockDevice(hint.getMacAddress());
        MqttBindingMapping mapping = dao.lockMapping(hint.getMacAddress());
        if (!matches(mapping, device) || !Objects.equals(cab, mapping.getCabGuid())) {
            throw new RenException("Registered device mapping is no longer valid");
        }
        return mapping;
    }

    private boolean matches(MqttBindingMapping mapping, DeviceEntity device) {
        return mapping != null && device != null && device.getUserId() != null
                && Objects.equals(mapping.getMacAddress(), UserBindingService.normalizeMac(device.getMacAddress()))
                && Objects.equals(mapping.getDeviceId(), device.getId())
                && Objects.equals(mapping.getOwnerId(), device.getUserId());
    }

    private boolean fresh(MqttBindingMapping mapping) {
        if (mapping.getReceivedAt() == null || mapping.getBootId() == null) return false;
        long age = clock.millis() - mapping.getReceivedAt().getTime();
        return age >= 0 && age <= FRESH_MILLIS;
    }

    private View view(String mac, MqttBindingMapping mapping) {
        if (mapping == null) return new View(mac, null, null, null, false);
        try {
            JsonNode report = mapping.getReportJson() == null ? null : sanitize(json.readTree(mapping.getReportJson()));
            return new View(mac, mapping.getCabGuid(), report,
                    mapping.getReceivedAt() == null ? null : mapping.getReceivedAt().toInstant().toString(), fresh(mapping));
        } catch (JsonProcessingException e) {
            throw new IllegalStateException("Stored MQTT report is invalid");
        }
    }

    private ObjectNode sanitize(JsonNode report) {
        if (report == null || !report.isObject() || !"KJ".equals(report.path("cmd").textValue())) {
            throw new RenException("Expected a KJ JSON object");
        }
        String cab = report.path("cabGuid").textValue();
        String boot = report.path("bootID").textValue();
        identifier(cab);
        identifier(boot);
        ObjectNode clean = json.createObjectNode().put("cmd", "KJ").put("cabGuid", cab).put("bootID", boot);
        if (report.has("macAddress")) {
            clean.put("macAddress", UserBindingService.normalizeMac(report.get("macAddress").textValue()));
        }
        for (String key : TEXT_FIELDS) {
            JsonNode value = report.get(key);
            if (value == null) continue;
            if (!value.isTextual() || value.textValue().length() > 128) throw new RenException("Invalid KJ field");
            clean.set(key, value);
        }
        JsonNode version = report.get("bindingVersion");
        if (version != null) {
            if (!version.isIntegralNumber() || !version.canConvertToLong() || version.longValue() < 0) {
                throw new RenException("Invalid KJ bindingVersion");
            }
            clean.set("bindingVersion", version);
        }
        for (String key : METRICS) {
            JsonNode value = report.get(key);
            if (value != null && (value.isTextual() || value.isNumber() || value.isBoolean())
                    && value.asText().length() <= 128) clean.set(key, value);
        }
        return clean;
    }

    private void identifier(String value) {
        if (value == null || value.isBlank() || value.length() > 128 || !value.equals(value.trim())
                || value.chars().anyMatch(Character::isISOControl)) throw new RenException("Invalid identifier");
    }
}
