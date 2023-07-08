#!/usr/bin/env bash

SOURCES=$(find $(git rev-parse --show-toplevel) | egrep "\.(c|cxx|cpp|h|hpp)\$")

set -x

for file in ${SOURCES};
do
    clang-format-12 ${file} > expected-format
    diff -u -p --label="${file}" --label="expected coding style" ${file} expected-format
done
exit $(clang-format-12 --output-replacements-xml ${SOURCES} | egrep -c "</replacement>")
