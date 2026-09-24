#!/usr/bin/env python3
"""Compile a gettext .po catalog into the compact "YMO" resource that
AudioPlaybackConnectorWinUI embeds.

The format is deliberately tiny: a 16-bit entry count, followed by that many
(hash, offset) pairs, followed by the UTF-16LE translations. The hash is FNV-1a
over the UTF-16LE bytes of the English source string, optionally prefixed with
a msgctxt and a 0x04 separator. See I18n.hpp for the reader.

This script replaces the previous dependency on the ``translate-toolkit``
package (and its git checkout), so the catalogs can be regenerated with nothing
but a stock Python 3 interpreter.
"""

import re
import sys

FNV1_32_INIT = 0x811c9dc5
FNV_32_PRIME = 0x01000193

_ESCAPES = {'n': '\n', 't': '\t', 'r': '\r', '"': '"', '\\': '\\'}


def fnv1a_32(data, hval=FNV1_32_INIT):
    for byte in data:
        hval ^= byte
        hval = (hval * FNV_32_PRIME) & 0xffffffff
    return hval


def _unescape(text):
    """Resolve the C-style escapes a .po quoted string may contain."""
    out = []
    i = 0
    while i < len(text):
        char = text[i]
        if char == '\\' and i + 1 < len(text):
            nxt = text[i + 1]
            out.append(_ESCAPES.get(nxt, nxt))
            i += 2
        else:
            out.append(char)
            i += 1
    return ''.join(out)


def parse_po(text):
    """Parse a .po file into a list of {msgctxt, msgid, msgstr, fuzzy} dicts."""
    entries = []
    entry = None
    field = None

    for raw in text.splitlines():
        line = raw.strip()

        if not line:
            if entry is not None and 'msgid' in entry:
                entries.append(entry)
            entry = None
            field = None
            continue

        if line.startswith('#'):
            if entry is None:
                entry = {'fuzzy': False}
            if line.startswith('#,') and 'fuzzy' in line:
                entry['fuzzy'] = True
            continue

        if entry is None:
            entry = {'fuzzy': False}

        match = re.match(r'^(msgctxt|msgid|msgstr)\s+"(.*)"\s*$', line)
        if match:
            field = match.group(1)
            entry[field] = _unescape(match.group(2))
            continue

        match = re.match(r'^"(.*)"\s*$', line)
        if match and field:
            entry[field] = entry.get(field, '') + _unescape(match.group(1))
            continue

        # Obsolete entries (#~) and anything else are ignored.

    if entry is not None and 'msgid' in entry:
        entries.append(entry)

    return entries


def po2ymo(infile, outfile, includefuzzy=False, encoding='utf-16le'):
    text = infile.read().decode('utf-8-sig')

    units = {}
    for entry in parse_po(text):
        source = entry.get('msgid', '')
        target = entry.get('msgstr', '')

        # The header entry has an empty msgid; untranslated entries have no target.
        if not source or not target:
            continue
        if entry.get('fuzzy') and not includefuzzy:
            continue

        context = entry.get('msgctxt')
        if context:
            source = context + '\u0004' + source

        key = fnv1a_32(source.encode(encoding))
        if key in units:
            raise SystemExit(f'hash collision: {source!r} shares {key:#010x} with another string')
        units[key] = target.encode(encoding) + bytes(2)

    if len(units) > 0xFFFF:
        raise SystemExit(f'too many entries for a 16-bit count: {len(units)}')
    outfile.write(len(units).to_bytes(2, 'little'))

    offset = 2 + len(units) * (4 + 2)
    for hash_value, data in units.items():
        if offset > 0xFFFF:
            raise SystemExit('the string table starts past the 16-bit offset limit')
        outfile.write(hash_value.to_bytes(4, 'little'))
        outfile.write(offset.to_bytes(2, 'little'))
        offset += len(data)

    for data in units.values():
        outfile.write(data)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        print('usage: po2ymo.py <infile.po> <outfile.ymo>')
        sys.exit(2)

    with open(sys.argv[1], 'rb') as infile, open(sys.argv[2], 'wb') as outfile:
        po2ymo(infile, outfile)
