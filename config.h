/* compile time config, rebuild after edits */
#ifndef CONFIG_H
#define CONFIG_H

/* no output selected, exactly one %s token
   stop swww/awww layers, start new swaybg, kill old when new pid appears
   bounded poll, old survives if swaybg never starts */
static const char *cmd[] = {
    "sh", "-c",
    "if pgrep -x swww-daemon >/dev/null 2>&1 || pgrep -x awww-daemon >/dev/null 2>&1; then "
    "  pkill -x swww-daemon 2>/dev/null; pkill -x awww-daemon 2>/dev/null; "
    "fi; "
    "old=$(pgrep -x swaybg | tr '\\n' ' '); "
    "setsid -f swaybg -i \"$1\" -m fill >/dev/null 2>&1; "
    "if [ -n \"$old\" ]; then "
    "  i=0; "
    "  while [ $i -lt 100 ]; do "
    "    new=$(pgrep -n -x swaybg); "
    "    if [ -n \"$new\" ]; then "
    "      case \" $old \" in *\" $new \"*) ;; "
    "        *) for p in $old; do [ \"$p\" = \"$new\" ] || kill \"$p\" 2>/dev/null; done; exit 0;; "
    "      esac; "
    "    fi; "
    "    sleep 0.005; i=$((i+1)); "
    "  done; "
    "fi",
    "sh", "%s", NULL
};

/* no per output support with swaybg */
static const char *cmd_out[] = { NULL };

/* one output name per line for Tab, NULL accepts a literal name
   example: "hyprctl monitors | awk '/^Monitor/ {print $2}'" */
static const char *outputs_cmd = NULL;

/* extensions to show, case insensitive */
static const char *exts[] = {
    ".jpg", ".jpeg", ".png", ".webp", ".bmp",
    ".tif", ".tiff", ".avif", ".jxl", NULL
};

/* leading ~ expanded, NULL uses $HOME/.local/share/backgrounds */
static const char *default_dir = NULL;

#endif /* CONFIG_H */
