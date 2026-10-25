/* videoinfo.c - command to list video modes.  */
/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2005,2007,2008,2009,2010  Free Software Foundation, Inc.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <grub/video.h>
#include <grub/dl.h>
#include <grub/env.h>
#include <grub/misc.h>
#include <grub/mm.h>
#include <grub/command.h>
#include <grub/extcmd.h>
#include <grub/i18n.h>
#include <grub/bitmap.h>
#include <grub/bitmap_scale.h>
#include <grub/font.h>
#include <grub/gfxmenu_view.h>
#include <grub/bind.h>

GRUB_MOD_LICENSE ("GPLv3+");

struct hook_ctx
{
  unsigned height, width, depth;
  struct grub_video_mode_info *current_mode;
};

static int
hook (const struct grub_video_mode_info *info, void *hook_arg)
{
  struct hook_ctx *ctx = hook_arg;

  if (ctx->height && ctx->width && (info->width != ctx->width || info->height != ctx->height))
    return 0;

  if (ctx->depth && info->bpp != ctx->depth)
    return 0;

  if (info->mode_number == GRUB_VIDEO_MODE_NUMBER_INVALID)
    grub_printf ("        ");
  else
    {
      if (ctx->current_mode && info->mode_number == ctx->current_mode->mode_number)
	grub_printf ("*");
      else
	grub_printf (" ");
      grub_printf (" 0x%03x ", info->mode_number);
    }
  grub_printf ("%4d x %4d x %2d (%4d)  ", info->width, info->height, info->bpp,
	       info->pitch);

  if (info->mode_type & GRUB_VIDEO_MODE_TYPE_PURE_TEXT)
    grub_xputs (_("Text-only "));
  /* Show mask and position details for direct color modes.  */
  if (info->mode_type & GRUB_VIDEO_MODE_TYPE_RGB)
    /* TRANSLATORS: "Direct color" is a mode when the color components
       are written dirrectly into memory.  */
    grub_printf_ (N_("Direct color, mask: %d/%d/%d/%d  pos: %d/%d/%d/%d"),
		  info->red_mask_size,
		  info->green_mask_size,
		  info->blue_mask_size,
		  info->reserved_mask_size,
		  info->red_field_pos,
		  info->green_field_pos,
		  info->blue_field_pos,
		  info->reserved_field_pos);
  if (info->mode_type & GRUB_VIDEO_MODE_TYPE_INDEX_COLOR)
    /* TRANSLATORS: In "paletted color" mode you write the index of the color
       in the palette. Synonyms include "packed pixel".  */
    grub_xputs (_("Paletted "));
  if (info->mode_type & GRUB_VIDEO_MODE_TYPE_YUV)
    grub_xputs (_("YUV "));
  if (info->mode_type & GRUB_VIDEO_MODE_TYPE_PLANAR)
    /* TRANSLATORS: "Planar" is the video memory where you have to write
       in several different banks "plans" to control the different color
       components of the same pixel.  */
    grub_xputs (_("Planar "));
  if (info->mode_type & GRUB_VIDEO_MODE_TYPE_HERCULES)
    grub_xputs (_("Hercules "));
  if (info->mode_type & GRUB_VIDEO_MODE_TYPE_CGA)
    grub_xputs (_("CGA "));
  if (info->mode_type & GRUB_VIDEO_MODE_TYPE_NONCHAIN4)
    /* TRANSLATORS: Non-chain 4 is a 256-color planar
       (unchained) video memory mode.  */
    grub_xputs (_("Non-chain 4 "));
  if (info->mode_type & GRUB_VIDEO_MODE_TYPE_1BIT_BITMAP)
    grub_xputs (_("Monochrome "));
  if (info->mode_type & GRUB_VIDEO_MODE_TYPE_UNKNOWN)
    grub_xputs (_("Unknown video mode "));

  grub_xputs ("\n");

  return 0;
}

static void
print_edid (struct grub_video_edid_info *edid_info)
{
  unsigned int edid_width, edid_height;

  if (grub_video_edid_checksum (edid_info))
    {
      grub_puts_ (N_("  EDID checksum invalid"));
      grub_errno = GRUB_ERR_NONE;
      return;
    }

  grub_printf_ (N_("  EDID version: %u.%u\n"),
		edid_info->version, edid_info->revision);
  if (grub_video_edid_preferred_mode (edid_info, &edid_width, &edid_height)
	== GRUB_ERR_NONE)
    grub_printf_ (N_("    Preferred mode: %ux%u\n"), edid_width, edid_height);
  else
    {
      grub_printf_ (N_("    No preferred mode available\n"));
      grub_errno = GRUB_ERR_NONE;
    }
}

struct s_hook_ctx
{
  grub_size_t len;
  char *data;
};

static int
s_hook (const struct grub_video_mode_info *info, void *hook_arg)
{
  struct s_hook_ctx *ctx = hook_arg;
  char buf[24];
  grub_size_t len;

  if (info->mode_type & GRUB_VIDEO_MODE_TYPE_PURE_TEXT)
    return 0;

  grub_snprintf (buf, sizeof (buf), "%dx%dx%d ",
		 info->width, info->height, info->bpp);
  len = grub_strlen (buf);
  if (ctx->data)
    grub_memcpy (ctx->data + ctx->len, buf, len);
  ctx->len += len;
  return 0;
}

static grub_err_t
grub_cmd_videoinfo (grub_command_t cmd __attribute__ ((unused)),
		    int argc, char **args)
{
  grub_video_adapter_t adapter;
  grub_video_driver_id_t id;
  struct hook_ctx ctx;

  ctx.height = ctx.width = ctx.depth = 0;
  if (argc)
    {
      const char *ptr;
      ptr = args[0];
      ctx.width = grub_strtoul (ptr, &ptr, 0);
      if (grub_errno)
	return grub_errno;
      if (*ptr != 'x')
	return grub_error (GRUB_ERR_BAD_ARGUMENT,
			   N_("invalid video mode specification `%s'"),
			   args[0]);
      ptr++;
      ctx.height = grub_strtoul (ptr, &ptr, 0);
      if (grub_errno)
	return grub_errno;
      if (*ptr == 'x')
	{
	  ptr++;
	  ctx.depth = grub_strtoul (ptr, &ptr, 0);
	  if (grub_errno)
	    return grub_errno;
	}
    }

#ifdef GRUB_MACHINE_PCBIOS
  if (grub_strcmp (cmd->name, "vbeinfo") == 0)
    grub_dl_load ("vbe");
#endif

  id = grub_video_get_driver_id ();

  grub_puts_ (N_("List of supported video modes:"));
  grub_puts_ (N_("Legend: mask/position=red/green/blue/reserved"));

  FOR_VIDEO_ADAPTERS (adapter)
  {
    struct grub_video_mode_info info;
    struct grub_video_edid_info edid_info;

    grub_printf_ (N_("Adapter `%s':\n"), adapter->name);

    if (!adapter->iterate)
      {
	grub_puts_ (N_("  No info available"));
	continue;
      }

    ctx.current_mode = NULL;

    if (adapter->id == id)
      {
	if (grub_video_get_info (&info) == GRUB_ERR_NONE)
	  ctx.current_mode = &info;
	else
	  /* Don't worry about errors.  */
	  grub_errno = GRUB_ERR_NONE;
      }
    else if (id != GRUB_VIDEO_DRIVER_NONE)
      {
	grub_puts_ (N_("  A video driver is active, cannot initialize this driver until it is deactivated\n"));
	continue;
      }
    else
      {
	if (adapter->init ())
	  {
	    grub_puts_ (N_("  Failed to initialize video adapter"));
	    grub_errno = GRUB_ERR_NONE;
	    continue;
	  }
      }

    if (adapter->print_adapter_specific_info)
      adapter->print_adapter_specific_info ();

    adapter->iterate (hook, &ctx);

    if (adapter->get_edid && adapter->get_edid (&edid_info) == GRUB_ERR_NONE)
      print_edid (&edid_info);
    else
      grub_errno = GRUB_ERR_NONE;

    ctx.current_mode = NULL;

    if (adapter->id != id)
      {
	if (adapter->fini ())
	  {
	    grub_errno = GRUB_ERR_NONE;
	    continue;
	  }
      }
  }
  return GRUB_ERR_NONE;
}

static const struct grub_arg_option videomode_options[] =
{
  {"list", 'l', 0, N_("List video modes and save to variable."), 0, 0},
  {"current", 'c', 0, N_("Get current mode (WxH) and save to variable."), 0, 0},
  {0, 0, 0, 0, 0, 0}
};

enum videomode_options
{
  GFXMODE_LIST,
  GFXMODE_CURRENT
};

/* Space separated "WxHxD " list of the active (or first usable) adapter.  */
static char *
video_mode_list (void)
{
  grub_video_adapter_t adapter;
  grub_video_driver_id_t id;
  struct s_hook_ctx ctx;

#ifdef GRUB_MACHINE_PCBIOS
  grub_dl_load ("vbe");
#endif

  id = grub_video_get_driver_id ();
  FOR_VIDEO_ADAPTERS (adapter)
    {
      if (!adapter->iterate)
	continue;

      if (adapter->id != id)
	{
	  if (id != GRUB_VIDEO_DRIVER_NONE)
	    continue;
	  if (adapter->init () != GRUB_ERR_NONE)
	    {
	      grub_errno = GRUB_ERR_NONE;
	      continue;
	    }
	}

      ctx.data = NULL;
      ctx.len = 0;
      adapter->iterate (s_hook, &ctx);
      ctx.data = grub_malloc (ctx.len + 1);
      if (!ctx.data)
	{
	  if (adapter->id != id)
	    adapter->fini ();
	  return NULL;
	}
      ctx.len = 0;
      adapter->iterate (s_hook, &ctx);
      ctx.data[ctx.len] = '\0';

      if (adapter->id != id)
	adapter->fini ();

      if (id != GRUB_VIDEO_DRIVER_NONE || ctx.len)
	return ctx.data;

      grub_free (ctx.data);
    }

  return grub_strdup ("");
}

static grub_err_t
grub_cmd_videomode (grub_extcmd_context_t ctxt, int argc, char **args)
{
  struct grub_arg_list *state = ctxt->state;

  if (argc != 1)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("variable name expected"));

  if (state[GFXMODE_CURRENT].set)
    {
      char cur[20];
      struct grub_video_mode_info info;
      unsigned int width = 0, height = 0;

      if (grub_video_get_info (&info) == GRUB_ERR_NONE)
	{
	  width = info.width;
	  height = info.height;
	}
      else
	grub_errno = GRUB_ERR_NONE;

      grub_snprintf (cur, sizeof (cur), "%ux%u", width, height);
      grub_env_set (args[0], cur);
      return 0;
    }

  {
    char *modes = video_mode_list ();

    if (!modes)
      return grub_errno;
    grub_env_set (args[0], modes);
    grub_free (modes);
  }
  return 0;
}

/* Video bindings use typed records for colors and managed handles for
   bitmaps.  No Lua VM or raw bitmap pointer enters this native module. */
static grub_dl_t video_module;

static void
video_bitmap_destroy (void *data)
{
  grub_video_bitmap_destroy (data);
}

static struct grub_video_bitmap *
video_bitmap_from_value (const struct grub_bind_value *value)
{
  return grub_bind_handle_data (value->value.handle, "video.bitmap");
}

static grub_err_t
video_bind_color (const struct grub_bind_value *value, grub_video_color_t *out)
{
  grub_uint8_t rgba[4] = { 0, 0, 0, 255 };
  unsigned seen = 0;
  grub_size_t i;
  static const char *keys[] = { "r", "g", "b", "a" };
  if (value->type != GRUB_BIND_RECORD)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "video color must be a record");
  for (i = 0; i < value->value.record.count; i++)
    {
      const struct grub_bind_field *field = &value->value.record.fields[i];
      unsigned j;
      for (j = 0; j < 4; j++)
        if (!grub_strcmp (keys[j], field->name))
          {
            if (field->value.type != GRUB_BIND_I64 ||
                field->value.value.i64 < 0 || field->value.value.i64 > 255)
              return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid video color");
            rgba[j] = field->value.value.i64;
            seen |= 1U << j;
            break;
          }
    }
  if ((seen & 7U) != 7U)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "video color missing r/g/b");
  *out = grub_video_map_rgba (rgba[0], rgba[1], rgba[2], rgba[3]);
  return GRUB_ERR_NONE;
}

static grub_err_t
bind_video_swap_buffers (grub_size_t argc __attribute__ ((unused)),
                         const struct grub_bind_value *args __attribute__ ((unused)),
                         struct grub_bind_value *results __attribute__ ((unused)),
                         grub_size_t *nresults)
{
  *nresults = 0;
  return grub_video_swap_buffers ();
}

/* Native video calls narrow coordinates to int and extents to unsigned.
   Reject values not representable in those target types. */
static grub_err_t
video_bind_position (const struct grub_bind_value *value)
{
  if (value->value.i64 < -2147483648LL ||
      value->value.i64 > 2147483647LL)
    return grub_error (GRUB_ERR_OUT_OF_RANGE, "video coordinate out of range");
  return GRUB_ERR_NONE;
}

static grub_err_t
video_bind_extent (const struct grub_bind_value *value)
{
  if (value->value.u64 > 0xffffffffULL)
    return grub_error (GRUB_ERR_OUT_OF_RANGE, "video extent out of range");
  return GRUB_ERR_NONE;
}

static grub_err_t
bind_video_fill_rect (grub_size_t argc __attribute__ ((unused)),
                      const struct grub_bind_value *args,
                      struct grub_bind_value *results __attribute__ ((unused)),
                      grub_size_t *nresults)
{
  grub_video_color_t color;
  grub_err_t err;
  if ((err = video_bind_position (&args[1])) ||
      (err = video_bind_position (&args[2])) ||
      (err = video_bind_extent (&args[3])) ||
      (err = video_bind_extent (&args[4])) ||
      (err = video_bind_color (&args[0], &color)))
    return err;
  *nresults = 0;
  return grub_video_fill_rect (color, args[1].value.i64,
                              args[2].value.i64, args[3].value.u64,
                              args[4].value.u64);
}

static grub_err_t
bind_video_draw_pixel (grub_size_t argc __attribute__ ((unused)),
                       const struct grub_bind_value *args,
                       struct grub_bind_value *results __attribute__ ((unused)),
                       grub_size_t *nresults)
{
  grub_video_color_t color;
  grub_err_t err;
  if ((err = video_bind_position (&args[1])) ||
      (err = video_bind_position (&args[2])) ||
      (err = video_bind_color (&args[0], &color)))
    return err;
  *nresults = 0;
  return grub_video_fill_rect (color, args[1].value.i64,
                              args[2].value.i64, 1, 1);
}

static grub_err_t
bind_video_get_info (grub_size_t argc __attribute__ ((unused)),
                     const struct grub_bind_value *args __attribute__ ((unused)),
                     struct grub_bind_value *results, grub_size_t *nresults)
{
  struct grub_video_mode_info info;
  unsigned int width = 0, height = 0;
  if (!grub_video_get_info (&info))
    {
      width = info.width;
      height = info.height;
    }
  else
    grub_errno = GRUB_ERR_NONE;
  results[0].type = results[1].type = GRUB_BIND_U64;
  results[0].value.u64 = width;
  results[1].value.u64 = height;
  *nresults = 2;
  return GRUB_ERR_NONE;
}

static grub_err_t
bind_video_draw_string (grub_size_t argc __attribute__ ((unused)),
                        const struct grub_bind_value *args,
                        struct grub_bind_value *results __attribute__ ((unused)),
                        grub_size_t *nresults)
{
  grub_video_color_t color;
  grub_font_t font;
  grub_err_t err;
  if ((err = video_bind_position (&args[3])) ||
      (err = video_bind_position (&args[4])) ||
      (err = video_bind_color (&args[2], &color)))
    return err;
  font = grub_font_get (args[1].value.bytes.data);
  if (!font)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "video font unavailable");
  *nresults = 0;
  return grub_font_draw_string (args[0].value.bytes.data, font, color,
                                args[3].value.i64, args[4].value.i64);
}

static grub_err_t
bind_video_info (grub_size_t argc __attribute__ ((unused)),
                 const struct grub_bind_value *args __attribute__ ((unused)),
                 struct grub_bind_value *results, grub_size_t *nresults)
{
  char *modes = video_mode_list ();
  if (!modes) return grub_errno;
  results[0].type = GRUB_BIND_STRING;
  results[0].owned = 1;
  results[0].value.bytes.data = modes;
  results[0].value.bytes.size = grub_strlen (modes);
  *nresults = 1;
  return GRUB_ERR_NONE;
}

static grub_err_t
video_bind_bitmap (struct grub_video_bitmap *bitmap,
                   struct grub_bind_value *results, grub_size_t *nresults)
{
  struct grub_bind_handle *handle;
  results[0].type = GRUB_BIND_NIL;
  *nresults = 1;
  if (!bitmap)
    {
      grub_errno = GRUB_ERR_NONE;
      return GRUB_ERR_NONE;
    }
  handle = grub_bind_handle_create (video_module, "video.bitmap",
                                   bitmap, video_bitmap_destroy);
  if (!handle)
    {
      grub_video_bitmap_destroy (bitmap);
      return grub_errno;
    }
  results[0].type = GRUB_BIND_HANDLE;
  results[0].value.handle = handle;
  return GRUB_ERR_NONE;
}

static grub_err_t
bind_video_bitmap_load (grub_size_t argc __attribute__ ((unused)),
                        const struct grub_bind_value *args,
                        struct grub_bind_value *results, grub_size_t *nresults)
{
  struct grub_video_bitmap *bitmap = NULL;
  grub_video_bitmap_load (&bitmap, args[0].value.bytes.data);
  return video_bind_bitmap (bitmap, results, nresults);
}

static grub_err_t
bind_video_bitmap_close (grub_size_t argc __attribute__ ((unused)),
                         const struct grub_bind_value *args,
                         struct grub_bind_value *results __attribute__ ((unused)),
                         grub_size_t *nresults)
{
  if (!video_bitmap_from_value (&args[0])) return grub_errno;
  *nresults = 0;
  return grub_bind_handle_close (args[0].value.handle);
}

static grub_err_t
bind_video_bitmap_info (grub_size_t argc __attribute__ ((unused)),
                        const struct grub_bind_value *args,
                        struct grub_bind_value *results, grub_size_t *nresults)
{
  struct grub_video_bitmap *bitmap = video_bitmap_from_value (&args[0]);
  if (!bitmap) return grub_errno;
  results[0].type = results[1].type = GRUB_BIND_U64;
  results[0].value.u64 = grub_video_bitmap_get_width (bitmap);
  results[1].value.u64 = grub_video_bitmap_get_height (bitmap);
  *nresults = 2;
  return GRUB_ERR_NONE;
}

static grub_err_t
bind_video_bitmap_blit (grub_size_t argc __attribute__ ((unused)),
                        const struct grub_bind_value *args,
                        struct grub_bind_value *results __attribute__ ((unused)),
                        grub_size_t *nresults)
{
  struct grub_video_bitmap *bitmap = video_bitmap_from_value (&args[0]);
  grub_err_t err;
  if (!bitmap) return grub_errno;
  if ((err = video_bind_position (&args[1])) ||
      (err = video_bind_position (&args[2])) ||
      (err = video_bind_position (&args[3])) ||
      (err = video_bind_position (&args[4])) ||
      (err = video_bind_extent (&args[5])) ||
      (err = video_bind_extent (&args[6])))
    return err;
  *nresults = 0;
  return grub_video_blit_bitmap (bitmap, GRUB_VIDEO_BLIT_BLEND,
                                 args[1].value.i64, args[2].value.i64,
                                 args[3].value.i64, args[4].value.i64,
                                 args[5].value.u64, args[6].value.u64);
}

static grub_err_t
bind_video_bitmap_rescale (grub_size_t argc __attribute__ ((unused)),
                           const struct grub_bind_value *args,
                           struct grub_bind_value *results, grub_size_t *nresults)
{
  struct grub_video_bitmap *bitmap = video_bitmap_from_value (&args[0]);
  struct grub_video_bitmap *scaled = NULL;
  grub_err_t err;
  if (!bitmap) return grub_errno;
  if (args[1].value.i64 <= 0 || args[2].value.i64 <= 0 ||
      args[1].value.i64 > 2147483647LL || args[2].value.i64 > 2147483647LL)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid bitmap dimensions");
  err = grub_video_bitmap_create_scaled (&scaled, args[1].value.i64,
                                         args[2].value.i64, bitmap,
                                         GRUB_VIDEO_BITMAP_SCALE_METHOD_BEST);
  if (err)
    {
      /* Never expose a partially initialized scaled bitmap. */
      if (scaled)
        grub_video_bitmap_destroy (scaled);
      grub_errno = GRUB_ERR_NONE;
      results[0].type = GRUB_BIND_NIL;
      *nresults = 1;
      return GRUB_ERR_NONE;
    }
  return video_bind_bitmap (scaled, results, nresults);
}

static const enum grub_bind_type video_fill_args[] =
  { GRUB_BIND_RECORD, GRUB_BIND_I64, GRUB_BIND_I64, GRUB_BIND_U64, GRUB_BIND_U64 };
static const enum grub_bind_type video_pixel_args[] =
  { GRUB_BIND_RECORD, GRUB_BIND_I64, GRUB_BIND_I64 };
static const enum grub_bind_type video_string_args[] =
  { GRUB_BIND_STRING, GRUB_BIND_STRING, GRUB_BIND_RECORD, GRUB_BIND_I64, GRUB_BIND_I64 };
static const enum grub_bind_type video_path_args[] = { GRUB_BIND_STRING };
static const enum grub_bind_type video_bitmap_args[] = { GRUB_BIND_HANDLE };
static const enum grub_bind_type video_blit_args[] =
  { GRUB_BIND_HANDLE, GRUB_BIND_I64, GRUB_BIND_I64, GRUB_BIND_I64,
    GRUB_BIND_I64, GRUB_BIND_U64, GRUB_BIND_U64 };
static const enum grub_bind_type video_scale_args[] =
  { GRUB_BIND_HANDLE, GRUB_BIND_I64, GRUB_BIND_I64 };

static const struct grub_bind_export video_exports[] =
{
  { "video", "swap_buffers",   NULL,               0, 0, bind_video_swap_buffers },
  { "video", "fill_rect",      video_fill_args,    5, 5, bind_video_fill_rect },
  { "video", "draw_pixel",     video_pixel_args,   3, 3, bind_video_draw_pixel },
  { "video", "get_info",       NULL,               0, 0, bind_video_get_info },
  { "video", "draw_string",    video_string_args,  5, 5, bind_video_draw_string },
  { "video", "info",           NULL,               0, 0, bind_video_info },
  { "video", "bitmap_load",    video_path_args,    1, 1, bind_video_bitmap_load },
  { "video", "bitmap_close",   video_bitmap_args,  1, 1, bind_video_bitmap_close },
  { "video", "bitmap_info",    video_bitmap_args,  1, 1, bind_video_bitmap_info },
  { "video", "bitmap_blit",    video_blit_args,    7, 7, bind_video_bitmap_blit },
  { "video", "bitmap_rescale", video_scale_args,   3, 3, bind_video_bitmap_rescale }
};

static grub_command_t cmd;
#ifdef GRUB_MACHINE_PCBIOS
static grub_command_t cmd_vbe;
#endif
static grub_extcmd_t cmd_videomode;

GRUB_MOD_INIT(videoinfo)
{
  cmd = grub_register_command ("videoinfo", grub_cmd_videoinfo,
			       /* TRANSLATORS: "x" has to be entered in,
				  like an identifier, so please don't
				  use better Unicode codepoints.  */
			       N_("[WxH[xD]]"),
			       N_("List available video modes. If "
				     "resolution is given show only modes"
				     " matching it."));
#ifdef GRUB_MACHINE_PCBIOS
  cmd_vbe = grub_register_command ("vbeinfo", grub_cmd_videoinfo,
				   /* TRANSLATORS: "x" has to be entered in,
				      like an identifier, so please don't
				      use better Unicode codepoints.  */
				   N_("[WxH[xD]]"),
				   N_("List available video modes. If "
				      "resolution is given show only modes"
				      " matching it."));
#endif
  cmd_videomode = grub_register_extcmd ("videomode", grub_cmd_videomode, 0,
					N_("[--list|--current] VAR"),
					N_("Store video mode information in variable VAR."),
					videomode_options);
  video_module = mod;
  if (grub_bind_register (mod, video_exports, 11))
    grub_print_error ();
}

GRUB_MOD_FINI(videoinfo)
{
  grub_bind_unregister (video_module);
  video_module = NULL;
  grub_unregister_command (cmd);
#ifdef GRUB_MACHINE_PCBIOS
  grub_unregister_command (cmd_vbe);
#endif
  grub_unregister_extcmd (cmd_videomode);
}
