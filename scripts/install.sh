#!/bin/sh
set -e

if [ ! -f "src/powerd.c" ]; then
echo "==> Cloning powerd repository..."
rm -rf /tmp/powerd-install
git clone --depth 1 https://github.com/kushagra112007/powerd.git /tmp/powerd-install
cd /tmp/powerd-install
fi

SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo"

if ! command -v gcc >/dev/null 2>&1; then
echo "Error: gcc not found. On Puppy Linux, load the devx SFS first!"
exit 1
fi

echo "==> Stopping existing powerd (if running)..."
if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
$SUDO systemctl stop powerd.service 2>/dev/null || true
else
$SUDO killall powerd 2>/dev/null || true
fi

echo "==> Building powerd daemon..."
gcc -O3 src/powerd.c -o powerd

echo "==> Installing binaries & CLI..."
$SUDO cp powerd /usr/local/bin/powerd
$SUDO cp scripts/powerctl /usr/local/bin/powerctl
$SUDO chmod +x /usr/local/bin/powerd /usr/local/bin/powerctl
touch /tmp/powerd.mode /tmp/powerd.watts
$SUDO chmod 666 /tmp/powerd.mode /tmp/powerd.watts

if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
echo "==> Setting up systemd service..."
printf "[Unit]\nDescription=powerd power management daemon\nAfter=multi-user.target\n\n[Service]\nType=simple\nExecStart=/usr/local/bin/powerd\nRestart=always\n\n[Install]\nWantedBy=multi-user.target\n" | $SUDO tee /etc/systemd/system/powerd.service > /dev/null
$SUDO systemctl daemon-reload
$SUDO systemctl enable --now powerd.service
else
echo "==> Non-systemd OS detected (e.g., Puppy Linux). Starting daemon in background..."
mkdir -p /root/Startup
printf "#!/bin/sh\npidof powerd >/dev/null || /usr/local/bin/powerd >/tmp/powerd.log 2>&1 &\n" > /root/Startup/powerd
chmod +x /root/Startup/powerd
/usr/local/bin/powerd >/tmp/powerd.log 2>&1 &
fi

if command -v gnome-shell >/dev/null 2>&1; then
echo "==> Installing GNOME Shell Extension..."
EXT_DIR="$HOME/.local/share/gnome-shell/extensions/powerd-toggle@local"
mkdir -p "$EXT_DIR"
cp -r gnome-extension/powerd-toggle@local/* "$EXT_DIR/"
gsettings set org.gnome.shell disable-extension-version-validation true 2>/dev/null || true
fi

echo "==> Installation complete! Run: powerctl status"
