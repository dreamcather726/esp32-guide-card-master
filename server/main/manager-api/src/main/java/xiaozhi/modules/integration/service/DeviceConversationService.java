package xiaozhi.modules.integration.service;

import java.util.Date;
import java.util.List;
import java.util.Objects;

import org.springframework.stereotype.Service;
import org.springframework.transaction.annotation.Transactional;

import lombok.RequiredArgsConstructor;
import xiaozhi.common.exception.RenException;
import xiaozhi.modules.binding.dao.UserBindingDao;
import xiaozhi.modules.binding.service.UserBindingService;
import xiaozhi.modules.device.entity.DeviceEntity;
import xiaozhi.modules.integration.dao.DeviceIntegrationDao;
import xiaozhi.modules.integration.dto.DeviceIntegrationDTOs.*;

@Service
@RequiredArgsConstructor
public class DeviceConversationService {
    private final DeviceIntegrationDao dao;
    private final UserBindingDao devices;

    public DeviceEntity ownedDevice(Long owner, String identifier) {
        if (owner == null || identifier == null || !identifier.matches("[A-Za-z0-9_:\\-]{1,128}")) {
            throw new RenException("Invalid device identifier or owner");
        }
        String mac = identifier.matches("(?i)[0-9a-f]{2}([:-][0-9a-f]{2}){5}")
                ? UserBindingService.normalizeMac(identifier) : "";
        List<DeviceEntity> found = dao.devices(owner, identifier, mac);
        if (found.size() != 1) throw new RenException("Device not available or identifier is ambiguous");
        return found.getFirst();
    }

    public DeviceEntity lockOwnedDevice(Long owner, String identifier) {
        var hint = ownedDevice(owner, identifier);
        var locked = devices.lockDevice(hint.getMacAddress());
        if (locked == null || !Objects.equals(locked.getUserId(), owner)
                || !Objects.equals(locked.getId(), hint.getId())) throw new RenException("Device not available");
        return locked;
    }

    @Transactional(readOnly=true)
    public History history(Long owner, String identifier, int page, int limit) {
        if (page < 1 || page > 1_000_000 || limit < 1 || limit > 100) {
            throw new RenException("page must be 1..1000000 and limit must be 1..100");
        }
        var device = ownedDevice(owner, identifier);
        var records = dao.history(owner, device.getMacAddress(), limit, (long) (page-1)*limit);
        return new History(device.getId(), device.getMacAddress(), page, limit,
                dao.historyCount(owner, device.getMacAddress()), records.stream().map(Message::from).toList());
    }

    @Transactional(rollbackFor=Exception.class)
    public Cleared clear(Long owner, String identifier) {
        var device = lockOwnedDevice(owner, identifier);
        dao.rememberDeletedIds(owner, device.getMacAddress());
        dao.deleteExclusiveAudio(owner, device.getMacAddress());
        long count = dao.deleteHistory(owner, device.getMacAddress());
        dao.saveClear(owner, device.getMacAddress());
        return new Cleared(device.getId(), device.getMacAddress(), count,
                dao.clearedAt(owner, device.getMacAddress()), "pending");
    }

    // Reports and clears share the device lock; retries carry their original event timestamp.
    public DeviceEntity lockForReport(String mac) {
        return devices.lockDevice(UserBindingService.normalizeMac(mac));
    }

    public boolean discardedReport(Long owner, String mac, Long timestamp, String messageId) {
        return discardedReport(owner, mac, timestamp, messageId, null);
    }

    public boolean discardedReport(Long owner, String mac, Long timestamp, String messageId, Long generation) {
        if (messageId != null && dao.deletedMessage(messageId) > 0) return true;
        if (owner == null) return false;
        Date cutoff = dao.clearedAt(owner, mac);
        if (cutoff == null) return false;
        if (generation != null && generation < cutoff.getTime()) return true;
        if (timestamp == null) throw new RenException("reportTime is required after device history was cleared");
        long millis = timestamp < 100_000_000_000L ? Math.multiplyExact(timestamp, 1000) : timestamp;
        return millis <= cutoff.getTime();
    }
}
