#pragma once
/*
 *  Copyright (C) 2012 Intel Corporation
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public License
 *  as published by the Free Software Foundation; either version 2.1
 *  of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free
 *  Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301 USA
 */

#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "utils/StringUtils.h"

class CWinPlatformKMS;

class CWinEGLPlatformKMS
{
public:
  CWinEGLPlatformKMS();
  virtual ~CWinEGLPlatformKMS();

  virtual EGLNativeWindowType InitWindowSystem(EGLNativeDisplayType nativeDisplay, int width, int height, int bpp);
  virtual void DestroyWindowSystem(EGLNativeWindowType nativeWindow);
  virtual bool SetDisplayResolution(int width, int height, float refresh, bool interlace);
  virtual bool ClampToGUIDisplayLimits(int &width, int &height);
  virtual bool ProbeDisplayResolutions(std::vector<CStdString> &resolutions);
  
  virtual bool InitializeDisplay();
  virtual bool UninitializeDisplay();
  virtual bool CreateWindow();
  virtual bool DestroyWindow();
  virtual bool BindSurface();
  virtual bool ReleaseSurface();
  
  virtual bool ShowWindow(bool show);
  virtual void SwapBuffers();
  virtual bool SetVSync(bool enable);
  virtual bool IsExtSupported(const char* extension);

  virtual EGLDisplay GetEGLDisplay();
  virtual EGLContext GetEGLContext();

protected:
  CStdString const &    getDevicePath();
  EGLNativeDisplayType  getNativeDisplay();
  EGLNativeWindowType   getNativeWindow();

  CWinPlatformKMS *     m_kms;
  CStdString            m_devicePath;
  int                   m_deviceFd;
  struct gbm_device *   m_gbmDevice;
  struct gbm_surface *  m_gbmSurface;
  EGLDisplay            m_display;
  EGLSurface            m_surface;
  EGLConfig             m_config;
  EGLContext            m_context;
  CStdString            m_eglext;
  int                   m_width;
  int                   m_height;
};
