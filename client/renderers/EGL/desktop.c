/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2019 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
cahe terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "desktop.h"
#include "common/debug.h"
#include "utils.h"

#include "texture.h"
#include "shader.h"
#include "model.h"

#include <stdlib.h>
#include <string.h>

#include "interface/app.h"

// these headers are auto generated by cmake
#include "desktop.vert.h"
#include "desktop_rgb.frag.h"
#include "desktop_yuv.frag.h"

struct EGL_Desktop
{
  EGL_Texture * texture;
  EGL_Shader  * shader; // the active shader
  EGL_Model   * model;

  // shader instances
  EGL_Shader * shader_generic;
  EGL_Shader * shader_yuv;

  // uniforms
  GLint uDesktopPos;
  GLint uNV, uNVGain;

  // internals
  enum EGL_PixelFormat pixFmt;
  unsigned int         width, height;
  unsigned int         pitch;
  const uint8_t      * data;
  bool                 update;

  // night vision
  KeybindHandle kbNV;
  bool  nv;
  int   nvGain;
};

// forwards
void egl_desktop_toggle_nv(SDL_Scancode key, void * opaque);

bool egl_desktop_init(EGL_Desktop ** desktop)
{
  *desktop = (EGL_Desktop *)malloc(sizeof(EGL_Desktop));
  if (!*desktop)
  {
    DEBUG_ERROR("Failed to malloc EGL_Desktop");
    return false;
  }

  memset(*desktop, 0, sizeof(EGL_Desktop));

  if (!egl_texture_init(&(*desktop)->texture))
  {
    DEBUG_ERROR("Failed to initialize the desktop texture");
    return false;
  }

  if (!egl_shader_init(&(*desktop)->shader_generic))
  {
    DEBUG_ERROR("Failed to initialize the generic desktop shader");
    return false;
  }

  if (!egl_shader_init(&(*desktop)->shader_yuv))
  {
    DEBUG_ERROR("Failed to initialize the yuv desktop shader");
    return false;
  }

  if (!egl_shader_compile((*desktop)->shader_generic,
        b_shader_desktop_vert    , b_shader_desktop_vert_size,
        b_shader_desktop_rgb_frag, b_shader_desktop_rgb_frag_size))
  {
    DEBUG_ERROR("Failed to compile the generic desktop shader");
    return false;
  }

  if (!egl_shader_compile((*desktop)->shader_yuv,
        b_shader_desktop_vert    , b_shader_desktop_vert_size,
        b_shader_desktop_yuv_frag, b_shader_desktop_yuv_frag_size))
  {
    DEBUG_ERROR("Failed to compile the yuv desktop shader");
    return false;
  }

  if (!egl_model_init(&(*desktop)->model))
  {
    DEBUG_ERROR("Failed to initialize the desktop model");
    return false;
  }

  egl_model_set_default((*desktop)->model);
  egl_model_set_texture((*desktop)->model, (*desktop)->texture);

  (*desktop)->kbNV = app_register_keybind(SDL_SCANCODE_N, egl_desktop_toggle_nv, *desktop);

  return true;
}


void egl_desktop_toggle_nv(SDL_Scancode key, void * opaque)
{
  EGL_Desktop * desktop = (EGL_Desktop *)opaque;
  if (++desktop->nvGain == 4)
    desktop->nvGain = 0;

       if (desktop->nvGain == 0) app_alert(LG_ALERT_INFO, "NV Disabled");
  else if (desktop->nvGain == 1) app_alert(LG_ALERT_INFO, "NV Enabled");
  else app_alert(LG_ALERT_INFO, "NV Gain + %d", desktop->nvGain - 1);
}

void egl_desktop_free(EGL_Desktop ** desktop)
{
  if (!*desktop)
    return;

  egl_texture_free(&(*desktop)->texture       );
  egl_shader_free (&(*desktop)->shader_generic);
  egl_shader_free (&(*desktop)->shader_yuv    );
  egl_model_free  (&(*desktop)->model         );

  app_release_keybind(&(*desktop)->kbNV);

  free(*desktop);
  *desktop = NULL;
}

bool egl_desktop_prepare_update(EGL_Desktop * desktop, const bool sourceChanged, const LG_RendererFormat format, const uint8_t * data)
{
  if (sourceChanged)
  {
    switch(format.type)
    {
      case FRAME_TYPE_BGRA:
        desktop->pixFmt = EGL_PF_BGRA;
        desktop->shader = desktop->shader_generic;
        break;

      case FRAME_TYPE_RGBA:
        desktop->pixFmt = EGL_PF_RGBA;
        desktop->shader = desktop->shader_generic;
        break;

      case FRAME_TYPE_RGBA10:
        desktop->pixFmt = EGL_PF_RGBA10;
        desktop->shader = desktop->shader_generic;
        break;

      case FRAME_TYPE_YUV420:
        desktop->pixFmt = EGL_PF_YUV420;
        desktop->shader = desktop->shader_yuv;
        break;

      default:
        DEBUG_ERROR("Unsupported frame format");
        return false;
    }

    desktop->width  = format.width;
    desktop->height = format.height;
    desktop->pitch  = format.pitch;
  }

  desktop->data   = data;
  desktop->update = true;

  return true;
}

bool egl_desktop_perform_update(EGL_Desktop * desktop, const bool sourceChanged)
{
  if (sourceChanged)
  {
    if (desktop->shader)
    {
      desktop->uDesktopPos = egl_shader_get_uniform_location(desktop->shader, "position");
      desktop->uNV         = egl_shader_get_uniform_location(desktop->shader, "nv"      );
      desktop->uNVGain     = egl_shader_get_uniform_location(desktop->shader, "nvGain"  );
    }

    if (!egl_texture_setup(
      desktop->texture,
      desktop->pixFmt,
      desktop->width,
      desktop->height,
      desktop->pitch,
      true // streaming texture
    ))
    {
      DEBUG_ERROR("Failed to setup the desktop texture");
      return false;
    }
  }

  if (!desktop->update)
    return true;

  if (!egl_texture_update(desktop->texture, desktop->data))
  {
    DEBUG_ERROR("Failed to update the desktop texture");
    return false;
  }

  desktop->update = false;
  return true;
}

void egl_desktop_render(EGL_Desktop * desktop, const float x, const float y, const float scaleX, const float scaleY)
{
  if (!desktop->shader)
    return;

  egl_shader_use(desktop->shader);
  glUniform4f(desktop->uDesktopPos, x, y, scaleX, scaleY);
  if (desktop->nvGain)
  {
    glUniform1i(desktop->uNV, 1);
    glUniform1f(desktop->uNVGain, (float)desktop->nvGain);
  }
  else
    glUniform1i(desktop->uNV, 0);

  egl_model_render(desktop->model);
}