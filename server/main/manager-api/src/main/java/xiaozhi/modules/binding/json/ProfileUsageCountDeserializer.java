package xiaozhi.modules.binding.json;

import java.io.IOException;

import com.fasterxml.jackson.core.JsonParser;
import com.fasterxml.jackson.core.JsonToken;
import com.fasterxml.jackson.databind.DeserializationContext;
import com.fasterxml.jackson.databind.JsonDeserializer;

public class ProfileUsageCountDeserializer extends JsonDeserializer<Integer> {
    @Override
    public Integer deserialize(JsonParser parser, DeserializationContext context) throws IOException {
        if (!parser.hasToken(JsonToken.VALUE_NUMBER_INT)) {
            return context.reportInputMismatch(Integer.class, "usageCount must be an integer from 0 to 2147483647");
        }
        int count = parser.getIntValue();
        if (count < 0) {
            return context.reportInputMismatch(Integer.class, "usageCount must be an integer from 0 to 2147483647");
        }
        return count;
    }
}
