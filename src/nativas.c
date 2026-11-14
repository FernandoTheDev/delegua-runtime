#include "interno.h"
#include <time.h>

typedef struct {
    char *data;
    sz size, cap;
} text_builder;
static void append_text(text_builder *b, const char *text, sz size) {
    if (size > SIZE_MAX - b->size - 1)
        delegua_panic("Texto grande demais.");
    sz required = b->size + size + 1;
    if (required > b->cap) {
        sz cap = b->cap ? b->cap : 128;
        while (cap < required) {
            if (cap > SIZE_MAX / 2) {
                cap = required;
                break;
            }
            cap *= 2;
        }
        b->data = GC_REALLOC(b->data, cap);
        CHECK_NULL(b->data);
        b->cap = cap;
    }
    memcpy(b->data + b->size, text, size);
    b->size += size;
    b->data[b->size] = '\0';
}
static void literal(text_builder *b, const char *text) { append_text(b, text, strlen(text)); }
static void render_value(text_builder *b, delegua_value value, void **ancestors, unsigned depth) {
    if (depth >= 128) {
        literal(b, "...");
        return;
    }
    if (value.type == DELEGUA_T_TEXT) {
        append_text(b, value.value.text.ptr, value.value.text.len);
        return;
    }
    if (value.type == DELEGUA_T_NULO) {
        literal(b, "nulo");
        return;
    }
    if (value.type == DELEGUA_T_BOOL || value.type == DELEGUA_T_NUM ||
        value.type == DELEGUA_T_REAL) {
        delegua_value text = delegua_op_cast(value, DELEGUA_T_TEXT);
        append_text(b, text.value.text.ptr, text.value.text.len);
        return;
    }
    void *identity = value.type == DELEGUA_T_VETOR || value.type == DELEGUA_T_TUPLA
                         ? (void *)value.value.vetor
                         : value.value.ptr;
    for (unsigned i = 0; i < depth; ++i)
        if (ancestors[i] == identity) {
            literal(b, "<ciclo>");
            return;
        }
    ancestors[depth] = identity;
    if (value.type == DELEGUA_T_VETOR || value.type == DELEGUA_T_TUPLA) {
        literal(b, value.type == DELEGUA_T_TUPLA ? "(" : "[");
        for (sz i = 0; i < value.value.vetor->size; ++i) {
            if (i)
                literal(b, ", ");
            render_value(b, value.value.vetor->values[i], ancestors, depth + 1);
        }
        literal(b, value.type == DELEGUA_T_TUPLA ? ")" : "]");
        return;
    }
    if (value.type == DELEGUA_T_MAP) {
        literal(b, "{");
        sz size = delegua_tamanho(value);
        for (sz i = 0; i < size; ++i) {
            if (i)
                literal(b, ", ");
            delegua_value pair = delegua_iterar(value, i);
            render_value(b, pair.value.vetor->values[0], ancestors, depth + 1);
            literal(b, ": ");
            render_value(b, pair.value.vetor->values[1], ancestors, depth + 1);
        }
        literal(b, "}");
        return;
    }
    if (value.type == DELEGUA_T_OBJETO) {
        if (delegua_objeto_metodo_definido(value, "paraTexto")) {
            delegua_value result =
                delegua_chamar(delegua_propriedade_em(value, "paraTexto", create_nulo()), 0, NULL);
            CHECK_ARG_TYPE(result.type, DELEGUA_T_TEXT, 1);
            append_text(b, result.value.text.ptr, result.value.text.len);
            return;
        }
        if (delegua_instancia_de(value, delegua_classe_excecao())) {
            render_value(b, delegua_objeto_obter(value, "mensagem"), ancestors, depth + 1);
            return;
        }
        literal(b, "<");
        delegua_value name = delegua_tipo_objeto(value);
        append_text(b, name.value.text.ptr, name.value.text.len);
        literal(b, " instância>");
        return;
    }
    literal(b, "<");
    literal(b, GET_TYPE(value));
    literal(b, ">");
}
delegua_value delegua_para_texto(delegua_value value) {
    text_builder builder = {0};
    void *ancestors[128] = {0};
    render_value(&builder, value, ancestors, 0);
    return create_text_len(builder.data ? builder.data : "", builder.size);
}

delegua_value delegua_para_tupla(delegua_value value) {
    CHECK_ARG_TYPE(value.type, DELEGUA_T_VETOR, 1);
    CHECK_NULL(value.value.vetor);
    sz n = value.value.vetor->size;
    if (n < 2 || n > 10)
        delegua_panic("Tupla exige de 2 a 10 elementos.");
    return create_tupla(n, value.value.vetor->values);
}
delegua_value delegua_leia(delegua_value prompt) {
    CHECK_ARG_TYPE(prompt.type, DELEGUA_T_TEXT, 1);
    if (fwrite(prompt.value.text.ptr, 1, prompt.value.text.len, stdout) != prompt.value.text.len ||
        fflush(stdout))
        delegua_panic("Falha ao escrever mensagem de entrada.");
    sz size = 0, cap = 128;
    char *buffer = rt_alloc(cap, 1);
    int ch;
    while ((ch = fgetc(stdin)) != EOF && ch != '\n') {
        if (size == cap) {
            if (cap > SIZE_MAX / 2)
                delegua_panic("Entrada grande demais.");
            cap *= 2;
            buffer = GC_REALLOC(buffer, cap);
            CHECK_NULL(buffer);
        }
        buffer[size++] = (char)ch;
    }
    if (ferror(stdin))
        delegua_panic("Falha de leitura da entrada.");
    if (size && buffer[size - 1] == '\r')
        --size;
    return create_text_len(buffer, size);
}
static void seed(void) {
    static bool initialized = false;
    if (!initialized) {
        srand((unsigned)time(NULL));
        initialized = true;
    }
}
delegua_value delegua_aleatorio(void) {
    seed();
    return create_real((f64)rand() / ((f64)RAND_MAX + 1.0));
}
delegua_value delegua_aleatorio_entre(delegua_value min, delegua_value max) {
    CHECK_ARG_TYPE(min.type, DELEGUA_T_NUM, 1);
    CHECK_ARG_TYPE(max.type, DELEGUA_T_NUM, 2);
    if (min.value.num >= max.value.num)
        delegua_panic("Intervalo aleatório vazio ou invertido.");
    /* Rejeição elimina viés e evita overflow assinado mesmo em intervalos amplos. */
    seed();
    u64 span = (u64)max.value.num - (u64)min.value.num;
    u64 value, limit = (u64)(-span) % span;
    do {
        value = 0;
        for (sz i = 0; i < 8; ++i)
            value = (value << 8) | ((u64)rand() & 255U);
    } while (value < limit);
    u64 bits = (u64)min.value.num + value % span;
    i64 result;
    memcpy(&result, &bits, sizeof(result));
    return create_num(result);
}
