package xiaozhi.modules.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import com.fasterxml.jackson.databind.ObjectMapper;
import xiaozhi.common.exception.RenException;
import xiaozhi.common.validator.ValidatorUtils;
import xiaozhi.modules.binding.dto.UserBindingDTOs.Profile;
import xiaozhi.modules.security.config.WebMvcConfig;

class ProfileFieldsTest {
    private final ObjectMapper json = new WebMvcConfig().jackson2HttpMessageConverter().getObjectMapper();

    @Test
    void newFieldsRoundTripThroughBothWireAndSnapshotMappers() throws Exception {
        var input = new Profile("Alice", "zh-CN", "history", "brief", "\u8001\u5a46", 12);
        for (var mapper : new ObjectMapper[]{json, new ObjectMapper()}) {
            String encoded = mapper.writeValueAsString(input);
            assertEquals(input, mapper.readValue(encoded, Profile.class));
            assertEquals("\u8001\u5a46", mapper.readTree(encoded).get("relationship").textValue());
            assertTrue(mapper.readTree(encoded).get("usageCount").isIntegralNumber());
        }
    }

    @Test
    void oldSnapshotsAndRequestsKeepMissingMetadataNull() throws Exception {
        Profile old = json.readValue("{\"nickname\":\"Alice\"}", Profile.class);
        assertNull(old.relationship());
        assertNull(old.usageCount());
        assertEquals(old, new ObjectMapper().readValue("{\"nickname\":\"Alice\"}", Profile.class));
        Profile nulls = json.readValue("{\"relationship\":null,\"usageCount\":null}", Profile.class);
        assertNull(nulls.relationship());
        assertNull(nulls.usageCount());
        assertDoesNotThrow(() -> json.readValue("{\"futureField\":123}", Profile.class));
    }

    @Test
    void supportsExplicitClearAndIntegerBounds() throws Exception {
        Profile clear = json.readValue("{\"relationship\":\"\",\"usageCount\":0}", Profile.class);
        assertEquals("", clear.relationship());
        assertEquals(0, clear.usageCount());
        assertDoesNotThrow(() -> ValidatorUtils.validateEntity(clear));
        Profile max = json.readValue("{\"usageCount\":2147483647}", Profile.class);
        assertEquals(Integer.MAX_VALUE, max.usageCount());
    }

    @ParameterizedTest
    @ValueSource(strings = {"-1", "1.5", "1.0", "true", "\"3\"", "\"\"", "2147483648", "{}", "[]"})
    void invalidCountsAreRejectedInsteadOfCoerced(String value) {
        assertThrows(com.fasterxml.jackson.core.JsonProcessingException.class,
                () -> json.readValue("{\"usageCount\":" + value + "}", Profile.class));
    }

    @Test
    void serviceValidationAlsoRejectsInvalidJavaInput() {
        assertThrows(RenException.class, () -> ValidatorUtils.validateEntity(
                new Profile(null, null, null, null, "r".repeat(65), 0)));
        assertThrows(RenException.class, () -> ValidatorUtils.validateEntity(
                new Profile(null, null, null, null, "self", -1)));
        assertDoesNotThrow(() -> ValidatorUtils.validateEntity(
                new Profile(null, null, null, null, "r".repeat(64), Integer.MAX_VALUE)));
    }
}
