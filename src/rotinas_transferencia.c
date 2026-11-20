/* Copyright 2026 Fernando the Dev. SPDX-License-Identifier: Apache-2.0 */
#include "rotinas_transferencia.h"

/* Representação local, sem serializar ponteiros nem depender de aliasing de
 * delegua_value. offsets são validados antes de produzir qualquer view. */
typedef struct {
    rt_transfer_kind kind;
    union {
        bool boolean;
        i64 integer;
        f64 real;
        struct {
            sz offset, length;
        } text;
    } value;
} transfer_item;
struct rt_transfer_packet {
    sz count, bytes;
    transfer_item items[];
};
enum { MAX_ITEMS = 4096, MAX_BYTES = 16 * 1024 * 1024 };

rt_transfer_status rt_transfer_pack_views(sz count, const rt_transfer_view *values,
                                          rt_transfer_packet **output) {
    if (!output)
        return RT_TRANSFER_INVALID;
    *output = NULL;
    if (count && !values)
        return RT_TRANSFER_INVALID;
    if (count > MAX_ITEMS)
        return RT_TRANSFER_LIMIT;
    sz bytes = sizeof(rt_transfer_packet) + count * sizeof(transfer_item);
    for (sz i = 0; i < count; i++) {
        switch (values[i].kind) {
        case RT_TRANSFER_NULL:
        case RT_TRANSFER_BOOL:
        case RT_TRANSFER_INT:
        case RT_TRANSFER_REAL:
            break;
        case RT_TRANSFER_TEXT:
            if (values[i].value.text.length && !values[i].value.text.bytes)
                return RT_TRANSFER_INVALID;
            if (values[i].value.text.length > MAX_BYTES - bytes)
                return RT_TRANSFER_LIMIT;
            bytes += values[i].value.text.length;
            break;
        default:
            return RT_TRANSFER_UNSUPPORTED;
        }
    }
    rt_transfer_packet *packet = calloc(1, bytes);
    if (!packet)
        return RT_TRANSFER_MEMORY;
    packet->count = count;
    packet->bytes = bytes;
    sz offset = sizeof(*packet) + count * sizeof(transfer_item);
    for (sz i = 0; i < count; i++) {
        transfer_item *item = &packet->items[i];
        switch (values[i].kind) {
        case RT_TRANSFER_NULL:
            item->kind = RT_TRANSFER_NULL;
            break;
        case RT_TRANSFER_BOOL:
            item->kind = RT_TRANSFER_BOOL;
            item->value.boolean = values[i].value.boolean;
            break;
        case RT_TRANSFER_INT:
            item->kind = RT_TRANSFER_INT;
            item->value.integer = values[i].value.integer;
            break;
        case RT_TRANSFER_REAL:
            item->kind = RT_TRANSFER_REAL;
            item->value.real = values[i].value.real;
            break;
        case RT_TRANSFER_TEXT:
            item->kind = RT_TRANSFER_TEXT;
            item->value.text.offset = offset;
            item->value.text.length = values[i].value.text.length;
            if (values[i].value.text.length)
                memcpy((unsigned char *)packet + offset, values[i].value.text.bytes,
                       values[i].value.text.length);
            offset += values[i].value.text.length;
            break;
        default:
            free(packet);
            return RT_TRANSFER_UNSUPPORTED;
        }
    }
    *output = packet;
    return RT_TRANSFER_OK;
}
rt_transfer_status rt_transfer_pack(sz count, const delegua_value *values,
                                    rt_transfer_packet **output) {
    if (!output)
        return RT_TRANSFER_INVALID;
    *output = NULL;
    if (count > MAX_ITEMS)
        return RT_TRANSFER_LIMIT;
    if (!count)
        return rt_transfer_pack_views(0, NULL, output);
    if (!values)
        return RT_TRANSFER_INVALID;
    rt_transfer_view *views = calloc(count, sizeof(*views));
    if (!views)
        return RT_TRANSFER_MEMORY;
    for (sz i = 0; i < count; i++) {
        switch (values[i].type) {
        case DELEGUA_T_NULO:
            views[i].kind = RT_TRANSFER_NULL;
            break;
        case DELEGUA_T_BOOL:
            views[i].kind = RT_TRANSFER_BOOL;
            views[i].value.boolean = values[i].value.b1;
            break;
        case DELEGUA_T_NUM:
            views[i].kind = RT_TRANSFER_INT;
            views[i].value.integer = values[i].value.num;
            break;
        case DELEGUA_T_REAL:
            views[i].kind = RT_TRANSFER_REAL;
            views[i].value.real = values[i].value.real;
            break;
        case DELEGUA_T_TEXT:
            views[i].kind = RT_TRANSFER_TEXT;
            views[i].value.text.bytes = (const unsigned char *)values[i].value.text.ptr;
            views[i].value.text.length = values[i].value.text.len;
            break;
        default:
            free(views);
            return RT_TRANSFER_UNSUPPORTED;
        }
    }
    rt_transfer_status status = rt_transfer_pack_views(count, views, output);
    free(views);
    return status;
}
sz rt_transfer_count(const rt_transfer_packet *packet) { return packet ? packet->count : 0; }
rt_transfer_status rt_transfer_get(const rt_transfer_packet *packet, sz index,
                                   rt_transfer_view *output) {
    if (!packet || !output || index >= packet->count)
        return RT_TRANSFER_INVALID;
    const transfer_item *item = &packet->items[index];
    memset(output, 0, sizeof(*output));
    output->kind = item->kind;
    switch (item->kind) {
    case RT_TRANSFER_NULL:
        break;
    case RT_TRANSFER_BOOL:
        output->value.boolean = item->value.boolean;
        break;
    case RT_TRANSFER_INT:
        output->value.integer = item->value.integer;
        break;
    case RT_TRANSFER_REAL:
        output->value.real = item->value.real;
        break;
    case RT_TRANSFER_TEXT:
        if (item->value.text.offset > packet->bytes ||
            item->value.text.length > packet->bytes - item->value.text.offset)
            return RT_TRANSFER_INVALID;
        output->value.text.bytes = (const unsigned char *)packet + item->value.text.offset;
        output->value.text.length = item->value.text.length;
        break;
    default:
        return RT_TRANSFER_INVALID;
    }
    return RT_TRANSFER_OK;
}
delegua_value rt_transfer_materialize_at(const rt_transfer_packet *packet, sz i) {
    rt_transfer_view view;
    if (rt_transfer_get(packet, i, &view) != RT_TRANSFER_OK)
        delegua_panic("Valor transferido inválido.");
    delegua_value value = create_nulo();
    switch (view.kind) {
    case RT_TRANSFER_NULL:
        break;
    case RT_TRANSFER_BOOL:
        value = create_bool(view.value.boolean);
        break;
    case RT_TRANSFER_INT:
        value = create_num(view.value.integer);
        break;
    case RT_TRANSFER_REAL:
        value = create_real(view.value.real);
        break;
    case RT_TRANSFER_TEXT:
        value = create_text_len((const char *)view.value.text.bytes, view.value.text.length);
        break;
    }
    return value;
}
delegua_value rt_transfer_materialize(const rt_transfer_packet *packet) {
    if (!packet)
        delegua_panic("Pacote de transferência inválido.");
    delegua_value result = create_vetor();
    for (sz i = 0; i < packet->count; i++) {
        delegua_value value = rt_transfer_materialize_at(packet, i);
        delegua_vetor_adicionar(&result, value);
    }
    return result;
}
void rt_transfer_free(rt_transfer_packet *packet) { free(packet); }
