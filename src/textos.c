#include "interno.h"
#include "unicode_tables.h"

static bool unicode_in(const unicode_range *ranges, sz count, u32 cp) {
    sz low = 0, high = count;
    while (low < high) {
        sz mid = low + (high - low) / 2;
        if (cp < ranges[mid].first)
            high = mid;
        else if (cp > ranges[mid].last)
            low = mid + 1;
        else
            return true;
    }
    return false;
}
static bool cased(u32 cp) {
    return unicode_in(unicode_cased, sizeof(unicode_cased) / sizeof(*unicode_cased), cp);
}
static bool ignorable(u32 cp) {
    return unicode_in(unicode_ignorable, sizeof(unicode_ignorable) / sizeof(*unicode_ignorable),
                      cp);
}
static bool whitespace(u32 cp) {
    return (cp >= 9 && cp <= 13) || cp == 32 || cp == 0xa0 || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 || cp == 0x2029 || cp == 0x202f ||
           cp == 0x205f || cp == 0x3000 || cp == 0xfeff;
}
u32 rt_utf8_next(delegua_value text, sz *offset) {
    CHECK_ARG_TYPE(text.type, DELEGUA_T_TEXT, 1);
    if (*offset >= text.value.text.len)
        delegua_panic("Índice de texto inválido.");
    const unsigned char *p = (const unsigned char *)text.value.text.ptr;
    unsigned char lead = p[(*offset)++];
    if (lead < 128)
        return lead;
    unsigned count;
    u32 cp, minimum;
    if (lead >= 0xc2 && lead <= 0xdf) {
        count = 1;
        cp = lead & 31U;
        minimum = 128;
    } else if (lead >= 0xe0 && lead <= 0xef) {
        count = 2;
        cp = lead & 15U;
        minimum = 2048;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
        count = 3;
        cp = lead & 7U;
        minimum = 65536;
    } else {
        delegua_panic("Texto UTF-8 inválido.");
        return 0;
    }
    if (count > text.value.text.len - *offset)
        delegua_panic("Texto UTF-8 incompleto.");
    for (unsigned i = 0; i < count; ++i) {
        unsigned char c = p[(*offset)++];
        if ((c & 0xc0U) != 0x80U)
            delegua_panic("Texto UTF-8 inválido.");
        cp = (cp << 6) | (c & 63U);
    }
    if (cp < minimum || cp > 0x10ffffU || (cp >= 0xd800U && cp <= 0xdfffU))
        delegua_panic("Caractere Unicode inválido.");
    return cp;
}
sz rt_text_count(delegua_value text) {
    sz offset = 0, n = 0;
    while (offset < text.value.text.len) {
        rt_utf8_next(text, &offset);
        ++n;
    }
    return n;
}
sz rt_text_offset(delegua_value text, sz index) {
    sz offset = 0;
    for (sz i = 0; i < index; ++i)
        rt_utf8_next(text, &offset);
    return offset;
}
static void encode(u32 c, char *out, sz *size) {
    if (c < 128)
        out[(*size)++] = (char)c;
    else if (c < 2048) {
        out[(*size)++] = (char)(0xc0U | (c >> 6));
        out[(*size)++] = (char)(0x80U | (c & 63U));
    } else if (c < 65536) {
        out[(*size)++] = (char)(0xe0U | (c >> 12));
        out[(*size)++] = (char)(0x80U | ((c >> 6) & 63U));
        out[(*size)++] = (char)(0x80U | (c & 63U));
    } else {
        out[(*size)++] = (char)(0xf0U | (c >> 18));
        out[(*size)++] = (char)(0x80U | ((c >> 12) & 63U));
        out[(*size)++] = (char)(0x80U | ((c >> 6) & 63U));
        out[(*size)++] = (char)(0x80U | (c & 63U));
    }
}
delegua_value rt_text_case(delegua_value text, bool upper) {
    CHECK_ARG_TYPE(text.type, DELEGUA_T_TEXT, 1);
    if (text.value.text.len > (SIZE_MAX - 1) / 12)
        delegua_panic("Texto grande demais.");
    char *out = rt_alloc(text.value.text.len * 12 + 1, 1);
    sz offset = 0, size = 0;
    bool previous_cased = false;
    const unicode_case *table = upper ? unicode_upper : unicode_lower;
    sz count = upper ? sizeof(unicode_upper) / sizeof(*unicode_upper)
                     : sizeof(unicode_lower) / sizeof(*unicode_lower);
    while (offset < text.value.text.len) {
        u32 cp = rt_utf8_next(text, &offset);
        if (!upper && cp == 0x3a3U && previous_cased) {
            sz look = offset;
            bool following = false;
            while (look < text.value.text.len) {
                u32 next = rt_utf8_next(text, &look);
                if (ignorable(next))
                    continue;
                following = cased(next);
                break;
            }
            if (!following) {
                encode(0x3c2U, out, &size);
                previous_cased = true;
                continue;
            }
        }
        sz low = 0, high = count;
        const unicode_case *mapping = NULL;
        while (low < high) {
            sz mid = low + (high - low) / 2;
            if (cp < table[mid].codepoint)
                high = mid;
            else if (cp > table[mid].codepoint)
                low = mid + 1;
            else {
                mapping = &table[mid];
                break;
            }
        }
        if (mapping) {
            for (unsigned i = 0; i < mapping->length; ++i)
                encode(mapping->mapped[i], out, &size);
        } else
            encode(cp, out, &size);
        if (!ignorable(cp))
            previous_cased = cased(cp);
    }
    return create_text_len(out, size);
}
delegua_value rt_text_trim(delegua_value text, int mode) {
    CHECK_ARG_TYPE(text.type, DELEGUA_T_TEXT, 1);
    sz offset = 0, first = text.value.text.len, last = 0;
    while (offset < text.value.text.len) {
        sz begin = offset;
        u32 cp = rt_utf8_next(text, &offset);
        if (!whitespace(cp)) {
            if (first == text.value.text.len)
                first = begin;
            last = offset;
        }
    }
    if (mode == 1)
        last = text.value.text.len;
    if (mode == 2)
        first = 0;
    if (last < first)
        last = first;
    return create_text_len(text.value.text.ptr + first, last - first);
}
/* Busca por bytes é equivalente à busca de sequências de pontos de código em UTF-8 válido. */
i64 rt_text_find(delegua_value text, delegua_value needle) {
    CHECK_ARG_TYPE(text.type, DELEGUA_T_TEXT, 1);
    CHECK_ARG_TYPE(needle.type, DELEGUA_T_TEXT, 2);
    sz offset = 0, index = 0;
    while (true) {
        if (needle.value.text.len <= text.value.text.len - offset &&
            !memcmp(text.value.text.ptr + offset, needle.value.text.ptr, needle.value.text.len))
            return (i64)index;
        if (offset == text.value.text.len)
            break;
        rt_utf8_next(text, &offset);
        ++index;
    }
    return -1;
}
delegua_value rt_text_split(delegua_value text, delegua_value delimiter) {
    CHECK_ARG_TYPE(text.type, DELEGUA_T_TEXT, 1);
    CHECK_ARG_TYPE(delimiter.type, DELEGUA_T_TEXT, 2);
    delegua_value out = create_vetor();
    sz start = 0, offset = 0;
    if (!delimiter.value.text.len) {
        while (offset < text.value.text.len) {
            start = offset;
            rt_utf8_next(text, &offset);
            delegua_vetor_adicionar(&out,
                                    create_text_len(text.value.text.ptr + start, offset - start));
        }
        return out;
    }
    while (offset <= text.value.text.len) {
        if (delimiter.value.text.len <= text.value.text.len - offset &&
            !memcmp(text.value.text.ptr + offset, delimiter.value.text.ptr,
                    delimiter.value.text.len)) {
            delegua_vetor_adicionar(&out,
                                    create_text_len(text.value.text.ptr + start, offset - start));
            offset += delimiter.value.text.len;
            start = offset;
        } else {
            if (offset == text.value.text.len)
                break;
            rt_utf8_next(text, &offset);
        }
    }
    delegua_vetor_adicionar(
        &out, create_text_len(text.value.text.ptr + start, text.value.text.len - start));
    return out;
}
delegua_value rt_text_replace(delegua_value text, delegua_value needle, delegua_value replacement) {
    CHECK_ARG_TYPE(replacement.type, DELEGUA_T_TEXT, 3);
    i64 found = rt_text_find(text, needle);
    if (found < 0)
        return text;
    sz pos = rt_text_offset(text, (sz)found);
    delegua_value a = create_text_len(text.value.text.ptr, pos),
                  b = create_text_len(text.value.text.ptr + pos + needle.value.text.len,
                                      text.value.text.len - pos - needle.value.text.len);
    return create_text_concat(create_text_concat(a, replacement), b);
}
