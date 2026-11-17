package xiaozhi.modules.security.oauth2;

import java.io.IOException;
import java.util.Collections;
import java.util.regex.Pattern;

import org.apache.shiro.SecurityUtils;
import org.apache.shiro.authz.UnauthorizedException;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import jakarta.servlet.ServletRequest;
import jakarta.servlet.ServletResponse;
import jakarta.servlet.http.HttpServletRequest;
import jakarta.servlet.http.HttpServletResponse;
import xiaozhi.common.utils.JsonUtils;
import xiaozhi.common.utils.Result;
import xiaozhi.modules.binding.service.ProfileApiKeyService;
import xiaozhi.modules.security.user.SecurityUser;

public class ProfileApiKeyFilter extends Oauth2Filter {
    private static final Logger LOG = LoggerFactory.getLogger(ProfileApiKeyFilter.class);
    private static final String IDENTITY = ProfileApiKeyFilter.class.getName() + ".identity";
    private static final Pattern PATH = Pattern.compile("/user/profile/[A-Za-z0-9_-]{1,128}");
    private final ProfileApiKeyService service;

    private record Identity(Long owner, String uri, String method) {}

    public ProfileApiKeyFilter(ProfileApiKeyService service) {
        this.service = service;
    }

    @Override
    protected boolean isAccessAllowed(ServletRequest request, ServletResponse response, Object mappedValue) {
        request.removeAttribute(IDENTITY);
        var httpResponse = (HttpServletResponse) response;
        httpResponse.setHeader("Cache-Control", "no-store");
        httpResponse.addHeader("Vary", "Authorization, X-API-Key");
        return super.isAccessAllowed(request, response, mappedValue);
    }

    @Override
    protected boolean onAccessDenied(ServletRequest request, ServletResponse response) throws Exception {
        var http = (HttpServletRequest) request;
        var headers = Collections.list(http.getHeaders("X-API-Key"));
        if (headers.isEmpty()) {
            return super.onAccessDenied(request, response);
        }
        if (headers.size() != 1 || http.getHeader("Authorization") != null) {
            return reject(response, 400, "Use exactly one authentication credential");
        }
        if (!scoped(http)) {
            return reject(response, 403, "API key permits profile GET/PUT only");
        }
        Long owner;
        try {
            owner = service.authenticate(headers.getFirst());
        } catch (RuntimeException e) {
            LOG.warn("Profile API key verification temporarily unavailable");
            return reject(response, 503, "Authentication temporarily unavailable");
        }
        if (owner == null) {
            return reject(response, 401, "Invalid or inactive profile API key");
        }
        // Request-only identity: never log in a Shiro subject with broad account permissions.
        http.setAttribute(IDENTITY, new Identity(owner, http.getRequestURI(), http.getMethod()));
        return true;
    }

    public static Long owner(HttpServletRequest request) {
        if (request.getAttribute(IDENTITY) instanceof Identity identity && scoped(request)
                && identity.uri().equals(request.getRequestURI()) && identity.method().equals(request.getMethod())) {
            return identity.owner();
        }
        if (request.getHeader("X-API-Key") != null) {
            throw new UnauthorizedException("Profile API key authentication required");
        }
        SecurityUtils.getSubject().checkPermission("sys:role:normal");
        return SecurityUser.getUserId();
    }

    private static boolean scoped(HttpServletRequest request) {
        String uri = request.getRequestURI();
        String context = request.getContextPath();
        return ("GET".equals(request.getMethod()) || "PUT".equals(request.getMethod()))
                && uri.startsWith(context) && PATH.matcher(uri.substring(context.length())).matches();
    }

    private boolean reject(ServletResponse response, int code, String message) throws IOException {
        var http = (HttpServletResponse) response;
        http.setStatus(code);
        http.setContentType("application/json;charset=utf-8");
        http.getWriter().print(JsonUtils.toJsonString(new Result<Void>().error(code, message)));
        return false;
    }
}
