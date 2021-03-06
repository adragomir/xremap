#include <stdbool.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/record.h>
#include <X11/extensions/XTest.h>
#include <X11/XKBlib.h>
#include <X11/Xmu/WinUtil.h>

#include "khash.h"

typedef struct {
    bool shift;
    bool control;
    bool alt;
    bool super;
    KeyCode key;
    int button;
} Hotkey;

Hotkey* new_hotkey() {
    Hotkey* h = malloc(sizeof(Hotkey));
    h->shift = false;
    h->control = false;
    h->alt = false;
    h->super = false;
    h->key = 0;
    h->button = 0;
    return h;
}

KHASH_MAP_INIT_STR(Mappings, Hotkey*)

KHASH_MAP_INIT_INT(Config, khash_t(Mappings)*)

typedef struct {
	Display *data_conn;
	Display *ctrl_conn;
	XRecordContext record_ctx;
	pthread_t sigwait_thread;
	sigset_t sigset;
	int debug;
	khash_t(Config) *config;
	Hotkey *current;
	volatile int handling;
} App;

static App *app = NULL;

void *sig_handler (void *user_data);

void intercept(XPointer user_data, XRecordInterceptData *data);
void grab_all_keys(App *app);

void print_usage (const char *program_name);

unsigned char SHIFT_MASK = 0b00001000;
unsigned char CONTROL_MASK = 0b00000100;
unsigned char ALT_MASK = 0b00000010;
unsigned char SUPER_MASK = 0b00000001;
unsigned char BUTTON1_MASK = 0b10000000;
unsigned char BUTTON2_MASK = 0b01000000;
unsigned char BUTTON3_MASK = 0b00100000;

Hotkey * short_to_hotkey(unsigned short in) {
    Hotkey *out = new_hotkey();
    out->shift = ((SHIFT_MASK << 8) & in) > 0;
    out->control = ((CONTROL_MASK << 8) & in) > 0;
    out->alt = ((ALT_MASK << 8) & in) > 0;
    out->super = ((SUPER_MASK << 8) & in) > 0;
    out->button = ((BUTTON1_MASK << 8) & in) > 0 ? Button1 : 0;
    out->button = ((BUTTON2_MASK << 8) & in) > 0 ? Button2 : 0;
    out->button = ((BUTTON3_MASK << 8) & in) > 0 ? Button3 : 0;
    out->key = (char)(in& 0xFF);
    return out;
}

unsigned short hotkey_to_short(Hotkey h) {
    unsigned short out = 0;
    if (h.shift) out |= (SHIFT_MASK << 8);
    if (h.control) out |= (CONTROL_MASK << 8);
    if (h.alt) out |= (ALT_MASK << 8);
    if (h.super) out |= (SUPER_MASK << 8);
    if (h.button == Button1) out |= (BUTTON1_MASK << 8);
    if (h.button == Button2) out |= (BUTTON2_MASK << 8);
    if (h.button == Button3) out |= (BUTTON3_MASK << 8);
    out |= h.key;
    return out;
}

void hotkey_to_grab_key(Hotkey h, int *keycode, unsigned int *modifiers) {
    *keycode = h.key;
    *modifiers = 0;

    if (h.shift) *modifiers |= ShiftMask;
    if (h.control) *modifiers |= ControlMask;
    if (h.alt) *modifiers |= Mod1Mask;
    if (h.super) *modifiers |= Mod4Mask;
}

void dump_hotkey(Hotkey h) {
    fprintf(stderr, "Hotkey: %d|%d|%d|%d - %d / %d\n", h.shift, h.control, h.alt, h.super, h.key, h.button);
}

int handle_token(Display *d, char *token, Hotkey *h) {
    if (strcmp(token, "shift") == 0) {
        h->shift = true;
    } else if (strcmp(token, "control") == 0 || strcmp(token, "ctrl") == 0) {
        h->control = true;
    } else if (strcmp(token, "alt") == 0 || strcmp(token, "mod1") == 0) {
        h->alt = true;
    } else if (strcmp(token, "super") == 0 || strcmp(token, "mod4") == 0) {
        h->super = true;
    } else if (strcmp(token, "b1") == 0) {
        h->button = Button1;
    } else if (strcmp(token, "b2") == 0) {
        h->super = Button2;
    } else if (strcmp(token, "b3") == 0) {
        h->super = Button3;
    } else {
        KeySym ks = NoSymbol;
        if ((ks = XStringToKeysym(token)) == NoSymbol) {
            fprintf(stderr, "Invalid key: %s\n", token);
            return 1;
        }
        KeyCode code = XKeysymToKeycode(d, ks);
        if (code == 0) {
            fprintf(stderr, "WARNING: No keycode found for keysym "
                    "%s (0x%x). Ignoring this "
                    "mapping.\n", token, (unsigned int)ks);
            return 1;
        }
        h->key = code;
    }
    return 0;
}

Hotkey* parse_string(Display *d, const char* input) {
    unsigned char mods = 0;
    unsigned char keycode = 0; 
    unsigned short out = 0;
    Hotkey *h = new_hotkey();
    char* inputCopy = strdup(input);
    char* token = strtok(inputCopy, "-");
    while (token != NULL) {
        if (handle_token(d, token, h) > 0) {
            fprintf(stderr, "Could not parse string %s", token);
            free(h);
            return NULL;
        }
        token = strtok(NULL, "-");
    }
    free(inputCopy);
    return h;
}

void add_key(Display *d, khash_t(Config) *config, const char * from, const char *class, const char *to) {
    Hotkey *hfrom = parse_string(d, from);
    if (hfrom == NULL) {
        fprintf(stderr, "Could not parse from hotkey: %s\n", from);
        return;
    }
    Hotkey *hto = parse_string(d, to);
    if (hto == NULL) {
        fprintf(stderr, "Could not parse to hotkey: %s\n", to);
        free(hfrom);
        return;
    }

    unsigned short from_short = hotkey_to_short(*hfrom);
    free(hfrom);
    fprintf(stderr, "Adding config key %s - %s for app %s\n", from, to, class);
    khint_t k = kh_get(Config, config, from_short);
    if (k == kh_end(config)) {
        int ret;
        k = kh_put(Config, config, from_short, &ret);
        if (!ret) {
            fprintf(stderr, "Could not insert hotkey %s\n", from);
            return;
        }
        kh_value(config, k) = kh_init(Mappings);
    }
    khash_t(Mappings)* mappings = kh_value(config, k);
    
    khint_t k2 = kh_get(Mappings, mappings, class);
    if (k2 == kh_end(mappings)) {
        int ret;
        k2 = kh_put(Mappings, mappings, class, &ret);
        kh_value(mappings, k2) = hto;
    }
}

void load_configuration_file(App* app) {
    app->config = kh_init(Config);

    if (1) {
        struct passwd *pw = getpwuid(getuid());
        const char *homedir = pw->pw_dir;
        char path[1000];
        sprintf(path, "%s/%s", homedir, ".config/xremap" );
        FILE *fd = fopen(path, "r");
        if (fd == NULL) {
            fprintf(stderr, "Error opening configuration file %s\n", path);
        }
        char line[255];
        while (fgets(line, sizeof(line), fd) != NULL) {
            char* from = strdup(strtok(line, " "));
            char* class = strdup(strtok(NULL, " "));
            char* to = strdup(strtok(NULL, " "));
            to[strcspn(to, "\n")] = 0;
            add_key(app->ctrl_conn, app->config, from, class, to);
            free(from);
            free(to);
        }
        fclose(fd);
    }
}

Window get_top_window(Display* d, Window start) {
    Window w = start;
    Window parent = start;
    Window root = None;
    Window *children;
    unsigned int nchildren;
    Status s;

    while (parent != root) {
        w = parent;
        s = XQueryTree(d, w, &root, &parent, &children, &nchildren);
        if (s) {
            XFree(children);
        }
    }
    return w;
}


Window *get_wm_window_list(Display *d, unsigned long *len) {
    Atom prop = XInternAtom(d, "_NET_CLIENT_LIST", True);
    Atom type;
    int format;
    unsigned long remain;
    unsigned char *list = 0;

    fprintf(stderr, "Getting wm window list using _NET_CLIENT_LIST\n");
    if (XGetWindowProperty(d, DefaultRootWindow(d), prop, 0, (~0L), False, XA_WINDOW, &type, &format, len, &remain, &list) != Success) {
        fprintf(stderr, "Could not get list of windows from WM\n");
        return 0;
    }

    return (Window*)list;
}

Window get_input_focus_window(Display* d) {
    Window w;
    int revert_to;
    XGetInputFocus(d, &w, &revert_to);
    return w;
}

Window get_active_window(Display *d) {
    Window root = XDefaultRootWindow(d);
    Atom netactivewindow = XInternAtom(d, "_NET_ACTIVE_WINDOW", False);

    Atom real;
    int format;
    unsigned long extra, n, window;
    unsigned char *data;

    if (XGetWindowProperty(d, root, netactivewindow, 0, ~0, False,
        AnyPropertyType, &real, &format, &n, &extra,
        &data) != Success && data != 0) {
        fprintf(stderr, "Could not get netactivewindow property !\n");
    }

    window = *(unsigned long *) data;
    XFree (data);

    if (window == 0) {
        fprintf(stderr, "No active window !\n");
    }
    return window;
}

XClassHint* get_window_class_hint(Display* d, Window w) {
    XClassHint* class = XAllocClassHint();

    Window win = w;
    Status s = 0;
    do {
        s = XGetClassHint(d, w, class);
        if (s == 0) {
            Window root = None;
            Window parent = win;
            Window *children;
            unsigned int nchildren;
            Status s2 = XQueryTree(d, win, &root, &parent, &children, &nchildren);
            if (s2 > 0) {
                win = parent;
            }
        }
    } while (win != DefaultRootWindow(d) && s == 0);

    if (s > 0) {
        return class;
    }
    return 0;
}

unsigned char * get_window_class(Display *d, Window w) {
    Atom real;
    int format;
    unsigned long extra, n;
    unsigned char *data = NULL;

    Window win = w;
    while (win != DefaultRootWindow(d) && data == NULL) {
        Status gs = XGetWindowProperty(d, win, XA_WM_CLASS, 0, ~0, False, AnyPropertyType, &real, &format, &n, &extra, &data);
        if (gs != Success) {
            Window root = None;
            Window parent = win;
            Window *children;
            unsigned int nchildren;
            Status s = XQueryTree(d, win, &root, &parent, &children, &nchildren);
            if (s > 0) {
                win = parent;
            }
        }
    }
    if (data != NULL) {
    }
    return data;
}

void recurse_window(Display *d, Window w, void (*cb)(Display *d, void *user_data, Window w), void *user_data) {
    Window root = None;
    Window parent = w;
    Window *children;
    unsigned int nchildren;
    cb(d, user_data, w);
	Status s = XQueryTree(d, w, &root, &parent, &children, &nchildren);
	if (s > 0) {
        if (children != NULL) {
            for (int i = 0; i < nchildren; i++) {
                recurse_window(d, children[i], cb, user_data);
            }
        }
    }
    if (children != NULL) {
        XFree(children);
    }
}

void grab_all_keys_for_window(void *tmp, Window w) {
    App *app = (App*) tmp;
    Display *d = app->ctrl_conn;
    XClassHint* class_hint = get_window_class_hint(d, w);
    char *class =  class_hint->res_class;
    fprintf(stderr, "Grab all keys for window %ld, %s, %s\n", w, class_hint->res_class, class_hint->res_name);
    for (khint_t k = kh_begin(app->config); k != kh_end(app->config); ++k) {
        if (kh_exist(app->config, k)) {
            Hotkey *from = short_to_hotkey(kh_key(app->config, k));
            int keycode;
            unsigned int modifiers;
            hotkey_to_grab_key(*from, &keycode, &modifiers);
            free(from);
            khash_t(Mappings)* mapping = kh_value(app->config, k);
            // search for window itself !!!
            khint_t kapp = kh_get(Mappings, mapping, class);
            if (kapp != kh_end(mapping)) {
                // SPECIFIC HOTKEY
                fprintf(stderr, "Got specific hotkey for window %s \n", class);
                XGrabKey(d, keycode, modifiers, w, False, GrabModeAsync, GrabModeAsync);
            } else {
                khint_t kall = kh_get(Mappings, mapping, "*");
                if (kall != kh_end(mapping)) {
                    // GLOBAL HOTKEY
                    fprintf(stderr, "Got ALL hotkey for window %s \n", class);
                    XGrabKey(d, keycode, modifiers, w, False, GrabModeAsync, GrabModeAsync);
                }
            }
        }
    }
    free(class);
    free(class_hint);
}

void grab_all_keys(App *app) {
    fprintf(stderr, "Grabbing all keys for all apps\n");
    Display *d = app->ctrl_conn;
    unsigned long nitems;
    Window* windows = get_wm_window_list(d, &nitems);
    for (int i = 0; i < nitems; i++) {
        grab_all_keys_for_window((void *)app, windows[i]);
    }
    free(windows);
}

void restore_current_mods(Display *d, Hotkey h) {
    fprintf(stderr, "Restoring current mods to %d, %d, %d, %d\n", h.shift, h.control, h.alt, h.super);
    if (h.shift) XTestFakeKeyEvent(d, XKeysymToKeycode(d, XK_Shift_L), True, 0);
    if (h.control) XTestFakeKeyEvent(d, XKeysymToKeycode(d, XK_Control_L), True, 0);
    if (h.alt) XTestFakeKeyEvent(d, XKeysymToKeycode(d, XK_Alt_L), True, 0);
    if (h.super) XTestFakeKeyEvent(d, XKeysymToKeycode(d, XK_Super_L), True, 0);
}
void release_current(Display *d, Hotkey h) {
    fprintf(stderr, "RELEASING current mods to %d, %d, %d, %d\n", h.shift, h.control, h.alt, h.super);
    if (h.shift) XTestFakeKeyEvent(d, XKeysymToKeycode(d, XK_Shift_L), False, 0);
    if (h.control) XTestFakeKeyEvent(d, XKeysymToKeycode(d, XK_Control_L), False, 0);
    if (h.alt) XTestFakeKeyEvent(d, XKeysymToKeycode(d, XK_Alt_L), False, 0);
    if (h.super) XTestFakeKeyEvent(d, XKeysymToKeycode(d, XK_Super_L), False, 0);
    if (h.key > 0) XTestFakeKeyEvent(d, h.key, False, 0);
}

void key_action(Display *d, Hotkey h) {
    if (h.shift) XTestFakeKeyEvent(d, XKeysymToKeycode(d, XK_Shift_L), True, 0);
    if (h.control) XTestFakeKeyEvent(d, XKeysymToKeycode(d, XK_Control_L), True, 0);
    if (h.alt) XTestFakeKeyEvent(d, XKeysymToKeycode(d, XK_Alt_L), True, 0);
    if (h.super) XTestFakeKeyEvent(d, XKeysymToKeycode(d, XK_Super_L), True, 0);
    if (h.key > 0) {
        XTestFakeKeyEvent(d, h.key, True, 0);
        XTestFakeKeyEvent(d, h.key, False, 0);
    } else if (h.button > 0) {
        // Window root, child;
        // int rootX, rootY, winX, winY;
        // unsigned int mask;
        // XQueryPointer(d, RootWindow(d, 0), &root, &child, &rootX, &rootY, &winX, &winY, &mask);
        XTestFakeButtonEvent (d, h.button, True,  0);
        XTestFakeButtonEvent (d, h.button, False, 0);
    }
    if (h.super) XTestFakeKeyEvent(d, XKeysymToKeycode(d, XK_Super_L), False, 0);
    if (h.alt) XTestFakeKeyEvent(d, XKeysymToKeycode(d, XK_Alt_L), False, 0);
    if (h.control) XTestFakeKeyEvent(d, XKeysymToKeycode(d, XK_Control_L), False, 0);
    if (h.shift) XTestFakeKeyEvent(d, XKeysymToKeycode(d, XK_Shift_L), False, 0);
}

void execute(App* app) {
    unsigned short config_key = hotkey_to_short(*app->current);
    khint_t hotkey_found = kh_get(Config, app->config, config_key);
    if (hotkey_found != kh_end(app->config)) {
        khash_t(Mappings)* mapping = kh_value(app->config, hotkey_found);
        khint_t any_found = kh_get(Mappings, mapping, "*");
        if (any_found != kh_end(mapping)) {
            fprintf(stderr, "Found remapping for ANY\n");
            app->handling = 1;
            Hotkey *to = kh_value(mapping, any_found);
            XTestGrabControl(app->ctrl_conn, True);
            release_current(app->ctrl_conn, *app->current);
            XFlush(app->ctrl_conn);
            key_action(app->ctrl_conn, *to);
            XFlush(app->ctrl_conn);
            restore_current_mods(app->ctrl_conn, *app->current);
            app->current->key = 0;
            XFlush(app->ctrl_conn);
            app->handling = 0;
        } else {
            Window w = get_active_window(app->ctrl_conn);
            if (w == None) {
                fprintf(stderr, "Could not get focused window !\n");
            } else {
                XClassHint* class_hint = get_window_class_hint(app->ctrl_conn, w);
                char* class = class_hint->res_class;
                if (class == NULL) {
                    fprintf(stderr, "Could not get focused window class !\n");
                } else {
                    khint_t app_found = kh_get(Mappings, mapping, class);
                    if (app_found != kh_end(mapping)) {
                        fprintf(stderr, "Found remapping for app %s\n", class);
                        app->handling = 1;
                        Hotkey current_copy = *app->current;
                        Hotkey *to = kh_value(mapping, app_found);
                        XTestGrabControl(app->ctrl_conn, True);
                        release_current(app->ctrl_conn, current_copy);
                        XFlush(app->ctrl_conn);
                        key_action(app->ctrl_conn, *to);
                        XFlush(app->ctrl_conn);
                        restore_current_mods(app->ctrl_conn, current_copy);
                        XFlush(app->ctrl_conn);
                        //*app->current = current_copy;
                        app->current->key = 0;
                        app->handling = 0;
                    }
                }
                XFree(class_hint);
                XFree(class);
            }
        }
    }
}

typedef union {
  unsigned char    type;
  xEvent           event;
  xResourceReq     req;
  xGenericReply    reply;
  xError           error;
  xConnSetupPrefix setup;
} XRecordDatum;

void intercept(XPointer user_data, XRecordInterceptData *data) {
    if (data->category != XRecordFromServer) {
        XRecordFreeData(data);
        return;
    }

	App *app = (App*)user_data;

	// mangle data
    XRecordDatum *datum = (XRecordDatum*) data->data;
    int event_type = datum->event.u.u.type;

	XLockDisplay(app->ctrl_conn);

    if (event_type == KeyPress) {
        KeyCode key_code  = datum->event.u.u.detail;
        if (app->debug) fprintf(stderr, "Intercepted key press, key code %d | %d, %ul, %d || %d\n", key_code, data->id_base, data->client_seq, data->category, data->client_swapped, app->handling);
        KeySym now = XkbKeycodeToKeysym(app->ctrl_conn, key_code, 0, 0);
        if (now == XK_Shift_L || now == XK_Shift_R) {
            app->current->shift = true;
        } else if (now == XK_Control_L || now == XK_Control_R) {
            app->current->control = true;
        } else if (now == XK_Alt_L || now == XK_Alt_R) {
            app->current->alt = true;
        } else if (now == XK_Super_L || now == XK_Super_R) {
            app->current->super = true;
        } else {
            // got modifiers, we can now do it
            app->current->key = key_code;
            execute(app);
        }
    } else if (event_type == KeyRelease) {
        // reset modifiers
        KeyCode key_code  = datum->event.u.u.detail;
        if (app->debug) fprintf(stderr, "Intercepted key release, key code %d | %d, %ul, %d || %d\n", key_code, data->id_base, data->client_seq, data->category, data->client_swapped, app->handling);
        KeySym now = XkbKeycodeToKeysym(app->ctrl_conn, key_code, 0, 0);
        if (now == XK_Shift_L || now == XK_Shift_R) {
            app->current->shift = false;
        } else if (now == XK_Control_L || now == XK_Control_R) {
            app->current->control = false;
        } else if (now == XK_Alt_L || now == XK_Alt_R) {
            app->current->alt = false;
        } else if (now == XK_Super_L || now == XK_Super_R) {
            app->current->super = false;
        } else {
            // got modifiers, we can now do it
            app->current->key = 0;
        }
    } else if (event_type == ButtonPress) {
        KeyCode key_code  = datum->event.u.u.detail;
        app->current->button = key_code;
        //execute(app);
    } else if (event_type == ButtonRelease) {
        app->current->button = 0;
    } else if (event_type == CreateNotify) {
        // attempt bind
        Window w = datum->event.u.createNotify.window;
        grab_all_keys_for_window(app, w);
    } else if (event_type == DestroyNotify) {
        Window w = datum->event.u.destroyNotify.window;
        // XXX what ?
    }

exit:
	XUnlockDisplay(app->ctrl_conn); 
	XRecordFreeData(data);
}

int main (int argc, char **argv) {
	app = malloc(sizeof(App));

	int dummy, ch;

	XRecordRange *rec_range = XRecordAllocRange();
	XRecordClientSpec client_spec = XRecordAllClients;

	app->debug = False;
	app->current = new_hotkey();

	rec_range->device_events.first = KeyPress;
	rec_range->device_events.last = DestroyNotify;

	while ((ch = getopt (argc, argv, "d")) != -1) {
		switch (ch) {
			case 'd':
				app->debug = True;
				break;
			default:
                print_usage(argv[0]);
                return EXIT_SUCCESS;
		}
	}

	if (optind < argc) {
		fprintf(stderr, "Not a command line option: '%s'\n", argv[optind]);
		print_usage (argv[0]);
		return EXIT_SUCCESS;
	}

	if (!XInitThreads()) {
		fprintf(stderr, "Failed to initialize threads.\n");
		exit (EXIT_FAILURE);
	}

	app->data_conn = XOpenDisplay(NULL);
	app->ctrl_conn = XOpenDisplay(NULL);

	if (!app->data_conn || !app->ctrl_conn) {
		fprintf(stderr, "Unable to connect to X11 display. Is $DISPLAY set?\n");
		exit (EXIT_FAILURE);
	}
	if (!XQueryExtension (app->ctrl_conn, "XTEST", &dummy, &dummy, &dummy)) {
		fprintf(stderr, "Xtst extension missing\n");
		exit (EXIT_FAILURE);
	}
	if (!XRecordQueryVersion (app->ctrl_conn, &dummy, &dummy)) {
		fprintf(stderr, "Failed to obtain xrecord version\n");
		exit (EXIT_FAILURE);
	}
	if (!XkbQueryExtension (app->ctrl_conn, &dummy, &dummy, &dummy, &dummy, &dummy)) {
		fprintf(stderr, "Failed to obtain xkb version\n");
		exit (EXIT_FAILURE);
	}

	if (app->debug != True)
		daemon (0, 0);

	sigemptyset(&app->sigset);
	sigaddset(&app->sigset, SIGINT);
	sigaddset(&app->sigset, SIGTERM);
	pthread_sigmask(SIG_BLOCK, &app->sigset, NULL);

	pthread_create(&app->sigwait_thread, NULL, sig_handler, app);

	app->record_ctx = XRecordCreateContext(app->ctrl_conn, 0, &client_spec, 1, &rec_range, 1);

	if (app->record_ctx == 0) {
		fprintf(stderr, "Failed to create xrecord context\n");
		exit (EXIT_FAILURE);
	}

	load_configuration_file(app);
	grab_all_keys(app);

	//XSync(app->ctrl_conn, False);
	XSync(app->ctrl_conn, True);

	if (!XRecordEnableContext(app->data_conn, app->record_ctx, intercept, (XPointer)app)) {
		fprintf(stderr, "Failed to enable xrecord context\n");
		exit (EXIT_FAILURE);
	}

	pthread_join(app->sigwait_thread, NULL);

	if (!XRecordFreeContext (app->ctrl_conn, app->record_ctx)) {
		fprintf(stderr, "Failed to free xrecord context\n");
	}

	if (app->debug) fprintf(stderr, "main exiting\n");
	XFree(rec_range);

	XCloseDisplay(app->ctrl_conn);
	XCloseDisplay(app->data_conn);
	free_app(app);
	free(app);

	return EXIT_SUCCESS;
}

void free_app(App *app) {
    for (khint_t k = kh_begin(app->config); k != kh_end(app->config); ++k) {
        if (kh_exist(app->config, k)) {
            khash_t(Mappings)* mapping = kh_value(app->config, k);
            for (khint_t k2 = kh_begin(mapping); k2 != kh_end(mapping); ++k2) {
                if (kh_exist(mapping, k2)) {
                    char * class = kh_key(mapping, k2);
                    Hotkey *h = kh_value(mapping, k2);
                    kh_del(Mappings, mapping, k2);
                    free(class);
                    free(h);
                }
            }
            kh_del(Config, app->config, k);
            kh_destroy(Mappings, mapping);
        }
    }
    kh_destroy(Config, app->config);
    free(app->current);
}

void *sig_handler(void *user_data) {
	App *app = (App*)user_data;
	int sig;

	if (app->debug)
	    fprintf(stderr, "sig_handler running...\n");

	sigwait(&app->sigset, &sig);

	if (app->debug)
	    fprintf(stderr, "Caught signal %d!\n", sig);

	XLockDisplay(app->ctrl_conn);

	if (!XRecordDisableContext (app->ctrl_conn, app->record_ctx)) {
		fprintf(stderr, "Failed to disable xrecord context\n");
		exit(EXIT_FAILURE);
	}

	XSync(app->ctrl_conn, False);
	XUnlockDisplay(app->ctrl_conn);

	if (app->debug)
	    fprintf(stderr, "sig_handler exiting...\n");

	return NULL;
}


void print_usage (const char *program_name) {
	fprintf(stderr, "Usage: %s [-d] [-e <mapping>]\n", program_name);
	fprintf(stderr, "Runs as a daemon unless -d flag is set\n");
}
