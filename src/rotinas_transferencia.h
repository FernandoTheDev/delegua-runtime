/* Copyright 2026 Fernando the Dev. SPDX-License-Identifier: Apache-2.0 */
#ifndef CGD_ROTINAS_TRANSFERENCIA_H
#define CGD_ROTINAS_TRANSFERENCIA_H
#include "bytes_interno.h"
#include "delegua_rt.h"

/* Preparação interna das rotinas: não é uma ABI estrangeira da linguagem.
 * Pacotes têm propriedade exclusiva e não contêm referências ao heap do GC.
 * Conversões entre pacotes e valores gerenciados exigem a thread principal.
 * Workers podem consultar pacotes e construir resultados a partir de views. */
typedef struct rt_transfer_packet rt_transfer_packet;
typedef enum {
    RT_TRANSFER_OK,
    RT_TRANSFER_UNSUPPORTED,
    RT_TRANSFER_LIMIT,
    RT_TRANSFER_MEMORY,
    RT_TRANSFER_INVALID,
    RT_TRANSFER_IO,
    RT_TRANSFER_EXCEPTION
} rt_transfer_status;
typedef enum {
    RT_TRANSFER_NULL,
    RT_TRANSFER_BOOL,
    RT_TRANSFER_INT,
    RT_TRANSFER_REAL,
    RT_TRANSFER_TEXT,
    RT_TRANSFER_BYTES,
    RT_TRANSFER_IMMUTABLE_BYTES
} rt_transfer_kind;
typedef struct {
    rt_transfer_kind kind;
    union {
        bool boolean;
        i64 integer;
        f64 real;
        struct {
            const unsigned char *bytes;
            sz length;
        } text;
        rt_shared_bytes *shared_bytes;
    } value;
} rt_transfer_view;
/* Falhas devolvem status e deixam *output NULL; nunca lançam Excecao.
 * Aceita somente nulo, lógico, inteiro, real e cópia de texto (inclusive NUL).
 * Nenhuma conversão implícita ou travessia de objetos/coleções. */
/* Construção a partir de valores nativos, permitida em workers. */
rt_transfer_status rt_transfer_pack_views(sz count, const rt_transfer_view *values,
                                          rt_transfer_packet **output);
rt_transfer_status rt_transfer_pack(sz count, const delegua_value *values,
                                    rt_transfer_packet **output);
sz rt_transfer_count(const rt_transfer_packet *packet);
/* A view de texto só é válida enquanto seu pacote continuar vivo. */
rt_transfer_status rt_transfer_get(const rt_transfer_packet *packet, sz index,
                                   rt_transfer_view *output);
/* Pode lançar em alocação; somente na thread principal, após obter propriedade. */
delegua_value rt_transfer_materialize_at(const rt_transfer_packet *packet, sz index);
delegua_value rt_transfer_materialize(const rt_transfer_packet *packet);
/* Exclusivamente pelo proprietário, sem leitores simultâneos. */
void rt_transfer_free(rt_transfer_packet *packet);
#endif
