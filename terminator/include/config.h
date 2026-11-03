#ifndef CONFIG_H
#define CONFIG_H

#include "terminator.h"
#include <X11/Xlib.h>
#include <X11/keysym.h>

typedef enum {
    ACT_MOVE_LEFT = 0,
    ACT_MOVE_RIGHT,
    ACT_MOVE_START,
    ACT_MOVE_END,
    ACT_HIST_PREV,
    ACT_HIST_NEXT,
    ACT_DEL_START,
    ACT_DEL_END,
    ACT_DEL_WORD,
    ACT_CLEAR_SCREEN,
    ACT_EXIT_SHELL,
    ACT_COPY,
    ACT_PASTE,
    ACT_COUNT
} ActionType;

typedef struct {
    ActionType action;
    KeySym keysym;
    unsigned int modifiers; /* ShiftMask, ControlMask, etc. */
    char name[32];
    char key_desc[64];
} KeyBinding;

typedef struct {
    KeyBinding bindings[ACT_COUNT];
    char config_path[MAX_PATH_LEN];
} AppConfig;

void config_init_defaults(AppConfig *cfg);
int config_load_file(AppConfig *cfg, const char *filepath);
ActionType config_match_action(const AppConfig *cfg, KeySym ks, unsigned int state);
const char *config_action_name(ActionType act);

#endif /* CONFIG_H */
