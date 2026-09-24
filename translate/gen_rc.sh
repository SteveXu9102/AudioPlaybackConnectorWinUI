#!/bin/sh
# Regenerates the compiled translation resources from the .po catalogs.
set -e

# Run from the directory that holds this script whichever directory it was started
# from; the paths below are relative to it.
cd "$(dirname "$0")"

PYTHON="${PYTHON:-python3}"
command -v "$PYTHON" >/dev/null 2>&1 || PYTHON=python
command -v "$PYTHON" >/dev/null 2>&1 || { echo "no Python interpreter found (tried $PYTHON)" >&2; exit 1; }

"$PYTHON" ./po2ymo.py ./source/zh_CN.po ./generated/zh_CN.ymo
"$PYTHON" ./po2ymo.py ./source/zh_TW.po ./generated/zh_TW.ymo

# A compile that silently did nothing would ship the previously committed catalogs.
for ymo in ./generated/zh_CN.ymo ./generated/zh_TW.ymo; do
    [ -s "$ymo" ] || { echo "the compiled catalog $ymo is missing or empty" >&2; exit 1; }
done

# .gitattributes forces *.rc to UTF-16LE with a BOM in the working tree, so the
# generated script has to be written with that encoding too.
"$PYTHON" - <<'PY'
content = '\r\n'.join([
    '#include "../../targetver.h"',
    '#include "windows.h"',
    'LANGUAGE LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED',
    '1 YMO "zh_CN.ymo"',
    'LANGUAGE LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL',
    '1 YMO "zh_TW.ymo"',
    '',
])
with open('./generated/translate.rc', 'wb') as handle:
    handle.write(b'\xff\xfe' + content.encode('utf-16-le'))
PY
