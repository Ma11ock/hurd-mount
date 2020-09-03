#   Copyright (C) 1994, 95, 96, 98, 1999, 2006, 2012, 2016-2019
#   Free Software Foundation, Inc.
#
#   This program is free software; you can redistribute it and/or
#   modify it under the terms of the GNU General Public License as
#   published by the Free Software Foundation; either version 2, or (at
#   your option) any later version.
#
#   This program is distributed in the hope that it will be useful, but
#   WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
#   General Public License for more details.
#
#   You should have received a copy of the GNU General Public License
#   along with the GNU Hurd.  If not, see <http://www.gnu.org/licenses/>.

dir := libsysmount
makemode := library

libname = libsysmount
SRCS = mount.c

installhdrs = mount.h

HURDLIBS = shouldbeinlibc fshelp ports

OBJS = $(subst .c,.o,$(SRCS))
OBJS += ../sutils/fstab.o
OBJS += ../sutils/clookup.o
OBJS += ../utils/match-options.o

mount.o: ../sutils/fstab.o ../sutils/clookup.o ../utils/match-options.o \
       $(foreach L,shouldbeinlibc fshelp ports,../lib$L/lib$L.a)
../sutils/fstab.o ../sutils/clookup.o: FORCE
    $(MAKE) -C $(@D) $(@F)
FORCE:


include ../Makeconf
