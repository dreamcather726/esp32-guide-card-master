const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const babel = require('@babel/core');
const compiler = require('vue-template-compiler');
const Vue = require('vue');

const root = path.resolve(__dirname, '..');
const file = compiler.parseComponent(fs.readFileSync(path.join(root, 'src/views/DeviceManagement.vue'), 'utf8'));
function page(rows = []) {
  const result = { exports: {} };
  const device = {
    getAgentBindDevices: (_agent, callback) => callback({ data: { code: 0, data: rows } }),
    getDeviceStatus: (_agent, callback) => callback({ data: { code: 0, data: '{}' } }),
    updateDeviceInfo: (_id, _payload, callback) => callback({ data: { code: 0 } })
  };
  const code = babel.transformSync(file.script.content, {
    configFile: false, babelrc: false, plugins: ['@babel/plugin-transform-modules-commonjs']
  }).code;
  new Function('require', 'module', 'exports', code)(name => name === '@/apis/api'
    ? { device, dict: { getDictDataByType: async () => ({ data: [] }) } } : {}, result, result.exports);
  const component = result.exports.default;
  const vm = new Vue({ ...component, beforeCreate() {
    this.$route = { query: { agentId: 'fixture-agent' } };
    this.$t = key => key;
    this.$message = { success() {}, warning() {}, error() {} };
  } });
  vm.fetchBindDevices('fixture-agent');
  return { vm, device };
}
const device = (id, day, alias = '') => ({
  id, board: 'jiuchuan-s3', macAddress: `aa:bb:cc:dd:ee:${id}`, alias,
  appVersion: '2.1.10', autoUpdate: 1, createDate: `2026-10-${day}T12:00:00`
});

test('template compiles; clear runs search and count uses filtered results', () => {
  assert.deepEqual(compiler.compile(file.template.content).errors, []);
  assert.match(file.template.content, /@clear="handleSearch"/);
  assert.match(file.template.content, /replace\('\{total\}', filteredDeviceList.length\)/);
});

test('default page contains 20 newest devices and latest entry comes first', () => {
  const rows = Array.from({ length: 25 }, (_, i) => device(String(i).padStart(2, '0'), String(i + 1).padStart(2, '0')));
  const { vm } = page(rows);
  assert.equal(vm.pageSize, 20);
  assert.equal(vm.pageCount, 2);
  assert.equal(vm.paginatedDeviceList.length, 20);
  assert.equal(vm.paginatedDeviceList[0].device_id, '24');
  vm.goNext();
  assert.equal(vm.paginatedDeviceList.length, 5);
  assert.equal(vm.paginatedDeviceList[0].device_id, '04');
  vm.$destroy();
});

test('search matches saved remark, partial numbers, MAC, board and display model', () => {
  const { vm } = page([device('01', '01', 'MEI 51160'), device('02', '02', null)]);
  for (const [query, ids] of [
    ['51160', ['01']], ['  mei 51160  ', ['01']], ['AA:BB:CC:DD:EE:02', ['02']],
    ['vanex s3', ['02', '01']], ['jiuchuan-s3', ['02', '01']], ['absent', []]
  ]) {
    vm.searchKeyword = query;
    vm.handleSearch();
    assert.deepEqual(vm.filteredDeviceList.map(row => row.device_id), ids);
  }
  vm.$destroy();
});

test('remark search uses saved text while editing and updates after successful save', () => {
  const { vm } = page([device('01', '01', 'MEI 51160')]);
  vm.searchKeyword = '51160';
  vm.handleSearch();
  const row = vm.deviceList[0];
  row.remark = 'MEI 90000';
  assert.equal(vm.filteredDeviceList.length, 1);
  vm.submitRemark(row);
  assert.equal(vm.filteredDeviceList.length, 0);
  vm.searchKeyword = '90000';
  vm.handleSearch();
  assert.equal(vm.filteredDeviceList.length, 1);
  vm.$destroy();
});

test('failed remark save retains original searchable text', () => {
  const { vm, device: api } = page([device('01', '01', 'MEI 51160')]);
  api.updateDeviceInfo = (_id, _payload, callback) => callback({ data: { code: 1, msg: 'failed' } });
  vm.deviceList[0].remark = 'unsaved';
  vm.submitRemark(vm.deviceList[0]);
  vm.searchKeyword = '51160';
  vm.handleSearch();
  assert.equal(vm.filteredDeviceList.length, 1);
  assert.equal(vm.deviceList[0].remark, 'MEI 51160');
  vm.$destroy();
});

test('search and clear reset page; no results cannot advance to nonexistent pages', () => {
  const { vm } = page([device('01', '01', 'MEI 51160')]);
  vm.currentPage = 4;
  vm.searchKeyword = 'missing';
  vm.handleSearch();
  assert.equal(vm.currentPage, 1);
  assert.equal(vm.pageCount, 1);
  vm.goNext();
  assert.equal(vm.currentPage, 1);
  vm.searchKeyword = '';
  vm.handleSearch();
  assert.equal(vm.filteredDeviceList.length, 1);
  vm.$destroy();
});

test('refresh after adding devices returns to first page with newest on top', () => {
  const rows = [device('01', '01')];
  const { vm } = page(rows);
  vm.currentPage = 3;
  rows.push(device('02', '02'));
  vm.fetchBindDevices('fixture-agent');
  assert.equal(vm.currentPage, 1);
  assert.equal(vm.paginatedDeviceList[0].device_id, '02');
  vm.$destroy();
});

test('missing dates sort last and equal dates have deterministic ordering', () => {
  const { vm } = page([{ ...device('00', '01'), createDate: 'invalid' }, device('02', '02'), device('01', '02')]);
  assert.deepEqual(vm.deviceList.map(row => row.device_id), ['01', '02', '00']);
  vm.$destroy();
});

test('page is clamped when matching count shrinks after an edit or removal', async () => {
  const { vm } = page(Array.from({ length: 21 }, (_, i) => device(String(i), '01', 'match')));
  vm.searchKeyword = 'match';
  vm.handleSearch();
  await Vue.nextTick();
  vm.goNext();
  assert.equal(vm.currentPage, 2);
  vm.deviceList[0]._originalRemark = 'other';
  await Vue.nextTick();
  assert.equal(vm.currentPage, 1);
  assert.equal(vm.paginatedDeviceList.length, 20);
  vm.handlePageSizeChange(10);
  assert.equal(vm.currentPage, 1);
  assert.equal(vm.pageCount, 2);
  vm.$destroy();
});
