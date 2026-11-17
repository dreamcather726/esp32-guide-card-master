package xiaozhi.modules.binding.json;

import com.fasterxml.jackson.databind.ser.std.NumberSerializer;

/** Numeric protocol fields opt out of the application's Long-ID string encoding. */
public class ProtocolNumberSerializer extends NumberSerializer {
    public ProtocolNumberSerializer() {
        super(Long.class);
    }
}
