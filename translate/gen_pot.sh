#!/bin/sh
# Regenerates translate/source/messages.pot from the translatable strings in the
# C++ sources. Requires GNU gettext (xgettext) to be installed.
set -e

# Run from the repository root whichever directory the script was started from.
cd "$(dirname "$0")/.."

# Every C++ source rather than a hand maintained list: a string added to a file
# that is not on the list never reaches a translator.
FILES=$(find . \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) \
    -not -path './Generated Files/*' \
    -not -path './obj/*' \
    -not -path './x64/*' \
    -not -path './ARM64/*' \
    -not -path './.vs/*' | sort)

COPYRIGHT_HOLDER="Richard Yu <yurichard3839@gmail.com>"
PKG_NAME="AudioPlaybackConnectorWinUI"

xgettext -o translate/source/messages.pot --c++ --add-comments=/ --keyword=_ --copyright-holder="$COPYRIGHT_HOLDER" --package-name="$PKG_NAME" $FILES
