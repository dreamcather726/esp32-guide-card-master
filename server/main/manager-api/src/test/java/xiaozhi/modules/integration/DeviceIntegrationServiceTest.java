package xiaozhi.modules.integration;

import static org.junit.jupiter.api.Assertions.*;
import static org.mockito.Mockito.*;
import java.util.Date;
import java.util.List;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;
import xiaozhi.common.exception.RenException;
import xiaozhi.modules.agent.entity.AgentEntity;
import xiaozhi.modules.agent.entity.AgentRoleVoiceBindingEntity;
import xiaozhi.modules.agent.service.*;
import xiaozhi.modules.binding.dao.UserBindingDao;
import xiaozhi.modules.device.entity.DeviceEntity;
import xiaozhi.modules.integration.dao.DeviceIntegrationDao;
import xiaozhi.modules.integration.dto.DeviceIntegrationDTOs;
import xiaozhi.modules.integration.dto.DeviceIntegrationDTOs.*;
import xiaozhi.modules.integration.service.*;
import xiaozhi.modules.timbre.service.TimbreService;
import xiaozhi.modules.voiceclone.service.VoiceCloneService;

class DeviceIntegrationServiceTest {
    DeviceIntegrationDao dao;
    UserBindingDao locks;
    DeviceConversationService history;
    DeviceRoleService roles;
    AgentService agents;
    AgentRoleVoiceBindingService bindings;
    DeviceEntity device;
    AgentRoleVoiceBindingEntity role;

    @BeforeEach void setup() {
        dao=mock(DeviceIntegrationDao.class); locks=mock(UserBindingDao.class);
        history=new DeviceConversationService(dao, locks);
        agents=mock(AgentService.class); bindings=mock(AgentRoleVoiceBindingService.class);
        roles=new DeviceRoleService(agents, bindings, mock(TimbreService.class), mock(VoiceCloneService.class), history, dao);
        device=new DeviceEntity(); device.setId("device"); device.setMacAddress("aa:bb:cc:dd:ee:ff");
        device.setUserId(10L); device.setAgentId("agent");
        when(dao.devices(eq(10L), anyString(), anyString())).thenReturn(List.of(device));
        when(locks.lockDevice(device.getMacAddress())).thenReturn(device);
        var agent=new AgentEntity(); agent.setId("agent"); agent.setUserId(10L);
        when(agents.selectById("agent")).thenReturn(agent);
        role=new AgentRoleVoiceBindingEntity(); role.setId("role"); role.setAgentId("agent");
        role.setEnabled(true); role.setSystemPrompt("Guide");
        when(bindings.selectById("role")).thenReturn(role);
    }

    @Test void bindsOnlyTheOwnedDeviceAndKeepsSameSelectionIdempotent() {
        var saved=new RoleBinding("device",device.getMacAddress(),"agent","role",1L,new Date());
        when(dao.binding("device")).thenReturn(null,saved,saved);
        assertEquals(saved,roles.bind(10L,"device",new SelectRole("agent","role")));
        assertEquals(saved,roles.bind(10L,"device",new SelectRole("agent","role")));
        verify(dao,times(1)).saveBinding(device,"role");
        verify(locks,times(2)).lockDevice(device.getMacAddress());
    }

    @Test void deniesOtherOwnersAndStaleDeviceOwnership() {
        when(dao.devices(eq(20L),anyString(),anyString())).thenReturn(List.of());
        assertThrows(RenException.class,()->roles.bind(20L,"device",new SelectRole("agent","role")));
        device.setUserId(20L);
        assertThrows(RenException.class,()->roles.bind(10L,"device",new SelectRole("agent","role")));
        verify(dao,never()).saveBinding(any(),anyString());
    }

    @Test void deniesDisabledEmptyAndForeignRoles() {
        for(int mode=0;mode<3;mode++) {
            role.setEnabled(mode!=0); role.setAgentId(mode==1?"other":"agent");
            role.setSystemPrompt(mode==2?null:"Guide");
            assertThrows(RenException.class,()->roles.bind(10L,"device",new SelectRole("agent","role")));
        }
        verify(dao,never()).saveBinding(any(),anyString());
    }

    @Test void neverReassignsDeviceToAnotherAgent() {
        var agent=new AgentEntity(); agent.setId("another"); agent.setUserId(10L);
        when(agents.selectById("another")).thenReturn(agent);
        assertThrows(RenException.class,()->roles.bind(10L,"device",new SelectRole("another","role")));
        verify(dao,never()).saveBinding(any(),anyString());
    }

    @Test void ambiguousIdentifiersAreRejected() {
        when(dao.devices(eq(10L),anyString(),anyString())).thenReturn(List.of(device,device));
        assertThrows(RenException.class,()->history.history(10L,"device",1,30));
    }

    @Test void historyIsPagedWithoutExportingProfilesOrOtherDevices() {
        when(dao.history(10L,device.getMacAddress(),30,30L)).thenReturn(List.of());
        when(dao.historyCount(10L,device.getMacAddress())).thenReturn(44L);
        var page=history.history(10L,"device",2,30);
        assertEquals(44,page.total()); assertTrue(page.list().isEmpty());
        assertThrows(RenException.class,()->history.history(10L,"device",0,30));
        assertThrows(RenException.class,()->history.history(10L,"device",1,101));
    }

    @Test void clearRemovesContentAndRecordsOnlyContentFreeRetryGuards() {
        when(dao.deleteHistory(10L,device.getMacAddress())).thenReturn(3);
        when(dao.clearedAt(10L,device.getMacAddress())).thenReturn(new Date(1700000000000L));
        assertEquals(3,history.clear(10L,"device").deletedCount());
        var order=inOrder(locks,dao);
        order.verify(locks).lockDevice(device.getMacAddress());
        order.verify(dao).rememberDeletedIds(10L,device.getMacAddress());
        order.verify(dao).deleteExclusiveAudio(10L,device.getMacAddress());
        order.verify(dao).deleteHistory(10L,device.getMacAddress());
        order.verify(dao).saveClear(10L,device.getMacAddress());
    }

    @Test void lateReportsAndDeletedMessageRetriesNeverReappear() {
        String mac=device.getMacAddress();
        when(dao.clearedAt(10L,mac)).thenReturn(new Date(1700000000000L));
        assertTrue(history.discardedReport(10L,mac,1699999999L,"old"));
        assertTrue(history.discardedReport(10L,mac,1700000000000L,"boundary"));
        assertFalse(history.discardedReport(10L,mac,1700000000001L,"new"));
        when(dao.deletedMessage("deleted")).thenReturn(1);
        assertTrue(history.discardedReport(10L,mac,1800000000000L,"deleted"));
        assertThrows(RenException.class,()->history.discardedReport(10L,mac,null,"undated"));
        assertFalse(history.discardedReport(20L,mac,1699999999L,"other-owner"));
        assertTrue(history.discardedReport(10L,mac,1800000000000L,"late-generation",0L));
        assertFalse(history.discardedReport(10L,mac,1700000000001L,"fresh-generation",1700000000000L));
    }

    @ParameterizedTest @ValueSource(strings={"file:///etc/passwd","javascript:alert(1)","https://user:pass@example.com/x",
            "ftp://example.com/x","/image.png"," https://example.com/x","https://example.com/\nsecret"})
    void rejectsInvalidMediaUrls(String value) { assertThrows(RenException.class,()->DeviceIntegrationDTOs.mediaUrl(value)); }

    @Test void optionalUrlsCanBeCleared() {
        assertNull(DeviceIntegrationDTOs.mediaUrl(null)); assertNull(DeviceIntegrationDTOs.mediaUrl(""));
        assertEquals("https://example.com/image.png", DeviceIntegrationDTOs.mediaUrl("https://example.com/image.png"));
    }
}
