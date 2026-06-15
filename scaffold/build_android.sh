#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

cd "$SCRIPT_DIR/examples/android"
./gradlew assembleDebug --no-daemon 2>&1
