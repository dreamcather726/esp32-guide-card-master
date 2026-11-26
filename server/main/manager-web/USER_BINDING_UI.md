# Device user binding

The authenticated Vue route is `/#/device-user-binding`. Device Management provides a page-level entry and a per-device entry carrying the device MAC and agent ID. No admin bypass or new framework is added.

## Workflow

1. Enter the registered device MAC, or open its entry in Device Management. The page reads its saved MQTT mapping and server user binding. Use the mapping edit button to enter the actual cabGuid. Saving opens a confirmation containing **both MAC and cabGuid**; only confirmation sends the owner-scoped PUT. A saved mapping is reused on future visits, never re-saved by polling. Neither cabGuid nor IMEI is used to derive a MAC. Importing a report does not silently create a mapping.
2. While visible, the page reads `/mqtt-binding` and `/user-binding` immediately and approximately every five seconds after the previous read cycle completes. It shows the received timestamp, freshness, current bootID, groupID, reported userGUID and server binding independently. The target userGUID follows the reported user until the operator edits it. Unsaved profiles, focused user/profile editors, mapping drafts, open report imports and active mutations are protected. Profile content is never fetched or saved by polling. The "Use reported user" action explicitly resumes following, with confirmation before discarding a dirty profile.
3. Load/save the four profile strings. Limits match Java: nickname 64, language 32, interests 1024, explanationStyle 512. Empty fields are sent as null. Profile changes apply to the next binding, not an existing session snapshot.
4. Bind the current boot when appropriate. Every POST rotates the credential and creates a usage session. Mapping saved, server binding saved, report received and device verified ready are distinct states. An unbound/pending report is shown as received, not verified. Only a **fresh** `ready` report with an exact userGUID/bootID/bindingVersion/usageSessionID match against a successfully read server binding is verified ready. The server's `fresh` flag is authoritative; the UI does not invent a freshness timeout. Stale/error/hidden live snapshots lose verified status.
5. A saved mapping does not prove MQTT dispatch. This frontend neither connects to the broker nor stores broker credentials, and this API contract does not expose bridge deployment/dispatch health. The status therefore makes no promise based on credential configuration or deployment preparation. Manual command copying remains available independently of backend bridge operation.
6. Manual fallback: import an actual `topic/{imei}/online` JSON report (`cmd: KJ`). The report does not normally contain a MAC. An imported report is retained as a fallback when no fresh server report is available, with an explicitly historical "imported snapshot" label. Fresh server reports take precedence again when received. Only matching imported `ready` reports display "ready in imported report", never live verified readiness. An ACK OK alone is not ready.
7. Copy a generated command once and publish it through the existing MQTT infrastructure to the displayed `topic/{imei}/cmd` topic. Copying does not indicate dispatch or acceptance. Tokens are hidden initially and the in-memory command is removed after copying, on identity/profile changes, when boot/topic or server binding changes, and on navigation. An unchanged automatic server refresh preserves an outstanding manual command.
8. Revoke uses DELETE. Its optional direct command response carries the atomically reserved unbind version. The client never allocates a version. Null means server-only revoke/no active command. A command for another boot is not offered for dispatch.

Polling pauses on hidden/pagehide and resumes immediately on visibility/pageshow. Route changes and destruction cancel the timer, remove listeners, and invalidate in-flight reads. Per-request sequences plus the selected MAC reject late results, including an A-to-B-to-A device change. Background reads started before a bind/revoke cannot overwrite that mutation. Missing endpoints, denied access, failed reads and invalid responses are shown without raw response details; the imported/manual workflow remains available.

## API Contract

All paths use `VUE_APP_API_BASE_URL` (normally `/xiaozhi`) and the existing login bearer token. Responses use the existing `{code,msg,data}` envelope.

- GET/PUT `/user/profile/{userGUID}`: nested `{userGUID,profileVersion,profile}`; missing GET returns null. PUT replaces all four fields.
- GET/POST/DELETE `/device/{macAddress}/user-binding`: POST sends only `{bootID,userGUID}`. GET is token-free. POST includes the one-time token and a bindUser command. DELETE returns a direct unbindUser command or null.
- GET `/device/{macAddress}/mqtt-binding`: `{macAddress,cabGuid,report,receivedAt,fresh}`. No mapping accepts a null/empty cabGuid, null report/timestamp and `fresh:false`. `report` is a sanitized KJ object or null, `receivedAt` is an ISO datetime or null, and `fresh` is boolean. The client checks both device identities and keeps only cabGuid, bootID, optional macAddress/groupID, bindingState, bindingVersion, userGUID and usageSessionID from the report.
- PUT `/device/{macAddress}/mqtt-binding`: exactly `{cabGuid}` after operator confirmation. The owner authorization remains server-enforced. The UI re-reads GET rather than depending on the PUT response shape; no automatic retries or profile/binding writes are made. An ambiguous save requires a successful GET before another change.
- GET `/user/profile/{userGUID}/sessions` and `/user/profile/{userGUID}/sessions/{usageSessionID}/messages`: `page` and `limit`, array results without totals. Next is enabled only when a full page was returned.

History preserves response order and groups only adjacent assistant records with the same session and turn ID. User ASR rows remain separate. Delivery status is displayed as recorded, including generated and sent_unconfirmed; playback is not inferred. Grouping is page-local and reports the number of fragments on that page. Truncation flags are preserved.

Sensitive requests opt out of raw transport logging, set a no-store request header and never automatically retry mutations. The command is held in a WeakMap rather than reactive/Vuex state or browser storage. Clipboard contents are outside the page's lifetime; operators should treat copied commands as credentials. A failed or ambiguous mutation requires a server refresh before retrying. Server authorization remains authoritative.

## Verification

- Runtime inspected: `C:\Program Files\nodejs\node.exe` v24.11.1, npm 11.6.2; existing local `node_modules` reused.
- `npm.cmd run test:binding`: 35 focused validation, mapping confirmation/recovery, freshness, visibility/timer cleanup, edit protection, race, auth, command lifecycle, API contract and streaming grouping tests.
- `npm run build`: production compilation. Existing large asset and Workbox precache warnings are unrelated to this page.
- Browser workflow: start `npm.cmd run serve -- --host 127.0.0.1 --port 8011` with `.env.example` values supplied in the environment. No environment/config file edits are required. Use a fresh isolated browser session per suite. CLI commands (via the existing npx runtime):

```powershell
npx.cmd --yes --package @playwright/cli playwright-cli -s=binding-manual open about:blank
npx.cmd --yes --package @playwright/cli playwright-cli -s=binding-manual run-code --filename tests/device-user-binding.browser.js
npx.cmd --yes --package @playwright/cli playwright-cli -s=binding-auto open about:blank
npx.cmd --yes --package @playwright/cli playwright-cli -s=binding-auto run-code --filename tests/device-user-binding-auto.browser.js
```

The first browser suite checks the existing manual profile/bind/copy/revoke/history workflow. The second checks one-time mapping confirmation, automatic reads, unbound/pending/verified states, stale-report fallback, dirty/focused form protection, visibility events, late MAC responses and navigation cleanup. It uses the browser clock to advance polling timers and dispatches visibility events with a test-only hidden state. Both intercept backend requests with isolated fixtures; neither uses real accounts nor publishes MQTT commands. Widths 320, 390, 768, 1024 and 1440 are checked. Screenshots: `output/playwright/binding-{desktop,mobile,history}.png` and `binding-auto-{desktop,mobile}.png`.

No migrations, device commands, deployment or real account writes are performed by these checks. Live Java/database/MQTT integration requires the corresponding backend and firmware changes to be provisioned separately.
