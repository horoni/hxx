/*
 * Copyright (c) 2026, horoni. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#define _GNU_SOURCE
#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "termbox2.h"

#define CL_HEX    TB_WHITE
#define CL_ASCII  TB_GREEN
#define CL_NULL   TB_BLUE

typedef struct action {
  size_t off;
  uint8_t old_val;
  uint8_t new_val;
  struct action *prev;
  struct action *next;
} __attribute__((packed)) action_t;

struct data_ctx {
  int fd;
  size_t size;
  int changed;
  char *path;
#ifdef USE_MMAP
  unsigned char *data;
#else
  /* buffer size 4 KiB */
  #define BUF_SIZ (1024 * 4)
  FILE *fp;
  uint8_t buf[BUF_SIZ];
  size_t buf_off;
  size_t buf_len;
  uint8_t buf_changed;
#endif
};

struct editor_view {
  int h, w;
  int want_quit;
  size_t cur; /* cursor */
  size_t page;
  int mode;
  int nibble;
  uint8_t snap;
};

struct editor_ctx {
  struct data_ctx *data;
  struct editor_view *v;
  action_t *hist; /* head */
};

enum {
  NORMAL,
  INSERT,
};

void draw_editor(struct editor_ctx *ctx);
void handle_input(struct editor_ctx *ctx, struct tb_event *ev);
void handle_command(struct editor_ctx *ctx);

int data_open(struct data_ctx *ctx, const char *path);
void data_close(struct data_ctx *ctx);
void data_flush(struct data_ctx *ctx);
uint8_t data_read(struct data_ctx *ctx, size_t off);
void data_write(struct data_ctx *ctx, size_t off, uint8_t new);

void hist_init(struct editor_ctx *ctx);
void hist_free(struct editor_ctx *ctx);
void hist_add(struct editor_ctx *ctx, size_t off, uint8_t old, uint8_t new);
void hist_add_smart(struct editor_ctx *ctx, size_t off, uint8_t old);
void hist_undo(struct editor_ctx *ctx);
void hist_redo(struct editor_ctx *ctx);

int main(int argc, char *argv[])
{
  struct editor_ctx ctx;
  struct editor_view view;
  struct data_ctx data;
  struct tb_event ev;

  if (argc < 2)
    return 1;

  memset(&ctx, 0, sizeof(ctx));
  memset(&view, 0, sizeof(view));
  memset(&data, 0, sizeof(data));

  ctx.v = &view;
  ctx.data = &data;

  hist_init(&ctx);
  if (data_open(&data, argv[1]))
    return 1;

  tb_init();

  for(;!view.want_quit;) {
    view.h = tb_height();
    view.w = tb_width();
    draw_editor(&ctx);

    tb_poll_event(&ev);
    if (ev.type == TB_EVENT_RESIZE)
      continue;

    handle_input(&ctx, &ev);
    while (!tb_peek_event(&ev, 0)) {
      handle_input(&ctx, &ev);
    }
  }

  tb_shutdown();

  hist_free(&ctx);
  data_close(&data);

  return 0;
}

void draw_editor(struct editor_ctx *ctx)
{
  static const char HEX[] = "0123456789abcdef";
  struct editor_view *v = ctx->v;
  int h, w;

  h = v->h;
  w = v->w;

  for (int row = 0; row < h - 2; row++) {
    size_t lineoff = v->page + (row * 16);
    int x = 0;

    if (lineoff >= ctx->data->size) {
      tb_set_cell(0, row, '~', TB_BLUE, TB_BLACK);
      break;
    }

    /* -- offset -- */
    size_t tmpoff = lineoff;
    for (int i = 8, x = 8; i > 0; i--, tmpoff >>= 4)
      tb_set_cell(x--, row, HEX[tmpoff & 0x0F], TB_WHITE, TB_BLACK);

    x += 9;
    tb_print(x, row, TB_WHITE, TB_BLACK, ": ");
    x += 2;

    size_t bytes = 16;
    if (lineoff + 16 > ctx->data->size)
      bytes = ctx->data->size - lineoff;

    int hex_start = x;

    /* --- HEX --- */
    for (size_t i = 0; i < 16; i++) {
      if (i >= bytes)
        break;

      size_t idx = lineoff + i;
      uint8_t b = data_read(ctx->data, idx);

      int is_cursor = (idx == v->cur);
      int is_print = isprint(b);

      /* -- base attr -- */
      uintattr_t fg = CL_HEX;
      uintattr_t bg = TB_BLACK;
      if (b == 0x00)
        fg = CL_NULL;
      if (is_print)
        fg = CL_ASCII;

      /* -- half bytes attr -- */
      uintattr_t fg_h1 = fg, bg_h1 = bg;
      uintattr_t fg_h2 = fg, bg_h2 = bg;

      if (is_cursor) {
        if (v->mode == NORMAL) {
          bg_h1 = bg_h2 = TB_REVERSE;
        } else {
          if (v->nibble == 0)
            bg_h1 = TB_REVERSE;
          else
            bg_h2 = TB_REVERSE;
        }
      }

      /* -- half bytes draw! -- */
      tb_set_cell(x++, row, HEX[(b >> 4) & 0x0F], fg_h1, bg_h1);
      tb_set_cell(x++, row, HEX[b & 0x0F], fg_h2, bg_h2);
      tb_set_cell(x++, row, ' ', TB_DEFAULT, TB_DEFAULT);
    }

    /* -- if end-of-file -- */
    while (x < hex_start + 48) {
      tb_set_cell(x++, row, ' ', TB_DEFAULT, TB_DEFAULT);
    }

    /* -- separator -- */
    tb_set_cell(x++, row, '|', TB_DEFAULT, TB_DEFAULT);
    tb_set_cell(x++, row, ' ', TB_DEFAULT, TB_DEFAULT);

    /* --- ASCII --- */
    for (size_t i = 0; i < bytes; i++) {
      size_t idx = lineoff + i;
      uint8_t b = data_read(ctx->data, idx);
      int is_cursor = (idx == v->cur);

      uintattr_t fg = CL_ASCII;
      uintattr_t bg = TB_BLACK;
      if (!isprint(b))
        fg = CL_NULL;
      if (is_cursor)
        bg = TB_REVERSE;

      uint32_t c = isprint(b) ? b : '.';
      tb_set_cell(x++, row, c, fg, bg);
    }
  }

  for (int i = 0; i < w; i++)
    tb_set_cell(i, h - 2, ' ', TB_DEFAULT, TB_REVERSE);
  for (int i = 0; i < w; i++)
    tb_set_cell(i, h - 1, ' ', TB_DEFAULT, TB_DEFAULT);
  tb_printf(0, h - 1, TB_BOLD, TB_DEFAULT, "POS: %08zx | HEX: %02x | DEC: %3d %s %s",
      v->cur, data_read(ctx->data, v->cur), data_read(ctx->data, v->cur),
      v->mode == INSERT ? "| --INSERT--" : "| NORMAL",
      ctx->data->changed ? "| [+]" : "");

  tb_present();
}

void handle_input(struct editor_ctx *ctx, struct tb_event *ev)
{
  struct editor_view *v = ctx->v;
  if (v->mode == INSERT) {
    if (ev->key == TB_KEY_ESC) {
      if (v->nibble == 1) {
        hist_add_smart(ctx, v->cur, v->snap);
        v->nibble = 0;
      }
      v->mode = NORMAL;
      return;
    }

    if (isxdigit(ev->ch)) {
      char hex_str[2] = {ev->ch, '\0'};
      uint8_t nib = (uint8_t)strtol(hex_str, NULL, 16);
      uint8_t cur_byte = data_read(ctx->data, v->cur);
      uint8_t new_byte;

      if (v->nibble == 0) {
        v->snap = cur_byte;
        new_byte = (nib << 4) | (cur_byte & 0x0F);
        data_write(ctx->data, v->cur, new_byte);
        v->nibble = 1;
      } else {
        new_byte = nib | (cur_byte & 0xF0);
        data_write(ctx->data, v->cur, new_byte);
        hist_add_smart(ctx, v->cur, v->snap);
        v->nibble = 0;
        if (v->cur + 1 < ctx->data->size)
          v->cur++;
      }
    }
  } else {
    long long cursor = v->cur;
    switch (ev->ch) {
      case 'h': /* left */
        if (cursor - 1 >= 0) v->cur -= 1;
        break;
      case 'j': /* down */ 
        if (cursor + 16 < ctx->data->size)
          v->cur += 16;
        else
          v->cur = ctx->data->size - 1;
        break;
      case 'k': /* up */
        if (cursor - 16 >= 0)
          v->cur -= 16;
        else
          v->cur = 0;
        break;
      case 'l': /* right */
        if (cursor + 1 < ctx->data->size)
          v->cur += 1;
        break;
      case 'i': /* insert */
        v->mode = INSERT;
        v->nibble = 0;
        break;
      case ':': /* command */
        handle_command(ctx);
        break;
      case 'u': /* undo */
        hist_undo(ctx);
        break;
    }
    switch (ev->key) {
      case TB_KEY_CTRL_R: /* redo */
        hist_redo(ctx);
        break;
    }
  }

  if (v->cur >= v->page + ((v->h - 2) * 16)) {
    size_t cur_row_start = (v->cur / 16) * 16;
    v->page = cur_row_start - ((v->h - 3) * 16);
  }

  if (v->cur < v->page) {
    v->page = (v->cur / 16) * 16;
  }
}

void handle_command(struct editor_ctx *ctx)
{
  struct editor_view *v = ctx->v;
  struct tb_event ev;
  char buf[64] = {0};
  int pos = 0;
  int h;

  h = tb_height();
  for (int x = 1; x < tb_width(); x++)
    tb_set_cell(x, h - 1, ' ', TB_DEFAULT, TB_DEFAULT);
  tb_set_cell(0, h - 1, ':', TB_DEFAULT, TB_DEFAULT);
  tb_set_cursor(1, h - 1);
  tb_present();

  for (;;) {
    tb_poll_event(&ev);

    if (ev.type == TB_EVENT_KEY) {
      if (ev.key == TB_KEY_ESC)
        goto clline;
      if (ev.key == TB_KEY_ENTER)
        break;
      if (ev.key == TB_KEY_BACKSPACE || ev.key == TB_KEY_BACKSPACE2)
        if (pos > 0)
          buf[--pos] = '\0';
      if (ev.ch && pos < 63)
        buf[pos++] = ev.ch;
    }
    for (int x = 1; x < tb_width(); x++)
      tb_set_cell(x, h - 1, ' ', TB_DEFAULT, TB_DEFAULT);
    tb_printf(0, h - 1, TB_DEFAULT, TB_DEFAULT, ":%s", buf);
    tb_set_cursor(1 + pos, h - 1);
    tb_present();
  }

  if (pos == 0)
    return;

  if (buf[0] == 'q' && buf[1] == '\0') {
    /* print message to user "no write since last change" */
    if (!ctx->data->changed)
      v->want_quit = 1;
  } else if (buf[0] == 'q' && buf[1] == '!' && buf[2] == '\0') {
    v->want_quit = 1;
  } else if (buf[0] == 'w' && buf[1] == '\0') {
    data_flush(ctx->data);
  } else if (buf[0] == 'w' && buf[1] == 'q' && buf[2] == '\0') {
    data_flush(ctx->data);
    if (!ctx->data->changed)
      v->want_quit = 1;
  } else {
    size_t off = 0;
    if (buf[pos - 1] == 'x') {
      buf[pos - 1] = '\0';
      off = strtoull(buf, NULL, 16);
    } else {
      off = strtoull(buf, NULL, 10);
    }
    if (off >= ctx->data->size)
      off = ctx->data->size - 1;

    v->cur = off;
    v->page = (v->cur / 16) * 16;

    /* if cursor move to EOF we need to clear garbage on the screen */
    tb_clear();
  }
clline:
  tb_hide_cursor();
  for (int x = 0; x < tb_width(); x++)
    tb_set_cell(x, h - 1, ' ', TB_DEFAULT, TB_DEFAULT);
}

/*
void extend_file(struct editor_ctx *ctx, size_t siz)
{
  void *new_data;

  if (siz <= ctx->size)
    return;

  if (ftruncate(ctx->fd, siz) < 0) {
    perror("ftruncate() failed");
    return;
  }

  new_data = mremap(ctx->data, ctx->size, siz, MREMAP_MAYMOVE);
  if (new_data == MAP_FAILED) {
    perror("mremap() failed");
    // TODO: old map is still live, maybe reuse
    exit(1);
  }

  ctx->data = new_data;
  ctx->size = siz;
}
*/

void hist_init(struct editor_ctx *ctx)
{
  ctx->hist = malloc(sizeof(action_t));
  bzero(ctx->hist, sizeof(action_t));
  assert(ctx->hist->next == NULL);
  assert(ctx->hist->prev == NULL);
}

void hist_free(struct editor_ctx *ctx)
{
  action_t *cur = ctx->hist;
  while (cur->prev) {
    cur = cur->prev;
  }
  while (cur) {
    action_t *next = cur->next;
    free(cur);
    cur = next;
  }
}

void hist_add(struct editor_ctx *ctx, size_t off, uint8_t old, uint8_t new)
{
  action_t *cur = ctx->hist->next;
  while (cur) {
    action_t *next = cur->next;
    free(cur);
    cur = next;
  }

  action_t *act = malloc(sizeof(action_t));
  act->off = off;
  act->old_val = old;
  act->new_val = new;
  act->next = NULL;
  act->prev = ctx->hist;

  ctx->hist->next = act;
  ctx->hist = act;
}

void hist_add_smart(struct editor_ctx *ctx, size_t off, uint8_t old)
{
  uint8_t new = data_read(ctx->data, off);
  if (new != old)
    hist_add(ctx, off, old, new);
}

void hist_undo(struct editor_ctx *ctx)
{
  if (!ctx->hist->prev)
    return;

  action_t *act = ctx->hist;

  data_write(ctx->data, act->off, act->old_val);
  ctx->hist = act->prev;

  ctx->v->cur = act->off;
}

void hist_redo(struct editor_ctx *ctx)
{
  if (!ctx->hist->next)
    return;

  action_t *act = ctx->hist->next;
  data_write(ctx->data, act->off, act->new_val);
  ctx->hist = act;

  ctx->v->cur = act->off;
}

#ifdef USE_MMAP

int data_open(struct data_ctx *ctx, const char *path)
{
  struct stat st;

  ctx->fd = open(path, O_RDWR);
  if (ctx->fd < 0) {
    perror("open() failed");
    return 1;
  }

  if (fstat(ctx->fd, &st) < 0) {
    perror("fstat() failed");
    return 1;
  }

  ctx->size = st.st_size;
  ctx->changed = 0;

  if (ctx->size == 0) {
    fprintf(stderr, "cant map empty file\n");
    return 1;
  }

  ctx->data = mmap(NULL, ctx->size, PROT_READ | PROT_WRITE, MAP_SHARED, ctx->fd, 0);
  if (ctx->data == MAP_FAILED) {
    perror("mmap() failed");
    return 1;
  }

  if (madvise(ctx->data, ctx->size, MADV_SEQUENTIAL) < 0) {
    perror("madvise failed");
  }

  return 0;
}

void data_close(struct data_ctx *ctx)
{
  if (!ctx)
    return;

  if (ctx->data != NULL && ctx->data != MAP_FAILED) {
    if (msync(ctx->data, ctx->size, MS_SYNC) < 0) {
      perror("msync() failed (data lost?)");
      // TODO: Retry
    }
    if (munmap(ctx->data, ctx->size) < 0) {
      perror("munmap() failed");
    }
  }

  if (ctx->fd != -1) {
    if (close(ctx->fd) < 0) {
      perror("close() failed");
    }
  }

  ctx->fd = -1;
  ctx->size = 0;
  ctx->data = NULL;
  ctx->changed = 0;
}

void data_flush(struct data_ctx *ctx)
{
  if (!ctx->changed)
    return;

  if (msync(ctx->data, ctx->size, MS_SYNC) < 0) {
    perror("msync() failed");
    // TODO: Print to status-bar
  } else {
    ctx->changed = 0;
  }
}

uint8_t data_read(struct data_ctx *ctx, size_t off)
{
  if (off >= ctx->size)
    return 0;
  return ctx->data[off];
}


void data_write(struct data_ctx *ctx, size_t off, uint8_t new)
{
  if (off >= ctx->size)
    return;

  ctx->data[off] = new;
  ctx->changed = 1;
}

#else

void _flush_buf(struct data_ctx *ctx)
{
  if (!ctx->buf_changed || !ctx->fp)
    return;

  fseek(ctx->fp, ctx->buf_off, SEEK_SET);
  fwrite(ctx->buf, sizeof(uint8_t), ctx->buf_len, ctx->fp);
  fflush(ctx->fp);
  ctx->buf_changed = 0;
}

void _load_chunk(struct data_ctx *ctx, uint64_t off)
{
  if (!ctx->fp)
    return;

  _flush_buf(ctx);

  ctx->buf_off = (off / BUF_SIZ) * BUF_SIZ;
  fseek(ctx->fp, ctx->buf_off, SEEK_SET);
  ctx->buf_len = fread(ctx->buf, sizeof(uint8_t), BUF_SIZ, ctx->fp);
}

int data_open(struct data_ctx *ctx, const char *path)
{
  ctx->fp = fopen(path, "rb+");
  if (!ctx->fp)
    return 1;

  if (fseek(ctx->fp, 0, SEEK_END) < 0)
    goto err;

  long size;
  if ((size = ftell(ctx->fp)) < 0)
    goto err;

  ctx->size = size;
  ctx->changed = 0;
  ctx->buf_off = -1;
  ctx->buf_len = 0;
  ctx->buf_changed = 0;

  /* has same lifetime as program. yes? */
  ctx->path = (char *)path;

  return 0;
err:
  fclose(ctx->fp);
  return 1;
}

void data_close(struct data_ctx *ctx)
{
  if (!ctx->fp)
    return;

  fclose(ctx->fp);
}

void data_flush(struct data_ctx *ctx)
{
  _flush_buf(ctx);
  ctx->changed = 0;
}

uint8_t data_read(struct data_ctx *ctx, size_t off)
{
  if (off >= ctx->size)
    return 0;

  if (off < ctx->buf_off || off >= ctx->buf_off + ctx->buf_len)
    _load_chunk(ctx, off);

  return ctx->buf[off - ctx->buf_off];
}

void data_write(struct data_ctx *ctx, size_t off, uint8_t new)
{
  if (off >= ctx->size)
    return;

  if (off < ctx->buf_off || off >= ctx->buf_off + ctx->buf_len)
    _load_chunk(ctx, off);

  ctx->buf[off - ctx->buf_off] = new;
  ctx->buf_changed = 1;
  ctx->changed = 1;
}

#endif
