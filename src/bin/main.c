#include "private.h"

#ifdef HAVE_PO
#include <locale.h>
#endif 

#include <Ecore_Getopt.h>
#include <Elementary.h>
#include "main.h"
#include "win.h"
#include "termio.h"
#include "termpty.h"
#include "config.h"
#include "controls.h"
#include "media.h"
#include "theme.h"
#include "ipc.h"
#include "sel.h"
#include "miniview.h"
#include "gravatar.h"
#include "keyin.h"

int terminology_starting_up;
int _log_domain = -1;
Eina_Bool multisense_available = EINA_TRUE;

static Config *_main_config = NULL;

static void
_set_instance_theme(Ipc_Instance *inst)
{
   char path[PATH_MAX];
   char theme_name[PATH_MAX];
   const char *theme_path = (const char *)&path;

   if (!inst->theme)
     return;

   if (eina_str_has_suffix(inst->theme, ".edj"))
     eina_strlcpy(theme_name, inst->theme, sizeof(theme_name));
   else
     snprintf(theme_name, sizeof(theme_name), "%s.edj", inst->theme);

   if (strchr(theme_name, '/'))
     eina_strlcpy(path, theme_name, sizeof(path));
   else
     theme_path = theme_path_get(theme_name);

   eina_stringshare_replace(&(inst->config->theme), theme_path);
   inst->config->temporary = EINA_TRUE;
}

#define IPC_INSTANCE_OPT_UNSET  0xff

static void
_configure_instance(Ipc_Instance *inst)
{
   Config *config = inst->config;

   _set_instance_theme(inst);

   if (inst->colorscheme)
   {
        eina_stringshare_replace(&(config->color_scheme_name), inst->colorscheme);
        config_compute_color_scheme(config);
        config->temporary = EINA_TRUE;
   }

   if (inst->background)
     {
        eina_stringshare_replace(&(config->background), inst->background);
        config->temporary = EINA_TRUE;
     }

   if (inst->font)
     {
        char *p = strchr(inst->font, '/');
        if (p)
          {
             int sz;
             char *fname = alloca(p - inst->font + 1);

             strncpy(fname, inst->font, p - inst->font);
             fname[p - inst->font] = '\0';
             sz = atoi(p+1);
             if (sz > 0)
               inst->config->font.size = sz;
             eina_stringshare_replace(&(inst->config->font.name), fname);
             inst->config->font.bitmap = EINA_FALSE;
          }
        else
          {
             char buf[4096], *file;
             Eina_List *files;
             int n = strlen(inst->font);
             Eina_Bool found = EINA_FALSE;

             snprintf(buf, sizeof(buf), "%s/fonts", elm_app_data_dir_get());
             files = ecore_file_ls(buf);
             EINA_LIST_FREE(files, file)
               {
                  if (n > 0)
                    {
                       if (!strncasecmp(file, inst->font, n))
                         {
                            n = -1;
                            eina_stringshare_replace(&(inst->config->font.name), file);
                            inst->config->font.bitmap = EINA_TRUE;
                            found = EINA_TRUE;
                         }
                    }
                  free(file);
               }
             if (!found)
               {
                  ERR("font '%s' not found in %s", inst->font, buf);
               }
          }
        config->font_set = EINA_TRUE;
        config->temporary = EINA_TRUE;
     }

   if (inst->login_shell != IPC_INSTANCE_OPT_UNSET)
     {
        inst->config->login_shell = inst->login_shell;
        inst->config->temporary = EINA_TRUE;
     }

   if (inst->xterm_256color)
     {
        inst->config->xterm_256color = EINA_TRUE;
        inst->config->temporary = EINA_TRUE;
     }
   if (inst->video_mute != IPC_INSTANCE_OPT_UNSET)
     {
        config->mute = inst->video_mute;
        config->temporary = EINA_TRUE;
     }
   if (inst->cursor_blink != IPC_INSTANCE_OPT_UNSET)
     {
        config->disable_cursor_blink = !inst->cursor_blink;
        config->temporary = EINA_TRUE;
     }
   if (inst->visual_bell != IPC_INSTANCE_OPT_UNSET)
     {
        config->disable_visual_bell = !inst->visual_bell;
        config->temporary = EINA_TRUE;
     }
   if (inst->active_links != IPC_INSTANCE_OPT_UNSET)
     {
        config->active_links = !!inst->active_links;
        config->active_links_email = inst->config->active_links;
        config->active_links_file = inst->config->active_links;
        config->active_links_url = inst->config->active_links;
        config->active_links_escape = inst->config->active_links;
        config->temporary = EINA_TRUE;
     }
}

static void
_check_multisense(void)
{
   int enabled;
   Eina_Bool setting = edje_audio_channel_mute_get(EDJE_CHANNEL_EFFECT);

   /* older versions of efl have no capability for determining whether multisense support
    * is available
    * to check, attempt to set mute on a channel and check the value: if the value has not been
    * set then the multisense codepath is disabled
    *
    * this is a no-op in either case, as the function only sets an internal variable and returns
    */
   for (enabled = 0; enabled < 2; enabled++)
     {
        edje_audio_channel_mute_set(EDJE_CHANNEL_EFFECT, enabled);
        if (enabled != edje_audio_channel_mute_get(EDJE_CHANNEL_EFFECT))
          multisense_available = EINA_FALSE;
     }
   edje_audio_channel_mute_set(EDJE_CHANNEL_EFFECT, setting);
}

Config *
main_config_get(void)
{
   return _main_config;
}


static void
main_ipc_new(Ipc_Instance *inst)
{
   Win *wn;
   Term *term;
   Config *config;
   int pargc = 0, nargc, i;
   char **pargv = NULL, **nargv = NULL, geom[256];
   Evas_Object *win;

   if (inst->startup_id)
     {
        char buf[4096];

        snprintf(buf, sizeof(buf), "DESKTOP_STARTUP_ID=%s", inst->startup_id);
        putenv(strdup(buf));
     }
   ecore_app_args_get(&pargc, &pargv);
   nargc = 1;

   if (inst->cd) nargc += 2;
   if (inst->background) nargc += 2;
   if (inst->name) nargc += 2;
   if (inst->role) nargc += 2;
   if (inst->title) nargc += 2;
   if (inst->font) nargc += 2;
   if (inst->startup_split) nargc += 2;
   if ((inst->pos) || (inst->w > 0) || (inst->h > 0)) nargc += 2;
   if (inst->login_shell) nargc += 1;
   if (inst->fullscreen) nargc += 1;
   if (inst->iconic) nargc += 1;
   if (inst->borderless) nargc += 1;
   if (inst->override) nargc += 1;
   if (inst->maximized) nargc += 1;
   if (inst->hold) nargc += 1;
   if (inst->nowm) nargc += 1;
   if (inst->xterm_256color) nargc += 1;
   if (inst->active_links) nargc += 1;
   if (inst->video_mute) nargc += 1;
   if (inst->cursor_blink) nargc += 1;
   if (inst->visual_bell) nargc += 1;
   if (inst->cmd) nargc += 2;
   if (inst->theme) nargc += 2;
   if (inst->colorscheme) nargc += 2;

   nargv = calloc(nargc + 1, sizeof(char *));
   if (!nargv) return;

   i = 0;
   nargv[i++] = pargv[0];
   if (inst->cd)
     {
        nargv[i++] = "-d";
        nargv[i++] = (char *)inst->cd;
     }
   if (inst->background)
     {
        nargv[i++] = "-b";
        nargv[i++] = (char *)inst->background;
     }
   if (inst->name)
     {
        nargv[i++] = "-n";
        nargv[i++] = (char *)inst->name;
     }
   if (inst->theme)
     {
        nargv[i++] = "-t";
        nargv[i++] = (char *)inst->theme;
     }
   if (inst->colorscheme)
     {
        nargv[i++] = "--colorscheme";
        nargv[i++] = (char *)inst->colorscheme;
     }
   if (inst->role)
     {
        nargv[i++] = "-r";
        nargv[i++] = (char *)inst->role;
     }
   if (inst->title)
     {
        nargv[i++] = "-T";
        nargv[i++] = (char *)inst->title;
     }
   if (inst->font)
     {
        nargv[i++] = "-f";
        nargv[i++] = (char *)inst->font;
     }
   if (inst->startup_split)
     {
        nargv[i++] = "-S";
        nargv[i++] = (char *)inst->startup_split;
     }
   if ((inst->pos) || (inst->w > 0) || (inst->h > 0))
     {
        if (!inst->pos)
          snprintf(geom, sizeof(geom), "%ix%i", inst->w, inst->h);
        else
          {
             if ((inst->w > 0) && (inst->h > 0))
               {
                  if (inst->x >= 0)
                    {
                       if (inst->y >= 0)
                         snprintf(geom, sizeof(geom), "%ix%i+%i+%i",
                                  inst->w, inst->h, inst->x, inst->y);
                       else
                         snprintf(geom, sizeof(geom), "%ix%i+%i%i",
                                  inst->w, inst->h, inst->x, inst->y);
                    }
                  else
                    {
                       if (inst->y >= 0)
                         snprintf(geom, sizeof(geom), "%ix%i%i+%i",
                                  inst->w, inst->h, inst->x, inst->y);
                       else
                         snprintf(geom, sizeof(geom), "%ix%i%i%i",
                                  inst->w, inst->h, inst->x, inst->y);
                    }
               }
             else
               {
                  if (inst->x >= 0)
                    {
                       if (inst->y >= 0)
                         snprintf(geom, sizeof(geom), "+%i+%i",
                                  inst->x, inst->y);
                       else
                         snprintf(geom, sizeof(geom), "+%i%i",
                                  inst->x, inst->y);
                    }
                  else
                    {
                       if (inst->y >= 0)
                         snprintf(geom, sizeof(geom), "%i+%i",
                                  inst->x, inst->y);
                       else
                         snprintf(geom, sizeof(geom), "%i%i",
                                  inst->x, inst->y);
                    }
               }
          }
        nargv[i++] = "-g";
        nargv[i++] = geom;
     }
   if ((inst->login_shell != IPC_INSTANCE_OPT_UNSET)
       && (inst->login_shell != 0))
     {
        nargv[i++] = "-l";
     }
   if (inst->fullscreen)
     {
        nargv[i++] = "-F";
     }
   if (inst->iconic)
     {
        nargv[i++] = "-I";
     }
   if (inst->borderless)
     {
        nargv[i++] = "-B";
     }
   if (inst->override)
     {
        nargv[i++] = "-O";
     }
   if (inst->maximized)
     {
        nargv[i++] = "-M";
     }
   if (inst->hold)
     {
        nargv[i++] = "-H";
     }
   if (inst->nowm)
     {
        nargv[i++] = "-W";
     }
   if (inst->xterm_256color)
     {
        nargv[i++] = "-2";
     }
   if ((inst->active_links != IPC_INSTANCE_OPT_UNSET)
       && (inst->active_links != 0))
     {
        nargv[i++] = "--active-links";
     }
   if (inst->cmd)
     {
        nargv[i++] = "-e";
        nargv[i++] = (char *)inst->cmd;
     }
   if ((inst->video_mute != IPC_INSTANCE_OPT_UNSET)
       && (inst->video_mute != 0))
     {
        nargv[i++] = "-m";
     }
   if ((inst->cursor_blink != IPC_INSTANCE_OPT_UNSET)
       && (inst->cursor_blink != 0))
     {
        nargv[i++] = "-c";
     }
   if ((inst->visual_bell != IPC_INSTANCE_OPT_UNSET)
       && (inst->visual_bell != 0))
     {
        nargv[i++] = "-G";
     }


   ecore_app_args_set(nargc, (const char **)nargv);
   wn = win_new(inst->name, inst->role, inst->title, inst->icon_name,
                _main_config, inst->fullscreen, inst->iconic,
                inst->borderless, inst->override, inst->maximized);
   if (!wn)
     {
        CRITICAL(_("Could not create window"));
        ecore_app_args_set(pargc, (const char **)pargv);
        free(nargv);
        return;
     }

   win = win_evas_object_get(wn);
   config = win_config_get(wn);
   inst->config = config;

   _configure_instance(inst);

   if (inst->w <= 0) inst->w = 80;
   if (inst->h <= 0) inst->h = 24;
   term = term_new(wn, config, inst->cmd, inst->login_shell,
                   inst->cd, inst->w, inst->h, inst->hold,
                   inst->title);
   if (!term)
     {
        CRITICAL(_("Could not create terminal widget"));
        win_free(wn);
        ecore_app_args_set(pargc, (const char **)pargv);
        free(nargv);
        return;
     }

   if (win_term_set(wn, term) < 0)
     return;

   main_trans_update();
   main_media_update(config);
   if (inst->pos)
     {
        int screen_w, screen_h;

        elm_win_screen_size_get(win, NULL, NULL, &screen_w, &screen_h);
        if (inst->x < 0) inst->x = screen_w + inst->x;
        if (inst->y < 0) inst->y = screen_h + inst->y;
        evas_object_move(win, inst->x, inst->y);
     }
   win_sizing_handle(wn);
   evas_object_show(win_evas_object_get(wn));
   if (inst->nowm)
     ecore_evas_focus_set
     (ecore_evas_ecore_evas_get(evas_object_evas_get(win)), 1);
   ecore_app_args_set(pargc, (const char **)pargv);
   free(nargv);
   unsetenv("DESKTOP_STARTUP_ID");
}

static Ecore_Getopt options = {
   PACKAGE_NAME,
   "%prog [options]",
   PACKAGE_VERSION,
   gettext_noop("(C) 2012-%d Carsten Haitzler and others"),
   "BSD 2-Clause",
   gettext_noop("Terminal emulator written with Enlightenment Foundation Libraries"),
   EINA_TRUE,
   {
      ECORE_GETOPT_STORE_STR ('b', "background",
                              gettext_noop("Use the named file as a background wallpaper")),
      ECORE_GETOPT_STORE_STR ('d', "current-directory",
                              gettext_noop("Change to directory for execution of terminal command")),
      ECORE_GETOPT_BREAK_STR ('e', "exec",
                              gettext_noop("Command to execute. Defaults to $SHELL (or passwd shell or /bin/sh)")),
      ECORE_GETOPT_STORE_STR ('t', "theme",
                              gettext_noop("Use the named edje theme or path to theme file")),
      ECORE_GETOPT_STORE_STR ('C', "colorscheme",
                              gettext_noop("Use the named color scheme")),
      ECORE_GETOPT_STORE_STR ('g', "geometry",
                              gettext_noop("Terminal geometry to use (eg 80x24 or 80x24+50+20 etc.)")),
      ECORE_GETOPT_STORE_STR ('n', "name",
                              gettext_noop("Set window name")),
      ECORE_GETOPT_STORE_STR ('r', "role",
                              gettext_noop("Set window role")),
      ECORE_GETOPT_STORE_STR ('T', "title",
                              gettext_noop("Set window title")),
      ECORE_GETOPT_STORE_STR ('i', "icon-name",
                              gettext_noop("Set icon name")),
      ECORE_GETOPT_STORE_STR ('f', "font",
                              gettext_noop("Set font (NAME/SIZE for scalable, NAME for bitmap)")),
      ECORE_GETOPT_STORE_STR ('S', "split",
                              gettext_noop("Split the terminal window."
                              " 'v' for vertical and 'h' for horizontal."
                              " Can be used multiple times. eg -S vhvv or --split hv"
                              " More description available on the man page")),
      ECORE_GETOPT_STORE_BOOL('l', "login",
                              gettext_noop("Run the shell as a login shell")),
      ECORE_GETOPT_STORE_BOOL('m', "video-mute",
                              gettext_noop("Set mute mode for video playback")),
      ECORE_GETOPT_STORE_BOOL('c', "cursor-blink",
                              gettext_noop("Set cursor blink mode")),
      ECORE_GETOPT_STORE_BOOL('G', "visual-bell",
                              gettext_noop("Set visual bell mode")),
      ECORE_GETOPT_STORE_TRUE('F', "fullscreen",
                              gettext_noop("Go into the fullscreen mode from the start")),
      ECORE_GETOPT_STORE_TRUE('I', "iconic",
                              gettext_noop("Start iconified")),
      ECORE_GETOPT_STORE_TRUE('B', "borderless",
                              gettext_noop("Start borderless")),
      ECORE_GETOPT_STORE_TRUE('O', "override",
                              gettext_noop("Start as a override-redirect window")),
      ECORE_GETOPT_STORE_TRUE('M', "maximized",
                              gettext_noop("Start maximized")),
      ECORE_GETOPT_STORE_TRUE('W', "nowm",
                              gettext_noop("Terminology is run without a window manager")),
      ECORE_GETOPT_STORE_TRUE('H', "hold",
                              gettext_noop("Do not exit when the command process exits")),
      ECORE_GETOPT_STORE_TRUE('s', "single",
                              gettext_noop("Force single executable if multi-instance is enabled")),
      ECORE_GETOPT_STORE_TRUE('2', "256color",
                              gettext_noop("Set TERM to 'xterm-256color' instead of 'xterm'")),
      ECORE_GETOPT_STORE_DOUBLE('\0', "scale",
                              gettext_noop("Set scaling factor")),
      ECORE_GETOPT_STORE_BOOL('\0', "active-links",
                              gettext_noop("Highlight links")),
      ECORE_GETOPT_STORE_BOOL('\0', "no-wizard",
                              gettext_noop("Do not display wizard on start up")),

      ECORE_GETOPT_VERSION   ('V', "version"),
      ECORE_GETOPT_COPYRIGHT ('\0', "copyright"),
      ECORE_GETOPT_LICENSE   ('L', "license"),
      ECORE_GETOPT_HELP      ('h', "help"),
      ECORE_GETOPT_SENTINEL
   }
};

#if ENABLE_NLS
static void
_translate_options(void)
{
   options.copyright = eina_stringshare_printf(gettext(options.copyright),
                                               2020);

   Ecore_Getopt_Desc *desc = (Ecore_Getopt_Desc *) options.descs;
   while ((desc->shortname != '\0') || (desc->longname) ||
          (desc->action == ECORE_GETOPT_ACTION_CATEGORY))
     {
        if (desc->help)
          {
             switch (desc->action)
               {
                case ECORE_GETOPT_ACTION_VERSION:
                   desc->help = _("show program version");
                   break;
                case ECORE_GETOPT_ACTION_COPYRIGHT:
                   desc->help = _("show copyright");
                   break;
                case ECORE_GETOPT_ACTION_LICENSE:
                   desc->help = _("show license");
                   break;
                case ECORE_GETOPT_ACTION_HELP:
                   desc->help = _("show this message");
                   break;
                default:
                   desc->help = gettext(desc->help);
               }
          }
        desc++;
     }
}
#endif

#if defined(BINARY_TYFUZZ) || defined(BINARY_TYTEST)
static void
_log_void(const Eina_Log_Domain *_d EINA_UNUSED,
          Eina_Log_Level level EINA_UNUSED,
          const char *_file EINA_UNUSED,
          const char *_fnc EINA_UNUSED,
          int _line EINA_UNUSED,
          const char *fmt EINA_UNUSED,
          void *_data EINA_UNUSED,
          va_list args EINA_UNUSED)
{
}
#endif

static void
_start(Ipc_Instance *instance, Eina_Bool need_scale_wizard)
{
   Win *wn;
   Evas_Object *win;
   Term *term;
   Config *config;

   wn = win_new(instance->name, instance->role, instance->title,
                instance->icon_name, instance->config,
                instance->fullscreen, instance->iconic, instance->borderless,
                instance->override, instance->maximized);
   // set an env so terminal apps can detect they are in terminology :)
   putenv("TERMINOLOGY=1");

   config_del(instance->config);
   config = NULL;
   if (!wn)
     {
        CRITICAL(_("Could not create window"));
        goto exit;
     }

   config = win_config_get(wn);

   term = term_new(wn, config, instance->cmd, instance->login_shell,
                   instance->cd,
                   instance->w, instance->h, instance->hold, instance->title);
   if (!term)
     {
        CRITICAL(_("Could not create terminal widget"));
        config = NULL;
        goto exit;
     }

   if (win_term_set(wn, term) < 0)
     {
        goto exit;
     }

   main_trans_update();
   main_media_update(config);
   win_sizing_handle(wn);
   win = win_evas_object_get(wn);
   evas_object_show(win);
   if (instance->startup_split)
     {
        unsigned int i = 0;
        Term *next = term;

        for (i = 0; i < strlen(instance->startup_split); i++)
          {
             char *cmd = NULL;

             if (instance->startup_split[i] == 'v')
               {
                  cmd = eina_list_data_get(instance->cmds);
                  split_vertically(term_termio_get(next), cmd);
                  instance->cmds = eina_list_remove_list(instance->cmds,
                                                         instance->cmds);
               }
             else if (instance->startup_split[i] == 'h')
               {
                  cmd = eina_list_data_get(instance->cmds);
                  split_horizontally(term_termio_get(next), cmd);
                  instance->cmds = eina_list_remove_list(instance->cmds,
                                                         instance->cmds);
               }
             else if (instance->startup_split[i] == '-')
               next = term_next_get(next);
             else
               {
                  CRITICAL(_("invalid argument found for option -S/--split."
                             " See --help"));
                  goto end;
               }
          }
     }
   if (instance->pos)
     {
        int screen_w, screen_h;

        elm_win_screen_size_get(win, NULL, NULL, &screen_w, &screen_h);
        if (instance->x < 0) instance->x = screen_w + instance->x;
        if (instance->y < 0) instance->y = screen_h + instance->y;
        evas_object_move(win, instance->x, instance->y);
     }
   if (instance->nowm)
      ecore_evas_focus_set(ecore_evas_ecore_evas_get(
            evas_object_evas_get(win)), 1);

   controls_init();

   if (need_scale_wizard)
     win_scale_wizard(win, term);

   terminology_starting_up = EINA_FALSE;


end:
   return;
exit:
   ecore_main_loop_quit();
}

struct Instance_Add {
     Ipc_Instance *instance;
     char **argv;
     Eina_Bool result;
     Eina_Bool timedout;
     Eina_Bool done;
     pthread_mutex_t lock;
};

static void
_instance_add_free(struct Instance_Add *add)
{
   if (!add)
     return;

   pthread_mutex_destroy(&add->lock);
   free(add);
}


static void *
_instance_sleep(void *data)
{
   struct Instance_Add *add = data;
   Eina_Bool timedout = EINA_FALSE;

   sleep(2);
   pthread_mutex_lock(&add->lock);
   if (!add->done)
     timedout = add->timedout = EINA_TRUE;
   pthread_mutex_unlock(&add->lock);
   if (timedout)
     {
        /* ok, we waited 2 seconds without any answer,
         * remove the unix socket and restart terminology from scratch in a
         * better state */
        ipc_instance_conn_free();
        errno = 0;
        execv(add->argv[0], add->argv);
        ERR("execv failed on '%s': %s", add->argv[0], strerror(errno));
     }
   else
     {
        _instance_add_free(add);
     }

   return NULL;
}

static Eina_Bool
_instance_add_waiter(Ipc_Instance *instance,
                     char **argv)
{
   struct Instance_Add *add;
   Eina_Bool timedout = EINA_FALSE;
   Eina_Bool result = EINA_TRUE;
   pthread_t thr;

   add = calloc(1, sizeof(*add));
   if (!add)
     return EINA_FALSE;

   add->instance = instance;
   add->argv = argv;
   pthread_mutex_init(&add->lock, NULL);

   pthread_create(&thr, NULL, &_instance_sleep, add);

   /* If the unix socket is stalled, this might block */
   result = ipc_instance_add(add->instance);
   pthread_mutex_lock(&add->lock);
   /* Hoora, it did not block! */
   add->done = EINA_TRUE;
   if (add->timedout)
       {
          timedout = add->timedout = EINA_TRUE;
          result = EINA_FALSE;
       }
   pthread_mutex_unlock(&add->lock);
   if (timedout)
     _instance_add_free(add);

   return result;
}

static Eina_Bool
_start_multi(Ipc_Instance *instance,
             char **argv,
             Eina_Bool need_scale_wizard)
{
   int remote_try = 0;
   do
     {
        if (_instance_add_waiter(instance, argv))
          {
             goto exit;
          }
        /* Could not start a new window remotely,
         * let's start our own server */
        ipc_instance_new_func_set(main_ipc_new);
        if (ipc_serve())
          {
             goto normal_start;
          }
        else
          {
             DBG("IPC server: failure");
          }
        remote_try++;
     }
   while (remote_try <= 1);

normal_start:
   _start(instance, need_scale_wizard);
   return EINA_FALSE;

exit:
   return EINA_TRUE;
}


EAPI_MAIN int
elm_main(int argc, char **argv)
{
   char *geometry = NULL;
   Eina_Bool quit_option = EINA_FALSE;
   Eina_Bool single = EINA_FALSE;
   Eina_Bool no_wizard = EINA_FALSE;
   Eina_Bool cmd_options = EINA_FALSE;
   double scale = -1.0; /* unset */
   Ipc_Instance instance = {
        .login_shell = IPC_INSTANCE_OPT_UNSET,
        .active_links = IPC_INSTANCE_OPT_UNSET,
        .video_mute = IPC_INSTANCE_OPT_UNSET,
        .cursor_blink = IPC_INSTANCE_OPT_UNSET,
        .visual_bell = IPC_INSTANCE_OPT_UNSET,
        .startup_id = getenv("DESKTOP_STARTUP_ID"),
        .w = 1,
        .h = 1,
   };
   Ecore_Getopt_Value values[] = {
     ECORE_GETOPT_VALUE_STR(instance.background),       /* -b, --background */
     ECORE_GETOPT_VALUE_STR(instance.cd),               /* -d, --current-directory */
     ECORE_GETOPT_VALUE_BOOL(cmd_options),              /* -e, --exec */
     ECORE_GETOPT_VALUE_STR(instance.theme),            /* -t, --theme */
     ECORE_GETOPT_VALUE_STR(instance.colorscheme),      /* -C, --colorscheme */
     ECORE_GETOPT_VALUE_STR(geometry),                  /* -g, --geometry */
     ECORE_GETOPT_VALUE_STR(instance.name),             /* -n, --name */
     ECORE_GETOPT_VALUE_STR(instance.role),             /* -r, --role */
     ECORE_GETOPT_VALUE_STR(instance.title),            /* -T, --title */
     ECORE_GETOPT_VALUE_STR(instance.icon_name),        /* -i, --icon-name */
     ECORE_GETOPT_VALUE_STR(instance.font),             /* -f, --font */
     ECORE_GETOPT_VALUE_STR(instance.startup_split),    /* -S, --split */
     ECORE_GETOPT_VALUE_BOOL(instance.login_shell),     /* -l, --login */
     ECORE_GETOPT_VALUE_BOOL(instance.video_mute),      /* -m, --video-mute */
     ECORE_GETOPT_VALUE_BOOL(instance.cursor_blink),    /* -c, --cursor-blink */
     ECORE_GETOPT_VALUE_BOOL(instance.visual_bell),     /* -G, --visual-bell */
     ECORE_GETOPT_VALUE_BOOL(instance.fullscreen),      /* -F, --fullscreen */
     ECORE_GETOPT_VALUE_BOOL(instance.iconic),          /* -I, --iconic */
     ECORE_GETOPT_VALUE_BOOL(instance.borderless),      /* -B, --borderless */
     ECORE_GETOPT_VALUE_BOOL(instance.override),        /* -O, --override */
     ECORE_GETOPT_VALUE_BOOL(instance.maximized),       /* -M, --maximized */
     ECORE_GETOPT_VALUE_BOOL(instance.nowm),            /* -W, --nowm */
     ECORE_GETOPT_VALUE_BOOL(instance.hold),            /* -H, --hold */
     ECORE_GETOPT_VALUE_BOOL(single),                   /* -s, --single */
     ECORE_GETOPT_VALUE_BOOL(instance.xterm_256color),  /* -2, --256color */
     ECORE_GETOPT_VALUE_DOUBLE(scale),                  /* --scale */
     ECORE_GETOPT_VALUE_BOOL(instance.active_links),    /* --active-links */
     ECORE_GETOPT_VALUE_BOOL(no_wizard),                /* --no-wizard */

     ECORE_GETOPT_VALUE_BOOL(quit_option),              /* -v, --version */
     ECORE_GETOPT_VALUE_BOOL(quit_option),              /* --copyright */
     ECORE_GETOPT_VALUE_BOOL(quit_option),              /* -L, --license */
     ECORE_GETOPT_VALUE_BOOL(quit_option),              /* -h, --help */

     ECORE_GETOPT_VALUE_NONE
   };
   int args, retval = EXIT_SUCCESS;
   Eina_Bool size_set = EINA_FALSE;
   Eina_Bool need_scale_wizard = utils_need_scale_wizard();

   terminology_starting_up = EINA_TRUE;

#if defined(BINARY_TYFUZZ) || defined(BINARY_TYTEST)
   eina_log_print_cb_set(_log_void, NULL);
#endif

   elm_config_item_select_on_focus_disabled_set(EINA_TRUE);

   elm_language_set("");
   elm_policy_set(ELM_POLICY_QUIT, ELM_POLICY_QUIT_LAST_WINDOW_CLOSED);
   elm_app_compile_bin_dir_set(PACKAGE_BIN_DIR);
   elm_app_compile_lib_dir_set(PACKAGE_LIB_DIR);
   elm_app_compile_data_dir_set(PACKAGE_DATA_DIR);
#if ENABLE_NLS
   elm_app_compile_locale_set(LOCALEDIR);
#endif
   elm_app_info_set(elm_main, "terminology", "themes/default.edj");

#if ENABLE_NLS
   bindtextdomain(PACKAGE, elm_app_locale_dir_get());
   textdomain(PACKAGE);
   _translate_options();
#else
   options.copyright = "(C) 2012-2020 Carsten Haitzler and others";
#endif

   _log_domain = eina_log_domain_register("terminology", NULL);
   if (_log_domain < 0)
     {
        EINA_LOG_CRIT(_("Could not create logging domain '%s'"), "terminology");
        elm_shutdown();
        return EXIT_FAILURE;
     }

   config_init();
   colors_init();

   _main_config = config_load();
   if (key_bindings_load(_main_config) < 0)
     {
        CRITICAL(_("Could not initialize key bindings"));
        retval = EXIT_FAILURE;
        goto end;
     }

   emile_init();
   ecore_con_init();
   ecore_con_url_init();

   ipc_init();

   instance.config = config_fork(_main_config);

   args = ecore_getopt_parse(&options, values, argc, argv);
   if (args < 0)
     {
        CRITICAL(_("Could not parse command line options"));
        retval = EXIT_FAILURE;
        goto end;
     }

   if (scale > 0.0)
     {
        elm_config_scale_set(scale);
        elm_config_all_flush();
        instance.config->temporary = EINA_TRUE;
     }

   if (no_wizard)
     need_scale_wizard = EINA_FALSE;

   if (quit_option)
     goto end;

   if (cmd_options)
     {
        int i;

        if (args == argc)
          {
             CRITICAL(_("option %s requires an argument!"), argv[args-1]);
             CRITICAL(_("invalid options found. See --help"));
             goto end;
          }

        if (instance.startup_split)
          {
             for(i = args+1; i < argc; i++)
               instance.cmds = eina_list_append(instance.cmds, argv[i]);
             instance.cmd = argv[args];
          }
        else
          {
             Eina_Strbuf *strb;
             strb = eina_strbuf_new();
             eina_strbuf_append(strb, argv[args]);
             for(i = args+1; i < argc; i++)
               {
                  eina_strbuf_append_char(strb, ' ');
                  eina_strbuf_append(strb, argv[i]);
               }
             instance.cmd = eina_strbuf_string_steal(strb);
             eina_strbuf_free(strb);
          }
     }

   _check_multisense();

   _configure_instance(&instance);


   if (geometry)
     {
        if (sscanf(geometry,"%ix%i+%i+%i", &instance.w, &instance.h,
                   &instance.x, &instance.y) == 4)
          {
             instance.pos = EINA_TRUE;
             size_set = EINA_TRUE;
          }
        else if (sscanf(geometry,"%ix%i-%i+%i", &instance.w, &instance.h,
                        &instance.x, &instance.y) == 4)
          {
             instance.x = -instance.x;
             instance.pos = EINA_TRUE;
             size_set = EINA_TRUE;
          }
        else if (sscanf(geometry,"%ix%i-%i-%i", &instance.w, &instance.h,
                        &instance.x, &instance.y) == 4)
          {
             instance.x = -instance.x;
             instance.y = -instance.y;
             instance.pos = EINA_TRUE;
             size_set = EINA_TRUE;
          }
        else if (sscanf(geometry,"%ix%i+%i-%i", &instance.w, &instance.h,
                        &instance.x, &instance.y) == 4)
          {
             instance.y = -instance.y;
             instance.pos = EINA_TRUE;
             size_set = EINA_TRUE;
          }
        else if (sscanf(geometry,"%ix%i", &instance.w, &instance.h) == 2)
          {
             size_set = EINA_TRUE;
          }
        else if (sscanf(geometry,"+%i+%i", &instance.x, &instance.y) == 2)
          {
             instance.pos = EINA_TRUE;
          }
        else if (sscanf(geometry,"-%i+%i", &instance.x, &instance.y) == 2)
          {
             instance.x = -instance.x;
             instance.pos = EINA_TRUE;
          }
        else if (sscanf(geometry,"+%i-%i", &instance.x, &instance.y) == 2)
          {
             instance.y = -instance.y;
             instance.pos = EINA_TRUE;
          }
        else if (sscanf(geometry,"-%i-%i", &instance.x, &instance.y) == 2)
          {
             instance.x = -instance.x;
             instance.y = -instance.y;
             instance.pos = EINA_TRUE;
          }
     }

   if (!size_set)
     {
        if (instance.config->custom_geometry)
          {
             instance.w = instance.config->cg_width;
             instance.h = instance.config->cg_height;
          }
        else
          {
             instance.w = 80;
             instance.h = 24;
          }
     }

   elm_theme_extension_add(NULL, theme_path_get("default.edj"));
   elm_theme_extension_add(NULL, config_theme_path_get(instance.config));

   if (instance.login_shell == IPC_INSTANCE_OPT_UNSET)
     instance.login_shell = instance.config->login_shell;

   if ((!single) && (instance.config->multi_instance))
     {
        char cwdbuf[4096];

        if (!instance.cd)
          instance.cd = getcwd(cwdbuf, sizeof(cwdbuf));
        if (_start_multi(&instance, argv, need_scale_wizard))
          goto end;
     }
   else
     {
        _start(&instance, need_scale_wizard);
     }
   elm_run();

   ecore_con_url_shutdown();
   ecore_con_shutdown();
   emile_shutdown();

   instance.config = NULL;
 end:
   if (instance.config)
     {
        config_del(instance.config);
        instance.config = NULL;
     }

   ipc_shutdown();

   termpty_shutdown();
   miniview_shutdown();
   gravatar_shutdown();

   windows_free();

   config_del(_main_config);
   key_bindings_shutdown();
   colors_shutdown();
   config_shutdown();
   controls_shutdown();
   eina_log_domain_unregister(_log_domain);
   _log_domain = -1;


#if ENABLE_NLS
   eina_stringshare_del(options.copyright);
#endif

   elm_shutdown();

   return retval;
}
ELM_MAIN()
