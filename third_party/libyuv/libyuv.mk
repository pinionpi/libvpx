LIBYUV_SRCS-yes += include/libyuv/basic_types.h  \
                	 include/libyuv/convert.h \
                	 include/libyuv/convert_argb.h \
                	 include/libyuv/convert_from.h \
                	 include/libyuv/cpu_id.h  \
                	 include/libyuv/planar_functions.h  \
                	 include/libyuv/rotate.h  \
                	 include/libyuv/row.h  \
                	 include/libyuv/scale.h  \
                	 include/libyuv/scale_row.h  \
                	 source/cpu_id.cc \
									 source/convert.cc \
									 source/convert_from.cc \
                	 source/planar_functions.cc \
                	 source/row_any.cc \
                	 source/row_common.cc \
                	 source/row_gcc.cc \
                	 source/row_msa.cc \
                	 source/row_neon.cc \
                	 source/row_neon64.cc \
                	 source/row_win.cc \
                	 source/scale.cc \
                	 source/scale_any.cc \
                	 source/scale_common.cc \
                	 source/scale_gcc.cc \
                	 source/scale_msa.cc \
                	 source/scale_neon.cc \
                	 source/scale_neon64.cc \
                	 source/scale_win.cc \

INTERNAL_CFLAGS += -I$(SRC_PATH_BARE)/third_party/libyuv/include
