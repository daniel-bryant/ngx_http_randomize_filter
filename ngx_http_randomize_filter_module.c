
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


typedef enum {
    OUTSIDE_TAG,
    TAG_NAME,
    READY_FOR_ATTR,
    ATTR_NAME,
    READY_FOR_VAL,
    ATTR_VALUE
} state_t;


typedef struct {
    ngx_http_complex_value_t   match;
} ngx_http_sub_pair_t;


typedef struct {
    ngx_str_t                  match;
} ngx_http_sub_match_t;


typedef struct {
    ngx_uint_t                 min_match_len;
    ngx_uint_t                 max_match_len;

    u_char                     index[257];
    u_char                     shift[256];
} ngx_http_sub_tables_t;


typedef struct {
    ngx_uint_t                 dynamic; /* unsigned dynamic:1; */

    ngx_http_complex_value_t  *salt;

    ngx_array_t               *pairs;

    ngx_http_sub_tables_t     *tables;

    ngx_hash_t                 types;

    ngx_flag_t                 once;
    ngx_flag_t                 last_modified;

    ngx_array_t               *types_keys;
    ngx_array_t               *matches;
} ngx_http_sub_loc_conf_t;


typedef struct {
    ngx_str_t                  saved;
    ngx_str_t                  looked;

    ngx_uint_t                 once;   /* unsigned  once:1 */

    ngx_buf_t                 *buf;

    u_char                    *pos;
    u_char                    *copy_start;
    u_char                    *copy_end;

    ngx_chain_t               *in;
    ngx_chain_t               *out;
    ngx_chain_t              **last_out;
    ngx_chain_t               *busy;
    ngx_chain_t               *free;

    ngx_str_t                 *sub;
    ngx_uint_t                 applied;

    ngx_int_t                  offset;
    ngx_uint_t                 index;

    ngx_http_sub_tables_t     *tables;
    ngx_array_t               *matches;

    state_t                    state;
} ngx_http_sub_ctx_t;


static ngx_uint_t ngx_http_sub_cmp_index;


static ngx_int_t ngx_http_sub_output(ngx_http_request_t *r,
    ngx_http_sub_ctx_t *ctx);
static ngx_int_t ngx_http_sub_parse(ngx_http_request_t *r,
    ngx_http_sub_ctx_t *ctx);
static ngx_int_t ngx_http_sub_parse_html(ngx_http_request_t *r,
    ngx_http_sub_ctx_t *ctx);
static void ngx_http_sub_hash(ngx_str_t *ngx_str);

static char * ngx_http_sub_filter(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char * ngx_http_sub_filter_salt(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static void *ngx_http_sub_create_conf(ngx_conf_t *cf);
static char *ngx_http_sub_merge_conf(ngx_conf_t *cf,
    void *parent, void *child);
static void ngx_http_sub_init_tables(ngx_http_sub_tables_t *tables,
    ngx_http_sub_match_t *match, ngx_uint_t n);
static ngx_int_t ngx_http_sub_cmp_matches(const void *one, const void *two);
static ngx_int_t ngx_http_sub_filter_init(ngx_conf_t *cf);


static ngx_command_t  ngx_http_sub_filter_commands[] = {

    { ngx_string("randomize_filter"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_sub_filter,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("randomize_filter_salt"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_sub_filter_salt,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("randomize_filter_types"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_http_types_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_sub_loc_conf_t, types_keys),
      &ngx_http_html_default_types[0] },

    { ngx_string("randomize_filter_once"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_sub_loc_conf_t, once),
      NULL },

    { ngx_string("randomize_filter_last_modified"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_sub_loc_conf_t, last_modified),
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_sub_filter_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_sub_filter_init,              /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_sub_create_conf,              /* create location configuration */
    ngx_http_sub_merge_conf                /* merge location configuration */
};


ngx_module_t  ngx_http_randomize_filter_module = {
    NGX_MODULE_V1,
    &ngx_http_sub_filter_module_ctx,       /* module context */
    ngx_http_sub_filter_commands,          /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_http_output_header_filter_pt  ngx_http_next_header_filter;
static ngx_http_output_body_filter_pt    ngx_http_next_body_filter;


static ngx_int_t
ngx_http_sub_header_filter(ngx_http_request_t *r)
{
    ngx_str_t                *m;
    ngx_uint_t                i, j, n;
    ngx_http_sub_ctx_t       *ctx;
    ngx_http_sub_pair_t      *pairs;
    ngx_http_sub_match_t     *matches;
    ngx_http_sub_loc_conf_t  *slcf;

    slcf = ngx_http_get_module_loc_conf(r, ngx_http_randomize_filter_module);

    if (slcf->pairs == NULL
        || r->headers_out.content_length_n == 0
        || ngx_http_test_content_type(r, &slcf->types) == NULL)
    {
        return ngx_http_next_header_filter(r);
    }

    ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_sub_ctx_t));
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    if (slcf->dynamic == 0) {
        ctx->tables = slcf->tables;
        ctx->matches = slcf->matches;

    } else {
        pairs = slcf->pairs->elts;
        n = slcf->pairs->nelts;

        matches = ngx_pcalloc(r->pool, sizeof(ngx_http_sub_match_t) * n);
        if (matches == NULL) {
            return NGX_ERROR;
        }

        j = 0;
        for (i = 0; i < n; i++) {
            if (pairs[i].match.lengths == NULL) {
                matches[j].match = pairs[i].match.value;
                j++;
                continue;
            }

            m = &matches[j].match;
            if (ngx_http_complex_value(r, &pairs[i].match, m) != NGX_OK) {
                return NGX_ERROR;
            }

            if (m->len == 0) {
                continue;
            }

            ngx_strlow(m->data, m->data, m->len);
            j++;
        }

        if (j == 0) {
            return ngx_http_next_header_filter(r);
        }

        ctx->matches = ngx_palloc(r->pool, sizeof(ngx_array_t));
        if (ctx->matches == NULL) {
            return NGX_ERROR;
        }

        ctx->matches->elts = matches;
        ctx->matches->nelts = j;

        ctx->tables = ngx_palloc(r->pool, sizeof(ngx_http_sub_tables_t));
        if (ctx->tables == NULL) {
            return NGX_ERROR;
        }

        ngx_http_sub_init_tables(ctx->tables, ctx->matches->elts,
                                 ctx->matches->nelts);
    }

    ngx_http_set_ctx(r, ctx, ngx_http_randomize_filter_module);

    ctx->saved.data = ngx_pnalloc(r->pool, ctx->tables->max_match_len - 1);
    if (ctx->saved.data == NULL) {
        return NGX_ERROR;
    }

    ctx->looked.data = ngx_pnalloc(r->pool, ctx->tables->max_match_len - 1);
    if (ctx->looked.data == NULL) {
        return NGX_ERROR;
    }

    ctx->offset = ctx->tables->min_match_len - 1;
    ctx->last_out = &ctx->out;

    r->filter_need_in_memory = 1;

    if (r == r->main) {
        ngx_http_clear_content_length(r);

        if (!slcf->last_modified) {
            ngx_http_clear_last_modified(r);
            ngx_http_clear_etag(r);

        } else {
            ngx_http_weak_etag(r);
        }
    }

    return ngx_http_next_header_filter(r);
}


static ngx_int_t
ngx_http_sub_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_int_t                  rc;
    ngx_int_t                  html;
    ngx_buf_t                 *b;
    ngx_str_t                 *sub;
    ngx_chain_t               *cl;
    ngx_http_sub_ctx_t        *ctx;
    ngx_http_sub_match_t      *match;
    ngx_http_sub_loc_conf_t   *slcf;

    ctx = ngx_http_get_module_ctx(r, ngx_http_randomize_filter_module);

    if (ctx == NULL) {
        return ngx_http_next_body_filter(r, in);
    }

    if ((in == NULL
         && ctx->buf == NULL
         && ctx->in == NULL
         && ctx->busy == NULL))
    {
        return ngx_http_next_body_filter(r, in);
    }

    if (ctx->once && (ctx->buf == NULL || ctx->in == NULL)) {

        if (ctx->busy) {
            if (ngx_http_sub_output(r, ctx) == NGX_ERROR) {
                return NGX_ERROR;
            }
        }

        return ngx_http_next_body_filter(r, in);
    }

    /* add the incoming chain to the chain ctx->in */

    if (in) {
        if (ngx_chain_add_copy(r->pool, &ctx->in, in) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http sub filter \"%V\"", &r->uri);

    html = ngx_strncmp(r->headers_out.content_type.data, "text/html", ngx_strlen("text/html")) == 0;
    if (html) {
      ctx->state = OUTSIDE_TAG;
    }

    while (ctx->in || ctx->buf) {

        if (ctx->buf == NULL) {
            ctx->buf = ctx->in->buf;
            ctx->in = ctx->in->next;
            ctx->pos = ctx->buf->pos;
        }

        b = NULL;

        while (ctx->pos < ctx->buf->last) {

            if (html) {
              rc = ngx_http_sub_parse_html(r, ctx);
            } else {
              rc = ngx_http_sub_parse(r, ctx);
            }

            ngx_log_debug4(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                           "parse: %i, looked: \"%V\" %p-%p",
                           rc, &ctx->looked, ctx->copy_start, ctx->copy_end);

            if (rc == NGX_ERROR) {
                return rc;
            }

            if (ctx->saved.len) {

                ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                               "saved: \"%V\"", &ctx->saved);

                cl = ngx_chain_get_free_buf(r->pool, &ctx->free);
                if (cl == NULL) {
                    return NGX_ERROR;
                }

                b = cl->buf;

                ngx_memzero(b, sizeof(ngx_buf_t));

                b->pos = ngx_pnalloc(r->pool, ctx->saved.len);
                if (b->pos == NULL) {
                    return NGX_ERROR;
                }

                ngx_memcpy(b->pos, ctx->saved.data, ctx->saved.len);
                b->last = b->pos + ctx->saved.len;
                b->memory = 1;

                *ctx->last_out = cl;
                ctx->last_out = &cl->next;

                ctx->saved.len = 0;
            }

            if (ctx->copy_start != ctx->copy_end) {

                cl = ngx_chain_get_free_buf(r->pool, &ctx->free);
                if (cl == NULL) {
                    return NGX_ERROR;
                }

                b = cl->buf;

                ngx_memcpy(b, ctx->buf, sizeof(ngx_buf_t));

                b->pos = ctx->copy_start;
                b->last = ctx->copy_end;
                b->shadow = NULL;
                b->last_buf = 0;
                b->last_in_chain = 0;
                b->recycled = 0;

                if (b->in_file) {
                    b->file_last = b->file_pos + (b->last - ctx->buf->pos);
                    b->file_pos += b->pos - ctx->buf->pos;
                }

                *ctx->last_out = cl;
                ctx->last_out = &cl->next;
            }

            if (rc == NGX_AGAIN) {
                continue;
            }


            /* rc == NGX_OK */

            cl = ngx_chain_get_free_buf(r->pool, &ctx->free);
            if (cl == NULL) {
                return NGX_ERROR;
            }

            b = cl->buf;

            ngx_memzero(b, sizeof(ngx_buf_t));

            slcf = ngx_http_get_module_loc_conf(r, ngx_http_randomize_filter_module);

            if (ctx->sub == NULL) {
                ctx->sub = ngx_pcalloc(r->pool, sizeof(ngx_str_t)
                                                * ctx->matches->nelts);
                if (ctx->sub == NULL) {
                    return NGX_ERROR;
                }
            }

            sub = &ctx->sub[ctx->index];

            if (sub->data == NULL) {
                match = ctx->matches->elts;

                ngx_str_t addr = r->connection->addr_text;
                ngx_str_t mstr = match[ctx->index].match;
                ngx_str_t salt;

                if (slcf->salt == NULL) {
                    salt = (ngx_str_t)ngx_string("");
                } else {
                    if (ngx_http_complex_value(r, slcf->salt, &salt) != NGX_OK) {
                        return NGX_ERROR;
                    }
                }

                sub->len = salt.len + addr.len + mstr.len;
                sub->data = ngx_pnalloc(r->pool, sub->len);
                if (sub->data == NULL) {
                    return NGX_ERROR;
                }

                ngx_memcpy(sub->data, salt.data, salt.len);
                ngx_memcpy(sub->data + salt.len, addr.data, addr.len);
                ngx_memcpy(sub->data + salt.len + addr.len, mstr.data, mstr.len);

                ngx_http_sub_hash(sub);

            }

            if (sub->len) {
                b->memory = 1;
                b->pos = sub->data;
                b->last = sub->data + sub->len;

            } else {
                b->sync = 1;
            }

            *ctx->last_out = cl;
            ctx->last_out = &cl->next;

            ctx->index = 0;
            ctx->once = slcf->once && (++ctx->applied == ctx->matches->nelts);

            continue;
        }

        if (ctx->looked.len
            && (ctx->buf->last_buf || ctx->buf->last_in_chain))
        {
            cl = ngx_chain_get_free_buf(r->pool, &ctx->free);
            if (cl == NULL) {
                return NGX_ERROR;
            }

            b = cl->buf;

            ngx_memzero(b, sizeof(ngx_buf_t));

            b->pos = ctx->looked.data;
            b->last = b->pos + ctx->looked.len;
            b->memory = 1;

            *ctx->last_out = cl;
            ctx->last_out = &cl->next;

            ctx->looked.len = 0;
        }

        if (ctx->buf->last_buf || ctx->buf->flush || ctx->buf->sync
            || ngx_buf_in_memory(ctx->buf))
        {
            if (b == NULL) {
                cl = ngx_chain_get_free_buf(r->pool, &ctx->free);
                if (cl == NULL) {
                    return NGX_ERROR;
                }

                b = cl->buf;

                ngx_memzero(b, sizeof(ngx_buf_t));

                b->sync = 1;

                *ctx->last_out = cl;
                ctx->last_out = &cl->next;
            }

            b->last_buf = ctx->buf->last_buf;
            b->last_in_chain = ctx->buf->last_in_chain;
            b->flush = ctx->buf->flush;
            b->shadow = ctx->buf;

            b->recycled = ctx->buf->recycled;
        }

        ctx->buf = NULL;
    }

    if (ctx->out == NULL && ctx->busy == NULL) {
        return NGX_OK;
    }

    return ngx_http_sub_output(r, ctx);
}


static ngx_int_t
ngx_http_sub_output(ngx_http_request_t *r, ngx_http_sub_ctx_t *ctx)
{
    ngx_int_t     rc;
    ngx_buf_t    *b;
    ngx_chain_t  *cl;

#if 1
    b = NULL;
    for (cl = ctx->out; cl; cl = cl->next) {
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "sub out: %p %p", cl->buf, cl->buf->pos);
        if (cl->buf == b) {
            ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                          "the same buf was used in sub");
            ngx_debug_point();
            return NGX_ERROR;
        }
        b = cl->buf;
    }
#endif

    rc = ngx_http_next_body_filter(r, ctx->out);

    if (ctx->busy == NULL) {
        ctx->busy = ctx->out;

    } else {
        for (cl = ctx->busy; cl->next; cl = cl->next) { /* void */ }
        cl->next = ctx->out;
    }

    ctx->out = NULL;
    ctx->last_out = &ctx->out;

    while (ctx->busy) {

        cl = ctx->busy;
        b = cl->buf;

        if (ngx_buf_size(b) != 0) {
            break;
        }

        if (b->shadow) {
            b->shadow->pos = b->shadow->last;
        }

        ctx->busy = cl->next;

        if (ngx_buf_in_memory(b) || b->in_file) {
            /* add data bufs only to the free buf chain */

            cl->next = ctx->free;
            ctx->free = cl;
        }
    }

    if (ctx->in || ctx->buf) {
        r->buffered |= NGX_HTTP_SUB_BUFFERED;

    } else {
        r->buffered &= ~NGX_HTTP_SUB_BUFFERED;
    }

    return rc;
}


static ngx_int_t
ngx_http_sub_parse(ngx_http_request_t *r, ngx_http_sub_ctx_t *ctx)
{
    u_char                   *p, *last, *pat, *pat_end, c;
    ngx_str_t                *m;
    ngx_int_t                 offset, start, next, end, len, rc;
    ngx_uint_t                shift, i, j;
    ngx_http_sub_match_t     *match;
    ngx_http_sub_tables_t    *tables;
    ngx_http_sub_loc_conf_t  *slcf;

    slcf = ngx_http_get_module_loc_conf(r, ngx_http_randomize_filter_module);
    tables = ctx->tables;

    offset = ctx->offset;
    end = ctx->buf->last - ctx->pos;

    if (ctx->once) {
        /* sets start and next to end */
        offset = end + (ngx_int_t) tables->min_match_len - 1;
        goto again;
    }

    while (offset < end) {

        c = offset < 0 ? ctx->looked.data[ctx->looked.len + offset]
                       : ctx->pos[offset];

        c = ngx_tolower(c);

        shift = tables->shift[c];
        if (shift > 0) {
            offset += shift;
            continue;
        }

        /* a potential match */

        start = offset - (ngx_int_t) tables->min_match_len + 1;
        match = ctx->matches->elts;

        i = ngx_max(tables->index[c], ctx->index);
        j = tables->index[c + 1];

        while (i != j) {

            if (slcf->once && ctx->sub && ctx->sub[i].data) {
                goto next;
            }

            m = &match[i].match;

            pat = m->data;
            pat_end = m->data + m->len;

            if (start >= 0) {
                p = ctx->pos + start;

            } else {
                last = ctx->looked.data + ctx->looked.len;
                p = last + start;

                while (p < last && pat < pat_end) {
                    if (ngx_tolower(*p) != *pat) {
                        goto next;
                    }

                    p++;
                    pat++;
                }

                p = ctx->pos;
            }

            while (p < ctx->buf->last && pat < pat_end) {
                if (ngx_tolower(*p) != *pat) {
                    goto next;
                }

                p++;
                pat++;
            }

            ctx->index = i;

            if (pat != pat_end) {
                /* partial match */
                goto again;
            }

            ctx->offset = offset + (ngx_int_t) m->len;
            next = start + (ngx_int_t) m->len;
            end = ngx_max(next, 0);
            rc = NGX_OK;

            goto done;

        next:

            i++;
        }

        offset++;
        ctx->index = 0;
    }

again:

    ctx->offset = offset;
    start = offset - (ngx_int_t) tables->min_match_len + 1;
    next = start;
    rc = NGX_AGAIN;

done:

    /* send [ - looked.len, start ] to client */

    ctx->saved.len = ctx->looked.len + ngx_min(start, 0);
    ngx_memcpy(ctx->saved.data, ctx->looked.data, ctx->saved.len);

    ctx->copy_start = ctx->pos;
    ctx->copy_end = ctx->pos + ngx_max(start, 0);

    /* save [ next, end ] in looked */

    len = ngx_min(next, 0);
    p = ctx->looked.data;
    p = ngx_movemem(p, p + ctx->looked.len + len, - len);

    len = ngx_max(next, 0);
    p = ngx_cpymem(p, ctx->pos + len, end - len);
    ctx->looked.len = p - ctx->looked.data;

    /* update position */

    ctx->pos += end;
    ctx->offset -= end;

    return rc;
}


static ngx_int_t
ngx_http_sub_parse_html(ngx_http_request_t *r, ngx_http_sub_ctx_t *ctx)
{
    u_char                   *pat, *pat_end, c;
    size_t                    len;
    ngx_str_t                *m;
    ngx_int_t                 rc;
    ngx_uint_t                i, j;
    ngx_http_sub_match_t     *match;

    char attr_name[7] = ""; /* space for 'classa\0' */
    ctx->copy_start = ctx->pos;
    match = ctx->matches->elts;
    j = ctx->matches->nelts;

    while (ctx->pos < ctx->buf->last) {
      c = ngx_tolower(*ctx->pos);

      switch(ctx->state) {
        case OUTSIDE_TAG:
          if (c == '<') {
            ctx->state = TAG_NAME;
          }
          break;
        case TAG_NAME:
          if (c == ' ') {
            ctx->state = READY_FOR_ATTR;
          } else if (c == '>') {
            ctx->state = OUTSIDE_TAG;
          }
          break;
        case READY_FOR_ATTR:
          if (c == ' ') {
            ctx->state = ctx->state;
          } else if (c == '>') {
            ctx->state = OUTSIDE_TAG;
          } else {
            ngx_memset(attr_name, 0, 7);
            attr_name[0] = c;
            ctx->state = ATTR_NAME;
          }
          break;
        case ATTR_NAME:
          if (c == '=') {
            ctx->state = READY_FOR_VAL;
          } else {
            len = ngx_strlen(attr_name);
            if (len < 6) {
              attr_name[len] = c;
            }
          }
          break;
        case READY_FOR_VAL:
          if (c == '"') {
            ctx->state = ATTR_VALUE;
          }
          break;
        case ATTR_VALUE:
          if (c == '"') {
            ctx->state = READY_FOR_ATTR;
          }
          break;
      }

      if (ctx->state == ATTR_VALUE && (!ngx_strcmp("class", attr_name) || !ngx_strcmp("id", attr_name))) {
        for (i = 0; i < j; ++i) {
          m = &match[i].match;
          pat = m->data;
          if (*pat == c) {
            u_char *pos = ctx->pos;
            pat_end = m->data + m->len;
            do {
              pat++;
              pos++;
              if (pat == pat_end) {
                ctx->copy_end = ctx->pos;
                ctx->pos = pos;
                ctx->index = i;
                rc = NGX_OK; // to copy match
                goto done;
              }
            } while (pos < ctx->buf->last && *pat == ngx_tolower(*pos));
          }
        }
      }
      ctx->pos++;
    }

    ctx->copy_end = ctx->pos;
    rc = NGX_AGAIN; // to NOT copy match
done:
    ctx->saved.len = 0;

    return rc;
}


static void
ngx_http_sub_hash(ngx_str_t *sub)
{
    unsigned long hash = 5381;
    u_char *str = sub->data;
    int c;

    /* available chars: A(65)-Z(90) and a(97)-z(122) */
    /********* max  = 116 // 'z' offset by -6 */
    static int min  = 65; // 'A'
    static int diff = 51; // max - min

    for(size_t i = 0; i < sub->len; ++str, ++i) {
        c = *str;
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
        c = (hash%diff) + min; /* get number between 65 and 116 */
        if (c > 90) { /* skipping 91-96 */
            c += 6; /* adjust for offset */
        }
        *str = c;
    }
}


static char *
ngx_http_sub_filter(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_sub_loc_conf_t *slcf = conf;

    ngx_str_t                         *value;
    ngx_http_sub_pair_t               *pair;
    ngx_http_compile_complex_value_t   ccv;

    value = cf->args->elts;

    if (value[1].len == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "empty search pattern");
        return NGX_CONF_ERROR;
    }

    if (slcf->pairs == NULL) {
        slcf->pairs = ngx_array_create(cf->pool, 1,
                                       sizeof(ngx_http_sub_pair_t));
        if (slcf->pairs == NULL) {
            return NGX_CONF_ERROR;
        }
    }

    if (slcf->pairs->nelts == 255) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "number of search patterns exceeds 255");
        return NGX_CONF_ERROR;
    }

    ngx_strlow(value[1].data, value[1].data, value[1].len);

    pair = ngx_array_push(slcf->pairs);
    if (pair == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

    ccv.cf = cf;
    ccv.value = &value[1];
    ccv.complex_value = &pair->match;

    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    if (ccv.complex_value->lengths != NULL) {
        slcf->dynamic = 1;

    } else {
        ngx_strlow(pair->match.value.data, pair->match.value.data,
                   pair->match.value.len);
    }

    return NGX_CONF_OK;
}


static char *
ngx_http_sub_filter_salt(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_sub_loc_conf_t *slcf = conf;

    ngx_str_t                         *value;
    ngx_http_compile_complex_value_t   ccv;

    value = cf->args->elts;

    if (value[1].len == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "empty hash salt");
        return NGX_CONF_ERROR;
    }

    if (slcf->salt != NULL) {
      ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "hash salt is already defined");
      return NGX_CONF_ERROR;
    }

    slcf->salt = ngx_palloc(cf->pool, sizeof(ngx_http_complex_value_t));
    if(slcf->salt == NULL) {
      return NGX_CONF_ERROR;
    }

    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

    ccv.cf = cf;
    ccv.value = &value[1];
    ccv.complex_value = slcf->salt;

    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


static void *
ngx_http_sub_create_conf(ngx_conf_t *cf)
{
    ngx_http_sub_loc_conf_t  *slcf;

    slcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_sub_loc_conf_t));
    if (slcf == NULL) {
        return NULL;
    }

    /*
     * set by ngx_pcalloc():
     *
     *     conf->dynamic = 0;
     *     conf->pairs = NULL;
     *     conf->tables = NULL;
     *     conf->types = { NULL };
     *     conf->types_keys = NULL;
     *     conf->matches = NULL;
     */

    slcf->once = NGX_CONF_UNSET;
    slcf->last_modified = NGX_CONF_UNSET;

    return slcf;
}


static char *
ngx_http_sub_merge_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_uint_t                i, n;
    ngx_http_sub_pair_t      *pairs;
    ngx_http_sub_match_t     *matches;
    ngx_http_sub_loc_conf_t  *prev = parent;
    ngx_http_sub_loc_conf_t  *conf = child;

    ngx_conf_merge_value(conf->once, prev->once, 1);
    ngx_conf_merge_value(conf->last_modified, prev->last_modified, 0);

    if (ngx_http_merge_types(cf, &conf->types_keys, &conf->types,
                             &prev->types_keys, &prev->types,
                             ngx_http_html_default_types)
        != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

    if (conf->pairs == NULL) {
        conf->dynamic = prev->dynamic;
        conf->pairs = prev->pairs;
        conf->matches = prev->matches;
        conf->tables = prev->tables;
    }

    if (conf->pairs && conf->dynamic == 0 && conf->tables == NULL) {
        pairs = conf->pairs->elts;
        n = conf->pairs->nelts;

        matches = ngx_palloc(cf->pool, sizeof(ngx_http_sub_match_t) * n);
        if (matches == NULL) {
            return NGX_CONF_ERROR;
        }

        for (i = 0; i < n; i++) {
            matches[i].match = pairs[i].match.value;
        }

        conf->matches = ngx_palloc(cf->pool, sizeof(ngx_array_t));
        if (conf->matches == NULL) {
            return NGX_CONF_ERROR;
        }

        conf->matches->elts = matches;
        conf->matches->nelts = n;

        conf->tables = ngx_palloc(cf->pool, sizeof(ngx_http_sub_tables_t));
        if (conf->tables == NULL) {
            return NGX_CONF_ERROR;
        }

        ngx_http_sub_init_tables(conf->tables, conf->matches->elts,
                                 conf->matches->nelts);
    }

    return NGX_CONF_OK;
}


static void
ngx_http_sub_init_tables(ngx_http_sub_tables_t *tables,
    ngx_http_sub_match_t *match, ngx_uint_t n)
{
    u_char      c;
    ngx_uint_t  i, j, min, max, ch;

    min = match[0].match.len;
    max = match[0].match.len;

    for (i = 1; i < n; i++) {
        min = ngx_min(min, match[i].match.len);
        max = ngx_max(max, match[i].match.len);
    }

    tables->min_match_len = min;
    tables->max_match_len = max;

    ngx_http_sub_cmp_index = tables->min_match_len - 1;
    ngx_sort(match, n, sizeof(ngx_http_sub_match_t), ngx_http_sub_cmp_matches);

    min = ngx_min(min, 255);
    ngx_memset(tables->shift, min, 256);

    ch = 0;

    for (i = 0; i < n; i++) {

        for (j = 0; j < min; j++) {
            c = match[i].match.data[tables->min_match_len - 1 - j];
            tables->shift[c] = ngx_min(tables->shift[c], (u_char) j);
        }

        c = match[i].match.data[tables->min_match_len - 1];
        while (ch <= c) {
            tables->index[ch++] = (u_char) i;
        }
    }

    while (ch < 257) {
        tables->index[ch++] = (u_char) n;
    }
}


static ngx_int_t
ngx_http_sub_cmp_matches(const void *one, const void *two)
{
    ngx_int_t              c1, c2;
    ngx_http_sub_match_t  *first, *second;

    first = (ngx_http_sub_match_t *) one;
    second = (ngx_http_sub_match_t *) two;

    c1 = first->match.data[ngx_http_sub_cmp_index];
    c2 = second->match.data[ngx_http_sub_cmp_index];

    return c1 - c2;
}


static ngx_int_t
ngx_http_sub_filter_init(ngx_conf_t *cf)
{
    ngx_http_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_http_sub_header_filter;

    ngx_http_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ngx_http_sub_body_filter;

    return NGX_OK;
}
