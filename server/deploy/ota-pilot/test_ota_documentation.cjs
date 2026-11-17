const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const root = path.resolve(process.argv[2]);
const collection = JSON.parse(fs.readFileSync(path.join(root, 'ota-api.postman_collection.json'), 'utf8'));
assert.equal(collection.item.length, 5);
assert.equal(collection.auth.type, 'noauth');
const definitions = new Map(collection.variable.map(v => [v.key, v.value]));
for (const item of collection.item) {
  assert.equal(item.request.auth.type, 'noauth');
  for (const event of item.event || []) new vm.Script(event.script.exec.join('\n'));
  for (const match of JSON.stringify(item.request).matchAll(/\{\{([^}]+)\}\}/g)) {
    assert(definitions.has(match[1]), 'Undefined variable: ' + match[1]);
  }
}
for (const key of ['deviceMac', 'currentVersion', 'otaDownloadUrl', 'otaETag', 'otaSha256']) {
  assert.equal(definitions.get(key), '', 'Exported runtime value: ' + key);
}
const url = 'https://robot.suji.work/xiaozhi/otaMag/download/11111111-1111-1111-1111-111111111111';
const digest = 'a'.repeat(64);
const etag = '"' + digest + '"';
const total = 3236640;
const placeholder = 'http://xiaozhi.server.com:8002/xiaozhi/otaMag/download/NOT_ACTIVATED_FIRMWARE_THIS_IS_A_INVALID_URL';
let count = 0;

function context() {
  const values = new Map(definitions);
  values.set('deviceMac', '02:fa:00:00:00:01');
  values.set('currentVersion', '2.1.12-diag');
  const variables = {get: key => values.get(key), set: (key, value) => values.set(key, value)};
  const pm = {
    variables, collectionVariables: variables,
    test: (_name, fn) => fn(),
    expect: value => ({not: {to: {eql: expected => assert.notEqual(value, expected)}}})
  };
  return {values, pm};
}

function run(ctx, index, listen, body = {}, status = 200, headers = {}) {
  ctx.pm.response = {
    code: status, json: () => body,
    headers: {get: name => headers[name]},
    to: {have: {status: expected => assert.equal(status, expected)}}
  };
  for (const event of collection.item[index].event || []) {
    if (event.listen === listen) vm.runInNewContext(event.script.exec.join('\n'), {pm: ctx.pm}, {timeout: 1000});
  }
}

function response(firmware = {version: '2.1.13-diag', url}) {
  return {server_time: {timestamp: 1}, websocket: {url: 'wss://fixture.invalid'}, firmware};
}
function test(name, fn) {
  fn();
  count++;
  console.log('PASS', name);
}
function checkedContext() {
  const ctx = context();
  run(ctx, 1, 'prerequest');
  run(ctx, 1, 'test', response());
  return ctx;
}
function firstChunk(ctx) {
  run(ctx, 2, 'prerequest');
  run(ctx, 2, 'test', {}, 206, {'Content-Range': 'bytes 0-65535/' + total,
    'Content-Length': '65536', ETag: etag, 'X-Firmware-SHA256': digest});
}

test('registered older device gets update URL', () => {
  const ctx = checkedContext();
  assert.equal(ctx.values.get('otaDownloadUrl'), url);
  assert.equal(ctx.values.get('otaStatus'), 'update_available');
});
for (const mac of ['', '864479086531093s3', '864479086531093', 'not-a-mac']) {
  test('invalid MAC blocked: ' + (mac || 'empty'), () => {
    const ctx = context();
    ctx.values.set('deviceMac', mac);
    ctx.values.set('otaDownloadUrl', url);
    assert.throws(() => run(ctx, 1, 'prerequest'));
    assert.equal(ctx.values.get('otaDownloadUrl'), '');
  });
}
test('missing installed version blocked', () => {
  const ctx = context();
  ctx.values.set('currentVersion', '');
  assert.throws(() => run(ctx, 1, 'prerequest'));
});
test('activation response does not expose update URL', () => {
  const ctx = context();
  run(ctx, 1, 'prerequest');
  run(ctx, 1, 'test', {...response(), activation: {code: '000000'}});
  assert.equal(ctx.values.get('otaStatus'), 'activation_required');
  assert.equal(ctx.values.get('otaDownloadUrl'), '');
});
test('missing firmware is not an upgrade', () => {
  const ctx = context();
  run(ctx, 1, 'prerequest');
  run(ctx, 1, 'test', {websocket: {url: 'wss://fixture.invalid'}});
  assert.equal(ctx.values.get('otaStatus'), 'no_firmware_check_auto_update_and_board');
});
test('no-update placeholder clears previous ticket', () => {
  const ctx = checkedContext();
  run(ctx, 1, 'prerequest');
  run(ctx, 1, 'test', response({version: '2.1.13-diag', url: placeholder}));
  assert.equal(ctx.values.get('otaDownloadUrl'), '');
  assert.equal(ctx.values.get('otaStatus'), 'no_update_url');
});
for (const body of [{error: 'Invalid device ID'}, {code: 500, msg: 'server error'}, {}, []]) {
  test('error/envelope response rejected: ' + JSON.stringify(body), () => {
    const ctx = checkedContext();
    run(ctx, 1, 'prerequest');
    assert.throws(() => run(ctx, 1, 'test', body));
    assert.equal(ctx.values.get('otaDownloadUrl'), '');
  });
}
for (const version of ['2.1.13', '2.1.13-diag', '2.1.14']) {
  test('no same-version install or downgrade: ' + version, () => {
    const ctx = context();
    ctx.values.set('currentVersion', version);
    run(ctx, 1, 'prerequest');
    assert.throws(() => run(ctx, 1, 'test', response()));
  });
}
test('untrusted download origin rejected', () => {
  const ctx = context();
  run(ctx, 1, 'prerequest');
  assert.throws(() => run(ctx, 1, 'test', response({version: '2.1.13-diag', url: 'https://example.invalid/file.bin'})));
});
test('range saves validated identity', () => {
  const ctx = checkedContext();
  firstChunk(ctx);
  assert.equal(ctx.values.get('otaETag'), etag);
  assert.equal(ctx.values.get('otaResumeOffset'), '65536');
});
test('invalid first range leaves no stale ETag', () => {
  const ctx = checkedContext();
  firstChunk(ctx);
  run(ctx, 2, 'prerequest');
  assert.throws(() => run(ctx, 2, 'test', {}, 206, {'Content-Range': 'bytes 1-65536/' + total}));
  assert.equal(ctx.values.get('otaETag'), '');
});
test('resume identity and range match', () => {
  const ctx = checkedContext();
  firstChunk(ctx);
  run(ctx, 3, 'prerequest');
  run(ctx, 3, 'test', {}, 206, {'Content-Range': 'bytes 65536-' + (total - 1) + '/' + total,
    'Content-Length': String(total - 65536), ETag: etag, 'X-Firmware-SHA256': digest});
});
test('full 200 response is never accepted as resumed chunk', () => {
  const ctx = checkedContext();
  firstChunk(ctx);
  assert.throws(() => run(ctx, 3, 'test', {}, 200));
});
test('resume 416 is rejected', () => {
  const ctx = checkedContext();
  firstChunk(ctx);
  assert.throws(() => run(ctx, 3, 'test', {}, 416));
});
test('full binary response accepted', () => {
  const ctx = checkedContext();
  run(ctx, 4, 'prerequest');
  run(ctx, 4, 'test', {}, 200, {'Content-Type': 'application/octet-stream', 'X-Firmware-SHA256': digest});
});
test('JSON error is never accepted as application binary', () => {
  const ctx = checkedContext();
  assert.throws(() => run(ctx, 4, 'test', {}, 200, {'Content-Type': 'application/json'}));
});
console.log('POSTMAN_OFFLINE_OK', count, 'behavior checks; no network or device changes');
