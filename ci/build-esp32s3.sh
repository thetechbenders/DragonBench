#!/usr/bin/env bash
set -euo pipefail

profile="${1:-n8r8}"
case "$profile" in
    n8r8)
        defaults="sdkconfig.defaults"
        expect=("CONFIG_SPIRAM_MODE_OCT 1" 'CONFIG_DB_TARGET_NAME "esp32s3-n8r8"' "CONFIG_DB_RF_SWITCH_GPIO -1")
        ;;
    tinys3d)
        defaults="sdkconfig.defaults;sdkconfig.defaults.tinys3d"
        expect=("CONFIG_SPIRAM_MODE_QUAD 1" 'CONFIG_DB_TARGET_NAME "esp32s3-tinys3d"' "CONFIG_DB_RF_SWITCH_GPIO 38")
        ;;
    *)
        echo "unknown board profile: $profile (expected n8r8 or tinys3d)" >&2
        exit 2
        ;;
esac

rm -f sdkconfig
idf.py -D SDKCONFIG_DEFAULTS="$defaults" fullclean
idf.py -D SDKCONFIG_DEFAULTS="$defaults" set-target esp32s3
idf.py -D SDKCONFIG_DEFAULTS="$defaults" build 2>&1 | tee idf-build.log

if grep -Ei 'warning:|error:' idf-build.log; then
    echo "ESP-IDF build emitted a warning or error" >&2
    exit 1
fi

for line in "${expect[@]}"; do
    if ! grep -qF "#define $line" build/config/sdkconfig.h; then
        echo "profile $profile did not produce '#define $line'" >&2
        exit 1
    fi
done
echo "profile $profile verified"
