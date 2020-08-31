#!/usr/bin/env bash

SOURCES=$(find $(git rev-parse --show-toplevel) | egrep "\.(c|h)\$")

set -x

exit $(clang-format --output-replacements-xml ${SOURCES} | egrep -c "</replacement>")
