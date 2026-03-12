#!/bin/bash
# Minimal wrapper to run basedpyright via pip in the container
pip install -q --user basedpyright==1.38.2
export PATH="$HOME/.local/bin:$PATH"
exec basedpyright "$@"
