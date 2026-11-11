#!/bin/sh
set -e

echo "=== Installing Powerd Daemon ==="

if systemctl is-active --quiet powerd.service 2>/dev/null; then
    sudo systemctl stop powerd.service
fi

gcc -O2 src/powerd.c -o powerd

sudo cp -f powerd /usr/local/bin/powerd
sudo chmod +x /usr/local/bin/powerd
sudo cp -f scripts/powerctl /usr/local/bin/powerctl
sudo chmod +x /usr/local/bin/powerctl

sudo touch /tmp/powerd.mode
sudo chmod 666 /tmp/powerd.mode

if [ -d /run/systemd/system ]; then
    printf "[Unit]\nDescription=Zero-Dependency Power & Core Telemetry Daemon\nAfter=multi-user.target\n\n[Service]\nType=simple\nExecStart=/usr/local/bin/powerd\nRestart=always\nRestartSec=2\n\n[Install]\nWantedBy=multi-user.target\n" | sudo tee /etc/systemd/system/powerd.service >/dev/null
    sudo systemctl daemon-reload
    sudo systemctl enable --now powerd.service
    echo "=== Service Started via systemd ==="
else
    sudo pkill -f /usr/local/bin/powerd || true
    sudo nohup /usr/local/bin/powerd >/dev/null 2>&1 &
    echo "=== Daemon Started in Background ==="
fi

echo "Installation Complete! Run 'powerctl' to switch profiles."

