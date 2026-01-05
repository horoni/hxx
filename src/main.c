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
#include <ncurses.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define CP_OFFSET 1
#define CP_HEX    2
#define CP_ASCII  3
#define CP_NULL   4

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
  int max_y, max_x;
  int want_quit;
  size_t cur; /* cursor */
  size_t page;
  int mode;
  int nibble;
  uint8_t snap;
  char c; /* pressed char */
};

enum {
  NORMAL,
  INSERT,
};

void draw_editor(struct editor_ctx *ctx, struct editor_view *v);
void handle_input(struct editor_ctx *ctx, struct editor_view *v);
void handle_jump(struct editor_ctx *ctx, struct editor_view *v);

void open_editor(struct editor_ctx *ctx, const char *filename);
void close_editor(struct editor_ctx *ctx);
void flush_data(struct editor_ctx *ctx);
void extend_file(struct editor_ctx *ctx, size_t siz);

void write_byte(struct editor_ctx *ctx, size_t off, uint8_t new);

void hist_init(struct editor_ctx *ctx);
void hist_add(struct editor_ctx *ctx, size_t off, uint8_t old, uint8_t new);
void hist_add_smart(struct editor_ctx *ctx, size_t off, uint8_t old);
void hist_undo(struct editor_ctx *ctx, struct editor_view *v);
void hist_redo(struct editor_ctx *ctx, struct editor_view *v);

int main(int argc, char *argv[])
{
  struct editor_ctx ctx;
  struct editor_view view;

  if (argc < 2)
    return 1;

  memset(&ctx, 0, sizeof(ctx));
  memset(&view, 0, sizeof(view));

  hist_init(&ctx);
  open_editor(&ctx, argv[1]);

  initscr();
  noecho();
  curs_set(0);

  if (has_colors()) {
    start_color();
    /*                   Foreground, Background */
    init_pair(CP_OFFSET, COLOR_CYAN, COLOR_BLACK);
    init_pair(CP_HEX,    COLOR_WHITE, COLOR_BLACK);
    init_pair(CP_ASCII,  COLOR_GREEN, COLOR_BLACK);
    init_pair(CP_NULL,   COLOR_BLUE, COLOR_BLACK);
  }

  for(;!view.want_quit;) {
    erase();
    draw_editor(&ctx, &view);
    refresh();

    view.c = getch();
    getmaxyx(stdscr, view.max_y, view.max_x);
    handle_input(&ctx, &view);
  }

  endwin();
  close_editor(&ctx);

  return 0;
}

void draw_editor(struct editor_ctx *ctx, struct editor_view *v)
{
  int max_y, max_x;

  getmaxyx(stdscr, max_y, max_x);

  for (int row = 0; row < max_y - 2; row++) {
    size_t lineoff = v->page + (row * 16);
    if (lineoff >= ctx->size) {
      mvprintw(row, 0, "~~");
      break;
    }

    mvprintw(row, 0, "%08zx: ", lineoff);

    for (size_t i = 0; i < 16; i++) {
      size_t idx = lineoff + i;

      if (idx >= ctx->size)
        break;

      unsigned char b = ctx->data[idx];
      int is_cursor = (idx == v->cur);
      int is_print = isprint(b);

      int x_pos = 10 + (i * 2) + i;

      char hex_num[3];
      snprintf(hex_num, sizeof(hex_num), "%02x", b);

      int attr_base = COLOR_PAIR(CP_HEX);
      if (b == 0x00)
        attr_base = COLOR_PAIR(CP_NULL) | A_DIM;
      if (is_print)
        attr_base = COLOR_PAIR(CP_ASCII);

      int attr_high = attr_base;
      if (is_cursor) {
        if (v->mode == NORMAL || (v->mode == INSERT && v->nibble == 0)) {
          attr_high |= A_REVERSE;
        }
      }

      attron(attr_high);
      mvaddch(row, x_pos, hex_num[0]);
      attroff(attr_high);

      int attr_low = attr_base;
      if (is_cursor) {
        if (v->mode == NORMAL || (v->mode == INSERT && v->nibble == 1)) {
          attr_low |= A_REVERSE;
        }
      }

      attron(attr_low);
      mvaddch(row, x_pos + 1, hex_num[1]);
      attroff(attr_low);

      int ascii_x = 62 + i;
      int ascii_attr = COLOR_PAIR(CP_ASCII);

      if (is_cursor)
        ascii_attr |= A_REVERSE;
      
      attron(ascii_attr);
      if (is_print) {
        mvaddch(row, ascii_x, b);
      } else {
        if (!is_cursor)
          attroff(COLOR_PAIR(CP_ASCII));
        mvaddch(row, ascii_x, '.');
        if (!is_cursor)
          attron(COLOR_PAIR(CP_ASCII));
      }
      attroff(ascii_attr);
    }
    mvprintw(row, 62 - 2, "|");
  }

  attron(A_REVERSE);
  for (int i = 0; i < max_x; i++)
    mvaddch(max_y - 2, i, ' ');
  attroff(A_REVERSE);

  mvprintw(max_y - 1, 0, "POS: %08zx | VAL: %02x | DEC: %3d ", v->cur, ctx->data[v->cur], ctx->data[v->cur]);

  if (ctx->changed) {
    attron(COLOR_PAIR(CP_ASCII) | A_BOLD);
    mvprintw(max_y - 1, max_x - 10, "[+]");
    attroff(COLOR_PAIR(CP_ASCII) | A_BOLD);
  }

  if (v->mode == INSERT)
    mvprintw(max_y - 1, 35, "| --INSERT--");
}

void handle_input(struct editor_ctx *ctx, struct editor_view *v)
{
  if (v->mode == INSERT) {
    if (v->c == 27) {
      if (v->nibble == 1) {
        hist_add_smart(ctx, v->cur, v->snap);
        v->nibble = 0;
      }
      v->mode = NORMAL;
      return;
    }

    if (isxdigit(v->c)) {
      char hex_str[2] = {v->c, '\0'};
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
    switch (v->c) {
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
      case ':': /* seek */
        handle_jump(ctx, v);
        break;
      case 'w':
        flush_data(ctx);
        break;
      case 'u': /* undo */
        hist_undo(ctx, v);
        break;
      case 18: /* CTRL+R : redo */
        hist_redo(ctx, v);
        break;
      case 'q': /* quit */
        /* print message to user "no write since last change" */
        if (!ctx->changed)
          v->want_quit = 1;
        break;
    }

    if (v->cur >= v->page + ((v->max_y - 2) * 16)) {
      size_t cur_row_start = (v->cur / 16) * 16;
      v->page = cur_row_start - ((v->max_y - 3) * 16);
    }

    if (v->cur < v->page) {
      v->page = (v->cur / 16) * 16;
    }
  }
}

void handle_jump(struct editor_ctx *ctx, struct editor_view *v)
{
  char buf[64] = {0};
  int pos = 0;
  int max_y;
  int c;

  max_y = getmaxy(stdscr);

  attron(A_BOLD);
  mvprintw(max_y - 1, 0, ":");
  clrtoeol();
  attroff(A_BOLD);

  curs_set(1);
  refresh();

  for (;;) {
    c = getch();

    if (c == '\n') {
      break;
    } else if (c == 27) {
      curs_set(0);
      return;
    } else if (c == KEY_BACKSPACE || c == 127 || c == '\b') {
      if (pos > 0) {
        buf[pos - 1] = '\0';
        mvaddch(max_y - 1, pos, ' ');
        move(max_y - 1, pos);
        pos--;
      }
    } else if (pos < 63 && (isalnum(c) || c == 'x')) {
      mvaddch(max_y - 1, 1 + pos, c);
      buf[pos++] = c;
    }
    refresh();
  }

  curs_set(0);
  if (pos == 0)
    return;

  size_t off = 0;
  if (buf[0] == 'x')
    off = strtoull(buf + 1, NULL, 16);
  else
    off = strtoull(buf, NULL, 10);

  if (off >= ctx->size)
    off = ctx->size - 1;

  v->cur = off;
  v->page = (v->cur / 16) * 16;
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

  if (madvise(ctx->data, ctx->size, MADV_RANDOM) < 0) {
    perror("madvise failed");
  }
}

void close_editor(struct editor_ctx *ctx)
{
  if (!ctx)
    return;

  action_t *cur = ctx->hist;
  while (cur->prev) {
    cur = cur->prev;
  }
  while (cur) {
    action_t *next = cur->next;
    free(cur);
    cur = next;
  }

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
