/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "usbh_rndis_protocol.h"

static inline bool rndis_response_valid(const void *data, size_t received, uint32_t type,
                                        uint32_t request_id, size_t minimum)
{
    rndis_set_cmplt_t header;
    if (data == NULL || received < sizeof(header) || received < minimum) {
        return false;
    }
    memcpy(&header, data, sizeof(header));
    return header.MessageType == type && header.RequestId == request_id &&
           header.Status == RNDIS_STATUS_SUCCESS && header.MessageLength >= minimum &&
           header.MessageLength <= received;
}

static inline bool rndis_query_copy(const void *data, size_t received, uint32_t request_id,
                                   void *output, size_t capacity, size_t minimum, uint32_t *length)
{
    if (length == NULL) {
        return false;
    }
    *length = 0;
    if (output == NULL || !rndis_response_valid(data, received, REMOTE_NDIS_QUERY_CMPLT,
                                               request_id, sizeof(rndis_query_cmplt_t))) {
        return false;
    }
    rndis_query_cmplt_t header;
    memcpy(&header, data, sizeof(header));
    // RNDIS offsets are relative to RequestId, eight bytes into the message.
    const size_t base = sizeof(rndis_generic_msg_t);
    if (header.InformationBufferOffset < sizeof(header) - base ||
        header.InformationBufferOffset > header.MessageLength - base) {
        return false;
    }
    const size_t offset = base + header.InformationBufferOffset;
    if (header.InformationBufferLength < minimum || header.InformationBufferLength > capacity ||
        header.InformationBufferLength > header.MessageLength - offset) {
        return false;
    }
    memcpy(output, (const uint8_t *)data + offset, header.InformationBufferLength);
    *length = header.InformationBufferLength;
    return true;
}
