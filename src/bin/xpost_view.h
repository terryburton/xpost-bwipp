/*
 * Xpost View - a small PostScript Level-3 viewer
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2013-2016 Vincent Torri
 * Copyright (c) 2026 Terry Burton
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_view.h
 * @brief Declares what a viewer back end must provide.
 *
 * One of the platform files beside this implements it.
 */

#ifndef XPOST_VIEW_H
#define XPOST_VIEW_H

typedef struct _Xpost_View_Window Xpost_View_Window;

Xpost_View_Window *xpost_view_win_new(int xorig, int yorig, int width, int height);

void xpost_view_win_del(Xpost_View_Window *win);

void xpost_view_page_change(int i);

void xpost_view_page_display(Xpost_View_Window *win,
                             const void *buffer);

void xpost_view_main_loop(const Xpost_View_Window *win);

#endif
