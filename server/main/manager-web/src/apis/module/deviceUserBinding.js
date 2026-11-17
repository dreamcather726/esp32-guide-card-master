import { getServiceUrl } from '../api';
import RequestService from '../httpRequest';

function request(path, method = 'GET', data = {}) {
    return new Promise((resolve, reject) => {
        // Only status/code leave the transport on failure. Messages may echo credentials.
        const fail = response => reject({
            status: response?.status || 0,
            code: response?.data?.code
        });
        RequestService.sendRequest()
            .url(`${getServiceUrl()}${path}`)
            .method(method)
            .sensitive()
            .data(data)
            .success(response => resolve(response.data.data))
            .fail(fail)
            .networkFail(fail)
            .send();
    });
}

const profilePath = userGUID => `/user/profile/${encodeURIComponent(userGUID)}`;
const bindingPath = macAddress => `/device/${encodeURIComponent(macAddress)}/user-binding`;
const mqttBindingPath = macAddress => `/device/${encodeURIComponent(macAddress)}/mqtt-binding`;

export default {
    getProfile: userGUID => request(profilePath(userGUID)),
    saveProfile: (userGUID, profile) => request(profilePath(userGUID), 'PUT', profile),
    getBinding: macAddress => request(bindingPath(macAddress)),
    getMqttBinding: macAddress => request(mqttBindingPath(macAddress)),
    saveMqttBinding: (macAddress, cabGuid) => request(mqttBindingPath(macAddress), 'PUT', { cabGuid }),
    bind: (macAddress, bootID, userGUID) => request(bindingPath(macAddress), 'POST', { bootID, userGUID }),
    unbind: macAddress => request(bindingPath(macAddress), 'DELETE'),
    getSessions: (userGUID, page, limit) => request(`${profilePath(userGUID)}/sessions`, 'GET', { page, limit }),
    getMessages: (userGUID, usageSessionID, page, limit) => request(
        `${profilePath(userGUID)}/sessions/${encodeURIComponent(usageSessionID)}/messages`, 'GET', { page, limit }
    )
};
