/* Copyright 2026 Fernando the Dev. SPDX-License-Identifier: Apache-2.0 */
#include "rotinas_reactor.h"

/* CRC-32/ISO-HDLC sobre bytes, inclusive NUL. O worker só toca o pacote. */
static rt_transfer_status crc32_worker(const rt_pool_job *job, rt_transfer_packet **packet) {
    rt_transfer_view input;
    if (rt_transfer_get(*packet, 0, &input) != RT_TRANSFER_OK || input.kind != RT_TRANSFER_TEXT)
        return RT_TRANSFER_INVALID;
    uint32_t crc = UINT32_MAX;
    for (sz i = 0; i < input.value.text.length; i++) {
        if ((i % 4096 == 0) && rt_pool_cancelled(job))
            return RT_TRANSFER_OK;
        crc ^= input.value.text.bytes[i];
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ ((crc & 1U) ? UINT32_C(0xedb88320) : 0U);
    }
    rt_transfer_view output = {.kind = RT_TRANSFER_INT, .value.integer = crc ^ UINT32_MAX};
    rt_transfer_packet *result;
    rt_transfer_status status = rt_transfer_pack_views(1, &output, &result);
    if (status == RT_TRANSFER_OK) {
        rt_transfer_free(*packet);
        *packet = result;
    }
    return status;
}
delegua_value delegua_rotina_crc32(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_TEXT, 1);
    return (delegua_value){.type = DELEGUA_T_PROMESSA,
                           .value.ptr = rt_routine_start(crc32_worker, argc, argv)};
}
