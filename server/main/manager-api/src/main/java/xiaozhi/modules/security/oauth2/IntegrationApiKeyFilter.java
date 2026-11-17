package xiaozhi.modules.security.oauth2;

import java.io.IOException;
import java.util.Collections;
import java.util.regex.Pattern;

import org.apache.shiro.SecurityUtils;
import org.apache.shiro.authz.UnauthorizedException;
import jakarta.servlet.*;
import jakarta.servlet.http.*;
import xiaozhi.common.utils.JsonUtils;
import xiaozhi.common.utils.Result;
import xiaozhi.modules.binding.service.ProfileApiKeyService;
import xiaozhi.modules.security.user.SecurityUser;

public class IntegrationApiKeyFilter extends Oauth2Filter {
    private static final String IDENTITY = IntegrationApiKeyFilter.class.getName();
    private static final Pattern ROLE = Pattern.compile("/integration/agents/[A-Za-z0-9_-]{1,32}/roles");
    private static final Pattern MEDIA = Pattern.compile("/integration/agents/[A-Za-z0-9_-]{1,32}/roles/[A-Za-z0-9_-]{1,64}/media");
    private static final Pattern DEVICE_ROLE = Pattern.compile("/integration/devices/[A-Za-z0-9_:\\-]{1,128}/role-binding");
    private static final Pattern HISTORY = Pattern.compile("/integration/devices/[A-Za-z0-9_:\\-]{1,128}/chat-history");
    private final ProfileApiKeyService service;
    private record Identity(Long owner, String uri, String method) {}

    public IntegrationApiKeyFilter(ProfileApiKeyService service) { this.service = service; }

    @Override
    protected boolean isAccessAllowed(ServletRequest request, ServletResponse response, Object mapped) {
        request.removeAttribute(IDENTITY);
        ((HttpServletResponse) response).setHeader("Cache-Control", "no-store");
        ((HttpServletResponse) response).addHeader("Vary", "Authorization, X-API-Key");
        return super.isAccessAllowed(request, response, mapped);
    }

    @Override
    protected boolean onAccessDenied(ServletRequest request, ServletResponse response) throws Exception {
        var http = (HttpServletRequest) request;
        var keys = Collections.list(http.getHeaders("X-API-Key"));
        if (keys.isEmpty()) return super.onAccessDenied(request, response);
        if (keys.size() != 1 || http.getHeader("Authorization") != null) {
            return reject(response, 400, "Use exactly one authentication credential");
        }
        String scope = scope(http);
        if (scope == null) return reject(response, 403, "Operation not permitted by integration API");
        Long owner;
        try { owner = service.authenticate(keys.getFirst(), scope); }
        catch (RuntimeException e) { return reject(response, 503, "Authentication temporarily unavailable"); }
        if (owner == null) return reject(response, 401, "Invalid API key or operation outside key scope");
        http.setAttribute(IDENTITY, new Identity(owner, http.getRequestURI(), http.getMethod()));
        return true;
    }

    public static Long owner(HttpServletRequest request) {
        if (request.getAttribute(IDENTITY) instanceof Identity identity && scope(request) != null
                && identity.uri().equals(request.getRequestURI()) && identity.method().equals(request.getMethod())) {
            return identity.owner();
        }
        if (request.getHeader("X-API-Key") != null) throw new UnauthorizedException("Integration authentication required");
        SecurityUtils.getSubject().checkPermission("sys:role:normal");
        return SecurityUser.getUserId();
    }

    static String scope(HttpServletRequest request) {
        String uri = request.getRequestURI();
        if (!uri.startsWith(request.getContextPath())) return null;
        String path = uri.substring(request.getContextPath().length()), method = request.getMethod();
        if ("GET".equals(method) && ROLE.matcher(path).matches()) return "roles:read";
        if ("PUT".equals(method) && MEDIA.matcher(path).matches()) return "roles:write";
        if (DEVICE_ROLE.matcher(path).matches() && ("GET".equals(method) || "PUT".equals(method))) return "device:role";
        if (HISTORY.matcher(path).matches()) {
            if ("GET".equals(method)) return "history:read";
            if ("DELETE".equals(method)) return "history:delete";
        }
        return null;
    }

    private boolean reject(ServletResponse response, int code, String message) throws IOException {
        var http = (HttpServletResponse) response;
        http.setStatus(code);
        http.setContentType("application/json;charset=utf-8");
        http.getWriter().print(JsonUtils.toJsonString(new Result<Void>().error(code, message)));
        return false;
    }
}
