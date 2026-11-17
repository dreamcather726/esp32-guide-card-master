const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const babel = require('@babel/core');
const compiler = require('vue-template-compiler');

const root = path.resolve(__dirname, '..');
const source = file => fs.readFileSync(path.join(root, file), 'utf8');
function moduleFrom(code, imports = {}, globals = {}) {
  const result = { exports: {} };
  const compiled = babel.transformSync(code, {
    configFile: false, babelrc: false, plugins: ['@babel/plugin-transform-modules-commonjs']
  }).code;
  new Function('require', 'module', 'exports', ...Object.keys(globals), compiled)(name => {
    assert.ok(name in imports, `Unexpected import: ${name}`);
    return imports[name];
  }, result, result.exports, ...Object.values(globals));
  return result.exports;
}
const utils = moduleFrom(source('src/utils/deviceUserBinding.js'));
const vueFile = compiler.parseComponent(source('src/views/DeviceUserBinding.vue'));
const token = 'test-only-opaque-token-'.padEnd(43, 'x');
const binding = (overrides = {}) => ({
  userGUID: 'test-user', bootID: 'test-boot', bindingVersion: 1,
  usageSessionID: 'test-session', bindingToken: token, profileVersion: 1,
  ...overrides
});
const report = (overrides = {}) => ({
  cmd: 'KJ', cabGuid: 'test-cabinet', bootID: 'test-boot', userGUID: '',
  bindingVersion: 0, bindingState: 'unbound', usageSessionID: '', ...overrides
});
const deferred = () => {
  let resolve;
  const promise = new Promise(complete => { resolve = complete; });
  return { promise, resolve };
};
function page(api = {}, globals = {}) {
  const component = moduleFrom(vueFile.script.content, {
    '@/apis/api': { deviceUserBinding: { getSessions: async () => [], ...api } },
    '@/utils/deviceUserBinding': utils,
    '@/components/HeaderBar.vue': {}, '@/components/VersionFooter.vue': {}
  }, { document: { hidden: false }, ...globals }).default;
  const vm = { $route: { query: {} }, $t: key => key,
    $confirm: async () => {}, $message: { success() {}, error() {} } };
  Object.assign(vm, component.data.call(vm));
  for (const [key, method] of Object.entries(component.methods)) vm[key] = method.bind(vm);
  for (const [key, getter] of Object.entries(component.computed)) Object.defineProperty(vm, key, { get: () => getter.call(vm) });
  vm.context = { macAddress: 'aa:bb:cc:dd:ee:ff', userGUID: 'test-user', cabGuid: 'test-cabinet', bootID: 'test-boot' };
  vm.report = report();
  vm.recordKnown = true;
  Object.defineProperty(vm, '$options', { value: component });
  return vm;
}

test('Vue template compiles and route participates in normal login guard', () => {
  assert.deepEqual(compiler.compile(vueFile.template.content).errors, []);
  let guard;
  class Router {
    constructor(options) { this.options = options; }
    beforeEach(callback) { guard = callback; }
    push() { return Promise.resolve(); }
  }
  const router = moduleFrom(source('src/router/index.js'), {
    vue: { use() {} }, 'vue-router': Router
  }).default;
  const route = router.options.routes.find(item => item.path === '/device-user-binding');
  assert.equal(route.meta.requiresAuth, true);
  const previousStorage = global.localStorage;
  global.localStorage = { getItem: () => null };
  let redirect;
  guard({ name: route.name, fullPath: route.path }, {}, next => { redirect = next; });
  assert.deepEqual(redirect, { name: 'login', query: { redirect: route.path } });
  global.localStorage = previousStorage;
});

test('profile accepts nullable bounded strings only, includes all replacement fields', () => {
  assert.deepEqual(utils.profilePayload({ nickname: 'Name' }), { nickname: 'Name', language: null, interests: null, explanationStyle: null });
  for (const [field, limit] of Object.entries(utils.PROFILE_LIMITS)) {
    assert.equal(utils.profilePayload({ [field]: 'a'.repeat(limit) })[field].length, limit);
    assert.throws(() => utils.profilePayload({ [field]: 'a'.repeat(limit + 1) }), /invalidProfile/);
    assert.throws(() => utils.profilePayload({ [field]: ['not a string'] }), /invalidProfile/);
  }
  assert.deepEqual(utils.readProfile(null).profile, utils.emptyProfile());
  assert.throws(() => utils.readProfile({ userGUID: 'other', profile: {} }, 'test-user'), /invalidResponse/);
});

test('real online report is parsed without retaining extra fields or deriving MAC', () => {
  const parsed = utils.readOnlineReport(JSON.stringify(report({ bindingToken: token, extra: 'ignored' })));
  assert.equal(parsed.macAddress, '');
  assert.equal(parsed.bootID, 'test-boot');
  assert.equal('bindingToken' in parsed, false);
  assert.throws(() => utils.readOnlineReport('{}'), /invalidReport/);
  assert.throws(() => utils.readOnlineReport(JSON.stringify(report({ cabGuid: '../#' }))), /invalidReport/);
  assert.throws(() => utils.readOnlineReport(JSON.stringify(report({ bindingVersion: 1.5 }))), /invalidReport/);
  assert.equal(utils.validIdentifier(' user '), false);
  assert.equal(utils.validIdentifier('a\nb'), false);
  assert.equal(utils.validMac('aa:bb:cc:dd:ee:ff'), true);
});

test('command whitelisting, safe integer versions and exact boot/user matching', () => {
  const command = utils.bindCommand(binding(), { userGUID: 'test-user', bootID: 'test-boot' });
  assert.match(command.cmdID, /^[\da-f]{8}-[\da-f]{4}-4[\da-f]{3}-[89ab][\da-f]{3}-[\da-f]{12}$/i);
  assert.equal(command.bindingToken, token);
  assert.equal('bindingToken' in utils.publicBinding(binding({ command })), false);
  assert.throws(() => utils.bindCommand(binding(), { userGUID: 'other', bootID: 'test-boot' }), /invalidCommand/);
  assert.throws(() => utils.publicBinding(binding({ bindingVersion: Number.MAX_SAFE_INTEGER + 1 })), /invalidResponse/);
  assert.throws(() => utils.bindCommand(binding({ command: { ...command, bootID: 'old' } }), { userGUID: 'test-user', bootID: 'test-boot' }), /invalidCommand/);
});

test('DELETE command consumes reserved server version and excludes tokens', () => {
  const result = utils.unbindCommand({ cmd: 'unbindUser', cmdID: 'new-id', userGUID: '', bootID: 'test-boot', bindingVersion: 2 }, binding());
  assert.equal(result.bindingVersion, 2);
  assert.equal('bindingToken' in result, false);
  assert.equal('usageSessionID' in result, false);
  assert.throws(() => utils.unbindCommand({ ...result, bindingVersion: 1 }, binding()), /invalidCommand/);
});

test('ready requires exact user/boot/version/session match from a real report', () => {
  const vm = page();
  vm.record = utils.publicBinding(binding());
  assert.equal(vm.readiness.key, 'unbound');
  vm.report = report({ ...vm.record, bindingState: 'ready' });
  assert.equal(vm.readiness.key, 'ready');
  vm.report.usageSessionID = 'old-session';
  assert.equal(vm.readiness.key, 'unconfirmed');
  vm.report = report({ ...vm.record, bindingState: 'pending' });
  assert.equal(vm.readiness.key, 'pending');
});

test('copy-once command is redacted by default, outside serializable state and cleared after copy', async () => {
  const vm = page();
  vm.setCommand(utils.bindCommand(binding(), vm.context));
  assert.equal(vm.commandPreview().includes(token), false);
  assert.equal(JSON.stringify(vm).includes(token), false);
  vm.commandRevealed = true;
  assert.equal(vm.commandPreview().includes(token), true);
  let copied;
  vm.writeClipboard = async text => { copied = text; };
  await vm.copyCommand();
  assert.equal(JSON.parse(copied).bindingToken, token);
  assert.equal(vm.hasCommand, false);
  assert.equal(vm.commandPreview(), '');
  assert.equal(vm.commandState, 'commandCopied');
});

test('clipboard failure retains command for retry; user/device/navigation clears it', async () => {
  const vm = page();
  vm.setCommand(utils.bindCommand(binding(), vm.context));
  vm.writeClipboard = async () => { throw new Error('denied'); };
  await vm.copyCommand();
  assert.equal(vm.hasCommand, true);
  for (const action of ['changeUser', 'changeDevice', 'invalidateRequests']) {
    vm.setCommand(utils.bindCommand(binding(), { userGUID: 'test-user', bootID: 'test-boot' }));
    vm[action]();
    assert.equal(vm.commandPreview(), '');
  }
});

test('late POST response after navigation never restores sensitive command', async () => {
  const response = deferred();
  let calls = 0;
  const vm = page({ bind: () => { calls++; return response.promise; } });
  const pending = vm.bindCurrentBoot();
  await Promise.resolve();
  await vm.bindCurrentBoot();
  assert.equal(calls, 1);
  vm.invalidateRequests();
  response.resolve(binding());
  await pending;
  assert.equal(vm.hasCommand, false);
  assert.equal(vm.record, null);
});

test('API saved is separate from ready and follows nullable DELETE response', async () => {
  const vm = page({ bind: async () => binding(), unbind: async () => null });
  await vm.bindCurrentBoot();
  assert.equal(vm.recordState, 'apiSaved');
  assert.equal(vm.readiness.key, 'unbound');
  assert.equal(vm.hasCommand, true);
  await vm.revokeBinding();
  assert.equal(vm.recordState, 'apiRevoked');
  assert.equal(vm.hasCommand, false);
  assert.equal(vm.commandState, 'serverOnlyRevoke');
});

test('server-issued unbind command is copied independently of revoked record', async () => {
  const vm = page({ unbind: async () => ({ cmd: 'unbindUser', cmdID: 'revoke-id', userGUID: '', bootID: 'test-boot', bindingVersion: 2 }) });
  vm.record = utils.publicBinding(binding());
  await vm.revokeBinding();
  assert.equal(vm.record, null);
  assert.equal(vm.hasCommand, true);
  assert.equal(JSON.parse(vm.commandPreview()).cmd, 'unbindUser');
});

test('network failure after POST requires record refresh before another mutation', async () => {
  const vm = page({ bind: async () => { throw { status: 0 }; } });
  await vm.bindCurrentBoot();
  assert.equal(vm.recordKnown, false);
  assert.equal(vm.canBind, false);
  assert.equal(vm.errors.binding, 'userBinding.networkError');
});

test('profile and history responses from the previous user are ignored', async () => {
  const response = deferred();
  const vm = page({ getProfile: () => response.promise, getSessions: () => response.promise });
  const pendingProfile = vm.loadProfile();
  const pendingSessions = vm.loadSessions();
  vm.context.userGUID = 'next-user';
  vm.changeUser();
  response.resolve({ userGUID: 'test-user', profile: { nickname: 'Previous user' } });
  await Promise.all([pendingProfile, pendingSessions]);
  assert.equal(vm.profile.nickname, '');
  assert.deepEqual(vm.sessions, []);
});

test('streamed assistant chunks merge only when adjacent and same session/turn; ASR stays independent', () => {
  const chunk = (id, content, overrides = {}) => ({ id, chatType: 2, usageSessionId: 's1', turnId: 't1', deliveryStatus: 'generated', content, ...overrides });
  const grouped = utils.groupMessageChunks([
    chunk('1', 'Hello '), chunk('2', 'world'), chunk('3', 'Question', { chatType: 1 }),
    chunk('4', 'Reply'), chunk('5', 'Other', { usageSessionId: 's2' }), chunk('6', 'New', { turnId: 't2' })
  ]);
  assert.equal(grouped.length, 5);
  assert.equal(grouped[0].content, 'Hello world');
  assert.equal(grouped[0].chunkCount, 2);
  assert.deepEqual(grouped[0].deliveryStatuses, ['generated']);
  assert.equal(grouped[1].chatType, 1);
  assert.equal(grouped[2].content, 'Reply');
});

test('API uses exact encoded scoped routes, no retries, and sanitizes errors', async () => {
  const calls = [];
  let failure = false;
  const api = moduleFrom(source('src/apis/module/deviceUserBinding.js'), {
    '../api': { getServiceUrl: () => '/xiaozhi' },
    '../httpRequest': { sendRequest() {
      const call = {};
      const chain = {};
      for (const key of ['url', 'method', 'data', 'success', 'fail', 'networkFail']) chain[key] = value => { call[key] = value; return chain; };
      chain.sensitive = () => { call.sensitive = true; return chain; };
      chain.send = () => { calls.push(call); failure ? call.networkFail({ status: 500, data: { code: 500, msg: token } }) : call.success({ data: { code: 0, data: [] } }); };
      return chain;
    } }
  }).default;
  await api.bind('aa:bb:cc:dd:ee:ff', 'boot', 'user');
  assert.equal(calls[0].url, '/xiaozhi/device/aa%3Abb%3Acc%3Add%3Aee%3Aff/user-binding');
  assert.deepEqual(calls[0].data, { bootID: 'boot', userGUID: 'user' });
  assert.equal(calls[0].sensitive, true);
  await api.getMessages('user/one', 'session/one', 2, 20);
  assert.equal(calls[1].url, '/xiaozhi/user/profile/user%2Fone/sessions/session%2Fone/messages');
  assert.deepEqual(calls[1].data, { page: 2, limit: 20 });
  await api.getMqttBinding('aa:bb:cc:dd:ee:ff');
  assert.equal(calls[2].method, 'GET');
  assert.equal(calls[2].url, '/xiaozhi/device/aa%3Abb%3Acc%3Add%3Aee%3Aff/mqtt-binding');
  await api.saveMqttBinding('aa:bb:cc:dd:ee:ff', 'test-cabinet');
  assert.equal(calls[3].method, 'PUT');
  assert.equal(calls[3].url, calls[2].url);
  assert.equal(calls[3].sensitive, true);
  assert.deepEqual(calls[3].data, { cabGuid: 'test-cabinet' });
  failure = true;
  await assert.rejects(api.unbind('aa:bb:cc:dd:ee:ff'), error => {
    assert.deepEqual(error, { status: 500, code: 500 });
    return true;
  });
  assert.equal(calls.length, 5);
});

test('sensitive transport suppresses raw errors and settles auth-expired requests', async () => {
  let response = { status: 500, data: { code: 500, bindingToken: token } };
  const logs = [];
  const commits = [];
  const redirects = [];
  class Fly {
    config = {};
    request() { return Promise.reject(response); }
  }
  const transport = moduleFrom(source('src/apis/httpRequest.js'), {
    'flyio/dist/npm/fly': Fly,
    '../store/index': { getters: { getToken: '{"token":"normal-login"}' }, commit: value => commits.push(value) },
    '../utils/constant': { PAGE: { LOGIN: '/login' } },
    '../utils/index': { goToPage: path => redirects.push(path), isNotNull: Boolean, showDanger() {}, showWarning() {} },
    '../i18n/index': { locale: 'en' }
  }, { console: { log: (...args) => logs.push(args) } }).default;
  const send = () => new Promise(resolve => transport.sendRequest().sensitive().url('/binding').fail(resolve).networkFail(resolve).send());
  await send();
  response = { status: 200, data: { code: 401 } };
  await send();
  response = { status: 401, data: {} };
  await send();
  assert.deepEqual(logs, []);
  assert.deepEqual(commits, ['clearAuth', 'clearAuth']);
  assert.deepEqual(redirects, ['/login', '/login']);
});

test('invalid success response remains API-saved but requires refresh before retry', async () => {
  const vm = page({ bind: async () => binding({ bindingToken: 'bad' }) });
  await vm.bindCurrentBoot();
  assert.equal(vm.recordState, 'apiSaved');
  assert.equal(vm.recordKnown, false);
  assert.equal(vm.hasCommand, false);
  assert.equal(vm.errors.binding, 'userBinding.savedNoCommand');
});

const mqtt = (overrides = {}) => ({ macAddress: 'aa:bb:cc:dd:ee:ff', cabGuid: 'test-cabinet',
  report: report({ groupID: 'group-7' }), receivedAt: '2026-10-04T08:00:00Z', fresh: true, ...overrides });

test('MQTT response is scoped to both identities and allowlists sanitized KJ fields', () => {
  const result = utils.readMqttBinding(mqtt({ report: report({ groupID: 'group-7', bindingToken: token, command: { token } }), token }), 'AA:BB:CC:DD:EE:FF');
  assert.equal(result.report.groupID, 'group-7');
  assert.equal(JSON.stringify(result).includes(token), false);
  assert.equal(result.report.macAddress, '');
  const invalid = [
    { macAddress: '11:22:33:44:55:66' }, { report: report({ cabGuid: 'other' }) },
    { report: report({ macAddress: '11:22:33:44:55:66' }) }, { fresh: 'true' },
    { report: null, fresh: true }, { receivedAt: 'not-a-date' }, { receivedAt: null },
    { report: report({ groupID: { token } }) }, { report: report({ userGUID: 'bad\nuser' }) }
  ];
  for (const patch of invalid) assert.throws(() => utils.readMqttBinding(mqtt(patch), 'aa:bb:cc:dd:ee:ff'));
  assert.equal(utils.readMqttBinding(mqtt({ cabGuid: null, report: null, receivedAt: null, fresh: false }), 'aa:bb:cc:dd:ee:ff').cabGuid, '');
});

test('mapping is saved once only after explicit confirmation, then re-read with GET', async () => {
  const calls = [];
  const vm = page({
    getMqttBinding: async () => { calls.push('GET'); return mqtt({ report: null, receivedAt: null, fresh: false }); },
    saveMqttBinding: async (...args) => { calls.push(args); return null; }
  });
  vm.mappingKnown = true;
  vm.editMapping();
  vm.mappingDraft = 'test-cabinet';
  vm.$confirm = async () => { throw new Error('cancel'); };
  await vm.saveMapping();
  assert.deepEqual(calls, []);
  assert.equal(vm.busy.mapping, false);
  const confirmation = deferred();
  vm.$confirm = () => confirmation.promise;
  const save = vm.saveMapping();
  assert.deepEqual(calls, []);
  assert.equal(vm.busy.mapping, true);
  await vm.saveMapping();
  confirmation.resolve();
  await save;
  assert.deepEqual(calls, [['aa:bb:cc:dd:ee:ff', 'test-cabinet'], 'GET']);
  assert.equal(vm.mqtt.cabGuid, 'test-cabinet');
  assert.equal(vm.mappingEditing, false);
  assert.equal(vm.readiness.key, 'unconfirmed');
  assert.equal(vm.hasCommand, false);
  await vm.saveMapping();
  await vm.refreshMqtt();
  assert.equal(calls.filter(Array.isArray).length, 1);
});

test('invalid mapping and failed/ambiguous PUT require validation and a fresh GET', async () => {
  let puts = 0;
  const vm = page({ saveMqttBinding: async () => { puts++; throw { status: 0 }; }, getMqttBinding: async () => mqtt() });
  vm.mappingKnown = true;
  for (const invalid of ['', 'bad/#', ' bad ', 'a'.repeat(129)]) {
    vm.mappingDraft = invalid;
    await vm.saveMapping();
  }
  assert.equal(puts, 0);
  vm.mappingDraft = 'test-cabinet';
  vm.setCommand(utils.bindCommand(binding(), vm.context));
  await vm.saveMapping();
  assert.equal(vm.mappingKnown, false);
  assert.equal(vm.hasCommand, false);
  await vm.saveMapping();
  assert.equal(puts, 1);
  await vm.refreshMqtt();
  assert.equal(vm.mappingKnown, true);
  assert.equal(vm.errors.mapping, '');
  assert.equal(vm.canSaveMapping, false);
});

test('mapping saved with failed follow-up GET recovers without repeating PUT', async () => {
  let fail = true;
  let writes = 0;
  const vm = page({ saveMqttBinding: async () => { writes++; }, getMqttBinding: async () => {
    if (fail) throw { status: 0 };
    return mqtt();
  } });
  vm.mappingKnown = true;
  vm.mappingDraft = 'test-cabinet';
  await vm.saveMapping();
  assert.equal(vm.errors.mapping, 'userBinding.mappingRefreshRequired');
  assert.equal(vm.mappingKnown, false);
  fail = false;
  await vm.refreshMqtt();
  assert.equal(writes, 1);
  assert.equal(vm.mappingKnown, true);
  assert.equal(vm.errors.mapping, '');
});

test('late mapping confirmation and save responses are ignored after navigation', async () => {
  for (const stage of ['confirmation', 'PUT']) {
    const pending = deferred();
    let puts = 0;
    const vm = page({ saveMqttBinding: () => { puts++; return pending.promise; }, getMqttBinding: async () => mqtt() });
    vm.mappingKnown = true;
    vm.mappingDraft = 'test-cabinet';
    if (stage === 'confirmation') vm.$confirm = () => pending.promise;
    const action = vm.saveMapping();
    await Promise.resolve();
    await Promise.resolve();
    vm.suspendPage();
    pending.resolve();
    await action;
    assert.equal(puts, stage === 'PUT' ? 1 : 0);
    assert.equal(vm.mqtt.cabGuid, '');
    assert.equal(vm.busy.mapping, false);
  }
});

test('fresh live report fills boot/group/current user while server record refresh is independent', async () => {
  let snapshot = mqtt({ report: report({ ...binding(), groupID: 'group-7', bindingState: 'pending' }) });
  const vm = page({ getMqttBinding: async () => snapshot, getBinding: async () => binding() });
  vm.context.userGUID = '';
  await vm.refreshMqtt();
  assert.equal(vm.context.bootID, 'test-boot');
  assert.equal(vm.context.cabGuid, 'test-cabinet');
  assert.equal(vm.report.groupID, 'group-7');
  assert.equal(vm.context.userGUID, 'test-user');
  assert.equal(vm.readiness.key, 'livePending');
  assert.equal(vm.hasCommand, false);
  await vm.refreshBinding(true);
  snapshot = mqtt({ report: report({ ...binding(), bindingState: 'ready' }) });
  await vm.refreshMqtt();
  assert.equal(vm.readiness.key, 'liveReady');
  for (const field of ['userGUID', 'bootID', 'usageSessionID', 'bindingVersion']) {
    snapshot = mqtt({ report: report({ ...binding(), bindingState: 'ready', [field]: field === 'bindingVersion' ? 2 : 'other' }) });
    await vm.refreshMqtt();
    assert.equal(vm.readiness.key, 'unconfirmed', field);
  }
});

test('received unbound and pending never imply verified ready, even without a matching record', async () => {
  for (const [state, expected] of [['unbound', 'liveUnbound'], ['pending', 'livePending'], ['error', 'liveError']]) {
    const vm = page({ getMqttBinding: async () => mqtt({ report: report({ bindingState: state }) }) });
    await vm.refreshMqtt();
    assert.equal(vm.record, null);
    assert.equal(vm.readiness.key, expected);
  }
});

test('stale/error live reports lose readiness, with imported legacy report as explicit fallback', async () => {
  let snapshot = mqtt({ report: report({ ...binding(), bindingState: 'ready' }) });
  const vm = page({ getMqttBinding: async () => snapshot });
  vm.record = utils.publicBinding(binding());
  await vm.refreshMqtt();
  assert.equal(vm.readiness.key, 'liveReady');
  snapshot = { ...snapshot, fresh: false };
  await vm.refreshMqtt();
  assert.equal(vm.readiness.key, 'reportStale');
  assert.equal(vm.canBind, false);
  vm.legacyReport = report({ bootID: 'legacy-boot' });
  await vm.refreshMqtt();
  assert.equal(vm.reportSource, 'imported');
  assert.equal(vm.context.bootID, 'legacy-boot');
  assert.equal(vm.readiness.key, 'unbound');
  snapshot = null;
  await vm.refreshMqtt();
  assert.equal(vm.reportSource, 'imported');
  assert.equal(vm.context.bootID, 'legacy-boot');
  assert.equal(vm.errors.mqtt, 'userBinding.invalidResponse');
});

test('manual user choice, focused editors and unsaved profiles survive changing live users', async () => {
  for (const flag of ['userEdited', 'userEditing', 'profileEditing', 'profileDirty', 'messagesDialog']) {
    const vm = page({ getMqttBinding: async () => mqtt({ report: report({ userGUID: 'new-user', bootID: 'new-boot' }) }) });
    vm[flag] = true;
    vm.profile.nickname = 'Keep this draft';
    await vm.refreshMqtt();
    assert.equal(vm.context.bootID, 'new-boot', flag);
    assert.equal(vm.report.userGUID, 'new-user', flag);
    assert.equal(vm.context.userGUID, 'test-user', flag);
    assert.equal(vm.profile.nickname, 'Keep this draft', flag);
  }
});

test('explicit use reported user confirms before discarding an unsaved profile', async () => {
  const vm = page();
  vm.report = report({ userGUID: 'reported-user' });
  vm.profile.nickname = 'Draft';
  vm.profileDirty = true;
  vm.userEdited = true;
  vm.$confirm = async () => { throw new Error('cancel'); };
  await vm.useReportedUser();
  assert.equal(vm.profile.nickname, 'Draft');
  vm.$confirm = async () => {};
  await vm.useReportedUser();
  assert.equal(vm.context.userGUID, 'reported-user');
  assert.equal(vm.userEdited, false);
  assert.equal(vm.profileDirty, false);
});

test('polling never overwrites a mapping draft or an open import; cancel applies latest report', async () => {
  const vm = page({ getMqttBinding: async () => mqtt({ report: report({ bootID: 'new-boot' }) }) });
  vm.editMapping();
  vm.mappingDraft = 'operator-draft';
  await vm.refreshMqtt();
  assert.equal(vm.mappingDraft, 'operator-draft');
  assert.equal(vm.context.bootID, 'test-boot');
  vm.cancelMapping();
  assert.equal(vm.context.bootID, 'new-boot');
  vm.openReport();
  vm.reportText = 'typing JSON';
  await vm.refreshMqtt();
  assert.equal(vm.reportText, 'typing JSON');
});

test('changed MAC rejects both late MQTT and binding responses including A-B-A selection', async () => {
  const pending = deferred();
  const vm = page({ getMqttBinding: () => pending.promise, getBinding: () => pending.promise });
  const mqttRequest = vm.refreshMqtt();
  const recordRequest = vm.refreshBinding(true);
  vm.context.macAddress = '11:22:33:44:55:66';
  vm.changeDevice();
  vm.context.macAddress = 'aa:bb:cc:dd:ee:ff';
  vm.changeDevice();
  pending.resolve(mqtt());
  await Promise.all([mqttRequest, recordRequest]);
  assert.equal(vm.report, null);
  assert.equal(vm.record, null);
  assert.equal(vm.mappingKnown, false);
  assert.equal(vm.context.bootID, '');
});

test('background GET started before a bind cannot replace its result or clear its command', async () => {
  const stale = deferred();
  const vm = page({ getBinding: () => stale.promise, bind: async () => binding() });
  const get = vm.refreshBinding(true);
  await vm.bindCurrentBoot();
  stale.resolve(null);
  await get;
  assert.equal(vm.record.usageSessionID, 'test-session');
  assert.equal(vm.recordState, 'apiSaved');
  assert.equal(vm.hasCommand, true);
});

test('live report started before a bind cannot change the boot or clear the command afterwards', async () => {
  const stale = deferred();
  const vm = page({ getMqttBinding: () => stale.promise, bind: async () => binding() });
  const get = vm.refreshMqtt();
  await vm.bindCurrentBoot();
  stale.resolve(mqtt({ report: report({ bootID: 'old-boot' }) }));
  await get;
  assert.equal(vm.context.bootID, 'test-boot');
  assert.equal(vm.hasCommand, true);
  assert.equal(vm.busy.mqtt, false);
});

test('unchanged background binding preserves copy command; changed versions clear it; errors revoke readiness', async () => {
  let current = binding();
  const vm = page({ getBinding: async () => current });
  vm.record = utils.publicBinding(current);
  vm.recordState = 'apiSaved';
  vm.setCommand(utils.bindCommand(current, vm.context));
  await vm.refreshBinding(true);
  assert.equal(vm.hasCommand, true);
  assert.equal(vm.recordState, 'apiSaved');
  current = binding({ bindingVersion: 2 });
  await vm.refreshBinding(true);
  assert.equal(vm.hasCommand, false);
  current = {};
  await vm.refreshBinding(true);
  assert.equal(vm.recordKnown, false);
  assert.equal(vm.canBind, false);
});

test('poller uses a single 5s timer, pauses while hidden, resumes immediately and cleans up on destroy', async () => {
  const timers = new Map();
  const listeners = new Map();
  let nextTimer = 0;
  let requests = 0;
  const target = { addEventListener: (name, fn) => listeners.set(name, fn), removeEventListener: name => listeners.delete(name) };
  const document = { hidden: false, ...target };
  const vm = page({ getMqttBinding: async () => { requests++; return mqtt(); }, getBinding: async () => null }, {
    document, window: target,
    setTimeout: (callback, delay) => { assert.equal(delay, 5000); timers.set(++nextTimer, callback); return nextTimer; },
    clearTimeout: id => timers.delete(id)
  });
  const settle = async () => { for (let i = 0; i < 8; i++) await Promise.resolve(); };
  vm.$options.mounted.call(vm);
  await settle();
  assert.equal(requests, 1);
  assert.equal(timers.size, 1);
  vm.startPolling();
  await settle();
  assert.equal(requests, 1);
  const callback = timers.values().next().value;
  timers.clear();
  await callback();
  assert.equal(requests, 2);
  assert.equal(timers.size, 1);
  document.hidden = true;
  listeners.get('visibilitychange')();
  assert.equal(timers.size, 0);
  assert.equal(vm.pollingActive, false);
  assert.equal(vm.mqtt.fresh, false);
  document.hidden = false;
  listeners.get('visibilitychange')();
  await settle();
  assert.equal(requests, 3);
  vm.$options.beforeDestroy.call(vm);
  assert.equal(timers.size, 0);
  assert.equal(listeners.size, 0);
  assert.equal(vm.pollingEnabled, false);
});

test('destroy during in-flight polling prevents state restoration and rescheduling', async () => {
  const pending = deferred();
  let timers = 0;
  const vm = page({ getMqttBinding: () => pending.promise, getBinding: () => pending.promise }, {
    window: { removeEventListener() {} }, document: { hidden: false, removeEventListener() {} },
    setTimeout: () => { timers++; }, clearTimeout() {}
  });
  vm.pollingEnabled = true;
  vm.pollingActive = true;
  vm.report = null;
  const polling = vm.poll();
  vm.$options.beforeDestroy.call(vm);
  pending.resolve(mqtt());
  await polling;
  assert.equal(vm.report, null);
  assert.equal(vm.record, null);
  assert.equal(timers, 0);
});

test('auto-sync translations have matching English and Chinese keys', () => {
  const messages = moduleFrom(source('src/i18n/deviceUserBinding.js'));
  assert.deepEqual(Object.keys(messages.en).sort(), Object.keys(messages.zhCN).sort());
});
