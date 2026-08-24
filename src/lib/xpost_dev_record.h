/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef XPOST_DEV_RECORD_H
#define XPOST_DEV_RECORD_H

/**
 * @file xpost_dev_record.h
 * @brief The device that writes a page down instead of painting it.
 *
 * Installs loadrecorddevice in systemdict, which when run creates
 *
 *       width height  newrecorddevice  device
 *
 * and the replay operator the recorded page is painted through.
 */
int xpost_oper_init_record_device_ops(Xpost_Context *ctx,
                                      Xpost_Object sd);

/**
 * @brief Hand a device a glyph's coverage, answering which mask it is.
 *
 * @param[in] devdic the device, which must be one declaring .recordglyph
 * @param[in] cov @p w x @p h coverage bytes, a row at a time
 * @param[out] at which of that device's masks the coverage became
 * @return 0, or the error to raise
 *
 * The bulk half of a glyph entry. A glyph is written down as a coverage
 * mask and a placement of it, and the two are handed over separately
 * because they happen at two moments: the coverage exists once the
 * glyph is rendered, and the placement is a mark, which belongs in the
 * record at the point the page is painted. So the coverage comes here
 * as it is rendered and the placement follows through .recordglyph,
 * naming the answer this gave.
 *
 * The coverage is copied and the caller keeps its buffer. It is handed
 * over as bytes rather than as a string because nothing about it is a
 * PostScript value: it is built by the glyph painter, read by the
 * record, and would otherwise cost the job a string per glyph on the
 * page to carry between the two.
 */
int xpost_dev_record_takemask(Xpost_Context *ctx, Xpost_Object devdic,
                              const unsigned char *cov, int w, int h,
                              size_t *at);

#endif
