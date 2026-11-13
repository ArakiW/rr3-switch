#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO. Install the devkitPro switch-dev group first.")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET      := rr3-native-rehost
BUILD       := build
SOURCES     := source source/gfx source/jni_compat source/jni_compat/classes source/eap_port
INCLUDES    := include
DATA        :=
ROMFS       := romfs
ROMFS_IMAGE := rr3.romfs
ROMFS_FILES := $(shell find $(ROMFS) -type f)
APP_TITLE   := RR3 Native Rehost
APP_AUTHOR  := Native Port Lab
APP_VERSION := 0.22.10

# THE NSP PACKAGE.
#
# WHY THE APP SHIPS AS AN NSP AND NOT AS AN NRO.  An NSP installs as a
# TITLE -- through hbloader or an NSP installer -- and launches from the
# Home menu; an NRO has to be launched by a homebrew menu every time.
#
# THE NSP IS NOT BUILT HERE.  libnx's own `%.nsp` rule builds the PFS0
# from the DIRECTORY `exefs`, so the container's entries come out named
# `main` and `main.npdm` -- that is an EXEFS image, not an NSP, and an NSP
# needs its entries named `exefs/main` and `exefs/main.npdm`.  So
# tools/Pack-NativeSwitchNsp.ps1 does the work, and Build-NativeSwitch.ps1
# calls it after this Makefile finishes.  The script reads the container
# back afterwards and refuses to report success unless it holds exactly
# three NCAs, one of them the content meta.
#
# THE ROMFS IS INSIDE THE NSP.  It was not, once, and that was the bug:
# the first NSP was 5,411,032 bytes against the NRO's 53,756,401, because
# the packer was called with --noromfs.  The 48 MB of difference was the
# whole tree -- the guest's own libraries (libRealRacing3.so is 33 MB of
# it), the catalogue faces and the original text -- and the port opens
# "romfs:/libc++_shared.so" with no card fallback, so the package could
# not even start the guest.  The packer now gets --romfsdir and the tree
# lands in the Program NCA's second section.
#
# BOTH PACKAGES CARRY THE SAME TREE AND REACH IT DIFFERENTLY.  The NRO
# embeds it and romfsInit() mounts it.  The NSP's lives in the Program
# NCA's second section, and libnx's romfsMountSelf() answers an NSO with
# romfsMountFromCurrentProcess -- the process's data storage.  So the
# port ALSO mounts the card's copy (rr3.romfs) when romfsInit() fails;
# see mountPortRomfs() in source/main.cpp.  See
# tools/README-hardware-test.md, section 10.12.

# Runtime-selectable graphics backend (native-switch/source/gfx).
#
# Every guest graphics import gets a -Wl,--wrap shim, so the rehosted guest and
# the host probe both reach either devkitPro's Mesa or the null backend through
# one runtime switch (RR3_GFX=null, see rr3/gfx_backend.hpp). With the switch
# unset the shim forwards to __real_* and the Mesa path is unchanged.
#
# The list is derived from the bind table in source/symbol_resolver.cpp -- the
# 145 guest imports -- plus the 26 remaining EGL entry points the host calls
# directly. Regenerate with .build/glbackend/gen_gl_wrap.py after adding a
# graphics row to the table; it also regenerates source/gfx/*.inc.
GFX_WRAP_SYMBOLS := eglGetDisplay eglGetProcAddress eglQueryString glActiveTexture glAttachShader glBindAttribLocation glBindBuffer glBindFramebuffer glBindRenderbuffer glBindTexture glBlendColor glBlendEquation glBlendEquationSeparate glBlendFunc glBlendFuncSeparate glBufferData glBufferSubData glCheckFramebufferStatus glClear glClearColor glClearDepthf glClearStencil glColorMask glCompileShader glCompressedTexImage2D glCompressedTexSubImage2D glCopyTexImage2D glCopyTexSubImage2D glCreateProgram glCreateShader glCullFace glDeleteBuffers glDeleteFramebuffers glDeleteProgram glDeleteRenderbuffers glDeleteShader glDeleteTextures glDepthFunc glDepthMask glDepthRangef glDetachShader glDisable glDisableVertexAttribArray glDrawArrays glDrawElements glEnable glEnableVertexAttribArray glFinish glFlush glFramebufferRenderbuffer glFramebufferTexture2D glFrontFace glGenBuffers glGenerateMipmap glGenFramebuffers glGenRenderbuffers glGenTextures glGetActiveAttrib glGetActiveUniform glGetAttachedShaders glGetAttribLocation glGetBooleanv glGetBufferParameteriv glGetError glGetFloatv glGetFramebufferAttachmentParameteriv glGetIntegerv glGetProgramInfoLog glGetProgramiv glGetRenderbufferParameteriv glGetShaderInfoLog glGetShaderiv glGetShaderPrecisionFormat glGetShaderSource glGetString glGetTexParameterfv glGetTexParameteriv glGetUniformfv glGetUniformiv glGetUniformLocation glGetVertexAttribfv glGetVertexAttribiv glGetVertexAttribPointerv glHint glIsBuffer glIsEnabled glIsFramebuffer glIsProgram glIsRenderbuffer glIsShader glIsTexture glLineWidth glLinkProgram glPixelStorei glPolygonOffset glReadPixels glReleaseShaderCompiler glRenderbufferStorage glSampleCoverage glScissor glShaderBinary glShaderSource glStencilFunc glStencilFuncSeparate glStencilMask glStencilMaskSeparate glStencilOp glStencilOpSeparate glTexImage2D glTexParameterf glTexParameterfv glTexParameteri glTexParameteriv glTexSubImage2D glUniform1f glUniform1fv glUniform1i glUniform1iv glUniform2f glUniform2fv glUniform2i glUniform2iv glUniform3f glUniform3fv glUniform3i glUniform3iv glUniform4f glUniform4fv glUniform4i glUniform4iv glUniformMatrix2fv glUniformMatrix3fv glUniformMatrix4fv glUseProgram glValidateProgram glVertexAttrib1f glVertexAttrib1fv glVertexAttrib2f glVertexAttrib2fv glVertexAttrib3f glVertexAttrib3fv glVertexAttrib4f glVertexAttrib4fv glVertexAttribPointer glViewport eglBindAPI eglChooseConfig eglCreateContext eglCreatePbufferSurface eglCreateWindowSurface eglDestroyContext eglDestroySurface eglGetConfigAttrib eglGetConfigs eglGetCurrentContext eglGetCurrentDisplay eglGetCurrentSurface eglGetError eglInitialize eglMakeCurrent eglQueryAPI eglQueryContext eglQuerySurface eglReleaseThread eglSurfaceAttrib eglSwapBuffers eglSwapInterval eglTerminate eglWaitClient eglWaitGL eglWaitNative
GFX_WRAP_FLAGS   := $(foreach sym,$(GFX_WRAP_SYMBOLS),-Wl,--wrap=$(sym))

ARCH        := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS      := -g -Wall -Wextra -O2 -ffunction-sections $(ARCH) $(DEFINES)
CFLAGS      += $(INCLUDE) -D__SWITCH__
CXXFLAGS    := $(CFLAGS) -Wno-missing-field-initializers -fno-rtti -fno-exceptions -std=gnu++20
ASFLAGS     := -g $(ARCH)
LDFLAGS     := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map) $(GFX_WRAP_FLAGS)
LIBS        := -lEGL -lGLESv2 -lglapi -ldrm_nouveau \
               -Wl,--start-group -lfreetype -lharfbuzz -Wl,--end-group \
               -lpng -lz -lbz2 -lnx -lm

LIBDIRS     := $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)
export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export LD := $(CXX)
export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES := $(OFILES_SRC)
export HFILES_BIN :=
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(PORTLIBS)/include/freetype2 \
                  -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifneq ($(ROMFS),)
	export NROFLAGS := $(NROFLAGS) --romfs=../$(ROMFS_IMAGE)
endif

export NROFLAGS := $(NROFLAGS) --icon=$(APP_ICON) --nacp=$(OUTPUT).nacp

.PHONY: all clean $(BUILD)

all: $(BUILD)

$(ROMFS_IMAGE): $(ROMFS_FILES)
	@echo building romfs image ...
	@build_romfs $(ROMFS) $(ROMFS_IMAGE)

$(BUILD): $(ROMFS_IMAGE)
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf $(TARGET).nsp

else

DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp ../$(ROMFS_IMAGE)
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
