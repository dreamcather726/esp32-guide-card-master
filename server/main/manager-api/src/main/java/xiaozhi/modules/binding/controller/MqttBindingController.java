package xiaozhi.modules.binding.controller;

import org.apache.shiro.authz.annotation.RequiresPermissions;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PathVariable;
import org.springframework.web.bind.annotation.PutMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RestController;

import jakarta.validation.Valid;
import lombok.RequiredArgsConstructor;
import xiaozhi.common.utils.Result;
import xiaozhi.modules.binding.dto.MqttBindingDTOs.Pair;
import xiaozhi.modules.binding.dto.MqttBindingDTOs.View;
import xiaozhi.modules.binding.service.MqttBindingService;
import xiaozhi.modules.security.user.SecurityUser;

@RestController
@RequiredArgsConstructor
@RequiresPermissions("sys:role:normal")
public class MqttBindingController {
    private final MqttBindingService service;

    @PutMapping("/device/{macAddress}/mqtt-binding")
    public Result<View> pair(@PathVariable("macAddress") String mac, @Valid @RequestBody Pair request) {
        return new Result<View>().ok(service.pair(SecurityUser.getUserId(), mac, request));
    }

    @GetMapping("/device/{macAddress}/mqtt-binding")
    public Result<View> current(@PathVariable("macAddress") String mac) {
        return new Result<View>().ok(service.current(SecurityUser.getUserId(), mac));
    }
}
