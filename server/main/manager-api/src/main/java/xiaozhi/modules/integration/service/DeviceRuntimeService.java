package xiaozhi.modules.integration.service;

import java.net.URI;
import java.net.http.*;
import java.time.Duration;
import java.util.Map;

import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Service;
import com.fasterxml.jackson.databind.ObjectMapper;
import lombok.RequiredArgsConstructor;
import xiaozhi.common.constant.Constant;
import xiaozhi.modules.sys.service.SysParamsService;

@Service
@RequiredArgsConstructor
public class DeviceRuntimeService {
    private final SysParamsService parameters;
    private final ObjectMapper json;
    private final HttpClient http = HttpClient.newBuilder().connectTimeout(Duration.ofSeconds(2)).build();
    @Value("${device-integration.runtime-url:http://xiaozhi-esp32-server:8003/api/internal/device-control}")
    private String url;

    public String apply(Map<String, Object> command) {
        try {
            String secret = parameters.getValue(Constant.SERVER_SECRET, true);
            if (secret == null || secret.isBlank()) return "saved_runtime_unavailable";
            var request = HttpRequest.newBuilder(URI.create(url)).timeout(Duration.ofSeconds(12))
                    .header("Authorization", "Bearer " + secret).header("Content-Type", "application/json")
                    .POST(HttpRequest.BodyPublishers.ofString(json.writeValueAsString(command))).build();
            var response = http.send(request, HttpResponse.BodyHandlers.ofString());
            if (response.statusCode() == 404) return "saved_device_offline";
            if (response.statusCode() == 409) return "saved_pending_reconnect";
            if (response.statusCode() == 200 && json.readTree(response.body()).path("success").asBoolean()) {
                return "applied";
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        } catch (Exception e) {
            // The binding/clear has committed; never misreport it as a rolled-back operation.
        }
        return "saved_runtime_unavailable";
    }
}
