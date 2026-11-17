package xiaozhi.modules.binding;

import static org.junit.jupiter.api.Assertions.*;
import static org.mockito.Mockito.*;

import java.sql.Connection;
import java.sql.DriverManager;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.Callable;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

import org.apache.ibatis.mapping.Environment;
import org.apache.ibatis.session.Configuration;
import org.apache.ibatis.session.SqlSessionFactoryBuilder;
import org.junit.jupiter.api.AfterAll;
import org.junit.jupiter.api.BeforeAll;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.TestInstance;
import org.junit.jupiter.api.condition.EnabledIfSystemProperty;
import org.mybatis.spring.SqlSessionTemplate;
import org.mybatis.spring.transaction.SpringManagedTransactionFactory;
import org.springframework.aop.framework.ProxyFactory;
import org.springframework.core.io.ClassPathResource;
import org.springframework.jdbc.core.JdbcTemplate;
import org.springframework.jdbc.datasource.DataSourceTransactionManager;
import org.springframework.jdbc.datasource.DriverManagerDataSource;
import org.springframework.jdbc.datasource.init.ScriptUtils;
import org.springframework.transaction.annotation.AnnotationTransactionAttributeSource;
import org.springframework.transaction.interceptor.TransactionInterceptor;
import org.yaml.snakeyaml.Yaml;

import com.fasterxml.jackson.databind.ObjectMapper;
import xiaozhi.common.constant.Constant;
import xiaozhi.common.exception.RenException;
import xiaozhi.modules.agent.entity.AgentEntity;
import xiaozhi.modules.agent.service.AgentService;
import xiaozhi.modules.binding.dao.MqttBindingDao;
import xiaozhi.modules.binding.dao.UserBindingDao;
import xiaozhi.modules.binding.dto.MqttBindingDTOs.*;
import xiaozhi.modules.binding.dto.UserBindingDTOs.Binding;
import xiaozhi.modules.binding.service.*;
import xiaozhi.modules.sys.service.SysParamsService;

/** Opt-in real MySQL fixture. Never connects to the application's configured database. */
@EnabledIfSystemProperty(named = "mqtt.test.mysql", matches = "true")
@TestInstance(TestInstance.Lifecycle.PER_CLASS)
class MqttBindingMysqlTest {
    private static final String MAC = "aa:bb:cc:dd:ee:ff";
    private final String schema = "mqtt_binding_it_" + UUID.randomUUID().toString().replace("-", "");
    private final ObjectMapper json = new ObjectMapper();
    private String adminUrl;
    private String user;
    private String password;
    private boolean created;
    private JdbcTemplate jdbc;
    private DataSourceTransactionManager transactions;
    private UserBindingDao devices;
    private MqttBindingDao dao;
    private SysParamsService params;
    private UserBindingService bindings;
    private MqttBindingService service;

    @BeforeAll
    void createFixture() throws Exception {
        // Local configuration is read only for credentials, never its database name or host.
        Map<?, ?> local;
        try (var stream = new ClassPathResource("application-dev.yml").getInputStream()) {
            Map<?, ?> root = new Yaml().load(stream);
            local = (Map<?, ?>) ((Map<?, ?>) ((Map<?, ?>) root.get("spring")).get("datasource")).get("druid");
        }
        adminUrl = System.getenv().getOrDefault("MQTT_TEST_MYSQL_URL",
                "jdbc:mysql://127.0.0.1:3306/?useUnicode=true&characterEncoding=UTF-8&serverTimezone=UTC");
        // Require an explicit server-only URL; an application schema is never a fixture target.
        if (!adminUrl.matches("jdbc:mysql://[^/]+/\\?.*")) {
            throw new IllegalArgumentException("MQTT_TEST_MYSQL_URL must contain an empty database path and query string");
        }
        user = System.getenv().getOrDefault("MQTT_TEST_MYSQL_USER", local.get("username").toString());
        password = System.getenv().getOrDefault("MQTT_TEST_MYSQL_PASSWORD", local.get("password").toString());
        try (Connection connection = DriverManager.getConnection(adminUrl, user, password)) {
            connection.createStatement().execute("CREATE DATABASE " + schema + " CHARACTER SET utf8mb4");
            created = true;
        }
        String url = adminUrl.replace("/?", "/" + schema + "?");
        var dataSource = new DriverManagerDataSource(url, user, password);
        jdbc = new JdbcTemplate(dataSource);
        jdbc.execute("CREATE TABLE ai_device (id VARCHAR(32) PRIMARY KEY, mac_address VARCHAR(50) UNIQUE, user_id BIGINT, agent_id VARCHAR(32)) ENGINE=InnoDB");
        jdbc.execute("CREATE TABLE ai_agent_chat_history (id BIGINT PRIMARY KEY, user_guid VARCHAR(128), created_at DATETIME(3)) ENGINE=InnoDB");
        try (Connection connection = dataSource.getConnection()) {
            ScriptUtils.executeSqlScript(connection, new ClassPathResource("db/changelog/202610021200.sql"));
            ScriptUtils.executeSqlScript(connection, new ClassPathResource("db/changelog/202610041200.sql"));
            ScriptUtils.executeSqlScript(connection, new ClassPathResource("db/changelog/202610101200.sql"));
        }
        Configuration configuration = new Configuration(new Environment("fixture", new SpringManagedTransactionFactory(), dataSource));
        configuration.setMapUnderscoreToCamelCase(true);
        configuration.addMapper(UserBindingDao.class);
        configuration.addMapper(MqttBindingDao.class);
        SqlSessionTemplate sql = new SqlSessionTemplate(new SqlSessionFactoryBuilder().build(configuration));
        devices = sql.getMapper(UserBindingDao.class);
        dao = sql.getMapper(MqttBindingDao.class);
        transactions = new DataSourceTransactionManager(dataSource);
        transactions.setDefaultTimeout(15);
    }

    @AfterAll
    void dropFixture() throws Exception {
        if (created && schema.matches("mqtt_binding_it_[0-9a-f]{32}")) {
            try (Connection connection = DriverManager.getConnection(adminUrl, user, password)) {
                connection.createStatement().execute("DROP DATABASE " + schema);
            }
        }
    }

    @BeforeEach
    void setup() {
        for (String table : List.of("ai_mqtt_binding_outbox", "ai_mqtt_binding_mapping", "ai_usage_session",
                "ai_device_binding_state", "ai_user_profile", "ai_device")) jdbc.execute("TRUNCATE TABLE " + table);
        jdbc.update("INSERT INTO ai_device VALUES (?,?,?,?)", "device-1", MAC, 10L, "agent-1");
        AgentService agents = mock(AgentService.class);
        AgentEntity agent = new AgentEntity();
        agent.setId("agent-1");
        agent.setUserId(10L);
        when(agents.selectById("agent-1")).thenReturn(agent);
        bindings = transactional(new UserBindingService(devices, agents, json));
        params = mock(SysParamsService.class);
        when(params.getValue(Constant.SERVER_SECRET, true)).thenReturn("fixture-only-stable-secret");
        restartService();
    }

    private void restartService() {
        service = transactional(new MqttBindingService(dao, devices, bindings, new MqttBindingCipher(params), json));
    }

    @SuppressWarnings("unchecked")
    private <T> T transactional(T target) {
        ProxyFactory proxy = new ProxyFactory(target);
        proxy.setProxyTargetClass(true);
        proxy.addAdvice(new TransactionInterceptor(transactions, new AnnotationTransactionAttributeSource()));
        return (T) proxy.getProxy();
    }

    private Legacy request(String id) {
        return new Legacy("cab-1", "boot-1", "user-1", id);
    }

    private void observe() {
        service.online(json.createObjectNode().put("cmd", "KJ").put("cabGuid", "cab-1").put("bootID", "boot-1")
                .put("bindingToken", "unsolicited-secret").put("Battery Level", "80"));
    }

    private void pairAndObserve() {
        service.pair(10L, MAC, new Pair("cab-1"));
        observe();
    }

    @Test
    void migrationsAndPersistenceRetainExactCommandAcrossRestartAndNeverPlaintext() {
        pairAndObserve();
        Binding first = service.legacy(request("r1"));
        restartService();
        assertEquals(first, service.legacy(request("r1")));
        assertEquals(first, service.legacy(request("r2")));
        assertEquals(1, jdbc.queryForObject("SELECT COUNT(*) FROM ai_usage_session", Integer.class));
        assertEquals(2, jdbc.queryForObject("SELECT COUNT(*) FROM ai_mqtt_binding_outbox", Integer.class));
        String encrypted = jdbc.queryForObject("SELECT encrypted_binding FROM ai_mqtt_binding_outbox WHERE request_id='r1'", String.class);
        assertTrue(encrypted.startsWith("v1."));
        assertFalse(encrypted.contains(first.bindingToken()));
        String hash = jdbc.queryForObject("SELECT token_hash FROM ai_usage_session", String.class);
        assertTrue(BindingTokens.matches(first.bindingToken(), hash));
        String report = jdbc.queryForObject("SELECT report_json FROM ai_mqtt_binding_mapping", String.class);
        assertFalse(report.contains("unsolicited-secret"));
        assertFalse(report.contains("bindingToken"));
        assertEquals(1, jdbc.queryForObject("SELECT COUNT(*) FROM information_schema.statistics WHERE table_schema=? AND table_name='ai_mqtt_binding_mapping' AND index_name='uk_mqtt_cab' AND non_unique=0", Integer.class, schema));
    }

    @Test
    void concurrentSameAndNewRequestIdsProduceOnlyOneSession() throws Exception {
        pairAndObserve();
        try (var pool = Executors.newFixedThreadPool(8)) {
            List<Callable<Binding>> calls = new ArrayList<>();
            for (int i = 0; i < 8; i++) calls.add(() -> service.legacy(request("same")));
            var results = pool.invokeAll(calls, 20, TimeUnit.SECONDS);
            Binding first = results.getFirst().get();
            for (var result : results) assertEquals(first, result.get());
            calls.clear();
            for (int i = 0; i < 8; i++) {
                String id = "alias-" + i;
                calls.add(() -> service.legacy(request(id)));
            }
            for (var result : pool.invokeAll(calls, 20, TimeUnit.SECONDS)) assertEquals(first, result.get());
        }
        assertEquals(1, jdbc.queryForObject("SELECT COUNT(*) FROM ai_usage_session", Integer.class));
        assertEquals(9, jdbc.queryForObject("SELECT COUNT(*) FROM ai_mqtt_binding_outbox", Integer.class));
        assertEquals(1L, jdbc.queryForObject("SELECT binding_version FROM ai_device_binding_state", Long.class));
    }

    @Test
    void encryptionFailureRollsBackSessionStateAndOutboxTogether() {
        pairAndObserve();
        when(params.getValue(Constant.SERVER_SECRET, true)).thenReturn("");
        assertThrows(IllegalStateException.class, () -> service.legacy(request("r1")));
        assertEquals(0, jdbc.queryForObject("SELECT COUNT(*) FROM ai_usage_session", Integer.class));
        assertEquals(0, jdbc.queryForObject("SELECT COUNT(*) FROM ai_mqtt_binding_outbox", Integer.class));
        assertEquals(0, jdbc.queryForObject("SELECT COUNT(*) FROM ai_device_binding_state", Integer.class));
        when(params.getValue(Constant.SERVER_SECRET, true)).thenReturn("fixture-only-stable-secret");
        assertEquals(1, service.legacy(request("r1")).bindingVersion());
    }

    @Test
    void staleBootRevocationSupersessionAndExpiredTombstonesRejectWithoutIssuance() {
        pairAndObserve();
        service.legacy(request("r1"));
        jdbc.update("UPDATE ai_mqtt_binding_mapping SET received_at=?",
                new java.sql.Timestamp(System.currentTimeMillis() - 181_000));
        assertThrows(RenException.class, () -> service.legacy(request("r1")));
        observe();
        assertThrows(RenException.class, () -> service.legacy(new Legacy("cab-1", "other-boot", "user-1", "r1")));
        assertThrows(RenException.class, () -> service.legacy(new Legacy("cab-1", "boot-1", "other-user", "r1")));
        bindings.revoke(10L, MAC);
        assertThrows(RenException.class, () -> service.legacy(request("r1")));
        service.legacy(request("r2"));
        service.legacy(new Legacy("cab-1", "boot-1", "user-2", "r3"));
        assertThrows(RenException.class, () -> service.legacy(request("r2")));
        jdbc.update("UPDATE ai_mqtt_binding_outbox SET expires_at=?",
                new java.sql.Timestamp(System.currentTimeMillis() - 1000));
        service.purgeExpiredCommands();
        assertEquals(0, jdbc.queryForObject("SELECT COUNT(*) FROM ai_mqtt_binding_outbox WHERE encrypted_binding IS NOT NULL", Integer.class));
        assertThrows(RenException.class, () -> service.legacy(new Legacy("cab-1", "boot-1", "user-2", "r3")));
        Binding recovered = service.legacy(new Legacy("cab-1", "boot-1", "user-2", "r4"));
        assertNotNull(recovered.command());
        assertThrows(RenException.class, () -> service.legacy(new Legacy("cab-1", "boot-1", "user-2", "r3")));
        assertEquals(4, jdbc.queryForObject("SELECT COUNT(*) FROM ai_usage_session", Integer.class));
        assertEquals(4, jdbc.queryForObject("SELECT COUNT(*) FROM ai_mqtt_binding_outbox", Integer.class));
    }

    @Test
    void reownershipAndRecreatedDeviceRejectOldMappingAndRequests() {
        pairAndObserve();
        service.legacy(request("r1"));
        jdbc.update("UPDATE ai_device SET user_id=20");
        assertThrows(RenException.class, this::observe);
        assertThrows(RenException.class, () -> service.legacy(request("r1")));
        assertNull(service.current(20L, MAC).cabGuid());
        jdbc.update("UPDATE ai_device SET user_id=10,id='replacement'");
        assertThrows(RenException.class, this::observe);
        service.pair(10L, MAC, new Pair("cab-1"));
        observe();
        assertThrows(RenException.class, () -> service.legacy(request("r1")));
        assertNotNull(service.legacy(request("r2")));
    }

    @Test
    void concurrentCabinetClaimDoesNotOverwriteWinningDevice() throws Exception {
        String other = "00:11:22:33:44:55";
        jdbc.update("INSERT INTO ai_device VALUES (?,?,?,?)", "device-2", other, 10L, "agent-1");
        try (var pool = Executors.newFixedThreadPool(2)) {
            List<Callable<Boolean>> calls = List.of(() -> claim(MAC), () -> claim(other));
            int successes = 0;
            for (var result : pool.invokeAll(calls, 20, TimeUnit.SECONDS)) if (result.get()) successes++;
            assertEquals(1, successes);
        }
        assertEquals(1, jdbc.queryForObject("SELECT COUNT(*) FROM ai_mqtt_binding_mapping", Integer.class));
        String winner = jdbc.queryForObject("SELECT mac_address FROM ai_mqtt_binding_mapping", String.class);
        assertEquals("cab-1", service.current(10L, winner).cabGuid());
        assertNull(service.current(10L, winner.equals(MAC) ? other : MAC).cabGuid());
    }

    private boolean claim(String mac) {
        try {
            service.pair(10L, mac, new Pair("cab-1"));
            return true;
        } catch (RenException | org.springframework.dao.DataAccessException expected) {
            return false;
        }
    }
}
