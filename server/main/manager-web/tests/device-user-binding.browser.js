// Isolated browser contract test. These fixtures are never imported by the application.
async (page) => {
  const assert = (condition, message) => { if (!condition) throw new Error(message); };
  const base = 'http://127.0.0.1:8011';
  const secret = 'browser-test-only-secret-'.padEnd(43, 'x');
  const bootID = '15450a34-45a6-4cce-9d05-a4a61650bda0';
  const userGUID = 'contract-test-user';
  let profile = null;
  let current = null;
  let version = 0;
  const sessions = [];
  const calls = [];
  const errors = [];
  const consoleMessages = [];
  page.on('pageerror', error => errors.push(error.message));
  page.on('console', message => consoleMessages.push(message.text()));
  await page.context().addInitScript(() => {
    if (location.origin !== 'http://127.0.0.1:8011') return;
    localStorage.setItem('token', JSON.stringify({ token: 'ui-contract-test' }));
    localStorage.setItem('userInfo', JSON.stringify({ username: 'UI contract test', superAdmin: false }));
    localStorage.setItem('userLanguage', 'en');
  });
  await page.unrouteAll();
  await page.context().grantPermissions(['clipboard-read', 'clipboard-write']);
  await page.route('**/xiaozhi/**', async route => {
    const request = route.request();
    const url = new URL(request.url());
    const method = request.method();
    let data = null;
    calls.push({ path: url.pathname, method, authorization: request.headers().authorization });
    if (url.pathname.endsWith('/mqtt-binding')) {
      data = { macAddress: 'aa:bb:cc:dd:ee:ff', cabGuid: null, report: null, receivedAt: null, fresh: false };
    } else if (url.pathname.endsWith('/user-binding')) {
      if (method === 'POST') {
        const input = request.postDataJSON();
        version++;
        current = { userGUID: input.userGUID, bootID: input.bootID, bindingVersion: version,
          usageSessionID: `test-session-${version}`, profileVersion: 1, expiresAt: '2099-01-01T00:00:00Z' };
        const command = { cmd: 'bindUser', cmdID: `test-bind-${version}`, ...current, bindingToken: secret };
        data = { ...current, bindingToken: secret, command };
        sessions.unshift({ id: current.usageSessionID, macAddress: 'aa:bb:cc:dd:ee:ff', userGuid: userGUID,
          bootId: bootID, bindingVersion: version, agentName: 'Contract test agent', createdAt: '2026-10-02T05:00:00Z', expiresAt: current.expiresAt });
      } else if (method === 'DELETE') {
        version++;
        data = { cmd: 'unbindUser', cmdID: `test-unbind-${version}`, bootID, bindingVersion: version, userGUID: '' };
        sessions[0].revokedAt = '2026-10-02T05:05:00Z';
        current = null;
      } else data = current;
    } else if (url.pathname.endsWith('/messages')) {
      data = [
        { id: '1', usageSessionId: sessions[0].id, chatType: 1, turnId: 'asr', content: 'Question', createdAt: '2026-10-02T05:00:00Z', deliveryStatus: 'received' },
        { id: '2', usageSessionId: sessions[0].id, chatType: 2, turnId: 'sentence', content: 'First fragment, ', createdAt: '2026-10-02T05:00:01Z', deliveryStatus: 'generated' },
        { id: '3', usageSessionId: sessions[0].id, chatType: 2, turnId: 'sentence', content: 'second fragment.', createdAt: '2026-10-02T05:00:01Z', deliveryStatus: 'generated', textTruncated: true }
      ];
    } else if (url.pathname.endsWith('/sessions')) data = sessions;
    else if (url.pathname.includes('/user/profile/')) {
      if (method === 'PUT') profile = { userGUID, profileVersion: 1, profile: request.postDataJSON() };
      data = profile;
    } else if (url.pathname.endsWith('/pub-config')) {
      data = { version: 'UI contract test', beianIcpNum: 'null', beianGaNum: 'null', systemWebMenu: { features: {} } };
    } else data = [];
    await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ code: 0, data }) });
  });
  await page.setViewportSize({ width: 1440, height: 1100 });
  await page.goto('about:blank');
  await page.goto(`${base}/#/device-user-binding`);
  await page.reload();
  await page.getByRole('textbox', { name: 'Device MAC', exact: true }).fill('aa:bb:cc:dd:ee:ff');
  await page.getByRole('textbox', { name: 'userGUID', exact: true }).fill(userGUID);
  await page.getByRole('button', { name: /Load$/, exact: false }).click();
  await page.getByText('No saved profile', { exact: true }).waitFor();
  await page.getByRole('textbox', { name: 'Nickname', exact: true }).fill('Contract test profile');
  await page.getByRole('textbox', { name: 'Language', exact: true }).fill('en-US');
  await page.getByRole('textbox', { name: 'Interests', exact: true }).fill('Architecture, history');
  await page.getByRole('textbox', { name: 'Explanation style', exact: true }).fill('Concise');
  await page.getByRole('button', { name: /Save profile/ }).click();
  await page.getByRole('button', { name: 'Confirm', exact: true }).click();
  await page.getByText('Profile saved', { exact: true }).first().waitFor();
  assert(profile.profile.nickname === 'Contract test profile', 'Profile PUT contract');

  const importReport = async ready => {
    await page.getByRole('button', { name: /Import online report/ }).click();
    await page.getByRole('textbox', { name: 'MQTT Online Report' }).fill(JSON.stringify({
      cmd: 'KJ', cabGuid: 'test-cabinet', bootID, bindingState: ready ? 'ready' : 'unbound',
      bindingVersion: ready ? current.bindingVersion : 0,
      usageSessionID: ready ? current.usageSessionID : '', userGUID: ready ? userGUID : ''
    }));
    await page.getByRole('button', { name: /Import$/, exact: false }).click();
    await page.getByRole('dialog', { name: 'MQTT Online Report', exact: true }).waitFor({ state: 'hidden' });
  };
  await importReport(false);
  await page.getByRole('button', { name: /Bind this boot/ }).click();
  await page.getByRole('button', { name: 'Confirm', exact: true }).click();
  await page.getByText('API saved', { exact: true }).waitFor();
  assert(await page.getByText('Device confirmation: Unbound in imported report', { exact: true }).isVisible(), 'API save is not ready');
  assert(!(await page.locator('.command-preview').innerText()).includes(secret), 'Default command redaction');
  await page.getByRole('button', { name: 'Reveal sensitive command' }).click();
  assert((await page.locator('.command-preview').innerText()).includes(secret), 'Explicit reveal');
  await page.getByRole('button', { name: 'Hide sensitive command' }).click();
  await page.mouse.move(0, 0);
  await page.waitForFunction(() => document.querySelectorAll('.el-message').length === 0);
  await page.screenshot({ path: 'output/playwright/binding-desktop.png', fullPage: true, animations: 'disabled' });
  await page.getByRole('button', { name: /Copy command once/ }).click();
  await page.getByText('Copied once; local command cleared', { exact: true }).waitFor();
  const copied = await page.evaluate(() => navigator.clipboard.readText());
  assert(JSON.parse(copied).bindingToken === secret, 'One-time copy content');
  assert(await page.locator('.command-preview').count() === 0, 'Command cleared after copy');
  assert(!(await page.evaluate(() => JSON.stringify({ local: { ...localStorage }, session: { ...sessionStorage } })) ).includes(secret), 'No browser storage token');

  await importReport(true);
  await page.getByText('Device confirmation: Ready in imported report', { exact: true }).waitFor();
  await page.setViewportSize({ width: 390, height: 844 });
  assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), 'Mobile overflow');
  await page.screenshot({ path: 'output/playwright/binding-mobile.png', fullPage: true, animations: 'disabled' });
  for (const width of [320, 768, 1024]) {
    await page.setViewportSize({ width, height: 900 });
    assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), `Overflow at ${width}`);
    if (width === 320) {
      await page.getByRole('button', { name: /Rebind this boot/ }).click();
      const confirmation = page.getByRole('dialog', { name: 'Device User Binding', exact: true });
      const box = await confirmation.boundingBox();
      assert(box.width <= 320, 'Mobile confirmation width');
      await confirmation.getByRole('button', { name: 'Cancel', exact: true }).click();
    }
  }
  await page.setViewportSize({ width: 1440, height: 1100 });
  await page.getByRole('button', { name: /Messages/ }).last().click();
  await page.getByText('First fragment, second fragment.', { exact: true }).waitFor();
  assert(await page.locator('.message-row').count() === 2, 'Adjacent streaming chunks grouped');
  assert(await page.getByText('Text truncated', { exact: true }).isVisible(), 'Truncation metadata');
  assert(!(await page.locator('.message-list').innerText()).includes('played'), 'No inferred playback');
  await page.screenshot({ path: 'output/playwright/binding-history.png', fullPage: true, animations: 'disabled' });
  await page.getByRole('dialog', { name: 'Messages', exact: true }).getByRole('button', { name: 'Close', exact: true }).click();

  await page.getByRole('button', { name: /Revoke binding/ }).click();
  await page.getByRole('button', { name: 'Confirm', exact: true }).click();
  await page.getByText('API revoked', { exact: true }).waitFor();
  const revokeCommand = JSON.parse(await page.locator('.command-preview').innerText());
  assert(revokeCommand.cmd === 'unbindUser' && revokeCommand.bindingVersion === 2, 'Reserved unbind version');
  assert(!('bindingToken' in revokeCommand), 'Unbind token excluded');
  await page.getByRole('button', { name: /Bind this boot/ }).click();
  await page.getByRole('button', { name: 'Confirm', exact: true }).click();
  await page.getByText('API saved', { exact: true }).waitFor();
  assert(current.bindingVersion === 3, 'Next POST version exceeds unbind');
  await page.getByRole('textbox', { name: 'userGUID', exact: true }).fill('next-user');
  assert(await page.locator('.command-preview').count() === 0, 'Identity change clears command');
  assert(await page.getByRole('textbox', { name: 'Nickname', exact: true }).inputValue() === '', 'Profile cleared on identity change');
  await page.route('**/user/profile/next-user', route => route.fulfill({
    status: 200, contentType: 'application/json', body: JSON.stringify({ code: 500, msg: secret, data: { bindingToken: secret } })
  }));
  await page.getByRole('button', { name: /Load$/, exact: false }).click();
  await page.getByText('API request rejected. Check account access and input values.', { exact: true }).waitFor();
  assert(!consoleMessages.some(message => message.includes(secret)), 'No raw sensitive error logging');
  assert(!(await page.locator('body').innerText()).includes(secret), 'No raw sensitive error rendering');
  assert(errors.length === 0, `Browser runtime errors: ${errors.join(', ')}`);
  assert(calls.filter(call => call.path.includes('/user/profile/') || call.path.includes('/user-binding')).every(call => call.authorization === 'Bearer ui-contract-test'), 'Normal login Authorization');
  return { passed: true, apiRequests: calls.length, runtimeErrors: errors.length, viewports: [320, 390, 768, 1024, 1440] };
}
