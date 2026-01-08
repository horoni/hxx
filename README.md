## hxx
lightweight hex editor with vim-like controls
### build
```
make install
```
or for MMAP backend (Linux only)
```
make install USE_MMAP=1
```
### how to use
 - h - left
 - j - down
 - k - up
 - l - right
 - i - insert
 - u - undo
 - ESC - return to normal mode
 - ctrl+r - redo
 - : - jump to offset ( if the last char is `x` then offset is hex)
 - :w - flush cache to disk
 - :wq - flush cache to disk and quit
 - :q - quit
 - :q! - quit without writing (currently doesnt work, need to undo history before exit or dont call msync)
