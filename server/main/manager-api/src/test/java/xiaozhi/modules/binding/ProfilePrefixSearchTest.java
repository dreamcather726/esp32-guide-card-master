package xiaozhi.modules.binding;

import static org.junit.jupiter.api.Assertions.*;
import static org.mockito.Mockito.*;

import java.util.List;
import java.util.Map;

import org.apache.ibatis.session.Configuration;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.NullAndEmptySource;
import org.junit.jupiter.params.provider.ValueSource;

import com.fasterxml.jackson.databind.ObjectMapper;
import xiaozhi.common.exception.RenException;
import xiaozhi.modules.agent.service.AgentService;
import xiaozhi.modules.binding.dao.UserBindingDao;
import xiaozhi.modules.binding.entity.UserProfileEntity;
import xiaozhi.modules.binding.service.UserBindingService;

class ProfilePrefixSearchTest {
    private UserBindingDao dao;
    private UserBindingService service;

    @BeforeEach
    void setup() {
        dao = mock(UserBindingDao.class);
        service = new UserBindingService(dao, mock(AgentService.class), new ObjectMapper());
    }

    @Test
    void retainsDaoOrderAndReturnsAllSixProfileFields() {
        var exact = profile("user");
        var child = profile("user-1");
        when(dao.profilesByPrefix(10L, "user", "user%")).thenReturn(List.of(exact, child));
        var result = service.searchProfiles(10L, "user");
        assertEquals(List.of("user", "user-1"), result.stream().map(r -> r.userGuid()).toList());
        assertEquals(7L, result.getFirst().profileVersion());
        var fields = result.getFirst().profile();
        assertEquals("Alice", fields.nickname());
        assertEquals("zh-CN", fields.language());
        assertEquals("history", fields.interests());
        assertEquals("brief", fields.explanationStyle());
        assertEquals("self", fields.relationship());
        assertEquals(3, fields.usageCount());
        verify(dao).profilesByPrefix(10L, "user", "user%");
        verifyNoMoreInteractions(dao);
    }

    @Test
    void escapesLiteralWildcardAndEscapeCharacters() {
        String prefix = "user_!%\\";
        when(dao.profilesByPrefix(10L, prefix, "user!_!!!%\\%")).thenReturn(List.of());
        assertEquals(List.of(), service.searchProfiles(10L, prefix));
        verify(dao).profilesByPrefix(10L, prefix, "user!_!!!%\\%");
    }

    @Test
    void emptyResultAndMaximumLengthAreSupported() {
        String prefix = "g".repeat(128);
        when(dao.profilesByPrefix(10L, prefix, prefix + "%")).thenReturn(List.of());
        assertTrue(service.searchProfiles(10L, prefix).isEmpty());
    }

    @ParameterizedTest
    @NullAndEmptySource
    @ValueSource(strings = {" ", " user", "user ", "\t", "\n"})
    void rejectsEmptyOrPaddedSearchInsteadOfListingWholeAccount(String prefix) {
        assertThrows(RenException.class, () -> service.searchProfiles(10L, prefix));
        verifyNoInteractions(dao);
    }

    @Test
    void requiresOwnerAndEnforcesLengthBeforeQuery() {
        assertThrows(RenException.class, () -> service.searchProfiles(null, "user"));
        assertThrows(RenException.class, () -> service.searchProfiles(10L, "g".repeat(129)));
        verifyNoInteractions(dao);
    }

    @Test
    void sqlBindsOwnerAndLiteralPatternAndCapsAtThirty() {
        var configuration = new Configuration();
        configuration.addMapper(UserBindingDao.class);
        var statement = configuration.getMappedStatement(UserBindingDao.class.getName() + ".profilesByPrefix");
        var bound = statement.getBoundSql(Map.of("owner", 10L, "prefix", "user_", "pattern", "user!_%"));
        String sql = bound.getSql().replaceAll("\\s+", " ");
        assertEquals("SELECT * FROM ai_user_profile WHERE owner_id=? AND user_guid LIKE ? ESCAPE '!' "
                + "ORDER BY (user_guid=?) DESC, user_guid ASC LIMIT 30", sql);
        assertEquals(List.of("owner", "pattern", "prefix"), bound.getParameterMappings().stream()
                .map(p -> p.getProperty()).toList());
    }

    private UserProfileEntity profile(String guid) {
        var entity = new UserProfileEntity();
        entity.setOwnerId(10L);
        entity.setUserGuid(guid);
        entity.setProfileVersion(7L);
        entity.setNickname("Alice");
        entity.setLanguage("zh-CN");
        entity.setInterests("history");
        entity.setExplanationStyle("brief");
        entity.setRelationship("self");
        entity.setUsageCount(3);
        return entity;
    }
}
