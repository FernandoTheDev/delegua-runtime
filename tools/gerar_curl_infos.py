#!/usr/bin/env python3
"""Enumera getinfo gerenciáveis dos headers da toolchain, sem ponteiros/socket IDs."""

import os
from pathlib import Path
import re
import shlex
import subprocess

flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "libcurl"], text=True))
source = subprocess.check_output(
    [os.environ.get("CC", "clang"), *flags, "-E", "-P", "-include", "curl/curl.h", "-x", "c", "-"],
    input="",
    text=True,
)
items = re.findall(
    r"\bCURLINFO_(\w+)\s*=\s*(0x100000|0x200000|0x300000|0x600000|0x400000)\s*\+\s*\d+", source
)
assert items, "Não foi possível extrair CURLINFO dos headers"
rows = []
for name, kind in items:
    if kind == "0x400000" and name not in ("COOKIELIST", "SSL_ENGINES"):
        continue
    rows.append(f'{{"{name}", CURLINFO_{name}}},')
Path(".build/curl_infos.inc").write_text(
    "/* Gerado dos headers locais; não editar. */\n" + "\n".join(rows) + "\n"
)
