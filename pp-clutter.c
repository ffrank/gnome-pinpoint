/*
 * Pinpoint: A small-ish presentation tool
 *
 * Copyright (C) 2010 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Written by: Øyvind Kolås <pippin@linux.intel.com>
 *             Damien Lespiau <damien.lespiau@intel.com>
 *             Emmanuele Bassi <ebassi@linux.intel.com>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pinpoint.h"

#include <gio/gio.h>
#ifdef USE_CLUTTER_GST
#include <clutter-gst/clutter-gst.h>
#endif
#ifdef USE_DAX
#include <dax/dax.h>
#include "pp-super-aa.h"
#endif
#include <stdlib.h>
#include <string.h>

#define DEFAULT_WIDTH (800)
#define DEFAULT_HEIGHT (600)

void cairo_renderer_unset_cr (PinPointRenderer *pp_renderer);

void cairo_renderer_set_cr (PinPointRenderer *pp_renderer,
                            cairo_t          *ctx,
                            float             width,
                            float             height);

void cairo_renderer_render_page (void          *renderer,
                                 PinPointPoint *point);

/* #define QUICK_ACCESS_LEFT - uncomment to move speed access from top to left,
 *                             useful on meego netbook
 */

#define RESTDEPTH   -9000.0
#define RESTX        4600.0
#define STARTPOS    -3000.0

/* The maximum of these is used, either 80% of the slide time
   or 20seconds left of the slide
 */
#define SLIDE_WARN_TIME          2.0
#define SLIDE_WARN_THRESHOLD     0.33
#define OPACITY_OK               0
#define OPACITY_PAST_THRESHOLD   96
#define OPACITY_OVER_TIME        140

#define PREVIEW_WIDTH  640
#define PREVIEW_HEIGHT 480

static ClutterColor c_prog_bg =    {0x11,0x11,0x11,0xff};
static ClutterColor c_prog_slide = {0xff,0xff,0xff,0x77};
static ClutterColor c_prog_time =  {0xff,0xff,0xff,0x55};

static ClutterColor black = {0x00,0x00,0x00,0xff};
static ClutterColor white = {0xff,0xff,0xff,0xff};
static ClutterColor red   = {0xff,0x00,0x00,0xff};

static ClutterColor gray  = {0x80,0x80,0x80,0xff};
static ClutterColor lightgray  = {0xdd,0xdd,0xdd,0xff};


#ifdef HAVE_PDF
PinPointRenderer *pp_cairo_renderer   (void);
#endif

typedef struct _ClutterRenderer
{
  PinPointRenderer renderer;

  GtkApplication *application;

  GHashTable      *bg_cache;    /* only load the same backgrounds once */

  GtkWidget       *window;
  ClutterActor    *stage;
  ClutterActor    *root;

  ClutterActor    *background;
  ClutterActor    *midground;
  ClutterActor    *shading;
  ClutterActor    *foreground;

  ClutterActor    *json_layer;
  ClutterActor    *curtain;

  ClutterActor    *commandline;
  ClutterActor    *commandline_shading;

  GTimer          *timer;
  gboolean         timer_paused;
  int              total_seconds;
  gboolean         autoadvance;

  gboolean         speaker_mode;
  GtkWidget       *speaker_window;
  ClutterActor    *speaker_screen;

  gdouble          slide_start_time;

  ClutterActor    *speaker_buttons_group;
  ClutterActor    *speaker_buttonbar;
  ClutterActor    *speaker_speakerscreen;
  ClutterActor    *speaker_rehearse;
  ClutterActor    *speaker_autoadvance;
  ClutterActor    *speaker_start;
  ClutterActor    *speaker_pause;
  ClutterActor    *speaker_fullscreen;

  ClutterActor    *speaker_notes;

  ClutterActor    *speaker_preview_bar;
  ClutterActor    *speaker_prev;
  ClutterActor    *speaker_current;
  ClutterActor    *speaker_next;

  ClutterActor    *speaker_slide_prog_warning;

  ClutterActor    *speaker_prog_bg;
  ClutterActor    *speaker_prog_slide;
  ClutterActor    *speaker_prog_time;

  ClutterActor    *speaker_time_remaining;

  char *path;               /* path of the file of the GFileMonitor callback */
  float rest_y;             /* where the text can rest */

  gboolean reset;           /* tells the speaker screen update function to
                               reset all state
                             */

  PinPointRenderer *cairo_renderer;

  /* Token returned by the gtk_application_inhibit() method; passed to
   * gtk_application_uninhibit() when we leave fullscreen to tell GSM
   * it's free to turn the screen off now.
   */
  guint32           inhibit_cookie;
} ClutterRenderer;

typedef struct
{
  PinPointRenderer *renderer;
  ClutterActor     *background;
  ClutterActor     *text;
  float rest_y;     /* y coordinate when text is stationary unused */

  ClutterState     *state;
  ClutterActor     *json_slide;
  ClutterActor     *background2;
  ClutterScript    *script;
  ClutterActor     *midground;
  ClutterActor     *foreground;
  ClutterActor     *shading;

#ifdef USE_CLUTTER_GST
  ClutterGstPlayer *player;
#endif
} ClutterPointData;

#define CLUTTER_RENDERER(renderer)  ((ClutterRenderer *) renderer)


static void     leave_slide   (ClutterRenderer  *renderer,
                               gboolean          backwards);
static void     show_slide    (ClutterRenderer  *renderer,
                               gboolean          backwards);
static void     action_slide  (ClutterRenderer  *renderer);
static void     activate_commandline   (ClutterRenderer  *renderer);
static void     file_changed  (GFileMonitor     *monitor,
                               GFile            *file,
                               GFile            *other_file,
                               GFileMonitorEvent event_type,
                               ClutterRenderer  *renderer);
static void     stage_resized (ClutterActor     *actor,
                               GParamSpec       *pspec,
                               ClutterRenderer  *renderer);
static gboolean key_pressed (ClutterActor    *actor,
                             ClutterEvent    *event,
                             ClutterRenderer *renderer);
static gboolean mouse_clicked  (ClutterActor    *actor,
                                ClutterEvent    *event,
                                ClutterRenderer *renderer);
static void     pp_inhibit (ClutterRenderer *renderer,
                            gboolean         fullscreen);

static void
pp_actor_animate (ClutterActor         *actor,
                   ClutterAnimationMode  mode,
                   guint                 duration,
                   const gchar          *first_property,
                   ...)
{
  va_list args;

  clutter_actor_save_easing_state (actor);
  clutter_actor_set_easing_mode (actor, mode);
  clutter_actor_set_easing_duration (actor, duration);

  va_start (args, first_property);
  g_object_set_valist (G_OBJECT (actor), first_property, args);
  va_end (args);

  clutter_actor_restore_easing_state (actor);
}

static ClutterActor *
pp_rectangle_new_with_color (const ClutterColor *color)
{
  ClutterActor *actor = clutter_actor_new ();

  clutter_actor_set_background_color (actor, color);

  return actor;
}

static void
pp_actor_reparent (ClutterActor *actor, ClutterActor *new_parent)
{
  g_object_ref (actor);

  clutter_actor_remove_child (clutter_actor_get_parent (actor), actor);
  clutter_actor_add_child (new_parent, actor);

  g_object_unref (actor);
}

static void
pp_clutter_render_adjust_background (ClutterRenderer *renderer,
                                     PinPointPoint   *point)
{
  float bg_x, bg_y, bg_width, bg_height, bg_scale_x, bg_scale_y;
  ClutterPointData *data = point->data;
  gboolean background_color_set;

  if (!data)
    return;

  g_object_get (G_OBJECT (data->background),
                "background-color-set", &background_color_set,
                NULL);

  if (background_color_set)
    {
      clutter_actor_get_size (renderer->stage, &bg_width, &bg_height);
      clutter_actor_set_size (data->background, bg_width, bg_height);
    }
  else
    {
      clutter_actor_get_size (data->background, &bg_width, &bg_height);
    }

  pp_get_background_position_scale (point,
                                    clutter_actor_get_width (renderer->stage),
                                    clutter_actor_get_height (renderer->stage),
                                    bg_width, bg_height,
                                    &bg_x, &bg_y, &bg_scale_x, &bg_scale_y);

  clutter_actor_set_scale (data->background, bg_scale_x, bg_scale_y);
  clutter_actor_set_position (data->background, bg_x, bg_y);
}

static void
pp_monitor_window_state (GtkWidget           *widget,
                         GdkEventWindowState *event,
                         gpointer             user_data)
{
  gboolean fullscreen = (event->new_window_state & GDK_WINDOW_STATE_FULLSCREEN) != 0;
  g_object_set_data (G_OBJECT (widget),
                     "fullscreen-state", GUINT_TO_POINTER (fullscreen));
}

static void
pp_set_fullscreen (ClutterRenderer *renderer,
                   GtkWindow       *window,
                   gboolean         fullscreen)
{
  if (fullscreen)
    gtk_window_fullscreen (window);
  else
    gtk_window_unfullscreen (window);
}

static gboolean
pp_get_fullscreen (ClutterRenderer *renderer,
                   GtkWindow       *window)
{
  return GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (window),
                                              "fullscreen-state"));
}

static void
_destroy_surface (gpointer data)
{
  /* not destroying background, since it would be destroyed with
   * the stage itself.
   */
}

static guint hide_cursor = 0;
static gboolean hide_cursor_cb (gpointer stage)
{
  hide_cursor = 0;
  clutter_stage_hide_cursor (stage);
  return FALSE;
}

static void update_commandline_shading (ClutterRenderer *renderer);

static void
activate_commandline (ClutterRenderer *renderer)
{
  PinPointPoint *point;

  if (!pp_slidep)
    return;

  point = pp_slidep->data;

  pp_actor_animate (renderer->commandline, CLUTTER_LINEAR, 500,
                     "opacity", 0xff, NULL);

  pp_actor_animate (renderer->commandline_shading, CLUTTER_LINEAR, 100,
                    "opacity", (int)(point->shading_opacity*0xff*0.33),
                    NULL);

  g_object_set (renderer->commandline,
                "editable", TRUE,
                "single-line-mode", TRUE,
                "activatable", TRUE,
                NULL);
  clutter_actor_grab_key_focus (renderer->commandline);
}


static gboolean commandline_cancel_cb (ClutterActor *actor,
                                       ClutterEvent *event,
                                       gpointer      data)
{
  ClutterRenderer *renderer = CLUTTER_RENDERER (data);
  PinPointPoint *point = pp_slidep->data;

  if (clutter_event_type (event) == CLUTTER_KEY_PRESS &&
      (clutter_event_get_key_symbol (event) == CLUTTER_KEY_Escape ||
       clutter_event_get_key_symbol (event) == CLUTTER_KEY_Tab))
    {
      clutter_actor_grab_key_focus (renderer->stage);
      pp_actor_animate (renderer->commandline,
                        CLUTTER_LINEAR, 500,
                        "opacity", (int)(0xff*0.33), NULL);
      g_object_set (renderer->commandline, "editable", FALSE, NULL);
      pp_actor_animate (renderer->commandline_shading,
                        CLUTTER_LINEAR, 500,
                        "opacity", (int)(point->shading_opacity*0xff*0.33), NULL);
      return TRUE;
    }
  update_commandline_shading (renderer);
  return FALSE;
}

static gboolean commandline_action_cb (ClutterActor *actor,
                                       gpointer      data)
{
  ClutterRenderer *renderer = CLUTTER_RENDERER (data);
  PinPointPoint *point = pp_slidep->data;
  clutter_actor_grab_key_focus (renderer->stage);
  pp_actor_animate (renderer->commandline,
                    CLUTTER_LINEAR, 500,
                    "opacity", (int)(0xff*0.33), NULL);
  g_object_set (renderer->commandline, "editable", FALSE, NULL);
  pp_actor_animate (renderer->commandline_shading,
                    CLUTTER_LINEAR, 500,
                    "opacity", (int)(point->shading_opacity*0xff*0.33), NULL);

  action_slide (renderer);
  return FALSE;
}

static void commandline_notify_cb (ClutterActor *actor,
                                   GParamSpec   *pspec,
                                   gpointer      data)
{
  ClutterRenderer *renderer = CLUTTER_RENDERER (data);
  float            scale;

  scale = clutter_actor_get_width (renderer->stage) /
          (clutter_actor_get_width (actor) / 0.9);
  if (scale > 1.0)
    scale = 1.0;
  clutter_actor_set_scale (actor, scale, scale);
}

static gboolean stage_left (ClutterActor *actor,
                            ClutterEvent *event,
                            gpointer      renderer)
{
  if (hide_cursor)
    g_source_remove (hide_cursor);

  hide_cursor = 0;
  clutter_stage_show_cursor (CLUTTER_STAGE (actor));

  return FALSE;
}

static gboolean stage_motion (ClutterActor *actor,
                              ClutterEvent *event,
                              gpointer      renderer)
{
  float stage_width, stage_height;

  if (hide_cursor)
    g_source_remove (hide_cursor);
  clutter_stage_show_cursor (CLUTTER_STAGE (actor));
  hide_cursor = g_timeout_add (500, hide_cursor_cb, actor);

  if (!pp_get_fullscreen (renderer,
                          GTK_WINDOW (((ClutterRenderer *)renderer)->window)))
    return FALSE;

  clutter_actor_get_size (CLUTTER_RENDERER (renderer)->stage,
                          &stage_width, &stage_height);
#if 0
#ifdef QUICK_ACCESS_LEFT
  if (event->motion.x < 8)
    {
      float d = event->motion.y / stage_height;
#else
  if (event->motion.y < 8)
    {
      float d = event->motion.x / stage_width;
#endif
      if (pp_slidep)
        {
          leave_slide (renderer, FALSE);
        }
      pp_slidep = g_list_nth (pp_slides, g_list_length (pp_slides) * d);
      show_slide (renderer, FALSE);
    }
#endif

  return FALSE;
}

#define NORMAL_OPACITY 100
#define HOVER_OPACITY  255

static gboolean
opacity_hover_enter (ClutterActor *actor,
                     ClutterEvent *event,
                     gpointer      data)
{
  pp_actor_animate (actor, CLUTTER_LINEAR, 200,
                    "opacity", HOVER_OPACITY,
                    NULL);
  return FALSE;
}


static gboolean
opacity_hover_leave (ClutterActor *actor,
                     ClutterEvent *event,
                     gpointer      data)
{
  pp_actor_animate (actor, CLUTTER_LINEAR, 200,
                    "opacity", NORMAL_OPACITY,
                    NULL);
  return FALSE;
}

#define opacity_hover(o)                             \
      clutter_actor_set_opacity (o, NORMAL_OPACITY); \
      clutter_actor_set_reactive (o, TRUE);          \
      g_signal_connect (o, "enter-event",            \
                        G_CALLBACK (opacity_hover_enter), NULL); \
      g_signal_connect (o, "leave-event",            \
                        G_CALLBACK (opacity_hover_leave), NULL); \

static gboolean
play_pause (ClutterActor *actor,
            ClutterEvent *event,
            gpointer      data)
{
  ClutterRenderer *renderer = CLUTTER_RENDERER (data);
  if (!renderer->speaker_window) {
    return TRUE;
  }
  if (renderer->timer_paused)
    {
      g_timer_continue (renderer->timer);
      renderer->timer_paused = FALSE;
      clutter_text_set_text (CLUTTER_TEXT (renderer->speaker_pause), "Pause");
    }
  else
    {
      g_timer_stop (renderer->timer);
      renderer->timer_paused = TRUE;
      clutter_text_set_text (CLUTTER_TEXT (renderer->speaker_pause), "Go");
    }
  return TRUE;
}

static gboolean
toggle_autoadvance (ClutterActor *actor,
                    ClutterEvent *event,
                    gpointer      data)
{
  ClutterRenderer *renderer = CLUTTER_RENDERER (data);
  if (renderer->autoadvance)
    {
      renderer->autoadvance = FALSE;
      clutter_text_set_text (CLUTTER_TEXT (renderer->speaker_autoadvance),
                             "enable autoadvance");
    }
  else
    {
      renderer->autoadvance = TRUE;
      clutter_text_set_text (CLUTTER_TEXT (renderer->speaker_autoadvance),
                             "disable autoadvance");
    }
  return TRUE;
}

static void end_of_presentation (ClutterRenderer *renderer);

static void
next_slide (ClutterRenderer *renderer)
{
  if (pp_slidep && pp_slidep->next)
    {
      leave_slide (renderer, FALSE);
      pp_slidep = pp_slidep->next;
      show_slide (renderer, FALSE);
    }
  else
    {
      end_of_presentation (renderer);
    }
}

static void
prev_slide (ClutterRenderer *renderer)
{
  if (pp_slidep && pp_slidep->prev)
    {
      leave_slide (renderer, TRUE);
      pp_slidep = pp_slidep->prev;
      show_slide (renderer, TRUE);
    }
}

static gboolean
go_prev (ClutterActor *actor,
         ClutterEvent *event,
         gpointer      data)
{
  ClutterRenderer *renderer = CLUTTER_RENDERER (data);
  prev_slide (renderer);
  return TRUE;
}

static gboolean
go_next (ClutterActor *actor,
         ClutterEvent *event,
         gpointer      data)
{
  ClutterRenderer *renderer = CLUTTER_RENDERER (data);
  next_slide (renderer);
  return TRUE;
}

static gboolean
start (ClutterActor *actor,
       ClutterEvent *event,
       gpointer      data)
{
  ClutterRenderer *renderer = CLUTTER_RENDERER (data);

  clutter_text_set_text(CLUTTER_TEXT(renderer->speaker_start), "Restart");

  g_timer_stop (renderer->timer);
  g_timer_start (renderer->timer);
  renderer->timer_paused = FALSE;
  leave_slide (renderer, TRUE);
  pp_slidep = pp_slides;
  play_pause (NULL, NULL, data);
  play_pause (NULL, NULL, data);
  if (pp_rehearse)
    {
      pp_rehearse = FALSE;
    }
  pp_rehearse_init (); /* zeroes out the new-time */
  show_slide (renderer, TRUE);
  renderer->reset = TRUE;
  return TRUE;
}

static gboolean
start_rehearse (ClutterActor *actor,
                ClutterEvent *event,
                gpointer      data)
{
  start (actor, event, data);
  pp_rehearse = TRUE;
  pp_rehearse_init ();

  return FALSE;
}


static void toggle_speaker_screen (ClutterRenderer *renderer);

static gboolean
speaker_screen_deleted (ClutterActor *actor,
         ClutterEvent *event,
         gpointer      data)
{
  ClutterRenderer *renderer = data;
  toggle_speaker_screen(renderer);
  return TRUE;
}


static void
clutter_renderer_init_speaker_screen (ClutterRenderer *renderer)
{
  GtkWidget *embed;

  renderer->speaker_window = g_object_new (GTK_TYPE_WINDOW,
                                           "type", GTK_WINDOW_TOPLEVEL,
                                           "hide-titlebar-when-maximized", TRUE,
                                           "resizable", TRUE,
                                           "title", "Pinpoint speaker screen",
                                           "default-width", DEFAULT_WIDTH,
                                           "default-height", DEFAULT_HEIGHT,
                                           NULL);
  gtk_application_add_window (renderer->application,
                              GTK_WINDOW (renderer->speaker_window));
  g_signal_connect (renderer->speaker_window, "window-state-event",
                    G_CALLBACK (pp_monitor_window_state), NULL);
  embed = gtk_clutter_embed_new ();
  gtk_container_add (GTK_CONTAINER (renderer->speaker_window),
                     embed);

  renderer->speaker_screen = gtk_clutter_embed_get_stage (GTK_CLUTTER_EMBED (embed));

  renderer->speaker_notes = g_object_new (CLUTTER_TYPE_TEXT,
                                "x", 10.0,
                                "y", 0.0,
                                "font-name",      "Sans 20px",
                                "color",          &white,
                                NULL);

  clutter_text_set_line_wrap(CLUTTER_TEXT(renderer->speaker_notes), TRUE);

  renderer->speaker_time_remaining = g_object_new (CLUTTER_TYPE_TEXT,
                                "x",           300.0,
                                "y",           0.0,
                                "opacity",     NORMAL_OPACITY,
                                "font-name",   "Sans 24px",
                                "text",        "-",
                                "color",       &white,
                                NULL);

  renderer->speaker_buttons_group = clutter_actor_new ();


#define BUTTON_FONT "Sans 16px"

  renderer->speaker_speakerscreen = g_object_new (CLUTTER_TYPE_TEXT,
                                "x",           10.0,
                                "y",           0.0,
                                "opacity",     NORMAL_OPACITY,
                                "font-name",   BUTTON_FONT,
                                "text",        "Speaker",
                                "reactive",    TRUE,
                                "color",       &black,
                                NULL);

  renderer->speaker_rehearse = g_object_new (CLUTTER_TYPE_TEXT,
                                "x",           0.0,
                                "y",           0.0,
                                "opacity",     NORMAL_OPACITY,
                                "font-name",   BUTTON_FONT,
                                "text",        "Rehearse",
                                "reactive",    TRUE,
                                "color",       &black,
                                NULL);
  renderer->speaker_autoadvance = g_object_new (CLUTTER_TYPE_TEXT,
                                "y",           0.0,
                                "opacity",     NORMAL_OPACITY,
                                "font-name",   BUTTON_FONT,
                                "text",        "Autoadvance",
                                "reactive",    TRUE,
                                "color",       &black,
                                NULL);
  renderer->speaker_start = g_object_new (CLUTTER_TYPE_TEXT,
                                "y",           0.0,
                                "opacity",     NORMAL_OPACITY,
                                "font-name",   BUTTON_FONT,
                                "text",        "Start",
                                "reactive",    TRUE,
                                "color",       &black,
                                NULL);
  renderer->speaker_pause = g_object_new (CLUTTER_TYPE_TEXT,
                                "y",           0.0,
                                "opacity",     NORMAL_OPACITY,
                                "font-name",   BUTTON_FONT,
                                "text",        "",
                                "reactive",    TRUE,
                                "color",       &black,
                                NULL);

  renderer->speaker_fullscreen = g_object_new (CLUTTER_TYPE_TEXT,
                                "x",           0.0,
                                "y",           0.0,
                                "opacity",     NORMAL_OPACITY,
                                "font-name",   BUTTON_FONT,
                                "text",        "Fullscreen",
                                "reactive",    TRUE,
                                "color",       &black,
                                NULL);

  renderer->speaker_buttonbar = pp_rectangle_new_with_color (&gray);

  clutter_actor_set_height(renderer->speaker_buttonbar,
                           clutter_actor_get_height(renderer->speaker_pause)*1.1);


  opacity_hover(renderer->speaker_speakerscreen);
  opacity_hover(renderer->speaker_rehearse);

  g_signal_connect (renderer->speaker_rehearse, "button-press-event",
                    G_CALLBACK (start_rehearse), renderer);

  opacity_hover(renderer->speaker_autoadvance);

  g_signal_connect (renderer->speaker_autoadvance, "button-press-event",
                    G_CALLBACK (toggle_autoadvance), renderer);

  opacity_hover(renderer->speaker_start);

  g_signal_connect (renderer->speaker_start, "button-press-event",
                    G_CALLBACK (start), renderer);

  opacity_hover(renderer->speaker_pause);

  g_signal_connect (renderer->speaker_pause, "button-press-event",
                    G_CALLBACK (play_pause), renderer);

  opacity_hover(renderer->speaker_fullscreen);

  clutter_actor_add_child (renderer->speaker_buttons_group,
                           renderer->speaker_buttonbar);
  clutter_actor_add_child (renderer->speaker_buttons_group,
                           renderer->speaker_speakerscreen);
  clutter_actor_add_child (renderer->speaker_buttons_group,
                           renderer->speaker_start);
  clutter_actor_add_child (renderer->speaker_buttons_group,
                           renderer->speaker_pause);
  clutter_actor_add_child (renderer->speaker_buttons_group,
                           renderer->speaker_autoadvance);
  clutter_actor_add_child (renderer->speaker_buttons_group,
                           renderer->speaker_rehearse);
  clutter_actor_add_child (renderer->speaker_buttons_group,
                           renderer->speaker_fullscreen);

  g_signal_connect (renderer->speaker_screen, "key-press-event",
                    G_CALLBACK (key_pressed), renderer);
  g_signal_connect (renderer->speaker_screen, "button-press-event",
                    G_CALLBACK (mouse_clicked), renderer);



  renderer->timer_paused = TRUE;
  renderer->timer = g_timer_new ();
  g_timer_stop (renderer->timer);

  renderer->speaker_prog_bg = pp_rectangle_new_with_color (&c_prog_bg);
  renderer->speaker_prog_time = pp_rectangle_new_with_color (&c_prog_time);
  renderer->speaker_prog_slide = pp_rectangle_new_with_color (&c_prog_slide);
  renderer->speaker_slide_prog_warning = pp_rectangle_new_with_color (&red);

  clutter_actor_set_background_color (renderer->speaker_screen, &black);
  clutter_actor_set_background_color (renderer->speaker_screen, &black);


  renderer->speaker_preview_bar = pp_rectangle_new_with_color (&lightgray);

  renderer->speaker_prev = clutter_cairo_texture_new (PREVIEW_WIDTH-1, PREVIEW_HEIGHT-1);
  renderer->speaker_current = clutter_cairo_texture_new (PREVIEW_WIDTH-1, PREVIEW_HEIGHT-1);
  renderer->speaker_next = clutter_cairo_texture_new (PREVIEW_WIDTH-1, PREVIEW_HEIGHT-1);

  clutter_actor_add_child (renderer->speaker_screen,
                           renderer->speaker_preview_bar);
  clutter_actor_add_child (renderer->speaker_screen,
                           renderer->speaker_prev);
  clutter_actor_add_child (renderer->speaker_screen,
                           renderer->speaker_current);
  clutter_actor_add_child (renderer->speaker_screen,
                           renderer->speaker_next);
  clutter_actor_add_child (renderer->speaker_screen,
                           renderer->speaker_notes);
  clutter_actor_add_child (renderer->speaker_screen,
                           renderer->speaker_prog_bg);
  clutter_actor_add_child (renderer->speaker_screen,
                           renderer->speaker_slide_prog_warning);
  clutter_actor_add_child (renderer->speaker_screen,
                           renderer->speaker_prog_time);

  clutter_actor_add_child (renderer->speaker_screen,
                           renderer->speaker_prog_slide);

  clutter_actor_add_child (renderer->speaker_screen,
                           renderer->speaker_buttons_group);
  clutter_actor_add_child (renderer->speaker_screen,
                           renderer->speaker_time_remaining);


  clutter_actor_set_opacity (renderer->speaker_slide_prog_warning, 0);



  opacity_hover(renderer->speaker_prev);
  opacity_hover(renderer->speaker_next);

  // reset opacity:
  clutter_actor_set_opacity (renderer->speaker_prev, 200);
  clutter_actor_set_opacity (renderer->speaker_next, 200);

  g_signal_connect (renderer->speaker_prev, "button-press-event",
                    G_CALLBACK (go_prev), renderer);
  g_signal_connect (renderer->speaker_next, "button-press-event",
                    G_CALLBACK (go_next), renderer);

  g_signal_connect (renderer->speaker_window, "delete-event",
                    G_CALLBACK (speaker_screen_deleted), renderer);

  gtk_widget_show_all (embed);
}

static gboolean
window_deleted (ClutterStage *stage,
                ClutterEvent *event,
                gpointer      user_data)
{
  ClutterRenderer *renderer = CLUTTER_RENDERER (user_data);

  g_application_quit (G_APPLICATION (renderer->application));

  return TRUE;
}

static void
clutter_renderer_init (PinPointRenderer   *pp_renderer,
                       char               *pinpoint_file)
{
  ClutterRenderer *renderer = CLUTTER_RENDERER (pp_renderer);
  GFileMonitor *monitor;
  ClutterActor *stage;
  GtkWidget *embed;

  renderer->application = gtk_application_new ("org.gnome.Pinpoint",
                                               G_APPLICATION_NON_UNIQUE);
  g_application_register (G_APPLICATION (renderer->application), NULL, NULL);

  renderer->window = g_object_new (GTK_TYPE_WINDOW,
                                   "type", GTK_WINDOW_TOPLEVEL,
                                   "hide-titlebar-when-maximized", TRUE,
                                   "resizable", TRUE,
                                   "title", "Pinpoint presentation",
                                   "default-width", DEFAULT_WIDTH,
                                   "default-height", DEFAULT_HEIGHT,
                                   NULL);
  gtk_application_add_window (renderer->application,
                              GTK_WINDOW (renderer->window));
  g_signal_connect (renderer->window, "window-state-event",
                    G_CALLBACK (pp_monitor_window_state), NULL);
  g_signal_connect (renderer->window, "delete-event",
                    G_CALLBACK (window_deleted), renderer);

  embed = gtk_clutter_embed_new ();
  gtk_container_add (GTK_CONTAINER (renderer->window), embed);
  gtk_window_set_focus (GTK_WINDOW (renderer->window), embed);

  renderer->stage = stage = gtk_clutter_embed_get_stage (GTK_CLUTTER_EMBED (embed));
  renderer->root = clutter_actor_new ();
  renderer->curtain = pp_rectangle_new_with_color (&black);
  renderer->rest_y = STARTPOS;
  renderer->background = clutter_actor_new ();
  renderer->midground = clutter_actor_new ();
  renderer->foreground = clutter_actor_new ();
  renderer->json_layer = clutter_actor_new ();
  renderer->shading = pp_rectangle_new_with_color (&black);
  renderer->commandline_shading = pp_rectangle_new_with_color (&black);
  renderer->commandline = clutter_text_new ();

  clutter_actor_set_size (renderer->curtain, 10000, 10000);
  clutter_actor_hide (renderer->curtain);
  clutter_actor_set_opacity (renderer->shading, 0x77);
  clutter_actor_set_opacity (renderer->commandline_shading, 0x77);

  clutter_actor_add_child (renderer->midground, renderer->shading);

  clutter_actor_add_child (renderer->stage, renderer->root);
  clutter_actor_add_child (renderer->stage, renderer->curtain);

  clutter_actor_add_child (renderer->root, renderer->background);
  clutter_actor_add_child (renderer->root, renderer->midground);
  clutter_actor_add_child (renderer->root, renderer->foreground);
  clutter_actor_add_child (renderer->root, renderer->json_layer);
  clutter_actor_add_child (renderer->root, renderer->commandline_shading);
  clutter_actor_add_child (renderer->root, renderer->commandline);

  renderer->timer_paused = FALSE;
  renderer->timer = g_timer_new ();


  if (pp_speakermode)
    toggle_speaker_screen (renderer);

  gtk_widget_show_all (renderer->window);

  clutter_actor_set_background_color (stage, &black);
  g_signal_connect (stage, "key-press-event",
                    G_CALLBACK (key_pressed), renderer);
  g_signal_connect (stage, "button-press-event",
                    G_CALLBACK (mouse_clicked), renderer);
  g_signal_connect (stage, "notify::width",
                    G_CALLBACK (stage_resized), renderer);

  if (renderer->speaker_screen)
    {
      g_signal_connect (renderer->speaker_screen, "notify::width",
                    G_CALLBACK (stage_resized), renderer);
      g_signal_connect (renderer->speaker_screen, "notify::height",
                    G_CALLBACK (stage_resized), renderer);
    }

  g_signal_connect (stage, "notify::height",
                    G_CALLBACK (stage_resized), renderer);
  g_signal_connect (stage, "motion-event",
                    G_CALLBACK (stage_motion), renderer);
  g_signal_connect (stage, "leave-event",
                    G_CALLBACK (stage_left), renderer);
  g_signal_connect (renderer->commandline, "activate",
                    G_CALLBACK (commandline_action_cb), renderer);
  g_signal_connect (renderer->commandline, "captured-event",
                    G_CALLBACK (commandline_cancel_cb), renderer);
  g_signal_connect (renderer->commandline, "notify::width",
                    G_CALLBACK (commandline_notify_cb), renderer);

  if (pp_fullscreen) {
    pp_set_fullscreen (renderer, GTK_WINDOW (renderer->window), TRUE);
    pp_inhibit (renderer, pp_fullscreen);
  }

  renderer->path = pinpoint_file;
  if (renderer->path)
    {
      monitor = g_file_monitor (g_file_new_for_commandline_arg (pinpoint_file),
                                G_FILE_MONITOR_NONE, NULL, NULL);
      g_signal_connect (monitor, "changed", G_CALLBACK (file_changed),
                                            renderer);
    }

  renderer->bg_cache = g_hash_table_new_full (g_str_hash, g_str_equal,
                                              NULL, _destroy_surface);

  renderer->cairo_renderer = pp_cairo_renderer ();
  renderer->cairo_renderer->init (renderer->cairo_renderer, pinpoint_file);
}

static gboolean update_speaker_screen (ClutterRenderer *renderer);

static void
clutter_renderer_run (PinPointRenderer *pp_renderer)
{
  ClutterRenderer *renderer = CLUTTER_RENDERER (pp_renderer);

  show_slide (renderer, FALSE);

  /* the presentaiton is not parsed at first initialization,.. */
  renderer->total_seconds = point_defaults->duration * 60;

  g_timeout_add (15, (GSourceFunc)update_speaker_screen, renderer);
  g_application_run (G_APPLICATION (renderer->application), 0, NULL);
}

static void
clutter_renderer_finalize (PinPointRenderer *pp_renderer)
{
  ClutterRenderer *renderer = CLUTTER_RENDERER (pp_renderer);

  clutter_actor_destroy (renderer->stage);
  g_hash_table_unref (renderer->bg_cache);
}

static ClutterActor *
_clutter_get_texture (ClutterRenderer *renderer,
                      const char      *file)
{
  ClutterActor *source;

  source = g_hash_table_lookup (renderer->bg_cache, file);
  if (source)
    {
      return clutter_clone_new (source);
    }

  source = g_object_new (CLUTTER_TYPE_TEXTURE,
                         "filename", file,
                         "load-data-async", TRUE,
                         NULL);

  if (!source)
    return NULL;

  clutter_actor_add_child (renderer->stage, source);
  clutter_actor_hide (source);

  g_hash_table_insert (renderer->bg_cache, (char *) g_strdup (file), source);

  return clutter_clone_new (source);
}

#if USE_CLUTTER_GST
static void
on_size_changed (ClutterGstPlayer *player,
                 gint              width,
                 gint              height,
                 gpointer          user_data)
{
  PinPointRenderer *pp_renderer = user_data;
  ClutterRenderer *renderer = CLUTTER_RENDERER (pp_renderer);
  PinPointPoint *point;
  ClutterPointData *data;

  point = pp_slidep->data;
  if (!point)
    return;

  data = point->data;

  if (data->player != player)
    {
      g_warning ("size changed but not current background ?!");
      return;
    }

  clutter_actor_set_size (data->background, width, height);
  pp_clutter_render_adjust_background (renderer, point);
}

static gboolean
setup_camera (PinPointRenderer *renderer,
              PinPointPoint    *point)
{
  /* These are static to be able to share the texture and the pipeline between
   * all the slides using the camera */
  static ClutterGstCamera *camera = NULL;
  ClutterPointData *data = point->data;

  if (camera)
    {
      data->background =
        g_object_new (CLUTTER_TYPE_ACTOR,
                      "content", g_object_new (CLUTTER_GST_TYPE_ASPECTRATIO,
                                               "player", camera,
                                               NULL),
                      "width", 1.0,
                      "height", 1.0,
                      NULL);
      data->player = CLUTTER_GST_PLAYER (camera);

      g_signal_connect (camera, "size-change",
                        G_CALLBACK (on_size_changed), renderer);
      return TRUE;
    }

  camera = clutter_gst_camera_new ();

  data->player = CLUTTER_GST_PLAYER (camera);
  data->background =
    g_object_new (CLUTTER_TYPE_ACTOR,
                  "content", g_object_new (CLUTTER_GST_TYPE_ASPECTRATIO,
                                           "player", camera,
                                           NULL),
                  NULL);

  g_signal_connect (camera, "size-change",
                    G_CALLBACK (on_size_changed), renderer);

  return TRUE;
}

static gboolean
setup_player (PinPointRenderer *renderer,
              PinPointPoint    *point,
              const gchar      *file)
{
  ClutterPointData *data = point->data;
  ClutterGstPlayback *playback = clutter_gst_playback_new ();
  clutter_gst_playback_set_filename (playback, file);

  data->player = CLUTTER_GST_PLAYER (playback);
  data->background =
    g_object_new (CLUTTER_TYPE_ACTOR,
                  "content", g_object_new (CLUTTER_GST_TYPE_ASPECTRATIO,
                                           "player", playback,
                                           NULL),
                  "width", 1.0,
                  "height", 1.0,
                  NULL);

  g_signal_connect (playback, "size-change",
                    G_CALLBACK (on_size_changed), renderer);

  return TRUE;
}
#endif

static gboolean
clutter_renderer_make_point (PinPointRenderer *pp_renderer,
                             PinPointPoint    *point)
{
  ClutterRenderer  *renderer  = CLUTTER_RENDERER (pp_renderer);
  ClutterPointData *data      = point->data;
  const char       *file      = point->bg;
        char       *full_path = NULL;
  ClutterColor color;
  gboolean ret = FALSE;

  if (point->bg_type != PP_BG_COLOR && renderer->path && file)
    {
      char *dir = g_path_get_dirname (renderer->path);
      full_path = g_build_filename (dir, file, NULL);
      g_free (dir);

      file = full_path;
    }

  switch (point->bg_type)
    {
    case PP_BG_COLOR:
      {
        ret = clutter_color_from_string (&color, point->bg);
        if (ret)
          {
            data->background = pp_rectangle_new_with_color (&color);
            clutter_actor_set_size (data->background, 100.0, 100.0);
          }
      }
      break;
    case PP_BG_NONE:
      {
        ClutterColor black = {0, 0, 0, 255};

        ret = clutter_color_from_string (&color, point->stage_color);
        if (ret)
          {
            data->background = pp_rectangle_new_with_color (&color);
            clutter_actor_set_size (data->background, 100.0, 100.0);
          }
        else
          {
            data->background = pp_rectangle_new_with_color (&black);
            clutter_actor_set_size (data->background, 100.0, 100.0);
          }
     }
      break;
    case PP_BG_IMAGE:
      data->background = _clutter_get_texture (renderer, file);
      ret = TRUE;
      break;
    case PP_BG_VIDEO:
#ifdef USE_CLUTTER_GST
      ret = setup_player (pp_renderer, point, file);
#endif
      break;
    case PP_BG_CAMERA:
#ifdef USE_CLUTTER_GST
      ret = setup_camera (pp_renderer, point);
#endif
      break;
    case PP_BG_SVG:
#ifdef USE_DAX
      {
        ClutterActor *aa, *svg;
        GError *error = NULL;

        aa = pp_super_aa_new ();
        pp_super_aa_set_resolution (PP_SUPER_AA (aa), 2, 2);
        svg = dax_actor_new_from_file (file, &error);
        mx_offscreen_set_pick_child (MX_OFFSCREEN (aa), TRUE);
        clutter_actor_add_child (aa, svg);

        data->background = aa;

        if (data->background == NULL)
          {
            g_warning ("Could not open SVG file %s: %s",
                       file, error->message);
            g_clear_error (&error);
          }
        ret = data->background != NULL;
      }
#endif
      break;
    default:
      g_assert_not_reached();
    }

  g_free (full_path);

  if (data->background)
    {
      clutter_actor_add_child (renderer->background, data->background);
      clutter_actor_set_opacity (data->background, 0);
    }

  clutter_color_from_string (&color, point->text_color);

  if (point->use_markup)
    {
      data->text = g_object_new (CLUTTER_TYPE_TEXT,
                                 "font-name",      point->font,
                                 "text",           point->text,
                                 "line-alignment", point->text_align,
                                 "color",          &color,
                                 "use-markup",     TRUE,
                                 NULL);
    }
  else
    {
      data->text = g_object_new (CLUTTER_TYPE_TEXT,
                                 "font-name",      point->font,
                                 "text",           point->text,
                                 "line-alignment", point->text_align,
                                 "color",          &color,
                                 NULL);
    }

  clutter_actor_add_child (renderer->foreground, data->text);

  clutter_actor_set_position (data->text, RESTX, renderer->rest_y);
  data->rest_y = renderer->rest_y;
  renderer->rest_y += clutter_actor_get_height (data->text);
  clutter_actor_set_z_position (data->text, RESTDEPTH);

  return ret;
}

static void *
clutter_renderer_allocate_data (PinPointRenderer *renderer)
{
  ClutterPointData *data = g_slice_new0 (ClutterPointData);
  data->renderer = renderer;
  return data;
}

static void
clutter_renderer_free_data (PinPointRenderer *renderer,
                            void             *datap)
{
  ClutterPointData *data = datap;

  if (data->background)
    clutter_actor_destroy (data->background);
  if (data->text)
    clutter_actor_destroy (data->text);
  if (data->json_slide)
    clutter_actor_destroy (data->json_slide);
  if (data->script)
    g_object_unref (data->script);
  g_slice_free (ClutterPointData, data);
}

static void end_of_presentation (ClutterRenderer *renderer)
{
  if (pp_rehearse)
    {
      pp_rehearse_done ();
    }
  pp_rehearse = FALSE;
  if (renderer->autoadvance)
    toggle_autoadvance (NULL, NULL, renderer);
}


static void
toggle_speaker_screen (ClutterRenderer *renderer)
{
  if (!renderer->speaker_window)
    clutter_renderer_init_speaker_screen (renderer);
  if (renderer->speaker_mode)
    {
      renderer->speaker_mode = FALSE;
      gtk_widget_hide (renderer->speaker_window);
    }
  else
    {
      renderer->speaker_mode = TRUE;
      gtk_widget_show_all (renderer->speaker_window);
    }
}

static void
pp_inhibit (ClutterRenderer *renderer,
            gboolean         fullscreen)
{
  if (fullscreen)
    {
      renderer->inhibit_cookie =
        gtk_application_inhibit (renderer->application,
                                 GTK_WINDOW (renderer->window),
                                 GTK_APPLICATION_INHIBIT_IDLE,
                                 "Presenting some blingin' slides");
    }
  else if (renderer->inhibit_cookie != 0)
    {
      gtk_application_uninhibit (renderer->application,
                                 renderer->inhibit_cookie);
      renderer->inhibit_cookie = 0;
    }
}

static gboolean
key_pressed (ClutterActor    *actor,
             ClutterEvent    *event,
             ClutterRenderer *renderer)
{
  if (event) /* There is no event for the first triggering */
  switch (clutter_event_get_key_symbol (event))
    {
      case CLUTTER_KEY_Left:
      case CLUTTER_KEY_Up:
      case CLUTTER_KEY_BackSpace:
      case CLUTTER_KEY_Prior:
        prev_slide (renderer);
        break;
      case CLUTTER_KEY_Right:
      case CLUTTER_KEY_space:
      case CLUTTER_KEY_Next:
      case CLUTTER_KEY_Down:
        next_slide (renderer);
        break;
      case CLUTTER_KEY_Escape:
      case CLUTTER_KEY_Q:
      case CLUTTER_KEY_q:
        g_application_quit (G_APPLICATION (renderer->application));
        break;
      case CLUTTER_KEY_F1:
        {
          gboolean was_fullscreen =
            pp_get_fullscreen (renderer,
                               GTK_WINDOW (renderer->window));
          toggle_speaker_screen (renderer);
          if (renderer->speaker_mode && renderer->speaker_window)
            pp_set_fullscreen (renderer,
                               GTK_WINDOW (renderer->speaker_window),
                               was_fullscreen);
        }
        break;
      case CLUTTER_KEY_F2:
        if (renderer->autoadvance)
          renderer->autoadvance = FALSE;
        else
          renderer->autoadvance = TRUE;
        break;
      case CLUTTER_KEY_F11:
      case CLUTTER_KEY_F:
      case CLUTTER_KEY_f:
        {
          gboolean was_fullscreen =
            pp_get_fullscreen (renderer,
                               GTK_WINDOW (renderer->window));
          pp_set_fullscreen (renderer,
                             GTK_WINDOW (renderer->window),
                             !was_fullscreen);
          if (renderer->speaker_mode && renderer->speaker_window)
            pp_set_fullscreen (renderer,
                               GTK_WINDOW (renderer->speaker_window),
                               !was_fullscreen);

          pp_inhibit (renderer, !was_fullscreen);
        }
        break;
      case CLUTTER_KEY_Return:
        action_slide (renderer);
        break;
      case CLUTTER_KEY_Tab:
        activate_commandline (renderer);
        break;
      case CLUTTER_KEY_b:
      case CLUTTER_KEY_B:
        if (clutter_actor_is_visible (renderer->curtain))
          clutter_actor_hide (renderer->curtain);
        else
          clutter_actor_show (renderer->curtain);
        break;
      case CLUTTER_KEY_H:
      case CLUTTER_KEY_h:
      case CLUTTER_KEY_Home:
        start (NULL, NULL, renderer);
        break;
    }
  return TRUE;
}

static gboolean
mouse_clicked (ClutterActor    *actor,
               ClutterEvent    *event,
               ClutterRenderer *renderer)
{
  if(event)
  switch (clutter_event_get_button(event))
  {
    case 1: /* Left mouse button is clicked */
      next_slide (renderer);
      break;
    case 3: /* Right mouse button is clicked */
      prev_slide (renderer);
      break;
  }
  return TRUE;
}


static void leave_slide (ClutterRenderer *renderer,
                         gboolean         backwards)
{
  PinPointPoint *point = pp_slidep->data;
  ClutterPointData *data = point->data;

  point->new_duration += g_timer_elapsed (renderer->timer, NULL) -
                                          renderer->slide_start_time;

  if (!point->transition)
    {
      pp_actor_animate (data->text,
                        CLUTTER_LINEAR, 2000,
                        "depth",        RESTDEPTH,
                        "scale-x",      1.0,
                        "scale-y",      1.0,
                        "x",            RESTX,
                        "y",            data->rest_y,
                        NULL);
      if (data->background)
        {
          pp_actor_animate (data->background,
                            CLUTTER_LINEAR, 1000,
                            "opacity",      0x0,
                            NULL);
        }
    }
  else
    {
      if (data->script)
        {
          if (backwards)
            clutter_state_set_state (data->state, "pre");
          else
            clutter_state_set_state (data->state, "post");
        }
    }

  if (data->background)
    {
#ifdef USE_CLUTTER_GST
      if (point->bg_type == PP_BG_CAMERA ||
          point->bg_type ==  PP_BG_VIDEO)
        {
          clutter_gst_player_set_playing (data->player, FALSE);
        }
#endif
#ifdef USE_DAX
      if (DAX_IS_ACTOR (data->background))
        {
          dax_actor_set_playing (DAX_ACTOR (data->background), FALSE);
        }
      else if (PP_IS_SUPER_AA (data->background))
        {
          ClutterActor *actor;

          actor = mx_offscreen_get_child (MX_OFFSCREEN (data->background));
          dax_actor_set_playing (DAX_ACTOR (actor), FALSE);
        }
#endif
    }
}

static void state_completed (ClutterState *state, gpointer user_data)
{
  PinPointPoint    *point     = user_data;
  ClutterPointData *data      = point->data;
  const char       *new_state = clutter_state_get_state (state);

  if (new_state == g_intern_static_string ("post") ||
      new_state == g_intern_static_string ("pre"))
    {
      clutter_actor_hide (data->json_slide);
      if (data->background2)
        {
          pp_actor_reparent (data->text,
                             CLUTTER_RENDERER (data->renderer)->foreground);

          g_object_set (data->text,
                        "depth",   RESTDEPTH,
                        "scale-x", 1.0,
                        "scale-y", 1.0,
                        "x",       RESTX,
                        "y",       data->rest_y,
                        NULL);
          clutter_actor_set_opacity (data->background, 0);
        }
    }
}

static void
action_slide (ClutterRenderer *renderer)
{
  PinPointPoint    *point;
  ClutterPointData *data;
  const char       *command = NULL;

  if (!pp_slidep)
    return;

  point = pp_slidep->data;
  data = point->data;

  if (data->state)
    clutter_state_set_state (data->state, "action");

  command = clutter_text_get_text (CLUTTER_TEXT (renderer->commandline));
  if (command && *command)
    {
      char *tmp = g_strdup_printf ("%s &", command);
      g_print ("running: %s\n", tmp);
      system (tmp);
      g_free (tmp);
    }
}

static char *pp_lookup_transition (const char *transition)
{
  int   i;
  char *dirs[] ={ "", PINPOINT_SRCDIR "/transitions/", PKGDATADIR, NULL};

  for (i = 0; dirs[i]; i++)
    {
      char *path = g_strdup_printf ("%s%s.json", dirs[i], transition);
      if (g_file_test (path, G_FILE_TEST_EXISTS))
        return path;
      g_free (path);
    }
  return NULL;
}

static void update_commandline_shading (ClutterRenderer *renderer)
{
  PinPointPoint *point;
  ClutterColor   color;
  const char    *command;

  float text_x,    text_y,    text_width,    text_height;
  float shading_x, shading_y, shading_width, shading_height;

  point = pp_slidep->data;
  clutter_actor_get_size (renderer->commandline, &text_width, &text_height);
  clutter_actor_get_position (renderer->commandline, &text_x, &text_y);
  pp_get_shading_position_size (clutter_actor_get_width (renderer->stage),
                                clutter_actor_get_height (renderer->stage),
                                text_x, text_y,
                                text_width, text_height,
                                1.0,
                                &shading_x, &shading_y,
                                &shading_width, &shading_height);

  clutter_color_from_string (&color, point->shading_color);
  g_object_set (renderer->commandline_shading,
         "x", shading_x,
         "y", shading_y,
         NULL);
  command = clutter_text_get_text (CLUTTER_TEXT (renderer->commandline));

  pp_actor_animate (renderer->commandline_shading,
                    CLUTTER_EASE_OUT_QUINT, 1000,

                     /* the opacity of the commandline shading depends
                        on whether we have a command or not */
                     "opacity", command && *command?(int)(point->shading_opacity*255*0.33):0,
                     "background-color",   &color,
                     "width",   shading_width,
                     "height",  shading_height,
                     NULL);
}

static gfloat point_time (ClutterRenderer *renderer,
                          PinPointPoint   *point)
{
  GList *iter;
  float time;
  gboolean after_current = FALSE;

  for (iter = pp_slides; iter && iter->data != point; iter = iter->next)
    {
      if (iter == pp_slidep)
        after_current = TRUE;
    }

  time = point->duration != 0.0 ? point->duration : 2.0;
  /* if before current point, use new time.. if at or after current point
     use historic time
   */
  if (!after_current)
    if (point->new_duration != 0.0)
      time = point->new_duration;
  return time;
}

static gfloat total_time (ClutterRenderer *renderer,
                          GList *start)
{
  GList *iter;
  gfloat total = 0;
  if (!start)
    start = pp_slides;
  for (iter = start; iter; iter = iter->next)
    {
      total += point_time (renderer, iter->data);
    }
  return total;
}

static gfloat slide_rel_duration (ClutterRenderer *renderer,
                                  GList *slide)
{
  return point_time (renderer, slide->data) / total_time (renderer, pp_slides);
}

static gfloat slide_rel_start (ClutterRenderer *renderer,
                               GList *slide)
{
  GList *iter;
  float time = 0;

  for (iter = slide ->prev; iter; iter = iter->prev)
    {
      time += point_time (renderer, iter->data);
    }

  time = time / total_time (renderer, pp_slides);
  return time;
}

static gfloat slide_time (ClutterRenderer *renderer,
                          GList *slide)
{
  float time = point_time (renderer, slide->data) /
                     total_time (renderer, slide);
  float remaining_time = renderer->total_seconds -
                           g_timer_elapsed (renderer->timer, NULL);
  time *= remaining_time;
  return time;
}


static gboolean update_speaker_screen (ClutterRenderer *renderer)
{
  PinPointPoint *point;

  if (!pp_slidep)
    return FALSE;

  point = pp_slidep->data;
  static gboolean is_updated = TRUE;
  static float current_slide_time = 0.0;
  static float current_slide_duration = 0.0;
  static GList *current_slide = NULL;
  float nh, nw;

  /* Skip this update since the previous one isn't finished */
  if (!is_updated)
    return TRUE;

  is_updated = FALSE;

  if (renderer->reset)
    {
      current_slide = NULL;
      current_slide_duration = 0.0;
      current_slide_time = 0.0;
      renderer->reset = FALSE;
    }

    {
      static float current_slide_prev_time = 0.0;
      float warn_time = SLIDE_WARN_TIME;
      float diff = g_timer_elapsed (renderer->timer, NULL) - current_slide_prev_time;

      if (current_slide != pp_slidep)
        {
          current_slide_time = 0;
          current_slide = pp_slidep;
          current_slide_duration = slide_time (renderer, pp_slidep);
        }

      /* if 33% of the slide is longer than the seconds based threshold, use
         the percentage
       */
      if ((warn_time <= current_slide_duration * SLIDE_WARN_THRESHOLD))
        warn_time =     current_slide_duration * SLIDE_WARN_THRESHOLD;

      current_slide_time += diff;
      if (current_slide_time >= current_slide_duration)
        {
          if (renderer->autoadvance)
            {
              current_slide_time = 0;
              next_slide (renderer);
            }


          if (renderer->speaker_slide_prog_warning)
            pp_actor_animate (renderer->speaker_slide_prog_warning,
                              CLUTTER_LINEAR, 500,
                              "opacity", OPACITY_OVER_TIME,
                              NULL);
        }


      else if ((current_slide_duration - current_slide_time < warn_time))
        {
          if (renderer->speaker_slide_prog_warning)
            pp_actor_animate (renderer->speaker_slide_prog_warning,
                              CLUTTER_LINEAR, 500,
                              "opacity", OPACITY_PAST_THRESHOLD,
                              NULL);
        }
      else
        {
          if (renderer->speaker_slide_prog_warning)
            pp_actor_animate (renderer->speaker_slide_prog_warning,
                              CLUTTER_LINEAR, 50,
                              "opacity", OPACITY_OK,
                              NULL);
        }

      current_slide_prev_time = g_timer_elapsed (renderer->timer, NULL);
    }

  if (!renderer->speaker_mode)
    goto out;

  nw = clutter_actor_get_width (renderer->speaker_screen) + 1;
  nh = clutter_actor_get_height (renderer->speaker_screen);

  { /* should draw rectangles representing progress instead... */
    GString *str = g_string_new ("");
    int i;
    GList *iter;
    for (iter = pp_slides, i=0; iter && iter != pp_slidep;
         iter = iter->next, i++);

    {
      int time;
      time = renderer->total_seconds -
                      g_timer_elapsed (renderer->timer, NULL) + 0.5;

      if (time <= -60)
        g_string_printf (str, "%imin", time/60);
      else if (time <= 10)
        g_string_printf (str, "%is", time);
      else if (time <= 60)
        {
          time = ((time + 4)/ 5) * 5;
          g_string_printf (str, "%is", time);
        }
      else
        g_string_printf (str, "%i%smin", time / 60, (time % 60 > 30) ? "½":"");

      clutter_text_set_text (CLUTTER_TEXT (renderer->speaker_time_remaining),
                             str->str);
    }
    g_string_free (str, TRUE);
  }

  {
#define append_ltr(a,b) \
  clutter_actor_set_x (b, clutter_actor_get_x (a) + clutter_actor_get_width (a) + 20)

	clutter_actor_set_width(renderer->speaker_buttonbar, nw);
	clutter_actor_set_x(renderer->speaker_speakerscreen, 10.0);

    /* should be replace with constraints */
    append_ltr (renderer->speaker_speakerscreen, renderer->speaker_start);
    append_ltr (renderer->speaker_start, renderer->speaker_pause);
    append_ltr (renderer->speaker_pause, renderer->speaker_autoadvance);
    append_ltr (renderer->speaker_autoadvance, renderer->speaker_rehearse);
    append_ltr (renderer->speaker_rehearse, renderer->speaker_fullscreen);
  }

  {
    float height = 32;
    float y = clutter_actor_get_height (renderer->speaker_screen) - height;
    float elapsed_part = g_timer_elapsed (renderer->timer, NULL) / renderer->total_seconds;

    clutter_actor_set_height (renderer->speaker_prog_bg, height);
    clutter_actor_set_height (renderer->speaker_prog_slide, height * 0.7);
    clutter_actor_set_height (renderer->speaker_prog_time, height * 0.84);
    clutter_actor_set_size   (renderer->speaker_slide_prog_warning,
                              nw, height * 1.0);
    clutter_actor_set_y (renderer->speaker_prog_bg, y);
    clutter_actor_set_y (renderer->speaker_prog_slide, y + height * 0.05);
    clutter_actor_set_y (renderer->speaker_prog_time, y);

    clutter_actor_set_y (renderer->speaker_slide_prog_warning, y - height * 0.1);
    /*
    clutter_actor_set_size   (renderer->speaker_slide_prog_warning,
                              nw, nh);
    clutter_actor_set_y (renderer->speaker_slide_prog_warning, 0);*/

    clutter_actor_set_x (renderer->speaker_buttons_group, 0);
    clutter_actor_set_y (renderer->speaker_buttons_group, 0);

    clutter_actor_set_width (renderer->speaker_prog_bg, nw);


    clutter_actor_set_position (renderer->speaker_time_remaining,
       nw - clutter_actor_get_width (renderer->speaker_time_remaining),
       nh - clutter_actor_get_height (renderer->speaker_time_remaining) - 4);

    clutter_actor_set_width (renderer->speaker_prog_slide,
                             nw * slide_rel_duration (renderer, pp_slidep));
    clutter_actor_set_x (renderer->speaker_prog_slide,
                         nw * slide_rel_start (renderer, pp_slidep));

    clutter_actor_set_x (renderer->speaker_prog_time, nw * elapsed_part);

    clutter_actor_set_width (renderer->speaker_prog_time, nw * (1.0-elapsed_part));


    elapsed_part = current_slide_time / current_slide_duration;
  }

  // if first slide, do not show "previous"
  if (!pp_slidep->prev) clutter_actor_hide(renderer->speaker_prev);
  else clutter_actor_show(renderer->speaker_prev);

  // same for last slide
  if (!pp_slidep->next) clutter_actor_hide(renderer->speaker_next);
  else clutter_actor_show(renderer->speaker_next);


  {
    float scale;
    scale = nh / clutter_actor_get_height (renderer->speaker_next) * 0.4;
    clutter_actor_set_scale (renderer->speaker_prev, scale, scale);
    clutter_actor_set_scale (renderer->speaker_current, scale, scale);
    clutter_actor_set_scale (renderer->speaker_next, scale, scale);

    clutter_actor_set_width(renderer->speaker_preview_bar,
    						clutter_actor_get_width(renderer->speaker_current)*scale+2);
    clutter_actor_set_height(renderer->speaker_preview_bar, nh);
  }

  {
    static GList *current_slide = NULL;
    if (current_slide != pp_slidep)
      {
        cairo_t *cr;

        /*************/
        cr = clutter_cairo_texture_create (CLUTTER_CAIRO_TEXTURE (renderer->speaker_prev));
        cairo_renderer_set_cr (renderer->cairo_renderer,
                               cr, clutter_actor_get_width (renderer->speaker_prev),
                               clutter_actor_get_height (renderer->speaker_prev));
        if (pp_slidep->prev)
          cairo_renderer_render_page (renderer->cairo_renderer,
                                      pp_slidep->prev->data);
        else
          cairo_renderer_render_page (renderer->cairo_renderer,
                                      NULL);
        cairo_renderer_unset_cr (renderer->cairo_renderer);
        cairo_destroy (cr);

        /*************/
        cr = clutter_cairo_texture_create (CLUTTER_CAIRO_TEXTURE (renderer->speaker_current));
        cairo_renderer_set_cr (renderer->cairo_renderer,
                               cr, clutter_actor_get_width (renderer->speaker_current),
                               clutter_actor_get_height (renderer->speaker_current));
        cairo_renderer_render_page (renderer->cairo_renderer,
                                    pp_slidep->data);
        cairo_renderer_unset_cr (renderer->cairo_renderer);
        cairo_destroy (cr);

        /*************/
        cr = clutter_cairo_texture_create (CLUTTER_CAIRO_TEXTURE (renderer->speaker_next));
        cairo_renderer_set_cr (renderer->cairo_renderer,
                               cr, clutter_actor_get_width (renderer->speaker_next),
                               clutter_actor_get_height (renderer->speaker_next));
        if (pp_slidep->next)
          cairo_renderer_render_page (renderer->cairo_renderer,
                                      pp_slidep->next->data);
        else
          cairo_renderer_render_page (renderer->cairo_renderer,
                                      NULL);
        cairo_renderer_unset_cr (renderer->cairo_renderer);
        cairo_destroy (cr);
        /*************/
        current_slide = pp_slidep;
    }
  }

  clutter_actor_set_position (renderer->speaker_prev,
                              nw * 0.0,
                              nh * -0.1-2);
  clutter_actor_set_position (renderer->speaker_current,
                              nw * 0.0,
                              nh * 0.3);
  clutter_actor_set_position (renderer->speaker_next,
                              nw * 0.0,
                              nh * 0.7+2);
  // speaker notes
  if (point->speaker_notes) {
    clutter_text_set_text (CLUTTER_TEXT (renderer->speaker_notes),
                           point->speaker_notes);
    char *spc_text = " ";
    float preview_width = clutter_actor_get_width(CLUTTER_ACTOR(renderer->speaker_preview_bar));
    clutter_actor_set_position (renderer->speaker_notes,
  		  	  	  	  	  	  (preview_width+20.0),
                              (20.0+clutter_actor_get_height(CLUTTER_ACTOR(renderer->speaker_buttons_group))));

    clutter_actor_set_width  (renderer->speaker_notes, nw-(preview_width+40.0));

    // handle notes-font-size=auto:
    float text_scale;
    if (g_strcmp0(point->notes_font_size, "auto")==0) {
    	text_scale = (nh-(clutter_actor_get_height(CLUTTER_ACTOR(renderer->speaker_buttons_group))+40.0))/clutter_actor_get_width(renderer->speaker_notes);
        clutter_text_set_font_name(CLUTTER_TEXT (renderer->speaker_notes),
        					   g_strconcat(point->notes_font, spc_text, "20px", NULL));
    } else {
    	text_scale = 1.0;
        clutter_text_set_font_name(CLUTTER_TEXT (renderer->speaker_notes),
        					   g_strconcat(point->notes_font, spc_text, point->notes_font_size, NULL));
    }
    g_object_set (renderer->speaker_notes,
				  "scale-x", text_scale,
				  "scale-y", text_scale,
				  NULL);
    clutter_actor_set_width  (renderer->speaker_notes, nw-(preview_width+40.0));
  }
  else
    clutter_text_set_text (CLUTTER_TEXT (renderer->speaker_notes), "");
out:
  is_updated = TRUE;

  return TRUE;
}

static void
show_slide (ClutterRenderer *renderer, gboolean backwards)
{
  PinPointPoint    *point;
  ClutterPointData *data;
  ClutterColor      color;

  if (!pp_slidep)
    return;

  renderer->slide_start_time = g_timer_elapsed (renderer->timer, NULL);

  point = pp_slidep->data;
  data = point->data;

  if (point->stage_color)
    {
      clutter_color_from_string (&color, point->stage_color);
      clutter_actor_set_background_color (renderer->stage, &color);
    }

  if (data->background)
    {
      pp_clutter_render_adjust_background (renderer, point);

#ifdef USE_CLUTTER_GST
      if (point->bg_type == PP_BG_CAMERA ||
          point->bg_type == PP_BG_VIDEO)
        {
          clutter_gst_player_set_playing (data->player, TRUE);
        }
      else
#endif
#ifdef USE_DAX
     if (DAX_IS_ACTOR (data->background))
       {
         dax_actor_set_playing (DAX_ACTOR (data->background), TRUE);
       }
     else if (PP_IS_SUPER_AA (data->background))
       {
         ClutterActor *actor;

         actor = mx_offscreen_get_child (MX_OFFSCREEN (data->background));
         dax_actor_set_playing (DAX_ACTOR (actor), TRUE);
       }
     else
#endif
       {
       }
    }

  if (!point->transition)
    {
      pp_actor_animate (renderer->foreground,
                         CLUTTER_LINEAR, 500,
                         "opacity",      255,
                         NULL);
      pp_actor_animate (renderer->midground,
                         CLUTTER_LINEAR, 500,
                         "opacity",      255,
                         NULL);
      pp_actor_animate (renderer->background,
                         CLUTTER_LINEAR, 500,
                         "opacity",      255,
                         NULL);

      if (point->text && *point->text)
        {
         float text_x, text_y, text_width, text_height, text_scale;
         float shading_x, shading_y, shading_width, shading_height;

         clutter_actor_get_size (data->text, &text_width, &text_height);

         pp_get_text_position_scale (
             point,
             clutter_actor_get_width (renderer->stage),
             clutter_actor_get_height (renderer->stage),
             text_width, text_height,
             &text_x, &text_y,
             &text_scale);

         pp_get_shading_position_size (
             clutter_actor_get_width (renderer->stage),
             clutter_actor_get_height (renderer->stage),
             text_x, text_y,
             text_width, text_height,
             text_scale,
             &shading_x, &shading_y,
             &shading_width, &shading_height);

         clutter_color_from_string (&color, point->shading_color);

         pp_actor_animate (data->text,
                            CLUTTER_EASE_OUT_QUINT, 1000,
                            "depth",   0.0,
                            "scale-x", text_scale,
                            "scale-y", text_scale,
                            "x",       text_x,
                            "y",       text_y,
                            NULL);

         pp_actor_animate (renderer->shading,
                            CLUTTER_EASE_OUT_QUINT, 1000,
                            "x",                shading_x,
                            "y",                shading_y,
                            "opacity",          (int)(point->shading_opacity*255),
                            "background-color", &color,
                            "width",            shading_width,
                            "height",           shading_height,
                            NULL);
        }
      else
        {
          pp_actor_animate (renderer->shading,
                             CLUTTER_LINEAR, 500,
                             "opacity", 0,
                             "width",   0.0,
                             "height",  0.0,
                             NULL);
        }


      if (data->background)
        pp_actor_animate (data->background,
                           CLUTTER_LINEAR, 1000,
                           "opacity", 0xff,
                           NULL);
    }
  else
    {
      GError *error = NULL;
      /* fade out global group of texts when using a custom .json template */
      pp_actor_animate (renderer->foreground,
                         CLUTTER_LINEAR, 500,
                         "opacity",      0,
                         NULL);
      pp_actor_animate (renderer->midground,
                         CLUTTER_LINEAR, 500,
                         "opacity",      0,
                         NULL);
      pp_actor_animate (renderer->background,
                         CLUTTER_LINEAR, 500,
                         "opacity",      0,
                         NULL);
      if (!data->script)
        {
          char *path = pp_lookup_transition (point->transition);
          data->script = clutter_script_new ();
          clutter_script_load_from_file (data->script, path, &error);
          g_free (path);
          data->foreground = CLUTTER_ACTOR (
              clutter_script_get_object (data->script, "foreground"));
          data->midground = CLUTTER_ACTOR (
              clutter_script_get_object (data->script, "midground"));
          data->background2 = CLUTTER_ACTOR (
              clutter_script_get_object (data->script, "background"));
          data->state = CLUTTER_STATE (
              clutter_script_get_object (data->script, "state"));
          data->json_slide = CLUTTER_ACTOR (
              clutter_script_get_object (data->script, "actor"));

          clutter_actor_add_child (renderer->json_layer, data->json_slide);
          g_signal_connect (data->state, "completed",
                            G_CALLBACK (state_completed), point);
          clutter_state_warp_to_state (data->state, "pre");

          if (data->background2) /* parmanently steal background */
            {
              pp_actor_reparent (data->background, data->background2);
            }
        }

      clutter_actor_set_size (data->json_slide,
                              clutter_actor_get_width (renderer->stage),
                              clutter_actor_get_height (renderer->stage));

      clutter_actor_set_size (data->foreground,
                              clutter_actor_get_width (renderer->stage),
                              clutter_actor_get_height (renderer->stage));

      clutter_actor_set_size (data->background2,
                              clutter_actor_get_width (renderer->stage),
                              clutter_actor_get_height (renderer->stage));

      if (!data->json_slide)
        {
          g_warning ("failed to load transition %s %s\n",
                     point->transition, error?error->message:"");
          return;
        }

      if (data->foreground)
        {
          pp_actor_reparent (data->text, data->foreground);
        }

      clutter_actor_set_opacity (data->background, 255);

      {
       float text_x, text_y, text_width, text_height, text_scale;

       clutter_actor_get_size (data->text, &text_width, &text_height);
       pp_get_text_position_scale (point,
                                   clutter_actor_get_width (renderer->stage),
                                   clutter_actor_get_height (renderer->stage),
                                   text_width, text_height,
                                   &text_x, &text_y,
                                   &text_scale);
       g_object_set (data->text,
                     "depth",   0.0,
                     "scale-x", text_scale,
                     "scale-y", text_scale,
                     "x",       text_x,
                     "y",       text_y,
                     NULL);

       if (clutter_actor_get_width (data->text) > 1.0)
         {
           ClutterColor color;
           float shading_x, shading_y, shading_width, shading_height;
           clutter_color_from_string (&color, point->shading_color);

           pp_get_shading_position_size (
                clutter_actor_get_width (renderer->stage),
                clutter_actor_get_height (renderer->stage),
                text_x, text_y,
                text_width, text_height,
                text_scale,
                &shading_x, &shading_y,
                &shading_width, &shading_height);

           if (!data->shading)
             {
               data->shading = pp_rectangle_new_with_color (&black);

               clutter_actor_add_child (data->midground, data->shading);
               clutter_actor_set_size (data->midground,
                                    clutter_actor_get_width (renderer->stage),
                                    clutter_actor_get_height (renderer->stage));
             }

           g_object_set (data->shading,
                         "depth",            -0.01,
                         "x",                shading_x,
                         "y",                shading_y,
                         "opacity",          (int)(point->shading_opacity*255),
                         "background-color", &color,
                         "width",            shading_width,
                         "height",           shading_height,
                         NULL);
         }
       else /* no text, fade out shading */
         if (data->shading)
           g_object_set (data->shading, "opacity", 0, NULL);
       if (data->foreground)
         {
           pp_actor_reparent (data->text, data->foreground);
         }
      }

      if (!backwards)
        clutter_actor_set_child_below_sibling (clutter_actor_get_parent (data->json_slide),
                                               data->json_slide,
                                               NULL);

      clutter_actor_show (data->json_slide);
      clutter_state_set_state (data->state, "show");
    }

  /* render potentially executed commands */
  {
   float text_x, text_y, text_width, text_height;

   clutter_color_from_string (&color, point->text_color);
   g_object_set (renderer->commandline,
                 "font-name", point->font,
                 "text",      point->command?point->command:"",
                 "color",     &color,
                 NULL);

   color.alpha *= 0.33;
   g_object_set (renderer->commandline,
                 "selection-color", &color,
                 NULL);

   clutter_actor_get_size (renderer->commandline, &text_width, &text_height);

   if (point->position == CLUTTER_GRAVITY_SOUTH ||
       point->position == CLUTTER_GRAVITY_SOUTH_WEST)
     text_y = clutter_actor_get_height (renderer->stage) * 0.05;
   else
     text_y = clutter_actor_get_height (renderer->stage) * 0.95 - text_height;

   text_x = clutter_actor_get_width (renderer->stage) * 0.05;

   g_object_set (renderer->commandline,
                 "x", text_x,
                 "y", text_y,
                 NULL);
   pp_actor_animate (renderer->commandline,
                     CLUTTER_EASE_OUT_QUINT, 1000,
                     "opacity", point->command?(gint)(0xff*0.33):0,
                     NULL);

   update_commandline_shading (renderer);
  }

  if (renderer->speaker_mode)
    {
      update_speaker_screen (renderer);
    }
}

static void
stage_resized (ClutterActor    *actor,
               GParamSpec      *pspec,
               ClutterRenderer *renderer)
{
  show_slide (renderer, FALSE); /* redisplay the current slide */
  update_speaker_screen (renderer);
}

static guint reload_tag = 0;

static gboolean
reload (gpointer data)
{
  ClutterRenderer *renderer = data;
  char            *text     = NULL;

  if (!g_file_get_contents (renderer->path, &text, NULL, NULL))
    g_error ("failed to load slides from %s\n", renderer->path);

  renderer->rest_y = STARTPOS;
  pp_parse_slides (PINPOINT_RENDERER (renderer), text);
  g_free (text);
  show_slide(renderer, FALSE);
  reload_tag = 0;
  return FALSE;
}

static void
file_changed (GFileMonitor      *monitor,
              GFile             *file,
              GFile             *other_file,
              GFileMonitorEvent  event_type,
              ClutterRenderer   *renderer)
{
  if (reload_tag)
    g_source_remove (reload_tag);

  reload_tag = g_timeout_add (200, reload, renderer);
}

static ClutterRenderer clutter_renderer_vtable =
{
  .renderer =
    {
      .init = clutter_renderer_init,
      .run = clutter_renderer_run,
      .finalize = clutter_renderer_finalize,
      .make_point = clutter_renderer_make_point,
      .allocate_data = clutter_renderer_allocate_data,
      .free_data = clutter_renderer_free_data
    }
};

PinPointRenderer *pp_clutter_renderer (void)
{
  return (void*)&clutter_renderer_vtable;
}
