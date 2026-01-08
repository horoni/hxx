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

struct editor_ctx {
  int fd;
  size_t size;
  unsigned char *data;
  int changed;
  action_t *hist; /* head */
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

enum {
  NORMAL,
  INSERT,
};

void draw_editor(struct editor_ctx *ctx, struct editor_view *v);
void handle_input(struct editor_ctx *ctx, struct editor_view *v, struct tb_event *ev);
void handle_command(struct editor_ctx *ctx, struct editor_view *v);

void open_editor(struct editor_ctx *ctx, const char *filename);
void close_editor(struct editor_ctx *ctx);
void flush_data(struct editor_ctx *ctx);
void extend_file(struct editor_ctx *ctx, size_t siz);

void write_byte(struct editor_ctx *ctx, size_t off, uint8_t new);

void hist_init(struct editor_ctx *ctx);
void hist_free(struct editor_ctx *ctx);
void hist_add(struct editor_ctx *ctx, size_t off, uint8_t old, uint8_t new);
void hist_add_smart(struct editor_ctx *ctx, size_t off, uint8_t old);
void hist_undo(struct editor_ctx *ctx, struct editor_view *v);
void hist_redo(struct editor_ctx *ctx, struct editor_view *v);

int main(int argc, char *argv[])
{
  struct editor_ctx ctx;
  struct editor_view view;
  struct tb_event ev;

  if (argc < 2)
    return 1;

  memset(&ctx, 0, sizeof(ctx));
  memset(&view, 0, sizeof(view));

  hist_init(&ctx);
  open_editor(&ctx, argv[1]);

  tb_init();

  for(;!view.want_quit;) {
    view.h = tb_height();
    view.w = tb_width();
    draw_editor(&ctx, &view);

    tb_poll_event(&ev);
    if (ev.type == TB_EVENT_RESIZE)
      continue;

    handle_input(&ctx, &view, &ev);
    while (!tb_peek_event(&ev, 0)) {
      handle_input(&ctx, &view, &ev);
    }
  }

  tb_shutdown();

  hist_free(&ctx);
  close_editor(&ctx);

  return 0;
}

void draw_editor(struct editor_ctx *ctx, struct editor_view *v)
{
  static const char HEX[] = "0123456789abcdef";
  int h, w;

  h = v->h;
  w = v->w;

  for (int row = 0; row < h - 2; row++) {
    size_t lineoff = v->page + (row * 16);
    int x = 0;

    if (lineoff >= ctx->size) {
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
    if (lineoff + 16 > ctx->size)
      bytes = ctx->size - lineoff;

    int hex_start = x;

    /* --- HEX --- */
    for (size_t i = 0; i < 16; i++) {
      if (i >= bytes)
        break;

      size_t idx = lineoff + i;
      unsigned char b = ctx->data[idx];

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
      unsigned char b = ctx->data[idx];
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
      v->cur, ctx->data[v->cur], ctx->data[v->cur],
      v->mode == INSERT ? "| --INSERT--" : "| NORMAL",
      ctx->changed ? "| [+]" : "");

  tb_present();
}

void handle_input(struct editor_ctx *ctx, struct editor_view *v, struct tb_event *ev)
{
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
      uint8_t cur_byte = ctx->data[v->cur];
      uint8_t new_byte;

      if (v->nibble == 0) {
        v->snap = cur_byte;
        new_byte = (nib << 4) | (cur_byte & 0x0F);
        write_byte(ctx, v->cur, new_byte);
        v->nibble = 1;
      } else {
        new_byte = nib | (cur_byte & 0xF0);
        write_byte(ctx, v->cur, new_byte);
        hist_add_smart(ctx, v->cur, v->snap);
        v->nibble = 0;
        if (v->cur + 1 < ctx->size)
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
        if (cursor + 16 < ctx->size)
          v->cur += 16;
        else
          v->cur = ctx->size - 1;
        break;
      case 'k': /* up */
        if (cursor - 16 >= 0)
          v->cur -= 16;
        else
          v->cur = 0;
        break;
      case 'l': /* right */
        if (cursor + 1 < ctx->size) v->cur += 1;
        break;
      case 'i': /* insert */
        v->mode = INSERT;
        v->nibble = 0;
        break;
      case ':': /* command */
        handle_command(ctx, v);
        break;
      case 'u': /* undo */
        hist_undo(ctx, v);
        break;
    }
    switch (ev->key) {
      case TB_KEY_CTRL_R: /* redo */
        hist_redo(ctx, v);
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

void handle_command(struct editor_ctx *ctx, struct editor_view *v)
{
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
    if (!ctx->changed)
      v->want_quit = 1;
  } else if (buf[0] == 'q' && buf[1] == '!' && buf[2] == '\0') {
    v->want_quit = 1;
  } else if (buf[0] == 'w' && buf[1] == '\0') {
    flush_data(ctx);
  } else if (buf[0] == 'w' && buf[1] == 'q' && buf[2] == '\0') {
    flush_data(ctx);
    if (!ctx->changed)
      v->want_quit = 1;
  } else {
    size_t off = 0;
    if (buf[pos - 1] == 'x') {
      buf[pos - 1] = '\0';
      off = strtoull(buf, NULL, 16);
    } else {
      off = strtoull(buf, NULL, 10);
    }
    if (off >= ctx->size)
      off = ctx->size - 1;

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

void open_editor(struct editor_ctx *ctx, const char *filename)
{
  struct stat st;

  ctx->fd = open(filename, O_RDWR);
  if (ctx->fd < 0) {
    perror("open() failed");
    exit(1);
  }

  if (fstat(ctx->fd, &st) < 0) {
    perror("fstat() failed");
    exit(1);
  }

  ctx->size = st.st_size;
  ctx->changed = 0;

  if (ctx->size == 0) {
    fprintf(stderr, "cant map empty file\n");
    exit(1);
  }

  ctx->data = mmap(NULL, ctx->size, PROT_READ | PROT_WRITE, MAP_SHARED, ctx->fd, 0);
  if (ctx->data == MAP_FAILED) {
    perror("mmap() failed");
    exit(1);
  }

  if (madvise(ctx->data, ctx->size, MADV_SEQUENTIAL) < 0) {
    perror("madvise failed");
  }
}

void close_editor(struct editor_ctx *ctx)
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

void flush_data(struct editor_ctx *ctx)
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

void write_byte(struct editor_ctx *ctx, size_t off, uint8_t new)
{
  if (off >= ctx->size)
    return;

  ctx->data[off] = new;
  ctx->changed = 1;
}

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
  uint8_t new = ctx->data[off];
  if (new != old)
    hist_add(ctx, off, old, new);
}

void hist_undo(struct editor_ctx *ctx, struct editor_view *v)
{
  if (!ctx->hist->prev)
    return;

  action_t *act = ctx->hist;

  ctx->data[act->off] = act->old_val;
  ctx->hist = act->prev;
  ctx->changed = 1;

  v->cur = act->off;
}

void hist_redo(struct editor_ctx *ctx, struct editor_view *v)
{
  if (!ctx->hist->next)
    return;

  action_t *act = ctx->hist->next;
  ctx->data[act->off] = act->new_val;
  ctx->hist = act;
  ctx->changed = 1;

  v->cur = act->off;
}
