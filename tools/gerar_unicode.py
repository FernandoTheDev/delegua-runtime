#!/usr/bin/env python3
"""Gera tabelas estáticas de conversão Unicode. Python só é necessário para regenerar."""
from pathlib import Path
import unicodedata

ROOT = Path(__file__).resolve().parents[1]
lines = [f'/* Gerado por tools/gerar_unicode.py; Unicode {unicodedata.unidata_version}. Não editar. */',
         '#ifndef DELEGUA_UNICODE_TABLES_H', '#define DELEGUA_UNICODE_TABLES_H',
         'typedef struct { u32 codepoint; unsigned length; u32 mapped[3]; } unicode_case;',
         'typedef struct { u32 first, last; } unicode_range;']
for operation in ('upper', 'lower'):
    lines.append(f'static const unicode_case unicode_{operation}[] = {{')
    for number in range(0x110000):
        character = chr(number)
        result = getattr(character, operation)()
        if result != character:
            assert 1 <= len(result) <= 3
            mapped = ', '.join(f'0x{ord(c):x}U' for c in result)
            lines.append(f'    {{0x{number:x}U, {len(result)}, {{{mapped}}}}},')
    lines.append('};')
for name, predicate in (
    ('cased', lambda c: c.islower() or c.isupper() or c.istitle()),
    ('ignorable', lambda c: unicodedata.category(c) in ('Mn', 'Me', 'Cf', 'Lm', 'Sk')
     or c in "'.:\u00b7\u0387\u05f4\u2019\u2027"),
):
    lines.append(f'static const unicode_range unicode_{name}[] = {{')
    start = previous = None
    for number in range(0x110000):
        if predicate(chr(number)):
            if start is None:
                start = number
            elif number != previous + 1:
                lines.append(f'    {{0x{start:x}U, 0x{previous:x}U}},')
                start = number
            previous = number
    if start is not None:
        lines.append(f'    {{0x{start:x}U, 0x{previous:x}U}},')
    lines.append('};')
lines += ['#endif', '']
(ROOT / 'src' / 'unicode_tables.h').write_text('\n'.join(lines))
