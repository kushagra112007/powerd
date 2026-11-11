import St from "gi://St";
import Gio from "gi://Gio";
import * as Main from "resource:///org/gnome/shell/ui/main.js";
import * as PanelMenu from "resource:///org/gnome/shell/ui/panelMenu.js";
import * as PopupMenu from "resource:///org/gnome/shell/ui/popupMenu.js";
import { Extension } from "resource:///org/gnome/shell/extensions/extension.js";

export default class PowerdToggleExtension extends Extension {
    _runPowerctl(mode) {
        try {
            let proc = new Gio.Subprocess({
                argv: ["/usr/local/bin/powerctl", mode],
                flags: Gio.SubprocessFlags.NONE,
            });
            proc.init(null);
        } catch (e) {
            console.error("[powerd-extension] Failed to run powerctl:", e);
        }
    }

    enable() {
        // 1. Create panel icon container
        this._indicator = new PanelMenu.Button(0.0, "Powerd Control", false);
        
        let icon = new St.Icon({
            icon_name: "power-profile-power-saver-symbolic",
            style_class: "system-status-icon"
        });
        this._indicator.add_child(icon);

        // 2. Add clickable profile items to the drop-down menu
        let itemLow = new PopupMenu.PopupMenuItem("Low-Power Mode");
        itemLow.connect("activate", () => this._runPowerctl("low"));
        this._indicator.menu.addMenuItem(itemLow);

        let itemBalanced = new PopupMenu.PopupMenuItem("Balanced Mode");
        itemBalanced.connect("activate", () => this._runPowerctl("balanced"));
        this._indicator.menu.addMenuItem(itemBalanced);

        let itemPerf = new PopupMenu.PopupMenuItem("Performance Mode");
        itemPerf.connect("activate", () => this._runPowerctl("perf"));
        this._indicator.menu.addMenuItem(itemPerf);

        // 3. Attach to top bar
        Main.panel.addToStatusArea("powerd-toggle", this._indicator);
    }

    disable() {
        if (this._indicator) {
            this._indicator.destroy();
            this._indicator = null;
        }
    }
}
