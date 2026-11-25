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

typedef struct {
    uintptr_t addr;
    sz byte_len;
    uint64_t gc_no;
    u64 head_sample;
    u64 mid_sample;
    u64 tail_sample;
    sz char_count;
    sz first_non_ascii;
    sz last_index;
    sz last_offset;
    bool is_ascii;
} utf8_cache_entry;

#define UTF8_CACHE_SLOTS 8
static DELEGUA_TLS utf8_cache_entry utf8_cache[UTF8_CACHE_SLOTS];
static DELEGUA_TLS unsigned utf8_cache_hand;
static DELEGUA_TLS unsigned utf8_cache_mru;

static inline void utf8_sample(const char *ptr, sz len, u64 *head, u64 *mid, u64 *tail) {
    u64 h = 0, m = 0, t = 0;
    if (len >= 8) {
        memcpy(&h, ptr, 8);
        memcpy(&t, ptr + len - 8, 8);
        if (len >= 24)
            memcpy(&m, ptr + len / 2 - 4, 8);
    } else if (len > 0) {
        memcpy(&h, ptr, len);
        t = h;
    }
    *head = h;
    *mid = m;
    *tail = t;
}

static utf8_cache_entry *utf8_cache_get(delegua_value text) {
    CHECK_ARG_TYPE(text.type, DELEGUA_T_TEXT, 1);
    const char *ptr = text.value.text.ptr;
    sz len = text.value.text.len;
    uintptr_t addr = (uintptr_t)ptr;
    uint64_t gc_no = cgd_gc_cycle_count();
    u64 head = 0, mid = 0, tail = 0;
    utf8_sample(ptr, len, &head, &mid, &tail);

    utf8_cache_entry *mru = &utf8_cache[utf8_cache_mru];
    if (mru->addr == addr && mru->byte_len == len && mru->gc_no == gc_no &&
        mru->head_sample == head && mru->mid_sample == mid && mru->tail_sample == tail)
        return mru;

    for (unsigned i = 0; i < UTF8_CACHE_SLOTS; ++i) {
        if (i == utf8_cache_mru)
            continue;
        utf8_cache_entry *e = &utf8_cache[i];
        if (e->addr == addr && e->byte_len == len && e->gc_no == gc_no && e->head_sample == head &&
            e->mid_sample == mid && e->tail_sample == tail) {
            utf8_cache_mru = i;
            return e;
        }
    }

    sz offset = 0, n = 0, first_non_ascii = ((sz)-1);
    const unsigned char *p = (const unsigned char *)ptr;
    while (offset < len) {
        if (p[offset] < 128) {
            while (offset + 8 <= len) {
                u64 word;
                memcpy(&word, p + offset, 8);
                if ((word & 0x8080808080808080ULL) == 0) {
                    offset += 8;
                    n += 8;
                } else {
                    break;
                }
            }
            while (offset < len && p[offset] < 128) {
                ++offset;
                ++n;
            }
        } else {
            if (first_non_ascii == ((sz)-1))
                first_non_ascii = n;
            rt_utf8_next(text, &offset);
            ++n;
        }
    }
    if (first_non_ascii == ((sz)-1))
        first_non_ascii = n;

    unsigned slot_idx = utf8_cache_hand;
    utf8_cache_entry *slot = &utf8_cache[slot_idx];
    utf8_cache_hand = (slot_idx + 1) % UTF8_CACHE_SLOTS;
    utf8_cache_mru = slot_idx;
    slot->addr = addr;
    slot->byte_len = len;
    slot->gc_no = gc_no;
    slot->head_sample = head;
    slot->mid_sample = mid;
    slot->tail_sample = tail;
    slot->char_count = n;
    slot->first_non_ascii = first_non_ascii;
    slot->last_index = 0;
    slot->last_offset = 0;
    slot->is_ascii = (n == len);
    return slot;
}

sz rt_text_count(delegua_value text) {
    CHECK_ARG_TYPE(text.type, DELEGUA_T_TEXT, 1);
    if (text.value.text.len == 0)
        return 0;
    if (text.value.text.len == 1 && (unsigned char)text.value.text.ptr[0] < 128)
        return 1;
    return utf8_cache_get(text)->char_count;
}

sz rt_text_offset(delegua_value text, sz index) {
    CHECK_ARG_TYPE(text.type, DELEGUA_T_TEXT, 1);
    if (index == 0)
        return 0;
    utf8_cache_entry *e = utf8_cache_get(text);
    if (index <= e->first_non_ascii)
        return index <= e->byte_len ? index : e->byte_len;
    if (index >= e->char_count)
        return e->byte_len;
    if (index == e->last_index)
        return e->last_offset;
    const unsigned char *p = (const unsigned char *)text.value.text.ptr;
    sz offset, cur;
    bool forward;
    if (index > e->last_index) {
        sz start_idx = e->last_index >= e->first_non_ascii ? e->last_index : e->first_non_ascii;
        sz start_off = e->last_index >= e->first_non_ascii ? e->last_offset : e->first_non_ascii;
        sz fwd = index - start_idx;
        sz bwd = e->char_count - index;
        if (bwd < fwd) {
            cur = e->char_count;
            offset = e->byte_len;
            forward = false;
        } else {
            cur = start_idx;
            offset = start_off;
            forward = true;
        }
    } else {
        sz fwd = index - e->first_non_ascii;
        sz bwd = e->last_index - index;
        if (bwd < fwd) {
            cur = e->last_index;
            offset = e->last_offset;
            forward = false;
        } else {
            cur = e->first_non_ascii;
            offset = e->first_non_ascii;
            forward = true;
        }
    }
    if (forward) {
        for (; cur < index; ++cur) {
            unsigned char lead = p[offset++];
            if (lead < 128)
                continue;
            if (lead < 0xE0U)
                offset += 1;
            else if (lead < 0xF0U)
                offset += 2;
            else
                offset += 3;
        }
    } else {
        for (; cur > index; --cur) {
            --offset;
            while (offset > 0 && (p[offset] & 0xC0U) == 0x80U)
                --offset;
        }
    }
    e->last_index = index;
    e->last_offset = offset;
    return offset;
}

delegua_value rt_text_char_at(delegua_value text, i64 raw_index) {
    utf8_cache_entry *e = utf8_cache_get(text);
    sz size = e->char_count;
    if (size > INT64_MAX)
        delegua_panic("Coleção grande demais.");
    if (raw_index < 0)
        raw_index += (i64)size;
    if (raw_index < 0 || (u64)raw_index >= size)
        delegua_panic("Índice fora do intervalo.");
    sz idx = (sz)raw_index;
    if (idx < e->first_non_ascii)
        return create_text_len(text.value.text.ptr + idx, 1);
    sz a = rt_text_offset(text, idx);
    unsigned char lead = (unsigned char)text.value.text.ptr[a];
    sz clen = lead < 128 ? 1 : lead < 0xE0U ? 2 : lead < 0xF0U ? 3 : 4;
    return create_text_len(text.value.text.ptr + a, clen);
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
    char *out = cgd_gc_alloc_atomic(text.value.text.len * 12 + 1);
    CHECK_NULL(out);
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
