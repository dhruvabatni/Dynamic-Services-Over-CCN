# csrc/subr.mk
# 
# Part of the CCNx distribution.
#
# Copyright (C) 2009 Palo Alto Research Center, Inc.
#
# This work is free software; you can redistribute it and/or modify it under
# the terms of the GNU General Public License version 2 as published by the
# Free Software Foundation.
# This work is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE.
#

REAL_CFLAGS = $(COPT) $(CWARNFLAGS) $(CPREFLAGS) $(PLATCFLAGS)

$(CSRC) $(HSRC) $(SCRIPTSRC) $(SRCLINKS):
	test -f $(SRCDIR)/$@ && ln -s $(SRCDIR)/$@

config_subdir: Makefile

Makefile:
	test -f $(SRCDIR)/../generic.mk && ln -s $(SRCDIR)/../generic.mk Makefile

$(DUPDIR):
	test -d $(SRCDIR)/$(DUPDIR) && mkdir $(DUPDIR) && cp -p $(SRCDIR)/$(DUPDIR)/* $(DUPDIR)

$(OBJDIR)/dir.mk: dir.mk
	test -d $(OBJDIR) || mkdir $(OBJDIR)
	test -f $(OBJDIR)/dir.mk && mv $(OBJDIR)/dir.mk $(OBJDIR)/dir.mk~ ||:
	cp -p dir.mk $(OBJDIR)/dir.mk

install_libs: $(LIBS)
	test -d $(INSTALL_LIB)
	for i in $(LIBS) ""; do test -z "$$i" || $(INSTALL) $$i $(INSTALL_LIB); done

install_programs: $(INSTALLED_PROGRAMS)
	test -d $(INSTALL_BIN)
	for i in $(INSTALLED_PROGRAMS) ""; do test -z "$$i" || $(INSTALL) $$i $(INSTALL_BIN); done

install: install_libs install_programs

uninstall_libs:
	for i in $(LIBS) ""; do test -z "$$i" || $(RM) $(INSTALL_LIB)/$$i; done

uninstall_programs:
	for i in $(PROGRAMS) ""; do test -z "$$i" || $(RM) $(INSTALL_BIN)/$$i; done

uninstall: uninstall_libs uninstall_programs

coverage:
	X () { test -f $$1 || return 0; gcov $$*; }; X *.gc??

shared:

depend: dir.mk $(CSRC)
	for i in $(CSRC); do gcc -MM $(CPREFLAGS) $$i; done > depend
	tail -n `wc -l < depend` dir.mk | diff - depend

install_libs install_programs install uninstall_libs uninstall_programs uninstall coverage shared documentation depend config_subdir: _always
.PHONY: _always
_always:
