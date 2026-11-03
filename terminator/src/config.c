/*
 * ==============================================================================
 * Project:      Terminator - A Custom Linux Terminal & Shell
 * File:         src/config.c
 * Purpose:      Runtime hotkey configuration and ~/.terminatorrc file parser.
 * Description:  Reads custom user shortcut configurations (e.g., move_start = Ctrl+A),
 *               converts string representations into X11 KeySyms and modifier bitmasks
 *               (ControlMask, ShiftMask, Mod1Mask/Alt), detects conflicting bindings,
 *               and falls back safely to default keybindings when syntax errors occur.
 *
 * Viva / Examination Notes:
 * - What is a KeySym vs Modifier Mask in X11?
 *   - KeySym (Key Symbol) is an integer code representing the physical key pressed
 *     (e.g., XK_a, XK_Return, XK_Left).
 *   - Modifier Mask is a bitfield representing active modifier keys
 *     (e.g., ControlMask for Ctrl, ShiftMask for Shift, Mod1Mask for Alt).
 *   - For example, Ctrl+A produces keysym = XK_a and modifiers = ControlMask.
 * - Why do we mask out LockMask and Mod2Mask?
 *   - CapsLock (LockMask) and NumLock (Mod2Mask) alter the raw state bits sent by X11.
 *     If we didn't mask them out, pressing Ctrl+A with CapsLock turned on would fail to match!
 * ==============================================================================
 */

#include "config.h" /* Declarations for AppConfig, ActionType, and KeyBinding */

/*
 * Table of internal action identifier strings matching the ActionType enum.
 * Used for parsing configuration files like ~/.terminatorrc and generating error messages.
 */
static const char *action_names[ACT_COUNT] = {
    "move_left",    /* ACT_MOVE_LEFT: Move cursor 1 character left */
    "move_right",   /* ACT_MOVE_RIGHT: Move cursor 1 character right */
    "move_start",   /* ACT_MOVE_START: Move cursor to beginning of line (Ctrl+A) */
    "move_end",     /* ACT_MOVE_END: Move cursor to end of line (Ctrl+E) */
    "hist_prev",    /* ACT_HIST_PREV: Navigate to previous command in history (Up Arrow) */
    "hist_next",    /* ACT_HIST_NEXT: Navigate to next command in history (Down Arrow) */
    "del_start",    /* ACT_DEL_START: Delete from cursor to start of line (Ctrl+U) */
    "del_end",      /* ACT_DEL_END: Delete from cursor to end of line (Ctrl+K) */
    "del_word",     /* ACT_DEL_WORD: Delete previous word (Ctrl+W) */
    "clear_screen", /* ACT_CLEAR_SCREEN: Clear terminal screen buffer (Ctrl+L) */
    "exit_shell",   /* ACT_EXIT_SHELL: Exit current shell tab (Ctrl+D) */
    "copy",         /* ACT_COPY: Copy selected line to clipboard (Ctrl+Shift+C) */
    "paste"         /* ACT_PASTE: Paste clipboard data into active tab (Ctrl+Shift+V) */
};

/*
 * Function:    config_action_name
 * Purpose:     Converts an ActionType enum value to its human-readable string.
 * Parameters:  act - ActionType enum value.
 * Returns:     const char* - String name of the action (e.g., "move_start") or "unknown".
 */
const char *config_action_name(ActionType act) {
    if (act >= 0 && act < ACT_COUNT) {
        return action_names[act];
    }
    return "unknown";
}

/*
 * Function:    config_init_defaults
 * Purpose:     Initializes the AppConfig structure with standard GNU Readline / bash keybindings.
 * Parameters:  cfg - Pointer to the AppConfig structure to initialize.
 * Returns:     void.
 */
void config_init_defaults(AppConfig *cfg) {
    if (!cfg) return; /* Guard against null pointer */

    /* Zero out all fields in the config structure */
    memset(cfg, 0, sizeof(AppConfig));

    /* Array of default standard keybindings */
    KeyBinding defs[ACT_COUNT] = {
        { ACT_MOVE_LEFT,    XK_Left,  0,                        "move_left",    "Left" },
        { ACT_MOVE_RIGHT,   XK_Right, 0,                        "move_right",   "Right" },
        { ACT_MOVE_START,   XK_a,     ControlMask,              "move_start",   "Ctrl+A" },
        { ACT_MOVE_END,     XK_e,     ControlMask,              "move_end",     "Ctrl+E" },
        { ACT_HIST_PREV,    XK_Up,    0,                        "hist_prev",    "Up" },
        { ACT_HIST_NEXT,    XK_Down,  0,                        "hist_next",    "Down" },
        { ACT_DEL_START,    XK_u,     ControlMask,              "del_start",    "Ctrl+U" },
        { ACT_DEL_END,      XK_k,     ControlMask,              "del_end",      "Ctrl+K" },
        { ACT_DEL_WORD,     XK_w,     ControlMask,              "del_word",     "Ctrl+W" },
        { ACT_CLEAR_SCREEN, XK_l,     ControlMask,              "clear_screen", "Ctrl+L" },
        { ACT_EXIT_SHELL,   XK_d,     ControlMask,              "exit_shell",   "Ctrl+D" },
        { ACT_COPY,         XK_C,     ControlMask | ShiftMask,  "copy",         "Ctrl+Shift+C" },
        { ACT_PASTE,        XK_V,     ControlMask | ShiftMask,  "paste",        "Ctrl+Shift+V" }
    };

    /* Copy defaults into the configuration structure */
    for (int i = 0; i < ACT_COUNT; i++) {
        cfg->bindings[i] = defs[i];
    }

    /* Set default configuration file path to ~/.terminatorrc */
    const char *home = getenv("HOME");
    if (home) {
        snprintf(cfg->config_path, sizeof(cfg->config_path), "%s/.terminatorrc", home);
    } else {
        snprintf(cfg->config_path, sizeof(cfg->config_path), ".terminatorrc");
    }
}

/*
 * Function:    parse_keysym
 * Purpose:     Converts a single key string (e.g. "Left", "Return", "A") to an X11 KeySym.
 * Parameters:  key_str - String representing the key.
 * Returns:     KeySym   - X11 KeySym integer, or NoSymbol if unparseable.
 */
static KeySym parse_keysym(const char *key_str) {
    /* Special named navigation keys */
    if (strcasecmp(key_str, "Left") == 0) return XK_Left;
    if (strcasecmp(key_str, "Right") == 0) return XK_Right;
    if (strcasecmp(key_str, "Up") == 0) return XK_Up;
    if (strcasecmp(key_str, "Down") == 0) return XK_Down;
    if (strcasecmp(key_str, "Tab") == 0) return XK_Tab;
    if (strcasecmp(key_str, "Return") == 0 || strcasecmp(key_str, "Enter") == 0) return XK_Return;
    if (strcasecmp(key_str, "BackSpace") == 0) return XK_BackSpace;
    if (strcasecmp(key_str, "Delete") == 0) return XK_Delete;

    /* Single character alphanumeric keys */
    if (strlen(key_str) == 1) {
        char c = key_str[0];
        if (c >= 'A' && c <= 'Z') return XK_A + (c - 'A');
        if (c >= 'a' && c <= 'z') return XK_a + (c - 'a');
        if (c >= '0' && c <= '9') return XK_0 + (c - '0');
    }
    return NoSymbol; /* Symbol not recognized */
}

/*
 * Function:    parse_combination
 * Purpose:     Parses compound hotkey strings like "Ctrl+Shift+V" or "Alt+Left".
 * Parameters:  combo_str - Input combination string.
 *              out_ks    - Pointer to store the extracted KeySym.
 *              out_mod   - Pointer to store the extracted modifier bitmask.
 * Returns:     int       - 1 on successful parsing, 0 on failure.
 */
static int parse_combination(const char *combo_str, KeySym *out_ks, unsigned int *out_mod) {
    char buf[128];
    strncpy(buf, combo_str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    unsigned int mod = 0;
    char *token = strtok(buf, "+"); /* Split on '+' token delimiter */
    char *last_key = NULL;

    while (token) {
        /* Trim leading whitespace */
        while (*token == ' ') token++;
        /* Trim trailing whitespace */
        char *end = token + strlen(token) - 1;
        while (end > token && *end == ' ') { *end = '\0'; end--; }

        /* Match modifier keywords */
        if (strcasecmp(token, "Ctrl") == 0 || strcasecmp(token, "Control") == 0) {
            mod |= ControlMask;
        } else if (strcasecmp(token, "Shift") == 0) {
            mod |= ShiftMask;
        } else if (strcasecmp(token, "Alt") == 0 || strcasecmp(token, "Mod1") == 0) {
            mod |= Mod1Mask;
        } else {
            /* The last non-modifier token must be the actual key symbol */
            last_key = token;
        }
        token = strtok(NULL, "+");
    }

    if (!last_key) return 0; /* Missing key symbol */

    KeySym ks = parse_keysym(last_key);
    if (ks == NoSymbol) {
        return 0; /* Invalid key symbol */
    }

    /* Normalize case for control letters */
    if ((mod & ControlMask) && (ks >= XK_A && ks <= XK_Z) && !(mod & ShiftMask)) {
        ks = ks - XK_A + XK_a;
    } else if ((mod & ShiftMask) && (ks >= XK_a && ks <= XK_z)) {
        ks = ks - XK_a + XK_A;
    }

    *out_ks = ks;
    *out_mod = mod;
    return 1; /* Successfully parsed combination */
}

/*
 * Function:    config_load_file
 * Purpose:     Loads and parses the user configuration file (~/.terminatorrc).
 * Parameters:  cfg      - Pointer to AppConfig structure to populate.
 *              filepath - Path to config file (if NULL, uses cfg->config_path).
 * Returns:     int      - Returns 1 if file was loaded, 0 if defaults were used/generated.
 * Explanation: Automatically detects conflicting bindings (e.g. two actions bound to
 *              the same key) and restores defaults for conflicting entries with a warning.
 */
int config_load_file(AppConfig *cfg, const char *filepath) {
    if (!cfg) return 0;

    const char *path = filepath ? filepath : cfg->config_path;
    FILE *fp = fopen(path, "r");
    if (!fp) {
        /* If configuration file does not exist, create a template file with default bindings */
        FILE *wfp = fopen(path, "w");
        if (wfp) {
            fprintf(wfp, "# Terminator Configuration (~/.terminatorrc)\n");
            fprintf(wfp, "# Format: action = key_combination\n\n");
            for (int i = 0; i < ACT_COUNT; i++) {
                fprintf(wfp, "%-15s = %s\n", cfg->bindings[i].name, cfg->bindings[i].key_desc);
            }
            fclose(wfp);
        }
        return 0;
    }

    /* Keep a copy of default bindings for fallback in case of errors/conflicts */
    AppConfig defaults;
    config_init_defaults(&defaults);

    char line[256];
    int line_num = 0;

    /* Read configuration file line by line */
    while (fgets(line, sizeof(line), fp)) {
        line_num++;
        char *p = line;
        /* Skip leading whitespace */
        while (*p == ' ' || *p == '\t') p++;
        /* Skip comment lines and empty lines */
        if (*p == '#' || *p == '\n' || *p == '\0') continue;

        /* Look for key-value delimiter '=' */
        char *eq = strchr(p, '=');
        if (!eq) continue;

        *eq = '\0';
        char *act_str = p;
        char *val_str = eq + 1;

        /* Trim action name */
        while (*act_str == ' ' || *act_str == '\t') act_str++;
        char *end = act_str + strlen(act_str) - 1;
        while (end > act_str && (*end == ' ' || *end == '\t')) { *end = '\0'; end--; }

        /* Trim key combination value */
        while (*val_str == ' ' || *val_str == '\t') val_str++;
        end = val_str + strlen(val_str) - 1;
        while (end >= val_str && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
            *end = '\0';
            end--;
        }

        /* Find matching internal ActionType */
        int found_act = -1;
        for (int i = 0; i < ACT_COUNT; i++) {
            if (strcasecmp(act_str, action_names[i]) == 0) {
                found_act = i;
                break;
            }
        }

        if (found_act < 0) {
            fprintf(stderr, "terminator: config warning in %s:%d: unknown action '%s', ignoring\n",
                    path, line_num, act_str);
            continue;
        }

        KeySym ks;
        unsigned int mod;
        if (!parse_combination(val_str, &ks, &mod)) {
            fprintf(stderr, "terminator: config warning in %s:%d: invalid key combination '%s' for '%s', falling back to default\n",
                    path, line_num, val_str, act_str);
            cfg->bindings[found_act] = defaults.bindings[found_act];
            continue;
        }

        /* Update binding with parsed user preference */
        cfg->bindings[found_act].keysym = ks;
        cfg->bindings[found_act].modifiers = mod;
        strncpy(cfg->bindings[found_act].key_desc, val_str, sizeof(cfg->bindings[found_act].key_desc) - 1);
    }
    fclose(fp);

    /*
     * Conflict Detection:
     * Check if the user accidentally bound two different actions to the exact same key combination.
     * If a conflict is found, revert both conflicting actions to their safe defaults.
     */
    for (int i = 0; i < ACT_COUNT; i++) {
        for (int j = i + 1; j < ACT_COUNT; j++) {
            if (cfg->bindings[i].keysym == cfg->bindings[j].keysym &&
                cfg->bindings[i].modifiers == cfg->bindings[j].modifiers) {
                fprintf(stderr, "terminator: config conflict between '%s' and '%s' (%s), falling back to defaults\n",
                        cfg->bindings[i].name, cfg->bindings[j].name, cfg->bindings[j].key_desc);
                cfg->bindings[i] = defaults.bindings[i];
                cfg->bindings[j] = defaults.bindings[j];
            }
        }
    }

    return 1;
}

/*
 * Function:    config_match_action
 * Purpose:     Matches an incoming X11 KeyPress event against configured keybindings.
 * Parameters:  cfg   - Pointer to AppConfig structure.
 *              ks    - KeySym generated by the X11 keypress.
 *              state - Modifier bitmask from XKeyEvent.
 * Returns:     ActionType - The matching ActionType, or ACT_COUNT if no hotkey matched.
 */
ActionType config_match_action(const AppConfig *cfg, KeySym ks, unsigned int state) {
    if (!cfg) return ACT_COUNT;

    /*
     * Clean Modifier State:
     * Mask out irrelevant state bits like CapsLock (LockMask) and NumLock (Mod2Mask).
     * Only retain Shift, Control, and Alt (Mod1Mask).
     */
    unsigned int clean_state = state & (ShiftMask | ControlMask | Mod1Mask);

    for (int i = 0; i < ACT_COUNT; i++) {
        const KeyBinding *b = &cfg->bindings[i];
        if (b->modifiers == clean_state) {
            if (b->keysym == ks) return (ActionType)i;
            /* Case-insensitivity check for letter keys with Ctrl */
            if ((clean_state & ControlMask) && !(clean_state & ShiftMask)) {
                if (tolower((int)b->keysym) == tolower((int)ks)) return (ActionType)i;
            }
        }
    }

    return ACT_COUNT; /* No configured shortcut matched this keystroke */
}
