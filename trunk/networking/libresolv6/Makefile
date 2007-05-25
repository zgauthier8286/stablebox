ifndef top_srcdir
top_srcdir=../..
endif
ifndef top_builddir
top_builddir=../..
endif
srcdir=$(top_srcdir)/networking/libresolv6
LIBRESOLV6_DIR:=./
include $(top_srcdir)/Rules.mak
include $(top_builddir)/.config
include Makefile.in
all: $(libraries-y)
-include $(top_builddir)/.depend

clean:
	rm -f *.o *.a $(AR_TARGET)

