/*
* Author: Christian Huitema
* Copyright (c) 2017, Private Octopus, Inc.
* All rights reserved.
*
* Permission to use, copy, modify, and distribute this software for any
* purpose with or without fee is hereby granted, provided that the above
* copyright notice and this permission notice appear in all copies.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL Private Octopus, Inc. BE LIABLE FOR ANY
* DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/* Common code used by the server and the client implementations.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <picotls.h>
#include "picosplay.h"
#include "picoquic.h"
#include "picoquic_utils.h"
#include "tls_api.h"
#include "h3zero.h"
#include "h3zero_common.h"



 /* Stream context splay management */

static int64_t picohttp_stream_node_compare(void *l, void *r)
{
	/* Stream values are from 0 to 2^62-1, which means we are not worried with rollover */
	return ((picohttp_server_stream_ctx_t*)l)->stream_id - ((picohttp_server_stream_ctx_t*)r)->stream_id;
}

static picosplay_node_t * picohttp_stream_node_create(void * value)
{
	return &((picohttp_server_stream_ctx_t *)value)->http_stream_node;
}

void * picohttp_stream_node_value(picosplay_node_t * node)
{
	return (void*)((char*)node - offsetof(struct st_picohttp_server_stream_ctx_t, http_stream_node));
}

static void picohttp_clear_stream_ctx(picohttp_server_stream_ctx_t* stream_ctx)
{
	if (stream_ctx->file_path != NULL) {
		free(stream_ctx->file_path);
		stream_ctx->file_path = NULL;
	}
	if (stream_ctx->F != NULL) {
		stream_ctx->F = picoquic_file_close(stream_ctx->F);
	}

	if (stream_ctx->path_callback != NULL) {
		(void)stream_ctx->path_callback(NULL, NULL, 0, picohttp_callback_free, stream_ctx, stream_ctx->path_callback_ctx);
	}

	if (stream_ctx->is_h3) {
		h3zero_delete_data_stream_state(&stream_ctx->ps.stream_state);
	}
	else {
		if (stream_ctx->ps.hq.path != NULL) {
			free(stream_ctx->ps.hq.path);
		}
	}
}

static void picohttp_stream_node_delete(void * tree, picosplay_node_t * node)
{
	picohttp_server_stream_ctx_t * stream_ctx = picohttp_stream_node_value(node);

	picohttp_clear_stream_ctx(stream_ctx);

	free(stream_ctx);
}

void h3zero_delete_stream(picosplay_tree_t * http_stream_tree, picohttp_server_stream_ctx_t* stream_ctx)
{
	picosplay_delete(http_stream_tree, &stream_ctx->http_stream_node);
}

picohttp_server_stream_ctx_t* h3zero_find_stream(picosplay_tree_t * stream_tree, uint64_t stream_id)
{
	picohttp_server_stream_ctx_t * ret = NULL;
	picohttp_server_stream_ctx_t target;
	target.stream_id = stream_id;
	picosplay_node_t * node = picosplay_find(stream_tree, (void*)&target);

	if (node != NULL) {
		ret = (picohttp_server_stream_ctx_t *)picohttp_stream_node_value(node);
	}

	return ret;
}

picohttp_server_stream_ctx_t * h3zero_find_or_create_stream(
	picoquic_cnx_t* cnx,
	uint64_t stream_id,
	picosplay_tree_t * stream_tree,
	int should_create,
	int is_h3)
{
	picohttp_server_stream_ctx_t * stream_ctx = h3zero_find_stream(stream_tree, stream_id);

	/* if stream is already present, check its state. New bytes? */

	if (stream_ctx == NULL && should_create) {
		stream_ctx = (picohttp_server_stream_ctx_t*)
			malloc(sizeof(picohttp_server_stream_ctx_t));
		if (stream_ctx == NULL) {
			/* Could not handle this stream */
			picoquic_reset_stream(cnx, stream_id, H3ZERO_INTERNAL_ERROR);
		}
		else {
			memset(stream_ctx, 0, sizeof(picohttp_server_stream_ctx_t));
			stream_ctx->stream_id = stream_id;
			stream_ctx->control_stream_id = UINT64_MAX;
			stream_ctx->is_h3 = is_h3;
			if (!IS_BIDIR_STREAM_ID(stream_id)) {
				if (IS_LOCAL_STREAM_ID(stream_id, picoquic_is_client(cnx))) {
					stream_ctx->ps.stream_state.is_fin_received = 1;
				}
				else {
					stream_ctx->ps.stream_state.is_fin_sent = 1;
				}
			}
			
			picosplay_insert(stream_tree, stream_ctx);
		}
	}

	return stream_ctx;
}

void h3zero_init_stream_tree(picosplay_tree_t * h3_stream_tree)
{
	picosplay_init_tree(h3_stream_tree, picohttp_stream_node_compare, picohttp_stream_node_create, picohttp_stream_node_delete, picohttp_stream_node_value);
}


/* Declare a stream prefix, such as used by webtransport or masque
 */

h3zero_stream_prefix_t* h3zero_find_stream_prefix(h3zero_stream_prefixes_t* prefixes, uint64_t prefix)
{
	h3zero_stream_prefix_t* prefix_ctx = prefixes->first;

	while (prefix_ctx != NULL) {
		if (prefix_ctx->prefix == prefix) {
			break;
		}
		prefix_ctx = prefix_ctx->next;
	}

	return prefix_ctx;
}

int h3zero_declare_stream_prefix(h3zero_stream_prefixes_t* prefixes, uint64_t prefix, picohttp_post_data_cb_fn function_call, void* function_ctx)
{
	int ret = 0;
	h3zero_stream_prefix_t* prefix_ctx = h3zero_find_stream_prefix(prefixes, prefix);

	if (prefix_ctx == NULL) {
		prefix_ctx = (h3zero_stream_prefix_t*)malloc(sizeof(h3zero_stream_prefix_t));
		if (prefix_ctx == NULL) {
			ret = -1;
		}
		else {
			memset(prefix_ctx, 0, sizeof(h3zero_stream_prefix_t));
			prefix_ctx->prefix = prefix;
			prefix_ctx->function_call = function_call;
			prefix_ctx->function_ctx = function_ctx;
			if (prefixes->last == NULL) {
				prefixes->first = prefix_ctx;
			}
			else {
				prefixes->last->next = prefix_ctx;
			}
			prefix_ctx->previous = prefixes->last;
			prefixes->last = prefix_ctx;
		}
	}
	else {
		ret = -1;
	}
	return ret;
}

void h3zero_delete_stream_prefix(h3zero_stream_prefixes_t* prefixes, uint64_t prefix)
{
	h3zero_stream_prefix_t* prefix_ctx = h3zero_find_stream_prefix(prefixes, prefix);
	if (prefix_ctx != NULL) {
		if (prefix_ctx->previous == NULL) {
			prefixes->first = prefix_ctx->next;
		}
		else {
			prefix_ctx->previous->next = prefix_ctx->next;
		}
		if (prefix_ctx->next == NULL) {
			prefixes->last = prefix_ctx->previous;
		}
		else {
			prefix_ctx->next->previous = prefix_ctx->previous;
		}
		free(prefix_ctx);
	}
}

void h3zero_delete_all_stream_prefixes(picoquic_cnx_t * cnx, h3zero_stream_prefixes_t* prefixes)
{
	h3zero_stream_prefix_t* next;

	while ((next = prefixes->first) != NULL) {
		/* Request the app to clean up its memory */
		if (next->function_call != NULL) {
			(void)next->function_call(cnx, NULL, 0, picohttp_callback_free,
				NULL, next->function_ctx);
		}
		if (prefixes->first == next){
			/* the prefix was not deleted as part of app cleanup */
			h3zero_delete_stream_prefix(prefixes, next->prefix);
		}
	}
}

uint64_t h3zero_parse_stream_prefix(uint8_t* buffer_8, size_t* nb_in_buffer, uint8_t* data, size_t data_length, size_t * nb_read)
{
	uint64_t prefix = UINT64_MAX;

	*nb_read = 0;
	while (*nb_read < data_length) {
		size_t v_len = (*nb_in_buffer > 0)?VARINT_LEN_T(buffer_8, size_t):8;
		if (*nb_in_buffer < v_len) {
			buffer_8[*nb_in_buffer] = data[*nb_read];
			*nb_read += 1;
			*nb_in_buffer += 1;
		}
		if (*nb_in_buffer >= v_len) {
			(void)picoquic_frames_uint64_decode(buffer_8, buffer_8 + 8, &prefix);
			break;
		}
	}

	return prefix;
}

int h3zero_protocol_init(picoquic_cnx_t* cnx)
{
	uint8_t decoder_stream_head = 0x03;
	uint8_t encoder_stream_head = 0x02;
	uint64_t settings_stream_id = picoquic_get_next_local_stream_id(cnx, 1);
	int ret = picoquic_add_to_stream(cnx, settings_stream_id, h3zero_default_setting_frame, h3zero_default_setting_frame_size, 0);

	if (ret == 0) {
		/* set the settings stream the first stream to write! */
		ret = picoquic_set_stream_priority(cnx, settings_stream_id, 0);
	}

	if (ret == 0) {
		uint64_t encoder_stream_id = picoquic_get_next_local_stream_id(cnx, 1);
		/* set the encoder stream, although we do not actually create dynamic codes. */
		ret = picoquic_add_to_stream(cnx, encoder_stream_id, &encoder_stream_head, 1, 0);
		if (ret == 0) {
			ret = picoquic_set_stream_priority(cnx, encoder_stream_id, 1);
		}
	}

	if (ret == 0) {
		uint64_t decoder_stream_id = picoquic_get_next_local_stream_id(cnx, 1);
		/* set the the decoder stream, although we do not actually create dynamic codes. */
		ret = picoquic_add_to_stream(cnx, decoder_stream_id, &decoder_stream_head, 1, 0);
		if (ret == 0) {
			ret = picoquic_set_stream_priority(cnx, decoder_stream_id, 1);
		}
	}
	return ret;
}

/* Parse the first bytes of an unidir stream, and determine what to do with that stream.
 */
uint8_t* h3zero_parse_incoming_remote_stream(
	uint8_t* bytes, uint8_t* bytes_max,
	picohttp_server_stream_ctx_t* stream_ctx,
	picosplay_tree_t* stream_tree, h3zero_stream_prefixes_t* prefixes)
{
	h3zero_data_stream_state_t* stream_state = &stream_ctx->ps.stream_state;
	size_t frame_type_length = 0;

	if (!stream_state->frame_header_parsed) {
		if (stream_state->frame_header_read < 1) {
			stream_state->frame_header[stream_state->frame_header_read++] = *bytes++;
		}
		frame_type_length = VARINT_LEN_T(stream_state->frame_header, size_t);
		while (stream_state->frame_header_read < frame_type_length && bytes < bytes_max) {
			stream_state->frame_header[stream_state->frame_header_read++] = *bytes++;
		}
		if (stream_state->frame_header_read >= frame_type_length) {
			int is_wt_context_id_required = 0;

			(void)picoquic_frames_varint_decode(stream_state->frame_header, stream_state->frame_header + frame_type_length,
				&stream_state->current_frame_type);

			if (IS_BIDIR_STREAM_ID(stream_ctx->stream_id)) {
				switch (stream_state->current_frame_type) {
				case h3zero_frame_webtransport_stream:
					is_wt_context_id_required = 1;
					break;
				default:
					bytes = NULL;
					break;
				}
			}
			else {
				switch (stream_state->current_frame_type) {
				case h3zero_stream_type_control: /* used to send/receive setting frame and other control frames. Ignored for now. */
					break;
				case h3zero_stream_type_push: /* Push type not supported in h3zero settings */
					bytes = NULL;
					break;
				case h3zero_stream_type_qpack_encoder: /* not required since not using dynamic table */
					break;
				case h3zero_stream_type_qpack_decoder: /* not required since not using dynamic table */
					break;
				case h3zero_stream_type_webtransport: /* unidir stream is used as specified in web transport */
					is_wt_context_id_required = 1;
					break;
				default:
					bytes = NULL;
					break;
				}
			}

			if (bytes != NULL) {
				if (!is_wt_context_id_required) {
					stream_state->frame_header_parsed = 1;
				}
				else {
					size_t context_id_length = 1;
					while (stream_state->frame_header_read < frame_type_length + 1 && bytes < bytes_max) {
						stream_state->frame_header[stream_state->frame_header_read++] = *bytes++;
					}
					context_id_length = VARINT_LEN_T((stream_state->frame_header + frame_type_length), size_t);
					while (stream_state->frame_header_read < frame_type_length + context_id_length && bytes < bytes_max) {
						stream_state->frame_header[stream_state->frame_header_read++] = *bytes++;
					}
					if (stream_state->frame_header_read >= frame_type_length + context_id_length) {
						h3zero_stream_prefix_t* stream_prefix;

						(void)picoquic_frames_varint_decode(stream_state->frame_header + frame_type_length,
							stream_state->frame_header + frame_type_length + context_id_length, &stream_ctx->control_stream_id);
						stream_prefix = h3zero_find_stream_prefix(prefixes, stream_ctx->control_stream_id);
						if (stream_prefix == NULL) {
							bytes = NULL;
						}
						else {
							stream_ctx->path_callback = stream_prefix->function_call;
							stream_ctx->path_callback_ctx = stream_prefix->function_ctx;
						}
						stream_state->frame_header_parsed = 1;
					}
				}
			}
		}
	}
	return bytes;
}

/*
* HTTP 3.0 common call back.
*/

/*
* Create and delete server side connection context
*/

h3zero_callback_ctx_t* h3zero_callback_create_context(picohttp_server_parameters_t* param)
{
	h3zero_callback_ctx_t* ctx = (h3zero_callback_ctx_t*)
		malloc(sizeof(h3zero_callback_ctx_t));

	if (ctx != NULL) {
		memset(ctx, 0, sizeof(h3zero_callback_ctx_t));

		h3zero_init_stream_tree(&ctx->h3_stream_tree);

		if (param != NULL) {
			ctx->path_table = param->path_table;
			ctx->path_table_nb = param->path_table_nb;
			ctx->web_folder = param->web_folder;
		}
	}

	return ctx;
}

void h3zero_callback_delete_context(picoquic_cnx_t* cnx, h3zero_callback_ctx_t* ctx)
{
	h3zero_delete_all_stream_prefixes(cnx, &ctx->stream_prefixes);
	picosplay_empty_tree(&ctx->h3_stream_tree);
	free(ctx);
}

/* There are some streams, like unidir or server initiated bidir, that
* require extra processing, such as tying to web transport
* application.
*/

int h3zero_process_remote_stream(picoquic_cnx_t* cnx,
	uint64_t stream_id, uint8_t* bytes, size_t length,
	picoquic_call_back_event_t event,
	picohttp_server_stream_ctx_t* stream_ctx,
	h3zero_callback_ctx_t* ctx)
{
	int ret = 0;

	if (stream_ctx == NULL) {
		stream_ctx = h3zero_find_or_create_stream(cnx, stream_id, &ctx->h3_stream_tree, 1, 1);
		picoquic_set_app_stream_ctx(cnx, stream_id, stream_ctx);
	}
	if (stream_ctx == NULL) {
		ret = -1;
	}
	else {
		uint8_t* bytes_max = bytes + length;

		bytes = h3zero_parse_incoming_remote_stream(bytes, bytes_max, stream_ctx,
			&ctx->h3_stream_tree, &ctx->stream_prefixes);
		if (bytes == NULL) {
			picoquic_log_app_message(cnx, "Cannot parse incoming stream: %"PRIu64, stream_id);
			ret = -1;
		}
		else if (stream_ctx->path_callback != NULL){
			if (bytes < bytes_max) {
				stream_ctx->path_callback(cnx, bytes, bytes_max - bytes, picohttp_callback_post_data, stream_ctx, stream_ctx->path_callback_ctx);
			}
			if (event == picoquic_callback_stream_fin) {
				/* FIN of the control stream is FIN of the whole session */
				stream_ctx->path_callback(cnx, NULL, 0, picohttp_callback_post_fin, stream_ctx, stream_ctx->path_callback_ctx);
			}
		}
	}
	return ret;
}

char const * h3zero_server_default_page = "\
<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">\r\n<HTML>\r\n<HEAD>\r\n<TITLE>\
Picoquic HTTP 3 service\
</TITLE>\r\n</HEAD><BODY>\r\n\
<h1>Simple HTTP 3 Responder</h1>\r\n\
<p>GET / or GET /index.html returns this text</p>\r\n\
<p>Get /NNNNN returns txt document of length NNNNN bytes(decimal)</p>\r\n\
<p>Any other command will result in an error, and an empty response.</p>\r\n\
<h1>Enjoy!</h1>\r\n\
</BODY></HTML>\r\n";

char const * h3zero_server_post_response_page = "\
<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">\r\n<HTML>\r\n<HEAD>\r\n<TITLE>\
Picoquic POST Response\
</TITLE>\r\n</HEAD><BODY>\r\n\
<h1>POST successful</h1>\r\n\
<p>Received %d bytes.\r\n\
</BODY></HTML>\r\n";

int h3zero_server_parse_path(const uint8_t* path, size_t path_length, uint64_t* echo_size,
	char** file_path, char const* web_folder, int* file_error);

int h3zero_find_path_item(const uint8_t * path, size_t path_length, const picohttp_server_path_item_t * path_table, size_t path_table_nb)
{
	size_t i = 0;

	while (i < path_table_nb) {
		if (path_length >= path_table[i].path_length && memcmp(path, path_table[i].path, path_table[i].path_length) == 0){
			return (int)i;
		}
		i++;
	}
	return -1;
}


/* Processing of the request frame.
* This function is called after the client's stream is closed,
* after verifying that a request was received */

int h3zero_process_request_frame(
	picoquic_cnx_t* cnx,
	picohttp_server_stream_ctx_t * stream_ctx,
	h3zero_callback_ctx_t * app_ctx)
{
	/* Prepare response header */
	uint8_t buffer[1024];
	uint8_t post_response[512];
	uint8_t * o_bytes = &buffer[0];
	uint8_t * o_bytes_max = o_bytes + sizeof(buffer);
	uint64_t response_length = 0;
	int ret = 0;
	int file_error = 0;
	int do_not_close = 0;

	*o_bytes++ = h3zero_frame_header;
	o_bytes += 2; /* reserve two bytes for frame length */

	if (stream_ctx->ps.stream_state.header.method == h3zero_method_get) {
		/* Manage GET */
		if (h3zero_server_parse_path(stream_ctx->ps.stream_state.header.path, stream_ctx->ps.stream_state.header.path_length,
			&stream_ctx->echo_length, &stream_ctx->file_path, app_ctx->web_folder, &file_error) != 0) {
			char log_text[256];
			picoquic_log_app_message(cnx, "Cannot find file for path: <%s> in folder <%s>, error: 0x%x",
				picoquic_uint8_to_str(log_text, 256, stream_ctx->ps.stream_state.header.path, stream_ctx->ps.stream_state.header.path_length),
				(app_ctx->web_folder == NULL) ? "NULL" : app_ctx->web_folder, file_error);
			/* If unknown, 404 */
			o_bytes = h3zero_create_not_found_header_frame(o_bytes, o_bytes_max);
			/* TODO: consider known-url?data construct */
		}
		else {
			response_length = (stream_ctx->echo_length == 0) ?
				strlen(h3zero_server_default_page) : stream_ctx->echo_length;
			o_bytes = h3zero_create_response_header_frame(o_bytes, o_bytes_max,
				(stream_ctx->echo_length == 0) ? h3zero_content_type_text_html :
				h3zero_content_type_text_plain);
		}
	}
	else if (stream_ctx->ps.stream_state.header.method == h3zero_method_post) {
		/* Manage Post. */
		if (stream_ctx->path_callback == NULL && stream_ctx->post_received == 0) {
			int path_item = h3zero_find_path_item(stream_ctx->ps.stream_state.header.path, stream_ctx->ps.stream_state.header.path_length, app_ctx->path_table, app_ctx->path_table_nb);
			if (path_item >= 0) {
				/* TODO-POST: move this code to post-fin callback.*/
				stream_ctx->path_callback = app_ctx->path_table[path_item].path_callback;
				stream_ctx->path_callback(cnx, (uint8_t*)stream_ctx->ps.stream_state.header.path, stream_ctx->ps.stream_state.header.path_length, picohttp_callback_post,
					stream_ctx, stream_ctx->path_callback_ctx);
			}
		}

		if (stream_ctx->path_callback != NULL) {
			response_length = stream_ctx->path_callback(cnx, post_response, sizeof(post_response), picohttp_callback_post_fin, stream_ctx, stream_ctx->path_callback_ctx);
		}
		else {
			/* Prepare generic POST response */
			size_t message_length = 0;
			(void)picoquic_sprintf((char*)post_response, sizeof(post_response), &message_length, h3zero_server_post_response_page, (int)stream_ctx->post_received);
			response_length = message_length;
		}

		/* If known, create response header frame */
		/* POST-TODO: provide content type of response as part of context */
		o_bytes = h3zero_create_response_header_frame(o_bytes, o_bytes_max,
			(stream_ctx->echo_length == 0) ? h3zero_content_type_text_html :
			h3zero_content_type_text_plain);
	}
	else if (stream_ctx->ps.stream_state.header.method == h3zero_method_connect) {
		/* The connect handling depends on the requested protocol */

		if (stream_ctx->path_callback == NULL) {
			int path_item = h3zero_find_path_item(stream_ctx->ps.stream_state.header.path, stream_ctx->ps.stream_state.header.path_length, app_ctx->path_table, app_ctx->path_table_nb);
			if (path_item >= 0) {
				stream_ctx->path_callback = app_ctx->path_table[path_item].path_callback;
				if (stream_ctx->path_callback(cnx, (uint8_t*)stream_ctx->ps.stream_state.header.path, stream_ctx->ps.stream_state.header.path_length, picohttp_callback_connect,
					stream_ctx, app_ctx->path_table[path_item].path_app_ctx) != 0) {
					/* This callback is not supported */
					picoquic_log_app_message(cnx, "Unsupported callback on stream: %"PRIu64 ", path:%s", stream_ctx->stream_id, app_ctx->path_table[path_item].path);
					o_bytes = h3zero_create_error_frame(o_bytes, o_bytes_max, "501", H3ZERO_USER_AGENT_STRING);
				}
				else {
					/* Create a connect accept frame */
					picoquic_log_app_message(cnx, "Connect accepted on stream: %"PRIu64 ", path:%s", stream_ctx->stream_id, app_ctx->path_table[path_item].path);
					o_bytes = h3zero_create_response_header_frame(o_bytes, o_bytes_max, h3zero_content_type_none);
					do_not_close = 1;
				}
			}
			else {
				/* No such connect path */
				char log_text[256];
				picoquic_log_app_message(cnx, "cannot find path context on stream: %"PRIu64 ", path:%s", stream_ctx->stream_id,
					picoquic_uint8_to_str(log_text, 256, stream_ctx->ps.hq.path, stream_ctx->ps.hq.path_length));
				o_bytes = h3zero_create_not_found_header_frame(o_bytes, o_bytes_max);
			}
		}
		else {
			/* Duplicate request? Bytes after connect? Should they just be sent to the app? */
			picoquic_log_app_message(cnx, "Duplicate request on stream: %"PRIu64, stream_ctx->stream_id);
			ret = -1;
		}
	}
	else
	{
		/* unsupported method */
		picoquic_log_app_message(cnx, "Unsupported method on stream: %"PRIu64, stream_ctx->stream_id);
		o_bytes = h3zero_create_error_frame(o_bytes, o_bytes_max, "501", H3ZERO_USER_AGENT_STRING);
	}

	if (o_bytes == NULL) {
		picoquic_log_app_message(cnx, "Error, resetting stream: %"PRIu64, stream_ctx->stream_id);
		ret = picoquic_reset_stream(cnx, stream_ctx->stream_id, H3ZERO_INTERNAL_ERROR);
	}
	else {
		size_t header_length = o_bytes - &buffer[3];
		int is_fin_stream = (stream_ctx->echo_length == 0) ? (1 - do_not_close) : 0;
		buffer[1] = (uint8_t)((header_length >> 8) | 0x40);
		buffer[2] = (uint8_t)(header_length & 0xFF);

		if (response_length > 0) {
			size_t ld = 0;

			if (o_bytes + 2 < o_bytes_max) {
				*o_bytes++ = h3zero_frame_data;
				ld = picoquic_varint_encode(o_bytes, o_bytes_max - o_bytes, response_length);
			}

			if (ld == 0) {
				o_bytes = NULL;
			}
			else {
				o_bytes += ld; 

				if (stream_ctx->echo_length == 0) {
					if (response_length <= sizeof(post_response)) {
						if (o_bytes + (size_t)response_length <= o_bytes_max) {
							memcpy(o_bytes, (stream_ctx->ps.stream_state.header.method == h3zero_method_post) ? post_response : (uint8_t*)h3zero_server_default_page, (size_t)response_length);
							o_bytes += (size_t)response_length;
						}
						else {
							o_bytes = NULL;
						}
					}
					else {
						/* Large post responses are not concatenated here, but will be pulled from the data */
						is_fin_stream = 0;
					}
				}
			}
		}

		if (o_bytes != NULL) {
			if (is_fin_stream && stream_ctx->ps.stream_state.header.method == h3zero_method_connect) {
				picoquic_log_app_message(cnx, "Setting FIN in connect response on stream: %"PRIu64, stream_ctx->stream_id);
			}
			ret = picoquic_add_to_stream_with_ctx(cnx, stream_ctx->stream_id,
				buffer, o_bytes - buffer, is_fin_stream, stream_ctx);
			if (ret != 0) {
				o_bytes = NULL;
			}
		}

		if (o_bytes == NULL) {
			ret = picoquic_reset_stream(cnx, stream_ctx->stream_id, H3ZERO_INTERNAL_ERROR);
		}
		else if (stream_ctx->echo_length != 0 || response_length > sizeof(post_response)) {
			ret = picoquic_mark_active_stream(cnx, stream_ctx->stream_id, 1, stream_ctx);
		}
	}

	return ret;
}


int h3zero_callback_server_data(
	picoquic_cnx_t* cnx, picohttp_server_stream_ctx_t * stream_ctx,
	uint64_t stream_id, uint8_t* bytes, size_t length,
	picoquic_call_back_event_t fin_or_event,
	h3zero_callback_ctx_t* ctx)
{
	int ret = 0;

	/* Find whether this is bidir or unidir stream */
	if (IS_BIDIR_STREAM_ID(stream_id)) {
		/* If client bidir stream, absorb data until end, then
		* parse the header */
		/* TODO: add an exception for bidir streams set by Webtransport */
		if (!IS_CLIENT_STREAM_ID(stream_id)) {
			/* This is the client writing back on a server created stream.
			* Call to selected callback, or ignore */
			if (stream_ctx->path_callback != NULL){
				if (length > 0) {
					ret = stream_ctx->path_callback(cnx, bytes, length, picohttp_callback_post_data, stream_ctx, stream_ctx->path_callback_ctx);
				}
				if (fin_or_event == picoquic_callback_stream_fin) {
					/* FIN of the control stream is FIN of the whole session */
					ret = stream_ctx->path_callback(cnx, NULL, 0, picohttp_callback_post_fin, stream_ctx, stream_ctx->path_callback_ctx);
				}
			}
		}
		else {
			/* Find or create stream context */
			if (stream_ctx == NULL) {
				stream_ctx = h3zero_find_or_create_stream(cnx, stream_id, &ctx->h3_stream_tree, 1, 1);
			}

			if (stream_ctx == NULL) {
				ret = picoquic_stop_sending(cnx, stream_id, H3ZERO_INTERNAL_ERROR);

				if (ret == 0) {
					ret = picoquic_reset_stream(cnx, stream_id, H3ZERO_INTERNAL_ERROR);
				}
			}
			else {
				/* TODO: move this to common code with unidir, after parsing beginning of unidir? */
				uint16_t error_found = 0;
				size_t available_data = 0;
				uint8_t * bytes_max = bytes + length;
				while (bytes < bytes_max) {
					bytes = h3zero_parse_data_stream(bytes, bytes_max, &stream_ctx->ps.stream_state, &available_data, &error_found);
					if (bytes == NULL) {
						ret = picoquic_close(cnx, error_found);
						break;
					}
					else if (available_data > 0) {
						if (stream_ctx->ps.stream_state.is_web_transport) {
							if (stream_ctx->path_callback == NULL) {
								h3zero_stream_prefix_t* stream_prefix;
								stream_prefix = h3zero_find_stream_prefix(&ctx->stream_prefixes, stream_ctx->ps.stream_state.control_stream_id);
								if (stream_prefix == NULL) {
									ret = picoquic_reset_stream(cnx, stream_id, H3ZERO_WEBTRANSPORT_BUFFERED_STREAM_REJECTED);
								}
								else {
									stream_ctx->path_callback = stream_prefix->function_call;
									stream_ctx->path_callback_ctx = stream_prefix->function_ctx;
									(void)picoquic_set_app_stream_ctx(cnx, stream_id, stream_ctx);
								}
							}

						} else if (stream_ctx->ps.stream_state.header_found && stream_ctx->post_received == 0) {
							int path_item = h3zero_find_path_item(stream_ctx->ps.stream_state.header.path, stream_ctx->ps.stream_state.header.path_length, ctx->path_table, ctx->path_table_nb);
							if (path_item >= 0) {
								stream_ctx->path_callback = ctx->path_table[path_item].path_callback;
								stream_ctx->path_callback(cnx, (uint8_t*)stream_ctx->ps.stream_state.header.path, stream_ctx->ps.stream_state.header.path_length, picohttp_callback_post,
									stream_ctx, ctx->path_table[path_item].path_app_ctx);
							}
							(void)picoquic_set_app_stream_ctx(cnx, stream_id, stream_ctx);
						}

						/* Received data for a POST command. */
						if (stream_ctx->path_callback != NULL) {
							/* if known URL, pass the data to URL specific callback. */
							ret = stream_ctx->path_callback(cnx, bytes, available_data, picohttp_callback_post_data, stream_ctx, stream_ctx->path_callback_ctx);
						}
						stream_ctx->post_received += available_data;
						bytes += available_data;
					}
				}
				/* Process the header if necessary */
				if (ret == 0) {
					if (stream_ctx->ps.stream_state.is_web_transport) {
						if (fin_or_event == picoquic_callback_stream_fin && stream_ctx->path_callback != NULL) {
							ret = stream_ctx->path_callback(cnx, NULL, 0, picohttp_callback_post_fin, stream_ctx, stream_ctx->path_callback_ctx);
						}
					} else {
						if (fin_or_event == picoquic_callback_stream_fin || stream_ctx->ps.stream_state.header.method == h3zero_method_connect) {
							/* Process the request header. */
							if (stream_ctx->ps.stream_state.header_found) {
								ret = h3zero_process_request_frame(cnx, stream_ctx, ctx);
							}
							else {
								/* Unexpected end of stream before the header is received */
								ret = picoquic_reset_stream(cnx, stream_id, H3ZERO_FRAME_ERROR);
							}
						}
					}
				}
			}
		}
	}
	else {
		/* process the unidir streams. */
		ret = h3zero_process_remote_stream(cnx, stream_id, bytes, length,
			fin_or_event, stream_ctx, ctx);
	}

	return ret;
}


int h3zero_client_open_stream_file(picoquic_cnx_t* cnx, h3zero_callback_ctx_t* ctx, picohttp_server_stream_ctx_t* stream_ctx)
{
	int ret = 0;

	if (!stream_ctx->is_file_open && ctx->no_disk == 0) {
		int last_err = 0;
		stream_ctx->F = picoquic_file_open_ex(stream_ctx->f_name, "wb", &last_err);
		if (stream_ctx->F == NULL) {
			picoquic_log_app_message(cnx,
				"Could not open file <%s> for stream %" PRIu64 ", error %d (0x%x)\n", stream_ctx->f_name, stream_ctx->stream_id, last_err, last_err);
			DBG_PRINTF("Could not open file <%s> for stream %" PRIu64 ", error %d (0x%x)", stream_ctx->f_name, stream_ctx->stream_id, last_err, last_err);
			ret = -1;
		}
		else {
			stream_ctx->is_file_open = 1;
			ctx->nb_open_files++;
		}
	}

	return ret;
}


int h3zero_client_close_stream(picoquic_cnx_t * cnx,
	h3zero_callback_ctx_t* ctx, picohttp_server_stream_ctx_t* stream_ctx)
{
	int ret = 0;
	if (stream_ctx != NULL && stream_ctx->is_open) {
		picoquic_unlink_app_stream_ctx(cnx, stream_ctx->stream_id);
		if (stream_ctx->f_name != NULL) {
			free(stream_ctx->f_name);
			stream_ctx->f_name = NULL;
		}
		stream_ctx->F = picoquic_file_close(stream_ctx->F);
		if (stream_ctx->is_file_open) {
			ctx->nb_open_files--;
			stream_ctx->is_file_open = 0;
		}
		stream_ctx->is_open = 0;
		ctx->nb_open_streams--; 
		ret = 1;
	}
	return ret;
}


int h3zero_callback_client_data(picoquic_cnx_t* cnx,
	uint64_t stream_id, uint8_t* bytes, size_t length,
	picoquic_call_back_event_t fin_or_event, h3zero_callback_ctx_t* ctx, 
	picohttp_server_stream_ctx_t* stream_ctx, uint64_t * fin_stream_id)
{
	int ret = 0;

	/* Data arrival on stream #x, maybe with fin mark */
	if (stream_ctx == NULL) {
		stream_ctx = h3zero_find_stream(&ctx->h3_stream_tree, stream_id);
	}
	if (IS_BIDIR_STREAM_ID(stream_id) && IS_LOCAL_STREAM_ID(stream_id, 1)) {
		if (stream_ctx == NULL) {
			fprintf(stdout, "unexpected data on local stream context: %" PRIu64 ".\n", stream_id);
			ret = -1;
		}
		else if (stream_ctx->is_open) {
			if (!stream_ctx->is_file_open && ctx->no_disk == 0 && stream_ctx->file_path != NULL) {
				ret = h3zero_client_open_stream_file(cnx, ctx, stream_ctx);
			}
			if (ret == 0 && length > 0) {
				uint16_t error_found = 0;
				size_t available_data = 0;
				uint8_t* bytes_max = bytes + length;
				while (bytes < bytes_max) {
					bytes = h3zero_parse_data_stream(bytes, bytes_max, &stream_ctx->ps.stream_state, &available_data, &error_found);
					if (bytes == NULL) {
						ret = picoquic_close(cnx, error_found);
						if (ret != 0) {
							picoquic_log_app_message(cnx,
								"Could not parse incoming data from stream %" PRIu64 ", error 0x%x", stream_id, error_found);
						}
						break;
					}
					else if (available_data > 0) {
						if (!stream_ctx->flow_opened) {
							if (stream_ctx->ps.stream_state.current_frame_length < 0x100000) {
								stream_ctx->flow_opened = 1;
							}
							else if (cnx->cnx_state == picoquic_state_ready) {
								stream_ctx->flow_opened = 1;
								ret = picoquic_open_flow_control(cnx, stream_id, stream_ctx->ps.stream_state.current_frame_length);
							}
						}
						if (ret == 0 && ctx->no_disk == 0) {
							ret = (fwrite(bytes, 1, available_data, stream_ctx->F) > 0) ? 0 : -1;
							if (ret != 0) {
								picoquic_log_app_message(cnx,
									"Could not write data from stream %" PRIu64 ", error 0x%x", stream_id, ret);
							}
						}
						stream_ctx->received_length += available_data;
						bytes += available_data;
					}
				}
			}

			if (fin_or_event == picoquic_callback_stream_fin) {
				if (stream_ctx->path_callback != NULL) {
					stream_ctx->path_callback(cnx, NULL, 0, picohttp_callback_post_fin, stream_ctx, stream_ctx->path_callback_ctx);
				}
				else {
					if (h3zero_client_close_stream(cnx, ctx, stream_ctx)) {
						*fin_stream_id = stream_id;
						if (stream_id <= 64 && !ctx->no_print) {
							fprintf(stdout, "Stream %" PRIu64 " ended after %" PRIu64 " bytes\n",
								stream_id, stream_ctx->received_length);
						}
						if (stream_ctx->received_length == 0) {
							picoquic_log_app_message(cnx, "Stream %" PRIu64 " ended after %" PRIu64 " bytes, ret=0x%x",
								stream_id, stream_ctx->received_length, ret);
						}
					}
				}
			}
		}
		else if (stream_ctx->path_callback != NULL) {
			stream_ctx->path_callback(cnx, bytes, length, picohttp_callback_post_data, stream_ctx, stream_ctx->path_callback_ctx);
			if (fin_or_event == picoquic_callback_stream_fin) {
				/* FIN of the control stream is FIN of the whole session */
				stream_ctx->path_callback(cnx, NULL, 0, picohttp_callback_post_fin, stream_ctx, stream_ctx->path_callback_ctx);
			}
		}
	}
	else {
		ret = h3zero_process_remote_stream(cnx, stream_id, bytes, length,
			fin_or_event, stream_ctx, ctx);
	}

	return ret;
}

/* Prepare to send. This is the same code as on the client side, except for the
* delayed opening of the data file */
int h3zero_prepare_to_send_buffer(void* context, size_t space,
	uint64_t echo_length, uint64_t* echo_sent, FILE* F)
{
	int ret = 0;

	if (*echo_sent < echo_length) {
		uint8_t * buffer;
		uint64_t available = echo_length - *echo_sent;
		int is_fin = 1;

		if (available > space) {
			available = space;
			is_fin = 0;
		}

		buffer = picoquic_provide_stream_data_buffer(context, (size_t)available, is_fin, !is_fin);
		if (buffer != NULL) {
			if (F) {
				size_t nb_read = fread(buffer, 1, (size_t)available, F);

				if (nb_read != available) {
					ret = -1;
				}
				else {
					*echo_sent += available;
					ret = 0;
				}
			}
			else {
				/* TODO: fill buffer with some text */
				memset(buffer, 0x5A, (size_t)available);
				*echo_sent += available;
				ret = 0;
			}
		}
		else {
			ret = -1;
		}
	}

	return ret;
}

int h3zero_prepare_to_send(int client_mode, void* context, size_t space,
	picohttp_server_stream_ctx_t* stream_ctx)
{
	int ret = 0;

	if (!client_mode && stream_ctx->F == NULL && stream_ctx->file_path != NULL) {
		stream_ctx->F = picoquic_file_open(stream_ctx->file_path, "rb");
		if (stream_ctx->F == NULL) {
			ret = -1;
		}
	}

	if (ret == 0) {
		if (client_mode) {
			ret = h3zero_prepare_to_send_buffer(context, space, stream_ctx->post_size, &stream_ctx->post_sent, NULL);
		}
		else {
			ret = h3zero_prepare_to_send_buffer(context, space, stream_ctx->echo_length, &stream_ctx->echo_sent,
				stream_ctx->F);
		}
	}
	return ret;
}

int h3zero_callback_prepare_to_send(picoquic_cnx_t* cnx,
	uint64_t stream_id, picohttp_server_stream_ctx_t * stream_ctx,
	void * context, size_t space, h3zero_callback_ctx_t* ctx)
{
	int ret = -1;

	if (stream_ctx == NULL) {
		stream_ctx = h3zero_find_stream(&ctx->h3_stream_tree, stream_id);
	}

	if (stream_ctx == NULL) {
		ret = picoquic_reset_stream(cnx, stream_id, H3ZERO_INTERNAL_ERROR);
	}
	else {
		if (stream_ctx->path_callback != NULL) {
			/* TODO: should we do that in the case of "post" ? */
			/* Get data from callback context of specific URL */
			ret = stream_ctx->path_callback(cnx, context, space, picohttp_callback_provide_data, stream_ctx, stream_ctx->path_callback_ctx);
		}
		else {
			/* default reply for known URL */
			ret = h3zero_prepare_to_send(cnx->client_mode, context, space, stream_ctx);
			/* if finished sending on server, delete stream */
			if (!cnx->client_mode) {
				if (stream_ctx->echo_sent >= stream_ctx->echo_length) {
					h3zero_delete_stream(&ctx->h3_stream_tree, stream_ctx);
					picoquic_unlink_app_stream_ctx(cnx, stream_id);
				}
			}
		}
	}

	return ret;
}

int h3zero_callback(picoquic_cnx_t* cnx,
	uint64_t stream_id, uint8_t* bytes, size_t length,
	picoquic_call_back_event_t fin_or_event, void* callback_ctx, void* v_stream_ctx)
{
	int ret = 0;
	h3zero_callback_ctx_t* ctx = NULL;
	picohttp_server_stream_ctx_t* stream_ctx = (picohttp_server_stream_ctx_t*)v_stream_ctx;
	uint64_t fin_stream_id = UINT64_MAX;

	if (callback_ctx == NULL || callback_ctx == picoquic_get_default_callback_context(cnx->quic)) {
		ctx = h3zero_callback_create_context((picohttp_server_parameters_t *)callback_ctx);
		if (ctx == NULL) {
			/* cannot handle the connection */
			picoquic_close(cnx, PICOQUIC_ERROR_MEMORY);
			return -1;
		}
		else {
			picoquic_set_callback(cnx, h3zero_callback, ctx);
			ret = h3zero_protocol_init(cnx);
		}
	} else{
		ctx = (h3zero_callback_ctx_t*)callback_ctx;
	}

	if (ret == 0) {
		switch (fin_or_event) {
		case picoquic_callback_stream_data:
		case picoquic_callback_stream_fin:
			/* Data arrival on stream #x, maybe with fin mark */
			if (picoquic_is_client(cnx)) {
				ret = h3zero_callback_client_data(cnx, stream_id, bytes, length,
					fin_or_event, ctx, stream_ctx, &fin_stream_id);
			} else {
				ret = h3zero_callback_server_data(cnx, stream_ctx, stream_id, bytes, length, fin_or_event, ctx);
			}
			break;
		case picoquic_callback_stream_reset: /* Peer reset stream #x */
		case picoquic_callback_stop_sending: /* Peer asks server to reset stream #x */
											 /* TODO: special case for uni streams. */
			if (stream_ctx == NULL) {
				stream_ctx = h3zero_find_stream(&ctx->h3_stream_tree, stream_id);
			}
			if (stream_ctx != NULL) {
				/* reset post callback. */
				if (stream_ctx->path_callback != NULL) {
					ret = stream_ctx->path_callback(NULL, NULL, 0, picohttp_callback_reset, stream_ctx, stream_ctx->path_callback_ctx);
				}

			    /* If a file is open on a client, close and do the accounting. */
				ret = h3zero_client_close_stream(cnx, ctx, stream_ctx);
			}
			if (IS_BIDIR_STREAM_ID(stream_id)) {
				picoquic_reset_stream(cnx, stream_id, 0);
			}
			break;
		case picoquic_callback_stateless_reset:
		case picoquic_callback_close: /* Received connection close */
		case picoquic_callback_application_close: /* Received application close */
			if (cnx->client_mode) {
				if (!ctx->no_print) {
					fprintf(stdout, "Received a %s\n",
						(fin_or_event == picoquic_callback_close) ? "connection close request" : (
							(fin_or_event == picoquic_callback_application_close) ?
							"request to close the application" :
							"stateless reset"));
				}
				ctx->connection_closed = 1;
				break;
			}
			else {
				picoquic_log_app_message(cnx, "Clearing context on connection close (%d)", fin_or_event);
				h3zero_callback_delete_context(cnx, ctx);
				picoquic_set_callback(cnx, NULL, NULL);
			}
			break;
		case picoquic_callback_version_negotiation:
			if (cnx->client_mode && !ctx->no_print) {
				fprintf(stdout, "Received a version negotiation request:");
				for (size_t byte_index = 0; byte_index + 4 <= length; byte_index += 4) {
					uint32_t vn = PICOPARSE_32(bytes + byte_index);
					fprintf(stdout, "%s%08x", (byte_index == 0) ? " " : ", ", vn);
				}
				fprintf(stdout, "\n");
			}
			break;
		case picoquic_callback_stream_gap:
			/* Gap indication, when unreliable streams are supported */
			ret = -1;
			break;
		case picoquic_callback_prepare_to_send:
			ret = h3zero_callback_prepare_to_send(cnx, stream_id, stream_ctx, (void*)bytes, length, ctx);
			break;
		case picoquic_callback_almost_ready:
		case picoquic_callback_ready:
			/* Check that the transport parameters are what Http3 expects */
			break;
		default:
			/* unexpected */
			break;
		}
	}

#if 0
	/* TODO: this is the plug-in for demo scenario manager */
	if (ret == 0 && fin_stream_id != UINT64_MAX) {
		/* start next batch of streams! */
		ret = picoquic_demo_client_start_streams(cnx, ctx, fin_stream_id);
	}
#endif

	return ret;
}
