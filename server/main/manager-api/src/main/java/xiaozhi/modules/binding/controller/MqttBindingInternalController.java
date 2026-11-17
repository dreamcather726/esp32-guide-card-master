package xiaozhi.modules.binding.controller;

import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RestController;

import com.fasterxml.jackson.databind.JsonNode;
import jakarta.validation.Valid;
import lombok.RequiredArgsConstructor;
import xiaozhi.common.utils.Result;
import xiaozhi.modules.binding.dto.MqttBindingDTOs.Legacy;
import xiaozhi.modules.binding.dto.MqttBindingDTOs.View;
import xiaozhi.modules.binding.dto.UserBindingDTOs.Binding;
import xiaozhi.modules.binding.service.MqttBindingService;

@RestController
@RequestMapping("/config/user-binding")
@RequiredArgsConstructor
public class MqttBindingInternalController {
    private final MqttBindingService service;

    @PostMapping("/online")
    public Result<View> online(@RequestBody JsonNode report) {
        return new Result<View>().ok(service.online(report));
    }

    @PostMapping("/legacy")
    public Result<Binding> legacy(@Valid @RequestBody Legacy request) {
        return new Result<Binding>().ok(service.legacy(request));
    }
}
