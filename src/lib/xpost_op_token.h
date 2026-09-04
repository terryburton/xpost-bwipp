/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2026 Terry Burton
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_op_token.h
 * @brief Declares the one function that installs the token operator.
 *
 * The operators themselves are in the .c beside this. Nothing here is
 * called by anything but the operator table's own set-up, which asks
 * each module in turn to install what it owns.
 */

#ifndef XPOST_OP_TOKEN_H
#define XPOST_OP_TOKEN_H

int xpost_oper_init_token_ops(Xpost_Context *ctx, Xpost_Object sd);

/**
 * @brief scan one token from the head of the string @p S, leaving the
 * rest of the string, the token, and true on the operand stack, or just
 * false where nothing remains to read.
 *
 * Carries no access rule. The token operator reads a string for the
 * program and asks for read access first; the interpreter executing a
 * string asks instead whether it may execute it, which PLRM 3.3.2
 * settles differently. Sharing the scan keeps one rule from standing in
 * for the other.
 */
int xpost_token_string_scan(Xpost_Context *ctx, Xpost_Object S);

/**
 * @brief decode one number of a binary token or encoded number string
 * (PLRM 3.14.4/3.14.5): @p rep selects representation (0..31 32-bit
 * fixed point scaled by rep, 32..47 16-bit fixed point scaled by
 * rep-32, 48 IEEE real, 49 native-order real; +128 = low-order byte
 * first), @p p the encoded bytes. 0 or the error to raise.
 */
int xpost_scanner_rep_number(unsigned int rep, const unsigned char *p, Xpost_Object *retval);

/**
 * @brief read a radix number (PLRM 3.2) from the head of @p s: a decimal
 * base of 2 through 36, '#', then one or more digits ranging from 0 to
 * base-1, with A through Z (or a through z) standing for 10 upwards.
 *
 * @p end is left at the first character that is not one of the base's
 * digits, past the whole of the numeral whether or not it fits.
 *
 * 0 with the number in @p out, limitcheck for a number past the
 * integer's field, or -1 for text that is not a radix number at all --
 * which PLRM 3.2 makes a name rather than an error.
 */
int xpost_scanner_radix_number(const char *s, const char **end, integer *out);

#endif
