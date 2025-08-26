# Makefile to build class 'timestretch_beatmode~' for Pure Data.
# Needs Makefile.pdlibbuilder as helper makefile for platform-dependent build
# settings and rules.

# library name
lib.name = timestretch_beatmode~

# input source file (class name == source file basename)
class.sources = timestretch_beatmode~.c

# all extra files to be included in binary distribution of the library
datafiles = timestretch_beatmode~-help.pd timestretch_beatmode~-meta.pd README.md

# link math library for cosf and other math functions
ldlibs = -lm

# include Makefile.pdlibbuilder from submodule directory 'pd-lib-builder'
PDLIBBUILDER_DIR=pd-lib-builder/
include $(PDLIBBUILDER_DIR)/Makefile.pdlibbuilder
