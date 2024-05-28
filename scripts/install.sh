#!/bin/bash

set -e

VENV=.venv
if [ ! -d "$VENV" ]; then
    python -m venv "$VENV"
fi
source "$VENV"/bin/activate

pip install -e .[dev,test,doc]

pre-commit install
