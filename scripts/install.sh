#!/bin/sh
set -e

REPO="kushagra112007/powerd"
BIN_DIR="/usr/local/bin"
STATE_DIR="/run/powerd"

CURL_OPTS="-fL --connect-timeout 15 --retry 3"
if ! curl -6 -sI --connect-timeout 2 https://github.com >/dev/null 2>&1; then
    CURL_OPTS="$CURL_OPTS -4"
fi

echo "==> Stopping existing powerd (if running)..."
if command -v systemctl >/dev/null 2>&1 && systemctl is-active --quiet powerd 2>/dev/null; then
    systemctl stop powerd || true
fi
killall powerd 2>/dev/null || true

echo "==> Downloading powerd binary & CLI..."
if ! curl $CURL_OPTS "https://github.com/$REPO/releases/latest/download/powerd" -o "$BIN_DIR/powerd" 2>/dev/null; then
    curl $CURL_OPTS "https://raw.githubusercontent.com/$REPO/main/bin/powerd-x86_64" -o "$BIN_DIR/powerd"
fi
curl $CURL_OPTS "https://raw.githubusercontent.com/$REPO/main/scripts/powerctl" -o "$BIN_DIR/powerctl"
chmod 0755 "$BIN_DIR/powerd" "$BIN_DIR/powerctl"

mkdir -p "$STATE_DIR"
chmod 0755 "$STATE_DIR"

if [ -d /sys/devices/system/cpu ] && command -v modprobe >/dev/null 2>&1; then
    modprobe intel_rapl_msr 2>/dev/null || true
fi

if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    printf "%s\n" "[Unit]
Description=Zero-Dependency Static C Power Daemon
After=multi-user.target
Conflicts=power-profiles-daemon.service tlp.service

[Service]
Type=simple
ExecStartPre=-/sbin/modprobe intel_rapl_msr
ExecStart=/usr/local/bin/powerd
Restart=on-failure
RestartSec=2

[Install]
WantedBy=multi-user.target" > /etc/systemd/system/powerd.service
    systemctl daemon-reload
    systemctl enable --now powerd
else
    if [ -d /etc/init.d ]; then
        printf "%s\n" "#!/bin/sh
case \"\$1\" in
    start)
        modprobe intel_rapl_msr 2>/dev/null || true
        /usr/local/bin/powerd >/var/log/powerd.log 2>&1 &
        ;;
    stop)
        killall powerd 2>/dev/null || true
        ;;
    restart)
        \"\$0\" stop
        sleep 1
        \"\$0\" start
        ;;
esac" > /etc/init.d/powerd
        chmod 0755 /etc/init.d/powerd
    fi
    /usr/local/bin/powerd >/var/log/powerd.log 2>&1 &
fi

echo "==> powerd installed and running. Check status with: powerctl status"
