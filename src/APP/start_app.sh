#!/bin/bash

APP="Path"

# Khong mo trung App neu da chay
if pgrep -f "mono.*app.exe" > /dev/null; then
    exit 0
fi

# Doi backend AI service khoi dong
for i in $(seq 1 30); do
    if systemctl is-active --quiet decal-ai.service; then
        break
    fi

    sleep 1
done

# Doi them mot chut cho TensorRT load 2 model on dinh
sleep 3

exec sudo -n \
    DISPLAY="$DISPLAY" \
    XAUTHORITY="$XAUTHORITY" \
    /usr/bin/mono "$APP"
