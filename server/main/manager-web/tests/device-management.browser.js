// Run with playwright-cli run-code --filename; all API data is isolated.
async (page) => {
  const assert = (value, message) => { if (!value) throw new Error(message); };
  const base = page.url().startsWith('https://robot.suji.work/')
    ? 'https://robot.suji.work' : 'http://127.0.0.1:18761';
  const context = await page.context().browser().newContext({ serviceWorkers: 'block' });
  page = await context.newPage();
  try {
    const rows = Array.from({ length: 25 }, (_, i) => ({
      id: `fixture-${i}`, board: 'jiuchuan-s3', appVersion: '2.1.10', autoUpdate: 1,
      macAddress: `aa:bb:cc:dd:ee:${String(i).padStart(2, '0')}`,
      createDate: `2026-10-${String(i + 1).padStart(2, '0')} 12:00:00`,
      alias: i === 2 ? 'MEI 51160' : `MEI 900${String(i).padStart(2, '0')}`,
      lastConnectedAtTimestamp: null
    }));
    const errors = [];
    const writes = [];
    await page.goto('about:blank');
    await page.unrouteAll();
    page.on('pageerror', error => errors.push(error.message));
    await page.context().addInitScript(({ base }) => {
      if (location.origin !== base) return;
      localStorage.setItem('token', JSON.stringify({ token: 'device-list-fixture-only' }));
      localStorage.setItem('userInfo', JSON.stringify({ username: 'Device list test', superAdmin: false }));
      localStorage.setItem('userLanguage', 'zh_CN');
    }, { base });
    await page.route('**/xiaozhi/**', async route => {
      const request = route.request();
      const path = new URL(request.url()).pathname;
      const statusQuery = path.endsWith('/device/bind/fixture-agent') &&
        request.method() === 'POST' && request.postData() === '{}';
      if (request.method() !== 'GET' && !statusQuery) writes.push(path);
      let data = [];
      if (path.endsWith('/device/bind/fixture-agent')) {
        data = request.method() === 'GET' ? rows : '{}';
      } else if (path.endsWith('/pub-config')) {
        data = { version: 'UI fixture', beianIcpNum: 'null', beianGaNum: 'null', systemWebMenu: { features: {} } };
      }
      await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ code: 0, data }) });
    });
    await page.setViewportSize({ width: 1440, height: 1000 });
    await page.goto(`${base}/#/device-management?agentId=fixture-agent`);
    const bodyRows = page.locator('.el-table__body-wrapper tbody tr');
    await bodyRows.first().waitFor();
    await page.locator('.el-loading-mask').waitFor({ state: 'hidden' });
    assert(await bodyRows.count() === 20, 'Default page has 20 rows');
    assert((await bodyRows.first().innerText()).includes('MEI 90024'), 'Newest device is first');
    assert(await page.locator('.page-size-select input').inputValue() === '20条/页', '20 per page selected');
    assert(await page.locator('.total-text').innerText() === '共25条记录', 'Full list count');
    await page.screenshot({ path: 'output/playwright/device-list-desktop.png', fullPage: true });

    await page.getByRole('button', { name: '下一页', exact: true }).click();
    assert(await bodyRows.count() === 5, 'Second page has remainder');
    const search = page.getByPlaceholder('搜索型号、MAC 或备注');
    await search.fill('  51160  ');
    await search.press('Enter');
    assert(await bodyRows.count() === 1, 'Remark search includes devices outside first page');
    assert((await bodyRows.first().innerText()).includes('MEI 51160'), 'Expected remark found');
    assert(await page.locator('.total-text').innerText() === '共1条记录', 'Filtered result count');
    assert(await page.getByRole('button', { name: '上一页', exact: true }).isDisabled(), 'Search resets to first page');
    await page.screenshot({ path: 'output/playwright/device-list-remark-search.png', fullPage: true });

    await search.hover();
    await page.locator('.search-input .el-input__clear').click();
    assert(await bodyRows.count() === 20, 'Clear restores list immediately');
    await search.fill('not-a-device');
    await page.getByRole('button', { name: '搜索', exact: true }).click();
    assert(await bodyRows.count() === 0, 'No matches yields empty table');
    assert(await page.locator('.total-text').innerText() === '共0条记录', 'Empty count is zero');
    assert(await page.getByRole('button', { name: '下一页', exact: true }).isDisabled(), 'Empty results cannot advance');
    await search.fill('vanex s3');
    await search.press('Enter');
    assert(await bodyRows.count() === 20, 'Visible model name is searchable');
    await search.fill('AA:BB:CC:DD:EE:02');
    await search.press('Enter');
    assert(await bodyRows.count() === 1, 'MAC search remains case-insensitive');

    for (const width of [1024, 390]) {
      await page.setViewportSize({ width, height: 900 });
      assert(await search.isVisible(), `Search remains available at ${width}`);
      const fit = await search.evaluate(el => el.scrollWidth <= el.clientWidth + 1);
      assert(fit, `Search text fits its field at ${width}`);
      await page.screenshot({ path: `output/playwright/device-list-${width}.png`, fullPage: true });
    }
    assert(errors.length === 0, `No page errors: ${errors.join(', ')}`);
    assert(writes.length === 0, `No device mutations: ${writes.join(', ')}`);
    return { result: 'DEVICE_LIST_BROWSER_OK', base, fixtureCount: rows.length,
      pageSize: 20, newestFirst: true, remarkSearch: true, modelAndMacSearch: true,
      paginationAndCounts: true, clearSearch: true, pageErrors: errors, mutations: writes };
  } finally {
    await context.close();
  }
}
