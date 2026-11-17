<template>
  <div class="binding-page">
    <HeaderBar />
    <main class="binding-main">
      <div class="page-heading">
        <div class="heading-title">
          <el-tooltip :content="t('back')" placement="bottom">
            <el-button icon="el-icon-back" size="small" :aria-label="t('back')" @click="backToDevices" />
          </el-tooltip>
          <h1>{{ t('title') }}</h1>
        </div>
        <el-tag type="info" size="small">{{ t('noAutoDispatch') }}</el-tag>
      </div>

      <section class="context-section" :aria-label="t('context')">
        <div class="section-heading">
          <h2>{{ t('context') }}</h2>
          <el-button size="small" icon="el-icon-upload2" :disabled="busy.binding || busy.mapping" @click="openReport">{{ t('importReport') }}</el-button>
        </div>
        <el-form label-position="top" size="small" @submit.native.prevent>
          <div class="context-grid">
            <el-form-item :label="t('macAddress')" :error="context.macAddress && !macValid ? t('invalidMac') : ''">
              <el-input v-model="context.macAddress" id="binding-mac" :label="t('macAddress')" maxlength="17"
                :disabled="busy.binding || busy.mapping" autocomplete="off" @input="changeDevice" />
            </el-form-item>
            <el-form-item :label="t('userGUID')" :error="context.userGUID && !userValid ? t('invalidId') : ''">
              <div class="user-input-row">
                <el-input v-model="context.userGUID" id="binding-user" :label="t('userGUID')" :maxlength="idLimit"
                  :disabled="busy.binding" autocomplete="off" @input="changeUser()"
                  @focus="userEditing = true" @blur="userEditing = false" />
                <el-tooltip :content="t('useReportedUser')" placement="top">
                  <el-button icon="el-icon-refresh-left" :aria-label="t('useReportedUser')"
                    :disabled="!report || busy.binding || busy.profile" @click="useReportedUser" />
                </el-tooltip>
              </div>
            </el-form-item>
            <el-form-item :label="t('cabGuid')">
              <el-input :value="context.cabGuid" :label="t('cabGuid')" readonly />
            </el-form-item>
            <el-form-item :label="t('bootID')">
              <el-input :value="context.bootID" :label="t('bootID')" readonly />
            </el-form-item>
            <el-form-item :label="t('groupID')">
              <el-input :value="report ? report.groupID : ''" :label="t('groupID')" readonly />
            </el-form-item>
            <el-form-item :label="t('reportedUser')">
              <el-input :value="report ? report.userGUID : ''" :label="t('reportedUser')" readonly />
            </el-form-item>
          </div>
        </el-form>
        <div class="mapping-section">
          <div class="section-heading">
            <h2>{{ t('mqttMapping') }}</h2>
            <div class="action-row">
              <el-tag :type="mappingKnown && mqtt.cabGuid ? 'success' : 'info'" size="small">{{ t(mappingKnown ? (mqtt.cabGuid ? 'mappingSaved' : 'mappingMissing') : 'notQueried') }}</el-tag>
              <el-tag type="info" size="small">{{ t(pollingActive ? 'autoSync' : 'syncPaused') }}</el-tag>
            </div>
          </div>
          <el-alert v-if="errors.mqtt || errors.mapping" :title="errors.mapping || errors.mqtt" type="warning" :closable="false" show-icon />
          <el-form label-position="top" size="small" @submit.native.prevent="saveMapping">
            <el-form-item :label="t('mappedCabGuid')" :error="mappingEditing && mappingDraft && !mappingValid ? t('invalidCabGuid') : ''">
              <div class="mapping-row">
                <el-input :value="mappingEditing ? mappingDraft : mqtt.cabGuid" :label="t('mappedCabGuid')"
                  :maxlength="idLimit" :readonly="!mappingEditing" :disabled="busy.mapping" autocomplete="off" @input="mappingDraft = $event" />
                <el-button v-if="mappingEditing" type="primary" icon="el-icon-check" :aria-label="t('saveMapping')" :loading="busy.mapping"
                  :disabled="!canSaveMapping" @click="saveMapping">{{ t('saveMapping') }}</el-button>
                <el-tooltip v-else :content="t('editMapping')" placement="top">
                  <el-button icon="el-icon-edit" :aria-label="t('editMapping')" :disabled="!mappingKnown || busy.binding || reportDialog" @click="editMapping" />
                </el-tooltip>
                <el-tooltip v-if="mappingEditing" :content="t('cancel')" placement="top">
                  <el-button icon="el-icon-close" :aria-label="t('cancel')" :disabled="busy.mapping" @click="cancelMapping" />
                </el-tooltip>
              </div>
            </el-form-item>
          </el-form>
          <div class="report-line">
            <span>{{ t('receivedAt') }}: {{ formatDate(mqtt.receivedAt) }}</span>
            <span>{{ t(!mqtt.report ? 'waitingReport' : mqtt.fresh ? 'reportFresh' : 'reportStale') }}</span>
          </div>
        </div>
        <div class="report-line">
          <template v-if="report">
            <span>{{ t(reportSource === 'live' ? 'liveReport' : 'importedFallback') }}</span>
            <span>{{ t('reportState') }}: <strong>{{ report.bindingState }}</strong></span>
            <span v-if="reportSource === 'imported'">{{ t('reportTime') }}: {{ formatDate(reportImportedAt) }}</span>
          </template>
          <span v-else>{{ t('noReport') }}</span>
        </div>
      </section>

      <div class="workflow-grid">
        <section class="profile-section" :aria-label="t('profile')" @focusin="profileEditing = true" @focusout="profileEditing = false">
          <div class="section-heading">
            <h2>{{ t('profile') }}</h2>
            <el-button size="small" icon="el-icon-download" :loading="busy.profile" :disabled="!userValid || busy.binding" @click="loadProfile">{{ t('load') }}</el-button>
          </div>
          <div class="status-line" aria-live="polite">
            <el-tag :type="profileDirty ? 'warning' : 'info'" size="small">{{ t(profileDirty ? 'profileDirty' : profileState) }}</el-tag>
            <span v-if="profileVersion != null">{{ t('profileVersion') }}: {{ profileVersion }}</span>
          </div>
          <p class="profile-notice">{{ t('profileSnapshotNotice') }}</p>
          <el-alert v-if="errors.profile" :title="errors.profile" type="error" :closable="false" show-icon />
          <el-form label-position="top" size="small" :disabled="!userValid || busy.profile || busy.binding" @submit.native.prevent="saveProfile">
            <div class="profile-short-fields">
              <el-form-item :label="t('nickname')">
                <el-input v-model="profile.nickname" :label="t('nickname')" :maxlength="limits.nickname" show-word-limit @input="editProfile" />
              </el-form-item>
              <el-form-item :label="t('language')">
                <el-input v-model="profile.language" :label="t('language')" :maxlength="limits.language" show-word-limit @input="editProfile" />
              </el-form-item>
            </div>
            <el-form-item :label="t('interests')">
              <el-input v-model="profile.interests" :label="t('interests')" type="textarea" :rows="3" :maxlength="limits.interests" show-word-limit @input="editProfile" />
            </el-form-item>
            <el-form-item :label="t('explanationStyle')">
              <el-input v-model="profile.explanationStyle" :label="t('explanationStyle')" type="textarea" :rows="3" :maxlength="limits.explanationStyle" show-word-limit @input="editProfile" />
            </el-form-item>
            <el-button type="primary" size="small" icon="el-icon-check" :loading="busy.profile" :disabled="!profileDirty" @click="saveProfile">{{ t('save') }}</el-button>
          </el-form>
        </section>

        <section class="record-section" :aria-label="t('serverRecord')">
          <div class="section-heading">
            <h2>{{ t('serverRecord') }}</h2>
            <el-tooltip :content="t('refresh')" placement="top">
              <el-button size="small" icon="el-icon-refresh" :aria-label="t('refresh')" :loading="busy.record"
                :disabled="!macValid || busy.binding" @click="refreshBinding()" />
            </el-tooltip>
          </div>
          <div class="status-line" aria-live="polite">
            <el-tag :type="recordState === 'apiSaved' ? 'success' : 'info'" size="small">{{ t(recordState) }}</el-tag>
            <el-tag :type="readiness.type" size="small">{{ t('deviceConfirmation') }}: {{ t(readiness.key) }}</el-tag>
          </div>
          <el-alert v-if="errors.binding" :title="errors.binding" type="error" :closable="false" show-icon />
          <el-alert v-if="record && report && record.bootID !== report.bootID" :title="t('bootMismatch')" type="warning" :closable="false" show-icon />
          <div v-loading="busy.record" class="record-body">
            <dl v-if="record" class="record-fields">
              <template v-for="field in bindingFields">
                <dt :key="field + '-label'">{{ t(field) }}</dt>
                <dd :key="field">{{ field === 'expiresAt' ? formatDate(record[field]) : record[field] }}</dd>
              </template>
            </dl>
            <el-empty v-else :image-size="64" :description="t(recordKnown ? 'noBinding' : 'notQueried')" />
          </div>
          <div class="action-row">
            <el-button type="primary" size="small" icon="el-icon-connection" :loading="busy.binding"
              :disabled="!canBind" @click="bindCurrentBoot">{{ t(record ? 'rebind' : 'createBinding') }}</el-button>
            <el-button type="danger" plain size="small" icon="el-icon-close" :disabled="!record || !recordKnown || busy.binding || busy.record || busy.mapping || mappingEditing || reportDialog"
              @click="revokeBinding">{{ t('revoke') }}</el-button>
          </div>
        </section>
      </div>

      <section v-if="commandState" class="command-section" :aria-label="t('command')">
        <div class="section-heading">
          <h2>{{ t('command') }}</h2>
          <el-tag :type="hasCommand ? 'warning' : 'info'" size="small">{{ t(commandState) }}</el-tag>
        </div>
        <template v-if="hasCommand">
          <div class="topic-row">
            <span>{{ t('topic') }}</span><code>{{ commandTopic }}</code>
            <el-tooltip :content="t('copyTopic')" placement="top">
              <el-button size="mini" icon="el-icon-document-copy" :aria-label="t('copyTopic')" @click="copyTopic" />
            </el-tooltip>
          </div>
          <div class="command-toolbar">
            <span>{{ t('sensitive') }}</span>
            <div class="action-row">
              <el-tooltip :content="t(commandRevealed ? 'hide' : 'reveal')" placement="top">
                <el-button size="small" :icon="commandRevealed ? 'el-icon-lock' : 'el-icon-view'"
                  :aria-label="t(commandRevealed ? 'hide' : 'reveal')" @click="commandRevealed = !commandRevealed" />
              </el-tooltip>
              <el-button size="small" type="primary" icon="el-icon-document-copy" :loading="busy.copying" @click="copyCommand">{{ t('copyOnce') }}</el-button>
              <el-tooltip :content="t('clear')" placement="top">
                <el-button size="small" icon="el-icon-delete" :aria-label="t('clear')" @click="clearCommand()" />
              </el-tooltip>
            </div>
          </div>
          <pre class="command-preview">{{ commandPreview() }}</pre>
        </template>
      </section>

      <section class="history-section" :aria-label="t('history')">
        <div class="section-heading">
          <h2>{{ t('history') }}</h2>
          <el-button size="small" icon="el-icon-refresh" :disabled="!userValid" :loading="busy.sessions" @click="loadSessions(1)">{{ t('refreshHistory') }}</el-button>
        </div>
        <el-alert v-if="errors.sessions" :title="errors.sessions" type="error" :closable="false" show-icon />
        <el-table :data="sessions" v-loading="busy.sessions" size="small" :empty-text="t(sessionsLoaded ? 'noSessions' : 'historyUnknown')">
          <el-table-column prop="id" :label="t('usageSessionID')" min-width="220" show-overflow-tooltip />
          <el-table-column prop="macAddress" :label="t('macAddress')" min-width="160" />
          <el-table-column prop="agentName" :label="t('agent')" min-width="130" show-overflow-tooltip />
          <el-table-column prop="bootId" :label="t('bootID')" min-width="200" show-overflow-tooltip />
          <el-table-column prop="bindingVersion" :label="t('bindingVersion')" width="130" />
          <el-table-column :label="t('createdAt')" min-width="170">
            <template slot-scope="{ row }">{{ formatDate(row.createdAt) }}</template>
          </el-table-column>
          <el-table-column :label="t('status')" min-width="120">
            <template slot-scope="{ row }"><el-tag size="mini" type="info">{{ sessionStatus(row) }}</el-tag></template>
          </el-table-column>
          <el-table-column width="130" :label="t('messages')" fixed="right">
            <template slot-scope="{ row }"><el-button type="text" size="small" icon="el-icon-chat-line-square" @click="openMessages(row)">{{ t('messages') }}</el-button></template>
          </el-table-column>
        </el-table>
        <div v-if="sessionsLoaded" class="pagination-row">
          <el-button size="mini" icon="el-icon-arrow-left" :disabled="sessionPage === 1 || busy.sessions" @click="loadSessions(sessionPage - 1)">{{ t('previous') }}</el-button>
          <span>{{ t('page', { page: sessionPage }) }}</span>
          <el-button size="mini" :disabled="sessions.length < pageLimit || busy.sessions" @click="loadSessions(sessionPage + 1)">{{ t('next') }} <i class="el-icon-arrow-right" /></el-button>
        </div>
      </section>
    </main>
    <el-footer><VersionFooter /></el-footer>

    <el-dialog :title="t('reportTitle')" :visible.sync="reportDialog" width="620px" custom-class="binding-dialog" @closed="closeReport">
      <div class="report-source"><code>{{ t('reportSource') }}</code></div>
      <el-input v-model="reportText" :label="t('reportTitle')" type="textarea" :rows="10" maxlength="16384" autocomplete="off" />
      <el-alert v-if="reportError" :title="reportError" type="error" :closable="false" show-icon />
      <span slot="footer"><el-button size="small" @click="reportDialog = false">{{ t('cancel') }}</el-button><el-button size="small" type="primary" icon="el-icon-upload2" @click="importReport">{{ t('import') }}</el-button></span>
    </el-dialog>

    <el-dialog :title="t('messages')" :visible.sync="messagesDialog" width="900px" custom-class="binding-dialog" @closed="closeMessages">
      <div v-if="selectedSession" class="session-caption"><strong>{{ context.userGUID }}</strong><code>{{ selectedSession.id }}</code></div>
      <el-alert v-if="errors.messages" :title="errors.messages" type="error" :closable="false" show-icon />
      <div class="message-list" v-loading="busy.messages">
        <el-empty v-if="!messages.length && !busy.messages" :image-size="64" :description="t('noMessages')" />
        <article v-for="message in groupedMessages" :key="message.id" class="message-row">
          <div class="message-meta">
            <strong>{{ message.chatType === 1 ? t('user') : t('assistant') }}</strong>
            <span>{{ formatDate(message.reportTime || message.createdAt) }}</span>
            <el-tag v-for="status in message.deliveryStatuses" :key="status" size="mini" type="info">{{ t('delivery') }}: {{ status }}</el-tag>
            <span v-if="message.chunkCount > 1">{{ t('chunks', { count: message.chunkCount }) }}</span>
            <el-tag v-if="message.textTruncated" size="mini" type="warning">{{ t('textTruncated') }}</el-tag>
          </div>
          <p class="message-content">{{ message.content }}</p>
          <div class="message-ids"><span v-if="message.roleId">{{ t('role') }}: {{ message.roleId }}</span><span v-if="message.turnId">{{ t('turnID') }}: {{ message.turnId }}</span></div>
        </article>
      </div>
      <div slot="footer" class="pagination-row">
        <el-button size="mini" icon="el-icon-arrow-left" :disabled="messagePage === 1 || busy.messages" @click="loadMessages(messagePage - 1)">{{ t('previous') }}</el-button>
        <span>{{ t('page', { page: messagePage }) }}</span>
        <el-button size="mini" :disabled="messages.length < pageLimit || busy.messages" @click="loadMessages(messagePage + 1)">{{ t('next') }} <i class="el-icon-arrow-right" /></el-button>
      </div>
    </el-dialog>
  </div>
</template>

<script>
import Api from '@/apis/api';
import HeaderBar from '@/components/HeaderBar.vue';
import VersionFooter from '@/components/VersionFooter.vue';
import {
  PROFILE_LIMITS, ID_LIMIT, emptyProfile, validIdentifier, validMac, validCabGuid, profilePayload,
  readProfile, readOnlineReport, readMqttBinding, publicBinding, bindCommand, unbindCommand,
  reportMatchesBinding, readPage, errorKey, groupMessageChunks
} from '@/utils/deviceUserBinding';

// Credentials stay outside reactive state, Vuex, storage, URLs and record tables.
const commands = new WeakMap();
const sessionFields = ['id', 'macAddress', 'userGuid', 'bootId', 'bindingVersion', 'agentId', 'agentName', 'expiresAt', 'revokedAt', 'createdAt'];
const messageFields = ['id', 'chatType', 'content', 'createdAt', 'reportTime', 'sequence', 'deliveryStatus', 'roleId', 'turnId', 'usageSessionId', 'sessionId', 'bindingVersion', 'textTruncated'];
const pick = (record, fields) => Object.fromEntries(fields.map(field => [field, record[field]]));
const emptyMqtt = () => ({ cabGuid: '', report: null, receivedAt: null, fresh: false });

export default {
  name: 'DeviceUserBinding',
  components: { HeaderBar, VersionFooter },
  data() {
    return {
      context: { macAddress: this.$route.query.macAddress || '', userGUID: '', cabGuid: '', bootID: '' },
      limits: PROFILE_LIMITS, idLimit: ID_LIMIT,
      profile: emptyProfile(), profileVersion: null, profileState: 'profileUnknown', profileDirty: false,
      record: null, recordState: 'notQueried', recordKnown: false,
      bindingFields: ['userGUID', 'bootID', 'bindingVersion', 'usageSessionID', 'profileVersion', 'expiresAt'],
      report: null, reportImportedAt: null, reportDialog: false, reportText: '', reportError: '',
      legacyReport: null, reportSource: 'imported', mqtt: emptyMqtt(), mappingKnown: false,
      mappingDraft: '', mappingEditing: false, userEdited: false, userEditing: false, profileEditing: false,
      pollingEnabled: false, pollingActive: false, pollTimer: null, pollGeneration: 0, mqttSequence: 0, mappingSequence: 0,
      hasCommand: false, commandRevealed: false, commandState: '', commandTopic: '',
      busy: { profile: false, binding: false, record: false, copying: false, sessions: false, messages: false, mapping: false, mqtt: false },
      errors: { profile: '', binding: '', sessions: '', messages: '', mqtt: '', mapping: '' },
      epoch: 0, profileSequence: 0, recordSequence: 0, sessionSequence: 0, messageSequence: 0,
      sessions: [], sessionsLoaded: false, sessionPage: 1, pageLimit: 20,
      messages: [], selectedSession: null, messagesDialog: false, messagePage: 1
    };
  },
  computed: {
    groupedMessages() { return groupMessageChunks(this.messages); },
    macValid() { return validMac(this.context.macAddress); },
    userValid() { return validIdentifier(this.context.userGUID); },
    mappingValid() { return validCabGuid(this.mappingDraft); },
    canSaveMapping() {
      return this.macValid && this.mappingKnown && this.mappingValid && this.mappingDraft !== this.mqtt.cabGuid &&
        !this.busy.mapping && !this.busy.binding && !this.reportDialog;
    },
    canBind() {
      return this.macValid && this.userValid && !!this.report && this.recordKnown &&
        (this.reportSource !== 'live' || (this.mqtt.fresh && this.report === this.mqtt.report)) && !this.mappingEditing && !this.reportDialog &&
        !this.profileDirty && !this.busy.binding && !this.busy.mapping && !this.busy.record && !this.busy.profile;
    },
    readiness() {
      if (!this.report) return { type: 'info', key: 'unconfirmed' };
      const live = this.reportSource === 'live';
      if (live && (!this.mqtt.fresh || this.report !== this.mqtt.report)) return { type: 'info', key: 'reportStale' };
      const states = live
        ? { unbound: ['warning', 'liveUnbound'], pending: ['warning', 'livePending'], error: ['danger', 'liveError'] }
        : { unbound: ['warning', 'unbound'], pending: ['warning', 'pending'], error: ['danger', 'deviceError'] };
      const state = states[this.report.bindingState];
      if (state) return { type: state[0], key: state[1] };
      if (!this.recordKnown || !reportMatchesBinding(this.report, this.record)) return { type: 'info', key: 'unconfirmed' };
      return { type: 'success', key: live ? 'liveReady' : 'ready' };
    }
  },
  mounted() {
    this.pollingEnabled = true;
    window.addEventListener('pagehide', this.suspendPage);
    window.addEventListener('pageshow', this.startPolling);
    document.addEventListener('visibilitychange', this.visibilityChanged);
    this.startPolling();
  },
  beforeRouteLeave(to, from, next) { this.pollingEnabled = false; this.suspendPage(); next(); },
  beforeRouteUpdate(to, from, next) {
    this.invalidateRequests();
    this.context.macAddress = to.query.macAddress || '';
    this.changeDevice();
    this.context.userGUID = '';
    this.changeUser(false);
    next();
  },
  beforeDestroy() {
    this.pollingEnabled = false;
    this.suspendPage();
    window.removeEventListener('pagehide', this.suspendPage);
    window.removeEventListener('pageshow', this.startPolling);
    document.removeEventListener('visibilitychange', this.visibilityChanged);
  },
  methods: {
    t(key, values) { return this.$t(`userBinding.${key}`, values); },
    formatDate(value) { return value ? new Date(value).toLocaleString() : '-'; },
    backToDevices() { this.$router.push({ name: 'DeviceManagement', query: { agentId: this.$route.query.agentId || '' } }); },
    visibilityChanged() {
      if (document.hidden) { this.commandRevealed = false; this.stopPolling(); }
      else this.startPolling();
    },
    suspendPage() { this.stopPolling(); this.invalidateRequests(); },
    stopPolling() {
      clearTimeout(this.pollTimer);
      this.pollTimer = null;
      this.pollingActive = false;
      this.pollGeneration++;
      this.mqttSequence++;
      this.recordSequence++;
      this.busy.mqtt = false;
      this.busy.record = false;
      this.mqtt.fresh = false;
    },
    startPolling() {
      if (!this.pollingEnabled || this.pollingActive || document.hidden || !this.macValid) return;
      this.pollingActive = true;
      this.poll();
    },
    async poll() {
      const generation = this.pollGeneration;
      if (!this.pollingActive) return;
      await Promise.all([this.refreshMqtt(), this.refreshBinding(true)]);
      if (!this.pollingActive || generation !== this.pollGeneration) return;
      this.pollTimer = setTimeout(() => this.poll(), 5000);
    },
    async refreshMqtt() {
      if (!this.macValid || this.busy.mapping || this.busy.binding) return;
      const mac = this.context.macAddress;
      const sequence = ++this.mqttSequence;
      this.busy.mqtt = true;
      try {
        const response = await Api.deviceUserBinding.getMqttBinding(mac);
        if (sequence !== this.mqttSequence || mac !== this.context.macAddress) return;
        this.mqtt = readMqttBinding(response, mac);
        this.mappingKnown = true;
        this.errors.mqtt = '';
        this.errors.mapping = '';
        this.selectReport();
      } catch (error) {
        if (sequence !== this.mqttSequence || mac !== this.context.macAddress) return;
        this.mappingKnown = false;
        this.mqtt.fresh = false;
        this.errors.mqtt = this.t(errorKey(error));
        this.selectReport();
      } finally { if (sequence === this.mqttSequence) this.busy.mqtt = false; }
    },
    selectReport() {
      if (this.busy.binding || this.busy.mapping || this.mappingEditing || this.reportDialog) return;
      const live = this.mqtt.fresh && this.mqtt.report;
      const report = live || this.legacyReport || this.mqtt.report;
      const source = live || (!this.legacyReport && this.mqtt.report) ? 'live' : 'imported';
      if (this.context.bootID !== (report?.bootID || '') || this.context.cabGuid !== (report?.cabGuid || '')) {
        this.epoch++;
        this.clearCommand();
      }
      this.report = report;
      this.reportSource = source;
      this.context.cabGuid = report?.cabGuid || '';
      this.context.bootID = report?.bootID || '';
      if (report && !this.userEdited && !this.userEditing && !this.profileEditing && !this.profileDirty && !this.busy.profile && !this.messagesDialog &&
          (source !== 'live' || this.mqtt.fresh) && this.context.userGUID !== report.userGUID) {
        this.context.userGUID = report.userGUID;
        this.changeUser(false);
      }
    },
    async useReportedUser() {
      if (!this.report || this.busy.binding || this.busy.profile) return;
      const epoch = this.epoch;
      const userGUID = this.report.userGUID;
      if (this.profileDirty && !await this.confirm('discardProfile')) return;
      if (epoch !== this.epoch || userGUID !== this.report?.userGUID) return;
      if (this.context.userGUID !== userGUID || this.profileDirty) {
        this.context.userGUID = userGUID;
        this.changeUser(false);
      }
      this.userEdited = false;
    },
    editMapping() {
      this.mappingDraft = this.mqtt.cabGuid || this.legacyReport?.cabGuid || '';
      this.mappingEditing = true;
      this.errors.mapping = '';
    },
    cancelMapping() { this.mappingEditing = false; this.selectReport(); },
    async saveMapping() {
      if (!this.canSaveMapping) return;
      const mac = this.context.macAddress;
      const cabGuid = this.mappingDraft;
      const sequence = ++this.mappingSequence;
      this.mqttSequence++;
      this.busy.mqtt = false;
      this.busy.mapping = true;
      this.errors.mapping = '';
      let saved = false;
      try {
        if (!await this.confirm('mappingConfirm', { macAddress: mac, cabGuid }) || sequence !== this.mappingSequence) return;
        this.clearCommand();
        await Api.deviceUserBinding.saveMqttBinding(mac, cabGuid);
        if (sequence !== this.mappingSequence || mac !== this.context.macAddress) return;
        saved = true;
        this.mqtt = emptyMqtt();
        this.mappingKnown = false;
        this.mappingEditing = false;
        if (this.legacyReport?.cabGuid !== cabGuid) { this.legacyReport = null; this.reportImportedAt = null; }
        const response = await Api.deviceUserBinding.getMqttBinding(mac);
        if (sequence !== this.mappingSequence || mac !== this.context.macAddress) return;
        this.mqtt = readMqttBinding(response, mac);
        this.mappingKnown = true;
        this.errors.mqtt = '';
      } catch (error) {
        if (sequence === this.mappingSequence) {
          this.mappingKnown = false;
          this.mqtt.fresh = false;
          this.errors.mapping = this.t(saved ? 'mappingRefreshRequired' : errorKey(error));
        }
      } finally {
        if (sequence === this.mappingSequence) {
          this.busy.mapping = false;
          this.selectReport();
        }
      }
    },
    clearCommand(state = 'commandCleared') {
      commands.delete(this);
      this.hasCommand = false;
      this.commandRevealed = false;
      this.commandTopic = '';
      this.busy.copying = false;
      if (this.commandState) this.commandState = state;
    },
    invalidateRequests() {
      this.epoch++;
      this.profileSequence++;
      this.recordSequence++;
      this.sessionSequence++;
      this.messageSequence++;
      this.mqttSequence++;
      this.mappingSequence++;
      this.clearCommand();
      Object.keys(this.busy).forEach(key => { this.busy[key] = false; });
      this.reportText = '';
    },
    changeDevice() {
      this.stopPolling();
      this.mappingSequence++;
      this.epoch++;
      this.recordSequence++;
      this.clearCommand();
      this.report = null;
      this.legacyReport = null;
      this.mqtt = emptyMqtt();
      this.mappingKnown = false;
      this.mappingDraft = '';
      this.mappingEditing = false;
      this.errors.mqtt = '';
      this.errors.mapping = '';
      this.reportImportedAt = null;
      this.context.cabGuid = '';
      this.context.bootID = '';
      this.record = null;
      this.recordKnown = false;
      this.recordState = 'notQueried';
      this.errors.binding = '';
      this.busy.record = false;
      this.startPolling();
    },
    changeUser(manual = true) {
      this.userEdited = manual;
      this.epoch++;
      this.profileSequence++;
      this.sessionSequence++;
      this.clearCommand();
      this.profile = emptyProfile();
      this.profileVersion = null;
      this.profileDirty = false;
      this.profileState = 'profileUnknown';
      this.errors.profile = '';
      this.errors.sessions = '';
      this.busy.profile = false;
      this.busy.sessions = false;
      this.sessions = [];
      this.sessionsLoaded = false;
      this.sessionPage = 1;
      this.messagesDialog = false;
      this.closeMessages();
    },
    editProfile() { this.profileDirty = true; this.clearCommand(); },
    async confirm(key, values) {
      try {
        await this.$confirm(this.t(key, values), this.t('title'), {
          confirmButtonText: this.t('confirm'), cancelButtonText: this.t('cancel'), type: 'warning',
          customClass: 'binding-confirm-dialog'
        });
        return true;
      } catch { return false; }
    },
    async loadProfile() {
      if (!this.userValid || this.busy.profile) return;
      const userGUID = this.context.userGUID;
      const sequence = ++this.profileSequence;
      this.busy.profile = true;
      this.errors.profile = '';
      try {
        if (this.profileDirty && !await this.confirm('discardProfile')) return;
        if (sequence !== this.profileSequence) return;
        const response = await Api.deviceUserBinding.getProfile(userGUID);
        if (sequence !== this.profileSequence || userGUID !== this.context.userGUID) return;
        const result = readProfile(response, userGUID);
        this.profile = result.profile;
        this.profileVersion = result.version;
        this.profileDirty = false;
        this.profileState = response === null ? 'profileMissing' : 'profileLoaded';
      } catch (error) {
        if (sequence === this.profileSequence) this.errors.profile = this.t(errorKey(error));
      } finally {
        if (sequence === this.profileSequence) this.busy.profile = false;
      }
    },
    async saveProfile() {
      if (!this.userValid || !this.profileDirty || this.busy.profile) return;
      const userGUID = this.context.userGUID;
      const sequence = ++this.profileSequence;
      this.busy.profile = true;
      this.errors.profile = '';
      this.clearCommand();
      try {
        const payload = profilePayload(this.profile);
        if (!await this.confirm('profileConfirm') || sequence !== this.profileSequence) return;
        const response = await Api.deviceUserBinding.saveProfile(userGUID, payload);
        if (sequence !== this.profileSequence || userGUID !== this.context.userGUID) return;
        const result = readProfile(response, userGUID);
        if (response === null) throw new Error('invalidResponse');
        this.profile = result.profile;
        this.profileVersion = result.version;
        this.profileDirty = false;
        this.profileState = 'profileSaved';
        this.$message.success({ message: this.t('profileSaved'), customClass: 'binding-notice' });
      } catch (error) {
        if (sequence === this.profileSequence) this.errors.profile = this.t(errorKey(error));
      } finally {
        if (sequence === this.profileSequence) this.busy.profile = false;
      }
    },
    openReport() {
      this.clearCommand();
      this.reportError = '';
      this.reportText = '';
      this.reportDialog = true;
    },
    closeReport() { this.reportText = ''; this.reportError = ''; this.selectReport(); },
    importReport() {
      try {
        const report = readOnlineReport(this.reportText);
        if (report.macAddress && this.context.macAddress && report.macAddress.toLowerCase() !== this.context.macAddress.toLowerCase()) {
          this.reportError = this.t('reportMacMismatch');
          return;
        }
        this.epoch++;
        this.clearCommand();
        this.report = report;
        this.legacyReport = report;
        this.reportSource = 'imported';
        this.reportImportedAt = new Date().toISOString();
        this.context.cabGuid = report.cabGuid;
        this.context.bootID = report.bootID;
        if (!this.context.macAddress && report.macAddress) this.context.macAddress = report.macAddress;
        this.reportText = '';
        this.reportDialog = false;
        this.selectReport();
        if (this.macValid) { this.refreshBinding(); this.startPolling(); }
      } catch (error) { this.reportError = this.t(errorKey(error)); }
    },
    async refreshBinding(background = false) {
      if (!this.macValid || this.busy.binding) return;
      const mac = this.context.macAddress;
      const sequence = ++this.recordSequence;
      if (!background) { this.busy.record = true; this.recordKnown = false; this.errors.binding = ''; }
      try {
        const response = await Api.deviceUserBinding.getBinding(mac);
        if (sequence !== this.recordSequence || mac !== this.context.macAddress) return;
        const record = publicBinding(response);
        const changed = this.record?.usageSessionID !== record?.usageSessionID || this.record?.bootID !== record?.bootID ||
          this.record?.bindingVersion !== record?.bindingVersion || this.record?.userGUID !== record?.userGUID;
        if (changed) this.clearCommand();
        this.record = record;
        this.recordKnown = true;
        if (changed || !['apiSaved', 'apiRevoked'].includes(this.recordState)) this.recordState = record ? 'apiLoaded' : 'noBinding';
        this.errors.binding = '';
      } catch (error) {
        if (sequence === this.recordSequence) {
          this.record = null;
          this.recordKnown = false;
          this.recordState = 'notQueried';
          this.errors.binding = this.t(errorKey(error));
          this.clearCommand();
        }
      } finally {
        if (sequence === this.recordSequence) this.busy.record = false;
      }
    },
    setCommand(command) {
      commands.set(this, command);
      this.commandRevealed = false;
      this.commandTopic = `cabinet/cmd/${this.context.cabGuid}`;
      this.commandState = 'commandPending';
      this.hasCommand = true;
    },
    async bindCurrentBoot() {
      if (!this.canBind) return;
      const epoch = this.epoch;
      const context = { ...this.context };
      this.recordSequence++;
      this.mqttSequence++;
      this.busy.mqtt = false;
      this.busy.binding = true;
      this.errors.binding = '';
      let saved = false;
      try {
        if (!await this.confirm('bindConfirm', { userGUID: context.userGUID }) || epoch !== this.epoch) return;
        this.clearCommand();
        const response = await Api.deviceUserBinding.bind(context.macAddress, context.bootID, context.userGUID);
        if (epoch !== this.epoch) return;
        saved = true;
        this.recordState = 'apiSaved';
        this.record = null;
        this.recordKnown = false;
        this.record = publicBinding(response);
        this.recordKnown = true;
        const command = bindCommand(response, context);
        if (command.bindingVersion <= this.report.bindingVersion) throw new Error('invalidCommand');
        this.setCommand(command);
        this.$message.success({ message: this.t('bindingSaved'), customClass: 'binding-notice' });
        this.loadSessions(1);
      } catch (error) {
        if (epoch === this.epoch) {
          this.errors.binding = this.t(saved ? 'savedNoCommand' : errorKey(error));
          this.recordKnown = false;
          if (!saved) { this.recordState = 'notQueried'; this.record = null; }
        }
      } finally { if (epoch === this.epoch) this.busy.binding = false; }
    },
    async revokeBinding() {
      if (!this.record || !this.recordKnown || this.busy.binding || this.busy.record || this.busy.mapping || this.mappingEditing || this.reportDialog) return;
      const epoch = this.epoch;
      const previous = { ...this.record };
      this.recordSequence++;
      this.mqttSequence++;
      this.busy.mqtt = false;
      this.busy.binding = true;
      this.errors.binding = '';
      let revoked = false;
      try {
        if (!await this.confirm('revokeConfirm') || epoch !== this.epoch) return;
        this.clearCommand();
        const response = await Api.deviceUserBinding.unbind(this.context.macAddress);
        if (epoch !== this.epoch) return;
        revoked = true;
        this.record = null;
        this.recordKnown = true;
        this.recordState = 'apiRevoked';
        this.commandState = 'serverOnlyRevoke';
        if (response !== null) {
          if (!this.report || this.context.bootID !== previous.bootID) throw new Error('invalidCommand');
          const command = unbindCommand(response, previous);
          if (command.bindingVersion <= this.report.bindingVersion) throw new Error('invalidCommand');
          this.setCommand(command);
        }
        this.$message.success({ message: this.t('bindingRevoked'), customClass: 'binding-notice' });
        if (this.userValid) this.loadSessions(1);
      } catch (error) {
        if (epoch === this.epoch) {
          this.errors.binding = this.t(revoked ? 'savedNoCommand' : errorKey(error));
          this.recordKnown = false;
          if (!revoked) { this.recordState = 'notQueried'; this.record = null; }
        }
      } finally { if (epoch === this.epoch) this.busy.binding = false; }
    },
    commandPreview() {
      const command = commands.get(this);
      if (!command) return '';
      return JSON.stringify(command.bindingToken && !this.commandRevealed ? { ...command, bindingToken: '********' } : command, null, 2);
    },
    async copyTopic() {
      try { await this.writeClipboard(this.commandTopic); this.$message.success({ message: this.t('topicCopied'), customClass: 'binding-notice' }); }
      catch { this.$message.error({ message: this.t('copyFailed'), customClass: 'binding-notice' }); }
    },
    async copyCommand() {
      const command = commands.get(this);
      if (!command || this.busy.copying) return;
      this.busy.copying = true;
      try {
        await this.writeClipboard(JSON.stringify(command, null, 2));
        if (commands.get(this) === command) this.clearCommand('commandCopied');
      } catch {
        if (commands.get(this) === command) this.$message.error({ message: this.t('copyFailed'), customClass: 'binding-notice' });
      } finally { if (commands.get(this) === command) this.busy.copying = false; }
    },
    async writeClipboard(text) {
      if (navigator.clipboard?.writeText) return navigator.clipboard.writeText(text);
      const input = document.createElement('textarea');
      input.value = text;
      input.style.cssText = 'position:fixed;opacity:0;pointer-events:none';
      document.body.appendChild(input);
      try {
        input.select();
        if (!document.execCommand('copy')) throw new Error('clipboard');
      } finally { input.value = ''; input.remove(); }
    },
    async loadSessions(page = 1) {
      if (!this.userValid) return;
      const userGUID = this.context.userGUID;
      const sequence = ++this.sessionSequence;
      this.busy.sessions = true;
      this.errors.sessions = '';
      this.sessions = [];
      this.sessionsLoaded = false;
      try {
        const response = await Api.deviceUserBinding.getSessions(userGUID, page, this.pageLimit);
        if (sequence !== this.sessionSequence || userGUID !== this.context.userGUID) return;
        this.sessions = readPage(response).map(row => pick(row, sessionFields));
        this.sessionPage = page;
        this.sessionsLoaded = true;
      } catch (error) {
        if (sequence === this.sessionSequence) this.errors.sessions = this.t(errorKey(error));
      } finally { if (sequence === this.sessionSequence) this.busy.sessions = false; }
    },
    sessionStatus(session) {
      if (session.revokedAt) return this.t('revoked');
      if (session.expiresAt && new Date(session.expiresAt).getTime() <= Date.now()) return this.t('expired');
      return this.t('active');
    },
    openMessages(session) {
      this.selectedSession = session;
      this.messagesDialog = true;
      this.loadMessages(1);
    },
    closeMessages() {
      this.messageSequence++;
      this.selectedSession = null;
      this.messages = [];
      this.messagePage = 1;
      this.busy.messages = false;
      this.errors.messages = '';
    },
    async loadMessages(page = 1) {
      if (!this.selectedSession || !this.userValid) return;
      const userGUID = this.context.userGUID;
      const sessionID = this.selectedSession.id;
      const sequence = ++this.messageSequence;
      this.busy.messages = true;
      this.errors.messages = '';
      this.messages = [];
      try {
        const response = await Api.deviceUserBinding.getMessages(userGUID, sessionID, page, this.pageLimit);
        if (sequence !== this.messageSequence || userGUID !== this.context.userGUID || sessionID !== this.selectedSession?.id) return;
        this.messages = readPage(response).map(row => pick(row, messageFields));
        this.messagePage = page;
      } catch (error) {
        if (sequence === this.messageSequence) this.errors.messages = this.t(errorKey(error));
      } finally { if (sequence === this.messageSequence) this.busy.messages = false; }
    }
  }
};
</script>

<style scoped>
.binding-page { min-height: 100vh; background: #f5f7fa; text-align: left; color: #303133; letter-spacing: 0; }
.binding-main { max-width: 1440px; margin: 0 auto; padding: 20px 24px 32px; }
.page-heading, .section-heading, .heading-title, .status-line, .action-row, .topic-row, .command-toolbar, .pagination-row, .message-meta { display: flex; align-items: center; gap: 12px; }
.page-heading, .section-heading, .command-toolbar { justify-content: space-between; }
.page-heading { margin-bottom: 18px; }
h1 { margin: 0; font-size: 22px; line-height: 1.4; }
h2 { margin: 0; font-size: 16px; line-height: 1.5; }
.section-heading { margin-bottom: 16px; }
.context-section, .workflow-grid, .command-section, .history-section { background: #fff; border-bottom: 1px solid #dcdfe6; padding: 20px; }
.context-grid { display: grid; grid-template-columns: 1fr 1fr 1fr 1.4fr; gap: 16px; }
.context-grid .el-form-item { margin-bottom: 18px; min-width: 0; }
.report-line { display: flex; flex-wrap: wrap; gap: 8px 24px; font-size: 12px; line-height: 1.6; color: #606266; overflow-wrap: anywhere; }
.mapping-section { border-top: 1px solid #ebeef5; padding-top: 16px; margin-bottom: 16px; }
.mapping-row, .user-input-row { display: flex; align-items: center; gap: 8px; min-width: 0; }
.mapping-row .el-input, .user-input-row .el-input { min-width: 0; }
.mapping-row .el-button + .el-button { margin-left: 0; }
.mapping-row > .el-button, .user-input-row > .el-button { flex-shrink: 0; }
.workflow-grid { display: grid; grid-template-columns: minmax(0, 1fr) minmax(0, 1.25fr); gap: 32px; }
.profile-section, .record-section { min-width: 0; }
.record-section { border-left: 1px solid #ebeef5; padding-left: 32px; }
.profile-short-fields { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; }
.status-line { flex-wrap: wrap; font-size: 12px; color: #606266; margin-bottom: 16px; }
.profile-notice { font-size: 12px; line-height: 1.5; color: #606266; margin: 0 0 16px; }
.record-body { min-height: 270px; }
.record-fields { display: grid; grid-template-columns: 130px minmax(0, 1fr); gap: 18px 12px; margin: 20px 0; font-size: 13px; }
dt { color: #606266; }
dd { margin: 0; overflow-wrap: anywhere; }
.action-row { flex-wrap: wrap; gap: 8px; }
.action-row .el-button + .el-button { margin-left: 0; }
.topic-row { flex-wrap: wrap; font-size: 13px; margin-bottom: 16px; }
code { font-family: Consolas, monospace; overflow-wrap: anywhere; }
.topic-row code { color: #245b48; }
.command-toolbar { flex-wrap: wrap; font-size: 12px; color: #606266; margin-bottom: 10px; }
.command-preview { margin: 0; padding: 14px; background: #f5f7fa; border: 1px solid #e4e7ed; border-radius: 4px; white-space: pre-wrap; overflow-wrap: anywhere; font-size: 12px; line-height: 1.7; }
.pagination-row { justify-content: flex-end; flex-wrap: wrap; margin-top: 16px; font-size: 12px; }
.pagination-row .el-button + .el-button { margin-left: 0; }
.el-alert { margin-bottom: 14px; }
.session-caption { display: flex; flex-wrap: wrap; gap: 8px 16px; padding-bottom: 16px; font-size: 13px; overflow-wrap: anywhere; }
.message-list { min-height: 160px; max-height: 55vh; overflow: auto; }
.message-row { padding: 16px 0; border-bottom: 1px solid #ebeef5; }
.message-meta { flex-wrap: wrap; font-size: 12px; color: #606266; }
.message-meta strong { color: #303133; }
.message-content { margin: 12px 0; white-space: pre-wrap; overflow-wrap: anywhere; font-size: 14px; line-height: 1.7; }
.message-ids { display: flex; flex-wrap: wrap; gap: 6px 16px; font-size: 12px; color: #606266; overflow-wrap: anywhere; }
.report-source { margin-bottom: 12px; font-size: 13px; }
::v-deep .binding-dialog { max-width: calc(100% - 24px); border-radius: 4px; text-align: left; }
::v-deep .el-textarea__inner { padding-bottom: 22px; }
::v-deep .el-tag { height: auto; min-height: 24px; white-space: normal; overflow-wrap: anywhere; line-height: 22px; }
::v-deep .el-form-item__label { padding-bottom: 6px; line-height: 22px; }
@media (max-width: 1000px) {
  .context-grid { grid-template-columns: 1fr 1fr; }
  .workflow-grid { gap: 24px; }
  .record-section { padding-left: 24px; }
  .record-fields { grid-template-columns: 105px minmax(0, 1fr); }
  ::v-deep .header { height: auto !important; min-width: 0; padding: 12px; }
  ::v-deep .header-container { width: 100%; padding: 0; min-width: 0; flex-wrap: wrap; gap: 8px; }
  ::v-deep .header-center { position: static; transform: none; order: 3; width: 100%; overflow-x: auto; justify-content: flex-start; gap: 12px; }
  ::v-deep .header-left { flex-shrink: 0; }
  ::v-deep .header-right { min-width: 0; margin-left: auto; }
}
@media (max-width: 700px) {
  .binding-main { padding: 16px 12px; }
  h1 { font-size: 18px; }
  .page-heading { align-items: flex-start; gap: 8px; flex-wrap: wrap; }
  .context-section, .workflow-grid, .command-section, .history-section { padding: 16px 12px; }
  .workflow-grid { grid-template-columns: minmax(0, 1fr); }
  .record-section { padding: 20px 0 0; border-left: 0; border-top: 1px solid #ebeef5; }
  .context-grid, .profile-short-fields { grid-template-columns: minmax(0, 1fr); gap: 0; }
  .record-body { min-height: 180px; }
  .section-heading { gap: 8px; flex-wrap: wrap; }
  .mapping-row { flex-wrap: wrap; }
  .mapping-row .el-input { flex: 1 0 100%; }
  .command-toolbar > span { width: 100%; }
  ::v-deep .binding-dialog .el-dialog__body { padding: 12px; }
}
</style>

<style>
.binding-confirm-dialog { max-width: calc(100% - 24px); }
.binding-confirm-dialog .el-message-box__message { overflow-wrap: anywhere; }
.binding-notice { min-width: 0; max-width: calc(100% - 24px); }
</style>
