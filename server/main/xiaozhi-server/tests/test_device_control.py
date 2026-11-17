import ast
import asyncio
import hmac
import json
from pathlib import Path
import re
from types import SimpleNamespace
import unittest
from unittest.mock import AsyncMock, Mock


class Response:
    def __init__(self, value, status=200): self.value, self.status = value, status


class DeviceControlTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        path = Path(__file__).resolve().parents[1] / 'core/api/device_control_handler.py'
        tree = ast.parse(path.read_text(encoding='utf-8'))
        tree.body = [n for n in tree.body if isinstance(n, ast.ClassDef)]
        self.lookup, self.config = AsyncMock(), AsyncMock()
        ns = dict(asyncio=asyncio, hmac=hmac, re=re, web=SimpleNamespace(json_response=Response),
                  get_connection=self.lookup, get_private_config_from_api=self.config)
        exec(compile(tree, str(path), 'exec'), ns)
        self.handler = ns['DeviceControlHandler']({'manager-api': {'secret': 'fixture-secret'}})
        self.mac = 'aa:bb:cc:dd:ee:ff'
        self.conn = SimpleNamespace(websocket=SimpleNamespace(close=AsyncMock()), components_ready=True,
            client_is_speaking=False, config={}, headers={}, user_guid='user', agent_id='agent',
            user_binding_required=True, clear_queues=Mock(), switch_role_voice_binding=AsyncMock(return_value=True))
        self.lookup.return_value = self.conn
        self.config.return_value = dict(agentId='agent', userGuid='user', deviceRoleId='role',
            deviceRoleBindingVersion='2', roleVoiceBindings=[{'id': 'role'}])

    async def request(self, action='apply_role', secret='fixture-secret', **kwargs):
        data = dict(action=action, deviceId=self.mac, agentId='agent', roleId='role', bindingVersion=2, **kwargs)
        return await self.handler.handle_post(SimpleNamespace(headers={'Authorization':'Bearer '+secret},
                                                               json=AsyncMock(return_value=data)))

    async def test_only_service_secret_is_accepted(self):
        result = await self.request(secret='customer-key')
        self.assertEqual(401, result.status); self.lookup.assert_not_awaited()

    async def test_offline_binding_is_not_claimed_as_applied(self):
        self.lookup.return_value = None
        self.assertEqual(404, (await self.request()).status)

    async def test_switch_uses_fresh_committed_configuration_and_correct_user(self):
        result = await self.request()
        self.assertEqual(200, result.status)
        self.conn.switch_role_voice_binding.assert_awaited_once_with('role')
        self.assertFalse(self.handler._locks)

    async def test_superseded_bindings_and_foreign_users_fail_closed(self):
        for key, value in [('deviceRoleBindingVersion',3),('agentId','foreign'),('userGuid','foreign')]:
            old = self.config.return_value[key]; self.config.return_value[key] = value
            self.assertEqual(409, (await self.request()).status)
            self.config.return_value[key] = old
        self.conn.switch_role_voice_binding.assert_not_awaited()

    async def test_busy_device_waits_until_reconnect_without_interrupting_speech(self):
        self.conn.client_is_speaking = True
        self.assertEqual(409, (await self.request()).status)
        self.conn.switch_role_voice_binding.assert_not_awaited()
        self.conn.websocket.close.assert_not_awaited()

    async def test_clear_closes_only_the_requested_live_context(self):
        self.assertEqual(200, (await self.request('clear_history')).status)
        self.conn.clear_queues.assert_called_once()
        self.conn.websocket.close.assert_awaited_once_with(code=1012, reason='device_history_cleared')
        self.config.assert_not_awaited()


class HistoryGenerationTests(unittest.TestCase):
    def test_report_epoch_is_an_immutable_snapshot_and_old_outbox_rows_remain_valid(self):
        import sys
        sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
        from core.utils.report_outbox import ReportIdentity, ReportIdentityError
        from core.handle.reportHandle import capture_report_identity
        conn = SimpleNamespace(user_binding_required=True, user_binding_ready=True, binding_version=1,
            device_id='mac', user_guid='user', session_id='connection', usage_session_id='usage',
            boot_id='boot', config={'deviceHistoryGeneration': 123})
        snapshot = capture_report_identity(conn)
        conn.config['deviceHistoryGeneration'] = 456
        self.assertEqual(123, snapshot.payload(1, 'test', 1000)['historyGeneration'])
        legacy = ReportIdentity('mac','user','connection','usage','boot',1)
        self.assertNotIn('historyGeneration', legacy.payload(1,'legacy',1000))
        for invalid in (-1, '123', True):
            with self.assertRaises(ReportIdentityError):
                ReportIdentity('mac','user','connection','usage','boot',1,history_generation=invalid)


if __name__ == '__main__': unittest.main()
