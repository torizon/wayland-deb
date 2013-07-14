/*
 * Copyright © 2008 Kristian Høgsberg
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 *
 * Authors:
 *    Kristian Høgsberg <krh@bitplanet.net>
 *    Benjamin Franzke <benjaminfranzke@googlemail.com>
 *
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "wayland-private.h"
#include "wayland-server.h"

struct wl_shm_pool {
	struct wl_resource *resource;
	int refcount;
	char *data;
	int size;
};

struct wl_shm_buffer {
	struct wl_resource *resource;
	int32_t width, height;
	int32_t stride;
	uint32_t format;
	int offset;
	struct wl_shm_pool *pool;
};

static void
shm_pool_unref(struct wl_shm_pool *pool)
{
	pool->refcount--;
	if (pool->refcount)
		return;

	munmap(pool->data, pool->size);
	free(pool);
}

static void
destroy_buffer(struct wl_resource *resource)
{
	struct wl_shm_buffer *buffer = wl_resource_get_user_data(resource);

	if (buffer->pool)
		shm_pool_unref(buffer->pool);
	free(buffer);
}

static void
shm_buffer_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct wl_buffer_interface shm_buffer_interface = {
	shm_buffer_destroy
};

static void
shm_pool_create_buffer(struct wl_client *client, struct wl_resource *resource,
		       uint32_t id, int32_t offset,
		       int32_t width, int32_t height,
		       int32_t stride, uint32_t format)
{
	struct wl_shm_pool *pool = wl_resource_get_user_data(resource);
	struct wl_shm_buffer *buffer;

	switch (format) {
	case WL_SHM_FORMAT_ARGB8888:
	case WL_SHM_FORMAT_XRGB8888:
		break;
	default:
		wl_resource_post_error(resource,
				       WL_SHM_ERROR_INVALID_FORMAT,
				       "invalid format");
		return;
	}

	if (offset < 0 || width <= 0 || height <= 0 || stride < width ||
	    INT32_MAX / stride <= height ||
	    offset > pool->size - stride * height) {
		wl_resource_post_error(resource,
				       WL_SHM_ERROR_INVALID_STRIDE,
				       "invalid width, height or stride (%dx%d, %u)",
				       width, height, stride);
		return;
	}

	buffer = malloc(sizeof *buffer);
	if (buffer == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	buffer->width = width;
	buffer->height = height;
	buffer->format = format;
	buffer->stride = stride;
	buffer->offset = offset;
	buffer->pool = pool;
	pool->refcount++;

	buffer->resource =
		wl_resource_create(client, &wl_buffer_interface, 1, id);
	if (buffer->resource == NULL) {
		wl_client_post_no_memory(client);
		shm_pool_unref(pool);
		free(buffer);
		return;
	}

	wl_resource_set_implementation(buffer->resource,
				       &shm_buffer_interface,
				       buffer, destroy_buffer);
}

static void
destroy_pool(struct wl_resource *resource)
{
	struct wl_shm_pool *pool = wl_resource_get_user_data(resource);

	shm_pool_unref(pool);
}

static void
shm_pool_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
shm_pool_resize(struct wl_client *client, struct wl_resource *resource,
		int32_t size)
{
	struct wl_shm_pool *pool = wl_resource_get_user_data(resource);
	void *data;

	data = mremap(pool->data, pool->size, size, MREMAP_MAYMOVE);

	if (data == MAP_FAILED) {
		wl_resource_post_error(resource,
				       WL_SHM_ERROR_INVALID_FD,
				       "failed mremap");
		return;
	}

	pool->data = data;
	pool->size = size;
}

struct wl_shm_pool_interface shm_pool_interface = {
	shm_pool_create_buffer,
	shm_pool_destroy,
	shm_pool_resize
};

static void
shm_create_pool(struct wl_client *client, struct wl_resource *resource,
		uint32_t id, int fd, int32_t size)
{
	struct wl_shm_pool *pool;

	pool = malloc(sizeof *pool);
	if (pool == NULL) {
		wl_client_post_no_memory(client);
		goto err_close;
	}

	if (size <= 0) {
		wl_resource_post_error(resource,
				       WL_SHM_ERROR_INVALID_STRIDE,
				       "invalid size (%d)", size);
		goto err_free;
	}

	pool->refcount = 1;
	pool->size = size;
	pool->data = mmap(NULL, size,
			  PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pool->data == MAP_FAILED) {
		wl_resource_post_error(resource,
				       WL_SHM_ERROR_INVALID_FD,
				       "failed mmap fd %d", fd);
		goto err_free;
	}
	close(fd);

	pool->resource =
		wl_resource_create(client, &wl_shm_pool_interface, 1, id);
	if (!pool->resource) {
		wl_client_post_no_memory(client);
		munmap(pool->data, pool->size);
		free(pool);
		return;
	}

	wl_resource_set_implementation(pool->resource,
				       &shm_pool_interface,
				       pool, destroy_pool);

	return;

err_close:
	close(fd);
err_free:
	free(pool);
}

static const struct wl_shm_interface shm_interface = {
	shm_create_pool
};

static void
bind_shm(struct wl_client *client,
	 void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource;

	resource = wl_resource_create(client, &wl_shm_interface, 1, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(resource, &shm_interface, data, NULL);

	wl_shm_send_format(resource, WL_SHM_FORMAT_ARGB8888);
	wl_shm_send_format(resource, WL_SHM_FORMAT_XRGB8888);
}

WL_EXPORT int
wl_display_init_shm(struct wl_display *display)
{
	if (!wl_global_create(display, &wl_shm_interface, 1, NULL, bind_shm))
		return -1;

	return 0;
}

WL_EXPORT struct wl_shm_buffer *
wl_shm_buffer_create(struct wl_client *client,
		     uint32_t id, int32_t width, int32_t height,
		     int32_t stride, uint32_t format)
{
	struct wl_shm_buffer *buffer;
			     
	switch (format) {
	case WL_SHM_FORMAT_ARGB8888:
	case WL_SHM_FORMAT_XRGB8888:
		break;
	default:
		return NULL;
	}

	buffer = malloc(sizeof *buffer + stride * height);
	if (buffer == NULL)
		return NULL;

	buffer->width = width;
	buffer->height = height;
	buffer->format = format;
	buffer->stride = stride;
	buffer->offset = 0;
	buffer->pool = NULL;

	buffer->resource =
		wl_resource_create(client, &wl_buffer_interface, 1, id);
	if (buffer->resource == NULL) {
		free(buffer);
		return NULL;
	}

	wl_resource_set_implementation(buffer->resource,
				       &shm_buffer_interface,
				       buffer, destroy_buffer);

	return buffer;
}

WL_EXPORT struct wl_shm_buffer *
wl_shm_buffer_get(struct wl_resource *resource)
{
	if (resource == NULL)
		return NULL;

	if (wl_resource_instance_of(resource, &wl_buffer_interface,
				    &shm_buffer_interface))
		return wl_resource_get_user_data(resource);
	else
		return NULL;
}

WL_EXPORT int32_t
wl_shm_buffer_get_stride(struct wl_shm_buffer *buffer)
{
	return buffer->stride;
}

WL_EXPORT void *
wl_shm_buffer_get_data(struct wl_shm_buffer *buffer)
{
	if (buffer->pool)
		return buffer->pool->data + buffer->offset;
	else
		return buffer + 1;
}

WL_EXPORT uint32_t
wl_shm_buffer_get_format(struct wl_shm_buffer *buffer)
{
	return buffer->format;
}

WL_EXPORT int32_t
wl_shm_buffer_get_width(struct wl_shm_buffer *buffer)
{
	return buffer->width;
}

WL_EXPORT int32_t
wl_shm_buffer_get_height(struct wl_shm_buffer *buffer)
{
	return buffer->height;
}
