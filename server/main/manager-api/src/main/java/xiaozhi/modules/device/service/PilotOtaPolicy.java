package xiaozhi.modules.device.service;

import java.util.Locale;
import cn.hutool.json.JSONArray;
import cn.hutool.json.JSONObject;
import cn.hutool.json.JSONUtil;

/** Optional, administrator-defined rollout of an immutable image to named devices. */
public final class PilotOtaPolicy {
    public static final String PARAM_CODE = "server.ota_pilot";
    private static final String TYPE_PREFIX = "pilot-";

    private PilotOtaPolicy() {}

    public static boolean isPilotType(String type) {
        return type != null && type.startsWith(TYPE_PREFIX);
    }

    public static String pilotType(String boardType) {
        return TYPE_PREFIX + boardType;
    }

    public static String firmwareId(String config, String boardType, String macAddress) {
        String mac = canonicalMac(macAddress);
        if (config == null || config.isBlank() || config.length() > 8192 ||
                boardType == null || boardType.isBlank() || isPilotType(boardType) || mac == null) {
            return null;
        }
        try {
            JSONObject root = JSONUtil.parseObj(config);
            if (!Boolean.TRUE.equals(root.get("enabled")) ||
                    !boardType.equals(root.get("boardType"))) {
                return null;
            }
            Object id = root.get("otaId");
            Object targetList = root.get("macAddresses");
            if (!(id instanceof String) || !((String) id).matches("[a-fA-F0-9]{32}") ||
                    !(targetList instanceof JSONArray)) {
                return null;
            }
            JSONArray targets = (JSONArray) targetList;
            if (targets.isEmpty() || targets.size() > 32) {
                return null;
            }
            boolean selected = false;
            for (Object target : targets) {
                String normalized = target instanceof String ? canonicalMac((String) target) : null;
                if (normalized == null) {
                    return null;
                }
                selected |= mac.equals(normalized);
            }
            return selected ? (String) id : null;
        } catch (RuntimeException malformedConfig) {
            return null;
        }
    }

    private static String canonicalMac(String value) {
        if (value == null || !value.matches("(?i)[0-9a-f]{2}(:[0-9a-f]{2}){5}")) {
            return null;
        }
        return value.toLowerCase(Locale.ROOT);
    }
}
