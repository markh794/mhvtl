# Build the mhVTL SELinux policy module (mhvtl.pp) with the reference-policy
# development tooling (selinux-policy-devel on Fedora/EL, selinux-policy-devel
# on openSUSE).
#
#   make            # -> mhvtl.pp
#   sudo semodule -i mhvtl.pp
#   sudo restorecon -R -v /usr/libexec/mhvtl /var/lib/mhvtl /run/mhvtl /run/lock/mhvtl
#
# The module ships as a PERMISSIVE domain (see mhvtl.te), so loading it logs
# AVCs without denying anything.

DEVEL_MAKEFILE := /usr/share/selinux/devel/Makefile

all:
	$(MAKE) -f $(DEVEL_MAKEFILE) mhvtl.pp

clean:
	$(MAKE) -f $(DEVEL_MAKEFILE) clean

.PHONY: all clean
