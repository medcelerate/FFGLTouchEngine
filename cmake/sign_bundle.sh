#!/bin/bash
# Signs all Mach-O binaries inside a bundle, then signs the bundle itself.
# Usage: sign_bundle.sh <bundle_path>
set -e

BUNDLE="$1"

if [ -z "$BUNDLE" ]; then
    echo "Usage: $0 <bundle_path>"
    exit 1
fi

# Find and sign all Mach-O binaries inside the Frameworks directory
if [ -d "$BUNDLE/Contents/Frameworks" ]; then
    find "$BUNDLE/Contents/Frameworks" -type f \( -name "*.dylib" -o -perm +111 \) | while read binary; do
        # Check if it's actually a Mach-O file
        if file "$binary" | grep -q "Mach-O"; then
            codesign --force --sign - --timestamp=none "$binary" 2>/dev/null || true
        fi
    done
fi

# Sign the main bundle
codesign --force --sign - --timestamp=none "$BUNDLE"
