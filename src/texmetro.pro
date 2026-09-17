# vcglib, draco and meshoptimizer are git submodules under src/thirdparty:
# they're large enough, and we use enough of each, that hand-picking files to
# vendor would mean nobody remembers which files were kept or why the next
# time they need updating. cgltf is the exception: it's a single header
# that's meant to be dropped into a project as-is, so it's vendored directly.
THIRDPARTY = $$PWD/thirdparty
VCGPATH = $$THIRDPARTY/vcglib

# vcglib's Histogram::Percentile() has an assert comparing two floating point
# sums accumulated in a different order, which can spuriously fail once a
# histogram has accumulated enough samples (see patches/README). Applied at
# qmake time (rather than as a build step) so it lands before anything
# #includes the header, regardless of build parallelism.
system(cd $$VCGPATH && git apply --check $$PWD/../patches/vcglib-histogram-tolerance.patch 2>/dev/null && git apply $$PWD/../patches/vcglib-histogram-tolerance.patch)

CONFIG += console
CONFIG += c++14
CONFIG -= app_bundle

TARGET = texmetro



#### QT STUFF ##################################################################

TEMPLATE = app
QT = core gui opengl widgets



##### INCLUDE PATH #############################################################

INCLUDEPATH += $$VCGPATH $$VCGPATH/eigenlib
INCLUDEPATH += $$THIRDPARTY/cgltf $$THIRDPARTY/meshoptimizer/src



#### LIBS ######################################################################

unix {
  CONFIG += link_pkgconfig
  PKGCONFIG += glew
  LIBS += -lGL -lGLEW
}

win32 {
  WIN_GLEW_PATH = $$PWD/glew       # set to glew dir

  INCLUDEPATH += $$WIN_GLEW_PATH/include
  LIBS += -L$$WIN_GLEW_PATH/lib/Release/x64 -lglew32

  LIBS += -lopengl32
}



#### DRACO (KHR_draco_mesh_compression decoding) ###############################
# draco (git submodule, see above) has no simple single-header form and no
# stable subset of files we could vendor without re-deriving that subset on
# every update, so instead its own CMake build produces a static library,
# which is then linked in statically.

DRACO_ROOT = $$THIRDPARTY/draco
DRACO_BUILD = $$DRACO_ROOT/build
DRACO_LIB = $$DRACO_BUILD/libdraco.a

INCLUDEPATH += $$DRACO_ROOT/src $$DRACO_BUILD

draco_lib.target = $$DRACO_LIB
draco_lib.commands = \
    cmake -S $$DRACO_ROOT -B $$DRACO_BUILD -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=OFF \
      -DDRACO_TESTS=OFF \
      -DDRACO_TRANSCODER_SUPPORTED=OFF \
      -DDRACO_ANIMATION_ENCODING=OFF \
      -DDRACO_UNITY_PLUGIN=OFF \
      -DDRACO_MAYA_PLUGIN=OFF \
      -DDRACO_WASM=OFF \
      -DDRACO_INSTALL=OFF \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    $$escape_expand(\\n\\t)cmake --build $$DRACO_BUILD --target draco_static
QMAKE_EXTRA_TARGETS += draco_lib
PRE_TARGETDEPS += $$DRACO_LIB
LIBS += $$DRACO_LIB



#### PLATFORM SPECIFIC FLAGS ###################################################

win32 {
  DEFINES += NOMINMAX
}



#### SOURCE FILES ##############################################################

HEADERS += \
    mesh.h \
    measure.h \
    gl_utils.h \
    color_consistency.h \
    gltf_import.h

SOURCES += \
    $$VCGPATH/wrap/system/qgetopt.cpp

SOURCES += \
    $$THIRDPARTY/meshoptimizer/src/vertexcodec.cpp \
    $$THIRDPARTY/meshoptimizer/src/indexcodec.cpp \
    $$THIRDPARTY/meshoptimizer/src/vertexfilter.cpp

SOURCES += \
    texmetro.cpp \
    mesh.cpp \
    gltf_import.cpp \
    measure.cpp \
    gl_utils.cpp \
    color_consistency.cpp
