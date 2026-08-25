/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2013-2016 Vincent Torri
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h> /* malloc free */
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
# define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#undef WIN32_LEAN_AND_MEAN

#include <GL/gl.h>

#include "xpost.h"
#include "xpost_log.h"
#include "xpost_memory.h"  /* save/restore works with mtabs */
#include "xpost_object.h"  /* save/restore examines objects */
#include "xpost_stack.h"  /* save/restore manipulates (internal) stacks */
#include "xpost_error.h"
#include "xpost_context.h"
#include "xpost_dict.h"
#include "xpost_string.h"
#include "xpost_name.h"

#include "xpost_operator.h"
#include "xpost_op_dict.h"
#include "xpost_dev_generic.h" /* the raster extent limit, the page's ground */
#include "xpost_dev_driver.h" /* device contract and shared helpers */
#include "xpost_dev_win32.h"

typedef enum
{
    RENDER_BACKEND_GDI,
    RENDER_BACKEND_GL,
} Render_Backend;

typedef struct
{
    HINSTANCE instance;
    HWND window;
    int width;
    int height;
} PrivateData;

/* Defined below, next to the render data it gives up. */
static void _reclaim(void *block);

typedef struct
{
   BITMAPINFOHEADER bih;
   DWORD masks[3];
} BITMAPINFO_XPOST;

typedef struct
{
    BITMAPINFO_XPOST *bitmap_info;
    HBITMAP bitmap;
    unsigned int *buf;
} Render_Data_Gdi;

typedef struct
{
    HGLRC glrc;
    unsigned int changed : 1;
} Render_Data_Gl;

typedef struct
{
    Render_Backend backend_type;
    HDC dc;
    union
    {
        Render_Data_Gdi gdi;
        Render_Data_Gl gl;
    } backend;
} Render_Data;

static unsigned int _event_handler_opcode;
static unsigned int _create_cont_opcode;

static Xpost_Object namePrivate;
static Xpost_Object namewidth;
static Xpost_Object nameheight;
static Xpost_Object namedotcopydict;

static void
_xpost_dev_gl_win32_viewport_set(int width, int height)
{
    glViewport(0, 0, width, height);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, width, 0, height, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glScalef(1, -1, 1);
    glTranslatef(0, (GLfloat)-height, 0);
}

static
int _event_handler(Xpost_Context *ctx,
                   Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;
    MSG msg;

    if (!xpost_dev_private_get(ctx, devdic, namePrivate,
                               &privatestr, &private, sizeof(private)))
        return undefined;

    while (PeekMessage(&msg, private.window, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}



static LRESULT CALLBACK
_xpost_dev_win32_procedure(HWND   window,
                           UINT   message,
                           WPARAM window_param,
                           LPARAM data_param)
{
    switch (message)
    {
        default:
            return DefWindowProc(window, message, window_param, data_param);
    }
}

/* create an instance of the device
   using the class .copydict procedure */
static
int _create(Xpost_Context *ctx,
            Xpost_Object width,
            Xpost_Object height,
            Xpost_Object classdic)
{
    return xpost_dev_create_begin(ctx, width, height, classdic,
                                  _create_cont_opcode);
}

/* initialize the C-level data
   and define in the device instance */
static
int _create_cont(Xpost_Context *ctx,
                 Xpost_Object w,
                 Xpost_Object h,
                 Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;
    Render_Data *rd;
    int width, height;
    WNDCLASSEX wc;
    RECT rect;
    HICON icon = NULL;
    HICON icon_sm = NULL;
    size_t bytes;
    int ret;

    /* The page the program asked for, as the extent of the buffer that
       will hold it. Every device here holds a whole page in one block,
       so the two carry the same numbers; a page naming an extent no
       buffer's row arithmetic carries is refused before anything is
       built for it. */
    if (!xpost_dev_page_extent(w.int_.val, h.int_.val, &width, &height))
        return limitcheck;

    /* A pixel is reached by its position within the bitmap, so a page
       whose far end has no address on this platform cannot be reached at
       all, whatever memory the system would give for it. The window
       system is told the size of the bitmap it holds through a field
       narrower than a size, which is a second thing the page has to fit
       and a limit of what this device can be given rather than of what
       it can compute. Both are held before the window is made, so
       nothing is put on screen for a page that is going to be refused. */
    {
        size_t described = (size_t)(DWORD)-1;

        if (!xpost_device_raster_bytes(width, height, sizeof(unsigned int),
                                       0, &bytes)
            || bytes > described)
        {
            XPOST_LOG_ERR("%d a raster for a page of %dx%d is larger than"
                          " this platform addresses", limitcheck,
                          width, height);
            return limitcheck;
        }
    }

    /* The block this device's instance state lives in, and, named with
       it rather than after it, what gives up whatever that state names.
       What this device holds is a window and the framebuffer behind it,
       which are not virtual memory: a device the run never retires --
       one a restore took back, or one nothing named by the time a
       collection came round -- would take them with it. This is what
       gives them up there. */
    ret = xpost_handle_cons(ctx, devdic, namePrivate, &privatestr,
                            XPOST_HANDLE_DEVICE, sizeof(PrivateData),
                            _reclaim);
    if (ret)
        return ret;

    /* create and map window */
    private.instance = GetModuleHandle(NULL);
    if (!private.instance)
        return unregistered;

    icon = LoadImage(private.instance,
                     MAKEINTRESOURCE(101),
                     IMAGE_ICON,
                     GetSystemMetrics(SM_CXICON),
                     GetSystemMetrics(SM_CYICON),
                     LR_DEFAULTCOLOR);
    if (!icon)
        icon = LoadIcon(NULL, IDI_APPLICATION);

    icon_sm = LoadImage(private.instance,
                        MAKEINTRESOURCE(101),
                        IMAGE_ICON,
                        GetSystemMetrics(SM_CXSMICON),
                        GetSystemMetrics(SM_CYSMICON),
                        LR_DEFAULTCOLOR);
    if (!icon_sm)
        icon_sm = LoadIcon(NULL, IDI_APPLICATION);

    memset (&wc, 0, sizeof (WNDCLASSEX));
    wc.cbSize = sizeof (WNDCLASSEX);
    wc.style = 0;
    wc.lpfnWndProc = _xpost_dev_win32_procedure;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = private.instance;
    wc.hIcon = icon;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(1 + COLOR_BTNFACE);
    wc.lpszMenuName =  NULL;
    wc.lpszClassName = TEXT("XPOST_DEV_WIN32");
    wc.hIconSm = icon_sm;

    if(!RegisterClassEx(&wc))
    {
        XPOST_LOG_ERR("RegisterClass() failed");
        return unregistered;
    }

    rect.left = 0;
    rect.top = 0;
    rect.right = width;
    rect.bottom = height;
    if (!AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW | WS_SIZEBOX, FALSE, 0))
    {
        XPOST_LOG_ERR("AdjustWindowRect() failed");
        goto unregister_class;
    }

    private.window = CreateWindow(TEXT("XPOST_DEV_WIN32"), TEXT(""),
                                  WS_OVERLAPPEDWINDOW | WS_SIZEBOX,
                                  0, 0,
                                  rect.right - rect.left,
                                  rect.bottom - rect.top,
                                  NULL, NULL,
                                  private.instance, NULL);

    if (!private.window)
    {
        XPOST_LOG_ERR("CreateWindowEx() failed");
        goto unregister_class;
    }

    SetWindowText(private.window, TEXT("Xpost"));

    rd = (Render_Data *)malloc(sizeof(Render_Data));
    if (!rd)
    {
        XPOST_LOG_ERR("allocation of memory failed");
        goto destroy_window;
    }

    rd->dc = GetDC(private.window);
    if (!rd->dc)
    {
        XPOST_LOG_ERR("GetDC() failed");
        goto free_rd;
    }

    if (strcmp(ctx->device_str, "gl") == 0)
    {
        PIXELFORMATDESCRIPTOR pfd;
        HGLRC glrc;
        int pixel_format;
        LONG_PTR res;

        ZeroMemory(&pfd, sizeof (pfd));
        pfd.nSize = sizeof(PIXELFORMATDESCRIPTOR);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 24;
        pfd.cDepthBits = 32;
        pfd.iLayerType = PFD_MAIN_PLANE;

        pixel_format = ChoosePixelFormat(rd->dc, &pfd);
        if (!pixel_format)
        {
            XPOST_LOG_ERR("ChoosePixelFormat() failed");
            goto release_dc;
        }

        if (!SetPixelFormat(rd->dc, pixel_format, &pfd))
        {
            XPOST_LOG_ERR("SetPixelFormat() failed");
            goto release_dc;
        }

        glrc = wglCreateContext(rd->dc);
        if (!glrc)
        {
            XPOST_LOG_ERR("wglCreateContext() failed %ld", GetLastError());
            goto release_dc;
        }

        if (!wglMakeCurrent(rd->dc, glrc))
        {
            XPOST_LOG_ERR("wglMakeCurrent() failed");
            wglDeleteContext(glrc);
            goto release_dc;
        }

        rd->backend_type = RENDER_BACKEND_GL;
        rd->backend.gl.glrc = glrc;
        rd->backend.gl.changed = 0;

        SetLastError(0);
        res = SetWindowLongPtr(private.window, GWLP_USERDATA, (LONG_PTR)rd);
        if ((res == 0) && (GetLastError() != 0))
        {
            XPOST_LOG_ERR("SetWindowLongPtr() failed %ld", GetLastError());
            wglDeleteContext(glrc);
            goto release_dc;
        }

        /* set the viewport to be a 2D rectangle of size width and height */
        _xpost_dev_gl_win32_viewport_set(width, height);
    }
    else
    {
        BITMAPINFO_XPOST *bitmap_info;
        HBITMAP bitmap;
        unsigned int *buf;
        LONG_PTR res;

        bitmap_info = (BITMAPINFO_XPOST *)malloc(sizeof(BITMAPINFO_XPOST));
        if (!bitmap_info)
        {
            XPOST_LOG_ERR("allocating bitmap info data failed");
            goto release_dc;
        }

        bitmap_info->bih.biSize = sizeof(BITMAPINFOHEADER);
        bitmap_info->bih.biWidth = width;
        bitmap_info->bih.biHeight = -height;
        bitmap_info->bih.biPlanes = 1;
        /* the bytes the raster comes to, which Create held against
           what this field expresses before the window was made */
        bitmap_info->bih.biSizeImage = (DWORD)bytes;
        bitmap_info->bih.biXPelsPerMeter = 0;
        bitmap_info->bih.biYPelsPerMeter = 0;
        bitmap_info->bih.biClrUsed = 0;
        bitmap_info->bih.biClrImportant = 0;
        bitmap_info->bih.biBitCount = 32;
        bitmap_info->bih.biCompression = BI_BITFIELDS;
        bitmap_info->masks[0] = 0x00ff0000;
        bitmap_info->masks[1] = 0x0000ff00;
        bitmap_info->masks[2] = 0x000000ff;

        bitmap = CreateDIBSection(rd->dc,
                                  (const BITMAPINFO *)bitmap_info,
                                  DIB_RGB_COLORS,
                                  (void **)(&buf),
                                  NULL,
                                  0);
        if (!bitmap)
        {
            XPOST_LOG_ERR("CreateDIBSection() failed");
            free(bitmap_info);
            goto release_dc;
        }

        rd->backend_type = RENDER_BACKEND_GDI;
        rd->backend.gdi.bitmap_info = bitmap_info;
        rd->backend.gdi.bitmap = bitmap;
        rd->backend.gdi.buf = buf;

        SetLastError(0);
        res = SetWindowLongPtr(private.window, GWLP_USERDATA, (LONG_PTR)rd);
        if ((res == 0) && (GetLastError() != 0))
        {
            XPOST_LOG_ERR("SetWindowLongPtr() failed %ld", GetLastError());
            DeleteObject(rd->backend.gdi.bitmap);
            free(bitmap_info);
            goto release_dc;
        }
    }

    private.width = width;
    private.height = height;

    ShowWindow(private.window, SW_SHOWNORMAL);
    if (!UpdateWindow(private.window))
    {
        XPOST_LOG_ERR("UpdateWindow() failed");
        goto free_rd;
    }

    xpost_context_install_event_handler(ctx,
                                        xpost_operator_cons_opcode(_event_handler_opcode),
                                        devdic);

    /* save private data struct in string */
    if (!xpost_dev_private_put(ctx, privatestr, &private, sizeof(private)))
        return VMerror;

    /* return device instance dictionary to ps */
    xpost_stack_push(ctx->lo, ctx->os, devdic);
    return 0;

  release_dc:
    ReleaseDC(private.window, rd->dc);
  free_rd:
    free(rd);
  destroy_window:
    DestroyWindow(private.window);
  unregister_class:
    /* the module handle came from GetModuleHandle, which takes no
       reference, so there is none to give back */
    UnregisterClass(TEXT("XPOST_DEV_WIN32"), private.instance);
    return unregistered;
}

static
int _putpix(Xpost_Context *ctx,
            Xpost_Object red,
            Xpost_Object green,
            Xpost_Object blue,
            Xpost_Object x,
            Xpost_Object y,
            Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;
    Render_Data *rd;
    int r, g, b, ix, iy;

    /* fold numbers per the driver contract */
    r = xpost_dev_num_to_byte(red);
    g = xpost_dev_num_to_byte(green);
    b = xpost_dev_num_to_byte(blue);
    ix = xpost_dev_pixel(xpost_object_number(x));
    iy = xpost_dev_pixel(xpost_object_number(y));

    if (!xpost_dev_private_get(ctx, devdic, namePrivate,
                               &privatestr, &private, sizeof(private)))
        return undefined;

    /* check bounds */
    if ((ix < 0) || (ix >= private.width) ||
        (iy < 0) || (iy >= private.height))
        return 0;

    rd = (Render_Data *)GetWindowLongPtr(private.window, GWLP_USERDATA);
    if (!rd)
        return 0;

    switch (rd->backend_type)
    {
        case RENDER_BACKEND_GDI:
        {
            HDC cdc;

            rd->backend.gdi.buf
                [xpost_dev_raster_offset(ix, iy, private.width)] =
                r << 16 | g << 8 | b;

            cdc = CreateCompatibleDC(rd->dc);
            SelectObject(cdc, rd->backend.gdi.bitmap);
            BitBlt(rd->dc, ix, iy, 1, 1, cdc, ix, iy, SRCCOPY);
            DeleteDC(cdc);
            break;
        }
        case RENDER_BACKEND_GL:
            glBegin(GL_POINTS);
            glColor4f(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
            glVertex2f((GLfloat)ix, (GLfloat)iy);
            glEnd();
            rd->backend.gl.changed = 1;
            break;
    }

    return 0;
}

/* Blend a coverage-weighted pixel: each channel moves toward the colour
   by cov/255 from the level the pixel already holds. The buffered
   backend reads that level out of its own buffer and writes the result
   back to the same place PutPix does; the backend that keeps no buffer
   has nothing to read and composites over the ground, which is the
   colour erasepage left and so is what such a page shows everywhere it
   has not been marked.

   The class this device specialises carries a blend that reads a raster
   held as PostScript row arrays. This device keeps no such array, and
   the driver contract names BlendPix among the slots a device with a
   raster of its own brings itself. */
static
int _blendpix(Xpost_Context *ctx,
              Xpost_Object red,
              Xpost_Object green,
              Xpost_Object blue,
              Xpost_Object cov,
              Xpost_Object x,
              Xpost_Object y,
              Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;
    Render_Data *rd;
    int r, g, b, c, ix, iy;
    int dr, dg, db;

    /* fold numbers per the driver contract */
    r = xpost_dev_num_to_byte(red);
    g = xpost_dev_num_to_byte(green);
    b = xpost_dev_num_to_byte(blue);
    c = xpost_dev_num_to_int(cov);
    ix = xpost_dev_pixel(xpost_object_number(x));
    iy = xpost_dev_pixel(xpost_object_number(y));

    if (!xpost_dev_private_get(ctx, devdic, namePrivate,
                               &privatestr, &private, sizeof(private)))
        return undefined;

    /* check bounds */
    if ((ix < 0) || (ix >= private.width) ||
        (iy < 0) || (iy >= private.height))
        return 0;

    if (c <= 0)
        return 0;
    if (c > 255)
        c = 255;

    /* what the pixel holds where the backend keeps nothing to read it
       from; the buffered one replaces this with what its buffer holds */
    xpost_device_ground_channels(ctx, devdic, &dr, &dg, &db);

    rd = (Render_Data *)GetWindowLongPtr(private.window, GWLP_USERDATA);
    if (!rd)
        return 0;

    switch (rd->backend_type)
    {
        case RENDER_BACKEND_GDI:
        {
            HDC cdc;
            unsigned int pix = rd->backend.gdi.buf
                [xpost_dev_raster_offset(ix, iy, private.width)];

            dr = (pix >> 16) & 0xFF;
            dg = (pix >> 8) & 0xFF;
            db = pix & 0xFF;

            rd->backend.gdi.buf
                [xpost_dev_raster_offset(ix, iy, private.width)] =
                xpost_dev_blend_channel(dr, r, c) << 16 |
                xpost_dev_blend_channel(dg, g, c) << 8 |
                xpost_dev_blend_channel(db, b, c);

            cdc = CreateCompatibleDC(rd->dc);
            SelectObject(cdc, rd->backend.gdi.bitmap);
            BitBlt(rd->dc, ix, iy, 1, 1, cdc, ix, iy, SRCCOPY);
            DeleteDC(cdc);
            break;
        }
        case RENDER_BACKEND_GL:
            glBegin(GL_POINTS);
            glColor4f(xpost_dev_blend_channel(dr, r, c) / 255.0f,
                      xpost_dev_blend_channel(dg, g, c) / 255.0f,
                      xpost_dev_blend_channel(db, b, c) / 255.0f, 1.0f);
            glVertex2f((GLfloat)ix, (GLfloat)iy);
            glEnd();
            rd->backend.gl.changed = 1;
            break;
    }

    return 0;
}

/* Read a pixel back in the device's stored channel scale, the same one
   PutPix writes. A pixel outside the raster, or a backend that keeps no
   buffer of its own, reads as the ground -- the colour erasepage left,
   which is what the page shows wherever this device holds no pixel to
   answer from: the slot declares three results and must answer three. */
static
int _getpix(Xpost_Context *ctx,
            Xpost_Object x,
            Xpost_Object y,
            Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;
    Render_Data *rd;
    int ix, iy, r, g, b;

    ix = xpost_dev_pixel(xpost_object_number(x));
    iy = xpost_dev_pixel(xpost_object_number(y));

    if (!xpost_dev_private_get(ctx, devdic, namePrivate,
                               &privatestr, &private, sizeof(private)))
        return undefined;

    xpost_device_ground_channels(ctx, devdic, &r, &g, &b);

    rd = (Render_Data *)GetWindowLongPtr(private.window, GWLP_USERDATA);
    if (rd && rd->backend_type == RENDER_BACKEND_GDI &&
        ix >= 0 && ix < private.width && iy >= 0 && iy < private.height)
    {
        unsigned int pix = rd->backend.gdi.buf
            [xpost_dev_raster_offset(ix, iy, private.width)];

        r = (pix >> 16) & 0xFF;
        g = (pix >> 8) & 0xFF;
        b = pix & 0xFF;
    }

    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(r));
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(g));
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(b));

    return 0;
}

static
int _drawline(Xpost_Context *ctx,
              Xpost_Object red,
              Xpost_Object green,
              Xpost_Object blue,
              Xpost_Object x1,
              Xpost_Object y1,
              Xpost_Object x2,
              Xpost_Object y2,
              Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;
    Render_Data *rd;
    Xpost_Dev_Line line;
    int r, g, b, px, py;
    int bx0, by0, bx1, by1, any;

    /* fold numbers per the driver contract */
    r = xpost_dev_num_to_byte(red);
    g = xpost_dev_num_to_byte(green);
    b = xpost_dev_num_to_byte(blue);

    if (!xpost_dev_private_get(ctx, devdic, namePrivate,
                               &privatestr, &private, sizeof(private)))
        return undefined;

    rd = (Render_Data *)GetWindowLongPtr(private.window, GWLP_USERDATA);
    if (!rd)
        return 0;

    switch (rd->backend_type)
    {
        case RENDER_BACKEND_GDI:
        {
            HDC cdc;

            /* the contract's line: the pixels whose centres the segment
               covers, so this device paints the same wire as every
               other. A Bresenham walk of its own painted a different
               set, and dropped the pixel a segment too short to reach a
               centre still owes. */
            any = 0;
            bx0 = by0 = bx1 = by1 = 0;
            xpost_dev_line_init(&line,
                                xpost_object_number(x1), xpost_object_number(y1),
                                xpost_object_number(x2), xpost_object_number(y2));
            while (xpost_dev_line_next(&line, &px, &py))
            {
                if (px < 0 || px >= private.width ||
                    py < 0 || py >= private.height)
                    continue;
                rd->backend.gdi.buf
                    [xpost_dev_raster_offset(px, py, private.width)] =
                    r << 16 | g << 8 | b;
                if (!any)
                {
                    bx0 = bx1 = px;
                    by0 = by1 = py;
                    any = 1;
                }
                else
                {
                    if (px < bx0) bx0 = px;
                    if (px > bx1) bx1 = px;
                    if (py < by0) by0 = py;
                    if (py > by1) by1 = py;
                }
            }
            if (!any)
                return 0;

            cdc = CreateCompatibleDC(rd->dc);
            SelectObject(cdc, rd->backend.gdi.bitmap);
            BitBlt(rd->dc, bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1,
                   cdc, bx0, by0, SRCCOPY);
            DeleteDC(cdc);
            break;
        }
        case RENDER_BACKEND_GL:
            glBegin(GL_LINES);
            glColor4f(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
            glVertex2f((GLfloat)xpost_object_number(x1),
                       (GLfloat)xpost_object_number(y1));
            glVertex2f((GLfloat)xpost_object_number(x2),
                       (GLfloat)xpost_object_number(y2));
            glEnd();
            rd->backend.gl.changed = 1;
            break;
    }

    return 0;
}

static
int _fillrect(Xpost_Context *ctx,
              Xpost_Object red,
              Xpost_Object green,
              Xpost_Object blue,
              Xpost_Object x,
              Xpost_Object y,
              Xpost_Object width,
              Xpost_Object height,
              Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;
    Render_Data *rd;
    int r, g, b;
    int x0, y0, x1, y1;

    /* fold numbers per the driver contract */
    r = xpost_dev_num_to_byte(red);
    g = xpost_dev_num_to_byte(green);
    b = xpost_dev_num_to_byte(blue);

    if (!xpost_dev_private_get(ctx, devdic, namePrivate,
                               &privatestr, &private, sizeof(private)))
        return undefined;

    /* the contract's rectangle: inclusive span, clipped to the device.
       Clamping the origin to zero without shrinking the extent, as this
       device did, slides the rectangle back onto the page instead of
       cutting off the part that hangs over the edge. */
    xpost_dev_rect_normalize(xpost_object_number(x), xpost_object_number(y),
                             xpost_object_number(width),
                             xpost_object_number(height),
                             &x0, &y0, &x1, &y1);
    if (!xpost_dev_rect_clip(&x0, &y0, &x1, &y1,
                             private.width, private.height))
        return 0;

    rd = (Render_Data *)GetWindowLongPtr(private.window, GWLP_USERDATA);
    if (!rd)
        return 0;

    switch (rd->backend_type)
    {
        case RENDER_BACKEND_GDI:
        {
            HDC cdc;
            int i;
            int j;

            for (i = y0; i <= y1; i++)
            {
                for (j = x0; j <= x1; j++)
                {
                    rd->backend.gdi.buf
                        [xpost_dev_raster_offset(j, i, private.width)] =
                        r << 16 | g << 8 | b;
                }
            }

            cdc = CreateCompatibleDC(rd->dc);
            SelectObject(cdc, rd->backend.gdi.bitmap);
            BitBlt(rd->dc, x0, y0, x1 - x0 + 1, y1 - y0 + 1,
                   cdc, x0, y0, SRCCOPY);
            DeleteDC(cdc);
            break;
        }
        case RENDER_BACKEND_GL:
            /* the quad covers the pixels x0..x1 and y0..y1, so its far
               corner is one past the last painted pixel */
            glBegin(GL_QUADS);
            glColor4f(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
            glVertex2f((GLfloat)x0, (GLfloat)y0);
            glVertex2f((GLfloat)(x1 + 1), (GLfloat)y0);
            glVertex2f((GLfloat)(x1 + 1), (GLfloat)(y1 + 1));
            glVertex2f((GLfloat)x0, (GLfloat)(y1 + 1));
            glEnd();
            rd->backend.gl.changed = 1;
            break;
    }

    return 0;
}

static
int _flush(Xpost_Context *ctx,
           Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;
    Render_Data *rd;

    if (!xpost_dev_private_get(ctx, devdic, namePrivate,
                               &privatestr, &private, sizeof(private)))
        return undefined;

    rd = (Render_Data *)GetWindowLongPtr(private.window, GWLP_USERDATA);
    if (!rd)
        return 0;

    switch (rd->backend_type)
    {
        case RENDER_BACKEND_GDI:
            UpdateWindow(private.window);
            break;
        case RENDER_BACKEND_GL:
            if (rd->backend.gl.changed)
            {
                wglMakeCurrent(rd->dc, rd->backend.gl.glrc);
                SwapBuffers(rd->dc);
                rd->backend.gl.changed = 0;
            }
            break;
    }

    return 0;
}

static
int _destroy(Xpost_Context *ctx,
             Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;

    if (!xpost_dev_private_get(ctx, devdic, namePrivate,
                               &privatestr, &private, sizeof(private)))
        return undefined;

    /* the window handle is this device's resource handle: cleared below
       once the window is gone, so a repeated Destroy is a no-op rather
       than a teardown of state that has already been released */
    if (!private.window)
        return 0;

    /* The handler names one device, and by now that may be a
       device made after this one: replacing a page device installs
       the new device before it retires the old, so clearing the
       handler unasked would leave the live window hearing nothing.
       A device gives up only what it was given. */
    if (xpost_dict_compare_objects(ctx, ctx->window_device, devdic) == 0)
        xpost_context_install_event_handler(ctx, null, null);

    /* the same release the collector runs, so what this device owns is
       stated once */
    _reclaim(&private);

    /* store the cleared handles back, so the second and third Destroy
       the interpreter makes find nothing left to release */
    if (!xpost_dev_private_put(ctx, privatestr, &private, sizeof(private)))
        return VMerror;

    return 0;
}

/* Give up the window the instance names and the render data the window
   carries. Called from the collector with the block the instance state
   is kept in, so it touches nothing in virtual memory -- including the
   context's record of which device the window is, which names a device
   the collector has not reached. A device the run retired has given the
   window up already and leaves this nothing to do. */
static void _reclaim(void *block)
{
    PrivateData *p = block;
    Render_Data *rd;

    if (!p->window)
        return;

    rd = (Render_Data *)GetWindowLongPtr(p->window, GWLP_USERDATA);
    if (rd)
    {
        /* the window stops naming the render data before the render
           data goes, so the window procedure cannot reach it while the
           window is being torn down */
        SetWindowLongPtr(p->window, GWLP_USERDATA, (LONG_PTR)0);

        switch (rd->backend_type)
        {
            case RENDER_BACKEND_GDI:
                /* the framebuffer belongs to the bitmap the device
                   created, and goes with it */
                DeleteObject(rd->backend.gdi.bitmap);
                free(rd->backend.gdi.bitmap_info);
                break;
            case RENDER_BACKEND_GL:
                wglMakeCurrent(NULL, NULL);
                wglDeleteContext(rd->backend.gl.glrc);
                break;
        }

        ReleaseDC(p->window, rd->dc);
        free(rd);
    }

    DestroyWindow(p->window);
    p->window = NULL;

    if (!UnregisterClass(TEXT("XPOST_DEV_WIN32"), p->instance))
        XPOST_LOG_INFO("UnregisterClass() failed");
    p->instance = NULL;
}

/* operator function to instantiate a new window device.
   installed in the private dictionary by calling 'loadXXXdevice'.
*/
static
int newwin32device(Xpost_Context *ctx,
                   Xpost_Object width,
                   Xpost_Object height)
{
    Xpost_Object classdic;
    int ret;

    xpost_stack_push(ctx->lo, ctx->os, width);
    xpost_stack_push(ctx->lo, ctx->os, height);

    /* note:
       an invalid name should cause an undefined error to propagate
       with extra handling here */

    ret = xpost_op_privatedict_load(ctx, xpost_name_cons(ctx, ".xpost_WIN32DEVICE"));
    if (ret)
        return ret;
    classdic = xpost_stack_topdown_fetch(ctx->lo, ctx->os, 0);

    /* xpost_stack_push will also throw an error upon an invalid object
       return from xpost_dict_get */
    if (!xpost_stack_push(ctx->lo, ctx->es,
                          xpost_dict_get(ctx, classdic,
                                         xpost_name_cons(ctx, "Create"))))
        return execstackoverflow;

    return 0;
}

static
unsigned int _loadwin32devicecont_opcode;

/* Specializes or sub-classes the .xpost_PPMIMAGE device class.
   load .xpost_PPMIMAGE
   load and call ps procedure .copydict which leaves copy on stack
   call loadXXXdevicecont by continuation.
*/
static
int loadwin32device(Xpost_Context *ctx)
{
    Xpost_Object classdic;
    int ret;

    /* see note in newwin32device above */
    ret = xpost_op_privatedict_load(ctx, xpost_name_cons(ctx, ".xpost_PPMIMAGE"));
    if (ret)
        return ret;
    classdic = xpost_stack_topdown_fetch(ctx->lo, ctx->os, 0);
    if (!xpost_stack_push(ctx->lo, ctx->es,
                          xpost_operator_cons_opcode(_loadwin32devicecont_opcode)))
        return execstackoverflow;
    if (!xpost_stack_push(ctx->lo, ctx->es,
                          xpost_dict_get(ctx, classdic, namedotcopydict)))
        return execstackoverflow;

    return 0;
}

/* replace procedures in the class with newly created special operators.
   defines the device class XXXDEVICE in the private dictionary.
   defines its maker beside it: newXXXdevice
*/
static
int loadwin32devicecont(Xpost_Context *ctx,
                        Xpost_Object classdic)
{
    /* this device's method suite; the arities follow from its
       declared colour space */
    static const Xpost_Dev_Method methods[] =
    {
        { "Create", "win32Create", (Xpost_Op_Func)_create, XPOST_DEV_M_CREATE },
        { "PutPix", "win32PutPix", (Xpost_Op_Func)_putpix, XPOST_DEV_M_PUTPIX },
        { "GetPix", "win32GetPix", (Xpost_Op_Func)_getpix, XPOST_DEV_M_GETPIX },
        { "BlendPix", "win32BlendPix", (Xpost_Op_Func)_blendpix, XPOST_DEV_M_BLEND },
        { "DrawLine", "win32DrawLine", (Xpost_Op_Func)_drawline, XPOST_DEV_M_LINE },
        { "FillRect", "win32FillRect", (Xpost_Op_Func)_fillrect, XPOST_DEV_M_RECT },
        /* showing the page and flushing it are the same act on a window;
           Flush is named separately because the raster operators call it
           when it is there, to keep a preview moving */
        { "Emit", "win32Emit", (Xpost_Op_Func)_flush, XPOST_DEV_M_PAGE },
        { "Flush", "win32Flush", (Xpost_Op_Func)_flush, XPOST_DEV_M_PAGE },
        { "Destroy", "win32Destroy", (Xpost_Op_Func)_destroy, XPOST_DEV_M_PAGE }
    };

    Xpost_Object op;
    int ret;

    ret = xpost_dict_put(ctx, classdic, xpost_name_cons(ctx, "nativecolorspace"), xpost_name_cons(ctx, "DeviceRGB"));
    if (ret)
        return ret;

    /* This device's page does not arrive a band at a time. Its pixels go
       to a window, which holds them, so a band could be sent as it was
       finished (doc/INTERNALS) -- but this driver keeps a buffer of the
       page and writes the whole of it, and what a device states about
       itself is what the machinery above it goes by.

       Taken back out rather than left unsaid. The class is a copy of the
       colour raster class, which says its page may arrive that way, and
       a copy carries what it was copied from -- so a device that has not
       considered the question says yes by inheritance, and the safe
       answer is the one that has to be stated. */
    ret = xpost_dict_undef(ctx, classdic, xpost_name_cons(ctx, "BandedPage"));
    if (ret && ret != undefined)
        return ret;

    /* And this class states nothing about what a row of its raster
       costs, because it has no one answer to state: which rendering
       backend a window is made with is settled when the device is made,
       and one of them draws through the graphics library with no
       buffer of its own here at all. What a row costs is then a
       property of the instance and not of the class, so what the colour
       raster class this one is a copy of states about its own
       three-byte planar row is taken back out rather than answered on
       behalf of a raster of some other shape. */
    ret = xpost_dev_class_no_rowcost(ctx, classdic);
    if (ret)
        return ret;

    op = xpost_operator_cons(ctx, "win32CreateCont", (Xpost_Op_Func)_create_cont, 3, integertype, integertype, dicttype);
    _create_cont_opcode = op.mark_.padw;

    ret = xpost_dev_class_install(ctx, classdic, 3, 1,
                                  methods, XPOST_DEV_METHOD_COUNT(methods));
    if (ret)
        return ret;



    /* Paint glyphs without blending their edges. This lands on the class,
       which is where a device's features are declared, and Create chooses
       the backend afterwards, so the one value serves both of them and
       the backend with the most to lose by it decides what it is.

       That is the backend which keeps no buffer: it reads a pixel back as
       the page's ground, so a partly covered edge falling over a mark
       already laid is pulled toward the ground rather than toward the ink
       beneath it. The buffered backend blends against what the window
       actually holds and has no such edge, so for it this is cost alone:
       blending reaches every pixel an edge partly covers and not only the
       ones the aliased path fills, and each of those is a device context
       made, a bitmap selected into it, a one-pixel blit, and the context
       destroyed.

       Declaring one bit of text alpha takes the aliased path, which
       paints through PutPix above. */
    ret = xpost_dict_put(ctx, classdic, xpost_name_cons(ctx, "TextAlphaBits"),
                         xpost_int_cons(1));
    if (ret)
        return ret;







    /* The class and its maker live in the private dictionary, beside the
       classes the boot files define: a program reaches a device through
       the page-device request, and the machinery reaches the class by
       name here. Nothing of the driver's is defined where a program
       could shadow it. */
    ret = xpost_dict_put(ctx, ctx->privatedict,
                         xpost_name_cons(ctx, ".xpost_WIN32DEVICE"), classdic);
    if (ret)
        return ret;

    op = xpost_operator_cons(ctx, "newwin32device", (Xpost_Op_Func)newwin32device, 2,
                             integertype, integertype);
    ret = xpost_dict_put(ctx, ctx->privatedict, xpost_name_cons(ctx, "newwin32device"), op);
    if (ret)
        return ret;

    op = xpost_operator_cons(ctx, "win32EventHandler", (Xpost_Op_Func)_event_handler, 1, dicttype);
    _event_handler_opcode = op.mark_.padw;

    return 0;
}

/*
   install the loadXXXdevice which may be called during graphics initialization
   to produce the operator newXXXdevice
   which creates the device instance dictionary.
*/
int xpost_oper_init_win32_device_ops(Xpost_Context *ctx,
                                     Xpost_Object sd)
{
    Xpost_Operator *optab;
    Xpost_Object n,op;

    if (xpost_object_get_type((namePrivate = xpost_name_cons(ctx, "Private"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namewidth = xpost_name_cons(ctx, "width"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((nameheight = xpost_name_cons(ctx, "height"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namedotcopydict = xpost_name_cons(ctx, ".copydict"))) == invalidtype)
        return VMerror;

    optab = xpost_operator_table(ctx->gl);
    op = xpost_operator_cons(ctx, "loadwin32device", (Xpost_Op_Func)loadwin32device, 0); INSTALL;
    op = xpost_operator_cons(ctx, "loadwin32devicecont", (Xpost_Op_Func)loadwin32devicecont, 1, dicttype);
    _loadwin32devicecont_opcode = op.mark_.padw;

    return 0;
}
