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

#include "system.h"
#include "system_gl.h"

#if defined(HAS_EGL) && defined(HAVE_KMS)
#include <string>
#include <poll.h>
#include <fcntl.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "WinEGLPlatformKMS.h"
#include "utils/log.h"
#include "threads/Thread.h"

class CPageFlipThread : public CThread
{
  CWinPlatformKMS *     m_kms;

public:
                        CPageFlipThread(CWinPlatformKMS *kms);
  virtual              ~CPageFlipThread();

protected:
  void                  Process();
};

class CWinPlatformKMS : public CCriticalSection
{
  struct CBufferInfo
  {
                        CBufferInfo();
    bool                isValid() const;

    struct gbm_surface *surface;
    struct gbm_bo *     buffer;
    uint32_t            bufferId;
  };

  CBufferInfo *         GetNextBuffer();
  void                  ReleaseBuffer(CBufferInfo *bi);
  void                  ReleaseResources();

  friend class          CPageFlipThread;
  void                  HandlePageFlip();
  void                  OnPageFlip();
  static void           OnPageFlipFunc(int fd, unsigned int frame,
                                       unsigned int sec, unsigned int usec,
                                       void *data);

public:
                        CWinPlatformKMS(int deviceFd);
                        ~CWinPlatformKMS();

  bool                  Reset();
  void                  WaitPageFlip();
  bool                  FlipSurface(struct gbm_surface *s);

  int                   m_deviceFd;
  drmModeConnector *    m_connector;
  drmModeEncoder *      m_encoder;
  drmModeModeInfo *     m_modes;
  unsigned int          m_modeCount;
  drmModeModeInfo       m_mode;
  drmModeCrtc *         m_savedCRTC;
  bool                  m_bSetCRTC;
  CBufferInfo           m_buffers[2];
  unsigned int          m_bufferIndex;
  unsigned int          m_pageFlipPending;
  CPageFlipThread *     m_pageFlipThread;
};

CPageFlipThread::CPageFlipThread(CWinPlatformKMS *kms)
  : CThread("CPageFlipThread")
  , m_kms(kms)
{
}

CPageFlipThread::~CPageFlipThread()
{
}

void CPageFlipThread::Process()
{
  while (!m_bStop)
  {
    struct pollfd pfd;

    pfd.fd      = m_kms->m_deviceFd;
    pfd.events  = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, 10) == 1)
      m_kms->HandlePageFlip();
  }
}

CWinPlatformKMS::CBufferInfo::CBufferInfo()
  : surface(NULL)
  , buffer(NULL)
  , bufferId(0)
{
}

inline bool CWinPlatformKMS::CBufferInfo::isValid() const
{
  return surface && buffer && bufferId;
}

CWinPlatformKMS::CWinPlatformKMS(int deviceFd)
  : m_deviceFd(deviceFd)
  , m_connector(NULL)
  , m_encoder(NULL)
  , m_modes(NULL)
  , m_modeCount(0)
  , m_savedCRTC(NULL)
  , m_bSetCRTC(false)
  , m_bufferIndex(0)
  , m_pageFlipPending(0)
  , m_pageFlipThread(new CPageFlipThread(this))
{
}

CWinPlatformKMS::~CWinPlatformKMS()
{
  ReleaseResources();
}

inline CWinPlatformKMS::CBufferInfo *CWinPlatformKMS::GetNextBuffer()
{
  return &m_buffers[m_bufferIndex ^ 1];
}

void CWinPlatformKMS::ReleaseBuffer(CBufferInfo *bi)
{
  if (bi->bufferId)
  {
    drmModeRmFB(m_deviceFd, bi->bufferId);
    bi->bufferId = 0;
  }

  if (bi->surface)
  {
    if (bi->buffer)
    {
      gbm_surface_release_buffer(bi->surface, bi->buffer);
      bi->buffer = NULL;
    }
    bi->surface = NULL;
  }
}

void CWinPlatformKMS::ReleaseResources()
{
  m_pageFlipThread->StopThread();

  if (m_modes)
  {
    delete[] m_modes;
    m_modeCount = 0;
  }

  if (m_encoder)
  {
    drmModeFreeEncoder(m_encoder);
    m_encoder = NULL;
  }

  if (m_connector)
  {
    if (m_savedCRTC)
    {
      drmModeSetCrtc(m_deviceFd, m_savedCRTC->crtc_id, m_savedCRTC->buffer_id,
          m_savedCRTC->x, m_savedCRTC->y, &m_connector->connector_id, 1,
          &m_savedCRTC->mode);
      drmModeFreeCrtc(m_savedCRTC);
      m_savedCRTC = NULL;
    }
    drmModeFreeConnector(m_connector);
    m_connector = NULL;
  }
}

bool CWinPlatformKMS::Reset()
{
  drmModeRes *resources;
  int i;

  ReleaseResources();

  if (m_deviceFd < 0)
    return false;

  resources = drmModeGetResources(m_deviceFd);
  if (!resources)
  {
    CLog::Log(LOGERROR, "KMS: failed to obtain resources");
    return false;
  }

  for (i = 0; i < resources->count_connectors; i++)
  {
    drmModeConnector *connector;

    connector = drmModeGetConnector(m_deviceFd, resources->connectors[i]);
    if (!connector)
      continue;

    if (connector->connection == DRM_MODE_CONNECTED &&
        connector->count_modes > 0)
    {
      m_connector = connector;
      break;
    }
    drmModeFreeConnector(connector);
  }
  if (!m_connector)
  {
    CLog::Log(LOGERROR, "KMS: failed to find an active connector");
    drmModeFreeResources(resources);
    return false;
  }

  for (i = 0; i < resources->count_encoders; i++)
  {
    drmModeEncoder *encoder;

    encoder = drmModeGetEncoder(m_deviceFd, resources->encoders[i]);
    if (!encoder)
      continue;

    if (encoder->encoder_id == m_connector->encoder_id)
    {
      m_encoder = encoder;
      break;
    }
    drmModeFreeEncoder(encoder);
  }
  if (!m_encoder)
  {
    CLog::Log(LOGERROR, "KMS: failed to find encoder for selected connector");
    drmModeFreeResources(resources);
    return false;
  }

  m_bSetCRTC = true;
  m_savedCRTC = drmModeGetCrtc(m_deviceFd, m_encoder->crtc_id);
  m_mode = m_connector->modes[0];
  m_modeCount = m_connector->count_modes;
  m_modes = new drmModeModeInfo[m_modeCount];
  memcpy(m_modes, m_connector->modes, m_modeCount * sizeof(m_modes[0]));
  drmModeFreeResources(resources);

  m_pageFlipThread->Create();
  return true;
}

void CWinPlatformKMS::OnPageFlip()
{
  CSingleLock lock(*this);

  if (--m_pageFlipPending > 0)
    return;

  ReleaseBuffer(&m_buffers[m_bufferIndex]);
  m_bufferIndex ^= 1;
}

void CWinPlatformKMS::OnPageFlipFunc(int fd, unsigned int frame,
                                     unsigned int sec, unsigned int usec,
                                     void *data)
{
  static_cast<CWinPlatformKMS *>(data)->OnPageFlip();
}

void CWinPlatformKMS::HandlePageFlip()
{
  CSingleLock lock(*this);

  if (!m_pageFlipPending)
    return;

  drmEventContext evctx;
  memset(&evctx, 0, sizeof(evctx));
  evctx.version = DRM_EVENT_CONTEXT_VERSION;
  evctx.page_flip_handler = CWinPlatformKMS::OnPageFlipFunc;
  drmHandleEvent(m_deviceFd, &evctx);
}

bool CWinPlatformKMS::FlipSurface(struct gbm_surface *s)
{
  CSingleLock lock(*this);

  if (GetNextBuffer()->isValid())
    HandlePageFlip();

  CBufferInfo * const bi = GetNextBuffer();
  bi->surface = s;
  bi->buffer = gbm_surface_lock_front_buffer(s);
  if (!bi->buffer)
  {
    CLog::Log(LOGERROR, "KMS: failed to lock surface front buffer");
    ReleaseBuffer(bi);
    return false;
  }

  if (drmModeAddFB(m_deviceFd, m_mode.hdisplay, m_mode.vdisplay, 24, 32,
                   gbm_bo_get_stride(bi->buffer),
                   gbm_bo_get_handle(bi->buffer).u32,
                   &bi->bufferId))
  {
    CLog::Log(LOGERROR, "KMS: failed to add frame buffer");
    ReleaseBuffer(bi);
    return false;
  }

  if (m_bSetCRTC)
  {
    if (drmModeSetCrtc(m_deviceFd, m_encoder->crtc_id, bi->bufferId, 0, 0,
                       &m_connector->connector_id, 1, &m_mode))
    {
      CLog::Log(LOGERROR, "KMS: failed to bind framebuffer to CRTC");
      ReleaseBuffer(bi);
      return false;
    }
    m_bSetCRTC = false;
  }

  if (drmModePageFlip(m_deviceFd, m_encoder->crtc_id, bi->bufferId,
                      DRM_MODE_PAGE_FLIP_EVENT, this))
  {
    CLog::Log(LOGERROR, "KMS: failed to request a page flip");
    ReleaseBuffer(bi);
    return false;
  }
  m_pageFlipPending++;
  return true;
}

CWinEGLPlatformKMS::CWinEGLPlatformKMS()
{
  m_kms        = NULL;
  m_devicePath = "/dev/dri/card0";
  m_deviceFd   = -1;

  m_surface = EGL_NO_SURFACE;
  m_context = EGL_NO_CONTEXT;
  m_display = EGL_NO_DISPLAY;

  // Default to 720p resolution
  m_width  = 1280;
  m_height = 720;
}

CWinEGLPlatformKMS::~CWinEGLPlatformKMS()
{
  UninitializeDisplay();
}

EGLNativeWindowType CWinEGLPlatformKMS::InitWindowSystem(EGLNativeDisplayType nativeDisplay, int width, int height, int bpp)
{
  m_gbmDevice = static_cast<struct gbm_device *>(nativeDisplay);
  m_width  = width;
  m_height = height;

  CLog::Log(LOGDEBUG, "EGL: init window system: size %dx%d", width, height);

  return getNativeWindow();
}

void CWinEGLPlatformKMS::DestroyWindowSystem(EGLNativeWindowType nativeWindow)
{
  UninitializeDisplay();
}

bool CWinEGLPlatformKMS::SetDisplayResolution(int width, int height, float refresh, bool interlace)
{
  CLog::Log(LOGDEBUG, "EGL: set display resolution %dx%d%c%dHz",
            width, height, interlace ? 'i' : 'p', (int)refresh);

  return false;
}

bool CWinEGLPlatformKMS::ClampToGUIDisplayLimits(int &width, int &height)
{
  width  = m_width;
  height = m_height;
  return true;
}

bool CWinEGLPlatformKMS::ProbeDisplayResolutions(std::vector<CStdString> &resolutions)
{
  resolutions.clear();

  if (!m_kms || m_kms->m_modeCount < 1)
    return false;

  for (unsigned int i = 0; i < m_kms->m_modeCount; i++)
  {
    drmModeModeInfo * const mi = &m_kms->m_modes[i];
    CStdString resolution;

    resolution.Format("%dx%d%c%dHz", mi->hdisplay, mi->vdisplay,
        (mi->flags & DRM_MODE_FLAG_INTERLACE) ? 'i' : 'p',
        mi->vrefresh);
    resolutions.push_back(resolution);
  }
  return true;
}

bool CWinEGLPlatformKMS::InitializeDisplay()
{
  if (m_display != EGL_NO_DISPLAY && m_config != NULL)
    return true;

  EGLNativeDisplayType nativeDisplay;
  EGLBoolean eglStatus;
  EGLint     configCount;
  EGLConfig* configList = NULL;

  nativeDisplay = getNativeDisplay();
  if (!nativeDisplay)
    return false;

  m_display = eglGetDisplay(nativeDisplay);
  if (m_display == EGL_NO_DISPLAY) 
  {
    CLog::Log(LOGERROR, "EGL: failed to obtain display");
    return false;
  }

  if (!eglInitialize(m_display, 0, 0)) 
  {
    CLog::Log(LOGERROR, "EGL: failed to initialize");
    return false;
  } 
  
  static const EGLint configAttrs[] =
  {
    EGL_RED_SIZE,        8,
    EGL_GREEN_SIZE,      8,
    EGL_BLUE_SIZE,       8,
    EGL_ALPHA_SIZE,      8,
    EGL_DEPTH_SIZE,      8,
    EGL_STENCIL_SIZE,    0,
    EGL_SAMPLE_BUFFERS,  0,
    EGL_SAMPLES,         0,
    EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_NONE
  };

  // Find out how many configurations suit our needs  
  eglStatus = eglChooseConfig(m_display, configAttrs, NULL, 0, &configCount);
  if (!eglStatus || !configCount) 
  {
    CLog::Log(LOGERROR, "EGL: failed to return any matching configurations: %d", eglStatus);
    return false;
  }

  // Allocate room for the list of matching configurations
  configList = (EGLConfig *)malloc(configCount * sizeof(EGLConfig));
  if (!configList) 
  {
    CLog::Log(LOGERROR, "EGL: failed to allocate configuration list");
    return false;
  }

  // Obtain the configuration list from EGL
  eglStatus = eglChooseConfig(m_display, configAttrs,
      configList, configCount, &configCount);
  if (!eglStatus || !configCount) 
  {
    CLog::Log(LOGERROR, "EGL: failed to populate configuration list: %d", eglStatus);
    free(configList);
    return false;
  }

  // Select an EGL configuration that matches the native window
  m_config = configList[0];

  if (m_surface != EGL_NO_SURFACE)
    ReleaseSurface();
 
  free(configList);
  return true;
}

bool CWinEGLPlatformKMS::UninitializeDisplay()
{
  EGLBoolean eglStatus;
  
  DestroyWindow();

  if (m_display != EGL_NO_DISPLAY)
  {
    eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    eglStatus = eglTerminate(m_display);
    if (!eglStatus)
      CLog::Log(LOGERROR, "Error terminating EGL");
    m_display = EGL_NO_DISPLAY;
  }

  if (m_kms)
  {
    delete m_kms;
    m_kms = NULL;
  }

  if (m_gbmDevice)
  {
    gbm_device_destroy(m_gbmDevice);
    m_gbmDevice = NULL;
  }

  if (m_deviceFd >= 0)
  {
    close(m_deviceFd);
    m_deviceFd = -1;
  }
  return true;
}

bool CWinEGLPlatformKMS::CreateWindow()
{
  if (m_display == EGL_NO_DISPLAY || m_config == NULL)
  {
    if (!InitializeDisplay())
      return false;
  }

  if (m_surface != EGL_NO_SURFACE)
    return true;

  EGLNativeWindowType nativeWindow = getNativeWindow();
  if (!nativeWindow)
    return false;

  m_surface = eglCreateWindowSurface(m_display, m_config, nativeWindow, NULL);
  if (!m_surface)
  {
    CLog::Log(LOGERROR, "EGL: failed to create window surface");
    return false;
  }
  return true;
}

bool CWinEGLPlatformKMS::DestroyWindow()
{
  EGLBoolean eglStatus;

  ReleaseSurface();

  if (m_surface == EGL_NO_SURFACE)
    return true;

  eglStatus = eglDestroySurface(m_display, m_surface);
  if (!eglStatus)
  {
    CLog::Log(LOGERROR, "EGL: failed to detroy EGL surface");
    return false;
  }

  m_surface = EGL_NO_SURFACE;
  m_width = 0;
  m_height = 0;

  return true;
}

bool CWinEGLPlatformKMS::BindSurface()
{
  EGLBoolean eglStatus;
  
  if (m_display == EGL_NO_DISPLAY || m_surface == EGL_NO_SURFACE || m_config == NULL)
  {
    CLog::Log(LOGINFO, "EGL not configured correctly. Let's try to do that now...");
    if (!CreateWindow())
    {
      CLog::Log(LOGERROR, "EGL not configured correctly to create a surface");
      return false;
    }
  }

  eglStatus = eglBindAPI(EGL_OPENGL_ES_API);
  if (!eglStatus) 
  {
    CLog::Log(LOGERROR, "EGL: failed to bind GLESv2 API: %d", eglStatus);
    return false;
  }

  // Create an EGL context
  if (m_context == EGL_NO_CONTEXT)
  {
    static const EGLint attribs[] = 
    {
      EGL_CONTEXT_CLIENT_VERSION, 2,
      EGL_NONE
    };

    m_context = eglCreateContext(m_display, m_config, EGL_NO_CONTEXT, attribs);
    if (!m_context)
    {
      CLog::Log(LOGERROR, "EGL: failed to create context");
      return false;
    }
  }

  // Make the context and surface current to this thread for rendering
  eglStatus = eglMakeCurrent(m_display, m_surface, m_surface, m_context);
  if (!eglStatus) 
  {
    CLog::Log(LOGERROR, "EGL couldn't make context/surface current: %d", eglStatus);
    return false;
  }

  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  m_eglext  = " ";
  m_eglext += eglQueryString(m_display, EGL_EXTENSIONS);
  m_eglext += " ";
  CLog::Log(LOGDEBUG, "EGL extensions:%s", m_eglext.c_str());

  CLog::Log(LOGINFO, "EGL window and context creation complete");
  return true;
}

bool CWinEGLPlatformKMS::ReleaseSurface()
{
  EGLBoolean eglStatus;
  
  if (m_context != EGL_NO_CONTEXT)
  {
    eglStatus = eglDestroyContext(m_display, m_context);
    if (!eglStatus)
      CLog::Log(LOGERROR, "Error destroying EGL context");
    m_context = EGL_NO_CONTEXT;
  }

  if (m_display != EGL_NO_DISPLAY)
    eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  return true;
}

bool CWinEGLPlatformKMS::ShowWindow(bool show)
{
  return true;
}

void CWinEGLPlatformKMS::SwapBuffers()
{
  eglSwapBuffers(m_display, m_surface);
  m_kms->FlipSurface(m_gbmSurface);
}

bool CWinEGLPlatformKMS::SetVSync(bool enable)
{
  // depending how buffers are setup, eglSwapInterval
  // might fail so let caller decide if this is an error.
  return eglSwapInterval(m_display, enable ? 1 : 0);
}

bool CWinEGLPlatformKMS::IsExtSupported(const char* extension)
{
  CStdString name;

  name  = " ";
  name += extension;
  name += " ";

  return m_eglext.find(name) != std::string::npos;
}

CStdString const & CWinEGLPlatformKMS::getDevicePath()
{
  // TODO: use libudev to determine the right device path
  return m_devicePath;
}

EGLNativeDisplayType CWinEGLPlatformKMS::getNativeDisplay()
{
  if (m_deviceFd < 0)
  {
    const char * const devicePath = getDevicePath().c_str();
    if (!devicePath || devicePath[0] == '\0')
    {
      CLog::Log(LOGERROR, "EGL: failed to obtain device path");
      return NULL;
    }

    m_deviceFd = open(devicePath, O_RDWR|O_CLOEXEC);
    if (m_deviceFd < 0)
    {
      CLog::Log(LOGERROR, "EGL: failed to open device '%s'", devicePath);
      return NULL;
    }
  }

  if (!m_gbmDevice)
  {
    m_gbmDevice = gbm_create_device(m_deviceFd);
    if (!m_gbmDevice)
    {
      CLog::Log(LOGERROR, "EGL: failed to create GBM device");
      return NULL;
    }
  }

  if (!m_kms)
  {
    m_kms = new CWinPlatformKMS(m_deviceFd);
    if (!m_kms->Reset())
      return NULL;

    // Get the current display size
    m_width  = m_kms->m_mode.hdisplay;
    m_height = m_kms->m_mode.vdisplay;
    CLog::Log(LOGDEBUG, "EGL: current display size %dx%d", m_width, m_height);
  }
  return static_cast<EGLNativeDisplayType>(m_gbmDevice);
}

EGLNativeWindowType CWinEGLPlatformKMS::getNativeWindow()
{
  if (!m_gbmSurface)
  {
    EGLNativeDisplayType nativeDisplay;

    nativeDisplay = getNativeDisplay();
    if (!nativeDisplay)
      return NULL;

    m_gbmSurface = gbm_surface_create(nativeDisplay, m_width, m_height,
        GBM_BO_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT|GBM_BO_USE_RENDERING);
    if (!m_gbmSurface)
    {
      CLog::Log(LOGERROR, "EGL: failed to create native window");
      return NULL;
    }
  }
  return static_cast<EGLNativeWindowType>(m_gbmSurface);
}

EGLDisplay CWinEGLPlatformKMS::GetEGLDisplay()
{
  return m_display;
}

EGLContext CWinEGLPlatformKMS::GetEGLContext()
{
  return m_context;
}

#endif
