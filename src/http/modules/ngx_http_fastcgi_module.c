
/*
 * Copyright (C) Igor Sysoev
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <nginx.h>


typedef struct {
    ngx_http_upstream_conf_t        upstream;

    ngx_peers_t                    *peers;

    ngx_str_t                       index;

    ngx_array_t                    *params_len;
    ngx_array_t                    *params;
    ngx_array_t                    *params_source;
} ngx_http_fastcgi_loc_conf_t;


typedef enum {
    ngx_http_fastcgi_st_version = 0,
    ngx_http_fastcgi_st_type,
    ngx_http_fastcgi_st_request_id_hi,
    ngx_http_fastcgi_st_request_id_lo,
    ngx_http_fastcgi_st_content_length_hi,
    ngx_http_fastcgi_st_content_length_lo,
    ngx_http_fastcgi_st_padding_length,
    ngx_http_fastcgi_st_reserved,
    ngx_http_fastcgi_st_data,
    ngx_http_fastcgi_st_padding,
} ngx_http_fastcgi_state_e;


typedef struct {
    ngx_http_fastcgi_state_e      state;
    u_char                       *pos;
    u_char                       *last;
    ngx_uint_t                    type;
    size_t                        length;
    size_t                        padding;

    ngx_uint_t                    header;
} ngx_http_fastcgi_ctx_t;


#define NGX_HTTP_FASTCGI_RESPONDER      1

#define NGX_HTTP_FASTCGI_BEGIN_REQUEST  1
#define NGX_HTTP_FASTCGI_ABORT_REQUEST  2
#define NGX_HTTP_FASTCGI_END_REQUEST    3
#define NGX_HTTP_FASTCGI_PARAMS         4
#define NGX_HTTP_FASTCGI_STDIN          5
#define NGX_HTTP_FASTCGI_STDOUT         6
#define NGX_HTTP_FASTCGI_STDERR         7
#define NGX_HTTP_FASTCGI_DATA           8


typedef struct {
    u_char  version;
    u_char  type;
    u_char  request_id_hi;
    u_char  request_id_lo;
    u_char  content_length_hi;
    u_char  content_length_lo;
    u_char  padding_length;
    u_char  reserved;
} ngx_http_fastcgi_header_t;


typedef struct {
    u_char  role_hi;
    u_char  role_lo;
    u_char  flags;
    u_char  reserved[5];
} ngx_http_fastcgi_begin_request_t;


typedef struct {
    u_char  version;
    u_char  type;
    u_char  request_id_hi;
    u_char  request_id_lo;
} ngx_http_fastcgi_header_small_t;


typedef struct {
    ngx_http_fastcgi_header_t         h0;
    ngx_http_fastcgi_begin_request_t  br;
    ngx_http_fastcgi_header_small_t   h1;
} ngx_http_fastcgi_request_start_t;


static ngx_int_t ngx_http_fastcgi_create_request(ngx_http_request_t *r);
static ngx_int_t ngx_http_fastcgi_reinit_request(ngx_http_request_t *r);
static ngx_int_t ngx_http_fastcgi_process_header(ngx_http_request_t *r);
static ngx_int_t ngx_http_fastcgi_input_filter(ngx_event_pipe_t *p,
    ngx_buf_t *buf);
static ngx_int_t ngx_http_fastcgi_process_record(ngx_http_request_t *r,
    ngx_http_fastcgi_ctx_t *f);
static void ngx_http_fastcgi_abort_request(ngx_http_request_t *r);
static void ngx_http_fastcgi_finalize_request(ngx_http_request_t *r,
    ngx_int_t rc);

static ngx_int_t ngx_http_fastcgi_add_variables(ngx_conf_t *cf);
static void *ngx_http_fastcgi_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_fastcgi_merge_loc_conf(ngx_conf_t *cf,
    void *parent, void *child);
static ngx_http_variable_value_t *
    ngx_http_fastcgi_script_name_variable(ngx_http_request_t *r,
    uintptr_t data);

static char *ngx_http_fastcgi_pass(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_fastcgi_lowat_check(ngx_conf_t *cf, void *post,
    void *data);


static ngx_http_fastcgi_request_start_t  ngx_http_fastcgi_request_start = {
    { 1,                                               /* version */
      NGX_HTTP_FASTCGI_BEGIN_REQUEST,                  /* type */
      0,                                               /* request_id_hi */
      1,                                               /* request_id_lo */
      0,                                               /* content_length_hi */
      sizeof(ngx_http_fastcgi_begin_request_t),        /* content_length_lo */
      0,                                               /* padding_length */
      0 },                                             /* reserved */

    { 0,                                               /* role_hi */
      NGX_HTTP_FASTCGI_RESPONDER,                      /* role_lo */
      0, /* NGX_HTTP_FASTCGI_KEEP_CONN */              /* flags */
      { 0, 0, 0, 0, 0 } },                             /* reserved[5] */

    { 1,                                               /* version */
      NGX_HTTP_FASTCGI_PARAMS,                         /* type */
      0,                                               /* request_id_hi */
      1 },                                             /* request_id_lo */

};


#if 0
static ngx_str_t ngx_http_fastcgi_methods[] = {
    ngx_string("GET"),
    ngx_string("HEAD"),
    ngx_string("POST")
};
#endif


static ngx_str_t  ngx_http_fastcgi_script_name =
    ngx_string("fastcgi_script_name");


#if (NGX_PCRE)
static ngx_str_t ngx_http_fastcgi_uri = ngx_string("/");
#endif


static ngx_conf_post_t  ngx_http_fastcgi_lowat_post =
    { ngx_http_fastcgi_lowat_check };

static ngx_conf_enum_t  ngx_http_fastcgi_set_methods[] = {
    { ngx_string("get"), NGX_HTTP_GET },
    { ngx_null_string, 0 }
};

static ngx_conf_bitmask_t  ngx_http_fastcgi_next_upstream_masks[] = {
    { ngx_string("error"), NGX_HTTP_UPSTREAM_FT_ERROR },
    { ngx_string("timeout"), NGX_HTTP_UPSTREAM_FT_TIMEOUT },
    { ngx_string("invalid_header"), NGX_HTTP_UPSTREAM_FT_INVALID_HEADER },
    { ngx_string("http_500"), NGX_HTTP_UPSTREAM_FT_HTTP_500 },
    { ngx_string("http_404"), NGX_HTTP_UPSTREAM_FT_HTTP_404 },
    { ngx_null_string, 0 }
};


static ngx_command_t  ngx_http_fastcgi_commands[] = {

    { ngx_string("fastcgi_pass"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_fastcgi_pass,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("fastcgi_index"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_fastcgi_loc_conf_t, index),
      NULL },

    { ngx_string("fastcgi_connect_timeout"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_fastcgi_loc_conf_t, upstream.connect_timeout),
      NULL },

    { ngx_string("fastcgi_send_timeout"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_fastcgi_loc_conf_t, upstream.send_timeout),
      NULL },

    { ngx_string("fastcgi_send_lowat"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_fastcgi_loc_conf_t, upstream.send_lowat),
      &ngx_http_fastcgi_lowat_post },

    { ngx_string("fastcgi_header_buffer_size"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_fastcgi_loc_conf_t, upstream.header_buffer_size),
      NULL },

    { ngx_string("fastcgi_method"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_enum_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_fastcgi_loc_conf_t, upstream.method),
      ngx_http_fastcgi_set_methods },

    { ngx_string("fastcgi_pass_request_headers"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_fastcgi_loc_conf_t, upstream.pass_request_headers),
      NULL },

    { ngx_string("fastcgi_pass_request_body"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_fastcgi_loc_conf_t, upstream.pass_request_body),
      NULL },

    { ngx_string("fastcgi_redirect_errors"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_fastcgi_loc_conf_t, upstream.redirect_errors),
      NULL },

    { ngx_string("fastcgi_x_powered_by"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_fastcgi_loc_conf_t, upstream.pass_x_powered_by),
      NULL },

    { ngx_string("fastcgi_read_timeout"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_fastcgi_loc_conf_t, upstream.read_timeout),
      NULL },

    { ngx_string("fastcgi_buffers"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE2,
      ngx_conf_set_bufs_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_fastcgi_loc_conf_t, upstream.bufs),
      NULL },

    { ngx_string("fastcgi_busy_buffers_size"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_fastcgi_loc_conf_t, upstream.busy_buffers_size),
      NULL },

    { ngx_string("fastcgi_temp_path"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1234,
      ngx_conf_set_path_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_fastcgi_loc_conf_t, upstream.temp_path),
      (void *) ngx_garbage_collector_temp_handler },

    { ngx_string("fastcgi_max_temp_file_size"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_fastcgi_loc_conf_t, upstream.max_temp_file_size),
      NULL },

    { ngx_string("fastcgi_temp_file_write_size"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_fastcgi_loc_conf_t, upstream.temp_file_write_size),
      NULL },

    { ngx_string("fastcgi_next_upstream"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_ANY,
      ngx_conf_set_bitmask_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_fastcgi_loc_conf_t, upstream.next_upstream),
      &ngx_http_fastcgi_next_upstream_masks },

    { ngx_string("fastcgi_param"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE2,
      ngx_conf_set_table_elt_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_fastcgi_loc_conf_t, params_source),
      NULL },

      ngx_null_command
};


ngx_http_module_t  ngx_http_fastcgi_module_ctx = {
    ngx_http_fastcgi_add_variables,        /* preconfiguration */
    NULL,                                  /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_fastcgi_create_loc_conf,      /* create location configuration */
    ngx_http_fastcgi_merge_loc_conf        /* merge location configuration */
};


ngx_module_t  ngx_http_fastcgi_module = {
    NGX_MODULE_V1,
    &ngx_http_fastcgi_module_ctx,          /* module context */
    ngx_http_fastcgi_commands,             /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init module */
    NULL                                   /* init process */
};


static ngx_int_t
ngx_http_fastcgi_handler(ngx_http_request_t *r)
{
    ngx_int_t                     rc;
    ngx_http_upstream_t          *u;
    ngx_http_fastcgi_loc_conf_t  *flcf;

    flcf = ngx_http_get_module_loc_conf(r, ngx_http_fastcgi_module);

    u = ngx_pcalloc(r->pool, sizeof(ngx_http_upstream_t));
    if (u == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    u->peer.log = r->connection->log;
    u->peer.log_error = NGX_ERROR_ERR;
    u->peer.peers = flcf->peers;
    u->peer.tries = flcf->peers->number;
#if (NGX_THREADS)
    u->peer.lock = &r->connection->lock;
#endif

    u->output.tag = (ngx_buf_tag_t) &ngx_http_fastcgi_module;

    u->conf = &flcf->upstream;

    u->create_request = ngx_http_fastcgi_create_request;
    u->reinit_request = ngx_http_fastcgi_reinit_request;
    u->process_header = ngx_http_fastcgi_process_header;
    u->abort_request = ngx_http_fastcgi_abort_request;
    u->finalize_request = ngx_http_fastcgi_finalize_request;

    u->pipe.input_filter = ngx_http_fastcgi_input_filter;
    u->pipe.input_ctx = r;

    r->upstream = u;

    rc = ngx_http_read_client_request_body(r, ngx_http_upstream_init);

    if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
        return rc;
    }

    return NGX_DONE;
}


static ngx_int_t
ngx_http_fastcgi_create_request(ngx_http_request_t *r)
{
    off_t                         file_pos;
    u_char                        ch, *pos;
    size_t                        size, len, key_len, val_len, padding;
    ngx_uint_t                    i, n, next;
    ngx_buf_t                    *b;
    ngx_chain_t                  *cl, *body;
    ngx_list_part_t              *part;
    ngx_table_elt_t              *header;
    ngx_http_script_code_pt       code;
    ngx_http_script_engine_t      e, le;
    ngx_http_fastcgi_header_t    *h;
    ngx_http_fastcgi_loc_conf_t  *flcf;
    ngx_http_script_len_code_pt   lcode;

    len = 0;

    flcf = ngx_http_get_module_loc_conf(r, ngx_http_fastcgi_module);

    if (flcf->params_len) {
        ngx_memzero(&le, sizeof(ngx_http_script_engine_t));

        le.ip = flcf->params_len->elts;
        le.request = r;

        while (*(uintptr_t *) le.ip) {

            lcode = *(ngx_http_script_len_code_pt *) le.ip;
            key_len = lcode(&le);

            for (val_len = 0; *(uintptr_t *) le.ip; val_len += lcode(&le)) {
                lcode = *(ngx_http_script_len_code_pt *) le.ip;
            }
            le.ip += sizeof(uintptr_t);

            if (val_len) {
                len += 1 + key_len + ((val_len > 127) ? 4 : 1) + val_len;
            }
        }
    }

    if (flcf->upstream.pass_request_headers) {

        part = &r->headers_in.headers.part;
        header = part->elts;

        for (i = 0; /* void */; i++) {

            if (i >= part->nelts) {
                if (part->next == NULL) {
                    break;
                }

                part = part->next;
                header = part->elts;
                i = 0;
            }

            len += ((sizeof("HTTP_") - 1 + header[i].key.len > 127) ? 4 : 1)
                + ((header[i].value.len > 127) ? 4 : 1)
                + sizeof("HTTP_") - 1 + header[i].key.len + header[i].value.len;
        }
    }


    if (len > 65535) {
        ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                      "fastcgi: the request record is too big");
        return NGX_ERROR;
    }


    padding = 8 - len % 8;
    padding = (padding == 8) ? 0 : padding;


    size = sizeof(ngx_http_fastcgi_header_t)
           + sizeof(ngx_http_fastcgi_begin_request_t)

           + sizeof(ngx_http_fastcgi_header_t)  /* NGX_HTTP_FASTCGI_PARAMS */
           + len + padding
           + sizeof(ngx_http_fastcgi_header_t)  /* NGX_HTTP_FASTCGI_PARAMS */

           + sizeof(ngx_http_fastcgi_header_t); /* NGX_HTTP_FASTCGI_STDIN */


    b = ngx_create_temp_buf(r->pool, size);
    if (b == NULL) {
        return NGX_ERROR;
    }

    cl = ngx_alloc_chain_link(r->pool);
    if (cl == NULL) {
        return NGX_ERROR;
    }

    cl->buf = b;

    ngx_memcpy(b->pos, &ngx_http_fastcgi_request_start,
               sizeof(ngx_http_fastcgi_request_start_t));

    h = (ngx_http_fastcgi_header_t *)
             (b->pos + sizeof(ngx_http_fastcgi_header_t)
                     + sizeof(ngx_http_fastcgi_begin_request_t));

    h->content_length_hi = (u_char) ((len >> 8) & 0xff);
    h->content_length_lo = (u_char) (len & 0xff);
    h->padding_length = (u_char) padding;
    h->reserved = 0;

    b->last = b->pos + sizeof(ngx_http_fastcgi_header_t)
                     + sizeof(ngx_http_fastcgi_begin_request_t)
                     + sizeof(ngx_http_fastcgi_header_t);


    if (flcf->params_len) {
        ngx_memzero(&e, sizeof(ngx_http_script_engine_t));

        e.ip = flcf->params->elts;
        e.pos = b->last;
        e.request = r;

        le.ip = flcf->params_len->elts;

        while (*(uintptr_t *) le.ip) {

            lcode = *(ngx_http_script_len_code_pt *) le.ip;
            key_len = (u_char) lcode(&le);

            for (val_len = 0; *(uintptr_t *) le.ip; val_len += lcode(&le)) {
                lcode = *(ngx_http_script_len_code_pt *) le.ip;
            }
            le.ip += sizeof(uintptr_t);

            if (val_len) {
                *e.pos++ = (u_char) key_len;

                if (val_len > 127) {
                    *e.pos++ = (u_char) (((val_len >> 24) & 0x7f) | 0x80);
                    *e.pos++ = (u_char) ((val_len >> 16) & 0xff);
                    *e.pos++ = (u_char) ((val_len >> 8) & 0xff);
                    *e.pos++ = (u_char) (val_len & 0xff);

                } else {
                    *e.pos++ = (u_char) val_len;
                }
            }

            e.skip = val_len ? 0 : 1;

            while (*(uintptr_t *) e.ip) {
                code = *(ngx_http_script_code_pt *) e.ip;
                code((ngx_http_script_engine_t *) &e);
            }
            e.ip += sizeof(uintptr_t);
        }

        b->last = e.pos;
    }


    if (flcf->upstream.pass_request_headers) {

        part = &r->headers_in.headers.part;
        header = part->elts;

        for (i = 0; /* void */; i++) {

            if (i >= part->nelts) {
                if (part->next == NULL) {
                    break; 
                }
    
                part = part->next;
                header = part->elts;
                i = 0;
            }

            len = sizeof("HTTP_") - 1 + header[i].key.len;
            if (len > 127) {
                *b->last++ = (u_char) (((len >> 24) & 0x7f) | 0x80);
                *b->last++ = (u_char) ((len >> 16) & 0xff);
                *b->last++ = (u_char) ((len >> 8) & 0xff);
                *b->last++ = (u_char) (len & 0xff);
    
            } else {
                *b->last++ = (u_char) len;
            }

            len = header[i].value.len;
            if (len > 127) {
                *b->last++ = (u_char) (((len >> 24) & 0x7f) | 0x80);
                *b->last++ = (u_char) ((len >> 16) & 0xff);
                *b->last++ = (u_char) ((len >> 8) & 0xff);
                *b->last++ = (u_char) (len & 0xff);

            } else {
                *b->last++ = (u_char) len;
            }

            b->last = ngx_cpymem(b->last, "HTTP_", sizeof("HTTP_") - 1);

            for (n = 0; n < header[i].key.len; n++) {
                ch = header[i].key.data[n];

                if (ch >= 'a' && ch <= 'z') {
                    ch &= ~0x20;

                } else if (ch == '-') {
                    ch = '_';
                }

                *b->last++ = ch;
            }

            b->last = ngx_cpymem(b->last, header[i].value.data,
                                 header[i].value.len);
        }
    }


    if (padding) {
        ngx_memzero(b->last, padding);
        b->last += padding;
    }


    h = (ngx_http_fastcgi_header_t *) b->last;
    b->last += sizeof(ngx_http_fastcgi_header_t);

    h->version = 1;
    h->type = NGX_HTTP_FASTCGI_PARAMS;
    h->request_id_hi = 0;
    h->request_id_lo = 1;
    h->content_length_hi = 0;
    h->content_length_lo = 0;
    h->padding_length = 0;
    h->reserved = 0;

    h = (ngx_http_fastcgi_header_t *) b->last;
    b->last += sizeof(ngx_http_fastcgi_header_t);

    if (flcf->upstream.pass_request_body) {
        body = r->upstream->request_bufs;
        r->upstream->request_bufs = cl;

#if (NGX_SUPPRESS_WARN)
        file_pos = 0;
        pos = NULL;
#endif

        while (body) {

            if (body->buf->in_file) {
                file_pos = body->buf->file_pos;

            } else {
                pos = body->buf->pos;
            }

            next = 0;

            do {
                b = ngx_alloc_buf(r->pool);
                if (b == NULL) {
                    return NGX_ERROR;
                }

                ngx_memcpy(b, body->buf, sizeof(ngx_buf_t));

                if (body->buf->in_file) {
                    b->file_pos = file_pos;
                    file_pos += 32 * 1024;

                    if (file_pos > body->buf->file_last) {
                        file_pos = body->buf->file_last;
                        next = 1;
                    }

                    b->file_last = file_pos;
                    len = (ngx_uint_t) (file_pos - b->file_pos);

                } else {
                    b->pos = pos;
                    pos += 32 * 1024;

                    if (pos > body->buf->last) {
                        pos = body->buf->last;
                        next = 1;
                    }

                    b->last = pos;
                    len = (ngx_uint_t) (pos - b->pos);
                }

                padding = 8 - len % 8;
                padding = (padding == 8) ? 0 : padding;

                h->version = 1;
                h->type = NGX_HTTP_FASTCGI_STDIN;
                h->request_id_hi = 0;
                h->request_id_lo = 1;
                h->content_length_hi = (u_char) ((len >> 8) & 0xff);
                h->content_length_lo = (u_char) (len & 0xff);
                h->padding_length = (u_char) padding;
                h->reserved = 0;

                cl->next = ngx_alloc_chain_link(r->pool);
                if (cl->next == NULL) {
                    return NGX_ERROR;
                }

                cl = cl->next;
                cl->buf = b;

                b = ngx_create_temp_buf(r->pool,
                                        sizeof(ngx_http_fastcgi_header_t)
                                        + padding);
                if (b == NULL) {
                    return NGX_ERROR;
                }

                if (padding) {
                    ngx_memzero(b->last, padding);
                    b->last += padding;
                }

                h = (ngx_http_fastcgi_header_t *) b->last;
                b->last += sizeof(ngx_http_fastcgi_header_t);

                cl->next = ngx_alloc_chain_link(r->pool);
                if (cl->next == NULL) {
                    return NGX_ERROR;
                }

                cl = cl->next;
                cl->buf = b;

            } while (!next);

            body = body->next;
        }

    } else {
        r->upstream->request_bufs = cl;
    }

    h->version = 1;
    h->type = NGX_HTTP_FASTCGI_STDIN;
    h->request_id_hi = 0;
    h->request_id_lo = 1;
    h->content_length_hi = 0;
    h->content_length_lo = 0;
    h->padding_length = 0;
    h->reserved = 0;

    cl->next = NULL;

    return NGX_OK;
}


static ngx_int_t
ngx_http_fastcgi_reinit_request(ngx_http_request_t *r)
{
    ngx_http_fastcgi_ctx_t  *f;

    f = ngx_http_get_module_ctx(r, ngx_http_fastcgi_module);

    if (f == NULL) {
        return NGX_OK;
    }

    f->state = ngx_http_fastcgi_st_version;
    f->header = 0;

    return NGX_OK;
}


static ngx_int_t
ngx_http_fastcgi_process_header(ngx_http_request_t *r)
{
    u_char                         *start, *last;
    ngx_str_t                      *status_line, line;
    ngx_int_t                       rc, status;
    ngx_uint_t                      key;
    ngx_table_elt_t                *h;
    ngx_http_upstream_t            *u;
    ngx_http_fastcgi_ctx_t         *f;
    ngx_http_upstream_header_t     *hh;
    ngx_http_upstream_main_conf_t  *umcf;

    f = ngx_http_get_module_ctx(r, ngx_http_fastcgi_module);

    umcf = ngx_http_get_module_main_conf(r, ngx_http_upstream_module);
    hh = (ngx_http_upstream_header_t *) umcf->headers_in_hash.buckets;

    if (f == NULL) {
        f = ngx_pcalloc(r->pool, sizeof(ngx_http_fastcgi_ctx_t));
        if (f == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        ngx_http_set_ctx(r, f, ngx_http_fastcgi_module);
    }

    u = r->upstream;

    for ( ;; ) {

        if (f->state < ngx_http_fastcgi_st_data) {

            f->pos = u->header_in.pos;
            f->last = u->header_in.last;

            rc = ngx_http_fastcgi_process_record(r, f);

            u->header_in.pos = f->pos;
            u->header_in.last = f->last;

            if (rc == NGX_AGAIN) {
                return NGX_AGAIN;
            }

            if (rc == NGX_ERROR) {
                return NGX_HTTP_UPSTREAM_INVALID_HEADER;
            }

            if (f->type != NGX_HTTP_FASTCGI_STDOUT
                && f->type != NGX_HTTP_FASTCGI_STDERR)
            {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                              "upstream sent unexpected FastCGI record: %d",
                              f->type);

                return NGX_HTTP_UPSTREAM_INVALID_HEADER;
            }

            if (f->type == NGX_HTTP_FASTCGI_STDOUT && f->length == 0) {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                              "upstream closed prematurely FastCGI stdout");

                return NGX_HTTP_UPSTREAM_INVALID_HEADER;
            }
        }

        if (f->state == ngx_http_fastcgi_st_padding) {

            if (u->header_in.pos + f->padding < u->header_in.last) {
                f->state = ngx_http_fastcgi_st_version;
                u->header_in.pos += f->padding;

                continue;
            }

            if (u->header_in.pos + f->padding == u->header_in.last) {
                f->state = ngx_http_fastcgi_st_version;
                u->header_in.pos = u->header_in.last;

                return NGX_AGAIN;
            }

            f->padding -= u->header_in.last - u->header_in.pos;
            u->header_in.pos = u->header_in.last;

            return NGX_AGAIN;
        }


        /* f->state == ngx_http_fastcgi_st_data */

        if (f->type == NGX_HTTP_FASTCGI_STDERR) {

            if (f->header) {
                ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                              "upstream split a header in FastCGI records");

                return NGX_HTTP_UPSTREAM_INVALID_HEADER;
            }

            if (f->length) {
                line.data = u->header_in.pos;

                if (u->header_in.pos + f->length <= u->header_in.last) {
                    line.len = f->length;
                    u->header_in.pos += f->length;
                    f->length = 0;
                    f->state = ngx_http_fastcgi_st_padding;

                } else { 
                    line.len = u->header_in.last - u->header_in.pos;
                    f->length -= u->header_in.last - u->header_in.pos;
                    u->header_in.pos = u->header_in.last;
                }

                while (line.data[line.len - 1] == LF
                       || line.data[line.len - 1] == CR
                       || line.data[line.len - 1] == '.'
                       || line.data[line.len - 1] == ' ')
                {
                    line.len--;
                }

                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                              "FastCGI sent in stderr: \"%V\"", &line);

                if (u->header_in.pos == u->header_in.last) {
                    return NGX_AGAIN;
                }

            } else {
                f->state = ngx_http_fastcgi_st_version;
            }

            continue;
        }


        /* f->type == NGX_HTTP_FASTCGI_STDOUT */

        start = u->header_in.pos;

        if (u->header_in.pos + f->length < u->header_in.last) {

            /*
             * set u->header_in.last to the end of the FastCGI record data
             * for ngx_http_parse_header_line()
             */

            last = u->header_in.last;
            u->header_in.last = u->header_in.pos + f->length;

        } else {
            last = NULL;
        }

        f->header = 1;

        for ( ;; ) {

            rc = ngx_http_parse_header_line(r, &u->header_in);

            ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                           "http fastcgi parser: %d", rc);

            if (rc == NGX_AGAIN) {
                break;
            }

            if (rc == NGX_OK) {

                /* a header line has been parsed successfully */

                h = ngx_list_push(&u->headers_in.headers);
                if (h == NULL) {
                    return NGX_HTTP_INTERNAL_SERVER_ERROR;
                }

                h->hash = r->header_hash;

                h->key.len = r->header_name_end - r->header_name_start;
                h->value.len = r->header_end - r->header_start;

                h->key.data = ngx_palloc(r->pool,
                                         h->key.len + 1 + h->value.len + 1);
                if (h->key.data == NULL) {
                    return NGX_HTTP_INTERNAL_SERVER_ERROR;
                }

                h->value.data = h->key.data + h->key.len + 1;

                ngx_cpystrn(h->key.data, r->header_name_start, h->key.len + 1);
                ngx_cpystrn(h->value.data, r->header_start, h->value.len + 1);

                key = h->hash % umcf->headers_in_hash.hash_size;

                if (hh[key].name.len == h->key.len
                    && ngx_strcasecmp(hh[key].name.data, h->key.data) == 0)
                {
                    if (hh[key].handler(r, h, hh[key].offset) != NGX_OK) {
                        return NGX_HTTP_INTERNAL_SERVER_ERROR;
                    }
                }

                ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                               "http fastcgi header: \"%V: %V\"",
                               &h->key, &h->value);

                continue;
            }

            if (rc == NGX_HTTP_PARSE_HEADER_DONE) {

                /* a whole header has been parsed successfully */

                ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                               "http fastcgi header done");

                if (u->headers_in.status) {
                    status_line = &u->headers_in.status->value;

                    status = ngx_atoi(status_line->data, 3);

                    if (status == NGX_ERROR) {
                        return NGX_HTTP_INTERNAL_SERVER_ERROR;
                    }

                    r->headers_out.status = status;
                    r->headers_out.status_line = *status_line;

                } else {
                    r->headers_out.status = 200;
                    r->headers_out.status_line.len = sizeof("200 OK") - 1;
                    r->headers_out.status_line.data = (u_char *) "200 OK";
                }

                u->state->status = r->headers_out.status;
#if 0
                if (u->cachable) {
                    u->cachable = ngx_http_upstream_is_cachable(r);
                }
#endif

                break;
            }

            /* there was error while a header line parsing */

            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          ngx_http_upstream_header_errors[rc
                                                - NGX_HTTP_PARSE_HEADER_ERROR]);

            return NGX_HTTP_UPSTREAM_INVALID_HEADER;

        }

        if (last) {
            u->header_in.last = last;
        }

        f->length -= u->header_in.pos - start;

        if (rc == NGX_AGAIN) {
            if (u->header_in.pos == u->header_in.last) {
                return NGX_AGAIN;
            }

            ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                          "upstream split a header in FastCGI records");

            return NGX_HTTP_UPSTREAM_INVALID_HEADER;
        }

        if (f->length == 0) {
            if (f->padding) {
                f->state = ngx_http_fastcgi_st_padding;
            } else {
                f->state = ngx_http_fastcgi_st_version;
            }
        }

        return NGX_OK;
    }
}


static ngx_int_t
ngx_http_fastcgi_input_filter(ngx_event_pipe_t *p, ngx_buf_t *buf)
{
    ngx_int_t                rc;
    ngx_buf_t               *b, **prev;
    ngx_str_t                line;
    ngx_chain_t             *cl;
    ngx_http_request_t      *r;
    ngx_http_fastcgi_ctx_t  *f;

    if (buf->pos == buf->last) {
        return NGX_OK;
    }

    r = p->input_ctx;
    f = ngx_http_get_module_ctx(r, ngx_http_fastcgi_module);

    b = NULL;
    prev = &buf->shadow;

    f->pos = buf->pos;
    f->last = buf->last;

    for ( ;; ) {
        if (f->state < ngx_http_fastcgi_st_data) {

            rc = ngx_http_fastcgi_process_record(r, f);

            if (rc == NGX_AGAIN) {
                break;
            }

            if (rc == NGX_ERROR) {
                return NGX_ERROR;
            }

            if (f->type == NGX_HTTP_FASTCGI_STDOUT && f->length == 0) {
                f->state = ngx_http_fastcgi_st_version;
                p->upstream_done = 1;

                ngx_log_debug0(NGX_LOG_DEBUG_HTTP, p->log, 0,
                               "http fastcgi closed stdout");

                continue;
            }

            if (f->type == NGX_HTTP_FASTCGI_END_REQUEST) {
                f->state = ngx_http_fastcgi_st_version;
                p->upstream_done = 1;

                ngx_log_debug0(NGX_LOG_DEBUG_HTTP, p->log, 0,
                               "http fastcgi sent end request");

                break;
            }
        }


        if (f->state == ngx_http_fastcgi_st_padding) {

            if (f->pos + f->padding < f->last) {
                f->state = ngx_http_fastcgi_st_version;
                f->pos += f->padding;

                continue;
            }

            if (f->pos + f->padding == f->last) {
                f->state = ngx_http_fastcgi_st_version;

                break;
            }

            f->padding -= f->last - f->pos;

            break;
        }


        /* f->state == ngx_http_fastcgi_st_data */

        if (f->type == NGX_HTTP_FASTCGI_STDERR) {

            if (f->length) {
                line.data = f->pos;

                if (f->pos + f->length <= f->last) {
                    line.len = f->length;
                    f->pos += f->length;
                    f->length = 0;
                    f->state = ngx_http_fastcgi_st_padding;

                } else { 
                    line.len = f->last - f->pos;
                    f->length -= f->last - f->pos;
                    f->pos = f->last;
                }

                while (line.data[line.len - 1] == LF
                       || line.data[line.len - 1] == CR
                       || line.data[line.len - 1] == '.'
                       || line.data[line.len - 1] == ' ')
                {
                    line.len--;
                }

                ngx_log_error(NGX_LOG_ERR, p->log, 0,
                              "FastCGI sent in stderr: \"%V\"", &line);

                if (f->pos == f->last) {
                    break;
                }

            } else {
                f->state = ngx_http_fastcgi_st_version;
            }

            continue;
        }


        /* f->type == NGX_HTTP_FASTCGI_STDOUT */

        if (p->free) {
            b = p->free->buf;
            p->free = p->free->next;

        } else {
            b = ngx_alloc_buf(p->pool);
            if (b == NULL) {
                return NGX_ERROR;
            }
        }

        ngx_memzero(b, sizeof(ngx_buf_t));

        b->pos = f->pos;
        b->start = buf->start;
        b->end = buf->end;
        b->tag = p->tag;
        b->temporary = 1;
        b->recycled = 1;

        *prev = b;
        prev = &b->shadow;

        cl = ngx_alloc_chain_link(p->pool);
        if (cl == NULL) {
            return NGX_ERROR;
        }

        cl->buf = b;
        cl->next = NULL;

        if (p->in) {
            *p->last_in = cl;
        } else {
            p->in = cl;
        }
        p->last_in = &cl->next;


        /* STUB */ b->num = buf->num;

        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, p->log, 0, "input buf #%d", b->num);


        if (f->pos + f->length < f->last) {

            if (f->padding) {
                f->state = ngx_http_fastcgi_st_padding;
            } else {
                f->state = ngx_http_fastcgi_st_version;
            }

            f->pos += f->length;
            b->last = f->pos;

            continue;
        }

        if (f->pos + f->length == f->last) {

            if (f->padding) {
                f->state = ngx_http_fastcgi_st_padding;
            } else {
                f->state = ngx_http_fastcgi_st_version;
            }

            b->last = f->last;

            break;
        }

        f->length -= f->last - f->pos;

        b->last = f->last;

        break;

    }

    if (b) {
        b->shadow = buf;
        b->last_shadow = 1;

        return NGX_OK;
    }

    /* there is no data record in the buf, add it to free chain */

    if (ngx_event_pipe_add_free_buf(p, buf) != NGX_OK) {
        return NGX_ERROR;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_fastcgi_process_record(ngx_http_request_t *r,
    ngx_http_fastcgi_ctx_t *f)
{
    u_char                     ch, *p;
    ngx_http_fastcgi_state_e   state;

    state = f->state;

    for (p = f->pos; p < f->last; p++) {

        ch = *p;

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "http fastcgi record byte: %02Xd", ch);

        switch (state) {

        case ngx_http_fastcgi_st_version:
            if (ch != 1) {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                              "upstream sent unsupported FastCGI "
                              "protocol version: %d", ch);
                return NGX_ERROR;
            }
            state = ngx_http_fastcgi_st_type;
            break;

        case ngx_http_fastcgi_st_type:
            switch (ch) {
            case NGX_HTTP_FASTCGI_STDOUT:
            case NGX_HTTP_FASTCGI_STDERR:
            case NGX_HTTP_FASTCGI_END_REQUEST:
                 f->type = (ngx_uint_t) ch;
                 break;
            default:
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                              "upstream sent invalid FastCGI "
                              "record type: %d", ch);
                return NGX_ERROR;

            }
            state = ngx_http_fastcgi_st_request_id_hi;
            break;

        /* we support the single request per connection */

        case ngx_http_fastcgi_st_request_id_hi:
            if (ch != 0) {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                              "upstream sent unexpected FastCGI "
                              "request id high byte: %d", ch);
                return NGX_ERROR;
            }
            state = ngx_http_fastcgi_st_request_id_lo;
            break;

        case ngx_http_fastcgi_st_request_id_lo:
            if (ch != 1) {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                              "upstream sent unexpected FastCGI "
                              "request id low byte: %d", ch);
                return NGX_ERROR;
            }
            state = ngx_http_fastcgi_st_content_length_hi;
            break;

        case ngx_http_fastcgi_st_content_length_hi:
            f->length = ch << 8;
            state = ngx_http_fastcgi_st_content_length_lo;
            break;

        case ngx_http_fastcgi_st_content_length_lo:
            f->length |= (size_t) ch;
            state = ngx_http_fastcgi_st_padding_length;
            break;

        case ngx_http_fastcgi_st_padding_length:
            f->padding = (size_t) ch;
            state = ngx_http_fastcgi_st_reserved;
            break;

        case ngx_http_fastcgi_st_reserved:
            state = ngx_http_fastcgi_st_data;

            ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                           "http fastcgi record length: %z", f->length);

            f->pos = p + 1;
            f->state = state;

            return NGX_OK;

        /* suppress warning */
        case ngx_http_fastcgi_st_data:
        case ngx_http_fastcgi_st_padding:
            break;
        }
    }

    f->state = state;

    return NGX_AGAIN;
}


static void
ngx_http_fastcgi_abort_request(ngx_http_request_t *r)
{
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "abort http fastcgi request");

    return;
}


static void
ngx_http_fastcgi_finalize_request(ngx_http_request_t *r, ngx_int_t rc)
{
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "finalize http fastcgi request");

    return;
}


static ngx_int_t
ngx_http_fastcgi_add_variables(ngx_conf_t *cf)
{
    ngx_http_variable_t  *var;

    var = ngx_http_add_variable(cf, &ngx_http_fastcgi_script_name, 0);
    if (var == NULL) {
        return NGX_ERROR;
    }

    var->handler = ngx_http_fastcgi_script_name_variable;

    return NGX_OK;
}


static void *
ngx_http_fastcgi_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_fastcgi_loc_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_fastcgi_loc_conf_t));
    if (conf == NULL) {
        return NGX_CONF_ERROR;
    }

    /*
     * set by ngx_pcalloc():
     *
     *     conf->upstream.bufs.num = 0;
     *     conf->upstream.path = NULL;
     *     conf->upstream.next_upstream = 0;
     *     conf->upstream.temp_path = NULL;
     *     conf->upstream.schema = { 0, NULL };
     *     conf->upstream.uri = { 0, NULL };
     *     conf->upstream.location = NULL;
     *
     *     conf->index.len = 0;
     *     conf->index.data = NULL;
     */

    conf->upstream.connect_timeout = NGX_CONF_UNSET_MSEC;
    conf->upstream.send_timeout = NGX_CONF_UNSET_MSEC;
    conf->upstream.read_timeout = NGX_CONF_UNSET_MSEC;

    conf->upstream.send_lowat = NGX_CONF_UNSET_SIZE;
    conf->upstream.header_buffer_size = NGX_CONF_UNSET_SIZE;
    conf->upstream.busy_buffers_size = NGX_CONF_UNSET_SIZE;
    conf->upstream.max_temp_file_size = NGX_CONF_UNSET_SIZE; 
    conf->upstream.temp_file_write_size = NGX_CONF_UNSET_SIZE;

    conf->upstream.pass_unparsed_uri = NGX_CONF_UNSET;
    conf->upstream.method = NGX_CONF_UNSET_UINT;
    conf->upstream.pass_request_headers = NGX_CONF_UNSET;
    conf->upstream.pass_request_body = NGX_CONF_UNSET;

    conf->upstream.redirect_errors = NGX_CONF_UNSET;

    /* "fastcgi_cyclic_temp_file" is disabled */
    conf->upstream.cyclic_temp_file = 0;

    conf->upstream.pass_x_powered_by = NGX_CONF_UNSET;

    /* the hardcoded values */
    conf->upstream.pass_server = 1;
    conf->upstream.pass_date = 1;

    return conf;
}


static char *
ngx_http_fastcgi_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_fastcgi_loc_conf_t *prev = parent;
    ngx_http_fastcgi_loc_conf_t *conf = child;

    u_char                       *p;
    size_t                        size;
    uintptr_t                    *code;
    ngx_uint_t                    i;
    ngx_table_elt_t              *src;
    ngx_http_script_compile_t     sc;
    ngx_http_script_copy_code_t  *copy;

    ngx_conf_merge_msec_value(conf->upstream.connect_timeout,
                              prev->upstream.connect_timeout, 60000);

    ngx_conf_merge_msec_value(conf->upstream.send_timeout,
                              prev->upstream.send_timeout, 60000);

    ngx_conf_merge_msec_value(conf->upstream.read_timeout,
                              prev->upstream.read_timeout, 60000);

    ngx_conf_merge_size_value(conf->upstream.send_lowat,
                              prev->upstream.send_lowat, 0);

    ngx_conf_merge_size_value(conf->upstream.header_buffer_size, 
                              prev->upstream.header_buffer_size,
                              (size_t) ngx_pagesize);


    ngx_conf_merge_bufs_value(conf->upstream.bufs, prev->upstream.bufs,
                              8, ngx_pagesize);

    if (conf->upstream.bufs.num < 2) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "there must be at least 2 \"fastcgi_buffers\"");
        return NGX_CONF_ERROR;
    }


    size = conf->upstream.header_buffer_size;
    if (size < conf->upstream.bufs.size) {
        size = conf->upstream.bufs.size;
    }


    ngx_conf_merge_size_value(conf->upstream.busy_buffers_size, 
                              prev->upstream.busy_buffers_size,
                              NGX_CONF_UNSET_SIZE);

    if (conf->upstream.busy_buffers_size == NGX_CONF_UNSET_SIZE) {
        conf->upstream.busy_buffers_size = 2 * size;

    } else if (conf->upstream.busy_buffers_size < size) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
             "\"fastcgi_busy_buffers_size\" must be equal or bigger than "
             "maximum of the value of \"fastcgi_header_buffer_size\" and "
             "one of the \"fastcgi_buffers\"");

        return NGX_CONF_ERROR;

    } else if (conf->upstream.busy_buffers_size
               > (conf->upstream.bufs.num - 1) * conf->upstream.bufs.size)
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
             "\"fastcgi_busy_buffers_size\" must be less than "
             "the size of all \"fastcgi_buffers\" minus one buffer");

        return NGX_CONF_ERROR;
    }


    ngx_conf_merge_size_value(conf->upstream.temp_file_write_size, 
                              prev->upstream.temp_file_write_size,
                              NGX_CONF_UNSET_SIZE);

    if (conf->upstream.temp_file_write_size == NGX_CONF_UNSET_SIZE) {
        conf->upstream.temp_file_write_size = 2 * size;

    } else if (conf->upstream.temp_file_write_size < size) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
             "\"fastcgi_temp_file_write_size\" must be equal or bigger than "
             "maximum of the value of \"fastcgi_header_buffer_size\" and "
             "one of the \"fastcgi_buffers\"");

        return NGX_CONF_ERROR;
    }


    ngx_conf_merge_size_value(conf->upstream.max_temp_file_size,
                              prev->upstream.max_temp_file_size,
                              NGX_CONF_UNSET_SIZE);

    if (conf->upstream.max_temp_file_size == NGX_CONF_UNSET_SIZE) {

        conf->upstream.max_temp_file_size = 1024 * 1024 * 1024;

    } else if (conf->upstream.max_temp_file_size != 0
               && conf->upstream.max_temp_file_size < size)
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
             "\"fastcgi_max_temp_file_size\" must be equal to zero to disable "
             "the temporary files usage or must be equal or bigger than "
             "maximum of the value of \"fastcgi_header_buffer_size\" and "
             "one of the \"fastcgi_buffers\"");

        return NGX_CONF_ERROR;
    }


    ngx_conf_merge_bitmask_value(conf->upstream.next_upstream,
                              prev->upstream.next_upstream,
                              (NGX_CONF_BITMASK_SET
                               |NGX_HTTP_UPSTREAM_FT_ERROR
                               |NGX_HTTP_UPSTREAM_FT_TIMEOUT));

    ngx_conf_merge_path_value(conf->upstream.temp_path,
                              prev->upstream.temp_path,
                              NGX_HTTP_FASTCGI_TEMP_PATH, 1, 2, 0,
                              ngx_garbage_collector_temp_handler, cf);

    ngx_conf_merge_msec_value(conf->upstream.pass_unparsed_uri,
                              prev->upstream.pass_unparsed_uri, 0);

    if (conf->upstream.pass_unparsed_uri && conf->upstream.location->len > 1) {
        ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                      "\"fastcgi_pass_unparsed_uri\" can be set for "
                      "location \"/\" or given by regular expression.");
        return NGX_CONF_ERROR;
    }

    if (conf->upstream.method == NGX_CONF_UNSET_UINT) {
        conf->upstream.method = prev->upstream.method; 
    }

    ngx_conf_merge_value(conf->upstream.pass_request_headers,
                              prev->upstream.pass_request_headers, 1);
    ngx_conf_merge_value(conf->upstream.pass_request_body,
                              prev->upstream.pass_request_body, 1);

    ngx_conf_merge_msec_value(conf->upstream.redirect_errors,
                              prev->upstream.redirect_errors, 0);

    ngx_conf_merge_msec_value(conf->upstream.pass_x_powered_by,
                              prev->upstream.pass_x_powered_by, 1);


    ngx_conf_merge_str_value(conf->index, prev->index, "");

    if (conf->peers == NULL) {
        conf->peers = prev->peers;
        conf->upstream = prev->upstream;
    }

    if (conf->params_source == NULL) {
        conf->params_source = prev->params_source;
        conf->params_len = prev->params_len;
        conf->params = prev->params;

        if (conf->params_source == NULL) {
            return NGX_CONF_OK;
        }
    }

    conf->params_len = ngx_array_create(cf->pool, 64, 1);
    if (conf->params_len == NULL) {
        return NGX_CONF_ERROR;
    }
    
    conf->params = ngx_array_create(cf->pool, 512, 1);
    if (conf->params == NULL) {
        return NGX_CONF_ERROR;
    }

    src = conf->params_source->elts;
    for (i = 0; i < conf->params_source->nelts; i++) {

        if (ngx_http_script_variables_count(&src[i].value) == 0) {
            copy = ngx_array_push_n(conf->params_len,
                                    sizeof(ngx_http_script_copy_code_t));
            if (copy == NULL) {
                return NGX_CONF_ERROR;
            }

            copy->code = (ngx_http_script_code_pt)
                                                  ngx_http_script_copy_len_code;
            copy->len = src[i].key.len;


            copy = ngx_array_push_n(conf->params_len,
                                    sizeof(ngx_http_script_copy_code_t));
            if (copy == NULL) {
                return NGX_CONF_ERROR;
            }

            copy->code = (ngx_http_script_code_pt)
                                                 ngx_http_script_copy_len_code;
            copy->len = src[i].value.len;


            size = (sizeof(ngx_http_script_copy_code_t)
                       + src[i].key.len + src[i].value.len
                       + sizeof(uintptr_t) - 1)
                    & ~(sizeof(uintptr_t) - 1);

            copy = ngx_array_push_n(conf->params, size);
            if (copy == NULL) {
                return NGX_CONF_ERROR;
            }
    
            copy->code = ngx_http_script_copy_code;
            copy->len = src[i].key.len + src[i].value.len;

            p = (u_char *) copy + sizeof(ngx_http_script_copy_code_t);

            p = ngx_cpymem(p, src[i].key.data, src[i].key.len);
            ngx_memcpy(p, src[i].value.data, src[i].value.len);

        } else {
            copy = ngx_array_push_n(conf->params_len,
                                    sizeof(ngx_http_script_copy_code_t));
            if (copy == NULL) {
                return NGX_CONF_ERROR;
            }

            copy->code = (ngx_http_script_code_pt)
                                                 ngx_http_script_copy_len_code;
            copy->len = src[i].key.len;


            size = (sizeof(ngx_http_script_copy_code_t)
                    + src[i].key.len + sizeof(uintptr_t) - 1)
                    & ~(sizeof(uintptr_t) - 1);

            copy = ngx_array_push_n(conf->params, size);
            if (copy == NULL) {
                return NGX_CONF_ERROR;
            }
    
            copy->code = ngx_http_script_copy_code;
            copy->len = src[i].key.len;

            p = (u_char *) copy + sizeof(ngx_http_script_copy_code_t);
            ngx_memcpy(p, src[i].key.data, src[i].key.len);


            ngx_memzero(&sc, sizeof(ngx_http_script_compile_t));

            sc.cf = cf;
            sc.source = &src[i].value;
            sc.lengths = &conf->params_len;
            sc.values = &conf->params;

            if (ngx_http_script_compile(&sc) != NGX_OK) {
                return NGX_CONF_ERROR;
            }
        }

        code = ngx_array_push_n(conf->params_len, sizeof(uintptr_t));
        if (code == NULL) {
            return NGX_CONF_ERROR;
        }

        *code = (uintptr_t) NULL;


        code = ngx_array_push_n(conf->params, sizeof(uintptr_t));
        if (code == NULL) {
            return NGX_CONF_ERROR;
        }

        *code = (uintptr_t) NULL;
    }

    code = ngx_array_push_n(conf->params_len, sizeof(uintptr_t));
    if (code == NULL) {
        return NGX_CONF_ERROR;
    }

    *code = (uintptr_t) NULL;

    return NGX_CONF_OK;
}


static ngx_http_variable_value_t *
ngx_http_fastcgi_script_name_variable(ngx_http_request_t *r, uintptr_t data)
{
    u_char                       *p;
    ngx_http_variable_value_t    *vv;
    ngx_http_fastcgi_loc_conf_t  *flcf;

    vv = ngx_palloc(r->pool, sizeof(ngx_http_variable_value_t));
    if (vv == NULL) {
        return NULL;
    }

    vv->value = 0;

    flcf = ngx_http_get_module_loc_conf(r, ngx_http_fastcgi_module);

    if (r->uri.data[r->uri.len - 1] != '/') {
        vv->text = r->uri;
        return vv;
    }

    vv->text.len = r->uri.len + flcf->index.len;

    vv->text.data = ngx_palloc(r->pool, vv->text.len);
    if (vv->text.data == NULL) {
        return NULL;
    }

    p = ngx_cpymem(vv->text.data, r->uri.data, r->uri.len);
    ngx_memcpy(p, flcf->index.data, flcf->index.len);

    return vv;
}


static char *
ngx_http_fastcgi_pass(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_fastcgi_loc_conf_t *lcf = conf;

    ngx_str_t                   *value;
    ngx_inet_upstream_t          inet_upstream;
    ngx_http_core_loc_conf_t    *clcf;
#if (NGX_HAVE_UNIX_DOMAIN)
    ngx_unix_domain_upstream_t   unix_upstream;
#endif

    value = cf->args->elts;

    if (ngx_strncasecmp(value[1].data, "unix:", 5) == 0) {

#if (NGX_HAVE_UNIX_DOMAIN)

        ngx_memzero(&unix_upstream, sizeof(ngx_unix_domain_upstream_t));

        unix_upstream.name = value[1];
        unix_upstream.url = value[1];

        lcf->peers = ngx_unix_upstream_parse(cf, &unix_upstream);
        if (lcf->peers == NULL) {
            return NGX_CONF_ERROR;
        }

#else
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "the unix domain sockets are not supported "
                           "on this platform");
        return NGX_CONF_ERROR;

#endif

    } else {
        ngx_memzero(&inet_upstream, sizeof(ngx_inet_upstream_t));

        inet_upstream.name = value[1];
        inet_upstream.url = value[1];
    
        lcf->peers = ngx_inet_upstream_parse(cf, &inet_upstream);
        if (lcf->peers == NULL) {
            return NGX_CONF_ERROR;
        }
    }

    lcf->upstream.schema.len = sizeof("fastcgi://") - 1;
    lcf->upstream.schema.data = (u_char *) "fastcgi://";
    lcf->upstream.uri.len = sizeof("/") - 1;
    lcf->upstream.uri.data = (u_char *) "/";

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);

    clcf->handler = ngx_http_fastcgi_handler;

#if (NGX_PCRE)
    lcf->upstream.location = clcf->regex ? &ngx_http_fastcgi_uri : &clcf->name;
#else
    lcf->upstream.location = &clcf->name;
#endif

    if (clcf->name.data[clcf->name.len - 1] == '/') {
        clcf->auto_redirect = 1;
    }

    return NGX_CONF_OK;
}


static char *
ngx_http_fastcgi_lowat_check(ngx_conf_t *cf, void *post, void *data)
{
#if (NGX_FREEBSD)
    ssize_t *np = data;

    if (*np >= ngx_freebsd_net_inet_tcp_sendspace) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "\"fastcgi_send_lowat\" must be less than %d "
                           "(sysctl net.inet.tcp.sendspace)",
                           ngx_freebsd_net_inet_tcp_sendspace);

        return NGX_CONF_ERROR;
    }

#elif !(NGX_HAVE_SO_SNDLOWAT)
    ssize_t *np = data;

    ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                       "\"fastcgi_send_lowat\" is not supported, ignored");

    *np = 0;

#endif

    return NGX_CONF_OK;
}