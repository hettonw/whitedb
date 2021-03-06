/*
* $Id:  $
* $Version: $
*
* Copyright (c) Priit J�rv 2013
*
* This file is part of WhiteDB
*
* WhiteDB is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
* 
* WhiteDB is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
* 
* You should have received a copy of the GNU General Public License
* along with WhiteDB.  If not, see <http://www.gnu.org/licenses/>.
*
*/

 /** @file dbjson.c
 * WhiteDB JSON input and output.
 */

/* ====== Includes =============== */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>


/* ====== Private headers and defs ======== */

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#include "../config-w32.h"
#else
#include "../config.h"
#endif

#include "dbdata.h"
#include "dbcompare.h"
#include "dbschema.h"
#include "dbjson.h"
#include "dbutil.h"
#include "../json/JSON_parser.h"

#ifdef _WIN32
#define snprintf(s, sz, f, ...) _snprintf_s(s, sz+1, sz, f, ## __VA_ARGS__)
#define strncpy(d, s, sz) strncpy_s(d, sz+1, s, sz)
#endif

#ifdef USE_BACKLINKING
#if !defined(WG_COMPARE_REC_DEPTH) || (WG_COMPARE_REC_DEPTH < 2)
#error WG_COMPARE_REC_DEPTH not defined or too small
#else
#define MAX_DEPTH WG_COMPARE_REC_DEPTH
#endif
#else /* !USE_BACKLINKING */
#define MAX_DEPTH 99 /* no reason to limit */
#endif

typedef enum { ARRAY, OBJECT } stack_entry_t;

struct __stack_entry_elem {
  gint enc;
  struct __stack_entry_elem *next;
};

typedef struct __stack_entry_elem stack_entry_elem;

typedef struct {
  stack_entry_t type;
  stack_entry_elem *head;
  stack_entry_elem *tail;
  char last_key[80];
  int size;
} stack_entry;

typedef struct {
  int state;
  stack_entry stack[MAX_DEPTH];
  int stack_ptr;
  void *db;
  int isparam;
  void **document;
} parser_context;

/* ======== Data ========================= */

/* ======= Private protos ================ */

static int push(parser_context *ctx, stack_entry_t type);
static int pop(parser_context *ctx);
static int add_elem(parser_context *ctx, gint enc);
static int add_key(parser_context *ctx, char *key);
static int add_literal(parser_context *ctx, gint val);

static gint run_json_parser(void *db, char *buf,
  JSON_parser_callback cb, int isparam, void **document);
static int parse_json_cb(void* ctx, int type, const JSON_value* value);
static int pretty_print_json(void *db, FILE *f, void *rec,
  int indent, int comma, int newline);

static gint show_json_error(void *db, char *errmsg);
static gint show_json_error_fn(void *db, char *errmsg, char *filename);
static gint show_json_error_byte(void *db, char *errmsg, int byte);

/* ====== Functions ============== */

/**
 * Parse an input file. Does an initial pass to verify the syntax
 * of the input and passes it on to the document parser.
 * XXX: caches the data in memory, so this is very unsuitable
 * for large files. An alternative would be to feed bytes directly
 * to the document parser and roll the transaction back, if something fails;
 */
#define WG_JSON_INPUT_CHUNK 16384

gint wg_parse_json_file(void *db, char *filename) {
  char *buf = NULL;
  FILE *f = NULL;
  int next_char, count = 0, result = 0, bufsize = 0;
  JSON_config config;
  struct JSON_parser_struct* jc = NULL;

  buf = malloc(WG_JSON_INPUT_CHUNK);
  if(!buf) {
    return show_json_error(db, "Failed to allocate memory");
  }
  bufsize = WG_JSON_INPUT_CHUNK;

  if(!filename) {
    printf("reading JSON from stdin, press CTRL-D when done\n");
    fflush(stdout);
    f = stdin;
  } else {
    f = fopen(filename, "r");
    if(!f) {
      show_json_error_fn(db, "Failed to open input", filename);
      result = -1;
      goto done;
    }
  }

  /* setup parser */
  init_JSON_config(&config);
  config.depth = MAX_DEPTH - 1;
  config.callback = NULL;
  config.callback_ctx = NULL;
  config.allow_comments = 1;
  config.handle_floats_manually = 0;
  jc = new_JSON_parser(&config);

  while((next_char = fgetc(f)) != EOF) {
    if(!JSON_parser_char(jc, next_char)) {
      show_json_error_byte(db, "Syntax error", count);
      result = -1;
      goto done;
    }
    buf[count] = (char) next_char;
    if(++count >= bufsize) {
      void *tmp = realloc(buf, bufsize + WG_JSON_INPUT_CHUNK);
      if(!tmp) {
        show_json_error(db, "Failed to allocate additional memory");
        result = -1;
        goto done;
      }
      buf = tmp;
      bufsize += WG_JSON_INPUT_CHUNK;
    }
  }
  if(!JSON_parser_done(jc)) {
    show_json_error(db, "Syntax error (JSON not properly terminated?)");
    result = -1;
    goto done;
  }

  buf[count] = '\0';
  result = wg_parse_json_document(db, buf);

done:
  if(buf) free(buf);
  if(filename && f) fclose(f);
  if(jc) delete_JSON_parser(jc);
  return result;
}
 
/* Parse a JSON buffer.
 * The data is inserted in database using the JSON schema.
 *
 * returns 0 for success.
 * returns -1 on non-fatal error.
 * returns -2 if database is left non-consistent due to an error.
 */
gint wg_parse_json_document(void *db, char *buf) {
  void *document = NULL; /* ignore */
  return run_json_parser(db, buf, &parse_json_cb, 0, &document);
}

/* Parse a JSON parameter(s).
 * The data is inserted in database as "special" records.
 *
 * returns 0 for success.
 * returns -1 on non-fatal error.
 * returns -2 if database is left non-consistent due to an error.
 */
gint wg_parse_json_param(void *db, char *buf, void **document) {
  return run_json_parser(db, buf, &parse_json_cb, 1, document);
}

/* Run JSON parser.
 * The data is inserted in the database. If there are any errors, the
 * database will currently remain in an inconsistent state, so beware.
 *
 * if isparam is specified, the data will not be indexed nor returned
 * by wg_get_*_record() calls.
 *
 * if the call is successful, *document contains a pointer to the
 * top-level record.
 *
 * returns 0 for success.
 * returns -1 on non-fatal error.
 * returns -2 if database is left non-consistent due to an error.
 */
static gint run_json_parser(void *db, char *buf,
  JSON_parser_callback cb, int isparam, void **document)
{
  int next_char, count = 0, result = 0;
  JSON_config config;
  struct JSON_parser_struct* jc = NULL;
  char *iptr = buf;
  parser_context ctx;

  init_JSON_config(&config);

  /* setup context */
  ctx.state = 0;
  ctx.stack_ptr = -1;
  ctx.db = db;
  ctx.isparam = isparam;
  ctx.document = document;

  /* setup parser */
  config.depth = MAX_DEPTH - 1;
  config.callback = cb;
  config.callback_ctx = &ctx;
  config.allow_comments = 1;
  config.handle_floats_manually = 0;

  jc = new_JSON_parser(&config);

  while((next_char = *iptr++)) {
    if(!JSON_parser_char(jc, next_char)) {
      show_json_error_byte(db, "JSON parsing failed", count);
      result = -2; /* Fatal error */
      goto done;
    }
    count++;
  }
  if(!JSON_parser_done(jc)) {
    show_json_error(db, "JSON parsing failed");
    result = -2; /* Fatal error */
    goto done;
  }

done:
  delete_JSON_parser(jc);
  return result;
}

/**
 * Push an object or an array on the stack.
 */
static int push(parser_context *ctx, stack_entry_t type)
{
  stack_entry *e;
  if(++ctx->stack_ptr >= MAX_DEPTH) /* paranoia, parser guards from this */
    return 0;
  e = &ctx->stack[ctx->stack_ptr];
  e->size = 0;
  e->type = type;
  e->head = NULL;
  e->tail = NULL;
  return 1;
}

/**
 * Pop an object or an array from the stack.
 * If this is not the top level in the document, the object is also added
 * as an element on the previous level.
 */
static int pop(parser_context *ctx)
{
  stack_entry *e;
  void *rec;
  int ret, istoplevel;

  if(ctx->stack_ptr < 0)
    return 0;
  e = &ctx->stack[ctx->stack_ptr--];

  /* is it a top level object? */
  if(ctx->stack_ptr < 0) {
    istoplevel = 1;
  } else {
    istoplevel = 0;
  }
    
  if(e->type == ARRAY) {
    rec = wg_create_array(ctx->db, e->size, istoplevel, ctx->isparam);
  } else {
    rec = wg_create_object(ctx->db, e->size, istoplevel, ctx->isparam);
  }

  /* add elements to the database */
  if(rec) {
    stack_entry_elem *curr = e->head;
    int i = 0;
    ret = 1;
    while(curr) {
      if(wg_set_field(ctx->db, rec, i++, curr->enc)) {
        ret = 0;
        break;
      }
      curr = curr->next;
    }
    if(istoplevel)
      *(ctx->document) = rec;
  } else {
    ret = 0;
  }
    
  /* free the elements */
  while(e->head) {
    stack_entry_elem *tmp = e->head;
    e->head = e->head->next;
    free(tmp);
  }
  e->tail = NULL;
  e->size = 0;
  
  /* is it an element of something? */
  if(!istoplevel && rec && ret) {
    gint enc = wg_encode_record(ctx->db, rec);
    ret = add_literal(ctx, enc);
  }
  return ret;
}

/**
 * Append an element to the current stack entry.
 */
static int add_elem(parser_context *ctx, gint enc)
{
  stack_entry *e;
  stack_entry_elem *tmp;

  if(ctx->stack_ptr < 0 || ctx->stack_ptr >= MAX_DEPTH)
    return 0; /* paranoia */

  e = &ctx->stack[ctx->stack_ptr];
  tmp = (stack_entry_elem *) malloc(sizeof(stack_entry_elem));
  if(!tmp)
    return 0;

  if(!e->tail) {
    e->head = tmp;
  } else {
    e->tail->next = tmp;
  }
  e->tail = tmp;
  e->size++;
  tmp->enc = enc;
  tmp->next = NULL;
  return 1;
}

/**
 * Store a key in the current stack entry.
 */
static int add_key(parser_context *ctx, char *key)
{
  stack_entry *e;

  if(ctx->stack_ptr < 0 || ctx->stack_ptr >= MAX_DEPTH)
    return 0; /* paranoia */

  e = &ctx->stack[ctx->stack_ptr];
  strncpy(e->last_key, key, 80);
  e->last_key[79] = '\0';
  return 1;
}

/**
 * Add a literal value. If it's inside an object, generate
 * a key-value pair using the last key. Otherwise insert
 * it directly.
 */
static int add_literal(parser_context *ctx, gint val)
{
  stack_entry *e;

  if(ctx->stack_ptr < 0 || ctx->stack_ptr >= MAX_DEPTH)
    return 0; /* paranoia */

  e = &ctx->stack[ctx->stack_ptr];
  if(e->type == ARRAY) {
    return add_elem(ctx, val);
  } else {
    void *rec;
    gint key = wg_encode_str(ctx->db, e->last_key, NULL);
    if(key == WG_ILLEGAL)
      return 0;
    rec = wg_create_kvpair(ctx->db, key, val, ctx->isparam);
    if(!rec)
      return 0;
    return add_elem(ctx, wg_encode_record(ctx->db, rec));
  }
}

#define OUT_INDENT(x,i,f) \
      for(i=0; i<x; i++) \
        fprintf(f, "  ");

static int parse_json_cb(void* cb_ctx, int type, const JSON_value* value)
{
/*  int i;*/
  gint val;
  parser_context *ctx = (parser_context *) cb_ctx;

  switch(type) {
    case JSON_T_ARRAY_BEGIN:
/*      OUT_INDENT(ctx->stack_ptr+1, i, stdout)
      printf("BEGIN ARRAY\n");*/
      if(!push(ctx, ARRAY))
        return 0;
      break;
    case JSON_T_ARRAY_END:
      if(!pop(ctx))
        return 0;
/*      OUT_INDENT(ctx->stack_ptr+1, i, stdout)
      printf("END ARRAY\n");*/
      break;
    case JSON_T_OBJECT_BEGIN:
/*      OUT_INDENT(ctx->stack_ptr+1, i, stdout)
      printf("BEGIN object\n");*/
      if(!push(ctx, OBJECT))
        return 0;
      break;
    case JSON_T_OBJECT_END:
      if(!pop(ctx))
        return 0;
/*      OUT_INDENT(ctx->stack_ptr+1, i, stdout)
      printf("END object\n");*/
      break;
    case JSON_T_INTEGER:
      val = wg_encode_int(ctx->db, value->vu.integer_value);
      if(val == WG_ILLEGAL)
        return 0;
      if(!add_literal(ctx, val))
        return 0;
/*      OUT_INDENT(ctx->stack_ptr+1, i, stdout)
      printf("INTEGER: %d\n", value->vu.integer_value);*/
      break;
    case JSON_T_FLOAT:
      val = wg_encode_double(ctx->db, value->vu.float_value);
      if(val == WG_ILLEGAL)
        return 0;
      if(!add_literal(ctx, val))
        return 0;
/*      OUT_INDENT(ctx->stack_ptr+1, i, stdout)
      printf("FLOAT: %.6f\n", value->vu.float_value);*/
      break;
    case JSON_T_KEY:
      if(!add_key(ctx, (char *) value->vu.str.value))
        return 0;
/*      OUT_INDENT(ctx->stack_ptr+1, i, stdout)
      printf("KEY: %s\n", value->vu.str.value);*/
      break;
    case JSON_T_STRING:
      val = wg_encode_str(ctx->db, (char *) value->vu.str.value, NULL);
      if(val == WG_ILLEGAL)
        return 0;
      if(!add_literal(ctx, val))
        return 0;
/*      OUT_INDENT(ctx->stack_ptr+1, i, stdout)
      printf("STRING: %s\n", value->vu.str.value);*/
      break;
    default:
      break;
  }
  
  return 1;
}

/*
 * Print a JSON document into the given stream.
 */
void wg_print_json_document(void *db, FILE *f, void *document) {
  if(!is_schema_document(document)) {
    /* Paranoia check. This increases the probability we're dealing
     * with records belonging to a proper schema. Omitting this check
     * would allow printing parts of documents as well.
     */
    show_json_error(db, "Given record is not a document");
    return;
  }
  pretty_print_json(db, f, document, 0, 0, 1);
}

/*
 * Recursively print JSON elements (using the JSON schema)
 * Returns 0 on success
 * Returns -1 on error.
 */
static int pretty_print_json(void *db, FILE *f, void *rec,
  int indent, int comma, int newline)
{
  if(is_schema_object(rec)) {
    gint i, reclen;

    /*OUT_INDENT(indent, i, f);*/
    fprintf(f, "%s{\n", (comma ? "," : ""));

    reclen = wg_get_record_len(db, rec);
    for(i=0; i<reclen; i++) {
      gint enc;
      enc = wg_get_field(db, rec, i);
      if(wg_get_encoded_type(db, enc) != WG_RECORDTYPE) {
        return show_json_error(db, "Object had an element of invalid type");
      }
      if(pretty_print_json(db, f, wg_decode_record(db, enc), indent+1, i, 1)) {
        return -1;
      }
    }
    
    OUT_INDENT(indent, i, f);
    fprintf(f, "}%s", (newline ? "\n" : ""));
  }
  else if(is_schema_array(rec)) {
    gint i, reclen;

    fprintf(f, "%s[", (comma ? "," : ""));

    reclen = wg_get_record_len(db, rec);
    for(i=0; i<reclen; i++) {
      gint enc, type;
      enc = wg_get_field(db, rec, i);
      type = wg_get_encoded_type(db, enc);
      if(type == WG_RECORDTYPE) {
        if(pretty_print_json(db, f, wg_decode_record(db, enc),
          indent, i, 0)) {
          return -1;
        }
      } else if(type == WG_STRTYPE) {
        fprintf(f, "%s\"%s\"", (i ? "," : ""), wg_decode_str(db, enc));
      } else {
        /* other literal value */
        char buf[80];
        wg_snprint_value(db, enc, buf, 79);
        fprintf(f, "%s%s", (i ? "," : ""), buf);
      }
    }
    
    fprintf(f, "]%s", (newline ? "\n" : ""));
  }
  else {
    /* assume key-value pair */
    gint i, key, value, valtype;
    key = wg_get_field(db, rec, WG_SCHEMA_KEY_OFFSET);
    value = wg_get_field(db, rec, WG_SCHEMA_VALUE_OFFSET);

    if(wg_get_encoded_type(db, key) != WG_STRTYPE) {
      return show_json_error(db, "Key is of invalid type");
    } else {
      OUT_INDENT(indent, i, f);
      fprintf(f, "%s\"%s\": ", (comma ? "," : ""), wg_decode_str(db, key));
    }

    valtype = wg_get_encoded_type(db, value);
    if(valtype == WG_RECORDTYPE) {
      if(pretty_print_json(db, f, wg_decode_record(db, value),
        indent, 0, 1)) {
        return -1;
      }
    } else if(valtype == WG_STRTYPE) {
      fprintf(f, "\"%s\"\n", wg_decode_str(db, value));
    } else {
      /* other literal value */
      char buf[80];
      wg_snprint_value(db, value, buf, 79);
      fprintf(f, "%s\n", buf);
    }
  }
  return 0;
}


/* ------------ error handling ---------------- */

static gint show_json_error(void *db, char *errmsg) {
#ifdef WG_NO_ERRPRINT
#else   
  fprintf(stderr,"wg json I/O error: %s.\n", errmsg);
#endif  
  return -1;
}

static gint show_json_error_fn(void *db, char *errmsg, char *filename) {
#ifdef WG_NO_ERRPRINT
#else   
  fprintf(stderr,"wg json I/O error: %s (file=`%s`)\n", errmsg, filename);
#endif  
  return -1;
}

static gint show_json_error_byte(void *db, char *errmsg, int byte) {
#ifdef WG_NO_ERRPRINT
#else   
  fprintf(stderr,"wg json I/O error: %s (byte=%d)\n", errmsg, byte);
#endif  
  return -1;
}

#ifdef __cplusplus
}
#endif
