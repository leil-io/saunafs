#!/usr/bin/env bash

tool=$(basename $0)

${tool/saunafs/saunafs } "$@"
