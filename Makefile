SYS := $(shell $(CXX) -dumpmachine)
SHLIB := libi2pd.so
ARLIB := libi2pd.a
SHLIB_LANG := libi2pdlang.so
ARLIB_LANG := libi2pdlang.a
SHLIB_CLIENT := libi2pdclient.so
ARLIB_CLIENT := libi2pdclient.a
SHLIB_WRAP := libi2pdwrapper.so
ARLIB_WRAP := libi2pdwrapper.a
I2PD := i2pd

LIB_SRC_DIR := libi2pd
LIB_CLIENT_SRC_DIR := libi2pd_client
WRAP_SRC_DIR := libi2pd_wrapper
LANG_SRC_DIR := i18n
DAEMON_SRC_DIR := daemon

# import source files lists
include filelist.mk

USE_AESNI	:= $(or $(USE_AESNI),yes)
USE_STATIC	:= $(or $(USE_STATIC),no)
USE_MESHNET	:= $(or $(USE_MESHNET),no)
USE_UPNP	:= $(or $(USE_UPNP),no)
DEBUG		:= $(or $(DEBUG),yes)

ifeq ($(DEBUG),yes)
	CXX_DEBUG = -g
else
	CXX_DEBUG = -Os
	LD_DEBUG = -s
endif

ifneq (, $(findstring darwin, $(SYS)))
	DAEMON_SRC += $(DAEMON_SRC_DIR)/UnixDaemon.cpp
	ifeq ($(HOMEBREW),1)
		include Makefile.homebrew
	else
		include Makefile.osx
	endif
else ifneq (, $(findstring linux, $(SYS))$(findstring gnu, $(SYS)))
	DAEMON_SRC += $(DAEMON_SRC_DIR)/UnixDaemon.cpp
	include Makefile.linux
else ifneq (, $(findstring freebsd, $(SYS))$(findstring openbsd, $(SYS)))
	DAEMON_SRC += $(DAEMON_SRC_DIR)/UnixDaemon.cpp
	include Makefile.bsd
else ifneq (, $(findstring mingw, $(SYS))$(findstring cygwin, $(SYS)))
	DAEMON_SRC += Win32/DaemonWin32.cpp Win32/Win32App.cpp Win32/Win32NetState.cpp
	include Makefile.mingw
else # not supported
	$(error Not supported platform)
endif

ifeq ($(USE_MESHNET),yes)
	NEEDED_CXXFLAGS += -DMESHNET
endif

NEEDED_CXXFLAGS += -MMD -MP -I$(LIB_SRC_DIR) -I$(LIB_CLIENT_SRC_DIR) -I$(LANG_SRC_DIR)

LIB_OBJS        += $(patsubst %.cpp,obj/%.o,$(LIB_SRC))
LIB_CLIENT_OBJS += $(patsubst %.cpp,obj/%.o,$(LIB_CLIENT_SRC))
WRAP_LIB_OBJS   += $(patsubst %.cpp,obj/%.o,$(WRAP_LIB_SRC))
LANG_OBJS       += $(patsubst %.cpp,obj/%.o,$(LANG_SRC))
DAEMON_OBJS     += $(patsubst %.cpp,obj/%.o,$(DAEMON_SRC))
DEPS            += $(LIB_OBJS:.o=.d) $(LIB_CLIENT_OBJS:.o=.d) $(LANG_OBJS:.o=.d) $(DAEMON_OBJS:.o=.d)

all: mk_obj_dir $(ARLIB) $(ARLIB_CLIENT) $(I2PD)

mk_obj_dir:
	@mkdir -p obj
	@mkdir -p obj/Win32
	@mkdir -p obj/$(LIB_SRC_DIR)
	@mkdir -p obj/$(LIB_CLIENT_SRC_DIR)
	@mkdir -p obj/$(LANG_SRC_DIR)
	@mkdir -p obj/$(WRAP_SRC_DIR)
	@mkdir -p obj/$(DAEMON_SRC_DIR)

api: mk_obj_dir $(SHLIB) $(ARLIB)
client: mk_obj_dir $(SHLIB_CLIENT) $(ARLIB_CLIENT)
api_client: mk_obj_dir $(SHLIB) $(ARLIB) $(SHLIB_CLIENT) $(ARLIB_CLIENT)
wrapper: api_client $(SHLIB_WRAP) $(ARLIB_WRAP)
lang: mk_obj_dir $(SHLIB_LANG) $(ARLIB_LANG)


## NOTE: The NEEDED_CXXFLAGS are here so that CXXFLAGS can be specified at build time
## **without** overwriting the CXXFLAGS which we need in order to build.
## For example, when adding 'hardening flags' to the build
## (e.g. -fstack-protector-strong -Wformat -Werror=format-security), we do not want to remove
## -std=c++11. If you want to remove this variable please do so in a way that allows setting
## custom FLAGS to work at build-time.

obj/%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(NEEDED_CXXFLAGS) $(INCFLAGS) -c -o $@ $<

# '-' is 'ignore if missing' on first run
-include $(DEPS)

$(I2PD): $(DAEMON_OBJS) $(ARLIB) $(ARLIB_CLIENT) $(ARLIB_LANG)
	$(CXX) -o $@ $(LDFLAGS) $^ $(LDLIBS)

$(SHLIB): $(LIB_OBJS)
ifneq ($(USE_STATIC),yes)
	$(CXX) $(LDFLAGS) -shared -o $@ $^ $(LDLIBS)
endif

$(SHLIB_CLIENT): $(LIB_CLIENT_OBJS)
ifneq ($(USE_STATIC),yes)
	$(CXX) $(LDFLAGS) -shared -o $@ $^ $(LDLIBS) $(SHLIB)
endif

$(SHLIB_WRAP): $(WRAP_LIB_OBJS)
ifneq ($(USE_STATIC),yes)
	$(CXX) $(LDFLAGS) -shared -o $@ $^ $(LDLIBS)
endif

$(SHLIB_LANG): $(LANG_OBJS)
ifneq ($(USE_STATIC),yes)
	$(CXX) $(LDFLAGS) -shared -o $@ $^ $(LDLIBS)
endif

$(ARLIB): $(LIB_OBJS)
	$(AR) -r $@ $^

$(ARLIB_CLIENT): $(LIB_CLIENT_OBJS)
	$(AR) -r $@ $^

$(ARLIB_WRAP): $(WRAP_LIB_OBJS)
	$(AR) -r $@ $^

$(ARLIB_LANG): $(LANG_OBJS)
	$(AR) -r $@ $^

clean:
	$(RM) -r obj
	$(RM) -r docs/generated
	$(RM) $(I2PD) $(SHLIB) $(ARLIB) $(SHLIB_CLIENT) $(ARLIB_CLIENT) $(SHLIB_LANG) $(ARLIB_LANG)

strip: $(I2PD) $(SHLIB) $(SHLIB_CLIENT) $(SHLIB_LANG)
	strip $^

LATEST_TAG=$(shell git describe --tags --abbrev=0 openssl)
BRANCH=$(shell git rev-parse --abbrev-ref HEAD)
dist:
	git archive --format=tar.gz -9 --worktree-attributes \
	    --prefix=i2pd_$(LATEST_TAG)/ $(LATEST_TAG) -o i2pd_$(LATEST_TAG).tar.gz

last-dist:
	git archive --format=tar.gz -9 --worktree-attributes \
	    --prefix=i2pd_$(LATEST_TAG)/ $(BRANCH) -o ../i2pd_$(LATEST_TAG).orig.tar.gz

doxygen:
	doxygen -s docs/Doxyfile

.PHONY: all
.PHONY: clean
.PHONY: doxygen
.PHONY: dist
.PHONY: last-dist
.PHONY: api
.PHONY: api_client
.PHONY: client
.PHONY: lang
.PHONY: mk_obj_dir
.PHONY: install
.PHONY: strip
