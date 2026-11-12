#!/bin/sh
set -e

SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo"

echo "==> Stopping existing powerd (if running)..."
if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    $SUDO systemctl stop powerd.service 2>/dev/null || true
else
    $SUDO killall powerd 2>/dev/null || true
fi

echo "==> Downloading powerd binary & CLI..."
$SUDO curl -sSL https://raw.githubusercontent.com/kushagra112007/powerd/main/bin/powerd-x86_64 -o /usr/local/bin/powerd
$SUDO curl -sSL https://raw.githubusercontent.com/kushagra112007/powerd/main/scripts/powerctl -o /usr/local/bin/powerctl
$SUDO chmod +x /usr/local/bin/powerd /usr/local/bin/powerctl
touch /tmp/powerd.mode /tmp/powerd.watts
$SUDO chmod 666 /tmp/powerd.mode /tmp/powerd.watts

if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    echo "==> Setting up systemd service..."
    printf "[Unit]\nDescription=powerd power management daemon\nAfter=multi-user.target\n\n[Service]\nType=simple\nExecStart=/usr/local/bin/powerd\nRestart=always\n\n[Install]\nWantedBy=multi-user.target\n" | $SUDO tee /etc/systemd/system/powerd.service > /dev/null
    $SUDO systemctl daemon-reload
    $SUDO systemctl enable --now powerd.service
else
    echo "==> Non-systemd OS detected (Puppy Linux). Starting daemon..."
    mkdir -p /root/Startup
    printf "#!/bin/sh\npidof powerd >/dev/null || /usr/local/bin/powerd >/tmp/powerd.log 2>&1 &\n" > /root/Startup/powerd
    chmod +x /root/Startup/powerd
    /usr/local/bin/powerd >/tmp/powerd.log 2>&1 &
fi

echo "==> Installation complete! Run: powerctl status"
