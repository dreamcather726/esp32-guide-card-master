// Isolated API fixtures; no real device mutations or MQTT publications.
async (page) => {
  const assert = (value, message) => { if (!value) throw new Error(message); };
  const mac = 'aa:bb:cc:dd:ee:ff';
  const otherMac = '11:22:33:44:55:66';
  const base = 'http://127.0.0.1:8011';
  const secret = 'auto-sync-test-secret';
  let cabGuid = null;
  let report = null;
  let fresh = false;
  let current = null;
  let holdNext = false;
  let release;
  const calls = [];
  const errors = [];
  const logs = [];
  page.on('pageerror', error => errors.push(error.message));
  page.on('console', message => logs.push(message.text()));
  await page.unrouteAll();
  await page.context().addInitScript(() => {
    if (location.origin !== 'http://127.0.0.1:8011') return;
    localStorage.setItem('token', JSON.stringify({ token: 'ui-auto-sync-test' }));
    localStorage.setItem('userInfo', JSON.stringify({ username: 'Auto-sync test', superAdmin: false }));
    localStorage.setItem('userLanguage', 'en');
  });
  await page.route('**/xiaozhi/**', async route => {
    const request = route.request();
    const path = decodeURIComponent(new URL(request.url()).pathname);
    const method = request.method();
    calls.push({ path, method, body: request.postDataJSON(), authorization: request.headers().authorization });
    let data = null;
    if (path.endsWith('/mqtt-binding')) {
      if (method === 'PUT') { cabGuid = request.postDataJSON().cabGuid; }
      else if (path.includes(otherMac)) data = {
        macAddress: otherMac, cabGuid: 'other-cabinet', receivedAt: new Date().toISOString(), fresh: true,
        report: { cmd: 'KJ', cabGuid: 'other-cabinet', bootID: 'other-boot', groupID: 'other-group',
          userGUID: 'other-user', usageSessionID: '', bindingVersion: 0, bindingState: 'unbound' }
      };
      else data = { macAddress: mac, cabGuid, report, receivedAt: report ? new Date().toISOString() : null, fresh };
      // Capture the previous device's response before allowing a MAC change.
      if (method === 'GET' && holdNext && !path.includes(otherMac)) {
        holdNext = false;
        await new Promise(resolve => { release = resolve; });
      }
    } else if (path.endsWith('/user-binding')) data = path.includes(otherMac) ? null : current;
    else if (path.endsWith('/pub-config')) data = {
      version: 'Auto-sync test', beianIcpNum: 'null', beianGaNum: 'null', systemWebMenu: { features: {} }
    };
    else if (!path.includes('/user/profile/')) data = [];
    await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ code: 0, data }) });
  });
  await page.goto('about:blank');
  await page.clock.install();
  await page.setViewportSize({ width: 1440, height: 1100 });
  await page.goto(`${base}/#/device-user-binding?macAddress=${encodeURIComponent(mac)}`);
  await page.getByText('No mapping', { exact: true }).waitFor();
  const settled = () => page.waitForFunction(() => {
    const vm = document.querySelector('.binding-page')?.__vue__;
    return vm && !vm.busy.mqtt && vm.pollTimer !== null;
  });
  const tick = async () => {
    await settled();
    const response = page.waitForResponse(r => r.url().endsWith('/mqtt-binding') && r.request().method() === 'GET');
    await page.clock.fastForward(5100);
    await response;
    await settled();
  };
  const puts = () => calls.filter(call => call.method === 'PUT' && call.path.endsWith('/mqtt-binding'));
  const gets = () => calls.filter(call => call.method === 'GET' && /\/(mqtt-binding|user-binding)$/.test(call.path));

  await page.getByRole('button', { name: 'Edit MAC / cabGuid mapping', exact: true }).click();
  const mappingInput = page.getByRole('textbox', { name: 'Mapped cabGuid', exact: true });
  await mappingInput.fill('bad/#');
  assert(await page.getByRole('button', { name: 'Save mapping', exact: true }).isDisabled(), 'Invalid cabinet blocks PUT');
  await mappingInput.fill('auto-cabinet');
  await page.getByRole('button', { name: 'Save mapping', exact: true }).click();
  const confirmation = page.getByRole('dialog', { name: 'Device User Binding', exact: true });
  assert((await confirmation.innerText()).includes(mac) && (await confirmation.innerText()).includes('auto-cabinet'), 'Confirmation includes both identities');
  assert(puts().length === 0, 'No PUT before explicit confirmation');
  await confirmation.getByRole('button', { name: 'Cancel', exact: true }).click();
  assert(puts().length === 0, 'Cancelled mapping sends no PUT');
  await page.getByRole('button', { name: 'Save mapping', exact: true }).click();
  await confirmation.getByRole('button', { name: 'Confirm', exact: true }).click();
  await page.getByText('Mapping saved', { exact: true }).waitFor();
  assert(puts().length === 1 && JSON.stringify(puts()[0].body) === JSON.stringify({ cabGuid: 'auto-cabinet' }), 'Owner PUT exact body');
  assert(await page.getByText('Device confirmation: Not confirmed', { exact: true }).isVisible(), 'Mapping save is not readiness');
  assert(await page.getByText('Dispatch unconfirmed; manual MQTT available', { exact: true }).isVisible(), 'No dispatch claim');
  await tick();
  assert(puts().length === 1, 'Polling never repeats mapping PUT');
  await page.reload();
  await page.getByText('Mapping saved', { exact: true }).waitFor();
  assert(await mappingInput.inputValue() === 'auto-cabinet', 'Mapping restored on a new visit');
  assert(puts().length === 1, 'Reload never repeats mapping PUT');

  report = { cmd: 'KJ', cabGuid, bootID: 'online-boot-1', groupID: 'online-group-1', userGUID: '',
    bindingVersion: 0, usageSessionID: '', bindingState: 'unbound', bindingToken: secret, command: { secret } };
  fresh = true;
  await tick();
  await page.getByText('Device confirmation: Received: unbound', { exact: true }).waitFor();
  assert(await page.getByRole('textbox', { name: 'bootID', exact: true }).inputValue() === 'online-boot-1', 'Automatic boot');
  assert(await page.getByRole('textbox', { name: 'Group ID', exact: true }).inputValue() === 'online-group-1', 'Automatic group');
  current = { bootID: report.bootID, userGUID: 'online-user-1', bindingVersion: 1, usageSessionID: 'online-session-1',
    profileVersion: 1, expiresAt: '2099-01-01T00:00:00Z' };
  report = { ...report, ...current, bindingState: 'pending' };
  await tick();
  await page.getByText('Device confirmation: Received: pending verification', { exact: true }).waitFor();
  assert(await page.getByRole('textbox', { name: 'userGUID', exact: true }).inputValue() === 'online-user-1', 'Automatic current user');
  assert((await page.locator('.record-fields').innerText()).includes('online-session-1'), 'Automatic server binding refresh');
  report = { ...report, bindingState: 'ready' };
  await tick();
  await page.getByText('Device confirmation: Verified ready in fresh report', { exact: true }).waitFor();
  assert(!(await page.locator('body').innerText()).includes(secret), 'Report allowlist in actual Vue rendering');
  assert(!(await page.evaluate(() => JSON.stringify(document.querySelector('.binding-page').__vue__.$data))).includes(secret), 'No report token in reactive state');
  await page.screenshot({ path: 'output/playwright/binding-auto-desktop.png', fullPage: true, animations: 'disabled' });

  await page.getByRole('textbox', { name: 'userGUID', exact: true }).focus();
  report = { ...report, userGUID: 'focused-editor-report-user' };
  await tick();
  assert(await page.getByRole('textbox', { name: 'userGUID', exact: true }).inputValue() === 'online-user-1', 'Focused user editor stays stable before typing');
  report = { ...report, ...current };
  await page.getByRole('textbox', { name: 'Nickname', exact: true }).fill('Unsaved operator profile');
  report = { ...report, userGUID: 'online-user-2', bootID: 'online-boot-2', groupID: 'online-group-2' };
  await tick();
  assert(await page.getByRole('textbox', { name: 'Reported current user', exact: true }).inputValue() === 'online-user-2', 'Latest report continues while editing');
  assert(await page.getByRole('textbox', { name: 'userGUID', exact: true }).inputValue() === 'online-user-1', 'Dirty profile retains user');
  assert(await page.getByRole('textbox', { name: 'Nickname', exact: true }).inputValue() === 'Unsaved operator profile', 'Dirty profile retained');
  await page.getByRole('button', { name: 'Use reported user', exact: true }).click();
  await confirmation.getByRole('button', { name: 'Cancel', exact: true }).click();
  assert(await page.getByRole('textbox', { name: 'Nickname', exact: true }).inputValue() === 'Unsaved operator profile', 'Cancel keeps draft');
  await page.getByRole('textbox', { name: 'userGUID', exact: true }).fill('manual-target');
  report = { ...report, userGUID: 'online-user-3' };
  await tick();
  assert(await page.getByRole('textbox', { name: 'userGUID', exact: true }).inputValue() === 'manual-target', 'Manual target survives polling');

  await page.getByRole('button', { name: 'Edit MAC / cabGuid mapping', exact: true }).click();
  await mappingInput.fill('unsaved-cabinet');
  await tick();
  assert(await mappingInput.inputValue() === 'unsaved-cabinet', 'Mapping edit survives polling');
  await page.locator('.mapping-row').getByRole('button', { name: 'Cancel', exact: true }).click();
  fresh = false;
  await tick();
  await page.getByText('Device confirmation: Report stale / unverified', { exact: true }).waitFor();
  assert(await page.getByRole('button', { name: /Rebind this boot/ }).isDisabled(), 'Stale auto report blocks binding');
  await page.getByRole('button', { name: /Import online report/ }).click();
  await page.getByRole('textbox', { name: 'MQTT Online Report', exact: true }).fill(JSON.stringify({
    cmd: 'KJ', cabGuid: 'legacy-cabinet', bootID: 'legacy-boot', groupID: 'legacy-group',
    bindingState: 'unbound', bindingVersion: 0, usageSessionID: '', userGUID: ''
  }));
  await page.getByRole('button', { name: /Import$/, exact: false }).click();
  await page.getByText('Imported snapshot (fallback)', { exact: true }).waitFor();
  await tick();
  assert(await page.getByRole('textbox', { name: 'bootID', exact: true }).inputValue() === 'legacy-boot', 'Imported report remains fallback');
  fresh = true;
  report = { ...report, ...current, bindingState: 'ready' };
  await tick();
  await page.getByText('Device confirmation: Verified ready in fresh report', { exact: true }).waitFor();

  await page.evaluate(() => {
    Object.defineProperty(document, 'hidden', { configurable: true, value: true });
    document.dispatchEvent(new Event('visibilitychange'));
  });
  const hiddenCalls = gets().length;
  await page.clock.fastForward(15000);
  assert(gets().length === hiddenCalls, 'No polling while hidden');
  assert(await page.getByText('Auto-sync paused', { exact: true }).isVisible(), 'Hidden status');
  await page.evaluate(() => {
    delete document.hidden;
    document.dispatchEvent(new Event('visibilitychange'));
  });
  await page.getByText('Device confirmation: Verified ready in fresh report', { exact: true }).waitFor();
  assert(gets().length > hiddenCalls, 'Immediate polling on visibility restore');

  for (const width of [320, 390, 768, 1024, 1440]) {
    await page.setViewportSize({ width, height: width < 700 ? 844 : 1100 });
    assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), `No overflow at ${width}`);
    assert(await page.locator('.mapping-row').evaluate(element => {
      const parent = element.getBoundingClientRect();
      return [...element.children].every(child => {
        const box = child.getBoundingClientRect();
        return box.left >= parent.left - 1 && box.right <= parent.right + 1;
      });
    }), `Mapping controls fit at ${width}`);
    assert(await page.locator('.record-section').evaluate(section => {
      const fields = section.querySelector('.record-fields').getBoundingClientRect();
      const actions = section.querySelector('.action-row').getBoundingClientRect();
      return fields.bottom <= actions.top;
    }), `Binding fields do not overlap actions at ${width}`);
    if (width === 390) await page.screenshot({ path: 'output/playwright/binding-auto-mobile.png', fullPage: true, animations: 'disabled' });
  }

  await settled();
  holdNext = true;
  const delayedRequest = page.waitForRequest(r => r.url().endsWith('/mqtt-binding'));
  await page.clock.fastForward(5100);
  await delayedRequest;
  await page.getByRole('textbox', { name: 'Device MAC', exact: true }).fill(otherMac);
  await page.waitForFunction(() => document.querySelector('input[aria-label="bootID"]').value === 'other-boot');
  release();
  await tick();
  assert(await page.getByRole('textbox', { name: 'bootID', exact: true }).inputValue() === 'other-boot', 'Late response cannot restore old MAC context');
  assert(await page.getByRole('textbox', { name: 'Mapped cabGuid', exact: true }).inputValue() === 'other-cabinet', 'New mapping remains selected');
  assert(puts().length === 1, 'Only operator-confirmed mapping was written');
  assert(calls.every(call => call.method === 'GET' || call.path.endsWith('/mqtt-binding')), 'No automatic bind/profile writes');
  assert(calls.filter(call => /\/(mqtt-binding|user-binding)$/.test(call.path)).every(call => call.authorization === 'Bearer ui-auto-sync-test'), 'Normal owner login header');
  assert(errors.length === 0, `Runtime errors: ${errors.join(', ')}`);
  assert(!logs.some(message => message.includes(secret)), 'No raw report credential logging');
  await page.getByRole('button', { name: 'Devices', exact: true }).click();
  const afterLeave = gets().length;
  await page.clock.fastForward(15000);
  assert(gets().length === afterLeave, 'Navigation destroys polling timers');
  return { passed: true, mappingWrites: puts().length, autoReadRequests: gets().length,
    runtimeErrors: errors.length, viewports: [320, 390, 768, 1024, 1440] };
}
