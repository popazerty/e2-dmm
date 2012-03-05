installdir = $(pkglibdir)/python/Plugins/Extensions

SUBDIRS = CutListEditor PicturePlayer MediaScanner MediaPlayer GraphMultiEPG SocketMMI 
 
if ENABLE_TF7700
SUBDIRS += TopfieldVFD
endif

if ENABLE_CUBEREVO
SUBDIRS += CuberevoVFD
endif

install_PYTHON =	\
	__init__.py
