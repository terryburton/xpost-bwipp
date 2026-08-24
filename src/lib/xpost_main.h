/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2013 Vincent Torri
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef XPOST_MAIN_H
#define XPOST_MAIN_H

/**
 * @file xpost_main.h
 * @brief Initializing and quitting functions
 */

/**
 * @brief Ask to be called when the library goes down.
 *
 * The library's lifetime is counted: the quit that balances the last
 * init takes it down, and a process may then start another lifetime. A
 * module holding something for that lifetime -- a cache, an open handle,
 * a table it allocated -- has to give it back at that point, or the next
 * lifetime is handed what the last one freed.
 *
 * A module says so by registering here, at the moment it takes the thing
 * it will have to give back. Acquiring and releasing are then written
 * together, and there is no separate list for a new module to be left
 * out of. Registering the same function again is not an error and does
 * not register it twice, so the call may sit on the acquisition path and
 * run as often as that path does.
 *
 * The registered functions run in the reverse of the order they
 * registered in, so a module reaches what it was built on before that
 * goes; the list is emptied as they run, and a later lifetime registers
 * afresh. Coming up later means going down sooner, so a module is torn
 * down before whatever it was built on: the findfont cache gives its
 * faces back while the font library that owns them is still there to
 * take them, because it registered after that library did.
 *
 * This is the whole of the teardown -- xpost_quit calls what is
 * registered and nothing else, so a module is taken down because it
 * asked to be and not because someone remembered it. An init that gives
 * up partway runs the same list, so what had come up before the refusal
 * does not stay up.
 *
 * A module registers itself, on the path where it takes what it will
 * give back. Two cannot: compat and the log are shared with
 * libxpost_dsc, which has no lifetime of its own to register against, so
 * xpost_init registers those two where it brings them up.
 *
 * @param fn The function to call. Ignored when NULL.
 * @return 1 when it is registered or was already, 0 when the table is
 *         full -- which is also reported on the error log.
 */
int xpost_at_quit(void (*fn)(void));

#endif
