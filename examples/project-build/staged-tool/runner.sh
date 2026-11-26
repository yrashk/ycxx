#!/bin/sh
set -eu

printf '#pragma once\ninline constexpr int generated_score = 7;\n' > "$1"
