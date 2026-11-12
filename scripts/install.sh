#!/bin/bash
set -e

if [ ! -f "src/powerd.c" ]; then
    echo "==> Cloning powerd repository..."
    rm -rf /tmp/powerd-install
    git clone --depth 1 https://github.com/kushagra112007/powerd.git /tmp/powerd-install
    cd /tmp/powerd-install
fi

echo "==> Stopping running powerd service (if active)..."
sudo systemctl stop powerd.service 2>/dev/null || true

echo "==> Building powerd daemon..."
gcc -O3 src/powerd.c -o powerd

echo "==> Installing binaries & CLI..."
sudo cp powerd /usr/local/bin/powerd
sudo cp scripts/powerctl /usr/local/bin/powerctl
sudo chmod +x /usr/local/bin/powerd /usr/local/bin/powerctl

echo "==> Setting up systemd service..."
printf "[Unit]\nDescription=powerd power management daemon\nAfter=multi-user.target\n\n[Service]\nType=simple\nExecStart=/usr/local/bin/powerd\nRestart=always\n\n[Install]\nWantedBy=multi-user.target\n" | sudo tee /etc/systemd/system/powerd.service > /dev/null

sudo systemctl daemon-reload
sudo systemctl enable --now powerd.service

echo "==> Installing GNOME Shell Extension..."
EXT_DIR="$HOME/.local/share/gnome-shell/extensions/powerd-toggle@local"
mkdir -p "$EXT_DIR"
cp -r gnome-extension/powerd-toggle@local/* "$EXT_DIR/"
gsettings set org.gnome.shell disable-extension-version-validation true 2>/dev/null || true

echo "==> Installation complete! Log out and back in to see the panel menu."
