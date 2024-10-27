#!/usr/bin/env bash

tool="$(basename $0)"
exec saunafs "${tool#sfs}" "$@"
