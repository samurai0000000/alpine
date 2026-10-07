/*
 * ========================================================================
 * Copyright (C) 2026, Charles Chiou
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * ========================================================================
 */

/*
 * Idempotency records, plan section 2.8.
 *
 * One file per key under $XDG_STATE_HOME/alpine-mcp/idempotency, or
 * $HOME/.local/state/alpine-mcp/idempotency.  Directory mode 0700, files
 * mode 0600.  A record holds the operation name, the SHA-256 of the
 * canonical arguments, a state ("pending" or "done"), and, when done,
 * the result text.
 *
 * The caller writes "pending" (with fsync) before the effect, and
 * replaces it with "done" after.  A retry of the same key and the same
 * arguments gets the stored result and must not repeat the effect.  The
 * same key with different arguments, or a record still "pending", is
 * refused.  The monitor screen can delete one pending record; that is
 * the only recovery.
 *
 * Main thread only.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "mcpjson.h"
#include "mcp.h"
#include "mcpint.h"

#define IDEM_MAX_FILE	(1024 * 1024)


/*----------------------------------------------------------------------
    SHA-256 (FIPS 180-4).  Small and self-contained so that the pith
    library does not gain a link dependency.
  ----*/
typedef struct {
    unsigned long  h[8];
    unsigned char  buf[64];
    unsigned long long total;
    size_t	   used;
} SHA256_CTX_S;

static const unsigned long K256[64] = {
    0x428a2f98UL,0x71374491UL,0xb5c0fbcfUL,0xe9b5dba5UL,0x3956c25bUL,
    0x59f111f1UL,0x923f82a4UL,0xab1c5ed5UL,0xd807aa98UL,0x12835b01UL,
    0x243185beUL,0x550c7dc3UL,0x72be5d74UL,0x80deb1feUL,0x9bdc06a7UL,
    0xc19bf174UL,0xe49b69c1UL,0xefbe4786UL,0x0fc19dc6UL,0x240ca1ccUL,
    0x2de92c6fUL,0x4a7484aaUL,0x5cb0a9dcUL,0x76f988daUL,0x983e5152UL,
    0xa831c66dUL,0xb00327c8UL,0xbf597fc7UL,0xc6e00bf3UL,0xd5a79147UL,
    0x06ca6351UL,0x14292967UL,0x27b70a85UL,0x2e1b2138UL,0x4d2c6dfcUL,
    0x53380d13UL,0x650a7354UL,0x766a0abbUL,0x81c2c92eUL,0x92722c85UL,
    0xa2bfe8a1UL,0xa81a664bUL,0xc24b8b70UL,0xc76c51a3UL,0xd192e819UL,
    0xd6990624UL,0xf40e3585UL,0x106aa070UL,0x19a4c116UL,0x1e376c08UL,
    0x2748774cUL,0x34b0bcb5UL,0x391c0cb3UL,0x4ed8aa4aUL,0x5b9cca4fUL,
    0x682e6ff3UL,0x748f82eeUL,0x78a5636fUL,0x84c87814UL,0x8cc70208UL,
    0x90befffaUL,0xa4506cebUL,0xbef9a3f7UL,0xc67178f2UL
};

#define ROR(x,n)  ((((x) & 0xffffffffUL) >> (n)) | (((x) << (32 - (n))) & 0xffffffffUL))

static void
sha_block(SHA256_CTX_S *c, const unsigned char *p)
{
    unsigned long w[64], a, b, cc, d, e, f, g, h, t1, t2;
    int		  i;

    for(i = 0; i < 16; i++)
      w[i] = ((unsigned long) p[i*4] << 24) | ((unsigned long) p[i*4+1] << 16)
	     | ((unsigned long) p[i*4+2] << 8) | (unsigned long) p[i*4+3];

    for(i = 16; i < 64; i++){
	unsigned long s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18)
			   ^ ((w[i-15] & 0xffffffffUL) >> 3);
	unsigned long s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19)
			   ^ ((w[i-2] & 0xffffffffUL) >> 10);

	w[i] = (w[i-16] + s0 + w[i-7] + s1) & 0xffffffffUL;
    }

    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];

    for(i = 0; i < 64; i++){
	unsigned long S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
	unsigned long ch = (e & f) ^ ((~e) & g);
	unsigned long S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
	unsigned long mj = (a & b) ^ (a & cc) ^ (b & cc);

	t1 = (h + S1 + ch + K256[i] + w[i]) & 0xffffffffUL;
	t2 = (S0 + mj) & 0xffffffffUL;
	h = g; g = f; f = e; e = (d + t1) & 0xffffffffUL;
	d = cc; cc = b; b = a; a = (t1 + t2) & 0xffffffffUL;
    }

    c->h[0] = (c->h[0] + a) & 0xffffffffUL;
    c->h[1] = (c->h[1] + b) & 0xffffffffUL;
    c->h[2] = (c->h[2] + cc) & 0xffffffffUL;
    c->h[3] = (c->h[3] + d) & 0xffffffffUL;
    c->h[4] = (c->h[4] + e) & 0xffffffffUL;
    c->h[5] = (c->h[5] + f) & 0xffffffffUL;
    c->h[6] = (c->h[6] + g) & 0xffffffffUL;
    c->h[7] = (c->h[7] + h) & 0xffffffffUL;
}


static void
sha_init(SHA256_CTX_S *c)
{
    static const unsigned long iv[8] = {
	0x6a09e667UL,0xbb67ae85UL,0x3c6ef372UL,0xa54ff53aUL,
	0x510e527fUL,0x9b05688cUL,0x1f83d9abUL,0x5be0cd19UL
    };

    memcpy(c->h, iv, sizeof(iv));
    c->total = 0;
    c->used  = 0;
}


static void
sha_update(SHA256_CTX_S *c, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *) data;

    c->total += len;
    while(len){
	size_t n = 64 - c->used;

	if(n > len)
	  n = len;

	memcpy(c->buf + c->used, p, n);
	c->used += n;
	p += n;
	len -= n;
	if(c->used == 64){
	    sha_block(c, c->buf);
	    c->used = 0;
	}
    }
}


static void
sha_final(SHA256_CTX_S *c, unsigned char out[32])
{
    unsigned long long bits = c->total * 8ULL;
    unsigned char      pad = 0x80, zero = 0, lenb[8];
    int		       i;

    sha_update(c, &pad, 1);
    while(c->used != 56)
      sha_update(c, &zero, 1);

    for(i = 0; i < 8; i++)
      lenb[i] = (unsigned char) (bits >> (56 - 8 * i));

    sha_update(c, lenb, 8);
    for(i = 0; i < 8; i++){
	out[i*4]   = (unsigned char) (c->h[i] >> 24);
	out[i*4+1] = (unsigned char) (c->h[i] >> 16);
	out[i*4+2] = (unsigned char) (c->h[i] >> 8);
	out[i*4+3] = (unsigned char) c->h[i];
    }
}


/* hex[65] */
void
mcp_sha256_hex(const void *data, size_t len, char *hex)
{
    SHA256_CTX_S c;
    unsigned char d[32];
    int		  i;

    sha_init(&c);
    sha_update(&c, data, len);
    sha_final(&c, d);
    for(i = 0; i < 32; i++)
      snprintf(hex + i * 2, 3, "%02x", d[i]);
}


/*----------------------------------------------------------------------
    Record directory
  ----*/
static int
idem_mkdirs(char *path)
{
    char *p;

    for(p = path + 1; *p; p++)
      if(*p == '/'){
	  *p = '\0';
	  if(mkdir(path, 0700) < 0 && errno != EEXIST){
	      *p = '/';
	      return(-1);
	  }

	  *p = '/';
      }

    if(mkdir(path, 0700) < 0 && errno != EEXIST)
      return(-1);

    return(chmod(path, 0700));
}


static int
idem_dir(char *buf, size_t len)
{
    char *x = getenv("XDG_STATE_HOME"), *h = getenv("HOME");
    int	  n;

    if(x && *x == '/')
      n = snprintf(buf, len, "%s/alpine-mcp/idempotency", x);
    else if(h && *h == '/')
      n = snprintf(buf, len, "%s/.local/state/alpine-mcp/idempotency", h);
    else
      return(-1);

    if(n < 0 || (size_t) n >= len)
      return(-1);

    return(idem_mkdirs(buf));
}


/*
 * A key becomes a file name.  The tools only pass keys of letters, digits
 * and ". _ -", but the store does not rely on that: nothing that could
 * name another place is accepted.
 */
static int
idem_key_ok(const char *key)
{
    return(key != NULL && *key != '\0' && strchr(key, '/') == NULL
	   && strcmp(key, ".") != 0 && strcmp(key, "..") != 0);
}


static int
idem_path(const char *key, char *buf, size_t len)
{
    char dir[1024];

    if(!idem_key_ok(key) || idem_dir(dir, sizeof(dir)) < 0)
      return(-1);

    if(snprintf(buf, len, "%s/%s", dir, key) >= (int) len)
      return(-1);

    return(0);
}


static void
idem_sync_dir(void)
{
    char dir[1024];
    int  fd;

    if(idem_dir(dir, sizeof(dir)) == 0 && (fd = open(dir, O_RDONLY)) >= 0){
	(void) fsync(fd);
	close(fd);
    }
}


/* write the whole text, fsync, close.  0 on success. */
static int
idem_write(int fd, const char *text)
{
    size_t left = strlen(text);
    ssize_t n;

    while(left){
	if((n = write(fd, text, left)) < 0){
	    if(errno == EINTR)
	      continue;

	    return(-1);
	}

	text += n;
	left -= n;
    }

    return(fsync(fd));
}


static char *
idem_record_text(const char *op, const char *hash, const char *state,
		 const char *result)
{
    cJSON *o = cJSON_CreateObject();
    char  *t = NULL;

    /*
     * Every member must be there: a "done" record without its result, or
     * any record without its hash, would make a later retry look like a
     * mismatch for good.  On any failure there is no record text at all.
     */
    if(o != NULL
       && cJSON_AddStringToObject(o, "op", op) != NULL
       && cJSON_AddStringToObject(o, "hash", hash) != NULL
       && cJSON_AddStringToObject(o, "state", state) != NULL
       && (result == NULL
	   || cJSON_AddStringToObject(o, "result", result) != NULL))
      t = cJSON_PrintUnformatted(o);

    if(o != NULL)
      cJSON_Delete(o);

    return(t);
}


/* whole file, NUL-terminated, malloc'd; NULL on error */
static char *
idem_read(const char *path)
{
    struct stat st;
    char       *b;
    int		fd;
    ssize_t	n, got = 0;

    if((fd = open(path, O_RDONLY)) < 0)
      return(NULL);

    if(fstat(fd, &st) < 0 || st.st_size < 0 || st.st_size > IDEM_MAX_FILE){
	close(fd);
	return(NULL);
    }

    if((b = (char *) malloc((size_t) st.st_size + 1)) == NULL){
	close(fd);
	return(NULL);
    }

    while(got < st.st_size){
	if((n = read(fd, b + got, (size_t) (st.st_size - got))) < 0){
	    if(errno == EINTR)
	      continue;

	    free(b);
	    close(fd);
	    return(NULL);
	}

	if(n == 0)
	  break;

	got += n;
    }

    close(fd);
    b[got] = '\0';
    return(b);
}


/*----------------------------------------------------------------------
    mcp_idem_begin: see the header comment.  On MCP_IDEM_DONE, *result
    is a malloc'd copy of the stored result (caller frees).
  ----*/
int
mcp_idem_begin(const char *op, const char *key, const char *hash,
	       char **result)
{
    char  path[1200], *text, *rec;
    cJSON *o, *v;
    int   fd, rv;

    *result = NULL;

    if(idem_path(key, path, sizeof(path)) < 0)
      return(MCP_IDEM_ERROR);

    if((fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600)) >= 0){
	if((text = idem_record_text(op, hash, "pending", NULL)) == NULL){
	    close(fd);
	    unlink(path);
	    return(MCP_IDEM_ERROR);
	}

	rv = idem_write(fd, text);
	cJSON_free(text);
	close(fd);
	if(rv < 0){
	    unlink(path);
	    return(MCP_IDEM_ERROR);
	}

	idem_sync_dir();
	return(MCP_IDEM_NEW);
    }

    if(errno != EEXIST)
      return(MCP_IDEM_ERROR);

    if((rec = idem_read(path)) == NULL)
      return(MCP_IDEM_ERROR);

    o = mcp_json_parse(rec, strlen(rec));
    free(rec);
    if(o == NULL)
      return(MCP_IDEM_ERROR);

    rv = MCP_IDEM_PENDING;
    v = cJSON_GetObjectItemCaseSensitive(o, "state");
    if(cJSON_IsString(v) && !strcmp(v->valuestring, "done")){
	cJSON *h = cJSON_GetObjectItemCaseSensitive(o, "hash");
	cJSON *p = cJSON_GetObjectItemCaseSensitive(o, "op");
	cJSON *r = cJSON_GetObjectItemCaseSensitive(o, "result");

	if(cJSON_IsString(h) && !strcmp(h->valuestring, hash)
	   && cJSON_IsString(p) && !strcmp(p->valuestring, op)
	   && cJSON_IsString(r)){
	    *result = strdup(r->valuestring);
	    rv = *result ? MCP_IDEM_DONE : MCP_IDEM_ERROR;
	}
	else
	  rv = MCP_IDEM_MISMATCH;
    }
    else{
	cJSON *h = cJSON_GetObjectItemCaseSensitive(o, "hash");

	/* pending: same arguments or not, nothing is repeated */
	if(!(cJSON_IsString(h) && !strcmp(h->valuestring, hash)))
	  rv = MCP_IDEM_MISMATCH;
    }

    cJSON_Delete(o);
    return(rv);
}


/*
 * Like mcp_idem_begin but writes nothing: MCP_IDEM_NEW means no record
 * exists.  Used before a question is put to the human, so a repeat of a
 * finished send is answered without asking again.
 */
int
mcp_idem_check(const char *op, const char *key, const char *hash,
	       char **result)
{
    char  path[1200], *rec;
    cJSON *o, *v, *h, *p, *r;
    int   rv;

    *result = NULL;
    if(idem_path(key, path, sizeof(path)) < 0)
      return(MCP_IDEM_ERROR);

    if((rec = idem_read(path)) == NULL)
      return(errno == ENOENT ? MCP_IDEM_NEW : MCP_IDEM_ERROR);

    o = mcp_json_parse(rec, strlen(rec));
    free(rec);
    if(o == NULL)
      return(MCP_IDEM_ERROR);

    h = cJSON_GetObjectItemCaseSensitive(o, "hash");
    p = cJSON_GetObjectItemCaseSensitive(o, "op");
    r = cJSON_GetObjectItemCaseSensitive(o, "result");
    v = cJSON_GetObjectItemCaseSensitive(o, "state");
    rv = MCP_IDEM_MISMATCH;
    if(cJSON_IsString(h) && !strcmp(h->valuestring, hash)
       && cJSON_IsString(p) && !strcmp(p->valuestring, op)){
	if(cJSON_IsString(v) && !strcmp(v->valuestring, "done")
	   && cJSON_IsString(r)){
	    *result = strdup(r->valuestring);
	    rv = *result ? MCP_IDEM_DONE : MCP_IDEM_ERROR;
	}
	else
	  rv = MCP_IDEM_PENDING;
    }

    cJSON_Delete(o);
    return(rv);
}


/* pending -> done with the result text, atomically */
int
mcp_idem_commit(const char *op, const char *key, const char *hash,
		const char *result)
{
    char path[1200], tmp[1210], *text;
    int  fd, rv;

    if(idem_path(key, path, sizeof(path)) < 0
       || snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int) sizeof(tmp))
      return(-1);

    if((text = idem_record_text(op, hash, "done", result)) == NULL)
      return(-1);

    if((fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600)) < 0){
	cJSON_free(text);
	return(-1);
    }

    rv = idem_write(fd, text);
    cJSON_free(text);
    close(fd);
    if(rv < 0 || rename(tmp, path) < 0){
	unlink(tmp);
	return(-1);
    }

    idem_sync_dir();
    return(0);
}


/* nothing happened: forget a pending record so the caller may retry */
void
mcp_idem_abort(const char *key)
{
    char path[1200];

    if(idem_path(key, path, sizeof(path)) == 0)
      (void) unlink(path);
}


/*----------------------------------------------------------------------
    Monitor screen support: names of pending records, and removal of one.
    *names is a malloc'd array of malloc'd strings, *n its length.
  ----*/
int
mcp_idem_pending(char ***names, int *n)
{
    char	   dir[1024], path[1200], *rec;
    DIR		  *d;
    struct dirent *e;
    cJSON	  *o, *v;
    int		   cap = 0;

    *names = NULL;
    *n = 0;
    if(idem_dir(dir, sizeof(dir)) < 0 || (d = opendir(dir)) == NULL)
      return(-1);

    while((e = readdir(d)) != NULL){
	if(e->d_name[0] == '.' && (e->d_name[1] == '\0'
	   || (e->d_name[1] == '.' && e->d_name[2] == '\0')))
	  continue;

	if(strlen(e->d_name) > 128
	   || snprintf(path, sizeof(path), "%s/%s", dir, e->d_name)
	      >= (int) sizeof(path))
	  continue;

	if((rec = idem_read(path)) == NULL)
	  continue;

	o = mcp_json_parse(rec, strlen(rec));
	free(rec);
	v = o ? cJSON_GetObjectItemCaseSensitive(o, "state") : NULL;
	if(cJSON_IsString(v) && !strcmp(v->valuestring, "pending")){
	    if(*n == cap){
		char **nn = (char **) realloc(*names,
					      (cap ? cap * 2 : 8) * sizeof(char *));

		if(nn == NULL){
		    cJSON_Delete(o);
		    break;
		}

		*names = nn;
		cap = cap ? cap * 2 : 8;
	    }

	    (*names)[*n] = strdup(e->d_name);
	    if((*names)[*n])
	      (*n)++;
	}

	cJSON_Delete(o);
    }

    closedir(d);
    return(0);
}


/* removes the named record only if it is still pending */
int
mcp_idem_delete_pending(const char *name)
{
    char  path[1200], *rec;
    cJSON *o, *v;
    int   ok = 0;

    if(name == NULL || strchr(name, '/') || !strcmp(name, ".")
       || !strcmp(name, "..") || idem_path(name, path, sizeof(path)) < 0)
      return(-1);

    if((rec = idem_read(path)) == NULL)
      return(-1);

    o = mcp_json_parse(rec, strlen(rec));
    free(rec);
    v = o ? cJSON_GetObjectItemCaseSensitive(o, "state") : NULL;
    if(cJSON_IsString(v) && !strcmp(v->valuestring, "pending"))
      ok = (unlink(path) == 0);

    cJSON_Delete(o);
    return(ok ? 0 : -1);
}
