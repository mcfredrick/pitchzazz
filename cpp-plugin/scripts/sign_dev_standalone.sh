#!/bin/bash
#
# Codesigns the locally-built Standalone app with a stable identity so macOS
# TCC (the privacy-permission system, e.g. microphone access) stops treating
# every debug rebuild as a brand-new app and silently dropping permissions
# already granted to a previous build. Root cause: an ad-hoc signature (the
# default for a plain `cmake --build`) is a hash of the binary itself, so it
# changes on every rebuild -- see docs/FINDINGS.md for the full writeup.
#
# Deliberately separate from scripts/build.sh's `sign` step, which signs
# *release* builds with $APP_CERT from .env (a paid Developer ID Application
# certificate, for distribution/notarization). This script is for local
# dev-loop iteration only, and uses a free "Apple Development" identity
# already in your keychain (tied to your own Apple ID -- no paid Program
# membership needed, no new certificate to create). Because the identity is
# the same across rebuilds, TCC's grant survives them too: run this once
# after each `cmake --build`, no repeated `tccutil reset` needed.
#
# Usage: ./scripts/sign_dev_standalone.sh [path-to-.app]
#   defaults to build/Pitchzazz_artefacts/Release/Standalone/Pitchzazz.app
#
# To use a different identity (e.g. if this one isn't in your keychain),
# list what's available with: security find-identity -v -p codesigning

set -euo pipefail

APP_PATH="${1:-build/Pitchzazz_artefacts/Release/Standalone/Pitchzazz.app}"
IDENTITY="Apple Development: mattcfredrick@icloud.com (Z8Y65Z29J4)"

if [ ! -d "$APP_PATH" ]; then
  echo "App bundle not found: $APP_PATH"
  exit 1
fi

codesign --force --deep --sign "$IDENTITY" "$APP_PATH"
echo "Signed $APP_PATH with: $IDENTITY"
codesign -dv "$APP_PATH" 2>&1 | grep -E "Identifier|TeamIdentifier|Authority" || true
