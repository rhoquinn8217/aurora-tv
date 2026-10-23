/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <stddef.h>

#define CERTIFICATE_FILE_NAME "client.pem"
#define KEY_FILE_NAME "key.pem"

typedef struct HTTP_T HTTP;

typedef struct _HTTP_DATA {
    char *memory;
    size_t size;
} HTTP_DATA;

HTTP *http_create(const char *keydir);

int http_request(HTTP *http, char *url, HTTP_DATA * data);

int http_post_json(HTTP *http, char *url, const char *payload, HTTP_DATA *data);

void http_destroy(HTTP *http);

void http_set_timeout(HTTP *http, int timeout);

void http_set_total_timeout(HTTP *http, int timeout);

/* This fork: the request in progress gives up within a second, and every one
 * after it at once. Safe from any thread while `http` exists. */
void http_abort(HTTP *http);

HTTP_DATA * http_data_alloc();

void http_data_free(HTTP_DATA * data);
