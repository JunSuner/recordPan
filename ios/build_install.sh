#!/bin/bash
# Builds, signs and installs RecordPan on the iPhone. Run from Terminal.app
# (the GUI session), where the login keychain is unlocked for code signing.
cd "$(dirname "$0")"
LOG=/tmp/recordpan_build.log
rm -f /tmp/recordpan_build.done
{
  xcodebuild -project RecordPan.xcodeproj -scheme RecordPan -configuration Debug \
    -destination 'id=00008140-00082CD91108801C' -derivedDataPath build -allowProvisioningUpdates build \
    | grep -E "error:|BUILD (SUCCEEDED|FAILED)"
  xcrun devicectl device install app --device 00008140-00082CD91108801C build/Build/Products/Debug-iphoneos/RecordPan.app 2>&1 | tail -3
  xcrun devicectl device process launch --device 00008140-00082CD91108801C com.junsuner.recordpan 2>&1 | tail -2
} > "$LOG" 2>&1
touch /tmp/recordpan_build.done
