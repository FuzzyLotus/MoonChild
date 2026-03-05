# Flux Apparition — Chorus/Reverb with Cross-Injection for Terrarium
# Lives inside DaisyCloudSeed/petal/FluxApparition/
#
# Setup:
#   cd DaisyCloudSeed
#   git submodule update --init --recursive
#   make -C libdaisy
#   make -C DaisySP
#   cd petal/FluxApparition
#   make clean
#   make
#   make program-dfu

TARGET = FluxApparition
CPP_SOURCES = flux_apparition.cpp

# Hardware Execution Optimizations
OPT = -O3 -flto
C_DEFS += -DNDEBUG

# Terrarium header from the Terrarium submodule
C_INCLUDES += -I.

# DaisySP (all subdirectories)
DAISYSP_DIR = ../../DaisySP
C_INCLUDES += -I$(DAISYSP_DIR)/Source
C_INCLUDES += -I$(DAISYSP_DIR)/Source/Control
C_INCLUDES += -I$(DAISYSP_DIR)/Source/Drums
C_INCLUDES += -I$(DAISYSP_DIR)/Source/Dynamics
C_INCLUDES += -I$(DAISYSP_DIR)/Source/Effects
C_INCLUDES += -I$(DAISYSP_DIR)/Source/Filters
C_INCLUDES += -I$(DAISYSP_DIR)/Source/Noise
C_INCLUDES += -I$(DAISYSP_DIR)/Source/PhysicalModeling
C_INCLUDES += -I$(DAISYSP_DIR)/Source/Synthesis
C_INCLUDES += -I$(DAISYSP_DIR)/Source/Utility
LIBS += -ldaisysp
LDFLAGS += -L$(DAISYSP_DIR)/build

# libDaisy
LIBDAISY_DIR = ../../libdaisy
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
