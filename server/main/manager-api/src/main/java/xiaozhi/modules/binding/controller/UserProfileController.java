package xiaozhi.modules.binding.controller;

import java.util.List;

import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PathVariable;
import org.springframework.web.bind.annotation.PutMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RequestParam;
import org.springframework.web.bind.annotation.RestController;

import jakarta.servlet.http.HttpServletRequest;
import jakarta.validation.Valid;
import lombok.RequiredArgsConstructor;
import xiaozhi.common.utils.Result;
import xiaozhi.modules.binding.dto.UserBindingDTOs.Profile;
import xiaozhi.modules.binding.dto.UserBindingDTOs.ProfileView;
import xiaozhi.modules.binding.service.UserBindingService;
import xiaozhi.modules.security.oauth2.ProfileApiKeyFilter;

@RestController
@RequiredArgsConstructor
public class UserProfileController {
    private final UserBindingService service;

    @GetMapping("/user/profile/{userGUID}")
    public Result<?> profile(@PathVariable("userGUID") String guid,
            @RequestParam(value = "match", defaultValue = "exact") String match, HttpServletRequest request) {
        Long owner = ProfileApiKeyFilter.owner(request);
        return switch (match) {
            case "exact" -> new Result<ProfileView>().ok(service.getProfile(owner, guid));
            case "prefix" -> new Result<List<ProfileView>>().ok(service.searchProfiles(owner, guid));
            default -> new Result<Void>().error(400, "match must be exact or prefix");
        };
    }

    @PutMapping("/user/profile/{userGUID}")
    public Result<ProfileView> profile(@PathVariable("userGUID") String guid,
            @Valid @RequestBody Profile profile,
            @RequestParam(value = "match", defaultValue = "exact") String match, HttpServletRequest request) {
        if (!"exact".equals(match)) {
            return new Result<ProfileView>().error(400, "PUT requires an exact userGUID; omit match");
        }
        return new Result<ProfileView>().ok(service.upsertProfile(ProfileApiKeyFilter.owner(request), guid, profile));
    }
}
