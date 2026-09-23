# 工具链默认指本机 Qt 自带的 MinGW。
# 坑：CC 是 make 内建变量（默认 cc），`CC ?=` 覆盖不了它 —— 必须判 origin，
#     否则会去调系统 cc 而报 "CreateProcess ... cc" 失败。
# CI 里用 make CC=gcc WINDRES=windres 覆盖（命令行优先级最高）。
ifeq ($(origin CC),default)
CC = C:/Qt/Tools/mingw1310_64/bin/gcc.exe
endif
ifeq ($(origin WINDRES),undefined)
WINDRES = C:/Qt/Tools/mingw1310_64/bin/windres.exe
endif
PYTHON  ?= python
CFLAGS  = -std=c11 -O2 -Wall -Wextra -DUNICODE -D_UNICODE -Isrc
LDFLAGS = -mwindows -static -lcomctl32 -lws2_32 -luser32 -lgdi32 -lkernel32 -ladvapi32

ENGINE = build/speedlimit.o build/sched.o build/taskstore.o build/queue.o build/category.o build/torrent.o

OBJS = build/util.o build/http.o build/download.o $(ENGINE) build/main_window.o build/main.o build/selftest_run.o build/resources.res

all: idm.exe idm_selftest.exe idm_nmhost.exe

idm.exe: $(OBJS)
	$(CC) -o idm.exe $(OBJS) $(LDFLAGS)

# 浏览器原生消息宿主（对标 IDM 的 IDMMsgHost.exe）：控制台子系统，走 stdio 帧
idm_nmhost.exe: build/jsonlite.o build/nmhost.o
	$(CC) -o idm_nmhost.exe build/jsonlite.o build/nmhost.o -mconsole -static -luser32 -lkernel32 -ladvapi32

build/jsonlite.o: src/common/jsonlite.c
	mkdir -p build && $(CC) $(CFLAGS) -c $< -o $@

build/nmhost.o: src/nmhost.c
	mkdir -p build && $(CC) $(CFLAGS) -c $< -o $@

build/speedlimit.o: src/engine/speedlimit.c
	mkdir -p build && $(CC) $(CFLAGS) -c $< -o $@

build/sched.o: src/engine/sched.c
	mkdir -p build && $(CC) $(CFLAGS) -c $< -o $@

build/taskstore.o: src/engine/taskstore.c
	mkdir -p build && $(CC) $(CFLAGS) -c $< -o $@

build/queue.o: src/engine/queue.c
	mkdir -p build && $(CC) $(CFLAGS) -c $< -o $@

build/category.o: src/engine/category.c
	mkdir -p build && $(CC) $(CFLAGS) -c $< -o $@

build/torrent.o: src/engine/torrent.c
	mkdir -p build && $(CC) $(CFLAGS) -c $< -o $@

idm_selftest.exe: build/util.o build/http.o build/download.o $(ENGINE) build/selftest.o build/selftest_run.o
	$(CC) -o idm_selftest.exe build/util.o build/http.o build/download.o $(ENGINE) build/selftest.o build/selftest_run.o -mconsole -static -lws2_32 -luser32 -lgdi32 -lkernel32 -ladvapi32

build/selftest.o: src/selftest.c
	mkdir -p build && $(CC) $(CFLAGS) -c $< -o $@

build/selftest_run.o: src/selftest_run.c
	mkdir -p build && $(CC) $(CFLAGS) -c $< -o $@

build/util.o: src/common/util.c
	mkdir -p build && $(CC) $(CFLAGS) -c $< -o $@

build/http.o: src/engine/http.c
	mkdir -p build && $(CC) $(CFLAGS) -c $< -o $@

build/download.o: src/engine/download.c
	mkdir -p build && $(CC) $(CFLAGS) -c $< -o $@

build/main_window.o: src/gui/main_window.c
	mkdir -p build && $(CC) $(CFLAGS) -c $< -o $@

build/main.o: src/main.c
	mkdir -p build && $(CC) $(CFLAGS) -c $< -o $@

build/resources.res: src/gui/resources.rc src/gui/app.ico src/gui/app.manifest src/common/version.h
	mkdir -p build && $(WINDRES) --output-format=coff $< -o $@

# 发行打包：产物 idm.exe/idm_nmhost.exe 已全静态，直接 zip 即可「下载即用」。
# 产出 dist/$(IDM_APP_NAME)-<版本>-win64.zip + SHA256SUMS.txt
dist: all
	$(PYTHON) packaging/make_release.py

package: dist

clean:
	rm -rf build dist idm.exe idm_selftest.exe idm_nmhost.exe

.PHONY: all clean dist package
