export const PROFILE_LIMITS = Object.freeze({ nickname: 64, language: 32, interests: 1024, explanationStyle: 512 });
export const ID_LIMIT = 128;

export function validIdentifier(value) {
    return typeof value === 'string' && value.length > 0 && value.length <= ID_LIMIT &&
        value === value.trim() && !/[\x00-\x1f\x7f]/.test(value);
}

export function validMac(value) {
    return typeof value === 'string' && /^([0-9a-f]{2}:){5}[0-9a-f]{2}$/i.test(value);
}

export function validCabGuid(value) {
    return validIdentifier(value) && !/[+#/]/.test(value);
}

export function emptyProfile() {
    return { nickname: '', language: '', interests: '', explanationStyle: '' };
}

export function profilePayload(profile) {
    const result = {};
    for (const [field, limit] of Object.entries(PROFILE_LIMITS)) {
        const value = profile[field];
        if (value != null && (typeof value !== 'string' || value.length > limit)) throw new Error('invalidProfile');
        result[field] = value || null;
    }
    return result;
}

export function readProfile(response, userGUID) {
    if (response === null) return { profile: emptyProfile(), version: null };
    if (!response || !response.profile || (userGUID && response.userGUID !== userGUID)) throw new Error('invalidResponse');
    return { profile: { ...emptyProfile(), ...profilePayload(response.profile) }, version: response.profileVersion };
}

export function readOnlineReport(text) {
    let report;
    try { report = JSON.parse(text); } catch { throw new Error('invalidReport'); }
    return publicOnlineReport(report);
}

export function publicOnlineReport(report) {
    if (!report || typeof report !== 'object' || Array.isArray(report) ||
        report.cmd !== 'KJ' || !validCabGuid(report.cabGuid) || !validIdentifier(report.bootID) ||
        !['unbound', 'pending', 'ready', 'error'].includes(report.bindingState) ||
        !Number.isSafeInteger(report.bindingVersion) || report.bindingVersion < 0 ||
        !(report.userGUID === '' || validIdentifier(report.userGUID)) ||
        !(report.usageSessionID === '' || validIdentifier(report.usageSessionID)) ||
        !(report.groupID == null || report.groupID === '' || validIdentifier(report.groupID))) throw new Error('invalidReport');
    // MAC is an independent device identity; never derive it from the cabinet GUID.
    if (report.macAddress != null && !validMac(report.macAddress)) throw new Error('invalidMac');
    return {
        cabGuid: report.cabGuid, bootID: report.bootID, macAddress: report.macAddress || '',
        bindingState: report.bindingState, bindingVersion: report.bindingVersion,
        userGUID: report.userGUID, usageSessionID: report.usageSessionID, groupID: report.groupID || ''
    };
}

export function readMqttBinding(response, macAddress) {
    if (!response || !validMac(response.macAddress) ||
        response.macAddress.toLowerCase() !== macAddress.toLowerCase() ||
        !(response.cabGuid == null || response.cabGuid === '' || validCabGuid(response.cabGuid)) ||
        typeof response.fresh !== 'boolean' ||
        !(response.receivedAt === null || (typeof response.receivedAt === 'string' &&
            /^\d{4}-\d{2}-\d{2}T/.test(response.receivedAt) && Number.isFinite(Date.parse(response.receivedAt))))) {
        throw new Error('invalidResponse');
    }
    const report = response.report === null ? null : publicOnlineReport(response.report);
    if ((report && (!response.receivedAt || report.cabGuid !== response.cabGuid ||
        (report.macAddress && report.macAddress.toLowerCase() !== macAddress.toLowerCase()))) ||
        (response.fresh && !report)) throw new Error('invalidResponse');
    return { cabGuid: response.cabGuid || '', report, receivedAt: response.receivedAt, fresh: response.fresh };
}

export function publicBinding(binding) {
    if (binding === null) return null;
    if (!binding || !validIdentifier(binding.userGUID) || !validIdentifier(binding.bootID) ||
        !Number.isSafeInteger(binding.bindingVersion) || binding.bindingVersion < 1 ||
        !validIdentifier(binding.usageSessionID)) throw new Error('invalidResponse');
    // Deliberate allowlist: responses must never place a token or nested command in Vue state.
    return {
        userGUID: binding.userGUID,
        bootID: binding.bootID,
        bindingVersion: binding.bindingVersion,
        usageSessionID: binding.usageSessionID,
        profileVersion: binding.profileVersion,
        agentId: binding.agentId,
        expiresAt: binding.expiresAt
    };
}

export function randomUuid(crypto = globalThis.crypto) {
    if (crypto?.randomUUID) return crypto.randomUUID();
    if (!crypto?.getRandomValues) throw new Error('cryptoUnavailable');
    const bytes = crypto.getRandomValues(new Uint8Array(16));
    bytes[6] = (bytes[6] & 15) | 64;
    bytes[8] = (bytes[8] & 63) | 128;
    const hex = Array.from(bytes, byte => byte.toString(16).padStart(2, '0')).join('');
    return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
}

export function bindCommand(response, context) {
    const binding = publicBinding(response);
    if (!binding || binding.bootID !== context.bootID || binding.userGUID !== context.userGUID ||
        typeof response.bindingToken !== 'string' || response.bindingToken.length < 43 ||
        response.bindingToken.length > 128) throw new Error('invalidCommand');
    const command = response.command;
    if (command && (command.cmd !== 'bindUser' || !validIdentifier(command.cmdID) ||
        command.userGUID !== binding.userGUID || command.bootID !== binding.bootID ||
        command.bindingVersion !== binding.bindingVersion || command.usageSessionID !== binding.usageSessionID ||
        command.bindingToken !== response.bindingToken)) throw new Error('invalidCommand');
    return {
        cmd: 'bindUser', cmdID: command?.cmdID || randomUuid(), userGUID: binding.userGUID,
        bootID: binding.bootID, bindingVersion: binding.bindingVersion,
        bindingToken: response.bindingToken, usageSessionID: binding.usageSessionID
    };
}

export function unbindCommand(response, context) {
    const command = response;
    if (!command || command.cmd !== 'unbindUser' || !validIdentifier(command.cmdID) ||
        command.bootID !== context.bootID || command.userGUID !== '' ||
        !Number.isSafeInteger(command.bindingVersion) || command.bindingVersion <= context.bindingVersion) {
        throw new Error('invalidCommand');
    }
    return {
        cmd: 'unbindUser', cmdID: command.cmdID, bootID: command.bootID,
        bindingVersion: command.bindingVersion, userGUID: ''
    };
}

export function reportMatchesBinding(report, binding) {
    return !!report && !!binding && report.bootID === binding.bootID &&
        report.userGUID === binding.userGUID && report.bindingVersion === binding.bindingVersion &&
        report.usageSessionID === binding.usageSessionID;
}

export function readPage(response) {
    if (!Array.isArray(response)) throw new Error('invalidResponse');
    return response;
}

export function groupMessageChunks(messages) {
    const result = [];
    for (const message of messages) {
        const last = result[result.length - 1];
        const session = message.usageSessionId || message.sessionId;
        const sameTurn = last && message.chatType === 2 && last.chatType === 2 &&
            message.turnId && last.turnId === message.turnId && session &&
            (last.usageSessionId || last.sessionId) === session;
        if (sameTurn) {
            last.content += message.content || '';
            last.chunkCount++;
            last.textTruncated = last.textTruncated || message.textTruncated;
            if (message.deliveryStatus && !last.deliveryStatuses.includes(message.deliveryStatus)) {
                last.deliveryStatuses.push(message.deliveryStatus);
            }
        } else {
            result.push({ ...message, content: message.content || '', chunkCount: 1,
                deliveryStatuses: message.deliveryStatus ? [message.deliveryStatus] : [] });
        }
    }
    return result;
}

export function errorKey(error) {
    if (error?.status === 401 || error?.code === 401) return 'authExpired';
    if (error?.status === 403 || error?.code === 403) return 'forbidden';
    if (error?.status === 404) return 'endpointMissing';
    if (error?.status === 409) return 'conflict';
    if (error?.code === 10034) return 'validationFailed';
    if (error?.status === 0) return 'networkError';
    if (['invalidProfile', 'invalidResponse', 'invalidCommand', 'invalidReport', 'invalidMac', 'cryptoUnavailable'].includes(error?.message)) return error.message;
    return 'requestFailed';
}
